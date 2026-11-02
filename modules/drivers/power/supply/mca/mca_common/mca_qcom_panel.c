// SPDX-License-Identifier: GPL-2.0
/*
 * mca_qcom_panel.c
 *
 * MCA panel event bridge for the Qualcomm panel event notifier.
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
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <drm/drm_panel.h>

#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_panel.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_panel"
#endif

struct mca_panel_dev {
	struct device *dev;
#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
	struct delayed_work panel_notify_register_work;
	void *notifier_cookie;
	unsigned int retries;
#endif

	int screen_state;
	int hbm_state;
};

static struct mca_panel_dev *g_panel;
static DEFINE_MUTEX(panel_state_lock);

int mca_panel_get_screen_state(void)
{
	guard(mutex)(&panel_state_lock);

	if (!g_panel)
		return 0;
	return g_panel->screen_state;
}
EXPORT_SYMBOL(mca_panel_get_screen_state);

int mca_panel_get_hbm_state(void)
{
	guard(mutex)(&panel_state_lock);

	if (!g_panel)
		return 0;
	return g_panel->hbm_state;
}
EXPORT_SYMBOL(mca_panel_get_hbm_state);

#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
static void
mca_panel_event_notifier_callback(enum panel_event_notifier_tag tag,
				  struct panel_event_notification *notification,
				  void *data)
{
	struct mca_panel_dev *panel_dev = data;
	unsigned int event;
	int value;

	if (!notification)
		return;
	mutex_lock(&panel_state_lock);
	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
	case DRM_PANEL_EVENT_UNBLANK:
		value = notification->notif_type == DRM_PANEL_EVENT_UNBLANK;
		panel_dev->screen_state = value;
		event = MCA_EVENT_PANEL_SCREEN_STATE_CHANGE;
		break;
	case DRM_PANEL_EVENT_HBM_ON:
	case DRM_PANEL_EVENT_HBM_OFF:
		value = notification->notif_type == DRM_PANEL_EVENT_HBM_ON;
		panel_dev->hbm_state = value;
		event = MCA_EVENT_PANEL_HBM_STATE_CHANGE;
		break;
	default:
		mutex_unlock(&panel_state_lock);
		return;
	}
	mutex_unlock(&panel_state_lock);
	/* Listener callbacks may query this bridge again. */
	mca_event_block_notify(MCA_EVENT_TYPE_PANEL, event, &value);
}

static int mca_panel_register_panel_notifier(struct mca_panel_dev *panel_dev)
{
	struct device_node *node, *pnode;
	struct drm_panel *panel;
	void *cookie;
	int i, count, ret = -ENODEV;

	node = of_find_node_by_name(NULL, "charge-screen");
	if (!node)
		return -ENODEV;
	count = of_count_phandle_with_args(node, "panel", NULL);
	for (i = 0; i < count; i++) {
		pnode = of_parse_phandle(node, "panel", i);
		if (!pnode)
			continue;
		panel = of_drm_find_panel(pnode);
		of_node_put(pnode);
		if (IS_ERR_OR_NULL(panel)) {
			ret = panel ? PTR_ERR(panel) : -ENODEV;
			continue;
		}
		cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_BATTERY_CHARGER, panel,
			mca_panel_event_notifier_callback, panel_dev);
		if (IS_ERR_OR_NULL(cookie)) {
			ret = cookie ? PTR_ERR(cookie) : -EINVAL;
			break;
		}
		panel_dev->notifier_cookie = cookie;
		ret = 0;
		break;
	}
	of_node_put(node);
	return ret;
}

static void mca_panel_register_panel_notifier_work(struct work_struct *work)
{
	struct mca_panel_dev *panel_dev =
		container_of(to_delayed_work(work), struct mca_panel_dev,
			     panel_notify_register_work);

	if (mca_panel_register_panel_notifier(panel_dev) &&
	    panel_dev->retries++ < 3)
		schedule_delayed_work(&panel_dev->panel_notify_register_work,
				      msecs_to_jiffies(5000));
}
#endif

static int mca_panel_probe(struct platform_device *pdev)
{
	struct mca_panel_dev *panel_dev;
	static int probe_cnt;

	mca_log_info("probe_cnt = %d\n", ++probe_cnt);

	panel_dev = devm_kzalloc(&pdev->dev, sizeof(*panel_dev), GFP_KERNEL);
	if (!panel_dev) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	panel_dev->dev = &pdev->dev;
	panel_dev->screen_state = 0;
	panel_dev->hbm_state = 0;
	platform_set_drvdata(pdev, panel_dev);
	mutex_lock(&panel_state_lock);
	if (g_panel) {
		mutex_unlock(&panel_state_lock);
		return -EBUSY;
	}
	g_panel = panel_dev;
	mutex_unlock(&panel_state_lock);

#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
	INIT_DELAYED_WORK(&panel_dev->panel_notify_register_work,
			  mca_panel_register_panel_notifier_work);
	schedule_delayed_work(&panel_dev->panel_notify_register_work,
			      msecs_to_jiffies(5000));
#endif

	mca_log_err("probe OK");
	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,mca_panel" },
	{},
};

static void mca_panel_remove(struct platform_device *pdev)
{
	struct mca_panel_dev *panel_dev = platform_get_drvdata(pdev);

#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
	disable_delayed_work_sync(&panel_dev->panel_notify_register_work);
	if (panel_dev->notifier_cookie)
		panel_event_notifier_unregister(panel_dev->notifier_cookie);
#endif
	mutex_lock(&panel_state_lock);
	if (g_panel == panel_dev)
		g_panel = NULL;
	mutex_unlock(&panel_state_lock);
}

static void mca_panel_shutdown(struct platform_device *pdev)
{
}

static struct platform_driver mca_panel_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "mca_panel",
		.of_match_table = match_table,
	},
	.probe = mca_panel_probe,
	.remove = mca_panel_remove,
	.shutdown = mca_panel_shutdown,
};

static int __init mca_panel_init(void)
{
	return platform_driver_register(&mca_panel_driver);
}
module_init(mca_panel_init);

static void __exit mca_panel_exit(void)
{
	platform_driver_unregister(&mca_panel_driver);
}
module_exit(mca_panel_exit);

MODULE_DESCRIPTION("mca get panel event");
MODULE_AUTHOR("muxinyi1@xiaomi.com");
MODULE_LICENSE("GPL v2");
