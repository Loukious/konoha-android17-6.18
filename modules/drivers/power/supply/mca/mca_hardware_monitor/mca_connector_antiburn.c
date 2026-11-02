// SPDX-License-Identifier: GPL-2.0
/*
 * mca_connector_antiburn.c
 *
 * connector antiburn driver
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
#include <linux/gpio/consumer.h>
#include <linux/srcu.h>
#include <linux/mutex.h>
#include <linux/math64.h>
#include <mca/common/mca_voter.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/ktime.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_workqueue.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/strategy/strategy_class.h>
#include "inc/mca_connector_antiburn.h"

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_connector_antiburn"
#endif

#define CONNECTOR_ANTIBURN_WORK_INTERVAL_FAST 1000
#define CONNECTOR_ANTIBURN_WORK_INTERVAL_NORMAL 5000

#define CONNECTOR_ANTIBURN_TRIGGER_TEMP 65
#define CONNECTOR_ANTIBURN_RECHARGE_TEMP 55
#define CONNECTOR_ANTIBURN_TEMPERATURE_INCREASE_RATE 4
#define NTC_SCALE_TEMP 1000
#define COMBINED_SENSOR_BOARD_CONNECTOR_ANTIBURN_TRIGGER_TEMP 60
#define COMBINED_RATE_CONNECTOR_ANTIBURN_TRIGGER_TEMP 35
#define THERMAL_SENSOR_BOARD_TRIGGER_TEMP 50

#define VBUS_SENSE_DEFAULT_UV 6000000
#define VBUS_DISABLE_VOLT_UV 3600000
#define VBUS_UV_THRESHOLD 4100000
#define VBUS_DELAY_TIME 50
#define VBUS_SENSE_MAX_COUNT 5

static struct connector_antiburn __rcu *g_conn;
static DEFINE_MUTEX(conn_provider_lock);
DEFINE_STATIC_SRCU(conn_provider_srcu);
static bool conn_claimed;
int connector_antiburn_is_triggered(void)
{
	struct connector_antiburn *conn;

	guard(srcu)(&conn_provider_srcu);

	conn = srcu_dereference(g_conn, &conn_provider_srcu);
	if (!conn || READ_ONCE(conn->stopping))
		return -ENODEV;
	return READ_ONCE(conn->triggered);
}
EXPORT_SYMBOL(connector_antiburn_is_triggered);

static void connector_temp_uevent(int temp)
{
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	int len = 0;
	struct mca_event_notify_data event_data;

	mca_log_info("connector temp uevent notify\n");

	len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
		       "POWER_SUPPLY_CONNECTOR_TEMP=%d", temp);
	event_data.event = event;
	event_data.event_len = len;
	mca_event_report_uevent(&event_data);
}

static void adapter_reset_vsafe0V_uevent(int is_reset_vsafe0V)
{
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	int len = 0;
	struct mca_event_notify_data event_data;

	mca_log_info("adapter vbus drop 0V notify\n");

	len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
		       "POWER_SUPPLY_ADAPTER_RESET_VSAFE0V=%d",
		       is_reset_vsafe0V);
	event_data.event = event;
	event_data.event_len = len;
	mca_event_report_uevent(&event_data);
}

static void connector_ntc_alarm_uevent(int ntc_alarm)
{
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	int len = 0;
	struct mca_event_notify_data event_data;

	mca_log_info("ntc alarm notify\n");

	len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
		       "POWER_SUPPLY_NTC_ALARM=%d", ntc_alarm);
	event_data.event = event;
	event_data.event_len = len;
	mca_event_report_uevent(&event_data);
}
static int connector_antiburn_get_temperature(struct connector_antiburn *conn,
					      int index, int *value)
{
	struct thermal_zone_device *zone;
	int temperature;
	int ret;

	if (!value || index < 0 || index >= CONNECTOR_PROP_TEMP_MAX ||
	    (index == CONNECTOR_PROP_TEMP_2 && !conn->use_double_ntc))
		return -EINVAL;
	if (conn->fake_connector_temp[index]) {
		*value = conn->fake_connector_temp[index];
		return 0;
	}
	zone = index == CONNECTOR_PROP_TEMP_1 ? conn->tzd_conn :
						conn->tzd_conn2;
	ret = thermal_zone_get_temp(zone, &temperature);
	if (ret)
		return ret;
	if (temperature < -50000 || temperature > 150000)
		return -ERANGE;
	*value = temperature / NTC_SCALE_TEMP;
	return 0;
}
static int
connector_antiburn_get_temp_increase_rate(struct connector_antiburn *conn)
{
	ktime_t now = ktime_get();
	s64 elapsed = ktime_to_ms(ktime_sub(now, conn->last_sample_time));
	int count = conn->use_double_ntc ? CONNECTOR_PROP_TEMP_MAX : 1;
	int index;

	for (index = 0; index < count; index++) {
		if (!conn->have_sample || elapsed <= 0)
			conn->temp_increase_rate[index] = 0;
		else
			conn->temp_increase_rate[index] =
				div64_s64((s64)(conn->temperature[index] -
					      conn->last_temperature[index]) *
						1000,
					elapsed);
		conn->last_temperature[index] = conn->temperature[index];
	}
	conn->last_sample_time = now;
	conn->have_sample = true;
	return 0;
}

static void
connector_antiburn_ensure_vbus_sense5V(struct connector_antiburn *conn)
{
	int i;
	int bus_volt = 0;
	int real_type = XM_CHARGER_TYPE_UNKNOW;
	unsigned int data = 0;

	for (i = 0; i < VBUS_SENSE_MAX_COUNT; i++) {
		protocol_class_get_adapter_type(ADAPTER_PROTOCOL_PD,
						&real_type);
		mca_log_err("real_type is %d, bus_volt is %d\n", real_type,
			    bus_volt);
		switch (real_type) {
		case XM_CHARGER_TYPE_HVDCP2:
		case XM_CHARGER_TYPE_HVDCP3:
		case XM_CHARGER_TYPE_HVDCP3_B:
		case XM_CHARGER_TYPE_HVDCP3P5:
			platform_class_buckchg_ops_get_bus_volt(
				MAIN_BUCK_CHARGER, &bus_volt);
			if (bus_volt < VBUS_SENSE_DEFAULT_UV)
				return;
			msleep(200);
			break;
		case XM_CHARGER_TYPE_PD:
		case XM_CHARGER_TYPE_PD_VERIFY:
		case XM_CHARGER_TYPE_PPS:
			protocol_class_pd_request_vdm_cmd(
				TYPEC_PORT_0, USBPD_UVDM_RESET_VSAFE0V, &data,
				0);
			msleep(200);
			platform_class_buckchg_ops_get_bus_volt(
				MAIN_BUCK_CHARGER, &bus_volt);
			if (bus_volt < VBUS_SENSE_DEFAULT_UV) {
				if (bus_volt < VBUS_DISABLE_VOLT_UV) {
					conn->is_reset_vsafe0V = 1;
					adapter_reset_vsafe0V_uevent(
						conn->is_reset_vsafe0V);
				}
				return;
			}
			break;
		default:
			return;
		}
	}
}

static int connector_antiburn_check_status(struct connector_antiburn *conn)
{
	int connector_temp;
	int temp_increase_rate;
	int bus_volt = 0;
	int real_type = XM_CHARGER_TYPE_UNKNOW;
	int ret;
	unsigned int data = 0;

	connector_temp = conn->use_double_ntc ? max(conn->temperature[0],
						    conn->temperature[1]) :
						conn->temperature[0];
	if (connector_temp < 26)
		connector_temp = 25;
	/* rate of whichever sensor is currently the hotter one */
	temp_increase_rate = (conn->use_double_ntc &&
			      conn->temperature[0] < conn->temperature[1]) ?
				     conn->temp_increase_rate[1] :
				     conn->temp_increase_rate[0];

	ret = protocol_class_pd_get_otg_plugin_status(TYPEC_PORT_0,
						      &conn->otg_plugin_status);
	if (ret)
		return ret;
	ret = platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER,
						    &conn->usb_online);
	if (ret)
		return ret;
	ret = protocol_class_pd_get_cid_status(TYPEC_PORT_0, &conn->cid_status);
	if (ret)
		return ret;

	mca_log_info(
		"connector_temp:%d triggered:%d, cid_status:%d, otg_plugin_status: %d, usb_online:%d\n",
		connector_temp, conn->triggered, conn->cid_status,
		conn->otg_plugin_status, conn->usb_online);

	if ((connector_temp >= conn->trigger_temp ||
	     (connector_temp >= conn->comb_sensorboard_con_trigger_temp &&
	      READ_ONCE(conn->thermal_board_temp) <=
		      conn->max_thermal_board_temp) ||
	     (connector_temp >= conn->comb_rate_conn_trigger_temp &&
	      temp_increase_rate >= conn->max_temp_increase_rate)) &&
	    !conn->triggered && !READ_ONCE(conn->disable_antiburn) &&
	    (conn->cid_status || conn->usb_online)) {
		mca_log_err(
			"triggering antiburn conn->usb_online is %d, conn->otg_plugin_status is %d\n",
			conn->usb_online, conn->otg_plugin_status);
		mca_log_err(
			"usb_online: %d, otg_plugin_status: %d, conn_therm: %d/%d, temp_increase_rate: %d, thermal_board_temp: %d\n",
			conn->usb_online, conn->otg_plugin_status,
			conn->temperature[0], conn->temperature[1],
			temp_increase_rate,
			READ_ONCE(conn->thermal_board_temp));
		WRITE_ONCE(conn->triggered, 1);
		conn->ntc_alarm = 1;
		connector_temp_uevent(connector_temp * 10);
		connector_ntc_alarm_uevent(conn->ntc_alarm);

		protocol_class_get_adapter_type(ADAPTER_PROTOCOL_PD,
						&real_type);
		if (real_type == XM_CHARGER_TYPE_PD ||
		    real_type == XM_CHARGER_TYPE_PD_VERIFY ||
		    real_type == XM_CHARGER_TYPE_PPS) {
			protocol_class_pd_request_vdm_cmd(
				TYPEC_PORT_0, USBPD_UVDM_RESET_VSAFE0V, &data,
				0);
			msleep(VBUS_DELAY_TIME);
			mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
					       MCA_EVENT_CONN_ANTIBURN_CHANGE,
					       NULL);
			mca_charge_mievent_report(
				CHARGE_DFX_ANTI_BURN_TRIGGERED, &connector_temp,
				1);
			msleep(200);
			connector_antiburn_ensure_vbus_sense5V(conn);
			platform_class_buckchg_ops_get_bus_volt(
				MAIN_BUCK_CHARGER, &bus_volt);
			if (bus_volt < VBUS_DISABLE_VOLT_UV) {
				conn->is_reset_vsafe0V = 1;
				adapter_reset_vsafe0V_uevent(
					conn->is_reset_vsafe0V);
			}
		} else {
			mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
					       MCA_EVENT_CONN_ANTIBURN_CHANGE,
					       NULL);
			mca_charge_mievent_report(
				CHARGE_DFX_ANTI_BURN_TRIGGERED, &connector_temp,
				1);
			msleep(200);
			connector_antiburn_ensure_vbus_sense5V(conn);
		}

		if (conn->otg_plugin_status)
			platform_class_buckchg_ops_set_boost_enable(
				MAIN_BUCK_CHARGER,
				(conn->en_src << 16) |
					(conn->otg_boost_src << 8));

		if (conn->support_hw_antiburn) {
			ret = gpiod_direction_output_raw(conn->mos_ctrl, 1);
			if (ret)
				return ret;
			mca_log_err("triggering hw anti burn\n");
			msleep(VBUS_DELAY_TIME);
			platform_class_buckchg_ops_get_bus_volt(
				MAIN_BUCK_CHARGER, &bus_volt);
			if (bus_volt >= VBUS_UV_THRESHOLD)
				mca_charge_mievent_report(
					CHARGE_DFX_ANTIBURN_ERR, NULL, 0);
		}
	} else if (connector_temp < conn->recharge_temp &&
		   temp_increase_rate < conn->max_temp_increase_rate &&
		   conn->triggered && !conn->otg_plugin_status &&
		   !conn->cid_status) {
		mca_log_err(
			"recovery antiburn conn->usb_online is %d, conn->otg_plugin_status is %d, conn->cid_status is %d\n",
			conn->usb_online, conn->otg_plugin_status,
			conn->cid_status);
		if (conn->support_hw_antiburn) {
			ret = gpiod_direction_output_raw(conn->mos_ctrl, 0);
			if (ret)
				return ret;
			mca_log_err("close hw anti burn\n");
		}
		WRITE_ONCE(conn->triggered, 0);
		conn->is_reset_vsafe0V = 0;
		connector_temp_uevent(connector_temp * 10);
		adapter_reset_vsafe0V_uevent(conn->is_reset_vsafe0V);
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_CONN_ANTIBURN_CHANGE, NULL);
		mca_charge_mievent_set_state(MIEVENT_STATE_END,
					     CHARGE_DFX_ANTI_BURN_TRIGGERED);
	}

	if (conn->ntc_alarm == 1 && conn->cid_status == 0) {
		conn->ntc_alarm = 0;
		connector_temp_uevent(connector_temp * 10);
		connector_ntc_alarm_uevent(conn->ntc_alarm);
	}

	if (conn->otg_plugin_status || conn->usb_online == 1)
		conn->monitor_interval = CONNECTOR_ANTIBURN_WORK_INTERVAL_FAST;
	else
		conn->monitor_interval =
			CONNECTOR_ANTIBURN_WORK_INTERVAL_NORMAL;
	return 0;
}
static void connector_antiburn_monitor_workfunc(struct work_struct *work)
{
	struct connector_antiburn *conn = container_of(
		work, struct connector_antiburn, monitor_work.work);
	int count = conn->use_double_ntc ? CONNECTOR_PROP_TEMP_MAX : 1;
	int temperature[CONNECTOR_PROP_TEMP_MAX];
	int ret = 0;
	int index;

	guard(mutex)(&conn->policy_lock);

	if (READ_ONCE(conn->stopping))
		return;
	for (index = 0; index < count; index++) {
		ret = connector_antiburn_get_temperature(conn, index,
							 &temperature[index]);
		if (ret)
			goto retry;
	}
	for (index = 0; index < count; index++)
		conn->temperature[index] = temperature[index];
	connector_antiburn_get_temp_increase_rate(conn);
	ret = connector_antiburn_check_status(conn);
retry:
	if (ret)
		conn->have_sample = false;
	/* A failed sensor/status read cannot restore charging from cached values. */
	mca_vote_named("chg_enable", "antiburn_sensor_error", ret != 0, 0);
	queue_delayed_work(conn->wq, &conn->monitor_work,
			   msecs_to_jiffies(conn->monitor_interval));
}
static int
connector_antiburn_mos_ctrl_gpio_init(struct connector_antiburn *conn)
{
	conn->mos_ctrl = devm_gpiod_get(conn->dev, "mos-ctrl", GPIOD_ASIS);
	if (IS_ERR(conn->mos_ctrl))
		return PTR_ERR(conn->mos_ctrl);
	return gpiod_direction_output_raw(conn->mos_ctrl, 0);
}

static int connector_antiburn_gpio_init(struct connector_antiburn *conn)
{
	mca_log_info("Hw antiburn init gpio\n");

	if (!conn->support_hw_antiburn) {
		mca_log_info("No gpio config\n");
		return 0;
	}

	return connector_antiburn_mos_ctrl_gpio_init(conn);
}

static void connector_antiburn_parse_dt(struct connector_antiburn *conn)
{
	struct device_node *np = conn->dev->of_node;

	mca_parse_dts_u32(np, "trigger_temp", &conn->trigger_temp,
			  CONNECTOR_ANTIBURN_TRIGGER_TEMP);
	mca_parse_dts_u32(np, "recharge_temp", &conn->recharge_temp,
			  CONNECTOR_ANTIBURN_RECHARGE_TEMP);
	mca_parse_dts_u32(np, "support_soft_antiburn",
			  &conn->support_soft_antiburn, 1);
	mca_parse_dts_u32(np, "support_hw_antiburn", &conn->support_hw_antiburn,
			  1);
	mca_parse_dts_u32(np, "use_double_ntc", &conn->use_double_ntc, 0);
	mca_parse_dts_u32(np, "antiburn_otg_detect", &conn->otg_detect_en, 1);
	mca_parse_dts_u32(np, "monitor_interval", &conn->monitor_interval,
			  CONNECTOR_ANTIBURN_WORK_INTERVAL_FAST);
	mca_parse_dts_u32(np, "max_temp_increase_rate",
			  &conn->max_temp_increase_rate,
			  CONNECTOR_ANTIBURN_TEMPERATURE_INCREASE_RATE);
	mca_parse_dts_string(np, "thermal-zone-name", &conn->thermal_zone_name);
	mca_parse_dts_string(np, "thermal-zone-name2",
			     &conn->thermal_zone_name2);
	mca_parse_dts_u32(
		np, "comb_sensorboard_con_trigger_temp",
		&conn->comb_sensorboard_con_trigger_temp,
		COMBINED_SENSOR_BOARD_CONNECTOR_ANTIBURN_TRIGGER_TEMP);
	mca_parse_dts_u32(np, "comb_rate_conn_trigger_temp",
			  &conn->comb_rate_conn_trigger_temp,
			  COMBINED_RATE_CONNECTOR_ANTIBURN_TRIGGER_TEMP);
	mca_parse_dts_u32(np, "max_thermal_board_temp",
			  &conn->max_thermal_board_temp,
			  THERMAL_SENSOR_BOARD_TRIGGER_TEMP);
	mca_parse_dts_u32(np, "otg_boost_src", &conn->otg_boost_src,
			  EXTERNAL_BOOST);
	mca_parse_dts_u32(np, "en_src", &conn->en_src, OTG_EN_BOOST);
	conn->support_base_flip =
		of_property_read_bool(np, "support-base-flip");
}

static ssize_t antiburn_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t antiburn_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

struct mca_sysfs_attr_info antiburn_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_TEMP_1,
			  connector_temp_1),
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_TEMP_2,
			  connector_temp_2),
	mca_sysfs_attr_ro(antiburn_sysfs, 0444, CONNECTOR_PROP_RESET_VSAFE0V,
			  reset_vsafe0V),
	mca_sysfs_attr_ro(antiburn_sysfs, 0444, CONNECTOR_PROP_NTC_ALARM,
			  ntc_alarm),
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_MOS_CTRL,
			  mos_ctrl),
};

#define ANTIBURN_SYSFS_ATTRS_SIZE ARRAY_SIZE(antiburn_sysfs_field_tbl)

static struct attribute *antiburn_sysfs_attrs[ANTIBURN_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group antiburn_sysfs_attr_group = {
	.attrs = antiburn_sysfs_attrs,
};
static ssize_t antiburn_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct connector_antiburn *conn = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	int value;
	int ret;

	if (!conn)
		return -ENODEV;
	guard(mutex)(&conn->policy_lock);
	if (READ_ONCE(conn->stopping))
		return -ENODEV;
	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  antiburn_sysfs_field_tbl,
					  ANTIBURN_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -EINVAL;
	switch (attr_info->sysfs_attr_name) {
	case CONNECTOR_PROP_TEMP_1:
	case CONNECTOR_PROP_TEMP_2:
		ret = connector_antiburn_get_temperature(
			conn, attr_info->sysfs_attr_name, &value);
		if (ret)
			return ret;
		value *= 10;
		break;
	case CONNECTOR_PROP_RESET_VSAFE0V:
		value = conn->is_reset_vsafe0V;
		break;
	case CONNECTOR_PROP_NTC_ALARM:
		value = conn->ntc_alarm;
		break;
	case CONNECTOR_PROP_MOS_CTRL:
		if (!conn->support_hw_antiburn)
			return -EOPNOTSUPP;
		value = gpiod_get_raw_value_cansleep(conn->mos_ctrl);
		if (value < 0)
			return value;
		break;
	default:
		return -EINVAL;
	}
	return sysfs_emit(buf, "%d\n", value);
}
static ssize_t antiburn_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct connector_antiburn *conn = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	int value;
	int ret = 0;

	if (!conn)
		return -ENODEV;
	guard(mutex)(&conn->policy_lock);
	if (READ_ONCE(conn->stopping))
		return -ENODEV;
	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  antiburn_sysfs_field_tbl,
					  ANTIBURN_SYSFS_ATTRS_SIZE);
	if (!attr_info || kstrtoint(buf, 10, &value))
		return -EINVAL;
	switch (attr_info->sysfs_attr_name) {
	case CONNECTOR_PROP_TEMP_1:
	case CONNECTOR_PROP_TEMP_2:
		if (value < -500 || value > 1500 ||
		    (attr_info->sysfs_attr_name == CONNECTOR_PROP_TEMP_2 &&
		     !conn->use_double_ntc))
			return -EINVAL;
		conn->fake_connector_temp[attr_info->sysfs_attr_name] =
			value / 10;
		conn->have_sample = false;
		mod_delayed_work(conn->wq, &conn->monitor_work, 0);
		break;
	case CONNECTOR_PROP_MOS_CTRL:
		if (!conn->support_hw_antiburn)
			return -EOPNOTSUPP;
		if (value < 0 || value > 1)
			return -EINVAL;
		ret = gpiod_direction_output_raw(conn->mos_ctrl, value);
		break;
	default:
		return -EINVAL;
	}
	return ret ? ret : count;
}

static int antiburn_sysfs_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(antiburn_sysfs_attrs, antiburn_sysfs_field_tbl,
			     ANTIBURN_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group(SYSFS_DEV_5, "connector", dev,
					   &antiburn_sysfs_attr_group);
}

static int connector_antiburn_thermal_notifier_event(struct notifier_block *nb,
						     unsigned long chg_event,
						     void *val)
{
	struct connector_antiburn *conn =
		container_of(nb, struct connector_antiburn, thermal_board_nb);

	if (!val || READ_ONCE(conn->stopping))
		return NOTIFY_DONE;
	switch (chg_event) {
	case MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE:
		WRITE_ONCE(conn->thermal_board_temp,
			   *(int *)val / NTC_SCALE_TEMP);
		mca_log_info("get thermal_board_temp: %d\n",
			     READ_ONCE(conn->thermal_board_temp));
		break;
	default:
		mca_log_info(
			"not supported thermal board notifier event: %lu\n",
			chg_event);
		break;
	}
	return NOTIFY_DONE;
}

static int connector_antiburn_debug_notifier_cb(struct notifier_block *nb,
						unsigned long chg_event,
						void *val)
{
	struct connector_antiburn *conn =
		container_of(nb, struct connector_antiburn, debug_nb);

	if (!val || READ_ONCE(conn->stopping))
		return NOTIFY_DONE;
	switch (chg_event) {
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
	case MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT:
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
		WRITE_ONCE(conn->disable_antiburn, !!*(int *)val);
		mca_log_info("debug[%lu] disable_antiburn: %d\n", chg_event,
			     conn->disable_antiburn);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static int connector_antiburn_dump_log_head(void *data, char *buf, int size)
{
	return snprintf(buf, size, "port_temp port_temp1 shell_temp ");
}
static int connector_antiburn_dump_log_context(void *data, char *buf, int size)
{
	struct connector_antiburn *conn = data;
	int first, second = -1;
	int ret;

	if (!conn)
		return -ENODEV;
	guard(mutex)(&conn->policy_lock);
	if (READ_ONCE(conn->stopping))
		return -ENODEV;
	ret = connector_antiburn_get_temperature(conn, CONNECTOR_PROP_TEMP_1,
						 &first);
	if (ret)
		return ret;
	if (conn->use_double_ntc) {
		ret = connector_antiburn_get_temperature(
			conn, CONNECTOR_PROP_TEMP_2, &second);
		if (ret)
			return ret;
	}
	return scnprintf(buf, size, "%-10d%-11d%-11d", first, second,
			 READ_ONCE(conn->thermal_board_temp));
}

static struct mca_log_charge_log_ops g_connector_antiburn_log_ops = {
	.dump_log_head = connector_antiburn_dump_log_head,
	.dump_log_context = connector_antiburn_dump_log_context,
};
static void connector_antiburn_stop(struct connector_antiburn *conn)
{
	if (!conn || !conn->wq)
		return;
	WRITE_ONCE(conn->stopping, true);
	mutex_lock(&conn_provider_lock);
	if (rcu_access_pointer(g_conn) == conn)
		rcu_assign_pointer(g_conn, NULL);
	mutex_unlock(&conn_provider_lock);
	synchronize_srcu(&conn_provider_srcu);
	if (conn->sysfs_registered)
		mca_sysfs_remove_link_group(SYSFS_DEV_5, "connector", conn->dev,
					    &antiburn_sysfs_attr_group);
	if (conn->log_registered)
		mca_log_charge_log_unregister(MCA_CHARGE_LOG_ID_USCP, conn);
	if (conn->debug_registered)
		mca_event_block_notify_unregister(MCA_EVENT_TYPE_SUBPMIC_INFO,
						  &conn->debug_nb);
	if (conn->thermal_registered)
		mca_event_block_notify_unregister(MCA_EVENT_TYPE_THERMAL_TEMP,
						  &conn->thermal_board_nb);
	disable_delayed_work_sync(&conn->monitor_work);
	destroy_workqueue(conn->wq);
	conn->wq = NULL;
	mutex_lock(&conn_provider_lock);
	conn_claimed = false;
	mutex_unlock(&conn_provider_lock);
}

static int connector_antiburn_validate_config(struct connector_antiburn *conn)
{
	if (conn->en_src < 0 || conn->en_src > WIRELESS_EN_BOOST ||
	    conn->otg_boost_src < 0 || conn->otg_boost_src >= BOOST_SRC_MAX ||
	    conn->comb_sensorboard_con_trigger_temp < -50 ||
	    conn->comb_sensorboard_con_trigger_temp > 150 ||
	    conn->comb_rate_conn_trigger_temp < -50 ||
	    conn->comb_rate_conn_trigger_temp > 150 ||
	    conn->max_thermal_board_temp < -50 || conn->max_thermal_board_temp > 150)
		return -EINVAL;
	if (conn->trigger_temp < -50 || conn->trigger_temp > 150 ||
	    conn->recharge_temp < -50 ||
	    conn->recharge_temp >= conn->trigger_temp ||
	    conn->monitor_interval < 100 || conn->monitor_interval > 60000 ||
	    conn->max_temp_increase_rate < 1 ||
	    conn->max_temp_increase_rate > 100 ||
	    conn->support_hw_antiburn < 0 || conn->support_hw_antiburn > 1 ||
	    conn->support_soft_antiburn < 0 ||
	    conn->support_soft_antiburn > 1 || conn->use_double_ntc < 0 ||
	    conn->use_double_ntc > 1 || !conn->thermal_zone_name ||
	    (conn->use_double_ntc && !conn->thermal_zone_name2))
		return -EINVAL;
	if (!conn->support_soft_antiburn && !conn->support_hw_antiburn)
		return -EOPNOTSUPP;
	return 0;
}

static int connector_antiburn_probe(struct platform_device *pdev)
{
	const struct mca_hwid *hwid = mca_get_hwid_info();
	struct connector_antiburn *conn;
	int ret;

	if (!hwid)
		return -EPROBE_DEFER;
	if (hwid->platform_version == 1 && hwid->major_version == 0 &&
	    hwid->minor_version == 1)
		return -EOPNOTSUPP;
	mutex_lock(&conn_provider_lock);
	if (conn_claimed) {
		mutex_unlock(&conn_provider_lock);
		return -EEXIST;
	}
	conn_claimed = true;
	mutex_unlock(&conn_provider_lock);
	conn = devm_kzalloc(&pdev->dev, sizeof(*conn), GFP_KERNEL);
	if (!conn) {
		ret = -ENOMEM;
		goto release_claim;
	}
	conn->dev = &pdev->dev;
	mutex_init(&conn->policy_lock);
	INIT_DELAYED_WORK(&conn->monitor_work,
			  connector_antiburn_monitor_workfunc);
	connector_antiburn_parse_dt(conn);
	ret = connector_antiburn_validate_config(conn);
	if (ret)
		goto release_claim;
	if (hwid->platform_version == 1 &&
	    (hwid->major_version == 0 ||
	     (hwid->major_version == 1 && hwid->minor_version == 0)))
		conn->otg_boost_src = EXTERNAL_BOOST;
	conn->wq = alloc_ordered_workqueue("mca_connector_antiburn",
					   WQ_MEM_RECLAIM);
	if (!conn->wq) {
		ret = -ENOMEM;
		goto release_claim;
	}
	platform_set_drvdata(pdev, conn);
	ret = connector_antiburn_gpio_init(conn);
	if (ret)
		goto fail;
	conn->tzd_conn = thermal_zone_get_zone_by_name(conn->thermal_zone_name);
	if (IS_ERR(conn->tzd_conn)) {
		ret = PTR_ERR(conn->tzd_conn);
		goto fail;
	}
	if (conn->use_double_ntc) {
		conn->tzd_conn2 =
			thermal_zone_get_zone_by_name(conn->thermal_zone_name2);
		if (IS_ERR(conn->tzd_conn2)) {
			ret = PTR_ERR(conn->tzd_conn2);
			goto fail;
		}
	}
	conn->thermal_board_nb.notifier_call =
		connector_antiburn_thermal_notifier_event;
	ret = mca_event_block_notify_register(MCA_EVENT_TYPE_THERMAL_TEMP,
					      &conn->thermal_board_nb);
	if (ret)
		goto fail;
	conn->thermal_registered = true;
	conn->debug_nb.notifier_call = connector_antiburn_debug_notifier_cb;
	ret = mca_event_block_notify_register(MCA_EVENT_TYPE_SUBPMIC_INFO,
					      &conn->debug_nb);
	if (ret)
		goto fail;
	conn->debug_registered = true;
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_USCP,
				    &g_connector_antiburn_log_ops, conn);
	conn->log_registered = true;
	ret = antiburn_sysfs_create_group(conn->dev);
	if (ret)
		goto fail;
	conn->sysfs_registered = true;
	mutex_lock(&conn_provider_lock);
	rcu_assign_pointer(g_conn, conn);
	mutex_unlock(&conn_provider_lock);
	queue_delayed_work(conn->wq, &conn->monitor_work,
			   msecs_to_jiffies(conn->monitor_interval));
	return 0;
fail:
	connector_antiburn_stop(conn);
	platform_set_drvdata(pdev, NULL);
	return ret == -ENODEV ? -EPROBE_DEFER : ret;
release_claim:
	mutex_lock(&conn_provider_lock);
	conn_claimed = false;
	mutex_unlock(&conn_provider_lock);
	return ret;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "xiaomi,connector_antiburn" },
	{},
};
static void connector_antiburn_remove(struct platform_device *pdev)
{
	connector_antiburn_stop(platform_get_drvdata(pdev));
	platform_set_drvdata(pdev, NULL);
}
static void connector_antiburn_shutdown(struct platform_device *pdev)
{
	connector_antiburn_stop(platform_get_drvdata(pdev));
}

MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver connector_antiburn_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "connector_antiburn",
		.of_match_table = match_table,
	},
	.probe = connector_antiburn_probe,
	.remove = connector_antiburn_remove,
	.shutdown = connector_antiburn_shutdown,
};

static int __init connector_antiburn_init(void)
{
	return platform_driver_register(&connector_antiburn_driver);
}
module_init(connector_antiburn_init);

static void __exit connector_antiburn_exit(void)
{
	platform_driver_unregister(&connector_antiburn_driver);
}
module_exit(connector_antiburn_exit);

MODULE_DESCRIPTION("Connector antiburn");
MODULE_AUTHOR("muxinyi1@xiaomi.com");
MODULE_LICENSE("GPL v2");
