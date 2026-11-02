// SPDX-License-Identifier: GPL-2.0
/*
 *mca_buckchg_jieta.c
 *
 * mca buck charger jeita control driver
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
#include <linux/stat.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_voter.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include "inc/mca_buckchg_jeita.h"
#include <mca/platform/platform_loadsw_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/common/mca_hwid.h>
#include "hwid.h"
#include <mca/protocol/protocol_class.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_buckchg_jeita"
#endif

#define MCA_DTPT_MOLECULE_SCALE 8
#define MCA_DTPT_DENOM_SCALE 10
#define MCA_BASE_OVER_CURR_COUNT_MAX 4

#define JEITA_COLD_ZONE_INDEX 1

static int mca_buckchg_jeita_get_curr(struct mca_buckchg_jeita_data *jeita_data,
				      int *vbat_index)
{
	int i;
	int ret;
	int vbat = 0;
	int min_cur = jeita_data->max_current;

	*vbat_index = -1;

	ret = strategy_class_fg_ops_get_voltage(&vbat);
	if (ret) {
		mca_log_err("get vbat failed\n");
		return 0;
	}

	if (!jeita_data->volt_para.size)
		return min_cur;

	for (i = 0; i < jeita_data->volt_para.size; i++) {
		min_cur = min(min_cur,
			      jeita_data->volt_para.volt_data[i].max_current);
		if (vbat <= jeita_data->volt_para.volt_data[i].voltage) {
			*vbat_index = i;
			return jeita_data->volt_para.volt_data[i].max_current;
		}
	}

	/* can not find para, use min current */
	return min_cur;
}

#define ABNORMAL_BATT_FV_MAX 4200
#define JEITA_HOT_RECHARGE_VBAT_HYS 150

static void mca_buckchg_base_jeita_update(struct mca_buckchg_jeita_dev *info)
{
	int temp = 0;
	int i, ret;
	struct mca_buckchg_jeita_data *jeita_data, *cur_jeita_data;
	int fastcharge_mode = 0;
	int now_curr = 0, chg_curr = 0, effective_curr;
	int hys_affect = 0;
	int data_change = 0;
	int vbat_index = -1;
	int vbat = 0;

	if (!info->base_jeita_para.jeita_data || !info->voter_ok) {
		mca_log_err("jeita data not ready\n");
		return;
	}
	effective_curr = mca_get_named_effective_result("buck_charge_curr");

	ret = platform_fg_ops_get_temp(FG_IC_MASTER, &temp);
	ret |= platform_fg_ops_get_curr(FG_IC_MASTER, &now_curr);
	if (ret) {
		mca_log_err("get battery temp or currfailed\n");
		return;
	}

	temp /= 10;
	fastcharge_mode = strategy_class_fg_get_fastcharge();
	if (fastcharge_mode < 0) {
		mca_vote_named("chg_enable", "jeita", true, 0);
		return;
	}
	mca_log_err("fastcharge_mode is %d\n", fastcharge_mode);
	platform_fg_ops_get_volt(FG_IC_MASTER, &vbat);
	mca_log_info("vbat is %d\n", vbat);

	if (!fastcharge_mode) {
		for (i = 0; i < info->base_jeita_para.size; i++) {
			cur_jeita_data = info->base_jeita_para.jeita_data + i;
			if (temp < cur_jeita_data->temp_high &&
			    temp >= cur_jeita_data->temp_low)
				break;
		}
		if (i == info->base_jeita_para.size) {
			mca_log_err("can not find flip jeita para\n");
			return;
		}
	} else {
		for (i = 0; i < info->base_jeita_para.fcc_size; i++) {
			cur_jeita_data =
				info->base_jeita_para.jeita_ffc_data + i;
			if (temp < cur_jeita_data->temp_high &&
			    temp >= cur_jeita_data->temp_low)
				break;
		}
		if (i == info->base_jeita_para.fcc_size) {
			mca_log_err("can not find flip jeita fcc para\n");
			return;
		}
	}

	/* select cur jeita_data (temperature hysteresis) */
	if (info->base_proc_data.cur_jeita_index == -1) {
		jeita_data = cur_jeita_data;
		info->last_chg_curr[1] =
			mca_buckchg_jeita_get_curr(cur_jeita_data, &vbat_index);
	} else {
		if (info->base_proc_data.cur_jeita_index == i ||
		    fastcharge_mode != info->last_fastcharge_mode[1])
			jeita_data = cur_jeita_data;
		else {
			if (!fastcharge_mode ||
			    info->base_jeita_para.fcc_size == 0)
				jeita_data = info->base_jeita_para.jeita_data +
					     info->base_proc_data.cur_jeita_index;
			else
				jeita_data =
					info->base_jeita_para.jeita_ffc_data +
					info->base_proc_data.cur_jeita_index;

			mca_log_info("temp_low: %d, temp_high: %d, temp: %d\n",
				     jeita_data->temp_low -
					     jeita_data->low_temp_hys,
				     jeita_data->temp_high +
					     jeita_data->high_temp_hys,
				     temp);
			if (i > info->base_proc_data.cur_jeita_index) {
				if (info->base_proc_data.cur_jeita_index == 0 &&
				    mca_smartchg_is_extreme_cold_enabled())
					jeita_data = cur_jeita_data;
				else if (temp < jeita_data->temp_high +
							 jeita_data->high_temp_hys)
					hys_affect = 1;
				else
					jeita_data = cur_jeita_data;
			} else {
				if (temp > jeita_data->temp_low -
						   jeita_data->low_temp_hys)
					hys_affect = 1;
				else
					jeita_data = cur_jeita_data;
			}
		}
	}
	info->last_fastcharge_mode[1] = fastcharge_mode;

	info->base_proc_data.max_chg_curr =
		mca_buckchg_jeita_get_curr(jeita_data, &vbat_index);
	if (info->base_proc_data.cur_jeita_index != i && !hys_affect) {
		data_change = 1;
		mca_log_info(
			"jeita_index: cur_jeita_index:%d, i:%d, hys_affect:%d\n",
			info->base_proc_data.cur_jeita_index, i, hys_affect);
		info->last_chg_curr[1] = info->base_proc_data.max_chg_curr;
	} else if (info->base_proc_data.max_chg_curr > info->last_chg_curr[1]) {
		if (vbat_index != -1 &&
		    vbat < jeita_data->volt_para.volt_data[vbat_index].voltage -
				   info->base_vbat_low_hyst) {
			data_change = 1;
			mca_log_info(
				"vbat_thre[l->s]: max_chg_curr:%d, info->last_chg_curr[1]:%d, vbat_index:%d, vbat:%d\n",
				info->base_proc_data.max_chg_curr, info->last_chg_curr[1],
				vbat_index, vbat);
			info->last_chg_curr[1] = info->base_proc_data.max_chg_curr;
		}
	} else if (info->base_proc_data.max_chg_curr < info->last_chg_curr[1]) {
		if (vbat_index > 0 &&
		    vbat > jeita_data->volt_para.volt_data[vbat_index - 1]
				   .voltage) {
			data_change = 1;
			mca_log_info(
				"vbat_thre[s->l]: max_chg_curr:%d, info->last_chg_curr[1]:%d, vbat_index:%d\n",
				info->base_proc_data.max_chg_curr, info->last_chg_curr[1],
				vbat_index);
			info->last_chg_curr[1] = info->base_proc_data.max_chg_curr;
		}
	}

	chg_curr = info->base_proc_data.max_chg_curr;
	mca_log_info(
		"cur index %d/%d max_chg_curr %d chg_curr %d hys_affect %d, now_curr %d, effective_curr %d\n",
		i, info->base_proc_data.cur_jeita_index,
		info->base_proc_data.max_chg_curr, chg_curr, hys_affect,
		now_curr, effective_curr);
	if (data_change && !hys_affect)
		info->base_proc_data.cur_jeita_index = i;

	if (effective_curr != 0 && now_curr / 1000 < -chg_curr)
		info->base_over_curr_count[1]++;
	else
		info->base_over_curr_count[1] = 0;

	if (info->base_over_curr_count[1] >= MCA_BASE_OVER_CURR_COUNT_MAX &&
	    effective_curr != 0) {
		effective_curr -= 100;
		info->base_over_curr_count[1] = 0;
		mca_vote_named("buck_charge_curr", "swocp", true, effective_curr);
		info->runswocp[1] = true;
		mca_log_err("reduce fcc %d by swocp\n", effective_curr);
	}
	if (now_curr / 1000 > -chg_curr + 500 && info->runswocp[1]) {
		info->runswocp[1] = false;
		mca_vote_named("buck_charge_curr", "swocp", false, 0);
		mca_log_err("disable swocp\n");
	}
}

static void mca_buckchg_flip_jeita_update(struct mca_buckchg_jeita_dev *info)
{
	int temp = 0;
	int i, ret;
	struct mca_buckchg_jeita_data *jeita_data, *cur_jeita_data;
	int fastcharge_mode = 0;
	int chg_curr = 0;
	int hys_affect = 0;
	int data_change = 0;
	int vbat_index = -1;
	int vbat = 0;

	if (!info->flip_jeita_para.jeita_data || !info->voter_ok) {
		mca_log_err("jeita data not ready\n");
		return;
	}

	ret = platform_fg_ops_get_temp(FG_IC_SLAVE, &temp);
	if (ret) {
		mca_log_err("get battery temp failed\n");
		return;
	}

	temp /= 10;
	fastcharge_mode = strategy_class_fg_get_fastcharge();
	if (fastcharge_mode < 0) {
		mca_vote_named("chg_enable", "jeita", true, 0);
		return;
	}
	mca_log_err("fastcharge_mode is %d\n", fastcharge_mode);
	platform_fg_ops_get_volt(FG_IC_SLAVE, &vbat);
	mca_log_info("vbat is %d\n", vbat);

	if (!fastcharge_mode) {
		for (i = 0; i < info->flip_jeita_para.size; i++) {
			cur_jeita_data = info->flip_jeita_para.jeita_data + i;
			if (temp < cur_jeita_data->temp_high &&
			    temp >= cur_jeita_data->temp_low)
				break;
		}
		if (i == info->flip_jeita_para.size) {
			mca_log_err("can not find flip jeita para\n");
			return;
		}
	} else {
		for (i = 0; i < info->flip_jeita_para.fcc_size; i++) {
			cur_jeita_data =
				info->flip_jeita_para.jeita_ffc_data + i;
			if (temp < cur_jeita_data->temp_high &&
			    temp >= cur_jeita_data->temp_low)
				break;
		}
		if (i == info->flip_jeita_para.fcc_size) {
			mca_log_err("can not find flip jeita fcc para\n");
			return;
		}
	}

	/* select cur jeita_data (temperature hysteresis) */
	if (info->flip_proc_data.cur_jeita_index == -1) {
		jeita_data = cur_jeita_data;
		info->last_chg_curr[2] =
			mca_buckchg_jeita_get_curr(cur_jeita_data, &vbat_index);
	} else {
		if (info->flip_proc_data.cur_jeita_index == i ||
		    fastcharge_mode != info->last_fastcharge_mode[2])
			jeita_data = cur_jeita_data;
		else {
			if (!fastcharge_mode ||
			    info->flip_jeita_para.fcc_size == 0)
				jeita_data = info->flip_jeita_para.jeita_data +
					     info->flip_proc_data.cur_jeita_index;
			else
				jeita_data =
					info->flip_jeita_para.jeita_ffc_data +
					info->flip_proc_data.cur_jeita_index;

			mca_log_info("temp_low: %d, temp_high: %d, temp: %d\n",
				     jeita_data->temp_low -
					     jeita_data->low_temp_hys,
				     jeita_data->temp_high +
					     jeita_data->high_temp_hys,
				     temp);
			if (i > info->flip_proc_data.cur_jeita_index) {
				if (info->flip_proc_data.cur_jeita_index == 0 &&
				    mca_smartchg_is_extreme_cold_enabled())
					jeita_data = cur_jeita_data;
				else if (temp < jeita_data->temp_high +
							 jeita_data->high_temp_hys)
					hys_affect = 1;
				else
					jeita_data = cur_jeita_data;
			} else {
				if (temp > jeita_data->temp_low -
						   jeita_data->low_temp_hys)
					hys_affect = 1;
				else
					jeita_data = cur_jeita_data;
			}
		}
	}
	info->last_fastcharge_mode[2] = fastcharge_mode;

	info->flip_proc_data.max_chg_curr =
		mca_buckchg_jeita_get_curr(jeita_data, &vbat_index);
	if (info->flip_proc_data.cur_jeita_index != i && !hys_affect) {
		data_change = 1;
		mca_log_info(
			"jeita_index: cur_jeita_index:%d, i:%d, hys_affect:%d\n",
			info->flip_proc_data.cur_jeita_index, i, hys_affect);
		info->last_chg_curr[2] = info->flip_proc_data.max_chg_curr;
	} else if (info->flip_proc_data.max_chg_curr > info->last_chg_curr[2]) {
		if (vbat_index != -1 &&
		    vbat < jeita_data->volt_para.volt_data[vbat_index].voltage -
				   info->flip_vbat_low_hyst) {
			data_change = 1;
			mca_log_info(
				"vbat_thre[l->s]: max_chg_curr:%d, info->last_chg_curr[2]:%d, vbat_index:%d, vbat:%d\n",
				info->flip_proc_data.max_chg_curr, info->last_chg_curr[2],
				vbat_index, vbat);
			info->last_chg_curr[2] = info->flip_proc_data.max_chg_curr;
		}
	} else if (info->flip_proc_data.max_chg_curr < info->last_chg_curr[2]) {
		if (vbat_index > 0 &&
		    vbat > jeita_data->volt_para.volt_data[vbat_index - 1]
				   .voltage) {
			data_change = 1;
			mca_log_info(
				"vbat_thre[s->l]: max_chg_curr:%d, info->last_chg_curr[2]:%d, vbat_index:%d\n",
				info->flip_proc_data.max_chg_curr, info->last_chg_curr[2],
				vbat_index);
			info->last_chg_curr[2] = info->flip_proc_data.max_chg_curr;
		}
	}

	chg_curr = info->flip_proc_data.max_chg_curr;
	mca_log_info("cur index %d/%d max_chg_curr %d chg_curr %d hys_affect %d\n",
		     i, info->flip_proc_data.cur_jeita_index,
		     info->flip_proc_data.max_chg_curr, chg_curr, hys_affect);

	if (data_change && !hys_affect) {
		info->flip_proc_data.cur_jeita_index = i;
		mca_vote(info->flip_fcc_voter, "jeita", true, chg_curr);
	}
}
static void mca_buckchg_jeita_update(struct mca_buckchg_jeita_dev *info)
{
	int temp = 0;
	int i, ret;
	struct mca_buckchg_jeita_data *jeita_data, *cur_jeita_data;
	int fastcharge_mode = 0;
	int vterm = 0, chg_curr = 0;
	int hys_affect = 0;
	int data_change = 0;
	int vbat = 0;
	int vbat_index = -1;

	if (!info->jeita_para.jeita_data || !info->voter_ok) {
		mca_log_err("jeita data not ready\n");
		return;
	}

	ret = strategy_class_fg_ops_get_temperature(&temp);
	if (ret) {
		mca_log_err("get battery temp failed\n");
		mca_vote_named("chg_enable", "jeita", true, 0);
		return;
	}

	temp /= 10;
	fastcharge_mode = strategy_class_fg_get_fastcharge();
	if (fastcharge_mode < 0) {
		mca_vote_named("chg_enable", "jeita", true, 0);
		return;
	}

	ret = strategy_class_fg_ops_get_voltage(&vbat);
	if (ret) {
		mca_log_err("get battery volt failed\n");
		mca_vote_named("chg_enable", "jeita", true, 0);
		return;
	}

	if (!fastcharge_mode) {
		for (i = 0; i < info->jeita_para.size; i++) {
			cur_jeita_data = info->jeita_para.jeita_data + i;
			if (temp < cur_jeita_data->temp_high &&
			    temp >= cur_jeita_data->temp_low)
				break;
		}
		if (i == info->jeita_para.size) {
			mca_log_err("can not find jeita para\n");
			return;
		}
	} else {
		for (i = 0; i < info->jeita_para.fcc_size; i++) {
			cur_jeita_data = info->jeita_para.jeita_ffc_data + i;
			if (temp < cur_jeita_data->temp_high &&
			    temp >= cur_jeita_data->temp_low)
				break;
		}
		if (i == info->jeita_para.fcc_size) {
			mca_log_err("can not find jeita fcc para\n");
			return;
		}
	}

	/* select cur jeita_data */
	if (info->proc_data.cur_jeita_index == -1 || info->baacfg_update) {
		jeita_data = cur_jeita_data;
		info->last_chg_curr[0] =
			mca_buckchg_jeita_get_curr(cur_jeita_data, &vbat_index);
	} else {
		if (info->proc_data.cur_jeita_index == i ||
		    fastcharge_mode != info->last_fastcharge_mode[0])
			jeita_data = cur_jeita_data;
		else {
			if (!fastcharge_mode || info->jeita_para.fcc_size == 0)
				jeita_data = info->jeita_para.jeita_data +
					     info->proc_data.cur_jeita_index;
			else
				jeita_data = info->jeita_para.jeita_ffc_data +
					     info->proc_data.cur_jeita_index;

			mca_log_info("temp_low: %d, temp_high: %d, temp: %d\n",
				     jeita_data->temp_low -
					     jeita_data->low_temp_hys,
				     jeita_data->temp_high +
					     jeita_data->high_temp_hys,
				     temp);
			if (i > info->proc_data.cur_jeita_index) {
				if (info->proc_data.cur_jeita_index == 0 &&
				    mca_smartchg_is_extreme_cold_enabled())
					jeita_data = cur_jeita_data;
				else if (temp <
					 jeita_data->temp_high +
						 jeita_data->high_temp_hys)
					hys_affect = 1;
				else
					jeita_data = cur_jeita_data;
			} else {
				if (temp > jeita_data->temp_low -
						   jeita_data->low_temp_hys)
					hys_affect = 1;
				else
					jeita_data = cur_jeita_data;
			}
		}
	}

	/* select whether vote */
	info->proc_data.max_chg_curr =
		mca_buckchg_jeita_get_curr(jeita_data, &vbat_index);
	if (info->baacfg_update) {
		data_change = 1;
		info->baacfg_update = false;
		mca_log_info("BAA config data update\n");
	} else if (info->proc_data.cur_jeita_index != i &&
		   !hys_affect) { //jeita_index change
		data_change = 1;
		mca_log_info(
			"jeita_index: cur_jeita_index:%d, i:%d, hys_affect:%d\n",
			info->proc_data.cur_jeita_index, i, hys_affect);
		info->last_chg_curr[0] = info->proc_data.max_chg_curr;
	} else { //jeita_index not change, check vbat_thre
		if (info->proc_data.max_chg_curr > info->last_chg_curr[0]) {
			bool in_cold_zone = (i == 1 || i == 2 ||
					     (i == 3 && hys_affect));
			int low_hyst = in_cold_zone ? info->vbat_low_cold_hyst :
						      info->vbat_low_hyst;

			if (vbat_index != -1 &&
			    vbat < jeita_data->volt_para.volt_data[vbat_index]
						    .voltage -
					    low_hyst) {
				data_change = 1;
				mca_log_info(
					"vbat_thre[l->s]: max_chg_curr:%d, info->last_chg_curr[0]:%d, vbat_index:%d\n",
					info->proc_data.max_chg_curr,
					info->last_chg_curr[0], vbat_index);
				info->last_chg_curr[0] = info->proc_data.max_chg_curr;
			}
		} else if (info->proc_data.max_chg_curr < info->last_chg_curr[0]) {
			if (vbat_index > 0 &&
			    vbat > jeita_data->volt_para
					    .volt_data[vbat_index - 1]
					    .voltage) {
				data_change = 1;
				mca_log_info(
					"vbat_thre[s->l]: max_chg_curr:%d, info->last_chg_curr[0]:%d, vbat_index:%d\n",
					info->proc_data.max_chg_curr,
					info->last_chg_curr[0], vbat_index);
				info->last_chg_curr[0] = info->proc_data.max_chg_curr;
			}
		}
	}

	vterm = jeita_data->vterm;

	if (vterm > ABNORMAL_BATT_FV_MAX) {
		vterm -= info->smartchg_data.delta_fv;
		mca_vote_named("chg_enable", "jeita-hot", true, 1);
		if (vterm != info->vterm) {
			mca_vote_named("term_volt", "jeita", true, vterm);
			info->vterm = vterm;
		}
	} else {
		if (vbat >= vterm - info->jeita_hot_termination_hyst)
			mca_vote_named("chg_enable", "jeita-hot", true, 0);
		else if (vbat < vterm - JEITA_HOT_RECHARGE_VBAT_HYS)
			mca_vote_named("chg_enable", "jeita-hot", true, 1);
	}

	if (info->dtpt_status)
		chg_curr = info->proc_data.max_chg_curr *
			   MCA_DTPT_MOLECULE_SCALE / MCA_DTPT_DENOM_SCALE;
	else
		chg_curr = info->proc_data.max_chg_curr;

	mca_log_info(
		"cur index %d/%d info->last_chg_curr[0] %d chg_curr %d/%d fastcharge %d jeita_vterm %d jeita_iterm %d vterm %d delta_fv %d hys_affect %d\n",
		i, info->proc_data.cur_jeita_index, info->last_chg_curr[0],
		info->proc_data.max_chg_curr, chg_curr, fastcharge_mode,
		jeita_data->vterm, jeita_data->iterm, vterm,
		info->smartchg_data.delta_fv, hys_affect);

	info->last_fastcharge_mode[0] = fastcharge_mode;
	if (chg_curr)
		mca_vote_named("chg_enable", "jeita", true, 1);
	else
		mca_vote_named("chg_enable", "jeita", true, 0);

	if (data_change) {
		if (!hys_affect)
			info->proc_data.cur_jeita_index = i;
		mca_vote_named("buck_charge_curr", "jeita", true, chg_curr);
		mca_vote_named("term_volt", "jeita", true, vterm);
		if (!info->support_base_flip && !info->base_flip_same)
			mca_vote_named("term_curr", "jeita", true,
				 jeita_data->iterm);
	}

	if (!fastcharge_mode && info->base_flip_same) {
		info->cold_zone =
			(info->proc_data.cur_jeita_index == JEITA_COLD_ZONE_INDEX);
		mca_log_info(
			"cur index %d/%d info->last_chg_curr[0] %d cold_zone %d\n", i,
			info->proc_data.cur_jeita_index, info->last_chg_curr[0],
			info->cold_zone);
	}
}

static int mca_buckchg_jeita_flip_charge_limit(struct mca_votable *votable,
					       void *data, int effective_result,
					       const char *effective_client)
{
	if (!data)
		return -1;

	mca_log_err("set flip limit current %d\n", effective_result);

	return platform_class_loadsw_set_ibat_limit(LOADSW_ROLE_MASTER,
						    effective_result);
}
static int mca_buckchg_jeita_init_voter(struct mca_buckchg_jeita_dev *info)
{
	if (!mca_named_votable_exists("chg_enable") ||
	    !mca_named_votable_exists("buck_charge_curr") ||
	    !mca_named_votable_exists("term_volt") ||
	    !mca_named_votable_exists("term_curr"))
		return -EPROBE_DEFER;
	info->flip_fcc_voter = mca_create_votable("flip_charge_curr", MCA_VOTE_MIN,
		mca_buckchg_jeita_flip_charge_limit,
		MCA_BUCKCHG_JEITA_FLIP_CURRENT_DEFAULT_VALUE, info);
	if (IS_ERR(info->flip_fcc_voter))
		return PTR_ERR(info->flip_fcc_voter);
	info->voter_ok = 1;
	return 0;
}
static void mca_buckchg_jeita_monitor_work(struct work_struct *work)
{
	struct mca_buckchg_jeita_dev *info = container_of(
		work, struct mca_buckchg_jeita_dev, monitor_work.work);

	guard(mutex)(&info->state_lock);
	if (!info->ready || info->stopping || !info->online)
		return;
	mca_buckchg_jeita_update(info);
	if (info->support_base_flip) {
		mca_buckchg_base_jeita_update(info);
		mca_buckchg_flip_jeita_update(info);
	}
	mod_delayed_work(system_wq, &info->monitor_work,
			 msecs_to_jiffies(MCA_BUCKCHG_JEITA_UPDATE_NORMAL_INTERVAL));
}

static int
mca_buckchg_parse_volt_para(struct device *dev, struct device_node *node, const char *name,
			    struct mca_buckchg_jeita_volt_para *volt_para)
{
	struct mca_buckchg_jeita_volt_data *volt_data;
	int array_len, row, col, i;
	const char *tmp_string = NULL;

	if (strcmp(name, "null") == 0) {
		mca_log_info("no need parse volt para\n");
		return 0;
	}
	array_len = mca_parse_dts_count_strings(node, name,
						JEITA_VOLT_DATA_MAX_GROUP,
						BUCKCHG_VOLTAGE_PARA_MAX);
	if (array_len <= 0) {
		mca_log_err("parse %s failed\n", name);
		return -EINVAL;
	}

	volt_para->size = array_len / BUCKCHG_VOLTAGE_PARA_MAX;
	volt_para->volt_data =
		devm_kcalloc(dev, volt_para->size, sizeof(*volt_data), GFP_KERNEL);
	if (!volt_para->volt_data) {
		mca_log_err("volt para no mem\n");
		return -ENOMEM;
	}
	volt_data = volt_para->volt_data;
	for (i = 0; i < array_len; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string))
			return -EINVAL;

		row = i / BUCKCHG_VOLTAGE_PARA_MAX;
		col = i % BUCKCHG_VOLTAGE_PARA_MAX;

		switch (col) {
		case BUCKCHG_VOLTAGE_TH:
			if (kstrtoint(tmp_string, 10, &volt_data[row].voltage))
				goto error;
			break;
		case BUCKCHG_CURRENT_MAX:
			if (kstrtoint(tmp_string, 10,
				      &volt_data[row].max_current))
				goto error;
			break;
		default:
			break;
		}
	}

	for (i = 0; i < volt_para->size; i++)
		mca_log_debug("volt_para %d %d\n", volt_data[i].voltage,
			      volt_data[i].max_current);

	return 0;
error:
	volt_data = NULL;
	return -EINVAL;
}

static int
mca_buckchg_jeita_parse_para(struct device *dev, struct device_node *node, const char *name,
			     struct mca_buckchg_jeita_para *jeita_info)
{
	int array_len, row, col, i;
	const char *tmp_string = NULL;
	char ffc_name[50];
	struct mca_buckchg_jeita_data *jeita_data, *jeita_ffc_data;

	if (!node) {
		mca_log_err("node in null\n");
		return -EINVAL;
	}

	array_len = mca_parse_dts_count_strings(
		node, name, JEITA_DATA_MAX_GROUP, JEITA_TEMP_PARA_MAX);
	if (array_len <= 0) {
		mca_log_err("parse jeita failed\n");
		return -EINVAL;
	}

	jeita_info->size = array_len / JEITA_TEMP_PARA_MAX;
	jeita_info->jeita_data = devm_kcalloc(dev,
		jeita_info->size, sizeof(*jeita_info->jeita_data), GFP_KERNEL);
	if (!jeita_info->jeita_data) {
		mca_log_err("temp para no mem\n");
		return -ENOMEM;
	}
	jeita_data = jeita_info->jeita_data;
	for (i = 0; i < array_len; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string))
			return -EINVAL;

		row = i / JEITA_TEMP_PARA_MAX;
		col = i % JEITA_TEMP_PARA_MAX;
		switch (col) {
		case JEITA_TEMP_PARA_TEMP_LOW:
			if (kstrtoint(tmp_string, 10,
				      &jeita_data[row].temp_low))
				goto error;
			break;
		case JEITA_TEMP_PARA_TEMP_HIGH:
			if (kstrtoint(tmp_string, 10,
				      &jeita_data[row].temp_high))
				goto error;
			break;
		case JEITA_TEMP_PARA_LOW_TEMP_HYS:
			if (kstrtoint(tmp_string, 10,
				      &jeita_data[row].low_temp_hys))
				goto error;
			break;
		case JEITA_TEMP_PARA_HIGH_TEMP_HYS:
			if (kstrtoint(tmp_string, 10,
				      &jeita_data[row].high_temp_hys))
				goto error;
			break;
		case JEITA_TEMP_PARA_MAX_CURRENT:
			if (kstrtoint(tmp_string, 10,
				      &jeita_data[row].max_current))
				goto error;
			break;
		case JEITA_TEMP_PARA_VTERM:
			if (kstrtoint(tmp_string, 10, &jeita_data[row].vterm))
				goto error;
			break;
		case JEITA_TEMP_PARA_ITERM:
			if (kstrtoint(tmp_string, 10, &jeita_data[row].iterm))
				goto error;
			break;
		case JEITA_TEMP_PARA_VOLT_PARA_NAME:
			if (mca_buckchg_parse_volt_para(
				    dev, node, tmp_string,
				    &jeita_data[row].volt_para))
				goto error;
			break;
		default:
			break;
		}
	}

	for (i = 0; i < jeita_info->size; i++)
		mca_log_err("jeita para %d %d %d %d %d %d %d %d\n",
			    jeita_data[i].temp_low, jeita_data[i].temp_high,
			    jeita_data[i].low_temp_hys,
			    jeita_data[i].high_temp_hys,
			    jeita_data[i].max_current, jeita_data[i].vterm,
			    jeita_data[i].iterm, jeita_data[i].volt_para.size);

	snprintf(ffc_name, sizeof(ffc_name), "%s", name);
	snprintf(ffc_name + strlen(ffc_name),
		 sizeof(ffc_name) - strlen(ffc_name), "_ffc");
	mca_log_err("new name:%s\n", ffc_name);
	array_len = mca_parse_dts_count_strings(
		node, ffc_name, JEITA_DATA_MAX_GROUP, JEITA_TEMP_PARA_MAX);
	if (array_len <= 0) {
		mca_log_err("parse jeita para ffc failed\n");
		return -EINVAL;
	}

	jeita_info->fcc_size = array_len / JEITA_TEMP_PARA_MAX;

	jeita_info->jeita_ffc_data =
		devm_kcalloc(dev, jeita_info->fcc_size,
			sizeof(*jeita_info->jeita_ffc_data), GFP_KERNEL);
	if (!jeita_info->jeita_ffc_data) {
		mca_log_err("temp para no mem\n");
		return -ENOMEM;
	}
	jeita_ffc_data = jeita_info->jeita_ffc_data;
	for (i = 0; i < array_len; i++) {
		if (mca_parse_dts_string_index(node, ffc_name, i, &tmp_string))
			return -EINVAL;

		row = i / JEITA_TEMP_PARA_MAX;
		col = i % JEITA_TEMP_PARA_MAX;
		switch (col) {
		case JEITA_TEMP_PARA_TEMP_LOW:
			if (kstrtoint(tmp_string, 10,
				      &jeita_ffc_data[row].temp_low))
				goto error1;
			break;
		case JEITA_TEMP_PARA_TEMP_HIGH:
			if (kstrtoint(tmp_string, 10,
				      &jeita_ffc_data[row].temp_high))
				goto error1;
			break;
		case JEITA_TEMP_PARA_LOW_TEMP_HYS:
			if (kstrtoint(tmp_string, 10,
				      &jeita_ffc_data[row].low_temp_hys))
				goto error1;
			break;
		case JEITA_TEMP_PARA_HIGH_TEMP_HYS:
			if (kstrtoint(tmp_string, 10,
				      &jeita_ffc_data[row].high_temp_hys))
				goto error1;
			break;
		case JEITA_TEMP_PARA_MAX_CURRENT:
			if (kstrtoint(tmp_string, 10,
				      &jeita_ffc_data[row].max_current))
				goto error1;
			break;
		case JEITA_TEMP_PARA_VTERM:
			if (kstrtoint(tmp_string, 10,
				      &jeita_ffc_data[row].vterm))
				goto error1;
			break;
		case JEITA_TEMP_PARA_ITERM:
			if (kstrtoint(tmp_string, 10,
				      &jeita_ffc_data[row].iterm))
				goto error1;
			break;
		case JEITA_TEMP_PARA_VOLT_PARA_NAME:
			if (mca_buckchg_parse_volt_para(
				    dev, node, tmp_string,
				    &jeita_ffc_data[row].volt_para))
				goto error1;
			break;
		default:
			break;
		}
	}
	for (i = 0; i < jeita_info->fcc_size; i++)
		mca_log_err("jeita ffc para %d %d %d %d %d %d %d %d\n",
			    jeita_ffc_data[i].temp_low,
			    jeita_ffc_data[i].temp_high,
			    jeita_ffc_data[i].low_temp_hys,
			    jeita_ffc_data[i].high_temp_hys,
			    jeita_ffc_data[i].max_current,
			    jeita_ffc_data[i].vterm, jeita_ffc_data[i].iterm,
			    jeita_ffc_data[i].volt_para.size);
	return 0;
error1:
	jeita_ffc_data = NULL;
error:
	jeita_data = NULL;
	return -EINVAL;
}
static int mca_jeita_validate_table(struct mca_buckchg_jeita_data *table, int count)
{
	int i, j;

	if (!table || count <= 0 || count > JEITA_DATA_MAX_GROUP)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		struct mca_buckchg_jeita_data *entry = &table[i];

		if (entry->temp_low < -65535 || entry->temp_high > 65535 ||
		    entry->temp_low >= entry->temp_high || entry->low_temp_hys < 0 ||
		    entry->low_temp_hys > 100 || entry->high_temp_hys < 0 ||
		    entry->high_temp_hys > 100 || entry->max_current < 0 ||
		    entry->max_current > 30000 || entry->vterm <= 0 || entry->vterm > 5000 ||
		    entry->iterm <= 0 || entry->iterm > 30000 ||
		    (i && table[i - 1].temp_high != entry->temp_low) ||
		    entry->volt_para.size < 0 ||
		    entry->volt_para.size > JEITA_VOLT_DATA_MAX_GROUP ||
		    (entry->volt_para.size && !entry->volt_para.volt_data))
			return -EINVAL;
		for (j = 0; j < entry->volt_para.size; j++) {
			struct mca_buckchg_jeita_volt_data *volt = &entry->volt_para.volt_data[j];

			if (volt->voltage <= 0 || volt->voltage > entry->vterm ||
			    volt->max_current < 0 || volt->max_current > entry->max_current ||
			    (j && entry->volt_para.volt_data[j - 1].voltage >= volt->voltage))
				return -EINVAL;
		}
		entry->design_vterm = entry->vterm;
	}
	return 0;
}

static int mca_jeita_validate_para(struct mca_buckchg_jeita_para *para)
{
	int ret = mca_jeita_validate_table(para->jeita_data, para->size);

	if (ret)
		return ret;
	return mca_jeita_validate_table(para->jeita_ffc_data, para->fcc_size);
}

static int mca_buckchg_jeita_parse_dt(struct mca_buckchg_jeita_dev *info)
{
	struct device_node *original = info->dev->of_node;
	struct device_node *node = of_node_get(original);
	const struct mca_hwid *hwid = mca_get_hwid_info();
	const char *dev_name = NULL;
	int ret;

	if (!hwid) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	mca_parse_dts_u32(original, "vbat_high_hyst", &info->vbat_high_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_HIGH_HYST);
	mca_parse_dts_u32(original, "vbat_low_hyst", &info->vbat_low_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(original, "vbat_low_cold_hyst", &info->vbat_low_cold_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(original, "base_vbat_low_hyst", &info->base_vbat_low_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(original, "flip_vbat_low_hyst", &info->flip_vbat_low_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(original, "jeita_hot_termination_hyst",
			  &info->jeita_hot_termination_hyst, 0);
	if (info->vbat_high_hyst < 0 || info->vbat_high_hyst > 1000 ||
	    info->vbat_low_hyst < 0 || info->vbat_low_hyst > 1000 ||
	    info->vbat_low_cold_hyst < 0 || info->vbat_low_cold_hyst > 1000 ||
	    info->base_vbat_low_hyst < 0 || info->base_vbat_low_hyst > 1000 ||
	    info->flip_vbat_low_hyst < 0 || info->flip_vbat_low_hyst > 1000 ||
	    info->jeita_hot_termination_hyst < 0 || info->jeita_hot_termination_hyst > 100) {
		ret = -EINVAL;
		goto out;
	}
	info->has_gbl_batt_para = of_property_read_bool(original, "has-global-batt-para");
	info->has_tmp_batt_para = of_property_read_bool(original, "has-tmp-batt-para");
	info->support_base_flip = of_property_read_bool(original, "support-base-flip");
	info->base_flip_same = of_property_read_bool(original, "base-flip-same");
	if (info->has_gbl_batt_para && hwid->country_version != CountryCN) {
		of_node_put(node);
		node = of_find_node_by_name(NULL, "mca_buckchg_jeita_gbl_para");
	}
	if (info->has_tmp_batt_para) {
		ret = platform_fg_ops_get_device_name(FG_IC_MASTER, &dev_name);
		if (ret || !dev_name) {
			ret = -EPROBE_DEFER;
			goto out;
		}
		if (!strcmp(dev_name, "2@BP")) {
			of_node_put(node);
			node = of_find_node_by_name(NULL, "mca_buckchg_jeita_tmp_para");
		}
	}
	if (!node) {
		ret = -EINVAL;
		goto out;
	}
	ret = mca_buckchg_jeita_parse_para(info->dev, node, "jeita_para", &info->jeita_para);
	if (ret)
		goto out;
	ret = mca_jeita_validate_para(&info->jeita_para);
	if (ret || !info->support_base_flip)
		goto out;
	ret = mca_buckchg_jeita_parse_para(info->dev, node, "base_jeita_para",
					 &info->base_jeita_para);
	if (ret)
		goto out;
	ret = mca_jeita_validate_para(&info->base_jeita_para);
	if (ret)
		goto out;
	ret = mca_buckchg_jeita_parse_para(info->dev, node, "flip_jeita_para",
					 &info->flip_jeita_para);
	if (!ret)
		ret = mca_jeita_validate_para(&info->flip_jeita_para);
out:
	of_node_put(node);
	return ret;
}
static int mca_buckchg_jeita_process_event(int event, int value, void *data)
{
	struct mca_buckchg_jeita_dev *info = data;

	if (!info)
		return -EINVAL;
	guard(mutex)(&info->state_lock);
	if (!info->ready || info->stopping)
		return -ENODEV;
	switch (event) {
	case MCA_EVENT_USB_CONNECT:
		info->wired_online = true;
		break;
	case MCA_EVENT_WIRELESS_CONNECT:
		info->wireless_online = true;
		break;
	case MCA_EVENT_USB_DISCONNECT:
		info->wired_online = false;
		break;
	case MCA_EVENT_WIRELESS_DISCONNECT:
		info->wireless_online = false;
		break;
	case MCA_EVENT_BATTERY_DTPT:
		info->dtpt_status = value;
		return 0;
	default:
		return 0;
	}
	info->online = info->wired_online || info->wireless_online;
	if (info->online) {
		info->proc_data.cur_jeita_index = -1;
		info->proc_data.max_chg_curr = 0;
		mod_delayed_work(system_wq, &info->monitor_work, 0);
	} else {
		cancel_delayed_work(&info->monitor_work);
		info->flip_proc_data.cur_jeita_index = 0;
		info->flip_proc_data.temp_hys_en = 0;
		info->flip_proc_data.max_chg_curr = -1;
		info->base_proc_data.cur_jeita_index = 0;
		info->base_proc_data.temp_hys_en = 0;
		info->base_proc_data.max_chg_curr = -1;
	}
	return 0;
}

static int
mca_strategy_buckchg_jeita_get_normal_vterm(struct mca_buckchg_jeita_dev *info)
{
	int cur_normal_vterm = 0, temp = 0;
	int ret, i;
	struct mca_buckchg_jeita_data *cur_jeita_data;

	ret = strategy_class_fg_ops_get_temperature(&temp);
	if (ret) {
		mca_log_err("get battery temp failed\n");
		return ret;
	}
	temp /= 10;

	for (i = 0; i < info->jeita_para.size; i++) {
		cur_jeita_data = info->jeita_para.jeita_data + i;
		if (temp < cur_jeita_data->temp_high &&
		    temp >= cur_jeita_data->temp_low)
			break;
	}
	if (i == info->jeita_para.size) {
		mca_log_err("can not find jeita para\n");
		return -ENODATA;
	}

	cur_normal_vterm = cur_jeita_data->vterm;

	return cur_normal_vterm;
}

static int
mca_strategy_buckchg_jeita_get_ffc_iterm(struct mca_buckchg_jeita_dev *info)
{
	int cur_ffc_itrem = 0, temp = 0;
	int ret, i;
	struct mca_buckchg_jeita_data *cur_jeita_data;

	ret = strategy_class_fg_ops_get_temperature(&temp);
	if (ret) {
		mca_log_err("get battery temp failed\n");
		return ret;
	}
	temp /= 10;

	for (i = 0; i < info->jeita_para.fcc_size; i++) {
		cur_jeita_data = info->jeita_para.jeita_ffc_data + i;
		if (temp < cur_jeita_data->temp_high &&
		    temp >= cur_jeita_data->temp_low)
			break;
	}
	if (i == info->jeita_para.fcc_size) {
		mca_log_err("can not find jeita fcc para\n");
		return -ENODATA;
	}

	cur_ffc_itrem = cur_jeita_data->iterm;

	return cur_ffc_itrem;
}

static int
mca_strategy_buckchg_jeita_get_ffc_vterm(struct mca_buckchg_jeita_dev *info)
{
	int cur_ffc_vterm = 0, temp = 0;
	int ret, i;
	struct mca_buckchg_jeita_data *cur_jeita_data;

	ret = strategy_class_fg_ops_get_temperature(&temp);
	if (ret) {
		mca_log_err("get battery temp failed\n");
		return ret;
	}
	temp /= 10;

	for (i = 0; i < info->jeita_para.fcc_size; i++) {
		cur_jeita_data = info->jeita_para.jeita_ffc_data + i;
		if (temp < cur_jeita_data->temp_high &&
		    temp >= cur_jeita_data->temp_low)
			break;
	}
	if (i == info->jeita_para.fcc_size) {
		mca_log_err("can not find jeita fcc para\n");
		return -ENODATA;
	}

	cur_ffc_vterm = cur_jeita_data->vterm;

	return cur_ffc_vterm;
}
static int mca_strategy_buckchg_jeita_get_status(int status, void *value, void *data)
{
	struct mca_buckchg_jeita_dev *info = data;
	int result;

	if (!info || !value)
		return -EINVAL;
	guard(mutex)(&info->state_lock);
	if (!info->ready || info->stopping)
		return -ENODEV;
	switch (status) {
	case STRATEGY_STATUS_TYPE_JEITA_FFC_ITERM:
		result = mca_strategy_buckchg_jeita_get_ffc_iterm(info);
		break;
	case STRATEGY_STATUS_TYPE_JEITA_NORMAL_VTERM:
		result = mca_strategy_buckchg_jeita_get_normal_vterm(info);
		break;
	case STRATEGY_STATUS_TYPE_JEITA_FFC_VTERM:
		result = mca_strategy_buckchg_jeita_get_ffc_vterm(info);
		break;
	case STRATEGY_STATUS_TYPE_JEITA_COLD_ZONE:
		result = info->cold_zone;
		break;
	default:
		return -EINVAL;
	}
	if (result < 0)
		return result;
	*(int *)value = result;
	return 0;
}

static int mca_jeita_smartchg_delta_fv_callback(void *data,
						int effective_result)
{
	struct mca_buckchg_jeita_dev *info =
		(struct mca_buckchg_jeita_dev *)data;

	if (!data || effective_result < 0 || effective_result > 1000)
		return -EINVAL;

	guard(mutex)(&info->state_lock);
	if (!info->ready || info->stopping)
		return -ENODEV;
	info->smartchg_data.delta_fv = effective_result;
	mca_log_err("effective_result: %d\n", effective_result);

	return 0;
}
static int mca_jeita_basp_prepare(void *data, const struct mca_basp_update *update)
{
	struct mca_buckchg_jeita_dev *info = data;
	u32 total, i;
	int ret = -EINVAL;

	if (!info || !update || !update->jeita ||
	    update->ffc_size > info->jeita_para.fcc_size ||
	    update->normal_size > info->jeita_para.size)
		return -EINVAL;
	mutex_lock(&info->state_lock);
	if (!info->ready || info->stopping) {
		ret = -ENODEV;
		goto unlock;
	}
	if (info->pending_basp) {
		ret = -EBUSY;
		goto unlock;
	}
	total = update->ffc_size + update->normal_size;
	for (i = 0; i < total; i++) {
		const struct smart_batt_jeita_term_para *para = &update->jeita[i];
		struct mca_buckchg_jeita_data *table;
		int count, j;

		if (i < update->ffc_size) {
			table = info->jeita_para.jeita_ffc_data;
			count = info->jeita_para.fcc_size;
		} else {
			table = info->jeita_para.jeita_data;
			count = info->jeita_para.size;
		}
		if (para->t_range.idx < 0 || para->t_range.idx >= count)
			goto unlock;
		table += para->t_range.idx;
		if (para->t_range.min != table->temp_low ||
		    para->t_range.max != table->temp_high ||
		    para->vterm <= 0 || para->vterm > table->design_vterm ||
		    para->iterm <= 0 || para->iterm > 30000)
			goto unlock;
		if (table->volt_para.size > 1 &&
		    para->vterm <= table->volt_para.volt_data[table->volt_para.size - 2].voltage)
			goto unlock;
		for (j = i < update->ffc_size ? 0 : update->ffc_size; j < i; j++)
			if (update->jeita[j].t_range.idx == para->t_range.idx)
				goto unlock;
	}
	info->pending_basp = kmemdup(update->jeita,
				     total * sizeof(*info->pending_basp), GFP_KERNEL);
	if (!info->pending_basp) {
		ret = -ENOMEM;
		goto unlock;
	}
	info->pending_ffc_size = update->ffc_size;
	info->pending_normal_size = update->normal_size;
	/* Keep state_lock held until the transaction commits or aborts. */
	return 0;
unlock:
	mutex_unlock(&info->state_lock);
	return ret;
}

static void mca_jeita_basp_abort(void *data)
{
	struct mca_buckchg_jeita_dev *info = data;

	kfree(info->pending_basp);
	info->pending_basp = NULL;
	mutex_unlock(&info->state_lock);
}

static void mca_jeita_basp_commit(void *data)
{
	struct mca_buckchg_jeita_dev *info = data;
	u32 total = info->pending_ffc_size + info->pending_normal_size, i;

	for (i = 0; i < total; i++) {
		const struct smart_batt_jeita_term_para *para = &info->pending_basp[i];
		struct mca_buckchg_jeita_data *table = i < info->pending_ffc_size ?
			info->jeita_para.jeita_ffc_data : info->jeita_para.jeita_data;

		table += para->t_range.idx;
		table->vterm = para->vterm;
		table->iterm = para->iterm;
		if (table->volt_para.size)
			table->volt_para.volt_data[table->volt_para.size - 1].voltage = para->vterm;
	}
	info->baacfg_update = true;
	info->proc_data.cur_jeita_index = -1;
	if (info->online)
		mod_delayed_work(system_wq, &info->monitor_work, 0);
	mca_jeita_basp_abort(data);
}

static struct mca_smartchg_if_ops g_jeita_smartchg_if_ops = {
	.type = MCA_SMARTCHG_IF_CHG_TYPE_JEITA,
	.data = NULL,
	.set_delta_fv = mca_jeita_smartchg_delta_fv_callback,
	.set_delta_ichg = NULL,
	.prepare_baa_para = mca_jeita_basp_prepare,
	.commit_baa_para = mca_jeita_basp_commit,
	.abort_baa_para = mca_jeita_basp_abort,
};
static int mca_buckchg_jeita_probe(struct platform_device *pdev)
{
	struct mca_buckchg_jeita_dev *info;
	struct mca_smartchg_if_ops ops = g_jeita_smartchg_if_ops;
	int ret, online = 0, wireless_online = 0;

	if (strategy_class_fg_ops_is_init_ok() <= 0)
		return -EPROBE_DEFER;
	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->dev = &pdev->dev;
	mutex_init(&info->state_lock);
	INIT_DELAYED_WORK(&info->monitor_work, mca_buckchg_jeita_monitor_work);
	platform_set_drvdata(pdev, info);
	ret = mca_buckchg_jeita_parse_dt(info);
	if (ret)
		return ret;
	ret = mca_buckchg_jeita_init_voter(info);
	if (ret)
		return ret;
	if (!mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					 STRATEGY_STATUS_TYPE_ONLINE, &online))
		info->wired_online = !!online;
	if (!mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					 STRATEGY_STATUS_TYPE_ONLINE, &wireless_online))
		info->wireless_online = !!wireless_online;
	info->online = info->wired_online || info->wireless_online;
	info->proc_data.cur_jeita_index = -1;
	info->base_proc_data.cur_jeita_index = -1;
	info->flip_proc_data.cur_jeita_index = -1;
	ret = mca_strategy_ops_register(STRATEGY_FUNC_TYPE_JEITA,
		mca_buckchg_jeita_process_event, mca_strategy_buckchg_jeita_get_status, NULL, info);
	if (ret)
		goto voter;
	ops.data = info;
	ret = mca_smartchg_if_ops_register(&ops);
	if (ret)
		goto strategy;
	WRITE_ONCE(info->ready, true);
	if (info->online)
		mod_delayed_work(system_wq, &info->monitor_work, 0);
	return 0;
strategy:
	mca_strategy_ops_unregister(STRATEGY_FUNC_TYPE_JEITA, info);
voter:
	disable_delayed_work_sync(&info->monitor_work);
	mca_destroy_votable(info->flip_fcc_voter);
	return ret;
}

static void mca_buckchg_jeita_stop(struct mca_buckchg_jeita_dev *info)
{
	if (READ_ONCE(info->stopping))
		return;
	WRITE_ONCE(info->ready, false);
	mca_smartchg_if_ops_unregister(MCA_SMARTCHG_IF_CHG_TYPE_JEITA, info);
	mca_strategy_ops_unregister(STRATEGY_FUNC_TYPE_JEITA, info);
	mutex_lock(&info->state_lock);
	info->stopping = true;
	mutex_unlock(&info->state_lock);
	disable_delayed_work_sync(&info->monitor_work);
	mca_vote_named("chg_enable", "jeita-hot", false, 0);
	mca_vote_named("buck_charge_curr", "swocp", false, 0);
	mca_vote_named("chg_enable", "jeita", false, 0);
	mca_vote_named("buck_charge_curr", "jeita", false, 0);
	mca_vote_named("term_volt", "jeita", false, 0);
	mca_vote_named("term_curr", "jeita", false, 0);
	mca_destroy_votable(info->flip_fcc_voter);
	info->flip_fcc_voter = NULL;
}

static void mca_buckchg_jeita_remove(struct platform_device *pdev)
{
	mca_buckchg_jeita_stop(platform_get_drvdata(pdev));
}
static void mca_buckchg_jeita_shutdown(struct platform_device *pdev)
{
	mca_buckchg_jeita_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,buckchg_jeita" },
	{},
};

MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_buckchg_jeita_driver = {
	.driver	= {
		.name = "mca_buckchg_jeita",
		.owner = THIS_MODULE,
		.of_match_table = match_table,
	},
	.probe = mca_buckchg_jeita_probe,
	.remove = mca_buckchg_jeita_remove,
	.shutdown = mca_buckchg_jeita_shutdown,
};

static int __init mca_buckchg_jeita_init(void)
{
	return platform_driver_register(&mca_buckchg_jeita_driver);
}
module_init(mca_buckchg_jeita_init);

static void __exit mca_buckchg_jeita_exit(void)
{
	platform_driver_unregister(&mca_buckchg_jeita_driver);
}
module_exit(mca_buckchg_jeita_exit);

MODULE_DESCRIPTION("buckchg jeita");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
