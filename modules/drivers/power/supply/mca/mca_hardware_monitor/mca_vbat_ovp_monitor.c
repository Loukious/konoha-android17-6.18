// SPDX-License-Identifier: GPL-2.0
/*
 * mca_vbat_ovp_monitor.c
 *
 * battery ovp software detection and protection driver
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
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <mca/common/mca_parse_dts.h>
#include "inc/mca_vbat_ovp_monitor.h"
#include <mca/common/mca_sysfs.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include "hwid.h"

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_vbat_ovp_mon"
#endif

#define FAST_HEARTBEAT_TIMER_MS 10000
#define NORMAL_HEARTBEAT_TIMER_MS 60000

/* Called with info->lock held. A failed observation cannot clear a fault. */
static int mca_vbat_mon_get_vbat_ovp_status(struct mca_vbat_ovp_mon_dev *info)
{
	int ret, vbat_now_mv, temp, ffc_sts, threshold;
	int usb_online = 0, wireless_online = 0;

	ret = strategy_class_fg_ops_get_temperature(&temp);
	if (ret)
		return ret;
	if (temp >= 480)
		return info->triggered;
	ret = strategy_class_fg_ops_get_voltage(&vbat_now_mv);
	if (ret)
		return ret;
	if (vbat_now_mv <= 0)
		return -ENODATA;
	ret = platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER,
						    &usb_online);
	if (ret)
		return ret;
	if (!usb_online && info->has_wireless) {
		ret = platform_class_wireless_is_present(WIRELESS_ROLE_MASTER,
						  &wireless_online);
		if (ret)
			return ret;
	}
	if (!usb_online && !wireless_online) {
		info->triggered = false;
		return 0;
	}
	ffc_sts = strategy_class_fg_get_fastcharge();
	if (ffc_sts < 0)
		return ffc_sts;
	threshold = ffc_sts ? info->vbat_ovp_thr_ffr_mv : info->vbat_ovp_thr_nor_mv;
	if (info->fake_vbat_override_mv > 0)
		vbat_now_mv = max(vbat_now_mv, info->fake_vbat_override_mv);
	if (!info->triggered && vbat_now_mv > threshold + info->vbat_ovp_thr_hys_mv)
		info->triggered = true;
	else if (info->triggered && vbat_now_mv <= threshold - info->vbat_ovp_recharge_delta_mv)
		info->triggered = false;
	return info->triggered;
}

static void mca_vbat_ovp_monitor_workfunc(struct work_struct *work)
{
	struct mca_vbat_ovp_mon_dev *info = container_of(
		work, struct mca_vbat_ovp_mon_dev, monitor_vbat_ovp_work.work);
	int status, interval, notify_status;
	bool notify = false;

	mutex_lock(&info->lock);
	if (info->stopping) {
		mutex_unlock(&info->lock);
		return;
	}
	status = mca_vbat_mon_get_vbat_ovp_status(info);
	if (status >= 0) {
		/* A debug trigger may assert protection, but cannot mask a real fault. */
		info->fake_vbat_ovp_status = info->fake_vbat_for_debug > 4600;
		status |= info->fake_vbat_ovp_status;
		if (info->batt_ovp_status != status) {
			info->batt_ovp_status = status;
			notify_status = status;
			notify = true;
		}
	}
	interval = status < 0 || info->batt_ovp_status ?
		FAST_HEARTBEAT_TIMER_MS : NORMAL_HEARTBEAT_TIMER_MS;
	mutex_unlock(&info->lock);
	if (notify)
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_VBAT_OVP_CHANGE, &notify_status);
	mutex_lock(&info->lock);
	if (!info->stopping)
		schedule_delayed_work(&info->monitor_vbat_ovp_work,
				      msecs_to_jiffies(interval));
	mutex_unlock(&info->lock);
}

/* Optional native settings use defaults; malformed values still fail probe. */
static int vbat_ovp_read_u32(struct device_node *np, const char *name,
			   int *value, u32 fallback)
{
	u32 parsed = fallback;
	int ret;

	if (of_find_property(np, name, NULL)) {
		ret = of_property_read_u32(np, name, &parsed);
		if (ret)
			return ret;
	}
	if (parsed > 20000)
		return -ERANGE;
	*value = parsed;
	return 0;
}

static int mca_vbat_ovp_mon_parse_dt(struct mca_vbat_ovp_mon_dev *info)
{
	struct device_node *np = info->dev->of_node;
	int ret = 0;
	const struct mca_hwid *hwid = mca_get_hwid_info();
	struct device_node *charger;
	u32 support_wls = 0;

	if (!np) {
		mca_log_err("device tree info missing\n");
		return -EINVAL;
	}

	info->support_global_fv =
		of_property_read_bool(np, "support_global_fv");
	{
		struct {
			const char *name;
			int *value;
			u32 fallback;
		} fields[] = {
			{ "vbat_ovp_threshold_ffc", &info->vbat_ovp_threshold_ffc_mv, VBAT_OVP_THR_FFC_DEFAULT_MV },
			{ "vbat_ovp_threshold_normal", &info->vbat_ovp_threshold_normal_mv, VBAT_OVP_THR_NORMAL_DEFAULT_UV },
			{ "vbat_ovp_threshold_ffc_gl", &info->vbat_ovp_threshold_ffc_gl_mv, VBAT_OVP_THR_FFC_GL_DEFAULT_MV },
			{ "vbat_ovp_threshold_normal_gl", &info->vbat_ovp_threshold_normal_gl_mv, VBAT_OVP_THR_NORMAL_GL_DEFAULT_UV },
			{ "vbat_ovp_threshold_hys", &info->vbat_ovp_thr_hys_mv, VBAT_OVP_THR_DEFAULT_HYS_MV },
			{ "vbat_ovp_recharge_delta", &info->vbat_ovp_recharge_delta_mv, VBAT_OVP_RECHARGE_DELTA_MV },
			{ "fg_type", &info->fg_type, MCA_FG_TYPE_SINGLE },
		};
		int i;

		for (i = 0; i < ARRAY_SIZE(fields); i++) {
			ret = vbat_ovp_read_u32(np, fields[i].name,
					      fields[i].value, fields[i].fallback);
			if (ret)
				return ret;
		}
	}
	if (info->fg_type != MCA_FG_TYPE_SINGLE)
		return -EOPNOTSUPP;
	if (info->support_global_fv && !hwid)
		return -EPROBE_DEFER;
	charger = of_find_compatible_node(NULL, NULL, "mca,business_charger");
	if (!charger)
		return -ENODEV;
	ret = of_property_read_u32(charger, "support-wls", &support_wls);
	info->has_wireless = of_device_is_available(charger) && support_wls;
	of_node_put(charger);
	if (ret)
		return ret;

	if (hwid && info->support_global_fv &&
	    hwid->country_version != CountryCN) {
		info->vbat_ovp_thr_ffr_mv = info->vbat_ovp_threshold_ffc_gl_mv;
		info->vbat_ovp_thr_nor_mv =
			info->vbat_ovp_threshold_normal_gl_mv;
	} else {
		info->vbat_ovp_thr_ffr_mv = info->vbat_ovp_threshold_ffc_mv;
		info->vbat_ovp_thr_nor_mv = info->vbat_ovp_threshold_normal_mv;
	}

	if (!info->vbat_ovp_thr_ffr_mv || !info->vbat_ovp_thr_nor_mv ||
	    info->vbat_ovp_thr_hys_mv >= info->vbat_ovp_thr_nor_mv ||
	    info->vbat_ovp_thr_hys_mv >= info->vbat_ovp_thr_ffr_mv ||
	    info->vbat_ovp_recharge_delta_mv >= info->vbat_ovp_thr_nor_mv ||
	    info->vbat_ovp_recharge_delta_mv >= info->vbat_ovp_thr_ffr_mv)
		return -EINVAL;
	return 0;
}

static ssize_t vbat_ovp_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t vbat_ovp_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

struct mca_sysfs_attr_info vbat_ovp_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(vbat_ovp_sysfs, 0664, FAKE_VBAT_FOR_DEBUG,
			  fake_vbat_for_debug),
	mca_sysfs_attr_rw(vbat_ovp_sysfs, 0664, FAKE_VBAT_OVERRIDE,
			  fake_vbat_override),
};

#define VBAT_OVP_SYSFS_ATTRS_SIZE ARRAY_SIZE(vbat_ovp_sysfs_field_tbl)

static struct attribute *vbat_ovp_sysfs_attrs[VBAT_OVP_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group vbat_ovp_sysfs_attr_group = {
	.attrs = vbat_ovp_sysfs_attrs,
};

static ssize_t vbat_ovp_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mca_sysfs_attr_info *attr_info;
	ssize_t count = 0;
	struct mca_vbat_ovp_mon_dev *info = dev_get_drvdata(dev);
	int vbat, vbat_override;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  vbat_ovp_sysfs_field_tbl,
					  VBAT_OVP_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -EINVAL;

	if (!info) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -EINVAL;
	}
	mutex_lock(&info->lock);
	if (info->stopping) {
		mutex_unlock(&info->lock);
		return -ENODEV;
	}
	switch (attr_info->sysfs_attr_name) {
	case FAKE_VBAT_FOR_DEBUG:
		vbat = info->fake_vbat_for_debug;
		count = scnprintf(buf, PAGE_SIZE, "%d\n", vbat);
		break;
	case FAKE_VBAT_OVERRIDE:
		vbat_override = info->fake_vbat_override_mv;
		count = scnprintf(buf, PAGE_SIZE, "%d\n", vbat_override);
		break;
	default:
		break;
	}
	mutex_unlock(&info->lock);
	return count;
}

static ssize_t vbat_ovp_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *attr_info;
	struct mca_vbat_ovp_mon_dev *info = dev_get_drvdata(dev);
	int val;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  vbat_ovp_sysfs_field_tbl,
					  VBAT_OVP_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -EINVAL;

	if (!info) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -EINVAL;
	}

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;
	if (val > 20000)
		return -ERANGE;

	mutex_lock(&info->lock);
	if (info->stopping) {
		mutex_unlock(&info->lock);
		return -ENODEV;
	}
	switch (attr_info->sysfs_attr_name) {
	case FAKE_VBAT_FOR_DEBUG:
		info->fake_vbat_for_debug = val;
		mca_log_err("set the %d fake vbat for debug = %d\n",
			    attr_info->sysfs_attr_name, val);

		mod_delayed_work(system_wq, &info->monitor_vbat_ovp_work, 0);
		break;
	case FAKE_VBAT_OVERRIDE:
		info->fake_vbat_override_mv = val;
		mca_log_err("set the %d fake vbat override = %d\n",
			    attr_info->sysfs_attr_name, val);

		mod_delayed_work(system_wq, &info->monitor_vbat_ovp_work, 0);
		break;
	default:
		break;
	}

	mutex_unlock(&info->lock);
	return count;
}

static int vbat_ovp_mon_sysfs_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(vbat_ovp_sysfs_attrs, vbat_ovp_sysfs_field_tbl,
			     VBAT_OVP_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group(SYSFS_DEV_5, "vbat_ovp", dev,
					   &vbat_ovp_sysfs_attr_group);
}

static int mca_vbat_ovp_mon_probe(struct platform_device *pdev)
{
	struct mca_vbat_ovp_mon_dev *info;
	int ret;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = &pdev->dev;
	mutex_init(&info->lock);
	info->stopping = true;
	platform_set_drvdata(pdev, info);
	ret = mca_vbat_ovp_mon_parse_dt(info);
	if (ret) {
		mca_log_err("parse dt failed: %d\n", ret);
		return ret;
	}

	info->batt_ovp_status = 0;
	info->fake_vbat_ovp_status = 0;
	info->fake_vbat_for_debug = -22;
	info->fake_vbat_override_mv = -22;
	INIT_DELAYED_WORK(&info->monitor_vbat_ovp_work,
			  mca_vbat_ovp_monitor_workfunc);
	ret = vbat_ovp_mon_sysfs_create_group(info->dev);
	if (ret)
		return dev_err_probe(info->dev, ret == -ENODEV ? -EPROBE_DEFER : ret,
				     "VBAT sysfs registration failed\n");
	mutex_lock(&info->lock);
	info->sysfs_created = true;
	info->stopping = false;
	schedule_delayed_work(&info->monitor_vbat_ovp_work,
			      msecs_to_jiffies(NORMAL_HEARTBEAT_TIMER_MS));
	mutex_unlock(&info->lock);
	mca_log_info("%s success\n", __func__);
	return 0;
}

static void mca_vbat_ovp_mon_stop(struct platform_device *pdev)
{
	struct mca_vbat_ovp_mon_dev *info = platform_get_drvdata(pdev);
	bool remove_sysfs;

	mutex_lock(&info->lock);
	info->stopping = true;
	remove_sysfs = info->sysfs_created;
	info->sysfs_created = false;
	mutex_unlock(&info->lock);
	if (remove_sysfs)
		mca_sysfs_remove_link_group(SYSFS_DEV_5, "vbat_ovp", info->dev,
					    &vbat_ovp_sysfs_attr_group);
	cancel_delayed_work_sync(&info->monitor_vbat_ovp_work);
}

static void mca_vbat_ovp_mon_remove(struct platform_device *pdev)
{
	mca_vbat_ovp_mon_stop(pdev);
}

static void mca_vbat_ovp_mon_shutdown(struct platform_device *pdev)
{
	mca_vbat_ovp_mon_stop(pdev);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,vbat_ovp_monitor" },
	{},
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_vbat_ovp_mon_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "mca_vbat_ovp_monitor",
		.of_match_table = match_table,
	},
	.probe = mca_vbat_ovp_mon_probe,
	.remove = mca_vbat_ovp_mon_remove,
	.shutdown = mca_vbat_ovp_mon_shutdown,
};

static int __init mca_vbat_ovp_mon_init(void)
{
	return platform_driver_register(&mca_vbat_ovp_mon_driver);
}
module_init(mca_vbat_ovp_mon_init);

static void __exit mca_vbat_ovp_mon_exit(void)
{
	platform_driver_unregister(&mca_vbat_ovp_mon_driver);
}
module_exit(mca_vbat_ovp_mon_exit);

MODULE_DESCRIPTION("battery ovp software protection");
MODULE_AUTHOR("jiangfei1@xiaomi.com");
MODULE_LICENSE("GPL v2");
