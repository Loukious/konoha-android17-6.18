// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
 #define pr_fmt(fmt) "platform_mpam: " fmt

#include <linux/io.h>
#include <linux/of.h>
#include <linux/configfs.h>
#include <linux/string.h>
#include <soc/qcom/mpam.h>

#define platform_mpam_get(ptr, member) ({	\
	(mpam_version.version >= 0x10002) ?	\
	ptr->V2.member : ptr->V1.member; })

struct platform_mpam_item {
	struct config_group group;
	int msc_id;
	int client_id;
	int monitor_index;
	bool monitor_enabled;
	struct platform_mpam_bw_ctrl_config *cfg;
};

struct platform_mpam_gear {
	int gear_id;
	const char *gear_name;
};

static int support_gear_cnt;
static struct mpam_ver_ret mpam_version;
static struct config_group *root_group;
static struct platform_mpam_gear *support_gears;
static void __iomem *mpam_mon_base;
static struct config_group *msc_group;
static DEFINE_MUTEX(platform_mpam_lock);

static inline struct platform_mpam_item *get_pm_item(
					   struct config_item *item)
{
	return container_of(to_config_group(item),
				struct platform_mpam_item, group);
}

static ssize_t platform_mpam_schemata_show_locked(struct config_item *item,
		char *page)
{
	return scnprintf(page, PAGE_SIZE, "gear=%d\n",
		get_pm_item(item)->cfg->platform_mpam_gear);
}

static ssize_t platform_mpam_schemata_store_locked(struct config_item *item,
		const char *page, size_t count)
{
	int ret, i;
	u32 gear;
	struct platform_mpam_bw_ctrl_cfg cfg = {};
	struct platform_mpam_item *pm_item = get_pm_item(item);

	ret = qcom_mpam_parse_gear(page, count, &gear);
	if (ret)
		return ret;
	for (i = 0; i < support_gear_cnt; i++)
		if (support_gears[i].gear_id == gear)
			break;
	if (i == support_gear_cnt)
		return -EINVAL;
	cfg.msc_id = pm_item->msc_id;
	cfg.client_id = pm_item->client_id;
	cfg.platform_mpam_gear = gear;
	ret = qcom_mpam_set_platform_bw_ctrl(&cfg);
	if (!ret)
		pm_item->cfg->platform_mpam_gear = gear;
	return ret ? ret : count;
}
static ssize_t platform_mpam_schemata_store(struct config_item *item, const char *page, size_t count)
{
	ssize_t ret;
	mutex_lock(&platform_mpam_lock);
	ret = platform_mpam_schemata_store_locked(item, page, count);
	mutex_unlock(&platform_mpam_lock);
	return ret;
}

static ssize_t platform_mpam_schemata_show(struct config_item *item, char *page)
{
	ssize_t ret;
	mutex_lock(&platform_mpam_lock);
	ret = platform_mpam_schemata_show_locked(item, page);
	mutex_unlock(&platform_mpam_lock);
	return ret;
}

CONFIGFS_ATTR(platform_mpam_, schemata);

static ssize_t platform_mpam_enable_monitor_show_locked(struct config_item *item,
		char *page)
{
	return scnprintf(page, PAGE_SIZE, "%s\n",
		(get_pm_item(item)->monitor_enabled) ? "enabled" : "disabled");
}

static ssize_t platform_mpam_enable_monitor_store_locked(struct config_item *item,
		const char *page, size_t count)
{
	int ret;
	bool input;
	struct platform_mpam_bw_monitor_cfg monitor_param = {};
	struct platform_mpam_item *pm_item = get_pm_item(item);

	monitor_param.msc_id = pm_item->msc_id;
	monitor_param.client_id = pm_item->client_id;
	monitor_param.mon_instance = pm_item->monitor_index;
	monitor_param.mon_type = MPAM_TYPE_MBW_MONITOR;

	ret = kstrtobool(page, &input);
	if (ret) {
		pr_err("invalid argument\n");
		return ret;
	}

	if (!input && pm_item->monitor_enabled)
		monitor_param.config_ctrl = 0;
	else if (input && !pm_item->monitor_enabled)
		monitor_param.config_ctrl = 1;
	else
		goto exit;

	ret = qcom_mpam_set_platform_bw_monitor(&monitor_param);
	if (ret) {
		pr_err("monitor %s failed %d\n",
			(input) ? "enable" : "disable", ret);
		return ret;
	}
	pm_item->monitor_enabled = input;

exit:
	return count;
}
static ssize_t platform_mpam_enable_monitor_store(struct config_item *item, const char *page, size_t count)
{
	ssize_t ret;
	mutex_lock(&platform_mpam_lock);
	ret = platform_mpam_enable_monitor_store_locked(item, page, count);
	mutex_unlock(&platform_mpam_lock);
	return ret;
}

static ssize_t platform_mpam_enable_monitor_show(struct config_item *item, char *page)
{
	ssize_t ret;
	mutex_lock(&platform_mpam_lock);
	ret = platform_mpam_enable_monitor_show_locked(item, page);
	mutex_unlock(&platform_mpam_lock);
	return ret;
}

CONFIGFS_ATTR(platform_mpam_, enable_monitor);

static ssize_t platform_mpam_monitor_data_show_locked(struct config_item *item,
		char *page)
{
	int index, retry;
	u32 seq;
	u64 byte_cnt, timestamp;
	void __iomem *data;
	bool v2 = mpam_version.version >= 0x10002;

	if (!get_pm_item(item)->monitor_enabled)
		return scnprintf(page, PAGE_SIZE, "monitor not enabled\n");
	index = get_pm_item(item)->monitor_index;
	data = mpam_mon_base + index * (v2 ?
		sizeof(((union platform_monitor_value *)0)->V2) :
		sizeof(((union platform_monitor_value *)0)->V1));
	for (retry = 0; retry < MPAM_MAX_RETRY; retry++) {
		seq = readl_relaxed(data);
		if (seq & 1)
			continue;
		byte_cnt = v2 ? readq_relaxed(data + 16) : readl_relaxed(data + 12);
		timestamp = readq_relaxed(data + (v2 ? 24 : 16));
		if (seq == readl_relaxed(data))
			return scnprintf(page, PAGE_SIZE, "timestamp=%llu,byte_cnt=%llu\n",
					 timestamp, byte_cnt);
	}
	return -EAGAIN;
}
static ssize_t platform_mpam_monitor_data_show(struct config_item *item, char *page)
{
	ssize_t ret;
	mutex_lock(&platform_mpam_lock);
	ret = platform_mpam_monitor_data_show_locked(item, page);
	mutex_unlock(&platform_mpam_lock);
	return ret;
}

CONFIGFS_ATTR_RO(platform_mpam_, monitor_data);

static ssize_t platform_mpam_available_gear_show_locked(struct config_item *item,
		char *page)
{
	int i;
	size_t len = 0;

	for (i = 0; i < support_gear_cnt; i++) {
		len += scnprintf(page + len, PAGE_SIZE - len,
			"%d - %s\n", support_gears[i].gear_id,
			support_gears[i].gear_name);
	}

	return len;
}
static ssize_t platform_mpam_available_gear_show(struct config_item *item, char *page)
{
	ssize_t ret;
	mutex_lock(&platform_mpam_lock);
	ret = platform_mpam_available_gear_show_locked(item, page);
	mutex_unlock(&platform_mpam_lock);
	return ret;
}

CONFIGFS_ATTR_RO(platform_mpam_, available_gear);

static struct configfs_attribute *platform_mpam_attrs[] = {
	&platform_mpam_attr_schemata,
	&platform_mpam_attr_enable_monitor,
	&platform_mpam_attr_monitor_data,
	&platform_mpam_attr_available_gear,
	NULL,
};

static void platform_mpam_item_release(struct config_item *item)
{
	struct platform_mpam_item *pm_item = get_pm_item(item);
	kfree(pm_item->cfg);
	kfree(pm_item);
}

static struct configfs_item_operations platform_mpam_item_ops = {
	.release = platform_mpam_item_release,
};

static const struct config_item_type platform_mpam_item_type = {
	.ct_attrs	= platform_mpam_attrs,
	.ct_item_ops = &platform_mpam_item_ops,
	.ct_owner = THIS_MODULE,
};

static struct platform_mpam_item *platform_mpam_make_group(
		struct device *dev, const char *name)
{
	struct platform_mpam_item *item;

	item = kzalloc(
		sizeof(struct platform_mpam_item), GFP_KERNEL);
	if (!item)
		return ERR_PTR(-ENOMEM);

	item->cfg = kzalloc(
		sizeof(struct platform_mpam_bw_ctrl_config), GFP_KERNEL);
	if (!item->cfg) {
		kfree(item);
		return ERR_PTR(-ENOMEM);
	}

	config_group_init_type_name(&item->group, name,
				   &platform_mpam_item_type);

	return item;
}

static void platform_mpam_base_release(struct config_item *item)
{
	kfree(to_config_group(item));
}
static struct configfs_item_operations platform_mpam_base_ops = {
	.release = platform_mpam_base_release,
};
static const struct config_item_type platform_mpam_base_type = {
	.ct_item_ops = &platform_mpam_base_ops,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem platform_mpam_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "platform_mpam",
			.ci_type = &platform_mpam_base_type,
		},
	},
};

static void platform_mpam_free_groups(void)
{
	if (msc_group) {
		configfs_remove_default_groups(msc_group);
		config_item_put(&msc_group->cg_item);
		msc_group = NULL;
	}
	INIT_LIST_HEAD(&platform_mpam_subsys.su_group.default_groups);
}

static int platform_mpam_probe(struct platform_device *pdev)
{
	int i, ret, client_cnt;
	u32 mscid, clientid;
	const char *name;
	struct resource *res;
	struct device_node *node, *np = pdev->dev.of_node;
	struct platform_mpam_item *new_item;
	struct platform_mpam_read_bw_ctrl bw_ctrl_param = {};
	size_t stride;

	ret = qcom_mpam_get_version(&mpam_version);
	if (ret)
		return ret == -ENODEV ? -EPROBE_DEFER : ret;
	if (mpam_version.version < 0x10000)
		return -ENODEV;
	client_cnt = of_get_available_child_count(np);
	support_gear_cnt = of_property_count_strings(np, "qcom,gears");
	if (!client_cnt || support_gear_cnt <= 0 ||
	    of_property_count_u32_elems(np, "qcom,gear-id") != support_gear_cnt)
		return -EINVAL;
	support_gears = devm_kcalloc(&pdev->dev, support_gear_cnt,
				     sizeof(*support_gears), GFP_KERNEL);
	if (!support_gears)
		return -ENOMEM;
	for (i = 0; i < support_gear_cnt; i++) {
		ret = of_property_read_string_index(np, "qcom,gears", i,
						   &support_gears[i].gear_name);
		if (ret)
			return ret;
		ret = of_property_read_u32_index(np, "qcom,gear-id", i,
						&support_gears[i].gear_id);
		if (ret)
			return ret;
	}
	ret = of_property_read_u32(np, "qcom,msc-id", &mscid);
	if (ret || mscid >= MSC_MAX)
		return -EINVAL;
	ret = of_property_read_string(np, "qcom,msc-name", &name);
	if (ret)
		return ret;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mon-base");
	if (!res)
		return -EINVAL;
	stride = mpam_version.version >= 0x10002 ?
		sizeof(((union platform_monitor_value *)0)->V2) :
		sizeof(((union platform_monitor_value *)0)->V1);
	if (client_cnt > resource_size(res) / stride)
		return -EINVAL;
	mpam_mon_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mpam_mon_base))
		return PTR_ERR(mpam_mon_base);
	config_group_init(&platform_mpam_subsys.su_group);
	mutex_init(&platform_mpam_subsys.su_mutex);
	msc_group = kzalloc(sizeof(*msc_group), GFP_KERNEL);
	if (!msc_group)
		return -ENOMEM;
	config_group_init_type_name(msc_group, name, &platform_mpam_base_type);
	configfs_add_default_group(msc_group, &platform_mpam_subsys.su_group);
	i = 0;
	for_each_available_child_of_node(np, node) {
		ret = of_property_read_u32(node, "qcom,client-id", &clientid);
		if (!ret)
			ret = of_property_read_string(node, "qcom,client-name", &name);
		if (ret)
			goto put_node;
		new_item = platform_mpam_make_group(&pdev->dev, name);
		if (IS_ERR(new_item)) {
			ret = PTR_ERR(new_item);
			goto put_node;
		}
		new_item->monitor_index = i++;
		new_item->msc_id = mscid;
		new_item->client_id = clientid;
		bw_ctrl_param.msc_id = mscid;
		bw_ctrl_param.client_id = clientid;
		ret = qcom_mpam_get_platform_bw_ctrl(&bw_ctrl_param, new_item->cfg);
		if (ret) {
			config_item_put(&new_item->group.cg_item);
			goto put_node;
		}
		configfs_add_default_group(&new_item->group, msc_group);
	}
	ret = configfs_register_subsystem(&platform_mpam_subsys);
	if (ret)
		goto free_groups;
	WRITE_ONCE(root_group, &platform_mpam_subsys.su_group);
	return 0;
put_node:
	of_node_put(node);
free_groups:
	platform_mpam_free_groups();
	return ret;
}

static void platform_mpam_remove(struct platform_device *pdev)
{
	WRITE_ONCE(root_group, NULL);
	configfs_unregister_subsystem(&platform_mpam_subsys);
	platform_mpam_free_groups();
}

struct config_group *platform_mpam_get_root_group(void)
{
	return READ_ONCE(root_group);
}
EXPORT_SYMBOL_GPL(platform_mpam_get_root_group);

static const struct of_device_id platform_mpam_table[] = {
	{ .compatible = "qcom,platform-mpam" },
	{}
};
MODULE_DEVICE_TABLE(of, platform_mpam_table);

static struct platform_driver platform_mpam_driver = {
	.driver = {
		.name = "platform-mpam",
		.of_match_table = platform_mpam_table,
		.suppress_bind_attrs = true,
	},
	.probe = platform_mpam_probe,
	.remove = platform_mpam_remove,
};

module_platform_driver(platform_mpam_driver);

MODULE_SOFTDEP("pre: mpam");
MODULE_DESCRIPTION("QCOM Platform MPAM driver");
MODULE_LICENSE("GPL");
