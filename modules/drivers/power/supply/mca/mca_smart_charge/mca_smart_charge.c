// SPDX-License-Identifier: GPL-2.0
/*
 * mca_smart_charge.c
 *
 * smart_charge driver
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
#include <linux/thermal.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/srcu.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/unaligned.h>
#include <linux/uaccess.h>
#include <linux/kref.h>
#include <linux/compat.h>
#include <mca/protocol/protocol_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/strategy/strategy_soc_limit.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_voter.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_panel.h>
#include <mca/smartchg/smart_chg_class.h>
#include "inc/mca_smart_charge.h"
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_hwid.h>
#include <mca/shared_memory/charger_partition_class.h>
#include <linux/ktime.h>
#include "hwid.h"

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_smart_charge"
#endif
#ifndef BIT
#define BIT(x) (1 << (x))
#endif

#define SMART_CHG_SOCLMT_TRIG_DEFAULT 80
#define SMART_CHG_SOCLMT_CANCEL_DEFAULT 5
#define SMART_CHARGE_DELTA_FV_DEFAULT_VALUE 0
#define SMART_CHARGE_DELTA_ICHG_DEFAULT_VALUE 0
#define SMART_CHARGE_DTPT_DELTA_VALUE 15
#define SMART_CHARGE_FAKE_SCENE_DEFAULT -1

static struct smart_charge_info __rcu *global_smartchg_info;
static DEFINE_MUTEX(smart_instance_lock);
DEFINE_STATIC_SRCU(smart_instance_srcu);
static DEFINE_MUTEX(smart_ops_lock);
DEFINE_STATIC_SRCU(smart_ops_srcu);
static void smart_charge_replay(void);
static void smart_charge_replay_votes(struct smart_charge_info *info);
static struct mca_smartchg_if_ops __rcu
	*g_mca_smartchg_if_ops[MCA_SMARTCHG_IF_CHG_TYPE_END];
static void smart_charge_report_scene(struct smart_charge_info *info,
				      int scene);

static struct mca_smartchg_if_ops *mca_smartchg_if_get_ops(unsigned int type)
{
	return srcu_dereference(g_mca_smartchg_if_ops[type], &smart_ops_srcu);
}

int mca_smartchg_if_ops_register(struct mca_smartchg_if_ops *ops)
{
	struct mca_smartchg_if_ops *entry;

	if (!ops || !ops->data || ops->type < 0 ||
	    ops->type >= MCA_SMARTCHG_IF_CHG_TYPE_END)
		return -EINVAL;
	entry = kmemdup(ops, sizeof(*ops), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	mutex_lock(&smart_ops_lock);
	if (rcu_access_pointer(g_mca_smartchg_if_ops[ops->type])) {
		mutex_unlock(&smart_ops_lock);
		kfree(entry);
		return -EBUSY;
	}
	rcu_assign_pointer(g_mca_smartchg_if_ops[ops->type], entry);
	mutex_unlock(&smart_ops_lock);
	smart_charge_replay();
	return 0;
}
EXPORT_SYMBOL(mca_smartchg_if_ops_register);

/* Not callable from a provider callback. */
void mca_smartchg_if_ops_unregister(unsigned int type, void *data)
{
	struct mca_smartchg_if_ops *entry;

	if (type >= MCA_SMARTCHG_IF_CHG_TYPE_END)
		return;
	mutex_lock(&smart_ops_lock);
	entry = rcu_dereference_protected(g_mca_smartchg_if_ops[type],
					  lockdep_is_held(&smart_ops_lock));
	if (!entry || entry->data != data) {
		mutex_unlock(&smart_ops_lock);
		return;
	}
	rcu_assign_pointer(g_mca_smartchg_if_ops[type], NULL);
	mutex_unlock(&smart_ops_lock);
	synchronize_srcu(&smart_ops_srcu);
	kfree(entry);
}
EXPORT_SYMBOL(mca_smartchg_if_ops_unregister);

static void smart_charge_replay(void)
{
	guard(srcu)(&smart_instance_srcu);
	struct smart_charge_info *info =
		srcu_dereference(global_smartchg_info, &smart_instance_srcu);

	if (info) {
		atomic_set(&info->replay_needed, 1);
		mod_delayed_work(system_wq, &info->smart_charge_work, 0);
	}
}

static ssize_t smart_charge_sysfs_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf);
static ssize_t smart_charge_sysfs_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count);

struct mca_sysfs_attr_info smartchg_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTCHG,
			  smart_chg),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTCHG_FV,
			  smart_fv),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTCHG_ICHG,
			  smart_ichg),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTBATT,
			  smart_batt),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTNIGHT,
			  smart_night),
	mca_sysfs_attr_ro(smart_charge_sysfs, 0440, MCA_PROP_POSTURE, posture),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SCENE, scene),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_BOARD_TEMP,
			  board_temp),
	mca_sysfs_attr_ro(smart_charge_sysfs, 0444, MCA_PROP_SMART_SIC_MODE,
			  smart_sic_mode),
};

#define SMARTCHG_SYSFS_ATTRS_SIZE ARRAY_SIZE(smartchg_sysfs_field_tbl)

static struct attribute *smartchg_sysfs_attrs[SMARTCHG_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group smartchg_sysfs_attr_group = {
	.attrs = smartchg_sysfs_attrs,
};

void mca_smartchg_set_scene(int scene)
{
	guard(srcu)(&smart_instance_srcu);
	struct smart_charge_info *info =
		srcu_dereference(global_smartchg_info, &smart_instance_srcu);

	if (!info) {
		mca_log_err("info is null\n");
		return;
	}

	guard(mutex)(&info->state_lock);
	mca_log_info("set scene: %d:%d => %d\n", info->scene, info->fake_scene,
		     scene);
	if (scene >= SMART_CHG_SCENE_NORMAL &&
	    scene < SMART_CHG_SCENE_INDEX_MAX) {
		smart_charge_report_scene(info, scene);
		info->scene = scene;
	} else if (scene >= SMART_CHG_SCENE_REDIR_START &&
		   scene < SMART_CHG_SCENE_REDIR_INDEX_MAX) {
		info->fake_scene = scene - SMART_CHG_SCENE_REDIR_START;
	} else if (scene == SMART_CHARGE_FAKE_SCENE_DEFAULT) {
		info->fake_scene = SMART_CHARGE_FAKE_SCENE_DEFAULT;
	}

	mod_delayed_work(system_wq, &info->smart_sense_chg_work, 0);
}
EXPORT_SYMBOL(mca_smartchg_set_scene);

int mca_smartchg_get_scene(void)
{
	guard(srcu)(&smart_instance_srcu);
	struct smart_charge_info *info =
		srcu_dereference(global_smartchg_info, &smart_instance_srcu);

	if (!info) {
		mca_log_err("info is null\n");
		return 0;
	}

	if (info->fake_scene != SMART_CHARGE_FAKE_SCENE_DEFAULT)
		return READ_ONCE(info->fake_scene);

	return READ_ONCE(info->scene);
}
EXPORT_SYMBOL(mca_smartchg_get_scene);

void mca_smartchg_set_board_temp(int board_temp)
{
	guard(srcu)(&smart_instance_srcu);
	struct smart_charge_info *info =
		srcu_dereference(global_smartchg_info, &smart_instance_srcu);

	if (!info) {
		mca_log_err("info is null\n");
		return;
	}

	mca_log_info("set board_temp: %d\n", board_temp);
	WRITE_ONCE(info->board_temp, board_temp);
}
EXPORT_SYMBOL(mca_smartchg_set_board_temp);

int mca_smartchg_get_board_temp(void)
{
	guard(srcu)(&smart_instance_srcu);
	struct smart_charge_info *info =
		srcu_dereference(global_smartchg_info, &smart_instance_srcu);

	if (!info) {
		mca_log_err("info is null\n");
		return 0;
	}

	return READ_ONCE(info->board_temp);
}
EXPORT_SYMBOL(mca_smartchg_get_board_temp);

int mca_smartchg_is_extreme_cold_enabled(void)
{
	guard(srcu)(&smart_instance_srcu);
	struct smart_charge_info *info =
		srcu_dereference(global_smartchg_info, &smart_instance_srcu);

	return info && READ_ONCE(info->smart_chg_data
					 .smart_chgcfg[SMART_CHG_EXTREME_COLD]
					 .enable) == 1;
}
EXPORT_SYMBOL(mca_smartchg_is_extreme_cold_enabled);

int mca_smartchg_get_limit_soc(void)
{
	guard(srcu)(&smart_instance_srcu);
	struct smart_charge_info *info =
		srcu_dereference(global_smartchg_info, &smart_instance_srcu);

	if (!info) {
		mca_log_err("info is null\n");
		return 0;
	}
	if (READ_ONCE(info->soc_limit_enable))
		return READ_ONCE(info->soc_limit);
	return 0;
}
EXPORT_SYMBOL(mca_smartchg_get_limit_soc);

static void smart_charge_report_scene(struct smart_charge_info *info, int scene)
{
	int report_scene = 0;
	int len = 0;
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	struct mca_event_notify_data event_data = { 0 };

	if (scene != info->scene) {
		switch (scene) {
		case SMART_CHG_SCENE_PHONE:
		case SMART_CHG_SCENE_VIDEO:
		case SMART_CHG_SCENE_VIDEOCHAT:
			report_scene = scene;
			break;
		default:
			report_scene = 0;
			break;
		}

		if (report_scene != info->last_report_scene) {
			mca_log_info("report scene: %d => %d\n",
				     info->last_report_scene, report_scene);
			info->last_report_scene = report_scene;

			len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
				       "MCA_SMARTCHG_SCENE=%d", report_scene);
			event_data.event = event;
			event_data.event_len = len;
			mca_event_report_uevent(&event_data);
		}
	}
}

static void smart_charge_handle_sense_chg(struct smart_charge_info *info)
{
	int posture_st = SMART_CHG_POSTURE_UNKNOW;
	int scene = SMART_CHG_SCENE_NORMAL;
	int sic_mode = SMART_CHG_SIC_MODE_BALANCED;
	bool active_stat = false;
	int len = 0;
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	struct mca_event_notify_data event_data = { 0 };

	scene = info->fake_scene != SMART_CHARGE_FAKE_SCENE_DEFAULT ?
			info->fake_scene :
			info->scene;
	posture_st =
		info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.use_fake_value ?
			info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.fake_value :
			info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.func_value;

	if (!info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG].enable ||
	    posture_st <= SMART_CHG_POSTURE_UNKNOW ||
	    posture_st >= SMART_CHG_POSTURE_INDEX_MAX)
		return;

	switch (scene) {
	case SMART_CHG_SCENE_PHONE:
		active_stat = true;
		if (posture_st >= SMART_CHG_POSTURE_DESKTOP &&
		    posture_st <= SMART_CHG_POSTURE_HOLDER)
			sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
		else if (posture_st >= SMART_CHG_POSTURE_ONEHAND &&
			 posture_st <= SMART_CHG_POSTURE_TWOHAND_V)
			sic_mode = SMART_CHG_SIC_MODE_MIDDLE;
		else if (posture_st == SMART_CHG_POSTURE_ANS_CALL)
			sic_mode = SMART_CHG_SIC_MODE_BALANCED;
		break;
	case SMART_CHG_SCENE_VIDEO:
	case SMART_CHG_SCENE_VIDEOCHAT:
	case SMART_CHG_SCENE_PER_VIDEO:
		active_stat = true;
		if (posture_st >= SMART_CHG_POSTURE_DESKTOP &&
		    posture_st <= SMART_CHG_POSTURE_HOLDER)
			sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
		else if (posture_st >= SMART_CHG_POSTURE_ONEHAND &&
			 posture_st <= SMART_CHG_POSTURE_ANS_CALL)
			sic_mode = SMART_CHG_SIC_MODE_BALANCED;
		break;
	case SMART_CHG_SCENE_CLASS0:
	case SMART_CHG_SCENE_PER_CLASS0:
		if (info->support_sensechg_v2) {
			active_stat = true;
			if (posture_st >= SMART_CHG_POSTURE_DESKTOP &&
			    posture_st <= SMART_CHG_POSTURE_HOLDER)
				sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
			else if (posture_st >= SMART_CHG_POSTURE_ONEHAND &&
				 posture_st <= SMART_CHG_POSTURE_ANS_CALL)
				sic_mode = SMART_CHG_SIC_MODE_BALANCED;
		} else {
			mca_log_err("unsupport scene: %d\n", info->scene);
			return;
		}
		break;
	default:
		mca_log_err("unsupport scene: %d\n", info->scene);
		return;
	}

	if (info->screen_state == 0)
		sic_mode /= 2;

	mca_log_info(
		"active_stat: %d, online: %d, screen_state: %d, scene: %d, posture: %d, smart_sic_mode: %d -> %d\n",
		active_stat, info->online, info->screen_state, scene,
		posture_st, info->smart_sic_mode, sic_mode);
	if (sic_mode != info->smart_sic_mode) {
		info->smart_sic_mode = sic_mode;
		if (active_stat && info->online) {
			len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
				       "POWER_SUPPLY_SMART_SIC_MODE=%d",
				       info->smart_sic_mode);
			event_data.event = event;
			event_data.event_len = len;
			mca_event_report_uevent(&event_data);
		}
	}
}

static void smart_charge_set_soc_limit(bool enable)
{
	guard(srcu)(&smart_ops_srcu);
	struct mca_smartchg_if_ops *if_ops;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_soc_limit_sts)
			continue;
		if_ops->set_soc_limit_sts(if_ops->data, enable);
	}
}

#define SMART_CHG_SOC_HYS 2
static void smart_charge_soc_limit_workfunc(struct work_struct *work)
{
	struct smart_charge_info *info = container_of(
		work, struct smart_charge_info, smart_soc_limit_work.work);
	int curr_soc = 0;

	guard(mutex)(&info->state_lock);
	if (info->stopping)
		return;

	if (!info->online) {
		mca_log_err("adapter is plugout, don't handle soc limit\n");
		return;
	}

	if (info->soc_limit) {
		curr_soc = strategy_class_fg_ops_get_soc();
		if (curr_soc < 0 || curr_soc > 100)
			goto retry_soc;
		mca_log_info("curr_soc = %d, soc_limit_thre = %d\n", curr_soc,
			     info->soc_limit);
		if (curr_soc >= info->soc_limit) {
			if (info->soc_limit_enable == false) {
				info->soc_limit_enable = true;
				smart_charge_set_soc_limit(
					info->soc_limit_enable);
			}

			if (curr_soc >= info->soc_limit + SMART_CHG_SOC_HYS) {
				if (info->smart_chg_data
					    .smart_chgcfg[SMART_CHG_ENDURANCE_PRO]
					    .enable &&
				    !info->ignore_upload.endurance_pro) {
					mca_charge_mievent_report(
						CHARGE_DFX_SMART_ENDURANCE_SOC_ERR,
						&curr_soc, 1);
					info->ignore_upload.endurance_pro =
						true;
				}
				if (info->smart_chg_data
					    .smart_chgcfg[SMART_CHG_NAVIGATION]
					    .enable &&
				    !info->ignore_upload.navigation) {
					mca_charge_mievent_report(
						CHARGE_DFX_SMART_NAVIGATION_SOC_ERR,
						&curr_soc, 1);
					info->ignore_upload.navigation = true;
				}
			}

		} else if (curr_soc <=
			   info->soc_limit - SMART_CHG_SOCLMT_CANCEL_DEFAULT) {
			if (info->soc_limit_enable == true) {
				info->soc_limit_enable = false;
				smart_charge_set_soc_limit(
					info->soc_limit_enable);
			}
		}
retry_soc:
		schedule_delayed_work(&info->smart_soc_limit_work,
				      msecs_to_jiffies(1000));
	} else {
		mca_log_info("disable soc limit\n");
		info->soc_limit_enable = false;
		smart_charge_set_soc_limit(info->soc_limit_enable);
	}
	mca_log_info("soc_limit_enable = %d\n", info->soc_limit_enable);
}

/*to handle navigation and smart night algorithm conflict*/
static void
smartcharging_handle_algorithm_conflict(struct smart_charge_info *info)
{
	bool smart_night = false;
	bool smart_navigation = false;
	bool smart_endurance_pro = false;
	uint32_t soc_upper_thre = SMART_CHG_SOCLMT_TRIG_DEFAULT;

	smart_night = info->smart_chg_data.smart_chgcfg[SMART_CHG_NIGHT].enable;
	smart_navigation =
		info->smart_chg_data.smart_chgcfg[SMART_CHG_NAVIGATION].enable;
	smart_endurance_pro =
		info->smart_chg_data.smart_chgcfg[SMART_CHG_ENDURANCE_PRO]
			.enable;

	/*endurance protect is prior to all others, then navigation is prior to smart night charge */
	if (smart_endurance_pro) {
		soc_upper_thre = info->smart_chg_data
					 .smart_chgcfg[SMART_CHG_ENDURANCE_PRO]
					 .func_value;
		mca_log_info("smart_endurance %d, soc_upper_thre:%d",
			     smart_endurance_pro, soc_upper_thre);
	} else if (smart_navigation) {
		soc_upper_thre =
			info->smart_chg_data.smart_chgcfg[SMART_CHG_NAVIGATION]
				.func_value;
		mca_log_info("smart_navigation %d, soc_upper_thre:%d",
			     smart_navigation, soc_upper_thre);
	} else if (smart_night) {
		soc_upper_thre =
			info->smart_chg_data.smart_chgcfg[SMART_CHG_NIGHT]
				.func_value;
		mca_log_info("smart_night %d, soc_upper_thre:%d", smart_night,
			     soc_upper_thre);
	} else {
		soc_upper_thre = 0;
	}
	info->soc_limit = soc_upper_thre;
	mod_delayed_work(system_wq, &info->smart_soc_limit_work, 0);
	//smartcharging_handle_soc_limit_process(info, !!soc_upper_thre, info->soc_limit);
}

static void smartcharging_handle_wls_super(struct smart_charge_info *info)
{
	guard(srcu)(&smart_ops_srcu);
	struct mca_smartchg_if_ops *if_ops;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_wls_super_sts)
			continue;
		if_ops->set_wls_super_sts(
			if_ops->data,
			info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_SUPER]
				.enable);
	}
}

static void smartcharging_handle_power_boost(struct smart_charge_info *info,
					     int type)
{
	guard(srcu)(&smart_ops_srcu);
	struct mca_smartchg_if_ops *ops;
	bool enable =
		info->smart_chg_data.smart_chgcfg[SMART_CHG_OUTDOOR].enable ||
		info->smart_chg_data.smart_chgcfg[SMART_CHG_TRAVELWAIT].enable;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		ops = mca_smartchg_if_get_ops(i);
		if (ops && ops->set_pwr_boost_sts)
			ops->set_pwr_boost_sts(ops->data, enable);
	}
}

static void smartcharging_handle_wls_quiet(struct smart_charge_info *info)
{
	guard(srcu)(&smart_ops_srcu);
	struct mca_smartchg_if_ops *if_ops;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_wls_quiet_sts)
			continue;
		if_ops->set_wls_quiet_sts(
			if_ops->data,
			info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_QUIET]
				.enable);
	}
}

static void smartcharging_handle_controlmessage(struct smart_charge_info *info)
{
	uint32_t smart_header = info->smart_chg_control.AsUINT32;
	int soc = strategy_class_fg_ops_get_soc();

	if (!info)
		return;

	smart_header = (smart_header & 0xFFFE);
	smart_header = (smart_header - 1) & smart_header;
	mca_log_info("smart_header: %d", smart_header);
	if (!smart_header) {
		info->smart_chg_data.ret_code = false;
		mca_log_err("input correct\n");
	} else {
		info->smart_chg_data.ret_code = true;
		mca_log_err("error input invalid\n");
		return;
	}

	mca_log_info(
		"smart_chg_control: enable: %d, func_type: 0x%04x, func_value: %d\n",
		info->smart_chg_control.enable,
		info->smart_chg_control.func_type << 1,
		info->smart_chg_control.func_value);

	if (info->smart_chg_control.navigation) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_NAVIGATION].enable =
			info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_NAVIGATION]
			.func_value = info->smart_chg_control.soc_limit;
		smartcharging_handle_algorithm_conflict(info);
		if (info->smart_chg_control.enable)
			mca_charge_mievent_report(
				CHARGE_DFX_SMART_NAVIGATION_TRIGGERED, &soc, 1);
		else
			mca_charge_mievent_set_state(
				MIEVENT_STATE_END,
				CHARGE_DFX_SMART_NAVIGATION_TRIGGERED);

		info->ignore_upload.navigation =
			soc >= info->smart_chg_control.soc_limit ? true : false;

	} else if (info->smart_chg_control.endurance_pro) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_ENDURANCE_PRO]
			.enable = info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_ENDURANCE_PRO]
			.func_value = info->smart_chg_control.soc_limit;
		smartcharging_handle_algorithm_conflict(info);
		if (info->smart_chg_control.enable)
			mca_charge_mievent_report(
				CHARGE_DFX_SMART_ENDURANCE_TRIGGERED, &soc, 1);
		else
			mca_charge_mievent_set_state(
				MIEVENT_STATE_END,
				CHARGE_DFX_SMART_ENDURANCE_TRIGGERED);

		info->ignore_upload.endurance_pro =
			soc >= info->smart_chg_control.soc_limit ? true : false;

	} else if (info->smart_chg_control.outdoor) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_OUTDOOR].enable =
			info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_OUTDOOR].func_value =
			0;
		smartcharging_handle_power_boost(info,
						 SMART_CHG_OUTDOOR_PWR_BOOST);
	} else if (info->smart_chg_control.wls_super_chg) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_SUPER].enable =
			info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_SUPER]
			.func_value = 0;
		smartcharging_handle_wls_super(info);
	} else if (info->smart_chg_control.lowfast) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_LOWFAST].enable =
			info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_LOWFAST].func_value =
			0;
	} else if (info->smart_chg_control.wls_quiet) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_QUIET].enable =
			info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_QUIET]
			.func_value = 0;
		smartcharging_handle_wls_quiet(info);
	} else if (info->smart_chg_control.sense_chg) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG].enable =
			info->smart_chg_control.enable;
		if (info->smart_chg_control.func_value >
		    SMART_CHG_POSTURE_FAKE_CLEAR) {
			mca_log_info("debug sense charge, set value: %d\n",
				     info->smart_chg_control.func_value);
			info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.use_fake_value = true;
			info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.fake_value =
				info->smart_chg_control.func_value -
				SMART_CHG_POSTURE_FAKE_CLEAR;
		} else if (info->smart_chg_control.func_value ==
			   SMART_CHG_POSTURE_FAKE_CLEAR) {
			mca_log_info(
				"debug sense charge, cancel fake value: %d\n",
				info->smart_chg_control.func_value);
			info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.use_fake_value = false;
			info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.fake_value = 0;
		} else {
			info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				.func_value =
				info->smart_chg_control.func_value;
		}
		smart_charge_handle_sense_chg(info);
	} else if (info->smart_chg_control.extreme_cold) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_EXTREME_COLD]
			.enable = info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_EXTREME_COLD]
			.func_value = 0;
	} else if (info->smart_chg_control.travel_wait) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_TRAVELWAIT].enable =
			info->smart_chg_control.enable;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_TRAVELWAIT]
			.func_value = 0;
		smartcharging_handle_power_boost(
			info, SMART_CHG_TRAVELWAIT_PWR_BOOST);
	} else if (info->smart_chg_control.smart_bypass) {
		info->bypass_reserved = 0;
		info->bypass_enable = info->smart_chg_control.enable & 1;
	}
}

static int smartcharging_receive_control_message(struct smart_charge_info *info,
						 int val)
{
	u32 word = val;
	u32 feature = word & GENMASK(15, 1);
	u32 value = word >> 16;

	if (!is_power_of_2(feature) || feature & ~GENMASK(10, 1))
		return -EINVAL;
	if ((feature & (BIT(1) | BIT(4))) && (word & BIT(0)) &&
	    (!value || value > 100))
		return -ERANGE;
	if (feature == BIT(6) && !(value < SMART_CHG_POSTURE_INDEX_MAX ||
				   (value >= SMART_CHG_POSTURE_FAKE_CLEAR &&
				    value < SMART_CHG_POSTURE_FAKE_INDEX_MAX)))
		return -ERANGE;
	if (feature == BIT(10) && !info->support_bypass)
		return -EOPNOTSUPP;
	info->smart_chg_control.AsUINT32 = word;
	smartcharging_handle_controlmessage(info);
	return info->smart_chg_data.ret_code ? -EINVAL : 0;
}

static int get_smart_chg_message(struct smart_charge_info *info)
{
	uint32_t control_header = 0;
	int index = 0;

	control_header |= (info->smart_chg_data.ret_code & 0x01);

	for (index = SMART_CHG_NAVIGATION; index < SMART_CHG_MAX; index++) {
		if (info->smart_chg_data.smart_chgcfg[index].enable)
			control_header |= BIT(index);
		else
			control_header &= ~BIT(index);
	}
	mca_log_info("control_header = %d\n", control_header);
	return control_header;
}

static int handle_smart_batt_message(struct smart_charge_info *info, int val)
{
	int vote_value, ret;

	if (val < 0 || check_mul_overflow(val, info->cell_type, &vote_value))
		return -ERANGE;
	ret = mca_vote(info->smartchg_delta_fv_voter, "smart_batt", true,
		       vote_value);
	if (!ret)
		info->smart_batt_val = val;
	return ret;
}

static int get_smart_batt_message(struct smart_charge_info *info)
{
	return info->smart_batt_val;
}

static int handle_smart_night_message(struct smart_charge_info *info, int val)
{
	if (val != 0 && val != 1)
		return -EINVAL;
	info->smart_night_val = val;
	mca_log_info("smart_night_val is %d\n", !!val);
	if (val)
		strategy_class_fg_ops_get_rsoc(&info->night_enable_rsoc);
	if (!!val == 1 || !!val == 0) {
		info->smart_chg_data.smart_chgcfg[SMART_CHG_NIGHT].enable = val;
		info->smart_chg_data.smart_chgcfg[SMART_CHG_NIGHT].func_value =
			SMART_CHG_SOCLMT_TRIG_DEFAULT;
		smartcharging_handle_algorithm_conflict(info);
	}

	return 0;
}

static int get_smart_night_message(struct smart_charge_info *info)
{
	return info->smart_night_val;
}

static void smart_charge_plugin_or_plugout_reset(struct smart_charge_info *info)
{
	if (info->online)
		mca_vote(info->smartchg_delta_ichg_voter, "batt_health", true,
			 info->smart_delta_ichg);
	else
		mca_vote(info->smartchg_delta_ichg_voter, "batt_health", false,
			 info->smart_delta_ichg);
}

static void smart_charge_csd_send_pulse(struct smart_charge_info *info)
{
	int soc = 0, rsoc = 0;
	int batt_temp = 0;
	bool smart_night = false;

	soc = strategy_class_fg_ops_get_soc();
	if (soc < 0 || strategy_class_fg_ops_get_rsoc(&rsoc) ||
	    strategy_class_fg_ops_get_temperature(&batt_temp))
		return;
	batt_temp /= 10;
	smart_night = info->smart_chg_data.smart_chgcfg[SMART_CHG_NIGHT].enable;

	if (!info->pulse_mode) {
		if ((soc >= 50 && soc <= 80) &&
		    (batt_temp >= 20 && batt_temp <= 45)) {
			if (info->plugin_rsoc <= 45 && !info->screen_state &&
			    info->cycle_count > 100 &&
			    (info->cycle_count % 25 == 0) && rsoc == 50) {
				info->pulse_mode = 1;
				mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
						       MCA_EVENT_CSD_SEND_PULSE,
						       &info->pulse_mode);
			} else if (smart_night == true) {
				if (info->night_enable_rsoc <= 45 &&
				    rsoc == 50) {
					info->pulse_mode = 2;
					mca_event_block_notify(
						MCA_EVENT_CHARGE_STATUS,
						MCA_EVENT_CSD_SEND_PULSE,
						&info->pulse_mode);
				} else if (info->night_enable_rsoc > 45 &&
					   rsoc == 76) {
					info->pulse_mode = 3;
					mca_event_block_notify(
						MCA_EVENT_CHARGE_STATUS,
						MCA_EVENT_CSD_SEND_PULSE,
						&info->pulse_mode);
				}
			}
		}
	} else if ((info->cycle_count % 25 != 0) && smart_night == false) {
		info->pulse_mode = 0;
		mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
				       MCA_EVENT_CSD_SEND_PULSE,
				       &info->pulse_mode);
	}
	mca_log_info(
		"soc: %d, rsoc: %d, plugin_rsoc: %d, night_enable_rsoc: %d, batt_temp: %d, cycle: %d, night: %d, pulse_mode: %d",
		soc, rsoc, info->plugin_rsoc, info->night_enable_rsoc,
		batt_temp, info->cycle_count, smart_night, info->pulse_mode);
}

static int smart_charge_thermal_notifier_cb(struct notifier_block *nb,
					    unsigned long event, void *val)
{
	struct smart_charge_info *info =
		container_of(nb, struct smart_charge_info, thermal_nb);

	guard(mutex)(&info->state_lock);
	if (info->stopping || !val)
		return NOTIFY_DONE;

	switch (event) {
	case MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE:
		WRITE_ONCE(info->board_temp,
			   *(int *)val / SMART_CHG_BOARD_TEMP_SCALE);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static void
update_smart_bypass_temp_section(struct smart_charge_info *info,
				 struct smart_bypass_temp_section *sections,
				 unsigned int count)
{
	int board_temp = info->board_temp;
	int cur = info->bypass_temp_index;
	int report_index;
	int i;

	if (!count || count > SMART_BYPASS_TEMP_SECTION_MAX)
		return;
	if (cur < 0 || cur >= count)
		cur = info->bypass_temp_index = 0;
	for (i = 0; i < (int)count; i++) {
		if (board_temp >= sections[i].temp_high ||
		    sections[i].temp_low > board_temp)
			continue;

		report_index = cur;
		if (cur < i) {
			if (board_temp >=
			    sections[cur].temp_high + sections[cur].hyst_high) {
				report_index = i;
				info->bypass_temp_index = i;
			}
		} else if (cur > i && (s64)sections[cur].temp_low -
						      sections[cur].hyst_low >
					      board_temp) {
			report_index = i;
			info->bypass_temp_index = i;
		}

		mca_log_err("for_bypass board_temp:%d, cur_temp_index:%d\n",
			    board_temp, report_index);
		return;
	}

	mca_log_err("temp over range invalid, current:%d\n", -1);
}

static void smart_charge_check_bypass_status(struct smart_charge_info *info)
{
	bool prev = info->bypass_once;

	if (info->bypass_exit_flag) {
		info->bypass_once = false;
		return;
	}

	if (!info->bypass_once) {
		mca_log_err("for_bypass start time\n");
		info->bypass_start_time = ktime_get_boottime();
	} else {
		if (ktime_get_boottime() >
		    info->bypass_start_time + SMART_BYPASS_DEBOUNCE_NS) {
			mca_log_err("for_bypass exit bypass\n");
			mca_vote(info->smartchg_set_fcc_voter, "smart_bypass",
				 false, 0);
			info->bypass_exit_flag = true;
		} else {
			goto debounce;
		}
	}
	info->bypass_once = prev ^ 1;
debounce:
	mca_log_err("for_bypass Debounce: %lld s\n",
		    (ktime_get_boottime() - info->bypass_start_time) /
			    NSEC_PER_SEC);
}

static const int smart_bypass_med_scenes[] = { 0x08, 0x0b, 0x1c,
					       0x3a, 0x3d, 0x4e };
static const int smart_bypass_high_scenes[] = { 0x12, 0x13,  0x14,  0x19,
						0x4b, 0x1f5, 0x2be, 0x2bc };

static bool smart_bypass_scene_in_set(int scene, const int *set, int n)
{
	for (int i = 0; i < n; i++)
		if (set[i] == scene)
			return true;
	return false;
}

static void smart_charge_handle_bypass_chg(struct smart_charge_info *info)
{
	int fcc = 0;
	uint16_t state;
	int soc;

	if (!info->support_bypass)
		return;

	state = info->bypass_enable;
	if (info->bypass_enable == 0)
		goto report;

	soc = strategy_class_fg_ops_get_soc();
	if (soc < info->bypass_entry_soc || soc > info->bypass_exit_soc) {
		mca_log_err("for_bypass soc not in range, quit bypass mode\n");
		state = 0;
		goto report;
	}
	if (info->wls_online) {
		mca_log_err("for_bypass wlschg online, quit bypass mode\n");
		state = 0;
		goto report;
	}

	if (smart_bypass_scene_in_set(info->scene, smart_bypass_med_scenes,
				      ARRAY_SIZE(smart_bypass_med_scenes))) {
		update_smart_bypass_temp_section(info, info->bypass_med_lmt,
						 info->bypass_med_num);
		info->bypass_active = 1;
		fcc = info->bypass_med_lmt[info->bypass_temp_index].fcc;
	} else if (smart_bypass_scene_in_set(
			   info->scene, smart_bypass_high_scenes,
			   ARRAY_SIZE(smart_bypass_high_scenes))) {
		update_smart_bypass_temp_section(info, info->bypass_high_lmt,
						 info->bypass_high_num);
		info->bypass_active = 1;
		fcc = info->bypass_high_lmt[info->bypass_temp_index].fcc;
	} else {
		info->bypass_active = 0;
		smart_charge_check_bypass_status(info);
	}

	if (fcc != 0 && fcc != info->last_bypass_fcc) {
		mca_vote(info->smartchg_set_fcc_voter, "smart_bypass", true,
			 fcc);
		info->bypass_exit_flag = false;
	}

report:
	mca_log_err("for_bypass [%d->%d] fcc[%d %d]\n", info->last_bypass_state,
		    state, fcc, info->last_bypass_fcc);
	if (info->last_bypass_state != state) {
		info->last_bypass_state = state;
		if (state == 0)
			mca_vote(info->smartchg_set_fcc_voter, "smart_bypass",
				 false, 0);
	}
	if (fcc != info->last_bypass_fcc)
		info->last_bypass_fcc = fcc;
}

static void smart_charge_handle_mishow_config(struct smart_charge_info *info)
{
	const struct mca_hwid *hwid = mca_get_hwid_info();
	bool mishow = false;

	if (!hwid || mca_log_get_charge_boot_mode() == 0)
		return;
	if (hwid->platform_version != HARDWARE_PROJECT_O8 &&
	    hwid->platform_version != HARDWARE_PROJECT_O9)
		return;

	charger_partition_get_mishow(&mishow);
	info->mishow_config = mishow;
	mca_log_info("mishow config is %d\n", info->mishow_config);
	if (info->mishow_config)
		smartcharging_handle_algorithm_conflict(info);
}

static void smart_charge_workfunc(struct work_struct *work)
{
	struct smart_charge_info *info = container_of(
		work, struct smart_charge_info, smart_charge_work.work);
	int delta_ichg = 0;
	const struct mca_hwid *hwid = mca_get_hwid_info();

	guard(mutex)(&info->state_lock);
	if (info->stopping)
		return;

	if (atomic_xchg(&info->replay_needed, 0))
		smart_charge_replay_votes(info);
	if (!info->online) {
		mca_log_err("adapter is plugout\n");
		return;
	}
	//battery health4.0 cyclecount decrease ichg
	if (!info->support_cc_ichg ||
	    strategy_class_fg_ops_get_cyclecount(&info->cycle_count) ||
	    info->cycle_count < 0)
		goto other_policy;
	delta_ichg = info->ichg_cc_table[info->ichg_cc_count - 1].delta_ichg;
	for (int i = 0; i < info->ichg_cc_count; i++) {
		if (info->cycle_count <= info->ichg_cc_table[i].cc_max) {
			delta_ichg = info->ichg_cc_table[i].delta_ichg;
			break;
		}
	}

	if (delta_ichg != info->smart_delta_ichg) {
		info->smart_delta_ichg = delta_ichg;
		mca_vote(info->smartchg_delta_ichg_voter, "batt_health", true,
			 delta_ichg);
		mca_log_info("cycle_count %d smart_delta_ichg %d\n",
			     info->cycle_count, info->smart_delta_ichg);
	}
other_policy:
	//csd1.0 feature:check battery swelling
	if (hwid && hwid->country_version == CountryCN && info->support_csd)
		smart_charge_csd_send_pulse(info);

	//smart bypass fcc limiter (mishow)
	smart_charge_handle_bypass_chg(info);
	smart_charge_handle_mishow_config(info);

	schedule_delayed_work(&info->smart_charge_work, msecs_to_jiffies(5000));
}

static void smart_charge_sense_chg_workfunc(struct work_struct *work)
{
	struct smart_charge_info *info = container_of(
		work, struct smart_charge_info, smart_sense_chg_work.work);

	guard(mutex)(&info->state_lock);
	if (info->stopping)
		return;

	if (!info)
		return;

	smart_charge_handle_sense_chg(info);
}

static int mca_smart_charge_process_event(int event, int value, void *data)
{
	struct smart_charge_info *info = data;

	if (!info)
		return -EINVAL;
	guard(mutex)(&info->state_lock);
	if (info->stopping)
		return -ENODEV;

	if (!info)
		return -1;

	mca_log_info("receive event %d, value %d\n", event, value);
	switch (event) {
	case MCA_EVENT_WIRELESS_CONNECT:
	case MCA_EVENT_USB_CONNECT:
	case MCA_EVENT_WIRELESS_DISCONNECT:
	case MCA_EVENT_USB_DISCONNECT:
		if (event == MCA_EVENT_USB_CONNECT ||
		    event == MCA_EVENT_USB_DISCONNECT)
			info->usb_online = event == MCA_EVENT_USB_CONNECT;
		else
			info->wls_online = event == MCA_EVENT_WIRELESS_CONNECT;
		info->online = info->usb_online || info->wls_online;
		smart_charge_plugin_or_plugout_reset(info);
		if (info->online) {
			strategy_class_fg_ops_get_rsoc(&info->plugin_rsoc);
			mod_delayed_work(system_wq, &info->smart_charge_work,
					 0);
			mod_delayed_work(system_wq, &info->smart_soc_limit_work,
					 0);
		} else {
			cancel_delayed_work(&info->smart_charge_work);
			cancel_delayed_work(&info->smart_soc_limit_work);
			memset(&info->smart_chg_control, 0,
			       sizeof(info->smart_chg_control));
			info->soc_limit_enable = false;
			info->ignore_upload.AsUINT32 = 0;
			smart_charge_set_soc_limit(false);
		}
		break;
	case MCA_EVENT_BATTERY_DTPT:
		if (value)
			mca_vote(info->smartchg_delta_fv_voter, "dtpt", true,
				 SMART_CHARGE_DTPT_DELTA_VALUE);
		else
			mca_vote(info->smartchg_delta_fv_voter, "dtpt", false,
				 0);
		break;
	default:
		break;
	}

	return 0;
}

static int smart_charge_fv_control(struct mca_votable *votable, void *data,
				   int effective_result,
				   const char *effective_client)
{
	guard(srcu)(&smart_ops_srcu);
	struct mca_smartchg_if_ops *ops;
	struct smart_charge_info *info = data;
	int ret = 0, error;

	if (!data)
		return -EINVAL;
	WRITE_ONCE(info->delta_fv, effective_result);
	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		ops = mca_smartchg_if_get_ops(i);
		if (!ops || !ops->set_delta_fv)
			continue;
		error = ops->set_delta_fv(ops->data, effective_result);
		if (!ret)
			ret = error;
	}
	return ret;
}

static int smart_charge_ichg_control(struct mca_votable *votable, void *data,
				     int effective_result,
				     const char *effective_client)
{
	guard(srcu)(&smart_ops_srcu);
	struct mca_smartchg_if_ops *ops;
	struct smart_charge_info *info = data;
	int ret = 0, error;

	if (!data)
		return -EINVAL;
	WRITE_ONCE(info->delta_ichg, effective_result);
	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		ops = mca_smartchg_if_get_ops(i);
		if (!ops || !ops->set_delta_ichg)
			continue;
		error = ops->set_delta_ichg(ops->data, effective_result);
		if (!ret)
			ret = error;
	}
	return ret;
}

static int smart_charge_set_fcc_control(struct mca_votable *votable, void *data,
					int effective_result,
					const char *effective_client)
{
	guard(srcu)(&smart_ops_srcu);
	struct mca_smartchg_if_ops *ops;
	int ret = 0, error;

	if (!data)
		return -EINVAL;
	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		ops = mca_smartchg_if_get_ops(i);
		if (!ops || !ops->set_fcc)
			continue;
		error = ops->set_fcc(ops->data, effective_result);
		if (!ret)
			ret = error;
	}
	return ret;
}

static void smart_charge_replay_votes(struct smart_charge_info *info)
{
	mca_rerun_election(info->smartchg_delta_fv_voter);
	mca_rerun_election(info->smartchg_delta_ichg_voter);
	mca_rerun_election(info->smartchg_set_fcc_voter);
	smart_charge_set_soc_limit(info->soc_limit_enable);
	smartcharging_handle_wls_super(info);
	smartcharging_handle_wls_quiet(info);
	smartcharging_handle_power_boost(info, SMART_CHG_OUTDOOR_PWR_BOOST);
}

static int smart_charge_init_voter(struct smart_charge_info *info)
{
	int ret;

	info->smartchg_delta_fv_voter = mca_create_votable(
		"smartchg_delta_fv", MCA_VOTE_MAX, smart_charge_fv_control,
		SMART_CHARGE_DELTA_FV_DEFAULT_VALUE, info);
	if (IS_ERR(info->smartchg_delta_fv_voter))
		return PTR_ERR(info->smartchg_delta_fv_voter);
	info->smartchg_delta_ichg_voter = mca_create_votable(
		"smartchg_delta_ichg", MCA_VOTE_MAX, smart_charge_ichg_control,
		SMART_CHARGE_DELTA_ICHG_DEFAULT_VALUE, info);
	if (IS_ERR(info->smartchg_delta_ichg_voter)) {
		ret = PTR_ERR(info->smartchg_delta_ichg_voter);
		goto free_fv;
	}
	info->smartchg_set_fcc_voter =
		mca_create_votable("smartchg_set_fcc", MCA_VOTE_MIN,
				   smart_charge_set_fcc_control, 0, info);
	if (IS_ERR(info->smartchg_set_fcc_voter)) {
		ret = PTR_ERR(info->smartchg_set_fcc_voter);
		mca_destroy_votable(info->smartchg_delta_ichg_voter);
		goto free_fv;
	}
	return 0;
free_fv:
	mca_destroy_votable(info->smartchg_delta_fv_voter);
	return ret;
}

static void smart_charge_destroy_voters(struct smart_charge_info *info)
{
	mca_destroy_votable(info->smartchg_set_fcc_voter);
	mca_destroy_votable(info->smartchg_delta_ichg_voter);
	mca_destroy_votable(info->smartchg_delta_fv_voter);
}

static void smart_charge_stop_work(struct smart_charge_info *info)
{
	mutex_lock(&info->state_lock);
	info->stopping = true;
	mutex_unlock(&info->state_lock);
	disable_delayed_work_sync(&info->smart_charge_work);
	disable_delayed_work_sync(&info->smart_sense_chg_work);
	disable_delayed_work_sync(&info->smart_soc_limit_work);
}

static ssize_t smart_charge_sysfs_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct mca_sysfs_attr_info *attr_info;
	ssize_t count = 0;
	struct smart_charge_info *info = dev_get_drvdata(dev);
	int val = 0;

	guard(mutex)(&info->state_lock);
	if (info->stopping)
		return -ENODEV;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  smartchg_sysfs_field_tbl,
					  SMARTCHG_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	if (!info) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	switch (attr_info->sysfs_attr_name) {
	case MCA_PROP_SMARTCHG:
		val = get_smart_chg_message(info);
		count = scnprintf(buf, PAGE_SIZE, "%d\n", val);
		break;
	case MCA_PROP_SMARTBATT:
		val = get_smart_batt_message(info);
		count = scnprintf(buf, PAGE_SIZE, "%d\n", val);
		break;
	case MCA_PROP_SMARTNIGHT:
		val = get_smart_night_message(info);
		count = scnprintf(buf, PAGE_SIZE, "%d\n", val);
		break;
	case MCA_PROP_SMARTCHG_FV:
		val = mca_get_client_vote(info->smartchg_delta_fv_voter,
					  "smart_chg");
		val = val < 0 ? 0 : val;
		count = scnprintf(buf, PAGE_SIZE, "%d\n", val);
		break;
	case MCA_PROP_SMARTCHG_ICHG:
		val = mca_get_client_vote(info->smartchg_delta_ichg_voter,
					  "smart_chg");
		val = val < 0 ? 0 : val;
		count = scnprintf(buf, PAGE_SIZE, "%d\n", val);
		break;
	case MCA_PROP_POSTURE:
		val = info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG]
				      .use_fake_value ?
			      info->smart_chg_data
				      .smart_chgcfg[SMART_CHG_SENSE_CHG]
				      .fake_value :
			      info->smart_chg_data
				      .smart_chgcfg[SMART_CHG_SENSE_CHG]
				      .func_value;
		count = scnprintf(buf, PAGE_SIZE, "%d\n", val);
		break;
	case MCA_PROP_SCENE:
		val = mca_smartchg_get_scene();
		count = scnprintf(buf, PAGE_SIZE, "%d\n", val);
		break;
	case MCA_PROP_BOARD_TEMP:
		count = scnprintf(buf, PAGE_SIZE, "%d\n", info->board_temp);
		break;
	case MCA_PROP_SMART_SIC_MODE:
		count = scnprintf(buf, PAGE_SIZE, "%d\n", info->smart_sic_mode);
		break;
	default:
		break;
	}

	return count;
}

static ssize_t smart_charge_sysfs_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *attr_info;
	struct smart_charge_info *info = dev_get_drvdata(dev);
	int val, ret = 0;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  smartchg_sysfs_field_tbl,
					  SMARTCHG_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	if (!info) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	if (attr_info->sysfs_attr_name == MCA_PROP_SCENE) {
		if (!((val >= 0 && val < SMART_CHG_SCENE_INDEX_MAX) ||
		      (val >= SMART_CHG_SCENE_REDIR_START &&
		       val < SMART_CHG_SCENE_REDIR_INDEX_MAX) ||
		      val == SMART_CHARGE_FAKE_SCENE_DEFAULT))
			return -EINVAL;
		mca_smartchg_set_scene(val);
		return count;
	}
	guard(mutex)(&info->state_lock);
	if (info->stopping)
		return -ENODEV;
	switch (attr_info->sysfs_attr_name) {
	case MCA_PROP_SMARTCHG:
		ret = smartcharging_receive_control_message(info, val);
		break;
	case MCA_PROP_SMARTBATT:
		ret = handle_smart_batt_message(info, val);
		break;
	case MCA_PROP_SMARTNIGHT:
		ret = handle_smart_night_message(info, val);
		break;
	case MCA_PROP_SMARTCHG_FV:
		if (val < 0 || check_mul_overflow(val, info->cell_type, &val))
			return -ERANGE;
		ret = mca_vote(info->smartchg_delta_fv_voter, "smart_chg", true,
			       val);
		break;
	case MCA_PROP_SMARTCHG_ICHG:
		if (val < 0)
			return -ERANGE;
		ret = mca_vote(info->smartchg_delta_ichg_voter, "smart_chg",
			       true, val);
		break;
	case MCA_PROP_BOARD_TEMP:
		if (val < -273 || val > 1000)
			return -ERANGE;
		mca_smartchg_set_board_temp(val);
		break;
	default:
		return -EOPNOTSUPP;
	}

	mca_log_info("set the %d value = %d\n", attr_info->sysfs_attr_name,
		     val);

	return ret ? ret : count;
}

static int smart_charge_sysfs_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(smartchg_sysfs_attrs, smartchg_sysfs_field_tbl,
			     SMARTCHG_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group("charger", "smart_charge", dev,
					   &smartchg_sysfs_attr_group);
}

static void smart_charge_sysfs_remove_group(struct device *dev)
{
	mca_sysfs_remove_link_group("charger", "smart_charge", dev,
				    &smartchg_sysfs_attr_group);
}

/* Stock producer ABI and exact field offsets are recorded in the handoff. */
struct smart_basp_record {
	struct smart_batt_spec spec;
	struct smart_batt_spec_curve steps[MCA_BASP_MAX_STEPS];
};

struct smart_basp_decoded {
	struct mca_basp_update update[3];
	struct smart_batt_jeita_term_para *jeita;
	struct smart_basp_record *records;
	struct smart_batt_spec *specs;
};

struct smart_basp_transport {
	struct kref ref;
	/* Serializes detach, file acquisition and policy commits. */
	struct mutex lock;
	struct smart_charge_info *info;
	bool opened;
};

struct smart_basp_file {
	struct kref ref;
	struct smart_basp_transport *transport;
	struct page *page;
	/* Serializes ioctl and mmap setup for one open file description. */
	struct mutex lock;
	bool mapped;
	bool committed;
};

static void smart_basp_free_decoded(struct smart_basp_decoded *decoded)
{
	kfree(decoded->specs);
	kfree(decoded->records);
	kfree(decoded->jeita);
	memset(decoded, 0, sizeof(*decoded));
}

static int smart_basp_decode(const u8 *bytes, size_t size,
			     struct smart_basp_decoded *decoded)
{
	u32 count[6], total, checksum = 0, jeita_count, record_count;
	size_t offset = MCA_BASP_HEADER_SIZE;
	u32 i, j, cursor = 0;
	int ret = -EINVAL;

	memset(decoded, 0, sizeof(*decoded));
	if (!bytes || size < MCA_BASP_HEADER_SIZE || size > MCA_BASP_MAX_SIZE)
		return -EINVAL;
	total = get_unaligned_le32(bytes + 8);
	if (total < MCA_BASP_HEADER_SIZE || total > size || total % sizeof(u32))
		return -EINVAL;
	if (get_unaligned_le32(bytes + 4))
		return -EOPNOTSUPP;
	for (i = sizeof(u32); i < total; i++)
		checksum += bytes[i];
	if (checksum != get_unaligned_le32(bytes))
		return -EBADMSG;
	for (i = 0; i < ARRAY_SIZE(count); i++) {
		count[i] = get_unaligned_le32(bytes + 12 + i * sizeof(u32));
		if (count[i] > MCA_BASP_MAX_SIZE / 20)
			return -EINVAL;
	}
	jeita_count = count[0] + count[1];
	record_count = count[2] + count[3] + count[4] + count[5];
	if (jeita_count > (total - offset) / 20)
		return -EINVAL;
	if (record_count > (total - offset - jeita_count * 20) / 24)
		return -EINVAL;
	if (jeita_count) {
		decoded->jeita = kcalloc(jeita_count, sizeof(*decoded->jeita), GFP_KERNEL);
		if (!decoded->jeita)
			return -ENOMEM;
	}
	if (record_count) {
		decoded->records = kcalloc(record_count, sizeof(*decoded->records), GFP_KERNEL);
		decoded->specs = kcalloc(record_count, sizeof(*decoded->specs), GFP_KERNEL);
		if (!decoded->records || !decoded->specs) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	decoded->update[0].jeita = decoded->jeita;
	decoded->update[0].ffc_size = count[0];
	decoded->update[0].normal_size = count[1];
	for (i = 0; i < jeita_count; i++, offset += 20) {
		struct smart_batt_jeita_term_para *para = &decoded->jeita[i];

		para->t_range.idx = (s32)get_unaligned_le32(bytes + offset);
		para->t_range.min = (s32)get_unaligned_le32(bytes + offset + 4);
		para->t_range.max = (s32)get_unaligned_le32(bytes + offset + 8);
		para->vterm = (s32)get_unaligned_le32(bytes + offset + 12);
		para->iterm = (s32)get_unaligned_le32(bytes + offset + 16);
		if (para->t_range.idx < 0 || para->t_range.min >= para->t_range.max ||
		    para->vterm < 0 || para->iterm < 0)
			goto fail;
		for (j = i < count[0] ? 0 : count[0]; j < i; j++)
			if (decoded->jeita[j].t_range.idx == para->t_range.idx)
				goto fail;
	}
	for (i = 2; i < ARRAY_SIZE(count); i++) {
		u32 first = cursor;

		for (j = 0; j < count[i]; j++, cursor++) {
			struct smart_basp_record *record = &decoded->records[cursor];
			struct smart_batt_spec *spec = &record->spec;
			u32 k;

			if (total - offset < 24)
				goto fail;
			spec->type = get_unaligned_le32(bytes + offset);
			spec->ffc = get_unaligned_le32(bytes + offset + 4);
			spec->t_range.idx = (s32)get_unaligned_le32(bytes + offset + 8);
			spec->t_range.min = (s32)get_unaligned_le32(bytes + offset + 12);
			spec->t_range.max = (s32)get_unaligned_le32(bytes + offset + 16);
			spec->step_size = get_unaligned_le32(bytes + offset + 20);
			offset += 24;
			if (spec->type != (i < 4) || spec->ffc != !(i % 2) ||
			    spec->t_range.idx < 0 || spec->t_range.min >= spec->t_range.max ||
			    !spec->step_size || spec->step_size > MCA_BASP_MAX_STEPS ||
			    spec->step_size > (total - offset) / 12)
				goto fail;
			for (k = first; k < cursor; k++)
				if (decoded->records[k].spec.t_range.idx == spec->t_range.idx)
					goto fail;
			for (k = 0; k < spec->step_size; k++, offset += 12) {
				struct smart_batt_spec_curve *step = &record->steps[k];

				step->mv = (s32)get_unaligned_le32(bytes + offset);
				step->ma_h = (s32)get_unaligned_le32(bytes + offset + 4);
				step->ma_l = (s32)get_unaligned_le32(bytes + offset + 8);
				if (step->mv < 0 || step->ma_l < 0 || step->ma_h < step->ma_l ||
				    (k && step->mv <= record->steps[k - 1].mv))
					goto fail;
			}
			spec->steps = record->steps;
			decoded->specs[cursor] = *spec;
		}
	}
	decoded->update[1].spec = decoded->specs;
	decoded->update[1].ffc_size = count[2];
	decoded->update[1].normal_size = count[3];
	if (decoded->specs)
		decoded->update[2].spec = decoded->specs + count[2] + count[3];
	decoded->update[2].ffc_size = count[4];
	decoded->update[2].normal_size = count[5];
	if (offset != total)
		goto fail;
	return 0;
fail:
	smart_basp_free_decoded(decoded);
	return ret;
}

static int smart_basp_dispatch(struct smart_basp_transport *transport,
			       const struct smart_basp_decoded *decoded)
{
	static const unsigned int types[] = {
		MCA_SMARTCHG_IF_CHG_TYPE_JEITA,
		MCA_SMARTCHG_IF_CHG_TYPE_QC,
		MCA_SMARTCHG_IF_CHG_TYPE_WL_QC,
	};
	struct mca_smartchg_if_ops *prepared[ARRAY_SIZE(types)] = { };
	struct smart_charge_info *info;
	unsigned int i;
	int ret = 0;

	guard(mutex)(&transport->lock);
	info = transport->info;
	if (!info)
		return -ENODEV;
	guard(mutex)(&info->state_lock);
	if (info->stopping || !info->enable_fv_dec_by_cc)
		return -ENODEV;
	guard(srcu)(&smart_ops_srcu);
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		const struct mca_basp_update *update = &decoded->update[i];
		struct mca_smartchg_if_ops *ops;

		if (!update->ffc_size && !update->normal_size)
			continue;
		ops = mca_smartchg_if_get_ops(types[i]);
		if (!ops || !ops->prepare_baa_para || !ops->commit_baa_para ||
		    !ops->abort_baa_para) {
			ret = -ENODEV;
			goto abort;
		}
		ret = ops->prepare_baa_para(ops->data, update);
		if (ret)
			goto abort;
		prepared[i] = ops;
	}
	for (i = 0; i < ARRAY_SIZE(prepared); i++)
		if (prepared[i])
			prepared[i]->commit_baa_para(prepared[i]->data);
	return 0;
abort:
	for (i = ARRAY_SIZE(prepared); i-- > 0;)
		if (prepared[i])
			prepared[i]->abort_baa_para(prepared[i]->data);
	return ret;
}

static int smart_basp_commit(struct smart_basp_file *ctx, const u8 *bytes, size_t size)
{
	struct smart_basp_decoded decoded;
	int ret;

	ret = smart_basp_decode(bytes, size, &decoded);
	if (ret)
		return ret;
	ret = smart_basp_dispatch(ctx->transport, &decoded);
	smart_basp_free_decoded(&decoded);
	return ret;
}

static void smart_basp_transport_free(struct kref *ref)
{
	struct smart_basp_transport *transport =
		container_of(ref, struct smart_basp_transport, ref);

	kfree(transport);
}

static void smart_basp_detach(struct smart_charge_info *info)
{
	struct smart_basp_transport *transport = info->basp;

	if (!transport)
		return;
	mutex_lock(&transport->lock);
	transport->info = NULL;
	mutex_unlock(&transport->lock);
	info->basp = NULL;
	kref_put(&transport->ref, smart_basp_transport_free);
}

static void smart_basp_file_free(struct kref *ref)
{
	struct smart_basp_file *ctx = container_of(ref, struct smart_basp_file, ref);

	__free_page(ctx->page);
	kref_put(&ctx->transport->ref, smart_basp_transport_free);
	kfree(ctx);
}

static int smart_charge_file_open(struct inode *inode, struct file *filp)
{
	struct smart_charge_info *info =
		container_of(filp->private_data, struct smart_charge_info, misc);
	struct smart_basp_transport *transport = info->basp;
	struct smart_basp_file *ctx;
	int ret = -ENODEV;

	/* misc_open holds misc_mtx until this callback returns. */
	if (!transport)
		return -ENODEV;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!ctx->page) {
		kfree(ctx);
		return -ENOMEM;
	}
	mutex_lock(&transport->lock);
	if (!transport->info || rcu_access_pointer(global_smartchg_info) != info ||
	    !info->enable_fv_dec_by_cc)
		goto unlock;
	if (transport->opened) {
		ret = -EBUSY;
		goto unlock;
	}
	transport->opened = true;
	kref_get(&transport->ref);
	ctx->transport = transport;
	kref_init(&ctx->ref);
	mutex_init(&ctx->lock);
	filp->private_data = ctx;
	ret = 0;
unlock:
	mutex_unlock(&transport->lock);
	if (ret) {
		__free_page(ctx->page);
		kfree(ctx);
	}
	return ret;
}

static int smart_charge_file_close(struct inode *inode, struct file *filp)
{
	struct smart_basp_file *ctx = filp->private_data;
	u8 *snapshot;
	int ret = 0;

	if (ctx->mapped && !ctx->committed) {
		snapshot = kmemdup(page_address(ctx->page), MCA_BASP_MAX_SIZE, GFP_KERNEL);
		if (!snapshot)
			ret = -ENOMEM;
		else {
			ret = smart_basp_commit(ctx, snapshot, MCA_BASP_MAX_SIZE);
			kfree(snapshot);
		}
		if (ret)
			pr_err("mca_smart_charge: BASP release commit failed: %d\n", ret);
	}
	mutex_lock(&ctx->transport->lock);
	ctx->transport->opened = false;
	mutex_unlock(&ctx->transport->lock);
	kref_put(&ctx->ref, smart_basp_file_free);
	return ret;
}

static void smart_basp_vma_open(struct vm_area_struct *vma)
{
	struct smart_basp_file *ctx = vma->vm_private_data;

	__module_get(THIS_MODULE);
	kref_get(&ctx->ref);
}

static void smart_basp_vma_close(struct vm_area_struct *vma)
{
	struct smart_basp_file *ctx = vma->vm_private_data;

	kref_put(&ctx->ref, smart_basp_file_free);
	module_put(THIS_MODULE);
}

static const struct vm_operations_struct smart_basp_vm_ops = {
	.open = smart_basp_vma_open,
	.close = smart_basp_vma_close,
};

static int smart_charge_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct smart_basp_file *ctx = filp->private_data;
	int ret;

	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != PAGE_SIZE ||
	    !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	guard(mutex)(&ctx->lock);
	if (ctx->mapped || ctx->committed)
		return -EBUSY;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vm_flags_clear(vma, VM_MAYEXEC);
	ret = vm_insert_page(vma, vma->vm_start, ctx->page);
	if (ret)
		return ret;
	vma->vm_ops = &smart_basp_vm_ops;
	vma->vm_private_data = ctx;
	smart_basp_vma_open(vma);
	ctx->mapped = true;
	return 0;
}

static long smart_charge_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct smart_basp_file *ctx = filp->private_data;
	void __user *user = (void __user *)arg;
	u8 header[MCA_BASP_HEADER_SIZE], *snapshot;
	u32 total;
	int ret;

	/* The stock producer passes this exact nonstandard command value. */
	if (cmd != 0xd0001001U)
		return -ENOTTY;
	guard(mutex)(&ctx->lock);
	if (ctx->mapped || ctx->committed)
		return -EBUSY;
	if (copy_from_user(header, user, sizeof(header)))
		return -EFAULT;
	total = get_unaligned_le32(header + 8);
	if (total < sizeof(header) || total > MCA_BASP_MAX_SIZE)
		return -EINVAL;
	snapshot = memdup_user(user, total);
	if (IS_ERR(snapshot))
		return PTR_ERR(snapshot);
	/* A changed header between the two user copies cannot grow this copy. */
	ret = smart_basp_commit(ctx, snapshot, total);
	kfree(snapshot);
	if (!ret)
		ctx->committed = true;
	return ret;
}

#ifdef CONFIG_COMPAT
static long smart_charge_compat_ioctl(struct file *filp, unsigned int cmd,
				      unsigned long arg)
{
	return smart_charge_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int smart_charge_panel_notifier_cb(struct notifier_block *nb,
					  unsigned long event, void *val)
{
	struct smart_charge_info *info =
		container_of(nb, struct smart_charge_info, panel_nb);
	int tmp = 0;

	guard(mutex)(&info->state_lock);
	if (info->stopping || !val)
		return NOTIFY_DONE;

	switch (event) {
	case MCA_EVENT_PANEL_SCREEN_STATE_CHANGE:
		tmp = *(int *)val;
		mca_log_info("update screen_state: %d => %d\n",
			     info->screen_state, tmp);
		info->screen_state = tmp;
		mod_delayed_work(system_wq, &info->smart_sense_chg_work, 0);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static const struct file_operations g_smt_chg_file_ops = {
	.owner = THIS_MODULE,
	.open = smart_charge_file_open,
	.release = smart_charge_file_close,
	.mmap = smart_charge_mmap,
	.unlocked_ioctl = smart_charge_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = smart_charge_compat_ioctl,
#endif
};

static int smart_charge_parse_dt(struct smart_charge_info *info)
{
	struct device_node *node = info->dev->of_node;
	int len = 0;
	int ret = 0;
	int batt_ichg_cfg[CC_ICHG_MAX_GROUP * SMARTCHG_MODE_PARA_MAX];

	(void)mca_parse_dts_u32(node, "cell_type", &info->cell_type,
				MCA_CELL_TYPE_1S);
	if (info->cell_type < MCA_CELL_TYPE_1S ||
	    info->cell_type >= MCA_CELL_TYPE_MAX)
		return -EINVAL;
	mca_parse_dts_u32(node, "enable_fv_dec_by_cc",
			  &info->enable_fv_dec_by_cc, 0);
	mca_parse_dts_u32(node, "support_csd", &info->support_csd, 0);
	info->support_sensechg_v2 =
		of_property_read_bool(node, "support_sensechg_v2");
	info->support_cc_ichg = of_property_read_bool(node, "support_cc_ichg");
	if (info->support_cc_ichg) {
		len = mca_parse_dts_u32_count(node, "cc_ichg_cfg",
					      CC_ICHG_MAX_GROUP,
					      SMARTCHG_MODE_PARA_MAX);
		if (len <= 0) {
			mca_log_err("parse cc_ichg_cfg failed\n");
			return -1;
		}

		ret = mca_parse_dts_u32_array(node, "cc_ichg_cfg",
					      batt_ichg_cfg, len);
		if (ret) {
			mca_log_err(" parse cc_ichg_cfg failed\n");
			return -1;
		}

		info->ichg_cc_count = len / SMARTCHG_MODE_PARA_MAX;
		for (int row = 0; row < info->ichg_cc_count; row++) {
			int col = row * SMARTCHG_MODE_PARA_MAX;

			info->ichg_cc_table[row].cc_max =
				batt_ichg_cfg[col + SMARTCHG_MODE_CYCLECOUNT];
			info->ichg_cc_table[row].delta_ichg =
				batt_ichg_cfg[col + SMARTCHG_MODE_DELTA_ICHG];
			if (info->ichg_cc_table[row].cc_max < 0 ||
			    info->ichg_cc_table[row].delta_ichg < 0 ||
			    (row &&
			     info->ichg_cc_table[row].cc_max <=
				     info->ichg_cc_table[row - 1].cc_max))
				return -EINVAL;
			mca_log_err("%d, %d\n", info->ichg_cc_table[row].cc_max,
				    info->ichg_cc_table[row].delta_ichg);
		}
	}

	mca_parse_dts_u32(node, "smart_bypass_support", &info->support_bypass,
			  0);
	if (info->support_bypass) {
		const int elems =
			sizeof(struct smart_bypass_temp_section) / sizeof(u32);

		mca_parse_dts_u32(node, "smart_bypass_entry_soc",
				  &info->bypass_entry_soc, 0);
		mca_parse_dts_u32(node, "smart_bypass_exit_soc",
				  &info->bypass_exit_soc, 0);

		if (info->bypass_entry_soc < 0 || info->bypass_exit_soc > 100 ||
		    info->bypass_entry_soc > info->bypass_exit_soc)
			return -EINVAL;

		len = mca_parse_dts_u32_count(node, "smart_bypass_high_lmt_cfg",
					      SMART_BYPASS_TEMP_SECTION_MAX,
					      elems);
		if (len <= 0) {
			mca_log_err("parse smart_bypass_high_lmt_cfg failed\n");
			return -1;
		}
		info->bypass_high_num = len / elems;
		ret = mca_parse_dts_u32_array(node, "smart_bypass_high_lmt_cfg",
					      (u32 *)info->bypass_high_lmt,
					      len);
		if (ret < 0)
			return -1;

		len = mca_parse_dts_u32_count(node, "smart_bypass_med_lmt_cfg",
					      SMART_BYPASS_TEMP_SECTION_MAX,
					      elems);
		if (len <= 0) {
			mca_log_err("parse smart_bypass_med_lmt_cfg failed\n");
			return -1;
		}
		info->bypass_med_num = len / elems;
		ret = mca_parse_dts_u32_array(node, "smart_bypass_med_lmt_cfg",
					      (u32 *)info->bypass_med_lmt, len);
		if (ret < 0)
			return -1;

		len = mca_parse_dts_u32_count(node, "smart_bypass_low_lmt_cfg",
					      SMART_BYPASS_TEMP_SECTION_MAX,
					      elems);
		if (len <= 0) {
			mca_log_err("parse smart_bypass_low_lmt_cfg failed\n");
			return -1;
		}
		info->bypass_low_num = len / elems;
		ret = mca_parse_dts_u32_array(node, "smart_bypass_low_lmt_cfg",
					      (u32 *)info->bypass_low_lmt, len);
		if (ret < 0)
			return -1;
	}

	return 0;
}

static int smart_charge_probe(struct platform_device *pdev)
{
	struct smart_charge_info *info;
	int ret, online = 0, wireless_online = 0;

	guard(mutex)(&smart_instance_lock);

	if (rcu_access_pointer(global_smartchg_info))
		return -EBUSY;
	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->dev = &pdev->dev;
	info->fake_scene = SMART_CHARGE_FAKE_SCENE_DEFAULT;
	atomic_set(&info->replay_needed, 1);
	mutex_init(&info->state_lock);
	INIT_DELAYED_WORK(&info->smart_charge_work, smart_charge_workfunc);
	INIT_DELAYED_WORK(&info->smart_sense_chg_work,
			  smart_charge_sense_chg_workfunc);
	INIT_DELAYED_WORK(&info->smart_soc_limit_work,
			  smart_charge_soc_limit_workfunc);
	platform_set_drvdata(pdev, info);
	ret = smart_charge_parse_dt(info);
	if (ret)
		return ret;
	ret = smart_charge_init_voter(info);
	if (ret)
		return ret;
	info->basp = kzalloc(sizeof(*info->basp), GFP_KERNEL);
	if (!info->basp) {
		ret = -ENOMEM;
		goto voters;
	}
	kref_init(&info->basp->ref);
	mutex_init(&info->basp->lock);
	info->basp->info = info;
	info->misc.minor = MISC_DYNAMIC_MINOR;
	info->misc.name = "smart_charge_cdev";
	info->misc.fops = &g_smt_chg_file_ops;
	info->misc.parent = &pdev->dev;
	ret = misc_register(&info->misc);
	if (ret)
		goto voters;
	info->panel_nb.notifier_call = smart_charge_panel_notifier_cb;
	ret = mca_event_block_notify_register(MCA_EVENT_TYPE_PANEL,
					      &info->panel_nb);
	if (ret)
		goto device;
	info->thermal_nb.notifier_call = smart_charge_thermal_notifier_cb;
	ret = mca_event_block_notify_register(MCA_EVENT_TYPE_THERMAL_TEMP,
					      &info->thermal_nb);
	if (ret)
		goto panel;
	if (!mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  STRATEGY_STATUS_TYPE_ONLINE, &online))
		info->usb_online = !!online;
	if (!mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					  STRATEGY_STATUS_TYPE_ONLINE,
					  &wireless_online))
		info->wls_online = !!wireless_online;
	info->online = info->usb_online || info->wls_online;
	ret = mca_strategy_ops_register(STRATEGY_FUNC_TYPE_SMARTCHG,
					mca_smart_charge_process_event, NULL,
					NULL, info);
	if (ret)
		goto thermal;
	/* Publish before creating sysfs; roll back publication on group failure. */
	rcu_assign_pointer(global_smartchg_info, info);
	ret = smart_charge_sysfs_create_group(info->dev);
	if (ret)
		goto strategy;
	mod_delayed_work(system_wq, &info->smart_charge_work, 0);
	return 0;
strategy:
	rcu_assign_pointer(global_smartchg_info, NULL);
	synchronize_srcu(&smart_instance_srcu);
	mca_strategy_ops_unregister(STRATEGY_FUNC_TYPE_SMARTCHG, info);
thermal:
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_THERMAL_TEMP,
					  &info->thermal_nb);
panel:
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_PANEL,
					  &info->panel_nb);
device:
	smart_charge_stop_work(info);
	misc_deregister(&info->misc);
voters:
	smart_basp_detach(info);
	smart_charge_destroy_voters(info);
	return ret;
}

static void smart_charge_remove(struct platform_device *pdev)
{
	struct smart_charge_info *info = platform_get_drvdata(pdev);

	guard(mutex)(&smart_instance_lock);

	misc_deregister(&info->misc);
	smart_basp_detach(info);
	smart_charge_sysfs_remove_group(&pdev->dev);
	rcu_assign_pointer(global_smartchg_info, NULL);
	synchronize_srcu(&smart_instance_srcu);
	mca_strategy_ops_unregister(STRATEGY_FUNC_TYPE_SMARTCHG, info);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_THERMAL_TEMP,
					  &info->thermal_nb);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_PANEL,
					  &info->panel_nb);
	smart_charge_stop_work(info);
	memset(&info->smart_chg_data, 0, sizeof(info->smart_chg_data));
	smart_charge_set_soc_limit(false);
	smartcharging_handle_wls_super(info);
	smartcharging_handle_wls_quiet(info);
	smartcharging_handle_power_boost(info, SMART_CHG_OUTDOOR_PWR_BOOST);
	smart_charge_fv_control(NULL, info, 0, NULL);
	smart_charge_ichg_control(NULL, info, 0, NULL);
	smart_charge_set_fcc_control(NULL, info, 0, NULL);
	smart_charge_destroy_voters(info);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "xiaomi,smart_charge" },
	{},
};

MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver smart_charge_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "smart_charge",
		.of_match_table = match_table,
	},
	.probe = smart_charge_probe,
	.remove = smart_charge_remove,
};

static int __init smart_charge_init(void)
{
	return platform_driver_register(&smart_charge_driver);
}
module_init(smart_charge_init);

static void __exit smart_charge_exit(void)
{
	platform_driver_unregister(&smart_charge_driver);
}
module_exit(smart_charge_exit);

MODULE_DESCRIPTION("smart charge driver");
MODULE_AUTHOR("liuzhengqing@xiaomi.com");
MODULE_LICENSE("GPL v2");
