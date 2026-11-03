// SPDX-License-Identifier: GPL-2.0
/*
 * mca_vbat_ovp_monitor.c
 *
 * battery ocp software detection and protection driver
 *
 * Copyright (c) 2024-2024 Xiaomi Technologies Co., Ltd.
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
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <mca/common/mca_parse_dts.h>
#include "inc/mca_ibat_ocp_monitor.h"
#include <mca/common/mca_sysfs.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/platform/platform_loadsw_class.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_event.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_ibat_ocp_mon"
#endif

#define FAST_HEARTBEAT_TIMER_MS 10000
#define NORMAL_HEARTBEAT_TIMER_MS 60000

static int mca_ibat_mon_get_ibat_ocp_flag(struct mca_ibat_ocp_mon_dev *info)
{
	int role, current, ret, flag = 0;
	s64 magnitude;

	for (role = FG_IC_MASTER; role <= FG_IC_SLAVE; role++) {
		ret = platform_fg_ops_get_curr(role, &current);
		if (ret)
			return ret;
		/* Native FG current and thresholds are microamps, despite _ma names. */
		magnitude = current < 0 ? -(s64)current : current;
		magnitude = max_t(s64, magnitude, role == FG_IC_MASTER ?
			info->fake_master_ibat_override_ma : info->fake_slave_ibat_override_ma);
		if (magnitude > info->ocp_threshold[role])
			flag |= role == FG_IC_MASTER ? 2 : 1;
	}
	return flag;
}

static int mca_ibat_mon_get_ibat_ocp_status(struct mca_ibat_ocp_mon_dev *info)
{
	int ret, ffc_sts;
	int usb_online = 0, wireless_online = 0;
	bool loadsw_present;

	ret = platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER, &usb_online);
	if (ret)
		return ret;
	if (!usb_online && info->has_wireless) {
		ret = platform_class_wireless_is_present(WIRELESS_ROLE_MASTER, &wireless_online);
		if (ret)
			return ret;
	}
	if (!usb_online && !wireless_online)
		return 0;
	ret = platform_class_loadsw_get_present(LOADSW_ROLE_MASTER, &loadsw_present);
	if (ret)
		return ret;
	if (!loadsw_present)
		mca_charge_mievent_report(CHARGE_DFX_LOAD_SWITCH_I2C_ERR, NULL, 0);
	ffc_sts = strategy_class_fg_get_fastcharge();
	if (ffc_sts < 0)
		return ffc_sts;
	if (!ffc_sts)
		return 0;
	return mca_ibat_mon_get_ibat_ocp_flag(info);
}

static void mca_ibat_ocp_monitor_workfunc(struct work_struct *work)
{
	struct mca_ibat_ocp_mon_dev *info = container_of(
		work, struct mca_ibat_ocp_mon_dev, monitor_ibat_ocp_work.work);
	int status, interval = NORMAL_HEARTBEAT_TIMER_MS;

	mutex_lock(&info->lock);
	if (info->stopping)
		goto out;
	status = mca_ibat_mon_get_ibat_ocp_status(info);
	if (status < 0) {
		mca_log_err("IBAT observation failed: %d\n", status);
		status = info->batt_ocp_status;
		interval = FAST_HEARTBEAT_TIMER_MS;
	}
	info->fake_ibat_ocp_status = info->fake_ibat_for_debug > 13000000;
	/* Debug can assert protection, never hide a measured master/slave fault. */
	status |= info->fake_ibat_ocp_status;
	if (info->batt_ocp_status != status) {
		info->batt_ocp_status = status;
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
			MCA_EVENT_IBAT_OCP_CHANGE, &info->batt_ocp_status);
	}
	if (status)
		interval = FAST_HEARTBEAT_TIMER_MS;
	schedule_delayed_work(&info->monitor_ibat_ocp_work, msecs_to_jiffies(interval));
out:
	mutex_unlock(&info->lock);
}

static int mca_ibat_ocp_mon_parse_dt(struct mca_ibat_ocp_mon_dev *info)
{
	struct device_node *np = info->dev->of_node, *charger;
	u32 fg_type = MCA_FG_TYPE_SINGLE, thresholds[2], support_wls;
	int ret, i;

	if (!np)
		return -EINVAL;
	if (of_find_property(np, "fg_type", NULL)) {
		ret = of_property_read_u32(np, "fg_type", &fg_type);
		if (ret)
			return ret;
	}
	/* The pinned monitor only implements two parallel fuel gauges. */
	if (fg_type != MCA_FG_TYPE_PARALLEL)
		return -EOPNOTSUPP;
	info->fg_type = fg_type;
	ret = of_property_read_u32_array(np, "ocp_threshold", thresholds, 2);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++) {
		if (!thresholds[i] || thresholds[i] > INT_MAX)
			return -ERANGE;
		info->ocp_threshold[i] = thresholds[i];
	}
	charger = of_find_compatible_node(NULL, NULL, "mca,business_charger");
	if (!charger)
		return -ENODEV;
	ret = of_property_read_u32(charger, "support-wls", &support_wls);
	if (!ret && support_wls > 1)
		ret = -EINVAL;
	info->has_wireless = !ret && of_device_is_available(charger) && support_wls;
	of_node_put(charger);
	return ret;
}

static ssize_t ibat_ocp_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t ibat_ocp_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

struct mca_sysfs_attr_info ibat_ocp_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(ibat_ocp_sysfs, 0664, FAKE_IBAT_FOR_DEBUG,
			  fake_ibat_for_debug),
	mca_sysfs_attr_rw(ibat_ocp_sysfs, 0664, FAKE_MASTER_IBAT_OVERRIDE,
			  fake_master_ibat_override),
	mca_sysfs_attr_rw(ibat_ocp_sysfs, 0664, FAKE_SLAVE_IBAT_OVERRIDE,
			  fake_slave_ibat_override),
};

#define IBAT_OVP_SYSFS_ATTRS_SIZE ARRAY_SIZE(ibat_ocp_sysfs_field_tbl)

static struct attribute *ibat_ocp_sysfs_attrs[IBAT_OVP_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group ibat_ocp_sysfs_attr_group = {
	.attrs = ibat_ocp_sysfs_attrs,
};

static ssize_t ibat_ocp_sysfs_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mca_sysfs_attr_info *field = mca_sysfs_lookup_attr(attr->attr.name,
		ibat_ocp_sysfs_field_tbl, IBAT_OVP_SYSFS_ATTRS_SIZE);
	struct mca_ibat_ocp_mon_dev *info = dev_get_drvdata(dev);
	ssize_t ret;
	int value;

	if (!field)
		return -EINVAL;
	if (!info)
		return -ENODEV;
	mutex_lock(&info->lock);
	if (info->stopping) {
		ret = -ENODEV;
		goto out;
	}
	switch (field->sysfs_attr_name) {
	case FAKE_IBAT_FOR_DEBUG: value = info->fake_ibat_for_debug; break;
	case FAKE_MASTER_IBAT_OVERRIDE: value = info->fake_master_ibat_override_ma; break;
	case FAKE_SLAVE_IBAT_OVERRIDE: value = info->fake_slave_ibat_override_ma; break;
	default: ret = -EINVAL; goto out;
	}
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", value);
out:
	mutex_unlock(&info->lock);
	return ret;
}

static ssize_t ibat_ocp_sysfs_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *field = mca_sysfs_lookup_attr(attr->attr.name,
		ibat_ocp_sysfs_field_tbl, IBAT_OVP_SYSFS_ATTRS_SIZE);
	struct mca_ibat_ocp_mon_dev *info = dev_get_drvdata(dev);
	int value, ret;

	if (!field)
		return -EINVAL;
	if (!info)
		return -ENODEV;
	ret = kstrtoint(buf, 10, &value);
	if (ret)
		return ret;
	if (value < 0 && value != -22)
		return -EINVAL;
	mutex_lock(&info->lock);
	if (info->stopping) {
		ret = -ENODEV;
		goto out;
	}
	switch (field->sysfs_attr_name) {
	case FAKE_IBAT_FOR_DEBUG: info->fake_ibat_for_debug = value; break;
	case FAKE_MASTER_IBAT_OVERRIDE: info->fake_master_ibat_override_ma = value; break;
	case FAKE_SLAVE_IBAT_OVERRIDE: info->fake_slave_ibat_override_ma = value; break;
	default: ret = -EINVAL; goto out;
	}
	mod_delayed_work(system_wq, &info->monitor_ibat_ocp_work, 0);
	ret = count;
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int ibat_ocp_mon_sysfs_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(ibat_ocp_sysfs_attrs, ibat_ocp_sysfs_field_tbl,
			     IBAT_OVP_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group(SYSFS_DEV_5, "ibat_ocp", dev,
					   &ibat_ocp_sysfs_attr_group);
}

static int mca_ibat_ocp_mon_probe(struct platform_device *pdev)
{
	struct mca_ibat_ocp_mon_dev *info;
	int ret;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->dev = &pdev->dev;
	mutex_init(&info->lock);
	info->stopping = true;
	platform_set_drvdata(pdev, info);
	ret = mca_ibat_ocp_mon_parse_dt(info);
	if (ret)
		return dev_err_probe(info->dev, ret, "Invalid IBAT OCP configuration\n");
	info->fake_ibat_for_debug = -22;
	info->fake_master_ibat_override_ma = -22;
	info->fake_slave_ibat_override_ma = -22;
	INIT_DELAYED_WORK(&info->monitor_ibat_ocp_work, mca_ibat_ocp_monitor_workfunc);
	ret = ibat_ocp_mon_sysfs_create_group(info->dev);
	if (ret)
		return ret;
	mutex_lock(&info->lock);
	info->sysfs_created = true;
	info->stopping = false;
	schedule_delayed_work(&info->monitor_ibat_ocp_work,
		msecs_to_jiffies(NORMAL_HEARTBEAT_TIMER_MS));
	mutex_unlock(&info->lock);
	return 0;
}

static void mca_ibat_ocp_mon_stop(struct platform_device *pdev)
{
	struct mca_ibat_ocp_mon_dev *info = platform_get_drvdata(pdev);
	bool remove_sysfs;

	if (!info)
		return;
	mutex_lock(&info->lock);
	info->stopping = true;
	remove_sysfs = info->sysfs_created;
	info->sysfs_created = false;
	mutex_unlock(&info->lock);
	if (remove_sysfs)
		mca_sysfs_remove_link_group(SYSFS_DEV_5, "ibat_ocp", info->dev,
			&ibat_ocp_sysfs_attr_group);
	cancel_delayed_work_sync(&info->monitor_ibat_ocp_work);
}

static void mca_ibat_ocp_mon_remove(struct platform_device *pdev)
{
	mca_ibat_ocp_mon_stop(pdev);
}

static void mca_ibat_ocp_mon_shutdown(struct platform_device *pdev)
{
	mca_ibat_ocp_mon_stop(pdev);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,ibat_ocp_monitor" },
	{},
};

MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_ibat_ocp_mon_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "mca_ibat_ocp_monitor",
		.of_match_table = match_table,
	},
	.probe = mca_ibat_ocp_mon_probe,
	.remove = mca_ibat_ocp_mon_remove,
	.shutdown = mca_ibat_ocp_mon_shutdown,
};

static int __init mca_ibat_ocp_mon_init(void)
{
	return platform_driver_register(&mca_ibat_ocp_mon_driver);
}
module_init(mca_ibat_ocp_mon_init);

static void __exit mca_ibat_ocp_mon_exit(void)
{
	platform_driver_unregister(&mca_ibat_ocp_mon_driver);
}
module_exit(mca_ibat_ocp_mon_exit);

MODULE_DESCRIPTION("battery ocp software protection");
MODULE_AUTHOR("yinshunan@xiaomi.com");
MODULE_LICENSE("GPL v2");
