// SPDX-License-Identifier: GPL-2.0
/*
 * mca_business_votable.c
 *
 * mca business voter driver
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
#include <linux/slab.h>
#include <linux/pm_wakeup.h>
#include "inc/mca_business_misc.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_voter.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_business_voter"
#endif

struct business_votable_info {
	struct device *dev;
	struct mca_votable *awake_votable;
	struct mca_votable *smart_batt_votable;
};

static int business_awake_vote_callback(struct mca_votable *votable, void *data,
					int awake, const char *client)
{
	struct business_votable_info *vote_info = data;

	if (awake)
		pm_stay_awake(vote_info->dev);
	else
		pm_relax(vote_info->dev);

	return 0;
}

static int startegy_chg_smart_batt_vote_callback(struct mca_votable *votable,
						 void *data, int value,
						 const char *client)
{
	/* Native callback has no smart-battery hardware operation. */
	return -EOPNOTSUPP;
}

static void business_destroy_votables(void *data)
{
	struct business_votable_info *vote_info = data;

	/* The named-vote SRCU grace period drains callbacks before devm data. */
	mca_destroy_votable(vote_info->smart_batt_votable);
	vote_info->smart_batt_votable = NULL;
	mca_destroy_votable(vote_info->awake_votable);
	vote_info->awake_votable = NULL;
	pm_relax(vote_info->dev);
	device_init_wakeup(vote_info->dev, false);
}

static int business_create_votable(struct business_votable_info *vote_info)
{
	int ret;

	vote_info->awake_votable = mca_create_votable("AWAKE", MCA_VOTE_OR,
		business_awake_vote_callback, 0, vote_info);
	if (IS_ERR(vote_info->awake_votable)) {
		ret = PTR_ERR(vote_info->awake_votable);
		vote_info->awake_votable = NULL;
		return ret;
	}
	vote_info->smart_batt_votable = mca_create_votable("SMART_BATT", MCA_VOTE_MIN,
		startegy_chg_smart_batt_vote_callback, 0, vote_info);
	if (IS_ERR(vote_info->smart_batt_votable)) {
		ret = PTR_ERR(vote_info->smart_batt_votable);
		vote_info->smart_batt_votable = NULL;
		return ret;
	}
	return 0;
}

int business_votable_init(struct business_misc *misc)
{
	struct business_votable_info *vote_info;
	int ret;

	vote_info = devm_kzalloc(misc->dev, sizeof(*vote_info), GFP_KERNEL);
	if (!vote_info)
		return -ENOMEM;
	vote_info->dev = misc->dev;
	ret = device_init_wakeup(misc->dev, true);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(misc->dev, business_destroy_votables, vote_info);
	if (ret)
		return ret;
	misc->vote_info = vote_info;
	ret = business_create_votable(vote_info);
	if (ret)
		business_votable_exit(misc);
	return ret;
}

void business_votable_exit(struct business_misc *misc)
{
	if (!misc || !misc->vote_info)
		return;
	devm_release_action(misc->dev, business_destroy_votables, misc->vote_info);
	misc->vote_info = NULL;
}

MODULE_DESCRIPTION("business votable");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
