// SPDX-License-Identifier: GPL-2.0
/*
 *mca_charger_thermal.c
 *
 * mca strategy class driver
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/thermal.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_smem.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_voter.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/protocol/protocol_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include <mca/strategy/strategy_class.h>
#include "inc/mca_charger_thermal.h"

#define MCA_LOG_TAG "mca_thermal"
#define QC_27W_SIC_INIT_FCC 6000
#define SIC_PWR_MAX_POWER_THR 68

static DEFINE_MUTEX(thermal_instance_lock);
DEFINE_STATIC_SRCU(thermal_instance_srcu);
static struct mca_thermal_info __rcu *thermal_instance;
static bool thermal_claimed;

static const struct mca_sic_data sic_init_list[] = {
	{ 33, 6600 },
	{ 67, 12400 },
	{ 90, 15600 },
	{ 120, 22000 },
};

static const char *const wired_voter_names[THERMAL_MODE_MAX] = {
	"buck_5v_in",  "buck_9v_in", "buck_5v_ich", "buck_9v_ich",
	"div1_single", "div1_multi", "div2_single", "div2_multi",
	"div4_single", "div4_multi",
};

static const char *const wireless_voter_names[THERMAL_MODE_WIRELESS_MAX] = {
	"wireless_bpp_in",	   "wireless_bppqc2_in",
	"wireless_bppqc3_in",	   "wireless_epp_in",
	"wireless_auth_20w",	   "wireless_auth_30w",
	"wireless_auth_50w",	   "wireless_auth_80w",
	"wireless_auth_voice_box", "wireless_auth_magnet_30w",
};

static int thermal_validate_level(int level, int maximum)
{
	return level < 0 || maximum < 0 || maximum > MAX_THERMAL_LEVEL ||
			       level > maximum ?
		       -ERANGE :
		       0;
}

static int thermal_vote(struct mca_votable *voter, const char *client,
			bool enabled, int milliamps, int *result)
{
	int ret;

	if (!voter)
		return -EAGAIN;
	ret = mca_vote(voter, client, enabled, milliamps);
	if (ret && !*result)
		*result = ret;
	return ret;
}

static bool thermal_boot_override(void)
{
	int zero_speed = 0;

	return ktime_get_boottime_seconds() < 60 &&
	       !get_smem_battery_info(&zero_speed) && zero_speed == 1;
}

static int thermal_apply_flip(struct mca_thermal_info *info, bool wireless)
{
	struct mca_thermal_ctrl_info *ctrl =
		wireless ? &info->wireless_ctrl_info : &info->wired_ctrl_info;
	struct mca_votable *voter = wireless ? info->wls_flip_voter :
					       info->flip_voter;
	int *data = wireless ? info->wireless_flip_thermal_data :
			       info->wired_flip_thermal_data;
	bool removed = wireless ? ctrl->wls_flip_thermal_remove :
				  ctrl->wired_thermal_remove;
	int ret = 0;

	if (thermal_validate_level(ctrl->limit_level, ctrl->max_level) ||
	    ctrl->limit_level >= MAX_THERMAL_LEVEL)
		return -ERANGE;
	if (!ctrl->voter_ok)
		return 0;
	if (thermal_boot_override() ||
	    !(wireless ? info->wls_online : info->online))
		return 0;
	thermal_vote(voter, "mca_thermal", !removed, data[ctrl->limit_level],
		     &ret);
	return ret;
}

static int thermal_apply_wired(struct mca_thermal_info *info)
{
	struct mca_thermal_ctrl_info *ctrl = &info->wired_ctrl_info;
	int currents[THERMAL_MODE_MAX] = { 0 };
	const int *table;
	bool cancel_iin;
	int i, ret = 0;

	if (thermal_validate_level(ctrl->limit_level, ctrl->max_level))
		return -ERANGE;
	if (!ctrl->voter_ok || !info->online || thermal_boot_override())
		return 0;
	if (info->support_base_flip)
		return thermal_apply_flip(info, false);
	table = ctrl->limit_level ?
			info->wired_thermal_data[ctrl->limit_level - 1]
				.milliamps :
			NULL;
	for (i = THERMAL_MODE_BUCK_5V_ICH; i < THERMAL_MODE_MAX; i++)
		currents[i] = !table || !table[i] ? ctrl->chg_curr :
			      !ctrl->chg_curr	  ? table[i] :
						    min(table[i], ctrl->chg_curr);
	cancel_iin = !ctrl->limit_level ||
		     info->real_type == XM_CHARGER_TYPE_HVDCP3_B ||
		     info->real_type == XM_CHARGER_TYPE_HVDCP3P5 ||
		     info->real_type == XM_CHARGER_TYPE_PPS ||
		     info->real_type == XM_CHARGER_TYPE_PD_VERIFY;
	if (!cancel_iin)
		for (i = 0; i <= THERMAL_MODE_INLIMIT; i++)
			currents[i] = table[i];
	for (i = 0; i < THERMAL_MODE_MAX; i++)
		thermal_vote(info->wired_voter[i], "mca_thermal",
			     !ctrl->wired_thermal_remove && currents[i] > 0,
			     currents[i], &ret);
	return ret;
}

static int thermal_apply_wireless(struct mca_thermal_info *info)
{
	struct mca_thermal_ctrl_info *ctrl = &info->wireless_ctrl_info;
	int level = info->smartchg_data.wls_super_sts ?
			    ctrl->quickchg_limit_level :
			    ctrl->limit_level;
	int currents[THERMAL_MODE_WIRELESS_MAX] = { 0 };
	const int *table;
	int i, ret = 0, sink;

	if (!info->support_wireless)
		return -EOPNOTSUPP;
	if (thermal_validate_level(level, ctrl->max_level))
		return -ERANGE;
	if (!ctrl->voter_ok)
		return 0;
	if (info->support_base_flip)
		return thermal_apply_flip(info, true);
	sink = mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					 MCA_EVENT_WIRELESS_THERMAL_PHONE_FLAG,
					 level > info->wireless_phone_level);
	if (sink && sink != -EOPNOTSUPP && sink != -ENODEV)
		ret = sink;
	sink = mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					 MCA_EVENT_WIRELESS_THERMAL_PHONE_FLAG,
					 level > info->wireless_phone_level);
	if (sink && sink != -EOPNOTSUPP && sink != -ENODEV && !ret)
		ret = sink;
	if (!info->wls_online)
		return ret;
	table = level ? info->wireless_thermal_data[level - 1].milliamps : NULL;
	for (i = 0; i < THERMAL_MODE_WIRELESS_MAX; i++) {
		if (table && table[i])
			currents[i] = ctrl->chg_curr ?
					      min(table[i], ctrl->chg_curr) :
					      table[i];
		else if (i > THERMAL_MODE_WIRELESS_INLIMIT)
			currents[i] = ctrl->chg_curr;
		thermal_vote(info->wireless_voter[i], "mca_wireless_thermal",
			     !ctrl->wls_thermal_remove && currents[i] > 0,
			     currents[i], &ret);
	}
	return ret;
}

int mca_set_wls_charger_thermal_remove(bool remove)
{
	struct mca_thermal_info *info;
	int index, ret;

	index = srcu_read_lock(&thermal_instance_srcu);
	info = srcu_dereference(thermal_instance, &thermal_instance_srcu);
	if (!info) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&info->policy_lock);
	if (info->stopping) {
		ret = -ENODEV;
	} else if (!info->support_wireless) {
		ret = -EOPNOTSUPP;
	} else {
		info->wireless_ctrl_info.wls_thermal_remove = remove;
		ret = thermal_apply_wireless(info);
	}
	mutex_unlock(&info->policy_lock);
out:
	srcu_read_unlock(&thermal_instance_srcu, index);
	return ret;
}
EXPORT_SYMBOL(mca_set_wls_charger_thermal_remove);

int mca_get_wls_charger_thermal_remove(bool *remove)
{
	struct mca_thermal_info *info;
	int index, ret = 0;

	if (!remove)
		return -EINVAL;
	index = srcu_read_lock(&thermal_instance_srcu);
	info = srcu_dereference(thermal_instance, &thermal_instance_srcu);
	if (!info) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&info->policy_lock);
	if (info->stopping)
		ret = -ENODEV;
	else if (!info->support_wireless)
		ret = -EOPNOTSUPP;
	else
		*remove = info->wireless_ctrl_info.wls_thermal_remove;
	mutex_unlock(&info->policy_lock);
out:
	srcu_read_unlock(&thermal_instance_srcu, index);
	return ret;
}
EXPORT_SYMBOL(mca_get_wls_charger_thermal_remove);

static void thermal_update_current(struct mca_thermal_info *info)
{
	struct mca_thermal_ctrl_info *ctrl = &info->wired_ctrl_info;

	ctrl->chg_curr = !ctrl->sic_init_fcc ?
				 ctrl->sic_chg_curr :
			 !ctrl->sic_chg_curr ?
				 ctrl->sic_init_fcc :
				 min(ctrl->sic_init_fcc, ctrl->sic_chg_curr);
}

static int thermal_initial_current(struct mca_thermal_info *info)
{
	unsigned int power;
	int qc_power = 0, ret, i;

	if (info->real_type == XM_CHARGER_TYPE_PPS ||
	    info->real_type == XM_CHARGER_TYPE_PD_VERIFY) {
		ret = mca_strategy_func_get_status(
			STRATEGY_FUNC_TYPE_QUICK_CHARGE,
			STRATEGY_STATUS_TYPE_POWER_MAX, &qc_power);
		if (ret)
			return ret;
		ret = qc_power < SIC_PWR_MAX_POWER_THR ?
			      protocol_class_get_adapter_pwr_max_power(
				      ADAPTER_PROTOCOL_PPS, &power) :
			      protocol_class_get_adapter_max_power(
				      ADAPTER_PROTOCOL_PPS, &power);
		if (ret)
			return ret;
		if (!power)
			return -ERANGE;
		for (i = 0; i < ARRAY_SIZE(sic_init_list); i++)
			if (power <= sic_init_list[i].power_max)
				break;
		if (i == ARRAY_SIZE(sic_init_list))
			i--;
		info->wired_ctrl_info.sic_init_fcc =
			sic_init_list[i].sic_init_fcc;
	} else if (info->real_type == XM_CHARGER_TYPE_HVDCP3P5 ||
		   info->real_type == XM_CHARGER_TYPE_HVDCP3_B ||
		   info->real_type == XM_CHARGER_TYPE_HVDCP3) {
		info->wired_ctrl_info.sic_init_fcc = QC_27W_SIC_INIT_FCC;
	} else {
		info->wired_ctrl_info.sic_init_fcc = 0;
	}
	thermal_update_current(info);
	return thermal_apply_wired(info);
}

static int thermal_clear_votes(struct mca_thermal_info *info, bool wireless)
{
	struct mca_votable **voters = wireless ? info->wireless_voter :
						 info->wired_voter;
	struct mca_votable *flip = wireless ? info->wls_flip_voter :
					      info->flip_voter;
	int count = wireless ? THERMAL_MODE_WIRELESS_MAX : THERMAL_MODE_MAX;
	const char *client = wireless ? "mca_wireless_thermal" : "mca_thermal";
	int i, ret = 0;

	for (i = 0; i < count; i++)
		if (voters[i])
			thermal_vote(voters[i], client, false, 0, &ret);
	if (flip)
		thermal_vote(flip, "mca_thermal", false, 0, &ret);
	return ret;
}

static int thermal_init_voters(struct mca_thermal_info *info, bool wireless)
{
	struct mca_thermal_ctrl_info *ctrl =
		wireless ? &info->wireless_ctrl_info : &info->wired_ctrl_info;
	struct mca_votable **voters = wireless ? info->wireless_voter :
						 info->wired_voter;
	const char *const *names = wireless ? wireless_voter_names :
					      wired_voter_names;
	int count = wireless ? THERMAL_MODE_WIRELESS_MAX : THERMAL_MODE_MAX;
	struct mca_votable *voter;
	int i;

	if (ctrl->voter_ok)
		return 0;
	if (info->support_base_flip) {
		voter = mca_find_votable(wireless ? "wls_thermal_flip" :
						    "thermal_flip");
		if (!voter)
			return -EAGAIN;
		if (wireless)
			info->wls_flip_voter = voter;
		else
			info->flip_voter = voter;
	} else {
		for (i = 0; i < count; i++) {
			voters[i] = mca_find_votable(names[i]);
			if (!voters[i])
				return -EAGAIN;
		}
	}
	ctrl->voter_ok = 1;
	return 0;
}

static void mca_charger_thermal_init_voter_work(struct work_struct *work)
{
	struct mca_thermal_info *info = container_of(
		work, struct mca_thermal_info, init_voter_work.work);
	int wired, wireless = 0;

	mutex_lock(&info->policy_lock);
	if (info->stopping)
		goto out;
	wired = thermal_init_voters(info, false);
	if (!wired && thermal_apply_wired(info))
		mca_log_err("wired thermal vote failed\n");
	if (info->support_wireless) {
		wireless = thermal_init_voters(info, true);
		if (!wireless && thermal_apply_wireless(info))
			mca_log_err("wireless thermal vote failed\n");
	}
	if (wired || wireless)
		queue_delayed_work(info->workqueue, &info->init_voter_work,
				   msecs_to_jiffies(1000));
out:
	mutex_unlock(&info->policy_lock);
}

static int thermal_read_table(struct device_node *node, const char *name,
			      void *data, int columns, int *maximum, bool flip)
{
	u32 values[MAX_THERMAL_LEVEL * THERMAL_MODE_MAX];
	int count, i, ret;

	count = of_property_count_u32_elems(node, name);
	if (count <= 0 || count > MAX_THERMAL_LEVEL * columns ||
	    count % columns || (flip && count != MAX_THERMAL_LEVEL))
		return -EINVAL;
	ret = of_property_read_u32_array(node, name, values, count);
	if (ret)
		return ret;
	for (i = 0; i < count; i++)
		if (values[i] > INT_MAX / 1000)
			return -ERANGE;
	memcpy(data, values, count * sizeof(*values));
	*maximum = flip ? count - 1 : count / columns;
	return 0;
}

static int thermal_parse_dt(struct mca_thermal_info *info)
{
	struct device_node *node = info->dev->of_node;
	u32 value;
	int ret, count;

	if (!node)
		return -EINVAL;
	if (of_find_property(node, "support_wireless", NULL)) {
		ret = of_property_read_u32(node, "support_wireless", &value);
		if (ret || value > 1)
			return ret ? ret : -EINVAL;
		info->support_wireless = value;
	}
	info->support_base_flip =
		of_property_read_bool(node, "support-base-flip");
	info->wireless_phone_level = 12;
	if (of_find_property(node, "wireless_phone_level", NULL)) {
		ret = of_property_read_u32(node, "wireless_phone_level",
					   &value);
		if (ret || value > MAX_THERMAL_LEVEL)
			return ret ? ret : -ERANGE;
		info->wireless_phone_level = value;
	}
	ret = thermal_read_table(
		node,
		info->support_base_flip ? "wired_flip_thermal" :
					  "wired_thermal",
		info->support_base_flip ? info->wired_flip_thermal_data :
					  (int *)info->wired_thermal_data,
		info->support_base_flip ? 1 : THERMAL_MODE_MAX,
		&info->wired_ctrl_info.max_level, info->support_base_flip);
	if (ret)
		return ret;
	if (info->support_wireless) {
		ret = thermal_read_table(
			node,
			info->support_base_flip ? "wireless_flip_thermal" :
						  "wireless_thermal",
			info->support_base_flip ?
				info->wireless_flip_thermal_data :
				(int *)info->wireless_thermal_data,
			info->support_base_flip ? 1 : THERMAL_MODE_WIRELESS_MAX,
			&info->wireless_ctrl_info.max_level,
			info->support_base_flip);
		if (ret)
			return ret;
	}
	if (of_find_property(node, "wired_sic_select", NULL)) {
		count = of_property_count_strings(node, "wired_sic_select");
		if (count <= 0 || count > BATT_CELL_SELECT_MAX)
			return -EINVAL;
		ret = of_property_read_string_array(node, "wired_sic_select",
						    info->wired_sic_select,
						    count);
		if (ret != count)
			return ret < 0 ? ret : -EINVAL;
		info->wired_sic_count = count;
	}
	return 0;
}

#ifdef CONFIG_SYSFS
static ssize_t mca_charger_thermal_sysfs_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf);
static ssize_t mca_charger_thermal_sysfs_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count);
static struct mca_sysfs_attr_info mca_charger_thermal_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_CHG_CURR, wired_chg_curr),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_CHG_CURR2, wired_chg_curr2),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_CTRL_LIMIT, wired_ctrl_limit),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_THERMAL_REMOVE,
			  wired_thermal_remove),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_CHG_CURR, wireless_chg_curr),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_CTRL_LIMIT, wireless_ctrl_limit),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_QUICK_CHG_CTRL_LIMIT,
			  wls_quick_chg_control_limit),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_THERMAL_REMOVE,
			  wireless_thermal_remove),
};
#define MCA_CHARGER_THERMAL_SYSFS_ATTRS_SIZE \
	ARRAY_SIZE(mca_charger_thermal_sysfs_field_tbl)
static struct attribute
	*mca_charger_thermal_sysfs_attrs[MCA_CHARGER_THERMAL_SYSFS_ATTRS_SIZE +
					 1];
static const struct attribute_group mca_charger_thermal_sysfs_attr_group = {
	.attrs = mca_charger_thermal_sysfs_attrs,
};

static int thermal_read_control(struct mca_thermal_info *info, int id,
				int *value)
{
	if (id >= MCA_THERMAL_WIRELESS_CHG_CURR && !info->support_wireless)
		return -EOPNOTSUPP;
	switch (id) {
	case MCA_THERMAL_WIRED_CHG_CURR:
	case MCA_THERMAL_WIRED_CHG_CURR2:
		*value = info->wired_ctrl_info.chg_curr * 1000;
		break;
	case MCA_THERMAL_WIRED_CTRL_LIMIT:
		*value = info->wired_ctrl_info.limit_level;
		break;
	case MCA_THERMAL_WIRED_THERMAL_REMOVE:
		*value = info->wired_ctrl_info.wired_thermal_remove;
		break;
	case MCA_THERMAL_WIRELESS_CHG_CURR:
		*value = info->wireless_ctrl_info.chg_curr;
		break;
	case MCA_THERMAL_WIRELESS_CTRL_LIMIT:
		*value = info->wireless_ctrl_info.limit_level;
		break;
	case MCA_THERMAL_WIRELESS_QUICK_CHG_CTRL_LIMIT:
		*value = info->wireless_ctrl_info.quickchg_limit_level;
		break;
	case MCA_THERMAL_WIRELESS_THERMAL_REMOVE:
		*value = info->wireless_ctrl_info.wls_thermal_remove;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int thermal_write_control(struct mca_thermal_info *info, int id,
				 int value)
{
	const char *cell = NULL;
	int index, ret;

	if (value < 0)
		return -ERANGE;
	if (id >= MCA_THERMAL_WIRELESS_CHG_CURR && !info->support_wireless)
		return -EOPNOTSUPP;
	switch (id) {
	case MCA_THERMAL_WIRED_CHG_CURR:
	case MCA_THERMAL_WIRED_CHG_CURR2:
		index = id == MCA_THERMAL_WIRED_CHG_CURR ? 0 : 1;
		if (index && info->wired_sic_count <= index)
			return -EOPNOTSUPP;
		if (info->wired_sic_count) {
			ret = platform_fg_ops_get_batt_cell_info(FG_IC_MASTER,
								 &cell);
			if (ret)
				return ret;
			if (!cell)
				return -ENODATA;
			if (strcmp(info->wired_sic_select[index], cell))
				return -ENODEV;
		}
		info->wired_ctrl_info.sic_chg_curr = value / 1000;
		thermal_update_current(info);
		return thermal_apply_wired(info);
	case MCA_THERMAL_WIRED_CTRL_LIMIT:
		ret = thermal_validate_level(value,
					     info->wired_ctrl_info.max_level);
		if (ret)
			return ret;
		info->wired_ctrl_info.limit_level = value;
		return thermal_apply_wired(info);
	case MCA_THERMAL_WIRED_THERMAL_REMOVE:
		if (value > 1)
			return -ERANGE;
		info->wired_ctrl_info.wired_thermal_remove = value;
		return thermal_apply_wired(info);
	case MCA_THERMAL_WIRELESS_CHG_CURR:
		info->wireless_ctrl_info.chg_curr = value / 1000;
		return thermal_apply_wireless(info);
	case MCA_THERMAL_WIRELESS_CTRL_LIMIT:
	case MCA_THERMAL_WIRELESS_QUICK_CHG_CTRL_LIMIT:
		ret = thermal_validate_level(
			value, info->wireless_ctrl_info.max_level);
		if (ret)
			return ret;
		if (id == MCA_THERMAL_WIRELESS_CTRL_LIMIT)
			info->wireless_ctrl_info.limit_level = value;
		else
			info->wireless_ctrl_info.quickchg_limit_level = value;
		return thermal_apply_wireless(info);
	case MCA_THERMAL_WIRELESS_THERMAL_REMOVE:
		if (value > 1)
			return -ERANGE;
		info->wireless_ctrl_info.wls_thermal_remove = value;
		return thermal_apply_wireless(info);
	default:
		return -EINVAL;
	}
}

static ssize_t mca_charger_thermal_sysfs_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct mca_thermal_info *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *entry;
	int ret, value;

	if (!info)
		return -ENODEV;
	entry = mca_sysfs_lookup_attr(attr->attr.name,
				      mca_charger_thermal_sysfs_field_tbl,
				      MCA_CHARGER_THERMAL_SYSFS_ATTRS_SIZE);
	if (!entry)
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	ret = info->stopping ?
		      -ENODEV :
		      thermal_read_control(info, entry->sysfs_attr_name,
					   &value);
	if (!ret)
		ret = sysfs_emit(buf, "%d\n", value);
	mutex_unlock(&info->policy_lock);
	return ret;
}

static ssize_t mca_charger_thermal_sysfs_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct mca_thermal_info *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *entry;
	int value, ret;

	if (!info)
		return -ENODEV;
	if (!count || memchr(buf, '\0', count))
		return -EINVAL;
	ret = kstrtoint(buf, 10, &value);
	if (ret)
		return ret;
	entry = mca_sysfs_lookup_attr(attr->attr.name,
				      mca_charger_thermal_sysfs_field_tbl,
				      MCA_CHARGER_THERMAL_SYSFS_ATTRS_SIZE);
	if (!entry)
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	ret = info->stopping ?
		      -ENODEV :
		      thermal_write_control(info, entry->sysfs_attr_name,
					    value);
	mutex_unlock(&info->policy_lock);
	return ret ? ret : count;
}

static int mca_charger_thermal_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(mca_charger_thermal_sysfs_attrs,
			     mca_charger_thermal_sysfs_field_tbl,
			     MCA_CHARGER_THERMAL_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group(
		"charger", "charger_thermal", dev,
		&mca_charger_thermal_sysfs_attr_group);
}

static void mca_charger_thermal_remove_group(struct device *dev)
{
	mca_sysfs_remove_link_group("charger", "charger_thermal", dev,
				    &mca_charger_thermal_sysfs_attr_group);
}
#else
static int mca_charger_thermal_create_group(struct device *dev)
{
	return 0;
}

static void mca_charger_thermal_remove_group(struct device *dev)
{
}
#endif

static int mca_charger_thermal_get_max_level(struct thermal_cooling_device *tcd,
					     unsigned long *state)
{
	struct mca_thermal_info *info = tcd->devdata;
	int ret = 0;

	if (!info || !state)
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	if (info->stopping)
		ret = -ENODEV;
	else
		*state = info->wired_ctrl_info.max_level;
	mutex_unlock(&info->policy_lock);
	return ret;
}

static int mca_charger_thermal_get_cur_level(struct thermal_cooling_device *tcd,
					     unsigned long *state)
{
	struct mca_thermal_info *info = tcd->devdata;
	int ret = 0;

	if (!info || !state)
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	if (info->stopping)
		ret = -ENODEV;
	else
		*state = info->wired_ctrl_info.limit_level;
	mutex_unlock(&info->policy_lock);
	return ret;
}

static int mca_charger_thermal_set_cur_level(struct thermal_cooling_device *tcd,
					     unsigned long state)
{
	struct mca_thermal_info *info = tcd->devdata;
	int ret;

	if (!info)
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	if (info->stopping) {
		ret = -ENODEV;
	} else if (state > info->wired_ctrl_info.max_level) {
		ret = -ERANGE;
	} else {
		info->wired_ctrl_info.limit_level = state;
		ret = thermal_apply_wired(info);
	}
	mutex_unlock(&info->policy_lock);
	return ret;
}

static const struct thermal_cooling_device_ops g_mca_charger_tcd_ops = {
	.get_max_state = mca_charger_thermal_get_max_level,
	.get_cur_state = mca_charger_thermal_get_cur_level,
	.set_cur_state = mca_charger_thermal_set_cur_level,
};

static int mca_charger_thermal_wls_super_sts_callback(void *data, int result)
{
	struct mca_thermal_info *info = data;
	int ret;

	if (!info || (result != 0 && result != 1))
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	if (info->stopping) {
		ret = -ENODEV;
	} else if (!info->support_wireless) {
		ret = -EOPNOTSUPP;
	} else {
		info->smartchg_data.wls_super_sts = result;
		ret = thermal_apply_wireless(info);
	}
	mutex_unlock(&info->policy_lock);
	return ret;
}

static int mca_charger_thermal_process_event(int event, int value, void *data)
{
	struct mca_thermal_info *info = data;
	int ret = 0;

	if (!info)
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	if (info->stopping) {
		ret = -ENODEV;
		goto out;
	}
	switch (event) {
	case MCA_EVENT_USB_CONNECT:
	case MCA_EVENT_USB_DISCONNECT:
		if (value != 0 && value != 1) {
			ret = -EINVAL;
			break;
		}
		info->online = value;
		if (value) {
			ret = thermal_apply_wired(info);
		} else {
			info->wired_ctrl_info.chg_curr = 0;
			info->wired_ctrl_info.sic_chg_curr = 0;
			info->wired_ctrl_info.sic_init_fcc = 0;
			info->real_type = XM_CHARGER_TYPE_UNKNOW;
			ret = thermal_clear_votes(info, false);
		}
		break;
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
		if (value < 0 || value > XM_CHARGER_TYPE_OCP) {
			ret = -EINVAL;
			break;
		}
		info->real_type = value;
		ret = thermal_initial_current(info);
		break;
	case MCA_EVENT_WIRELESS_CONNECT:
	case MCA_EVENT_WIRELESS_DISCONNECT:
		if (!info->support_wireless) {
			ret = -EOPNOTSUPP;
			break;
		}
		if (value != 0 && value != 1) {
			ret = -EINVAL;
			break;
		}
		info->wls_online = value;
		ret = value ? thermal_apply_wireless(info) :
			      thermal_clear_votes(info, true);
		break;
	case MCA_EVENT_WIRELESS_EPP_MODE:
		ret = thermal_apply_wireless(info);
		break;
	default:
		ret = -EOPNOTSUPP;
	}
out:
	mutex_unlock(&info->policy_lock);
	return ret;
}

static int mca_charge_thermal_dump_log_head(void *data, char *buf, int size)
{
	if (!buf || size <= 0)
		return -EINVAL;
	return scnprintf(buf, size, "W_TL W_SIC   WL_TL WL_STL WL_SIC   ");
}

static int mca_charge_thermal_dump_log_context(void *data, char *buf, int size)
{
	struct mca_thermal_info *info = data;
	int ret;

	if (!info || !buf || size <= 0)
		return -EINVAL;
	mutex_lock(&info->policy_lock);
	ret = info->stopping ?
		      -ENODEV :
		      scnprintf(buf, size, "%-5d%-8d%-6d%-7d%-9d",
				info->wired_ctrl_info.limit_level,
				info->wired_ctrl_info.chg_curr,
				info->wireless_ctrl_info.limit_level,
				info->wireless_ctrl_info.quickchg_limit_level,
				info->wireless_ctrl_info.chg_curr);
	mutex_unlock(&info->policy_lock);
	return ret;
}

static struct mca_log_charge_log_ops g_mca_thermal_log_ops = {
	.dump_log_head = mca_charge_thermal_dump_log_head,
	.dump_log_context = mca_charge_thermal_dump_log_context,
};

static void thermal_stop(struct mca_thermal_info *info)
{
	int ret;

	if (!info)
		return;
	mutex_lock(&info->policy_lock);
	if (info->stopping) {
		mutex_unlock(&info->policy_lock);
		return;
	}
	info->stopping = true;
	mutex_unlock(&info->policy_lock);
	mutex_lock(&thermal_instance_lock);
	if (rcu_access_pointer(thermal_instance) == info)
		rcu_assign_pointer(thermal_instance, NULL);
	mutex_unlock(&thermal_instance_lock);
	synchronize_srcu(&thermal_instance_srcu);
	if (info->strategy_registered)
		mca_strategy_ops_unregister(STRATEGY_FUNC_TYPE_THERMAL, info);
	if (info->smart_registered)
		mca_smartchg_if_ops_unregister(MCA_SMARTCHG_IF_CHG_TYPE_THERMAL,
					       info);
	if (info->log_registered)
		mca_log_charge_log_unregister(MCA_CHARGE_LOG_ID_THERMAL, info);
	if (info->sysfs_registered)
		mca_charger_thermal_remove_group(info->dev);
	if (info->tcd) {
		thermal_cooling_device_unregister(info->tcd);
		info->tcd = NULL;
	}
	if (info->workqueue) {
		cancel_delayed_work_sync(&info->init_voter_work);
		destroy_workqueue(info->workqueue);
		info->workqueue = NULL;
	}
	ret = thermal_clear_votes(info, false);
	if (info->support_wireless && thermal_clear_votes(info, true) && !ret)
		ret = -EIO;
	if (ret)
		mca_log_err("thermal vote teardown failed: %d\n", ret);
	mutex_lock(&thermal_instance_lock);
	if (info->instance_claimed) {
		thermal_claimed = false;
		info->instance_claimed = false;
	}
	mutex_unlock(&thermal_instance_lock);
}

static int mca_charger_thermal_probe(struct platform_device *pdev)
{
	struct mca_smartchg_if_ops smart_ops = {
		.type = MCA_SMARTCHG_IF_CHG_TYPE_THERMAL,
		.set_wls_super_sts = mca_charger_thermal_wls_super_sts_callback,
	};
	struct mca_thermal_info *info;
	int ret;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->dev = &pdev->dev;
	mutex_init(&info->policy_lock);
	INIT_DELAYED_WORK(&info->init_voter_work,
			  mca_charger_thermal_init_voter_work);
	mutex_lock(&thermal_instance_lock);
	if (thermal_claimed) {
		mutex_unlock(&thermal_instance_lock);
		return -EBUSY;
	}
	thermal_claimed = true;
	info->instance_claimed = true;
	mutex_unlock(&thermal_instance_lock);
	platform_set_drvdata(pdev, info);
	ret = thermal_parse_dt(info);
	if (ret)
		goto fail;
	info->workqueue =
		alloc_ordered_workqueue("mca_thermal", WQ_MEM_RECLAIM);
	if (!info->workqueue) {
		ret = -ENOMEM;
		goto fail;
	}
	thermal_init_voters(info, false);
	if (info->support_wireless)
		thermal_init_voters(info, true);
	ret = mca_charger_thermal_create_group(info->dev);
	if (ret)
		goto fail;
	info->sysfs_registered = true;
	ret = mca_strategy_ops_register(STRATEGY_FUNC_TYPE_THERMAL,
					mca_charger_thermal_process_event, NULL,
					NULL, info);
	if (ret)
		goto fail;
	info->strategy_registered = true;
	info->tcd = thermal_of_cooling_device_register(
		info->dev->of_node, "battery", info, &g_mca_charger_tcd_ops);
	if (IS_ERR(info->tcd)) {
		ret = PTR_ERR(info->tcd);
		info->tcd = NULL;
		goto fail;
	}
	if (info->support_wireless) {
		smart_ops.data = info;
		ret = mca_smartchg_if_ops_register(&smart_ops);
		if (ret)
			goto fail;
		info->smart_registered = true;
	}
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_THERMAL,
				    &g_mca_thermal_log_ops, info);
	info->log_registered = true;
	mutex_lock(&thermal_instance_lock);
	rcu_assign_pointer(thermal_instance, info);
	mutex_unlock(&thermal_instance_lock);
	mutex_lock(&info->policy_lock);
	if (!info->wired_ctrl_info.voter_ok ||
	    (info->support_wireless && !info->wireless_ctrl_info.voter_ok))
		queue_delayed_work(info->workqueue, &info->init_voter_work,
				   msecs_to_jiffies(1000));
	mutex_unlock(&info->policy_lock);
	return 0;
fail:
	thermal_stop(info);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static void mca_charger_thermal_remove(struct platform_device *pdev)
{
	thermal_stop(platform_get_drvdata(pdev));
	platform_set_drvdata(pdev, NULL);
}

static void mca_charger_thermal_shutdown(struct platform_device *pdev)
{
	thermal_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca_charger_thermal" },
	{},
};
MODULE_DEVICE_TABLE(of, match_table);
static struct platform_driver mca_charger_thermal_driver = {
	.driver = { .name = "mca_charger_thermal",
		    .of_match_table = match_table },
	.probe = mca_charger_thermal_probe,
	.remove = mca_charger_thermal_remove,
	.shutdown = mca_charger_thermal_shutdown,
};
module_platform_driver(mca_charger_thermal_driver);
MODULE_DESCRIPTION("charge thermal");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
