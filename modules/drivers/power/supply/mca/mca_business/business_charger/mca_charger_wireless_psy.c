// SPDX-License-Identifier: GPL-2.0
/*
 * mca_charger_wireless_psy.c
 *
 * mca_charger_wireless_psy
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

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <mca/common/mca_log.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include "inc/mca_charger_wireless_psy.h"

static const enum power_supply_property wireless_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
};

static int wireless_psy_get_prop(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct wireless_psy_info *info = power_supply_get_drvdata(psy);
	int value = 0, scale = 1;
	int ret = 0;

	if (!info || !val)
		return -EINVAL;
	guard(mutex)(&info->lock);
	if (info->stopping)
		return -ENODEV;
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		ret = mca_strategy_func_get_status(
			STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
			STRATEGY_STATUS_TYPE_ONLINE, &value);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		value = info->online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = strategy_class_fg_ops_get_voltage(&value);
		scale = 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = strategy_class_fg_ops_get_current(&value);
		scale = 1000;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = strategy_class_fg_ops_get_temperature(&value);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		return -ENODATA;
	default:
		return -EINVAL;
	}
	if (ret)
		return ret;
	if (psp == POWER_SUPPLY_PROP_PRESENT && value != 0 && value != 1)
		return -ERANGE;
	if (psp == POWER_SUPPLY_PROP_VOLTAGE_NOW && value < 0)
		return -ERANGE;
	if (check_mul_overflow(value, scale, &val->intval))
		return -ERANGE;
	return 0;
}

static int wireless_psy_set_prop(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct wireless_psy_info *info = power_supply_get_drvdata(psy);

	if (!info || !val || psp != POWER_SUPPLY_PROP_ONLINE ||
	    (val->intval != 0 && val->intval != 1))
		return -EINVAL;
	guard(mutex)(&info->lock);
	if (info->stopping)
		return -ENODEV;
	info->online = val->intval;
	return 0;
}

static int wireless_psy_prop_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_ONLINE;
}

static const struct power_supply_desc wireless_psy_desc = {
	.name = "wireless",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = wireless_psy_props,
	.num_properties = ARRAY_SIZE(wireless_psy_props),
	.get_property = wireless_psy_get_prop,
	.set_property = wireless_psy_set_prop,
	.property_is_writeable = wireless_psy_prop_is_writeable,
};

void business_charger_update_wireless_psy(struct wireless_psy_info *info)
{
	struct power_supply *battery;

	if (!info)
		return;
	scoped_guard(mutex, &info->lock)
	{
		if (info->stopping)
			return;
		power_supply_changed(info->wireless_psy);
	}
	battery = power_supply_get_by_name("battery");
	if (battery) {
		power_supply_changed(battery);
		power_supply_put(battery);
	}
}

struct wireless_psy_info *business_charger_wireless_psy_init(struct device *dev)
{
	struct power_supply_config config = { .fwnode = dev_fwnode(dev) };
	struct wireless_psy_info *info;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->dev = dev;
	mutex_init(&info->lock);
	config.drv_data = info;
	info->wireless_psy =
		power_supply_register(dev, &wireless_psy_desc, &config);
	if (IS_ERR(info->wireless_psy))
		return ERR_CAST(info->wireless_psy);
	return info;
}

void business_charger_wireless_psy_deinit(struct wireless_psy_info *info)
{
	if (!info || IS_ERR(info))
		return;
	mutex_lock(&info->lock);
	if (info->stopping) {
		mutex_unlock(&info->lock);
		return;
	}
	info->stopping = true;
	mutex_unlock(&info->lock);
	power_supply_unregister(info->wireless_psy);
}
