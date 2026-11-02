// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "qcom-dcvs: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/rculist.h>
#include <linux/of_fdt.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <soc/qcom/dcvs.h>
#include <soc/qcom/of_common.h>
#include "dcvs_private.h"
#include "trace-dcvs.h"

static const char * const dcvs_hw_names[NUM_DCVS_HW_TYPES] = {
	[DCVS_DDR]		= "DDR",
	[DCVS_LLCC]		= "LLCC",
	[DCVS_L3]		= "L3",
	[DCVS_DDRQOS]		= "DDRQOS",
	[DCVS_UBWCP]		= "UBWCP",
};

enum dcvs_type {
	QCOM_DCVS_DEV,
	QCOM_DCVS_HW,
	QCOM_DCVS_PATH,
	NUM_DCVS_TYPES
};

struct qcom_dcvs_spec {
	enum dcvs_type		type;
};

struct dcvs_voter {
	struct dcvs_freq	freq;
	struct list_head	node;
	struct rcu_head	rcu;
	char			name[];
};

struct qcom_dcvs_data {
	struct kobject		kobj;
	struct dcvs_hw		*hw_devs[NUM_DCVS_HW_TYPES];
	u32			num_hw;
	u32			num_inited_hw;
	bool			inited;
	bool			root_ready;

};

static struct qcom_dcvs_data *dcvs_data;
static DEFINE_MUTEX(dcvs_init_lock);

static bool dcvs_ready(void)
{
	struct qcom_dcvs_data *data;

	guard(rcu)();
	data = READ_ONCE(dcvs_data);
	/* Paired with publication after all hardware paths are initialized. */
	return data && smp_load_acquire(&data->inited);
}

static u32 get_target_freq(struct dcvs_path *path, u32 freq);

struct qcom_dcvs_attr {
	struct attribute	attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
			const char *buf, size_t count);
};

#define to_qcom_dcvs_attr(_attr) \
	container_of(_attr, struct qcom_dcvs_attr, attr)
#define to_dcvs_hw(k)	container_of(k, struct dcvs_hw, kobj)

#define DCVS_ATTR_RW(_name)						\
static struct qcom_dcvs_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)			\

#define DCVS_ATTR_RO(_name)						\
static struct qcom_dcvs_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)					\


#define show_attr(name)							\
static ssize_t show_##name(struct kobject *kobj,			\
			struct attribute *attr, char *buf)		\
{									\
	struct dcvs_hw *hw = to_dcvs_hw(kobj);				\
	return scnprintf(buf, PAGE_SIZE, "%u\n", hw->name);		\
}									\

#define store_attr(name, _min, _max) \
static ssize_t store_##name(struct kobject *kobj,			\
			struct attribute *attr, const char *buf,	\
			size_t count)					\
{									\
	int ret;							\
	unsigned int val;						\
	struct dcvs_hw *hw = to_dcvs_hw(kobj);			\
	ret = kstrtouint(buf, 10, &val);				\
	if (ret < 0)							\
		return ret;						\
	val = max(val, _min);						\
	val = min(val, _max);						\
	hw->name = val;						\
	return count;							\
}									\

static ssize_t store_boost_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;
	struct dcvs_hw *hw = to_dcvs_hw(kobj);
	struct dcvs_path *path;
	struct dcvs_voter *voter;
	struct dcvs_freq new_freq;

	guard(mutex)(&dcvs_init_lock);

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;
	if (val > hw->hw_max_freq)
		return -EINVAL;
	/* boost_freq only supported on hw with slow path */
	path = hw->dcvs_paths[DCVS_SLOW_PATH];
	if (!path)
		return -EPERM;

	val = max(val, hw->hw_min_freq);
	hw->boost_freq = val;

	/* must re-aggregate votes to get new freq after boost update */
	mutex_lock(&path->voter_lock);
	new_freq.ib = new_freq.ab = 0;
	new_freq.hw_type = hw->type;
	list_for_each_entry(voter, &path->voter_list, node) {
		new_freq.ib = max(voter->freq.ib, new_freq.ib);
		new_freq.ab += voter->freq.ab;
	}
	new_freq.ib = get_target_freq(path, new_freq.ib);
	if (new_freq.ib != path->cur_freq.ib) {
		ret = path->commit_dcvs_freqs(path, &new_freq, 1);
		if (ret < 0)
			pr_err("Error setting boost freq: %d\n", ret);
	}
	mutex_unlock(&path->voter_lock);

	trace_qcom_dcvs_boost(hw->type, path->type, hw->boost_freq,
				new_freq.ib, new_freq.ab);

	return count;
}

static ssize_t show_cur_freq(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	int i, cpu;
	struct dcvs_hw *hw = to_dcvs_hw(kobj);
	struct dcvs_path *path;
	u32 cur_freq = 0;

	guard(mutex)(&dcvs_init_lock);

	for (i = 0; i < NUM_DCVS_PATHS; i++) {
		path = hw->dcvs_paths[i];
		if (!path)
			continue;
		if (path->type != DCVS_PERCPU_PATH) {
			cur_freq = max(cur_freq, path->cur_freq.ib);
			continue;
		}
		for_each_possible_cpu(cpu)
			cur_freq = max(cur_freq, path->percpu_cur_freqs[cpu]);
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", cur_freq);
}

static ssize_t show_available_frequencies(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct dcvs_hw *hw = to_dcvs_hw(kobj);
	int i, cnt = 0;

	for (i = 0; i < hw->table_len; i++)
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%d ",
				hw->freq_table[i]);

	if (cnt)
		cnt--;

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	return cnt;
}

show_attr(hw_min_freq);
show_attr(hw_max_freq);
show_attr(boost_freq);

DCVS_ATTR_RO(hw_min_freq);
DCVS_ATTR_RO(hw_max_freq);
DCVS_ATTR_RW(boost_freq);
DCVS_ATTR_RO(cur_freq);
DCVS_ATTR_RO(available_frequencies);

static struct attribute *dcvs_hw_attrs[] = {
	&hw_min_freq.attr,
	&hw_max_freq.attr,
	&boost_freq.attr,
	&cur_freq.attr,
	&available_frequencies.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dcvs_hw);

static ssize_t attr_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	struct qcom_dcvs_attr *dcvs_attr = to_qcom_dcvs_attr(attr);
	ssize_t ret = -EIO;

	if (dcvs_attr->show)
		ret = dcvs_attr->show(kobj, attr, buf);

	return ret;
}

static ssize_t attr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	struct qcom_dcvs_attr *dcvs_attr = to_qcom_dcvs_attr(attr);
	ssize_t ret = -EIO;

	if (dcvs_attr->store)
		ret = dcvs_attr->store(kobj, attr, buf, count);

	return ret;
}

static const struct sysfs_ops qcom_dcvs_sysfs_ops = {
	.show	= attr_show,
	.store	= attr_store,
};

static void qcom_dcvs_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct qcom_dcvs_data, kobj));
}

static void dcvs_hw_release(struct kobject *kobj)
{
	kfree(to_dcvs_hw(kobj));
}

static const struct kobj_type qcom_dcvs_ktype = {
	.release = qcom_dcvs_release,
	.sysfs_ops	= &qcom_dcvs_sysfs_ops,
};

static const struct kobj_type dcvs_hw_ktype = {
	.release = dcvs_hw_release,
	.sysfs_ops	= &qcom_dcvs_sysfs_ops,
	.default_groups	= dcvs_hw_groups,
};

static inline struct dcvs_path *get_dcvs_path(enum dcvs_hw_type hw,
						enum dcvs_path_type path)
{
	if ((unsigned int)hw >= NUM_DCVS_HW_TYPES ||
	    (unsigned int)path >= NUM_DCVS_PATHS)
		return NULL;

	if (dcvs_data && dcvs_data->hw_devs[hw] &&
			dcvs_data->hw_devs[hw]->dcvs_paths[path])
		return dcvs_data->hw_devs[hw]->dcvs_paths[path];

	return NULL;
}

static u32 get_target_freq(struct dcvs_path *path, u32 freq)
{
	struct dcvs_hw *hw = path->hw;
	u32 *freq_table = hw->freq_table;
	u32 len = hw->table_len;
	u32 target_freq = 0;
	int i;

	if (path->type == DCVS_SLOW_PATH)
		freq = max(freq, hw->boost_freq);

	for (i = 0; i < len; i++) {
		if (freq <= freq_table[i]) {
			target_freq = freq_table[i];
			break;
		}
	}

	if (i == len)
		target_freq = freq_table[len-1];

	return target_freq;
}

/* slow path update: does aggregation across multiple clients */
static int qcom_dcvs_sp_update(const char *name, struct dcvs_freq *votes,
						u32 update_mask)
{
	int i, ret = 0;
	bool found = false;
	struct dcvs_voter *voter;
	struct dcvs_path *path;
	struct dcvs_freq new_freq;
	enum dcvs_hw_type hw_type;

	if (!name || !votes || !update_mask ||
	    (update_mask & ~GENMASK(NUM_DCVS_HW_TYPES - 1, 0)))
		return -EINVAL;

	for (i = 0; i < NUM_DCVS_HW_TYPES; i++) {
		if (!(update_mask & BIT(i)))
			continue;
		hw_type = votes[i].hw_type;
		path = get_dcvs_path(hw_type, DCVS_SLOW_PATH);
		if (!path)
			return -EINVAL;

		mutex_lock(&path->voter_lock);
		new_freq.ib = new_freq.ab = 0;
		new_freq.hw_type = hw_type;
		found = false;
		list_for_each_entry(voter, &path->voter_list, node) {
			if (!strcmp(voter->name, name)) {
				found = true;
				voter->freq.ib = votes[i].ib;
				voter->freq.ab = votes[i].ab;
			}
			new_freq.ib = max(voter->freq.ib, new_freq.ib);
			new_freq.ab += voter->freq.ab;
		}
		if (!found) {
			mutex_unlock(&path->voter_lock);
			return -EINVAL;
		}
		new_freq.ib = get_target_freq(path, new_freq.ib);
		if (new_freq.ib != path->cur_freq.ib ||
					new_freq.ab != path->cur_freq.ab)
			ret = path->commit_dcvs_freqs(path, &new_freq, 1);
		mutex_unlock(&path->voter_lock);
		if (ret < 0)
			return ret;
		trace_qcom_dcvs_update(name, hw_type, path->type, votes[i].ib,
					new_freq.ib, votes[i].ab, new_freq.ab,
					path->hw->boost_freq);
	}

	return ret;
}

/* fast path update: only single client allowed so lockless */
static int qcom_dcvs_fp_update(const char *name, struct dcvs_freq *votes,
						u32 update_mask)
{
	int i, ret = 0;
	u32 commit_mask = 0;
	struct dcvs_voter *voter;
	struct dcvs_path *path;
	struct dcvs_freq new_freqs[NUM_DCVS_HW_TYPES];
	enum dcvs_hw_type hw_type;

	guard(rcu)();

	if (!name || !votes || !update_mask ||
	    (update_mask & ~GENMASK(NUM_DCVS_HW_TYPES - 1, 0)))
		return -EINVAL;

	for (i = 0; i < NUM_DCVS_HW_TYPES; i++) {
		if (!(update_mask & BIT(i)))
			continue;
		hw_type = votes[i].hw_type;
		path = get_dcvs_path(hw_type, DCVS_FAST_PATH);
		/* fast path requires votes be passed in correct order */
		if (!path || i != hw_type)
			return -EINVAL;

		/* should match one and only voter in list */
		voter = list_first_or_null_rcu(&path->voter_list, struct dcvs_voter, node);
		if (!voter || strcmp(voter->name, name))
			return -EINVAL;

		/* no aggregation required since only single client allowed */
		new_freqs[i].ib = get_target_freq(path, votes[i].ib);
		if (new_freqs[i].ib != path->cur_freq.ib)
			commit_mask |= BIT(i);
		trace_qcom_dcvs_update(name, hw_type, path->type, votes[i].ib,
					new_freqs[i].ib, 0, 0, 0);
	}

	if (commit_mask)
		ret = path->commit_dcvs_freqs(path, new_freqs, commit_mask);

	return ret;
}

/*
 * percpu path update: only single client per cpu allowed so lockless.
 * Also note that client is responsible for disabling preemption
 */
static int qcom_dcvs_percpu_update(const char *name, struct dcvs_freq *votes,
						u32 update_mask)
{
	int i, ret = 0;
	u32 cpu = smp_processor_id();
	struct dcvs_voter *voter;
	struct dcvs_path *path;
	struct dcvs_freq new_freq;
	enum dcvs_hw_type hw_type;

	guard(rcu)();

	if (!name || !votes || !update_mask ||
	    (update_mask & ~GENMASK(NUM_DCVS_HW_TYPES - 1, 0)))
		return -EINVAL;

	for (i = 0; i < NUM_DCVS_HW_TYPES; i++) {
		if (!(update_mask & BIT(i)))
			continue;
		hw_type = votes[i].hw_type;
		path = get_dcvs_path(hw_type, DCVS_PERCPU_PATH);
		if (!path)
			return -EINVAL;

		/* should match one and only voter in list */
		voter = list_first_or_null_rcu(&path->voter_list, struct dcvs_voter, node);
		if (!voter || strcmp(voter->name, name))
			return -EINVAL;

		/* no aggregation required since only single client per cpu */
		new_freq.ib = get_target_freq(path, votes[i].ib);
		new_freq.hw_type = hw_type;
		if (new_freq.ib != path->percpu_cur_freqs[cpu]) {
			ret = path->commit_dcvs_freqs(path, &new_freq, 1);
			if (ret < 0)
				return ret;
		}
		trace_qcom_dcvs_update(name, hw_type, path->type, votes[i].ib,
					new_freq.ib, 0, 0, 0);
	}

	return ret;
}

int qcom_dcvs_update_votes(const char *name, struct dcvs_freq *votes,
				u32 update_mask, enum dcvs_path_type path)
{
	if (!dcvs_ready())
		return -EPROBE_DEFER;

	switch (path) {
	case DCVS_SLOW_PATH:
		return qcom_dcvs_sp_update(name, votes, update_mask);
	case DCVS_FAST_PATH:
		return qcom_dcvs_fp_update(name, votes, update_mask);
	case DCVS_PERCPU_PATH:
		return qcom_dcvs_percpu_update(name, votes, update_mask);
	default:
		break;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(qcom_dcvs_update_votes);

int qcom_dcvs_register_voter(const char *name, enum dcvs_hw_type hw_type,
				enum dcvs_path_type path_type)
{
	int ret = 0;
	struct dcvs_voter *voter;
	struct dcvs_path *path;

	if (!name || (unsigned int)hw_type >= NUM_DCVS_HW_TYPES ||
	    (unsigned int)path_type >= NUM_DCVS_PATHS)
		return -EINVAL;

	if (!dcvs_ready())
		return -EPROBE_DEFER;

	path = get_dcvs_path(hw_type, path_type);
	if (!path)
		return -ENODEV;

	mutex_lock(&path->voter_lock);
	if (path_type == DCVS_FAST_PATH && path->num_voters >= 1) {
		ret = -EINVAL;
		goto unlock_out;
	}
	list_for_each_entry(voter, &path->voter_list, node)
		if (!strcmp(voter->name, name)) {
			ret = -EINVAL;
			goto unlock_out;
		}

	voter = kzalloc(struct_size(voter, name, strlen(name) + 1), GFP_KERNEL);
	if (!voter) {
		ret = -ENOMEM;
		goto unlock_out;
	}
	strscpy(voter->name, name, strlen(name) + 1);
	list_add_tail_rcu(&voter->node, &path->voter_list);
	path->num_voters++;

unlock_out:
	mutex_unlock(&path->voter_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_dcvs_register_voter);

int qcom_dcvs_unregister_voter(const char *name, enum dcvs_hw_type hw_type,
				enum dcvs_path_type path_type)
{
	int ret = 0;
	struct dcvs_voter *voter;
	struct dcvs_path *path;
	bool found = false;

	if (!name || (unsigned int)hw_type >= NUM_DCVS_HW_TYPES ||
	    (unsigned int)path_type >= NUM_DCVS_PATHS)
		return -EINVAL;

	if (!dcvs_ready())
		return -EPROBE_DEFER;

	path = get_dcvs_path(hw_type, path_type);
	if (!path)
		return -ENODEV;

	mutex_lock(&path->voter_lock);
	list_for_each_entry(voter, &path->voter_list, node)
		if (!strcmp(voter->name, name)) {
			found = true;
			break;
		}

	if (!found) {
		ret = -EINVAL;
		goto unlock_out;
	}

	path->num_voters--;
	list_del_rcu(&voter->node);
	kfree_rcu(voter, rcu);

unlock_out:
	mutex_unlock(&path->voter_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_dcvs_unregister_voter);

struct kobject *qcom_dcvs_kobject_get(enum dcvs_hw_type type)
{
	struct kobject *kobj;

	if ((unsigned int)type > NUM_DCVS_HW_TYPES)
		return ERR_PTR(-EINVAL);

	if (!dcvs_ready())
		return ERR_PTR(-EPROBE_DEFER);

	if (type == NUM_DCVS_HW_TYPES)
		kobj = &dcvs_data->kobj;
	else if (dcvs_data->hw_devs[type])
		kobj = &dcvs_data->hw_devs[type]->kobj;
	else
		return ERR_PTR(-ENODEV);

	if (!kobj->state_initialized)
		return ERR_PTR(-ENODEV);

	return kobj;
}
EXPORT_SYMBOL_GPL(qcom_dcvs_kobject_get);

int qcom_dcvs_hw_minmax_get(enum dcvs_hw_type hw_type, u32 *min, u32 *max)
{
	struct dcvs_hw *hw;

	if ((unsigned int)hw_type >= NUM_DCVS_HW_TYPES)
		return -EINVAL;

	if (!dcvs_ready())
		return -EPROBE_DEFER;

	hw = dcvs_data->hw_devs[hw_type];
	if (!hw)
		return -ENODEV;

	*min = hw->hw_min_freq;
	*max = hw->hw_max_freq;

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_dcvs_hw_minmax_get);

struct device_node *qcom_dcvs_get_ddr_child_node(
				struct device_node *of_parent)
{
	struct device_node *of_child;
	int dcvs_ddr_type = -1;
	int of_ddr_type = of_fdt_get_ddrtype();
	int ret;

	for_each_child_of_node(of_parent, of_child) {
		ret = of_property_read_u32(of_child, "qcom,ddr-type",
						&dcvs_ddr_type);
		if (!ret && (dcvs_ddr_type == of_ddr_type))
			return of_child;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(qcom_dcvs_get_ddr_child_node);

static bool qcom_dcvs_hw_and_paths_inited(void)
{
	int i;
	struct dcvs_hw *hw;

	if (!dcvs_data->root_ready || dcvs_data->num_inited_hw < dcvs_data->num_hw)
		return false;

	for (i = 0; i < NUM_DCVS_HW_TYPES; i++) {
		hw = dcvs_data->hw_devs[i];
		if (!hw)
			continue;
		if (!hw->populated || hw->num_inited_paths < hw->num_paths)
			return false;
	}

	return true;
}

#define FTBL_PROP	"qcom,freq-tbl"
static int populate_freq_table(struct device *dev, u32 **freq_table)
{
	int ret, len;
	struct device_node *of_tbl_node __free(device_node) = NULL;
	struct device_node *of_node __free(device_node) = NULL;

	of_node = of_parse_phandle(dev->of_node, FTBL_PROP, 0);
	if (!of_node)
		of_node = of_node_get(dev->of_node);

	if (of_get_child_count(of_node))
		of_tbl_node = qcom_dcvs_get_ddr_child_node(of_node);
	else
		of_tbl_node = of_node_get(of_node);

	if (!of_tbl_node || !of_find_property(of_tbl_node, FTBL_PROP, &len)) {
		dev_err(dev, "Unable to find freq tbl prop\n");
		ret = -EINVAL;
		goto out;
	}
	len /= sizeof(**freq_table);
	if (!len) {
		dev_err(dev, "Error: empty freq table\n");
		ret = -EINVAL;
		goto out;
	}

	*freq_table = devm_kzalloc(dev, len * sizeof(**freq_table), GFP_KERNEL);
	if (!*freq_table) {
		ret = -ENOMEM;
		goto out;
	}

	ret = of_property_read_u32_array(of_tbl_node, FTBL_PROP, *freq_table, len);
	if (ret < 0) {
		dev_err(dev, "Error reading freq table from DT: %d\n", ret);
		goto out;
	}
	ret = len;

out:
	return ret;
}

static void dcvs_root_cleanup(void *arg)
{
	struct qcom_dcvs_data *data = arg;

	mutex_lock(&dcvs_init_lock);
	WRITE_ONCE(data->inited, false);
	if (dcvs_data == data)
		WRITE_ONCE(dcvs_data, NULL);
	mutex_unlock(&dcvs_init_lock);
	synchronize_rcu();
	kobject_del(&data->kobj);
	kobject_put(&data->kobj);
}

static void dcvs_hw_cleanup(void *arg)
{
	struct dcvs_hw *hw = arg;

	mutex_lock(&dcvs_init_lock);
	if (dcvs_data && dcvs_data->hw_devs[hw->type] == hw) {
		WRITE_ONCE(dcvs_data->inited, false);
		dcvs_data->hw_devs[hw->type] = NULL;
		dcvs_data->num_inited_hw--;
	}
	mutex_unlock(&dcvs_init_lock);
	if (dev_get_drvdata(hw->dev) == hw)
		dev_set_drvdata(hw->dev, NULL);
	kobject_del(&hw->kobj);
	kobject_put(&hw->kobj);
}

static int qcom_dcvs_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev, *dev_root;
	struct qcom_dcvs_data *data;
	int ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	kobject_init(&data->kobj, &qcom_dcvs_ktype);

	data->num_hw = of_get_available_child_count(dev->of_node);
	if (!data->num_hw) {
		ret = -ENODEV;
		goto put_kobj;
	}

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (!dev_root) {
		ret = -ENODEV;
		goto put_kobj;
	}
	ret = kobject_add(&data->kobj, &dev_root->kobj, "bus_dcvs");
	put_device(dev_root);
	if (ret)
		goto put_kobj;

	ret = devm_add_action_or_reset(dev, dcvs_root_cleanup, data);
	if (ret)
		return ret;
	WRITE_ONCE(dcvs_data, data);
	return 0;

put_kobj:
	kobject_put(&data->kobj);
	return ret;
}

static int qcom_dcvs_hw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	enum dcvs_hw_type hw_type = NUM_DCVS_HW_TYPES;
	struct dcvs_hw *hw = NULL;
	int ret = 0;

	if (!dcvs_data) {
		dev_err(dev, "Missing QCOM DCVS dev data\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(dev->of_node, QCOM_DCVS_HW_PROP, &hw_type);
	if (ret < 0 || (unsigned int)hw_type >= NUM_DCVS_HW_TYPES) {
		dev_err(dev, "Invalid dcvs hw type:%d, ret:%d\n", hw_type, ret);
		return -ENODEV;
	}

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return -ENOMEM;
	hw->dev = dev;
	hw->type = hw_type;
	kobject_init(&hw->kobj, &dcvs_hw_ktype);
	hw->num_paths = of_get_available_child_count(dev->of_node);
	if (!hw->num_paths) {
		dev_err(dev, "No dcvs paths provided!\n");
		ret = -ENODEV;
		goto put_kobj;
	}

	if (hw_type == DCVS_L3)
		ret = populate_l3_table(dev, &hw->freq_table);
	else
		ret = populate_freq_table(dev, &hw->freq_table);

	if (ret <= 0) {
		dev_err(dev, "Error reading freq table: %d\n", ret);
		if (!ret)
			ret = -EINVAL;
		goto put_kobj;
	}
	hw->table_len = ret;

	hw->hw_max_freq = hw->freq_table[hw->table_len-1];
	hw->hw_min_freq = hw->freq_table[0];

	ret = of_property_read_u32(dev->of_node, QCOM_DCVS_WIDTH_PROP,
								&hw->width);
	if (ret < 0 || !hw->width) {
		dev_err(dev, "Missing or invalid bus-width: %d\n", ret);
		ret = -EINVAL;
		goto put_kobj;
	}

	ret = devm_add_action_or_reset(dev, dcvs_hw_cleanup, hw);
	if (ret)
		return ret;

	ret = kobject_add(&hw->kobj, &dcvs_data->kobj, "%s", dcvs_hw_names[hw_type]);
	if (ret < 0) {
		dev_err(dev, "failed to init dcvs hw kobj: %d\n", ret);
		return ret;
	}
	dev_dbg(dev, "Created hw kobj: %s\n", kobject_name(&hw->kobj));

	dev_set_drvdata(dev, hw);
	mutex_lock(&dcvs_init_lock);
	if (dcvs_data->hw_devs[hw_type]) {
		mutex_unlock(&dcvs_init_lock);
		return -EEXIST;
	}
	dcvs_data->hw_devs[hw_type] = hw;
	dcvs_data->num_inited_hw++;
	mutex_unlock(&dcvs_init_lock);

	return ret;

put_kobj:
	kobject_put(&hw->kobj);
	return ret;
}

static void dcvs_path_cleanup(void *arg)
{
	struct dcvs_path *path = arg;
	struct dcvs_hw *hw = path->hw;

	mutex_lock(&dcvs_init_lock);
	if (hw->dcvs_paths[path->type] == path) {
		if (dcvs_data)
			WRITE_ONCE(dcvs_data->inited, false);
		hw->dcvs_paths[path->type] = NULL;
		hw->num_inited_paths--;
	}
	mutex_unlock(&dcvs_init_lock);
	if (path->type == DCVS_FAST_PATH)
		teardown_ddrllcc_fp_device(hw, path);
}

static int qcom_dcvs_path_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	enum dcvs_path_type path_type = NUM_DCVS_PATHS;
	struct dcvs_hw *hw = dev_get_drvdata(dev->parent);
	struct dcvs_path *path = NULL;
	struct dcvs_freq new_freqs[NUM_DCVS_HW_TYPES];

	if (!hw) {
		dev_err(dev, "QCOM DCVS HW not configured\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(dev->of_node, QCOM_DCVS_PATH_PROP,
								&path_type);
	if (ret < 0 || (unsigned int)path_type >= NUM_DCVS_PATHS) {
		dev_err(dev, "Invalid path type:%d, ret:%d\n", path_type, ret);
		return -ENODEV;
	}

	path = devm_kzalloc(dev, sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;
	path->dev = dev;
	path->type = path_type;
	path->hw = hw;

	switch (path_type) {
	case DCVS_SLOW_PATH:
		if (hw->type == DCVS_DDR || hw->type == DCVS_LLCC
					|| hw->type == DCVS_DDRQOS
					|| hw->type == DCVS_UBWCP)
			ret = setup_icc_sp_device(dev, hw, path);
		else if (hw->type == DCVS_L3)
			ret = setup_epss_l3_sp_device(dev, hw, path);
		if (ret < 0) {
			dev_err(dev, "Error setting up sp dev: %d\n", ret);
			return ret;
		}
		break;
	case DCVS_FAST_PATH:
		if (hw->type != DCVS_DDR && hw->type != DCVS_LLCC) {
			dev_err(dev, "Unsupported HW for dcvs fp: %d\n", ret);
			return -EINVAL;
		}
		ret = setup_ddrllcc_fp_device(dev, hw, path);
		if (ret < 0) {
			dev_err(dev, "Error setting up fp dev: %d\n", ret);
			return ret;
		}
		break;
	case DCVS_PERCPU_PATH:
		if (hw->type != DCVS_L3) {
			dev_err(dev, "Unsupported HW for path: %d\n", ret);
			return -EINVAL;
		}
		path->percpu_cur_freqs = devm_kzalloc(dev, nr_cpu_ids *
						sizeof(*path->percpu_cur_freqs),
						GFP_KERNEL);
		if (!path->percpu_cur_freqs)
			return -ENOMEM;
		ret = setup_epss_l3_percpu_device(dev, hw, path);
		if (ret < 0) {
			dev_err(dev, "Error setting up percpu dev: %d\n", ret);
			return ret;
		}
		break;
	default:
		/* this should never happen */
		return -EINVAL;
	}

	INIT_LIST_HEAD(&path->voter_list);
	mutex_init(&path->voter_lock);

	/* start slow paths with boost_freq = max_freq for better boot perf */
	if (path->type == DCVS_SLOW_PATH) {
		hw->boost_freq = hw->hw_max_freq;
		new_freqs[hw->type].ib = hw->hw_max_freq;
		new_freqs[hw->type].ab = 0;
		new_freqs[hw->type].hw_type = hw->type;
		ret = path->commit_dcvs_freqs(path, &new_freqs[hw->type], 1);
		if (ret < 0)
			return dev_err_probe(dev, ret, "Failed to set initial frequency\n");
	}

	ret = devm_add_action_or_reset(dev, dcvs_path_cleanup, path);
	if (ret)
		return ret;

	mutex_lock(&dcvs_init_lock);
	if (hw->dcvs_paths[path_type]) {
		mutex_unlock(&dcvs_init_lock);
		return -EEXIST;
	}
	hw->dcvs_paths[path_type] = path;
	hw->num_inited_paths++;

	if (qcom_dcvs_hw_and_paths_inited()) {
		/* Publish device, path and voter-list initialization to consumers. */
		smp_store_release(&dcvs_data->inited, true);
	}
	mutex_unlock(&dcvs_init_lock);

	return ret;
}

static int qcom_dcvs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	const struct qcom_dcvs_spec *spec = of_device_get_match_data(dev);
	enum dcvs_type type = NUM_DCVS_TYPES;

	if (spec)
		type = spec->type;

	switch (type) {
	case QCOM_DCVS_DEV:
		if (dcvs_data) {
			dev_err(dev, "Only one qcom-dcvs device allowed\n");
			ret = -ENODEV;
			break;
		}
		ret = qcom_dcvs_dev_probe(pdev);
		if (!ret)
			ret = devm_of_platform_populate(dev);
		if (!ret) {
			mutex_lock(&dcvs_init_lock);
			dcvs_data->root_ready = true;
			if (qcom_dcvs_hw_and_paths_inited()) {
				/* Publish paths after root population has succeeded. */
				smp_store_release(&dcvs_data->inited, true);
			}
			mutex_unlock(&dcvs_init_lock);
		}
		break;
	case QCOM_DCVS_HW:
		ret = qcom_dcvs_hw_probe(pdev);
		if (!ret && of_get_available_child_count(dev->of_node))
			ret = devm_of_platform_populate(dev);
		if (!ret) {
			struct dcvs_hw *hw = dev_get_drvdata(dev);

			mutex_lock(&dcvs_init_lock);
			hw->populated = true;
			if (qcom_dcvs_hw_and_paths_inited()) {
				/* Publish paths after hardware population has succeeded. */
				smp_store_release(&dcvs_data->inited, true);
			}
			mutex_unlock(&dcvs_init_lock);
		}
		break;
	case QCOM_DCVS_PATH:
		ret = qcom_dcvs_path_probe(pdev);
		break;
	default:
		dev_err(dev, "Invalid qcom-dcvs type: %u\n", type);
		return -EINVAL;
	}

	if (ret < 0) {
		dev_err(dev, "Failed to probe qcom-dcvs device: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct qcom_dcvs_spec spec[] = {
	[0] = { QCOM_DCVS_DEV },
	[1] = { QCOM_DCVS_HW },
	[2] = { QCOM_DCVS_PATH },
};

static const struct of_device_id qcom_dcvs_match_table[] = {
	{ .compatible = "qcom,dcvs", .data = &spec[0] },
	{ .compatible = "qcom,dcvs-hw", .data = &spec[1] },
	{ .compatible = "qcom,dcvs-path", .data = &spec[2] },
	{}
};

static struct platform_driver qcom_dcvs_driver = {
	.probe = qcom_dcvs_probe,
	.driver = {
		.name = "qcom-dcvs",
		.of_match_table = qcom_dcvs_match_table,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(qcom_dcvs_driver);

MODULE_DESCRIPTION("QCOM DCVS Driver");
MODULE_LICENSE("GPL");
