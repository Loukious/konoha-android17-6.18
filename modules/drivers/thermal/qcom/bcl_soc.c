// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include "thermal_zone_internal.h"

#define BCL_DRIVER_NAME       "bcl_soc_peripheral"

struct bcl_soc_device {
	struct device				*dev;
	struct notifier_block			psy_nb;
	struct work_struct			soc_eval_work;
	long					trip_temp;
	struct mutex				state_trans_lock;
	bool					irq_enabled;
	bool					ready;
	bool					stopping;
	bool					notifier_registered;
	struct thermal_zone_device		*tz_dev;
	struct thermal_zone_device_ops	ops;
};

static int bcl_soc_get_trend(struct thermal_zone_device *tz,
			const struct thermal_trip *trip,
			enum thermal_trend *trend)
{
	int trip_temp = 0, trip_hyst = 0, temp;

	if (!tz || !trip)
		return -EINVAL;

	trip_temp = trip->temperature;
	trip_hyst = trip->hysteresis;
	temp = READ_ONCE(tz->temperature);

	if (temp >= trip_temp)
		*trend = THERMAL_TREND_RAISING;
	else if (!trip_hyst && temp < trip_temp)
		*trend = THERMAL_TREND_DROPPING;
	else if (temp <= (trip_temp - trip_hyst))
		*trend = THERMAL_TREND_DROPPING;
	else
		*trend = THERMAL_TREND_STABLE;

	return 0;
}

static int bcl_set_soc(struct thermal_zone_device *tz, int low, int high)
{
	struct bcl_soc_device *bcl_soc = tz->devdata;

	mutex_lock(&bcl_soc->state_trans_lock);
	if (!bcl_soc->stopping && high != bcl_soc->trip_temp) {
		bcl_soc->trip_temp = high;
		bcl_soc->irq_enabled = high != INT_MAX;
		if (bcl_soc->ready && bcl_soc->irq_enabled)
			schedule_work(&bcl_soc->soc_eval_work);
	}
	mutex_unlock(&bcl_soc->state_trans_lock);
	return 0;
}

static int bcl_soc_change_mode(struct thermal_zone_device *tz,
			       enum thermal_device_mode mode)
{
	int ret = qti_tz_change_mode(tz, mode);

	if (!ret && mode == THERMAL_DEVICE_DISABLED)
		ret = bcl_set_soc(tz, -INT_MAX, INT_MAX);
	return ret;
}

static int bcl_read_soc(struct thermal_zone_device *tz, int *val)
{
	struct power_supply *battery;
	union power_supply_propval capacity;
	int ret;

	battery = power_supply_get_by_name("battery");
	if (!battery)
		return -EAGAIN;
	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_CAPACITY, &capacity);
	power_supply_put(battery);
	if (ret)
		return ret;
	if (capacity.intval < 0 || capacity.intval > 100)
		return -ERANGE;

	*val = 100 - capacity.intval;
	return 0;
}

static void bcl_evaluate_soc(struct work_struct *work)
{
	struct bcl_soc_device *bcl_soc = container_of(work, struct bcl_soc_device,
						    soc_eval_work);
	bool update;

	mutex_lock(&bcl_soc->state_trans_lock);
	update = bcl_soc->ready && !bcl_soc->stopping;
	mutex_unlock(&bcl_soc->state_trans_lock);
	/* Also retry a first reading when the battery appears after probe. */
	if (update)
		thermal_zone_device_update(bcl_soc->tz_dev, THERMAL_TRIP_VIOLATED);
}

static int battery_supply_callback(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct bcl_soc_device *bcl_soc = container_of(nb, struct bcl_soc_device, psy_nb);
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED || strcmp(psy->desc->name, "battery"))
		return NOTIFY_DONE;

	mutex_lock(&bcl_soc->state_trans_lock);
	if (bcl_soc->ready && !bcl_soc->stopping)
		schedule_work(&bcl_soc->soc_eval_work);
	mutex_unlock(&bcl_soc->state_trans_lock);
	return NOTIFY_OK;
}

static void bcl_soc_stop(void *data)
{
	struct bcl_soc_device *bcl_soc = data;

	mutex_lock(&bcl_soc->state_trans_lock);
	bcl_soc->stopping = true;
	mutex_unlock(&bcl_soc->state_trans_lock);
	if (bcl_soc->notifier_registered)
		power_supply_unreg_notifier(&bcl_soc->psy_nb);
	cancel_work_sync(&bcl_soc->soc_eval_work);
}

static int bcl_soc_probe(struct platform_device *pdev)
{
	struct bcl_soc_device *bcl_soc;
	struct thermal_zone_device *tz;
	int ret;

	bcl_soc = devm_kzalloc(&pdev->dev, sizeof(*bcl_soc), GFP_KERNEL);
	if (!bcl_soc)
		return -ENOMEM;

	mutex_init(&bcl_soc->state_trans_lock);
	bcl_soc->dev = &pdev->dev;
	bcl_soc->trip_temp = INT_MAX;
	bcl_soc->ops.get_temp = bcl_read_soc;
	bcl_soc->ops.set_trips = bcl_set_soc;
	bcl_soc->ops.get_trend = bcl_soc_get_trend;
	bcl_soc->ops.change_mode = bcl_soc_change_mode;
	INIT_WORK(&bcl_soc->soc_eval_work, bcl_evaluate_soc);
	bcl_soc->psy_nb.notifier_call = battery_supply_callback;

	tz = devm_thermal_of_zone_register(&pdev->dev, 0, bcl_soc, &bcl_soc->ops);
	if (IS_ERR(tz))
		return dev_err_probe(&pdev->dev, PTR_ERR(tz), "Cannot register SOC zone\n");
	bcl_soc->tz_dev = tz;
	/* No work may start before the zone and its cleanup are ready. */
	ret = devm_add_action_or_reset(&pdev->dev, bcl_soc_stop, bcl_soc);
	if (ret)
		return ret;
	ret = power_supply_reg_notifier(&bcl_soc->psy_nb);
	if (ret)
		return ret;
	bcl_soc->notifier_registered = true;

	mutex_lock(&bcl_soc->state_trans_lock);
	bcl_soc->ready = true;
	schedule_work(&bcl_soc->soc_eval_work);
	mutex_unlock(&bcl_soc->state_trans_lock);
	platform_set_drvdata(pdev, bcl_soc);
	return 0;
}

static const struct of_device_id bcl_match[] = {
	{
		.compatible = "qcom,msm-bcl-soc",
	},
	{},
};

MODULE_DEVICE_TABLE(of, bcl_match);

static struct platform_driver bcl_driver = {
	.probe  = bcl_soc_probe,
	.driver = {
		.name           = BCL_DRIVER_NAME,
		.of_match_table = bcl_match,
	},
};

module_platform_driver(bcl_driver);
MODULE_DESCRIPTION("Qualcomm battery state-of-charge thermal monitor");
MODULE_LICENSE("GPL");
