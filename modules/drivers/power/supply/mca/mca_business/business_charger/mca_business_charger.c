// SPDX-License-Identifier: GPL-2.0
/*
 * mca_business_charger.c
 *
 * mc charger business driver
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
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/soc/qcom/fsa4480-i2c.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/strategy/strategy_wireless_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/shared_memory/charger_partition_class.h>
#include "inc/mca_business_charger.h"
#include "inc/mca_charger_usb_psy.h"

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_business_charger"
#endif

extern int connector_antiburn_is_triggered(void);
extern int lpd_is_charging_limit(void);

static DEFINE_MUTEX(business_owner_lock);
static struct business_charger *business_owner;

#define QUICK_CHG_TYPE_FLASH_CHG_POWER 20
#define QUICK_CHG_TYPE_TURBO_CHG_POWER 30
#define QUICK_CHG_TYPE_SUPER_CHG_POWER 50

static void business_charger_report_wired_quick_charge_type(
	struct business_charger *charger, int icon_type);
static int business_charger_strategy_process(struct business_charger *charger,
					     int type, int event, int value)
{
	if (!charger->wls_support &&
	    (type == STRATEGY_FUNC_TYPE_REV_WIRELESS ||
	     type == STRATEGY_FUNC_TYPE_BASIC_WIRELESS ||
	     type == STRATEGY_FUNC_TYPE_QUICK_WIRELESS))
		return -EOPNOTSUPP;
	return mca_strategy_func_process(type, event, value);
}
static int update_usb_pd_type(struct business_charger *charger)
{
	int cur_type = XM_CHARGER_TYPE_UNKNOW;
	int bc12_type = XM_CHARGER_TYPE_UNKNOW;
	int pd_type = XM_CHARGER_TYPE_UNKNOW;
	int ret;

	if (charger->bc12_active) {
		ret = protocol_class_get_adapter_type(ADAPTER_PROTOCOL_BC12,
						      &bc12_type);
		if (ret)
			return ret;
		/* PD is optional for an attached BC1.2 adapter. */
		if (protocol_class_get_adapter_type(ADAPTER_PROTOCOL_PD,
						    &pd_type))
			pd_type = XM_CHARGER_TYPE_UNKNOW;
		cur_type = pd_type ? pd_type : bc12_type;
	}
	charger->bc12_type = bc12_type;
	charger->pd_type = pd_type;
	if (charger->last_type == cur_type)
		return 0;
	charger->real_type = cur_type;
	business_charger_update_usb_psy(cur_type, charger->usb_psy_info);
	if (cur_type == XM_CHARGER_TYPE_FLOAT)
		mca_charge_mievent_report(CHARGE_DFX_NOT_STANDARD_ADAPTER, NULL,
					  0);
	charger->last_type = cur_type;
	return 0;
}

static int business_charger_parse_dt(struct business_charger *charger)
{
	struct device_node *node = charger->dev->of_node;
	int ret;

	ret = mca_parse_dts_u32(node, "support-wls", &charger->wls_support, 0);
	if (ret)
		return ret;
	if (charger->wls_support != 0 && charger->wls_support != 1)
		return -EINVAL;
	if (charger->wls_support &&
	    !IS_ENABLED(CONFIG_MCA_BUSINESS_WLS_REV_COORDINATION))
		return -EOPNOTSUPP;
	ret = of_property_read_u32(node, "business,charger-core-test",
				   &charger->dt.test);
	if (ret)
		return ret;
	return charger->dt.test < 0 ? -EINVAL : 0;
}

static int business_charger_add_event_node(unsigned long event,
					   const void *data, int size,
					   struct business_charger *charger)
{
	struct charger_event *entry;
	unsigned long flags;
	int ret = 0;

	if (size < 0 || size > sizeof(entry->data) || (size && !data))
		return -EINVAL;
	spin_lock_irqsave(&charger->event_lock, flags);
	if (charger->stopping) {
		ret = -ENODEV;
	} else if (charger->event_count == BUSINESS_CHARGER_EVENT_DEPTH) {
		ret = -ENOSPC;
	} else {
		entry = &charger->events[(charger->event_head +
					  charger->event_count) %
					 BUSINESS_CHARGER_EVENT_DEPTH];
		*entry = (struct charger_event){ .event = event };
		if (size)
			memcpy(entry->data, data, size);
		charger->event_count++;
		if (charger->ready)
			queue_work(charger->workqueue, &charger->event_work);
	}
	spin_unlock_irqrestore(&charger->event_lock, flags);
	return ret;
}

static int business_charger_event_size(unsigned long event)
{
	switch (event) {
	case MCA_EVENT_BATTERY_DTPT:
	case MCA_EVENT_CC_SHORT_VBUS:
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
	case MCA_EVENT_CHARGE_VERIFY_PROCESS_END:
	case MCA_EVENT_CP_CBOOT_FAIL:
	case MCA_EVENT_CP_REVERT_CHANGE:
	case MCA_EVENT_CSD_SEND_PULSE:
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
	case MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT:
	case MCA_EVENT_DEBUG_CTRL_SOC_LIMIT:
	case MCA_EVENT_FG_OTA_PROCESS:
	case MCA_EVENT_IBAT_OCP_CHANGE:
	case MCA_EVENT_LPD_STATUS_CHANGE:
	case MCA_EVENT_SBU_LPD_CHANGE:
	case MCA_EVENT_SOC_LIMIT:
	case MCA_EVENT_VBAT_OVP_CHANGE:
	case MCA_EVENT_WIRELESS_REVCHG:
		return sizeof(int);
	case MCA_EVENT_BATT_BTB_CHANGE:
	case MCA_EVENT_SINK_PWR_SUSPEND_CHANGE:
	case MCA_EVENT_USB_SUSPEND:
		return sizeof(bool);
	case MCA_EVENT_CP_TSHUT_FLAG:
		return sizeof(int) * 2;
	case MCA_EVENT_BATTERY_HEALTH_CHANGE:
	case MCA_EVENT_BATT_AUTH_PASS:
	case MCA_EVENT_CHARGE_ABNORMAL:
	case MCA_EVENT_CHARGE_CAP_CHANGE:
	case MCA_EVENT_CHARGE_RESTORE:
	case MCA_EVENT_CONN_ANTIBURN_CHANGE:
	case MCA_EVENT_CP_IBAT_OCP:
	case MCA_EVENT_CP_IBUS_OCP:
	case MCA_EVENT_CP_IIC_ERROR:
	case MCA_EVENT_CP_VBAT_OVP:
	case MCA_EVENT_CP_VBUS_OVP:
	case MCA_EVENT_CP_VUSB_OVP:
	case MCA_EVENT_PMIC_INIT_DONE:
	case MCA_EVENT_USB_CONNECT:
	case MCA_EVENT_USB_DISCONNECT:
	case MCA_EVENT_USB_STS_CHANGE:
	case MCA_EVENT_WIRELESS_CONNECT:
	case MCA_EVENT_WIRELESS_DISCONNECT:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int business_charger_notifier_connect_cb(struct notifier_block *nb,
						unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, connect_nb);
	int size = business_charger_event_size(event);

	if (size < 0)
		return NOTIFY_DONE;
	if (!charger->wls_support && (event == MCA_EVENT_WIRELESS_CONNECT ||
				      event == MCA_EVENT_WIRELESS_DISCONNECT ||
				      event == MCA_EVENT_WIRELESS_REVCHG))
		return notifier_from_errno(-EOPNOTSUPP);
	return notifier_from_errno(
		business_charger_add_event_node(event, data, size, charger));
}

static int business_charger_notifier_type_cb(struct notifier_block *nb,
					     unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, type_change_nb);
	int size = business_charger_event_size(event);

	if (size < 0)
		return NOTIFY_DONE;
	if (!charger->wls_support && (event == MCA_EVENT_WIRELESS_CONNECT ||
				      event == MCA_EVENT_WIRELESS_DISCONNECT ||
				      event == MCA_EVENT_WIRELESS_REVCHG))
		return notifier_from_errno(-EOPNOTSUPP);
	return notifier_from_errno(
		business_charger_add_event_node(event, data, size, charger));
}

static int business_charger_notifier_hw_cb(struct notifier_block *nb,
					   unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, hw_info_nb);
	int size = business_charger_event_size(event);

	if (size < 0)
		return NOTIFY_DONE;
	if (!charger->wls_support && (event == MCA_EVENT_WIRELESS_CONNECT ||
				      event == MCA_EVENT_WIRELESS_DISCONNECT ||
				      event == MCA_EVENT_WIRELESS_REVCHG))
		return notifier_from_errno(-EOPNOTSUPP);
	return notifier_from_errno(
		business_charger_add_event_node(event, data, size, charger));
}

static int business_charger_notifier_chg_sts_cb(struct notifier_block *nb,
						unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, chg_sts_nb);
	int size = business_charger_event_size(event);

	if (size < 0)
		return NOTIFY_DONE;
	if (!charger->wls_support && (event == MCA_EVENT_WIRELESS_CONNECT ||
				      event == MCA_EVENT_WIRELESS_DISCONNECT ||
				      event == MCA_EVENT_WIRELESS_REVCHG))
		return notifier_from_errno(-EOPNOTSUPP);
	return notifier_from_errno(
		business_charger_add_event_node(event, data, size, charger));
}

static int business_charger_notifier_cp_info_cb(struct notifier_block *nb,
						unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, cp_info_nb);
	int size = business_charger_event_size(event);

	if (size < 0)
		return NOTIFY_DONE;
	if (!charger->wls_support && (event == MCA_EVENT_WIRELESS_CONNECT ||
				      event == MCA_EVENT_WIRELESS_DISCONNECT ||
				      event == MCA_EVENT_WIRELESS_REVCHG))
		return notifier_from_errno(-EOPNOTSUPP);
	return notifier_from_errno(
		business_charger_add_event_node(event, data, size, charger));
}

static int business_charger_notifier_debug_cb(struct notifier_block *nb,
					      unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, debug_nb);
	int size = business_charger_event_size(event);

	if (size < 0)
		return NOTIFY_DONE;
	if (!charger->wls_support && (event == MCA_EVENT_WIRELESS_CONNECT ||
				      event == MCA_EVENT_WIRELESS_DISCONNECT ||
				      event == MCA_EVENT_WIRELESS_REVCHG))
		return notifier_from_errno(-EOPNOTSUPP);
	return notifier_from_errno(
		business_charger_add_event_node(event, data, size, charger));
}

static void
business_charger_wireless_report_status_work(struct work_struct *work)
{
	struct business_charger *charger = container_of(
		work, struct business_charger, delay_report_status_work.work);
	int len = 0, ret = 0;
	int usb_online = 0;
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	struct mca_event_notify_data event_data = { 0 };
	union power_supply_propval prop;

	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return;
	if (!charger->wls_support)
		return;

	mca_log_info("wireless delay report status work\n");

	prop.intval = charger->wls_active;
	ret = power_supply_set_property(charger->wls_psy_info->wireless_psy,
					POWER_SUPPLY_PROP_ONLINE, &prop);
	if (ret < 0)
		mca_log_err("failed to set wls_online");
	business_charger_update_wireless_psy(charger->wls_psy_info);

	len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
		       "POWER_SUPPLY_TX_ADAPTER=0");
	event_data.event = event;
	event_data.event_len = len;
	mca_event_report_uevent(&event_data);

	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_WIRELESS_DISCONNECT,
					  charger->wls_active);

	if (mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					 STRATEGY_STATUS_TYPE_ONLINE,
					 &usb_online))
		return;
	if (usb_online)
		return;

	__pm_relax(charger->online_wake_lock);

	len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
		       "POWER_SUPPLY_QUICK_CHARGE_TYPE=0");
	event_data.event = event;
	event_data.event_len = len;
	mca_event_report_uevent(&event_data);
}

static void
business_charger_wireless_delay_enable_rx_work(struct work_struct *work)
{
	struct business_charger *charger = container_of(
		work, struct business_charger, delay_enable_rx_work.work);
	int usb_online = 0;

	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return;
	if (!charger->wls_support)
		return;

	if (mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					 STRATEGY_STATUS_TYPE_ONLINE,
					 &usb_online))
		return;
	if (!usb_online)
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER,
							true);

	mca_log_info("delay_enable_rx when usb out\n");
}

static void business_charger_wireless_reset_rx_work(struct work_struct *work)
{
	struct business_charger *charger =
		container_of(work, struct business_charger, reset_rx_work.work);
	int usb_present = 0;

	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return;
	if (!charger->wls_support)
		return;

	if (mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					 STRATEGY_STATUS_TYPE_ONLINE,
					 &usb_present))
		return;
	mca_log_info("get usb_present %d\n", usb_present);
	if (!usb_present) {
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER,
							false);
		usleep_range(125000, 150000);
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER,
							true);
	}
}

static void
business_charger_process_cap_change(struct business_charger *charger,
				    unsigned int event)
{
	if (charger->real_type == XM_CHARGER_TYPE_PPS) {
		cancel_delayed_work(&charger->report_quick_charge_type_work);
		mod_delayed_work(charger->workqueue,
				 &charger->report_quick_charge_type_work, 0);
	} else {
		cancel_delayed_work(&charger->report_quick_charge_type_work);
		mod_delayed_work(charger->workqueue,
				 &charger->report_quick_charge_type_work,
				 msecs_to_jiffies(250));
	}

	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, 0);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, 0);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_THERMAL,
					  event, 0);
}

static void
business_charger_process_snk_power_suspend(struct business_charger *charger,
					   unsigned int event, void *data)
{
	bool bsinkpowersuspend = *((bool *)data);

	mca_log_info("bsinkpowersuspend: %d\n", bsinkpowersuspend);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  event, bsinkpowersuspend);
}

static void business_charger_report_wired_quick_charge_type(
	struct business_charger *charger, int icon_type)
{
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	struct mca_event_notify_data event_data;
	int len;

	charger->wired_qucik_charge_type = icon_type;

	if (charger->abnormal_info.asuint32) {
		icon_type = ADP_ICON_TYPE_NORMAL;
		mca_log_info("force normal icon 0x%x",
			     charger->abnormal_info.asuint32);
	}

	len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
		       "POWER_SUPPLY_QUICK_CHARGE_TYPE=%d", icon_type);
	event_data.event = event;
	event_data.event_len = len;
	mca_event_report_uevent(&event_data);
}

static void
business_charger_report_quick_charge_type_work(struct work_struct *work)
{
	struct business_charger *charger =
		container_of(work, struct business_charger,
			     report_quick_charge_type_work.work);
	unsigned int power_max = 0;
	int icon_type;

	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return;

	if (charger->real_type != XM_CHARGER_TYPE_PPS)
		return;

	if (protocol_class_get_adapter_max_power(ADAPTER_PROTOCOL_PPS,
						 &power_max) ||
	    power_max > INT_MAX)
		return;
	charger->wired_power_max = power_max;
	if (power_max >= QUICK_CHG_TYPE_SUPER_CHG_POWER)
		icon_type = ADP_ICON_TYPE_SUPER;
	else if (power_max >= QUICK_CHG_TYPE_TURBO_CHG_POWER)
		icon_type = ADP_ICON_TYPE_TURBO;
	else if (power_max >= QUICK_CHG_TYPE_FLASH_CHG_POWER)
		icon_type = ADP_ICON_TYPE_FLASH;
	else
		icon_type = ADP_ICON_TYPE_FAST;

	mca_log_info("power_max: %d, icon_type: %d\n", power_max, icon_type);
	business_charger_report_wired_quick_charge_type(charger, icon_type);
}

static int business_charger_battery_property(enum power_supply_property prop,
					     union power_supply_propval *val)
{
	struct power_supply *battery = power_supply_get_by_name("battery");
	int ret;

	if (!battery)
		return -ENODEV;
	ret = power_supply_get_property(battery, prop, val);
	power_supply_put(battery);
	return ret;
}

static void business_charger_update_wired_quick_charge_type(
	struct business_charger *charger)
{
	union power_supply_propval pval = {
		0,
	};
	int icon_type = 0;

	if (!charger->bc12_active)
		return;

	if (business_charger_battery_property(POWER_SUPPLY_PROP_HEALTH,
					      &pval)) {
		business_charger_report_wired_quick_charge_type(
			charger, ADP_ICON_TYPE_NORMAL);
		return;
	}
	charger->abnormal_info.batt_cold = pval.intval ==
					   POWER_SUPPLY_HEALTH_COLD;
	charger->abnormal_info.batt_overhot = pval.intval ==
					      POWER_SUPPLY_HEALTH_OVERHEAT;

	switch (charger->real_type) {
	case XM_CHARGER_TYPE_SDP:
	case XM_CHARGER_TYPE_CDP:
	case XM_CHARGER_TYPE_DCP:
	case XM_CHARGER_TYPE_FLOAT:
	case XM_CHARGER_TYPE_TYPEC:
	case XM_CHARGER_TYPE_ACA:
	case XM_CHARGER_TYPE_OCP:
		icon_type = ADP_ICON_TYPE_NORMAL;
		break;
	case XM_CHARGER_TYPE_PD:
	case XM_CHARGER_TYPE_HVDCP2:
	case XM_CHARGER_TYPE_HVDCP3:
		icon_type = ADP_ICON_TYPE_FAST;
		break;
	case XM_CHARGER_TYPE_HVDCP3_B:
	case XM_CHARGER_TYPE_HVDCP3P5:
		icon_type = ADP_ICON_TYPE_FLASH;
		break;
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		icon_type = ADP_ICON_TYPE_FAST;
		if (protocol_class_get_adapter_max_power(
			    ADAPTER_PROTOCOL_PPS,
			    (unsigned int *)&charger->wired_power_max))
			charger->wired_power_max = 0;
		break;
	default:
		break;
	}

	business_charger_report_wired_quick_charge_type(charger, icon_type);
}

static void
business_charger_process_type_change(struct business_charger *charger,
				     unsigned int event, void *data)
{
	if (update_usb_pd_type(charger))
		return;
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  event, charger->real_type);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
					  charger->real_type);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_FG, event,
					  charger->real_type);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_THERMAL,
					  event, charger->real_type);
	if (charger->real_type != XM_CHARGER_TYPE_PPS)
		business_charger_update_wired_quick_charge_type(charger);
}

static void business_charger_process_wireless_online_change(
	struct business_charger *charger, unsigned int event)
{
	int wls_active_flag = charger->last_wls_active;
	int ret = 0;
	union power_supply_propval prop;

	if (!charger->wls_psy_info ||
	    mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					 STRATEGY_STATUS_TYPE_ONLINE,
					 &charger->wls_active))
		return;
	mca_log_info("wireless online %d\n", charger->wls_active);
	if (wls_active_flag == charger->wls_active)
		return;

	wls_active_flag = charger->wls_active;
	charger->last_wls_active = wls_active_flag;
	if (!wls_active_flag) {
		charger->wls_adapter_type = 0;
		event = MCA_EVENT_WIRELESS_DISCONNECT;
		mca_log_info("relax wake lock\n");
		mca_charge_mievent_set_state(MIEVENT_STATE_PLUG, 0);
	} else {
		if (!charger->dam_test_flag) {
			mca_log_info("stay wake lock\n");
			event = MCA_EVENT_WIRELESS_CONNECT;
			__pm_stay_awake(charger->online_wake_lock);
		}
	}

	if (wls_active_flag) {
		prop.intval = charger->wls_active;
		ret = power_supply_set_property(
			charger->wls_psy_info->wireless_psy,
			POWER_SUPPLY_PROP_ONLINE, &prop);
		if (ret < 0)
			mca_log_err("failed to set wls_online");
		cancel_delayed_work(&charger->delay_report_status_work);
		business_charger_update_wireless_psy(charger->wls_psy_info);
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
			wls_active_flag);
	} else
		mod_delayed_work(charger->workqueue,
				 &charger->delay_report_status_work,
				 msecs_to_jiffies(500));

	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_REV_WIRELESS,
					  event, wls_active_flag);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					  event, wls_active_flag);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					  event, wls_active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_JEITA,
					  event, wls_active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_FG, event,
					  wls_active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_SMARTCHG,
					  event, wls_active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_THERMAL,
					  event, wls_active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_BMD,
					  event, wls_active_flag);
}

static void
business_charger_process_usb_sns_func(struct business_charger *charger,
				      unsigned int event, void *data)
{
#if IS_ENABLED(CONFIG_MCA_BUSINESS_WLS_REV_COORDINATION)
	int usb_present = *((int *)data);
	bool fw_update = false;

	if (!charger->wls_support)
		return;

	mca_wireless_rev_get_fw_update(&fw_update);
	mca_log_err("usb_resent: %d, fw_update: %d\n", usb_present, fw_update);

	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_REV_WIRELESS, event, usb_present);

	if (!usb_present) {
		if (!fw_update)
			mod_delayed_work(charger->workqueue,
					 &charger->delay_enable_rx_work,
					 msecs_to_jiffies(375));
	} else {
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER,
							false);
	}
	mca_wireless_rev_set_usb_plugin(usb_present != 0);
#endif
}

static void
business_charger_wireless_rerun_usb_sns_work(struct work_struct *work)
{
	struct business_charger *charger =
		container_of(work, struct business_charger,
			     wireless_rerun_usb_sns_work.work);
	int usb_present = 0;

	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return;
	if (!charger->wls_support)
		return;

	mca_log_info("wireless rerun cp vusb work\n");
	if (mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					 STRATEGY_STATUS_TYPE_ONLINE,
					 &usb_present))
		return;
	business_charger_process_usb_sns_func(charger, (usb_present != 0),
					      &usb_present);
}

static void
business_charger_process_online_change(struct business_charger *charger,
				       unsigned int event)
{
	int active_flag;
	struct mca_event_notify_data n_data = { 0 };
	union power_supply_propval pval = {
		0,
	};

	if (charger->wls_psy_info &&
	    power_supply_get_property(charger->wls_psy_info->wireless_psy,
				      POWER_SUPPLY_PROP_ONLINE, &pval))
		return;

	if (mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					 STRATEGY_STATUS_TYPE_ONLINE,
					 &charger->bc12_active))
		return;
	mca_log_info("online %d\n", charger->bc12_active);

	active_flag = charger->bc12_active;
	if (!active_flag) {
		charger->pd_type = 0;
		charger->real_type = 0;
		charger->dam_test_flag = 0;
		charger->wired_qucik_charge_type = 0;
		charger->wired_power_max = 0;
		event = MCA_EVENT_USB_DISCONNECT;
		__pm_relax(charger->online_wake_lock);
		mca_charge_mievent_set_state(MIEVENT_STATE_PLUG, 0);
		mca_log_info("relax wake lock\n");
	} else if (!charger->dam_test_flag) {
		mca_log_info("stay wake lock\n");
		event = MCA_EVENT_USB_CONNECT;
		__pm_stay_awake(charger->online_wake_lock);
	}
	if (update_usb_pd_type(charger))
		return;
	business_charger_process_usb_sns_func(charger, event, &active_flag);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, active_flag);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_JEITA,
					  event, active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_FG, event,
					  active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_SMARTCHG,
					  event, active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_THERMAL,
					  event, active_flag);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_BMD,
					  event, active_flag);
	if (active_flag) {
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE,
			MCA_EVENT_CHARGE_TYPE_CHANGE, charger->real_type);
	} else if (!pval.intval) {
		n_data.event = "POWER_SUPPLY_QUICK_CHARGE_TYPE=0";
		n_data.event_len =
			32; /* length of POWER_SUPPLY_QUICK_CHARGE_TYPE=0 */
		mca_event_report_uevent(&n_data);
	}
}

static void
business_charger_process_usb_suspend_change(struct business_charger *charger,
					    unsigned int event, void *data)
{
	bool usb_suspend = *((bool *)data);

	if (usb_suspend) {
		__pm_relax(charger->online_wake_lock);
		mca_log_info("relax wake lock\n");
	} else {
		if (charger->bc12_active) {
			__pm_stay_awake(charger->online_wake_lock);
			mca_log_info("stay wake lock\n");
		}
	}
}

static void
business_charger_process_is_eu_model(struct business_charger *charger,
				     int value)
{
	mca_log_info("is_eu_model: %d\n", value);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_FG,
					  MCA_EVENT_IS_EU_MODEL, value);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  MCA_EVENT_IS_EU_MODEL, value);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_IS_EU_MODEL, value);
}

static void
business_charger_process_plate_shock(struct business_charger *charger,
				     int value)
{
	mca_log_info("plate_shock: %d\n", value);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  MCA_EVENT_PLATE_SHOCK,
					  value == 1 ? 1 : 0);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_PLATE_SHOCK,
					  value == 1 ? 1 : 0);
}

static void
business_charger_process_handle_logic(struct business_charger *charger)
{
	int last_handle_state = charger->last_handle_state;
	int last_stop_handle_charge = charger->last_stop_handle_charge;
	int stop = charger->stop_handle_charge;
	int hs = charger->handle_state;
	int allow_charge = -1;

	if (last_stop_handle_charge != 0 && stop == 0 && hs != 0) {
		/* stop just cleared while a handle-state is set: hold off */
		allow_charge = -1;
	} else if (last_stop_handle_charge != 0 && stop == 0 && hs == 0) {
		allow_charge = 1;
	} else if (stop != 0 && last_stop_handle_charge != stop) {
		allow_charge = 1;
	}

	if (hs == 0 && last_handle_state != 0)
		allow_charge = 1;
	if (stop == 0 && hs != 0 && last_handle_state != hs)
		allow_charge = 0;

	mca_log_info(
		"last_allow_charge = %d, last_handle_state = %d, last_stop_handle_charge = %d, allow_charge = %d, handle_state = %d, stop_handle_charge = %d\n",
		charger->usb_psy_info->charge_enable, last_handle_state,
		last_stop_handle_charge, allow_charge, hs, stop);

	charger->last_handle_state = charger->handle_state;
	charger->last_stop_handle_charge = charger->stop_handle_charge;

	if (allow_charge != -1 &&
	    allow_charge != charger->usb_psy_info->charge_enable) {
		charger->usb_psy_info->charge_enable = allow_charge;
		business_usb_psy_event_process(charger->usb_psy_info);
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE,
			MCA_EVENT_HANDLE_ALLOW_CHARGE, allow_charge);
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE,
			MCA_EVENT_HANDLE_ALLOW_CHARGE, allow_charge);
		if (allow_charge == 0) {
			if (charger->real_type != 0) {
				struct mca_event_notify_data n_data = { 0 };

				n_data.event =
					"POWER_SUPPLY_QUICK_CHARGE_TYPE=0";
				n_data.event_len = 32;
				mca_event_report_uevent(&n_data);
			}
		} else if (charger->real_type != XM_CHARGER_TYPE_PPS) {
			business_charger_update_wired_quick_charge_type(
				charger);
		}
	}
}

static void
business_charger_process_lpd_status_change(struct business_charger *charger,
					   unsigned int event, void *data)
{
	int force_5v_charging = 0;
	int is_liquited = *((int *)data);
	int is_charginglimit = lpd_is_charging_limit();

	if (is_charginglimit < 0) {
		mca_log_err("LPD provider read failed: %d\n", is_charginglimit);
		force_5v_charging = 1;
	} else {
		force_5v_charging = !!is_liquited && !!is_charginglimit;
	}

	mca_log_info("%s: occur liquited need drop charging power: %d\n",
		     __func__, force_5v_charging);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
					  force_5v_charging);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  event, force_5v_charging);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_REV_WIRELESS, event, is_liquited);
	mca_charge_mievent_report(CHARGE_DFX_LPD_DETECTED, &is_liquited, 1);
}

static void
business_charger_process_verify_process_change(struct business_charger *charger,
					       unsigned int event, void *data)
{
	int verify_process_end = *((int *)data);

	mca_log_info("pd verify process done: %d\n", !verify_process_end);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
					  !verify_process_end);
}

static void
business_charger_process_pmic_init_done(struct business_charger *charger)
{
	if (charger->pmic_notified)
		return;
	charger->pmic_notified = true;
	mca_log_err("pmic init done\n");
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_PMIC_INIT_DONE, 1);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					  MCA_EVENT_PMIC_INIT_DONE, 1);
}

static void
business_charger_pmic_init_done_notify_work(struct work_struct *work)
{
	struct business_charger *charger = container_of(
		work, struct business_charger, pmic_init_done_notify_work.work);
	int wls_init = 0;
	int buck_init = 0;

	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return;

	mca_log_info("pmic init done notify work\n");

	if (!charger->wls_support)
		wls_init = 1;
	else
		(void)mca_strategy_func_get_status(
			STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
			STRATEGY_STATUS_TYPE_CHARGING, &wls_init);
	(void)mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					   STRATEGY_STATUS_TYPE_CHARGING,
					   &buck_init);

	if (!wls_init || !buck_init) {
		mca_log_err("wls_basic or buckchg not init done, rerun work\n");
		mod_delayed_work(charger->workqueue,
				 &charger->pmic_init_done_notify_work,
				 msecs_to_jiffies(250));
		return;
	}

	if (platform_class_buckchg_ops_is_init_ok(MAIN_BUCK_CHARGER) < 1) {
		mca_log_err("pmic not init done, return & wait event\n");
		return;
	}
	mca_log_err("pmic init done, process event\n");
	business_charger_process_pmic_init_done(charger);
}

static void
business_charger_process_cp_cboot_short(struct business_charger *charger,
					unsigned int event, void *data)
{
	int cp_cboot_short = *((int *)data);

	mca_log_info("occur cp cboot short: %d\n", cp_cboot_short);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, cp_cboot_short);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  event, cp_cboot_short);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_REV_WIRELESS,
					  event, cp_cboot_short);
}

static void
business_charger_process_quick_revchg(struct business_charger *charger,
				      unsigned int event, void *data)
{
	int auth_pos = *((int *)data);

	mca_log_info("handle quick revchg changed: %#x\n", auth_pos);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, auth_pos);
}

static void
business_charger_process_rp_short_vbus_change(struct business_charger *charger,
					      unsigned int event, void *data)
{
	int cc_short_vbus = *((int *)data);

	mca_log_info("occur cc short vbus: %d\n", cc_short_vbus);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, cc_short_vbus);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, cc_short_vbus);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_REV_WIRELESS, event, cc_short_vbus);
	if (cc_short_vbus)
		mca_charge_mievent_report(CHARGE_DFX_RP_SHORT_VBUS_DETECTED,
					  NULL, 0);
}

static void business_charger_process_vbat_ovp_status_change(
	struct business_charger *charger, unsigned int event, void *data)
{
	int vbat_ovp_status = *((int *)data);

	mca_log_info("occur vbat_ovp_status: %d\n", vbat_ovp_status);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
					  vbat_ovp_status);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  event, vbat_ovp_status);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					  event, vbat_ovp_status);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					  event, vbat_ovp_status);
}

static void business_charger_process_ibat_ocp_status_change(
	struct business_charger *charger, unsigned int event, void *data)
{
	int ibat_ocp_status = *((int *)data);

	mca_log_info("occur ibat_ocp_status: %d\n", ibat_ocp_status);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  event, ibat_ocp_status);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					  event, ibat_ocp_status);
}

static void
business_charger_process_csd_pulse_change(struct business_charger *charger,
					  unsigned int event, void *data)
{
	int csd_pulse = *((int *)data);

	if (charger->real_type == XM_CHARGER_TYPE_PPS) {
		mca_log_info("receive csd pulse request: %d\n", csd_pulse);
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event,
			csd_pulse);
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
			csd_pulse);
	} else
		mca_log_info(
			"not standard pd verify, ignore csd pulse event\n");
}

static void
business_charger_process_soc_limit_change(struct business_charger *charger,
					  unsigned int event, void *data)
{
	int smartchg_pmic = *((int *)data);

	mca_log_info("smartchg_pmic: %d\n", smartchg_pmic);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, smartchg_pmic);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, smartchg_pmic);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					  event, smartchg_pmic);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					  event, smartchg_pmic);
}

static void
business_charger_process_dtpt_change(struct business_charger *charger,
				     unsigned int event, void *data)
{
	int dtpt_status = *((int *)data);

	mca_log_info("dtpt_status: %d\n", dtpt_status);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_SMARTCHG,
					  event, dtpt_status);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_JEITA,
					  event, dtpt_status);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, dtpt_status);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, dtpt_status);
}

static void business_charger_process_fg_ota(struct business_charger *charger,
					    unsigned int event, void *data)
{
	int fg_ota_process = *((int *)data);

	mca_log_info("fg_ota_process: %d\n", fg_ota_process);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  event, fg_ota_process);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					  event, fg_ota_process);
}

static void
business_charger_process_batt_health_change(struct business_charger *charger,
					    unsigned int event)
{
	business_charger_update_wired_quick_charge_type(charger);
	if (charger->real_type == XM_CHARGER_TYPE_PPS)
		mod_delayed_work(charger->workqueue,
				 &charger->report_quick_charge_type_work, 0);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, 0);
}

static void
business_charger_process_batt_btb_change(struct business_charger *charger,
					 unsigned int event, void *data)
{
	bool batt_missing = *((bool *)data);

	mca_log_info("batt_missing: %d\n", batt_missing);
	charger->abnormal_info.batt_missing = (int)batt_missing;
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, batt_missing);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, batt_missing);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_REV_WIRELESS, event, batt_missing);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					  event, batt_missing);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					  event, batt_missing);
}

static void
business_charger_process_batt_auth_pass(struct business_charger *charger,
					unsigned int event)
{
	mca_log_err("reviced batt_auth pass\n");
	charger->abnormal_info.batt_auth_failed = 0;
	business_charger_update_wired_quick_charge_type(charger);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, true);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, true);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, true);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, true);
}

static void
business_charger_process_chg_sts_change(struct business_charger *charger,
					unsigned int event)
{
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, 0);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, 0);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, 0);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, 0);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_REV_WIRELESS, event, 0);
}

static void
business_charger_process_antiburn_change(struct business_charger *charger,
					 unsigned int event)
{
	int antiburn = connector_antiburn_is_triggered();

	if (antiburn < 0) {
		mca_log_err("antiburn provider read failed: %d\n", antiburn);
		antiburn = 1;
	}
	mca_log_info("antiburn: %d\n", antiburn);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, antiburn);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, antiburn);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_REV_WIRELESS, event, antiburn);
}

static void business_charger_mievent_report_cp_vbat_ovp(void)
{
	int event_data[2] = { 0 };
	int vcell = 0, vpack = 0;

	if (strategy_class_fg_ops_get_voltage(&vcell) ||
	    platform_class_cp_get_battery_voltage(CP_ROLE_MASTER, &vpack))
		return;
	event_data[0] = vcell;
	event_data[1] = vpack;
	mca_charge_mievent_report(CHARGE_DFX_CP_VBAT_OVP, event_data, 2);
}

static void
business_charger_process_debug_double85(struct business_charger *charger,
					int val)
{
	if (val != 0 && val != 1)
		return;
	if (val == charger->last_double85)
		return;
	charger->last_double85 = val;
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_THERMAL,
					  MCA_EVENT_DEBUG_CTRL_DOUBLE85, val);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_FG,
					  MCA_EVENT_DEBUG_CTRL_DOUBLE85, val);
	platform_class_buckchg_ops_set_too_hot_limit(MAIN_BUCK_CHARGER,
						     val == 0);
}

static void business_charger_process_debug_remove_temp_limit(
	struct business_charger *charger, int val)
{
	if (val != 0 && val != 1)
		return;
	if (val == charger->last_remove_temp_limit)
		return;
	charger->last_remove_temp_limit = val;
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_THERMAL,
		MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT, val);
	business_charger_strategy_process(
		charger, STRATEGY_FUNC_TYPE_FG,
		MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT, val);
	platform_class_buckchg_ops_set_too_hot_limit(MAIN_BUCK_CHARGER,
						     val == 0);
}

static void
business_charger_process_debug_memory_test(struct business_charger *charger,
					   int val)
{
	if (val != 0 && val != 1)
		return;
	if (val == charger->last_memory_test)
		return;
	charger->last_memory_test = val;
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_THERMAL,
					  MCA_EVENT_DEBUG_CTRL_MEMORY_TEST,
					  val);
	business_charger_strategy_process(charger, STRATEGY_FUNC_TYPE_FG,
					  MCA_EVENT_DEBUG_CTRL_MEMORY_TEST,
					  val);
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_DEBUG_CTRL_MEMORY_TEST,
					  val);
	platform_class_buckchg_ops_set_too_hot_limit(MAIN_BUCK_CHARGER,
						     val == 0);
}

static void
business_charger_process_debug_soc_limit(struct business_charger *charger,
					 int packed)
{
	int hi = (packed >> 8) & 0xff;
	int lo = packed & 0xff;

	if (packed < 0 || packed > 0xffff || hi > 100 || lo > hi) {
		mca_log_err("soc_limit value is invalid: %d %d\n", hi, lo);
		return;
	}
	if (hi == charger->last_soc_limit_hi &&
	    lo == charger->last_soc_limit_lo)
		return;
	charger->last_soc_limit_hi = hi;
	charger->last_soc_limit_lo = lo;
	business_charger_strategy_process(charger,
					  STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_DEBUG_CTRL_SOC_LIMIT,
					  packed);
}

static void
business_charger_process_sbu_lpd_status_change(struct business_charger *charger,
					       int value)
{
	struct of_phandle_args switch_args = { 0 };
	int sbu_event;
	int rc;

	rc = of_parse_phandle_with_args(charger->dev->of_node,
					"fsa4480-i2c-handle", NULL, 0,
					&switch_args);
	if (rc != 0 || switch_args.np == NULL) {
		mca_log_err("no switch node found\n");
		return;
	}

	mca_log_info("handle sbu lpd changed: %#x\n", value);
	if (value == 1)
		sbu_event = FSA_USBC_SBU_LPD_SENSE;
	else if (value == 2)
		sbu_event = FSA_USBC_SBU_LPD_ISOLATE;
	else
		sbu_event = FSA_USBC_DISPLAYPORT_DISCONNECTED;

	mca_log_err("third sbu event is %d\n", sbu_event);
	rc = fsa4480_switch_event(switch_args.np, sbu_event);
	of_node_put(switch_args.np);
	if (rc)
		mca_log_err("SBU switch failed: %d\n", rc);
}

static void business_charger_dispatch_event(struct business_charger *charger,
					    struct charger_event *entry)
{
	int data[2] = { 0 };

	switch (entry->event) {
	case MCA_EVENT_USB_DISCONNECT:
	case MCA_EVENT_USB_CONNECT:
		business_charger_process_online_change(charger, entry->event);
		break;
	case MCA_EVENT_WIRELESS_CONNECT:
	case MCA_EVENT_WIRELESS_DISCONNECT:
		business_charger_process_wireless_online_change(charger,
								entry->event);
		break;
	case MCA_EVENT_WIRELESS_REVCHG:
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, entry->event,
			*((int *)entry->data));
		break;
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
		business_charger_process_type_change(charger, entry->event,
						     entry->data);
		break;
	case MCA_EVENT_CHARGE_CAP_CHANGE:
		business_charger_process_cap_change(charger, entry->event);
		break;
	case MCA_EVENT_CHARGE_VERIFY_PROCESS_END:
		mca_log_info("receive verify_process_end notify: %d\n",
			     *((int *)entry->data));
		business_charger_process_verify_process_change(
			charger, entry->event, entry->data);
		break;
	case MCA_EVENT_BATTERY_HEALTH_CHANGE:
		business_charger_process_batt_health_change(charger,
							    entry->event);
		break;
	case MCA_EVENT_CP_VUSB_OVP:
		mca_charge_mievent_report(CHARGE_DFX_CP_VAC_OVP, NULL, 0);
		break;
	case MCA_EVENT_CP_VBAT_OVP:
		business_charger_mievent_report_cp_vbat_ovp();
		break;
	case MCA_EVENT_CP_VBUS_OVP:
		mca_charge_mievent_report(CHARGE_DFX_CP_VBUS_OVP, NULL, 0);
		break;
	case MCA_EVENT_CP_IBAT_OCP:
		mca_charge_mievent_report(CHARGE_DFX_CP_IBAT_OCP, NULL, 0);
		break;
	case MCA_EVENT_CP_IBUS_OCP:
		mca_charge_mievent_report(CHARGE_DFX_CP_IBUS_OCP, NULL, 0);
		break;
	case MCA_EVENT_CP_CBOOT_FAIL:
		business_charger_process_cp_cboot_short(charger, entry->event,
							entry->data);
		break;
	case MCA_EVENT_CP_IIC_ERROR:
		business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE, entry->event,
			0);
		mca_charge_mievent_report(CHARGE_DFX_CP_ABSENT, data, 2);
		break;
	case MCA_EVENT_CP_TSHUT_FLAG:
		if (charger->wls_support == 1) {
			platform_class_cp_get_tdie(CP_ROLE_MASTER,
						   &((int *)entry->data)[0]);
			platform_class_cp_get_tdie(CP_ROLE_SLAVE,
						   &((int *)entry->data)[1]);
		}
		mca_charge_mievent_report(CHARGE_DFX_CP_TDIE_HOT, entry->data,
					  2);
		break;
	case MCA_EVENT_CONN_ANTIBURN_CHANGE:
		business_charger_process_antiburn_change(charger, entry->event);
		break;
	case MCA_EVENT_BATT_BTB_CHANGE:
		business_charger_process_batt_btb_change(charger, entry->event,
							 entry->data);
		break;
	case MCA_EVENT_BATT_AUTH_PASS:
		business_charger_process_batt_auth_pass(charger, entry->event);
		break;
	case MCA_EVENT_LPD_STATUS_CHANGE:
		business_charger_process_lpd_status_change(
			charger, entry->event, entry->data);
		break;
	case MCA_EVENT_PMIC_INIT_DONE:
		business_charger_process_pmic_init_done(charger);
		break;
	case MCA_EVENT_CC_SHORT_VBUS:
		business_charger_process_rp_short_vbus_change(
			charger, entry->event, entry->data);
		break;
	case MCA_EVENT_VBAT_OVP_CHANGE:
		business_charger_process_vbat_ovp_status_change(
			charger, entry->event, entry->data);
		break;
	case MCA_EVENT_IBAT_OCP_CHANGE:
		business_charger_process_ibat_ocp_status_change(
			charger, entry->event, entry->data);
		break;
	case MCA_EVENT_CP_REVERT_CHANGE:
		business_charger_process_quick_revchg(charger, entry->event,
						      entry->data);
		break;
	case MCA_EVENT_USB_STS_CHANGE:
		business_usb_psy_event_process(charger->usb_psy_info);
		break;
	case MCA_EVENT_CHARGE_ABNORMAL:
	case MCA_EVENT_CHARGE_RESTORE:
		business_charger_process_chg_sts_change(charger, entry->event);
		break;
	case MCA_EVENT_SOC_LIMIT:
		business_charger_process_soc_limit_change(charger, entry->event,
							  entry->data);
		break;
	case MCA_EVENT_BATTERY_DTPT:
		business_charger_process_dtpt_change(charger, entry->event,
						     entry->data);
		break;
	case MCA_EVENT_CSD_SEND_PULSE:
		business_charger_process_csd_pulse_change(charger, entry->event,
							  entry->data);
		break;
	case MCA_EVENT_SINK_PWR_SUSPEND_CHANGE:
		business_charger_process_snk_power_suspend(
			charger, entry->event, entry->data);
		break;
	case MCA_EVENT_USB_SUSPEND:
		business_charger_process_usb_suspend_change(
			charger, entry->event, entry->data);
		break;
	case MCA_EVENT_FG_OTA_PROCESS:
		business_charger_process_fg_ota(charger, entry->event,
						entry->data);
		break;
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
		business_charger_process_debug_double85(charger,
							*((int *)entry->data));
		break;
	case MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT:
		business_charger_process_debug_remove_temp_limit(
			charger, *((int *)entry->data));
		break;
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
		business_charger_process_debug_memory_test(
			charger, *((int *)entry->data));
		break;
	case MCA_EVENT_DEBUG_CTRL_SOC_LIMIT:
		business_charger_process_debug_soc_limit(charger,
							 *((int *)entry->data));
		break;
	case MCA_EVENT_SBU_LPD_CHANGE:
		business_charger_process_sbu_lpd_status_change(
			charger, *((int *)entry->data));
		break;
	}
}

static bool business_charger_next_event(struct business_charger *charger,
					struct charger_event *entry)
{
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&charger->event_lock, flags);
	if (!charger->stopping && charger->event_count) {
		*entry = charger->events[charger->event_head];
		charger->event_head = (charger->event_head + 1) %
				      BUSINESS_CHARGER_EVENT_DEPTH;
		charger->event_count--;
		found = true;
	}
	spin_unlock_irqrestore(&charger->event_lock, flags);
	return found;
}

static void business_charger_event_work(struct work_struct *work)
{
	struct business_charger *charger =
		container_of(work, struct business_charger, event_work);
	struct charger_event entry;
	unsigned int count;

	for (count = 0; count < BUSINESS_CHARGER_EVENT_DEPTH &&
			business_charger_next_event(charger, &entry);
	     count++) {
		guard(mutex)(&charger->policy_lock);
		if (!READ_ONCE(charger->stopping))
			business_charger_dispatch_event(charger, &entry);
	}
}

/* Standard usb_type definitions similar to power_supply_sysfs.c */
static const char *const power_supply_usb_type_text[] = {
	"Unknown", "SDP",	"CDP",	     "DCP", "USB_FLOAT", "HVDCP",
	"HVDCP_3", "HVDCP_3_B", "HVDCP_3P5", "C",   "PD",	 "PD_PPS",
	"PD_PPS",  "Unknown",	"ACA",	     "DCP",
};

static const char *get_usb_type_name(u32 usb_type)
{
	if (usb_type < ARRAY_SIZE(power_supply_usb_type_text))
		return power_supply_usb_type_text[usb_type];

	return "Unknown";
}

enum bussiness_charger_attr_list {
	BUSSINESS_CHARGER_PROP_REAL_TYPE,
	BUSSINESS_CHARGER_PROP_POWER_MAX,
	BUSSINESS_CHARGER_PROP_QUICK_CHARGE_TYPE,
	BUSSINESS_CHARGER_PROP_IS_EU_MODEL,
	BUSSINESS_CHARGER_PROP_PLATE_SHOCK,
	BUSSINESS_CHARGER_PROP_START_QUICK_REVCHG,
	BUSSINESS_CHARGER_PROP_REVCHG_BCL,
	BUSSINESS_CHARGER_PROP_HANDLE_STATE,
	BUSSINESS_CHARGER_PROP_STOP_HANDLE_CHARGE,
	BUSSINESS_CHARGER_PROP_DEBUG_CTRL,
};

static ssize_t bussiness_charger_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf);

static ssize_t bussiness_charger_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count);

struct mca_sysfs_attr_info bussiness_charger_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(bussiness_charger_sysfs, 0444,
			  BUSSINESS_CHARGER_PROP_REAL_TYPE, real_type),
	mca_sysfs_attr_ro(bussiness_charger_sysfs, 0444,
			  BUSSINESS_CHARGER_PROP_POWER_MAX, power_max),
	mca_sysfs_attr_ro(bussiness_charger_sysfs, 0444,
			  BUSSINESS_CHARGER_PROP_QUICK_CHARGE_TYPE,
			  quick_charge_type),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664,
			  BUSSINESS_CHARGER_PROP_IS_EU_MODEL, is_eu_model),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664,
			  BUSSINESS_CHARGER_PROP_PLATE_SHOCK, plate_shock),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664,
			  BUSSINESS_CHARGER_PROP_START_QUICK_REVCHG,
			  start_quick_revchg),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664,
			  BUSSINESS_CHARGER_PROP_REVCHG_BCL, revchg_bcl),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664,
			  BUSSINESS_CHARGER_PROP_HANDLE_STATE, handle_state),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664,
			  BUSSINESS_CHARGER_PROP_STOP_HANDLE_CHARGE,
			  stop_handle_charge),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664,
			  BUSSINESS_CHARGER_PROP_DEBUG_CTRL, debug_ctrl),
};

#define BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE \
	ARRAY_SIZE(bussiness_charger_sysfs_field_tbl)

static struct attribute
	*bussiness_charger_sysfs_attrs[BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group bussiness_charger_sysfs_attr_group = {
	.attrs = bussiness_charger_sysfs_attrs,
};
static int business_charger_get_power_max(struct business_charger *charger,
					  int *pdata)
{
	union power_supply_propval pval = {};
	int ret;

	*pdata = 0;
	if (charger->bc12_active) {
		*pdata = charger->wired_power_max;
		return 0;
	}
	if (!charger->wls_psy_info)
		return 0;
	ret = power_supply_get_property(charger->wls_psy_info->wireless_psy,
					POWER_SUPPLY_PROP_ONLINE, &pval);
	if (ret || !pval.intval)
		return ret;
	return mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					    STRATEGY_STATUS_TYPE_POWER_MAX,
					    pdata);
}

static int
business_charger_get_quick_charge_type(struct business_charger *charger,
				       int *pdata)
{
	int online = 0;
	int ret;

	*pdata = 0;
	if (charger->bc12_active) {
		*pdata = charger->wired_qucik_charge_type;
		return 0;
	}
	if (!charger->wls_psy_info)
		return 0;
	ret = mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					   STRATEGY_STATUS_TYPE_ONLINE,
					   &online);
	if (ret || !online)
		return ret;
	return mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					    STRATEGY_STATUS_TYPE_QC_TYPE,
					    pdata);
}

static int business_charger_partition_set_status(int val)
{
	charger_partition_info_2 info = { .eu_mode = val, .test = 0x34567890 };
	int ret, cleanup;

	ret = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_2, sizeof(info));
	if (ret)
		return ret;
	ret = charger_partition_write(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_2, &info,
				      sizeof(info));
	cleanup = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL,
					    CHARGER_PARTITION_INFO_2,
					    sizeof(info));
	return ret ? ret : cleanup;
}

struct business_debug_ctrl_cmd {
	const char *name;
	int argc;
};

static const struct business_debug_ctrl_cmd g_debug_ctrl[] = {
	{ "double85", 1 },
	{ "remove_temp_limit", 1 },
	{ "soc_limit", 2 },
	{ "memory_test", 1 },
};
static int
business_charger_process_debug_ctrl_input(struct business_charger *charger,
					  const char *buf)
{
	char *copy, *cursor, *token;
	char *argv[4];
	int argc = 0, v1, v2 = 0, packed, event, ret;

	copy = kstrdup(buf, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	cursor = strim(copy);
	while ((token = strsep(&cursor, " \t\n"))) {
		if (!*token)
			continue;
		if (argc == ARRAY_SIZE(argv)) {
			ret = -EINVAL;
			goto out;
		}
		argv[argc++] = token;
	}
	if (argc < 2 || kstrtoint(argv[1], 10, &v1)) {
		ret = -EINVAL;
		goto out;
	}
	if (!strcmp(argv[0], "soc_limit")) {
		if (argc != 3 || kstrtoint(argv[2], 10, &v2) || v1 < 0 ||
		    v1 > 100 || v2 < 0 || v2 > v1) {
			ret = -EINVAL;
			goto out;
		}
		packed = (v1 << 8) | v2;
		event = MCA_EVENT_DEBUG_CTRL_SOC_LIMIT;
		ret = charger_partition_write_soc_limit(packed);
	} else {
		if (argc != 2 || (v1 != 0 && v1 != 1)) {
			ret = -EINVAL;
			goto out;
		}
		packed = v1;
		if (!strcmp(argv[0], "double85")) {
			event = MCA_EVENT_DEBUG_CTRL_DOUBLE85;
			ret = charger_partition_write_double85(v1);
		} else if (!strcmp(argv[0], "remove_temp_limit")) {
			event = MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT;
			ret = charger_partition_write_remove_temp_limit(v1);
		} else if (!strcmp(argv[0], "memory_test")) {
			event = MCA_EVENT_DEBUG_CTRL_MEMORY_TEST;
			ret = charger_partition_write_memory_test(v1);
		} else {
			ret = -EINVAL;
			goto out;
		}
	}
	if (!ret)
		mca_event_block_notify(MCA_EVENT_TYPE_SUBPMIC_INFO, event,
				       &packed);
out:
	kfree(copy);
	return ret;
}

static ssize_t bussiness_charger_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *info;
	struct business_charger *charger = dev_get_drvdata(dev);
	int value, ret = 0;

	if (!charger)
		return -ENODEV;
	info = mca_sysfs_lookup_attr(attr->attr.name,
				     bussiness_charger_sysfs_field_tbl,
				     BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE);
	if (!info)
		return -EINVAL;
	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return -ENODEV;
	if (info->sysfs_attr_name == BUSSINESS_CHARGER_PROP_DEBUG_CTRL) {
		ret = business_charger_process_debug_ctrl_input(charger, buf);
		return ret ? ret : count;
	}
	if (kstrtoint(buf, 10, &value))
		return -EINVAL;
	if (info->sysfs_attr_name == BUSSINESS_CHARGER_PROP_REVCHG_BCL) {
		if (value < 0 || value > 30000)
			return -ERANGE;
	} else if (value != 0 && value != 1) {
		return -EINVAL;
	}
	/* These stock extension events have no downstream strategy handlers yet. */
	if (info->sysfs_attr_name == BUSSINESS_CHARGER_PROP_HANDLE_STATE ||
	    info->sysfs_attr_name ==
		    BUSSINESS_CHARGER_PROP_STOP_HANDLE_CHARGE ||
	    info->sysfs_attr_name ==
		    BUSSINESS_CHARGER_PROP_START_QUICK_REVCHG ||
	    info->sysfs_attr_name == BUSSINESS_CHARGER_PROP_REVCHG_BCL)
		return -EOPNOTSUPP;

	switch (info->sysfs_attr_name) {
	case BUSSINESS_CHARGER_PROP_IS_EU_MODEL:
		ret = business_charger_partition_set_status(value);
		if (ret)
			break;
		charger->is_eu_model = value;
		business_charger_process_is_eu_model(charger, value);
		break;
	case BUSSINESS_CHARGER_PROP_PLATE_SHOCK:
		charger->plate_shock = value;
		business_charger_process_plate_shock(charger, value);
		break;
	case BUSSINESS_CHARGER_PROP_START_QUICK_REVCHG:
		ret = business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE,
			MCA_EVENT_START_QUICK_REVCHG, value);
		if (!ret)
			charger->start_quick_revchg = value;
		break;
	case BUSSINESS_CHARGER_PROP_REVCHG_BCL:
		ret = business_charger_strategy_process(
			charger, STRATEGY_FUNC_TYPE_BUCK_CHARGE,
			MCA_EVENT_REVCHG_BCL, value);
		if (!ret)
			charger->revchg_bcl = value;
		break;
	case BUSSINESS_CHARGER_PROP_HANDLE_STATE:
		charger->handle_state = value;
		business_charger_process_handle_logic(charger);
		break;
	case BUSSINESS_CHARGER_PROP_STOP_HANDLE_CHARGE:
		charger->stop_handle_charge = value;
		business_charger_process_handle_logic(charger);
		break;
	default:
		return -EINVAL;
	}
	return ret ? ret : count;
}

static ssize_t bussiness_charger_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct mca_sysfs_attr_info *attr_info;
	struct business_charger *charger =
		(struct business_charger *)dev_get_drvdata(dev);
	ssize_t count = 0;
	int value = 0;
	int ret;

	if (!charger)
		return -ENODEV;
	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return -ENODEV;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  bussiness_charger_sysfs_field_tbl,
					  BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -EINVAL;

	switch (attr_info->sysfs_attr_name) {
	case BUSSINESS_CHARGER_PROP_REAL_TYPE:
		if (charger)
			count = snprintf(buf, PAGE_SIZE, "%s\n",
					 get_usb_type_name(charger->real_type));
		break;
	case BUSSINESS_CHARGER_PROP_POWER_MAX:
		ret = business_charger_get_power_max(charger, &value);
		if (ret)
			return ret;
		mca_log_info("power_max: %d\n", value);
		count = snprintf(buf, PAGE_SIZE, "%d\n", value);
		break;
	case BUSSINESS_CHARGER_PROP_QUICK_CHARGE_TYPE:
		ret = business_charger_get_quick_charge_type(charger, &value);
		if (ret)
			return ret;
		count = snprintf(buf, PAGE_SIZE, "%d\n", value);
		break;
	case BUSSINESS_CHARGER_PROP_IS_EU_MODEL:
		if (charger)
			count = snprintf(buf, PAGE_SIZE, "%d\n",
					 charger->is_eu_model);
		break;
	case BUSSINESS_CHARGER_PROP_PLATE_SHOCK:
		if (charger)
			count = snprintf(buf, PAGE_SIZE, "%d\n",
					 charger->plate_shock);
		break;
	case BUSSINESS_CHARGER_PROP_START_QUICK_REVCHG:
		if (charger)
			count = snprintf(buf, PAGE_SIZE, "%d\n",
					 charger->start_quick_revchg);
		break;
	case BUSSINESS_CHARGER_PROP_REVCHG_BCL:
		if (charger)
			count = snprintf(buf, PAGE_SIZE, "%d\n",
					 charger->revchg_bcl);
		break;
	case BUSSINESS_CHARGER_PROP_HANDLE_STATE:
		if (charger)
			count = snprintf(buf, PAGE_SIZE, "%d\n",
					 charger->handle_state);
		break;
	case BUSSINESS_CHARGER_PROP_STOP_HANDLE_CHARGE:
		if (charger)
			count = snprintf(buf, PAGE_SIZE, "%d\n",
					 charger->stop_handle_charge);
		break;
	case BUSSINESS_CHARGER_PROP_DEBUG_CTRL:
		count += sprintf(buf + count, "%s %d\n", g_debug_ctrl[0].name,
				 charger->last_double85);
		count += sprintf(buf + count, "%s %d\n", g_debug_ctrl[1].name,
				 charger->last_remove_temp_limit);
		count += sprintf(buf + count, "%s %d %d\n",
				 g_debug_ctrl[2].name,
				 charger->last_soc_limit_hi,
				 charger->last_soc_limit_lo);
		count += sprintf(buf + count, "%s %d\n", g_debug_ctrl[3].name,
				 charger->last_memory_test);
		break;
	default:
		break;
	}
	return count;
}

static int business_charger_sysfs_create_group(struct business_charger *charger)
{
	mca_sysfs_init_attrs(bussiness_charger_sysfs_attrs,
			     bussiness_charger_sysfs_field_tbl,
			     BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group(SYSFS_DEV_1, "charger_common",
					   charger->dev,
					   &bussiness_charger_sysfs_attr_group);
}

static void business_charger_sysfs_remove_group(struct device *dev)
{
	mca_sysfs_remove_link_group(SYSFS_DEV_1, "charger_common", dev,
				    &bussiness_charger_sysfs_attr_group);
}

static void business_charger_stop(struct business_charger *charger)
{
	unsigned long flags;

	if (!charger)
		return;
	mutex_lock(&charger->policy_lock);
	spin_lock_irqsave(&charger->event_lock, flags);
	if (charger->stopping) {
		spin_unlock_irqrestore(&charger->event_lock, flags);
		mutex_unlock(&charger->policy_lock);
		return;
	}
	WRITE_ONCE(charger->stopping, true);
	charger->ready = false;
	charger->event_count = 0;
	spin_unlock_irqrestore(&charger->event_lock, flags);
	mutex_unlock(&charger->policy_lock);

	if (charger->sysfs_registered)
		business_charger_sysfs_remove_group(charger->dev);
	if (charger->debugfs_registered)
		mca_debugfs_remove_group("business_charger");
	if (charger->log_registered)
		mca_log_charge_log_unregister(MCA_CHARGE_LOG_ID_BUSINESS_CHG,
					      charger);
	if (charger->notifiers_registered & BIT(5))
		mca_event_block_notify_unregister(MCA_EVENT_TYPE_SUBPMIC_INFO,
						  &charger->debug_nb);
	if (charger->notifiers_registered & BIT(4))
		mca_event_block_notify_unregister(MCA_EVENT_TYPE_CP_INFO,
						  &charger->cp_info_nb);
	if (charger->notifiers_registered & BIT(3))
		mca_event_block_notify_unregister(MCA_EVENT_CHARGE_STATUS,
						  &charger->chg_sts_nb);
	if (charger->notifiers_registered & BIT(2))
		mca_event_block_notify_unregister(MCA_EVENT_TYPE_HW_INFO,
						  &charger->hw_info_nb);
	if (charger->notifiers_registered & BIT(1))
		mca_event_block_notify_unregister(MCA_EVENT_TYPE_CHARGE_TYPE,
						  &charger->type_change_nb);
	if (charger->notifiers_registered & BIT(0))
		mca_event_block_notify_unregister(
			MCA_EVENT_TYPE_CHARGER_CONNECT, &charger->connect_nb);
	if (charger->workqueue) {
		cancel_delayed_work_sync(&charger->delay_report_status_work);
		cancel_delayed_work_sync(&charger->delay_enable_rx_work);
		cancel_delayed_work_sync(&charger->reset_rx_work);
		cancel_delayed_work_sync(
			&charger->report_quick_charge_type_work);
		cancel_delayed_work_sync(&charger->wireless_rerun_usb_sns_work);
		cancel_delayed_work_sync(&charger->pmic_init_done_notify_work);
		cancel_work_sync(&charger->event_work);
		destroy_workqueue(charger->workqueue);
		charger->workqueue = NULL;
	}
	business_charger_wireless_psy_deinit(charger->wls_psy_info);
	business_charger_usb_psy_deinit(charger->usb_psy_info);
	if (charger->online_wake_lock) {
		__pm_relax(charger->online_wake_lock);
		wakeup_source_unregister(charger->online_wake_lock);
		charger->online_wake_lock = NULL;
	}
	mutex_lock(&business_owner_lock);
	if (business_owner == charger)
		business_owner = NULL;
	mutex_unlock(&business_owner_lock);
}

static ssize_t business_debugfs_show(void *priv_data, char *buf)
{
	struct mca_debugfs_attr_data *attr_data =
		(struct mca_debugfs_attr_data *)priv_data;
	struct mca_debugfs_attr_info *attr_info = attr_data->attr_info;
	struct business_charger *charger =
		(struct business_charger *)attr_data->private;
	size_t count = 0;

	if (!charger || !attr_info) {
		mca_log_err("null pointer store\n");
		return count;
	}

	if (!charger)
		return -ENODEV;
	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return -ENODEV;

	switch (attr_info->debugfs_attr_name) {
	case BUSINESS_CHARGER_DEBUG_PROP_DAM_TEST:
		count = scnprintf(buf, PAGE_SIZE, "%d\n",
				  charger->dam_test_flag);
		break;
	default:
		break;
	}

	return count;
}

static ssize_t business_debugfs_store(void *priv_data, const char *buf,
				      size_t count)
{
	struct mca_debugfs_attr_data *attr_data =
		(struct mca_debugfs_attr_data *)priv_data;
	struct mca_debugfs_attr_info *attr_info = attr_data->attr_info;
	struct business_charger *charger =
		(struct business_charger *)attr_data->private;
	int val;

	if (!charger)
		return -ENODEV;
	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return -ENODEV;

	if (!attr_info || kstrtoint(buf, 16, &val) || (val != 0 && val != 1))
		return -EINVAL;

	switch (attr_info->debugfs_attr_name) {
	case BUSINESS_CHARGER_DEBUG_PROP_DAM_TEST:
		charger->dam_test_flag = val;
		if (charger->dam_test_flag) {
			mca_log_info("relax wake_lock\n");
			__pm_relax(charger->online_wake_lock);
		} else if (charger->bc12_active) {
			__pm_stay_awake(charger->online_wake_lock);
			mca_log_info("stay wake_lock\n");
		}
		break;
	default:
		break;
	}

	return count;
}

static struct mca_debugfs_attr_info g_business_charge_debugfs_field_tbl[] = {
	mca_debugfs_attr(business_debugfs, 0664,
			 BUSINESS_CHARGER_DEBUG_PROP_DAM_TEST, dam_test),
};
#define BUSINESS_CHARGER_DEBUGFS_ATTRS_SIZE \
	ARRAY_SIZE(g_business_charge_debugfs_field_tbl)

static int business_charge_dump_log_head(void *data, char *buf, int size)
{
	return snprintf(
		buf, size,
		"Online ChargeType ChargeEnable ChargeMode Vbus   Ibus   BTB_Status Batt_Auth ");
}
static int business_charge_dump_log_context(void *data, char *buf, int size)
{
	struct business_charger *charger = data;
	int online = 0, chg_type = 0, mode = 0, vbus = 0, ibus = 0;
	int enable = 0, temp, type, ret;

	if (!charger)
		return -ENODEV;
	guard(mutex)(&charger->policy_lock);
	if (READ_ONCE(charger->stopping))
		return -ENODEV;
	if (charger->bc12_active || charger->wls_active) {
		online = charger->bc12_active ? 1 : 2;
		chg_type = charger->bc12_active ? charger->real_type :
						  charger->wls_adapter_type;
		type = charger->bc12_active ? STRATEGY_FUNC_TYPE_QUICK_CHARGE :
					      STRATEGY_FUNC_TYPE_QUICK_WIRELESS;
		ret = mca_strategy_func_get_status(
			type, STRATEGY_STATUS_TYPE_MODE, &mode);
		if (ret)
			return ret;
		ret = mca_strategy_func_get_status(
			type, STRATEGY_STATUS_TYPE_ENABLE, &temp);
		if (ret)
			return ret;
		if (temp < 0 || temp > 0xf)
			return -ERANGE;
		enable = temp << (charger->bc12_active ? 4 : 14);
	}
	ret = mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					   STRATEGY_STATUS_TYPE_ENABLE, &temp);
	if (ret)
		return ret;
	if (temp < 0 || temp > 0xf)
		return -ERANGE;
	enable |= temp;
	if (mode) {
		ret = mca_strategy_func_get_status(
			type, STRATEGY_STATUS_TYPE_VBUS, &vbus);
		if (!ret)
			ret = mca_strategy_func_get_status(
				type, STRATEGY_STATUS_TYPE_IBUS, &ibus);
	} else {
		ret = platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER,
							      &vbus);
		if (!ret)
			ret = platform_class_buckchg_ops_get_bus_curr(
				MAIN_BUCK_CHARGER, &ibus);
		vbus /= 1000;
		ibus /= 1000;
	}
	if (ret)
		return ret;
	return scnprintf(buf, size, "%-7d%-11d0x%-11x%-11d%-7d%-7d%-11d%-10d",
			 online, chg_type, enable, mode, vbus, ibus,
			 charger->abnormal_info.batt_missing,
			 !charger->abnormal_info.batt_auth_failed);
}

static struct mca_log_charge_log_ops g_business_charger_log_ops = {
	.dump_log_head = business_charge_dump_log_head,
	.dump_log_context = business_charge_dump_log_context,
};
static int business_charger_probe(struct platform_device *pdev)
{
	struct business_charger *charger;
	unsigned long flags;
	int rc;

	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;
	charger->dev = &pdev->dev;
	platform_set_drvdata(pdev, charger);
	mutex_init(&charger->policy_lock);
	spin_lock_init(&charger->event_lock);
	INIT_WORK(&charger->event_work, business_charger_event_work);
	INIT_DELAYED_WORK(&charger->delay_report_status_work,
			  business_charger_wireless_report_status_work);
	INIT_DELAYED_WORK(&charger->delay_enable_rx_work,
			  business_charger_wireless_delay_enable_rx_work);
	INIT_DELAYED_WORK(&charger->reset_rx_work,
			  business_charger_wireless_reset_rx_work);
	INIT_DELAYED_WORK(&charger->report_quick_charge_type_work,
			  business_charger_report_quick_charge_type_work);
	INIT_DELAYED_WORK(&charger->wireless_rerun_usb_sns_work,
			  business_charger_wireless_rerun_usb_sns_work);
	INIT_DELAYED_WORK(&charger->pmic_init_done_notify_work,
			  business_charger_pmic_init_done_notify_work);
	charger->abnormal_info.batt_auth_failed = 1;
	rc = business_charger_parse_dt(charger);
	if (rc)
		goto fail;
	mutex_lock(&business_owner_lock);
	if (business_owner) {
		mutex_unlock(&business_owner_lock);
		rc = -EBUSY;
		goto fail;
	}
	business_owner = charger;
	mutex_unlock(&business_owner_lock);
	charger->workqueue =
		alloc_ordered_workqueue("mca_business_charger", WQ_MEM_RECLAIM);
	if (!charger->workqueue) {
		rc = -ENOMEM;
		goto fail;
	}
	charger->online_wake_lock =
		wakeup_source_register(charger->dev, "charger_wakelock");
	if (!charger->online_wake_lock) {
		rc = -ENOMEM;
		goto fail;
	}
	charger->usb_psy_info = business_charger_usb_psy_init(charger->dev);
	if (IS_ERR(charger->usb_psy_info)) {
		rc = PTR_ERR(charger->usb_psy_info);
		charger->usb_psy_info = NULL;
		goto fail;
	}
	if (charger->wls_support) {
		charger->wls_psy_info =
			business_charger_wireless_psy_init(charger->dev);
		if (IS_ERR(charger->wls_psy_info)) {
			rc = PTR_ERR(charger->wls_psy_info);
			charger->wls_psy_info = NULL;
			goto fail;
		}
	}

	charger->connect_nb.notifier_call =
		business_charger_notifier_connect_cb;
	rc = mca_event_block_notify_register(MCA_EVENT_TYPE_CHARGER_CONNECT,
					     &charger->connect_nb);
	if (rc)
		goto fail;
	charger->notifiers_registered |= BIT(0);

	charger->type_change_nb.notifier_call =
		business_charger_notifier_type_cb;
	rc = mca_event_block_notify_register(MCA_EVENT_TYPE_CHARGE_TYPE,
					     &charger->type_change_nb);
	if (rc)
		goto fail;
	charger->notifiers_registered |= BIT(1);

	charger->hw_info_nb.notifier_call = business_charger_notifier_hw_cb;
	rc = mca_event_block_notify_register(MCA_EVENT_TYPE_HW_INFO,
					     &charger->hw_info_nb);
	if (rc)
		goto fail;
	charger->notifiers_registered |= BIT(2);

	charger->chg_sts_nb.notifier_call =
		business_charger_notifier_chg_sts_cb;
	rc = mca_event_block_notify_register(MCA_EVENT_CHARGE_STATUS,
					     &charger->chg_sts_nb);
	if (rc)
		goto fail;
	charger->notifiers_registered |= BIT(3);

	charger->cp_info_nb.notifier_call =
		business_charger_notifier_cp_info_cb;
	rc = mca_event_block_notify_register(MCA_EVENT_TYPE_CP_INFO,
					     &charger->cp_info_nb);
	if (rc)
		goto fail;
	charger->notifiers_registered |= BIT(4);

	charger->debug_nb.notifier_call = business_charger_notifier_debug_cb;
	rc = mca_event_block_notify_register(MCA_EVENT_TYPE_SUBPMIC_INFO,
					     &charger->debug_nb);
	if (rc)
		goto fail;
	charger->notifiers_registered |= BIT(5);

	rc = business_charger_sysfs_create_group(charger);
	if (rc)
		goto fail;
	charger->sysfs_registered = true;
	rc = mca_debugfs_create_group("business_charger",
				      g_business_charge_debugfs_field_tbl,
				      BUSINESS_CHARGER_DEBUGFS_ATTRS_SIZE,
				      charger);
	if (rc && rc != -ENODEV)
		goto fail;
	charger->debugfs_registered = !rc;
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_BUSINESS_CHG,
				    &g_business_charger_log_ops, charger);
	charger->log_registered = true;
	spin_lock_irqsave(&charger->event_lock, flags);
	charger->ready = true;
	queue_work(charger->workqueue, &charger->event_work);
	spin_unlock_irqrestore(&charger->event_lock, flags);
	business_charger_add_event_node(MCA_EVENT_USB_CONNECT, NULL, 0,
					charger);
	if (charger->wls_support) {
		business_charger_add_event_node(MCA_EVENT_WIRELESS_CONNECT,
						NULL, 0, charger);
		mod_delayed_work(charger->workqueue,
				 &charger->wireless_rerun_usb_sns_work,
				 msecs_to_jiffies(2500));
	}
	mod_delayed_work(charger->workqueue,
			 &charger->pmic_init_done_notify_work, 0);
	return 0;
fail:
	business_charger_stop(charger);
	platform_set_drvdata(pdev, NULL);
	return rc;
}

static void business_charger_remove(struct platform_device *pdev)
{
	business_charger_stop(platform_get_drvdata(pdev));
	platform_set_drvdata(pdev, NULL);
}

static void business_charger_shutdown(struct platform_device *pdev)
{
	business_charger_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,business_charger" },
	{},
};

MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver business_charger_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "business_charger",
		.of_match_table = match_table,
	},
	.probe = business_charger_probe,
	.remove = business_charger_remove,
	.shutdown = business_charger_shutdown,
};

static int __init business_charger_init(void)
{
	return platform_driver_register(&business_charger_driver);
}
module_init(business_charger_init);

static void __exit business_charger_exit(void)
{
	platform_driver_unregister(&business_charger_driver);
}
module_exit(business_charger_exit);

MODULE_DESCRIPTION("business charger core");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
