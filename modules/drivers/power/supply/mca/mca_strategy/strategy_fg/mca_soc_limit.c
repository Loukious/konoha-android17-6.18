// SPDX-License-Identifier: GPL-2.0
/*
 * mca_soc_limit.c
 *
 * soc limit driver
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
#include <linux/srcu.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/strategy/strategy_soc_limit.h>
#include "inc/mca_soc_limit.h"

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_soc_limit"
#endif

static struct soc_limit_info __rcu *global_soc_limit_info;
static DEFINE_MUTEX(soc_limit_lock);
DEFINE_STATIC_SRCU(soc_limit_srcu);

void soc_limit_process(bool enable, int soc_limit_thre)
{
	guard(srcu)(&soc_limit_srcu);
	struct soc_limit_info *info =
		srcu_dereference(global_soc_limit_info, &soc_limit_srcu);
	bool limit = false;
	int soc;

	if (!info || (enable && (soc_limit_thre < 0 || soc_limit_thre > 100)))
		return;
	if (enable) {
		soc = strategy_class_fg_ops_get_soc();
		if (soc < 0 || soc > 100)
			return;
		limit = soc > soc_limit_thre;
	}
	/* A local event value remains valid across a reentrant listener. */
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS, MCA_EVENT_SOC_LIMIT,
			       &limit);
}
EXPORT_SYMBOL(soc_limit_process);

static int soc_limit_probe(struct platform_device *pdev)
{
	struct soc_limit_info *info;
	guard(mutex)(&soc_limit_lock);

	if (rcu_access_pointer(global_soc_limit_info))
		return -EBUSY;
	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->dev = &pdev->dev;
	platform_set_drvdata(pdev, info);
	rcu_assign_pointer(global_soc_limit_info, info);
	return 0;
}

static void soc_limit_remove(struct platform_device *pdev)
{
	guard(mutex)(&soc_limit_lock);

	rcu_assign_pointer(global_soc_limit_info, NULL);
	synchronize_srcu(&soc_limit_srcu);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "xiaomi,soc_limit" },
	{},
};

MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver soc_limit_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "soc_limit",
		.of_match_table = match_table,
	},
	.probe = soc_limit_probe,
	.remove = soc_limit_remove,
};

static int __init soc_limit_init(void)
{
	return platform_driver_register(&soc_limit_driver);
}
module_init(soc_limit_init);

static void __exit soc_limit_exit(void)
{
	platform_driver_unregister(&soc_limit_driver);
}
module_exit(soc_limit_exit);

MODULE_DESCRIPTION("soc limit driver");
MODULE_AUTHOR("liuzhengqing@xiaomi.com");
MODULE_LICENSE("GPL v2");
