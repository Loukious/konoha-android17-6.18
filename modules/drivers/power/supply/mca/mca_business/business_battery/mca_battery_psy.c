// SPDX-License-Identifier: GPL-2.0
/* Xiaomi MCA battery power_supply and battery thermal-zone interface. */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <mca/common/mca_log.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include "inc/mca_battery_psy.h"

static int battery_supply_property(const char *name,
				   enum power_supply_property property,
				   union power_supply_propval *value)
{
	struct power_supply *supply = power_supply_get_by_name(name);
	int ret;

	if (!supply)
		return -ENODEV;
	ret = power_supply_get_property(supply, property, value);
	power_supply_put(supply);
	return ret;
}

static int battery_scale_value(int ret, int value, int scale,
			       union power_supply_propval *out)
{
	if (ret)
		return ret;
	if (check_mul_overflow(value, scale, &out->intval))
		return -ERANGE;
	return 0;
}

static int battery_capacity_level(int soc, union power_supply_propval *value)
{
	if (soc < 0)
		return soc;
	if (soc > 100)
		return -ERANGE;
	if (!soc)
		value->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	else if (soc <= 20)
		value->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else if (soc <= 80)
		value->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	else if (soc < 100)
		value->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
	else
		value->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	return 0;
}

static int battery_status(union power_supply_propval *value)
{
	union power_supply_propval usb = {}, wireless = {};
	int ret, soc;

	if (mca_get_named_client_vote("input_suspend", "micharge") > 0) {
		value->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}
	ret = battery_supply_property("usb", POWER_SUPPLY_PROP_ONLINE, &usb);
	if (ret)
		return ret;
	if (!usb.intval) {
		ret = battery_supply_property(
			"wireless", POWER_SUPPLY_PROP_ONLINE, &wireless);
		/* A wired-only system has no wireless power supply. */
		if (ret && ret != -ENODEV)
			return ret;
	}
	if (!usb.intval && !wireless.intval) {
		value->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}
	/* A wired handle hold inhibits both charging paths while USB stays online. */
	if (usb.intval &&
	    mca_get_named_client_vote("chg_enable", "handle") == 0 &&
	    mca_get_named_client_vote("quick_chg_en", "handle") == 0) {
		value->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}
	soc = strategy_class_fg_ops_get_soc();
	if (soc < 0)
		return soc;
	if (soc > 100)
		return -ERANGE;
	value->intval =
		soc == 100 && strategy_class_fg_ops_get_charging_done() ?
			POWER_SUPPLY_STATUS_FULL :
			POWER_SUPPLY_STATUS_CHARGING;
	return 0;
}

/* The existing explicit factory event selects this mode; never a read fallback. */
static int fake_batt_psy_get_prop(struct batt_psy_info *info,
				  enum power_supply_property property,
				  union power_supply_propval *value)
{
	int ret;

	switch (property) {
	case POWER_SUPPLY_PROP_PRESENT:
		value->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = battery_supply_property("usb", POWER_SUPPLY_PROP_ONLINE,
					      value);
		if (!ret)
			value->intval = value->intval ?
						POWER_SUPPLY_STATUS_CHARGING :
						POWER_SUPPLY_STATUS_DISCHARGING;
		return ret;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		return platform_class_buckchg_ops_get_chg_type(
			MAIN_BUCK_CHARGER, &value->intval);
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		return battery_capacity_level(15, value);
	case POWER_SUPPLY_PROP_CAPACITY:
		value->intval = 15;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		value->intval = 3700000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		value->intval = -500000;
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		value->intval = 250;
		return 0;
	default:
		return -ENODATA;
	}
}

static int batt_psy_get_prop(struct power_supply *psy,
			     enum power_supply_property property,
			     union power_supply_propval *value)
{
	struct batt_psy_info *info = power_supply_get_drvdata(psy);
	int result = 0, ret;

	if (!info || !value)
		return -EINVAL;
	if (READ_ONCE(info->fake_power))
		return fake_batt_psy_get_prop(info, property, value);
	ret = strategy_class_fg_ops_is_init_ok();
	if (ret <= 0)
		return ret < 0 ? ret : -ENODATA;
	switch (property) {
	case POWER_SUPPLY_PROP_STATUS:
		return battery_status(value);
	case POWER_SUPPLY_PROP_HEALTH:
		return strategy_class_fg_get_health(&value->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		ret = strategy_class_fg_is_chip_ok();
		if (ret < 0)
			return ret;
		value->intval = !!ret;
		return 0;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = platform_class_buckchg_ops_get_input_curr_lmt(
			MAIN_BUCK_CHARGER, &result);
		return battery_scale_value(ret, result, 1000, value);
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		return platform_class_buckchg_ops_get_chg_type(
			MAIN_BUCK_CHARGER, &value->intval);
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		return battery_capacity_level(strategy_class_fg_ops_get_soc(),
					      value);
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = strategy_class_fg_ops_get_soc();
		if (ret < 0)
			return ret;
		if (ret > 100)
			return -ERANGE;
		value->intval = ret;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = strategy_class_fg_ops_get_voltage(&result);
		return battery_scale_value(ret, result, 1000, value);
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return battery_supply_property("usb", property, value);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return strategy_class_fg_ops_get_current(&value->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		return battery_supply_property(
			"usb", POWER_SUPPLY_PROP_CURRENT_MAX, value);
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = platform_class_buckchg_ops_get_term_curr(
			MAIN_BUCK_CHARGER, &result);
		return battery_scale_value(ret, result, 1000, value);
	case POWER_SUPPLY_PROP_TEMP:
		return strategy_class_fg_ops_get_temperature(&value->intval);
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		value->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		return strategy_class_fg_get_rm(&value->intval);
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		return strategy_class_fg_ops_get_cyclecount(&value->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		return strategy_class_fg_get_fcc(&value->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		return strategy_class_fg_get_dc(&value->intval);
	case POWER_SUPPLY_PROP_MODEL_NAME: {
		/* Publish an immutable device-owned copy, independent of FG teardown. */
		guard(mutex)(&info->model_lock);
		if (!info->model_valid) {
			ret = strategy_class_fg_copy_model_name(
				info->model_name, sizeof(info->model_name));
			if (ret)
				return ret;
			info->model_valid = true;
		}
		value->strval = info->model_name;
		return 0;
	}
	default:
		return -ENODATA;
	}
}

static enum power_supply_property batt_psy_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
};

static const struct power_supply_desc batt_psy_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = batt_psy_props,
	.num_properties = ARRAY_SIZE(batt_psy_props),
	.get_property = batt_psy_get_prop,
	/* The separate thermal zone uses the FG's thermal-adjusted temperature. */
	.no_thermal = true,
};

static int power_supply_read_temp(struct thermal_zone_device *tzd, int *temp)
{
	int value, ret;

	ret = strategy_class_fg_ops_get_thermal_temperature(&value);
	if (ret)
		return ret;
	if (check_mul_overflow(value, 100, temp))
		return -ERANGE;
	return 0;
}

static const struct thermal_zone_device_ops psy_tzd_ops = {
	.get_temp = power_supply_read_temp,
};

static int business_battery_dump_log_head(void *data, char *buf, int size)
{
	return scnprintf(buf, size,
			 "ChargeStatus uiSoc uiSoh F_Volt F_Curr F_temp ");
}

static int business_battery_dump_log_context(void *data, char *buf, int size)
{
	struct batt_psy_info *info = data;
	static const enum power_supply_property properties[] = {
		POWER_SUPPLY_PROP_STATUS,      POWER_SUPPLY_PROP_CAPACITY,
		POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
		POWER_SUPPLY_PROP_TEMP,
	};
	union power_supply_propval values[ARRAY_SIZE(properties)];
	int i, ret, soh;

	for (i = 0; i < ARRAY_SIZE(properties); i++) {
		ret = batt_psy_get_prop(info->batt_psy, properties[i],
					&values[i]);
		if (ret)
			return ret;
	}
	ret = strategy_class_fg_get_soh(&soh);
	if (ret)
		return ret;
	return scnprintf(buf, size, "%-13d%-6d%-6d%-7d%-8d%-7d",
			 values[0].intval, values[1].intval, soh,
			 values[2].intval / 1000, values[3].intval / 1000,
			 values[4].intval);
}

static struct mca_log_charge_log_ops battery_log_ops = {
	.dump_log_head = business_battery_dump_log_head,
	.dump_log_context = business_battery_dump_log_context,
};

struct batt_psy_info *business_battery_psy_init(struct device *dev)
{
	struct power_supply_config config = { .fwnode = dev_fwnode(dev) };
	struct thermal_zone_params params = {};
	struct batt_psy_info *info;
	int ret;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->dev = dev;
	mutex_init(&info->model_lock);
	config.drv_data = info;
	info->batt_psy = power_supply_register(dev, &batt_psy_desc, &config);
	if (IS_ERR(info->batt_psy))
		return ERR_CAST(info->batt_psy);
	info->tzd = thermal_tripless_zone_device_register(
		"battery", info, &psy_tzd_ops, &params);
	if (IS_ERR(info->tzd)) {
		ret = PTR_ERR(info->tzd);
		goto unregister_psy;
	}
	ret = thermal_zone_device_enable(info->tzd);
	if (ret) {
		thermal_zone_device_unregister(info->tzd);
		goto unregister_psy;
	}
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_BATTERY_INFO,
				    &battery_log_ops, info);
	return info;
unregister_psy:
	power_supply_unregister(info->batt_psy);
	return ERR_PTR(ret);
}

void business_battery_psy_deinit(struct batt_psy_info *info)
{
	mca_log_charge_log_unregister(MCA_CHARGE_LOG_ID_BATTERY_INFO, info);
	thermal_zone_device_unregister(info->tzd);
	power_supply_unregister(info->batt_psy);
}
