// SPDX-License-Identifier: GPL-2.0
/*
 * mca_charger_usb_psy.c
 *
 * mc charger usb psy driver
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
#include <mca/protocol/protocol_class.h>
#include <mca/platform/platform_buckchg_class.h>
#include "inc/mca_charger_usb_psy.h"

static const enum power_supply_property usb_psy_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_TYPE,
};

static int usb_psy_get_prop(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct usb_psy_info *info = power_supply_get_drvdata(psy);
	int value = 0;
	int ret;

	if (!info || !val)
		return -EINVAL;
	guard(mutex)(&info->lock);
	if (info->stopping)
		return -ENODEV;
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		ret = mca_strategy_func_get_status(
			STRATEGY_FUNC_TYPE_BUCK_CHARGE,
			STRATEGY_STATUS_TYPE_ONLINE, &value);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* The PMIC transport already returns microvolts/microamps. */
		ret = platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER,
							      &value);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = platform_class_buckchg_ops_get_bus_curr(MAIN_BUCK_CHARGER,
							      &value);
		break;
	case POWER_SUPPLY_PROP_TYPE:
		value = READ_ONCE(info->usb_psy_desc.type);
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		/* No maximum/current-limit getter exists in the strategy API. */
		return -ENODATA;
	default:
		return -EINVAL;
	}
	if (ret)
		return ret;
	if ((psp == POWER_SUPPLY_PROP_PRESENT ||
	     psp == POWER_SUPPLY_PROP_ONLINE) &&
	    (value != 0 && value != 1))
		return -ERANGE;
	if (psp == POWER_SUPPLY_PROP_VOLTAGE_NOW && value < 0)
		return -ERANGE;
	val->intval = value;
	return 0;
}

static int usb_psy_set_prop(struct power_supply *psy,
			    enum power_supply_property psp,
			    const union power_supply_propval *val)
{
	struct usb_psy_info *info = power_supply_get_drvdata(psy);

	if (!info || !val || psp != POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
	    val->intval < -1000 || val->intval > 30000000)
		return -EINVAL;
	guard(mutex)(&info->lock);
	if (info->stopping)
		return -ENODEV;
	return mca_strategy_func_set_config(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					    STRATEGY_CONFIG_INPUT_CURRENT_LIMIT,
					    val->intval);
}

static int usb_psy_prop_is_writeable(struct power_supply *psy,
				     enum power_supply_property prop)
{
	return prop == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

static const struct power_supply_desc usb_psy_template = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.properties = usb_psy_props,
	.num_properties = ARRAY_SIZE(usb_psy_props),
	.get_property = usb_psy_get_prop,
	.set_property = usb_psy_set_prop,
	.property_is_writeable = usb_psy_prop_is_writeable,
};

void business_charger_update_usb_psy(int real_type, struct usb_psy_info *info)
{
	struct power_supply *battery;
	enum power_supply_type type;

	if (!info)
		return;
	switch (real_type) {
	case XM_CHARGER_TYPE_SDP:
		type = POWER_SUPPLY_TYPE_USB;
		break;
	case XM_CHARGER_TYPE_CDP:
		type = POWER_SUPPLY_TYPE_USB_CDP;
		break;
	case XM_CHARGER_TYPE_FLOAT:
	case XM_CHARGER_TYPE_DCP:
	case XM_CHARGER_TYPE_HVDCP2:
	case XM_CHARGER_TYPE_HVDCP3:
	case XM_CHARGER_TYPE_HVDCP3_B:
	case XM_CHARGER_TYPE_HVDCP3P5:
	case XM_CHARGER_TYPE_OCP:
		type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	case XM_CHARGER_TYPE_TYPEC:
	case XM_CHARGER_TYPE_PD:
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		type = POWER_SUPPLY_TYPE_USB_PD;
		break;
	case XM_CHARGER_TYPE_ACA:
		type = POWER_SUPPLY_TYPE_USB_ACA;
		break;
	default:
		type = POWER_SUPPLY_TYPE_UNKNOWN;
		break;
	}
	scoped_guard(mutex, &info->lock)
	{
		if (info->stopping)
			return;
		WRITE_ONCE(info->usb_psy_desc.type, type);
		power_supply_changed(info->usb_psy);
	}
	battery = power_supply_get_by_name("battery");
	if (battery) {
		power_supply_changed(battery);
		power_supply_put(battery);
	}
}

void business_usb_psy_event_process(struct usb_psy_info *info)
{
	struct power_supply *battery;

	if (!info)
		return;
	scoped_guard(mutex, &info->lock) {
		if (info->stopping)
			return;
		power_supply_changed(info->usb_psy);
	}
	battery = power_supply_get_by_name("battery");
	if (battery) {
		power_supply_changed(battery);
		power_supply_put(battery);
	}
}

struct usb_psy_info *business_charger_usb_psy_init(struct device *dev)
{
	struct power_supply_config config = { .fwnode = dev_fwnode(dev) };
	struct usb_psy_info *info;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->dev = dev;
	mutex_init(&info->lock);
	info->charge_enable = 1;
	info->usb_psy_desc = usb_psy_template;
	config.drv_data = info;
	info->usb_psy =
		power_supply_register(dev, &info->usb_psy_desc, &config);
	if (IS_ERR(info->usb_psy))
		return ERR_CAST(info->usb_psy);
	return info;
}

void business_charger_usb_psy_deinit(struct usb_psy_info *info)
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
	power_supply_unregister(info->usb_psy);
}
