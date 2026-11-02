// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/devfreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define MAX_RETRY_CNT 20
#define RETRY_DELAY msecs_to_jiffies(1000)
#define DEVFREQ_CDEV_NAME "gpu"
#define DEVFREQ_CDEV "qcom-devfreq-cdev"

struct devfreq_cdev_device {
	struct device_node *np;
	/* Serializes registration and the removal gate. */
	struct mutex lock;
	bool stopping;
	struct device *dev;
	unsigned long *freq_table;
	int cur_state;
	int max_state;
	int retry_cnt;
	struct dev_pm_qos_request qos_max_freq_req;
	struct delayed_work register_work;
	struct thermal_cooling_device *cdev;
};


static int devfreq_cdev_set_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;
	int ret = 0;
	unsigned long freq;

	if (state > cdev_data->max_state)
		return -EINVAL;
	if (state == cdev_data->cur_state)
		return 0;
	freq = cdev_data->freq_table[state];
	pr_debug("cdev:%s Limit:%lu\n", cdev->type, freq);
	ret = dev_pm_qos_update_request(&cdev_data->qos_max_freq_req, freq);
	if (ret < 0) {
		pr_err("Error placing qos request:%lu. cdev:%s err:%d\n",
				freq, cdev->type, ret);
		return ret;
	}
	cdev_data->cur_state = state;

	return 0;
}

static int devfreq_cdev_get_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->cur_state;
	return 0;
}

static int devfreq_cdev_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->max_state;
	return 0;
}

static struct thermal_cooling_device_ops devfreq_cdev_ops = {
	.set_cur_state = devfreq_cdev_set_state,
	.get_cur_state = devfreq_cdev_get_state,
	.get_max_state = devfreq_cdev_get_max_state,
};

static void devfreq_cdev_work(struct work_struct *work)
{
	struct devfreq_cdev_device *data = container_of(work,
			struct devfreq_cdev_device, register_work.work);
	struct dev_pm_opp *opp;
	unsigned long freq = ULONG_MAX, khz;
	unsigned long *table;
	int count, i, ret;

	mutex_lock(&data->lock);
	if (data->stopping || data->cdev)
		goto unlock;
	/* Use this API only as an availability check: it returns no reference. */
	if (IS_ERR(devfreq_get_devfreq_by_node(data->np))) {
		if (data->retry_cnt > 0 && --data->retry_cnt > 0)
			queue_delayed_work(system_highpri_wq, &data->register_work,
					   RETRY_DELAY);
		goto unlock;
	}
	count = dev_pm_opp_get_opp_count(data->dev);
	if (count <= 0)
		goto unlock;
	table = kcalloc(count, sizeof(*table), GFP_KERNEL);
	if (!table)
		goto unlock;
	for (i = 0; i < count; i++) {
		opp = dev_pm_opp_find_freq_floor(data->dev, &freq);
		if (IS_ERR(opp))
			goto free_table;
		dev_pm_opp_put(opp);
		/* Hz to kHz without rounding-add overflow or signed QoS truncation. */
		khz = freq / 1000 + !!(freq % 1000);
		if (!freq || khz > S32_MAX)
			goto free_table;
		table[i] = khz;
		freq--;
	}
	data->max_state = count - 1;
	data->freq_table = table;
	ret = dev_pm_qos_add_request(data->dev, &data->qos_max_freq_req,
				     DEV_PM_QOS_MAX_FREQUENCY,
				     PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret < 0)
		goto clear_table;
	data->cdev = thermal_cooling_device_register(DEVFREQ_CDEV_NAME, data,
						     &devfreq_cdev_ops);
	if (IS_ERR(data->cdev)) {
		data->cdev = NULL;
		dev_pm_qos_remove_request(&data->qos_max_freq_req);
		goto clear_table;
	}
	mutex_unlock(&data->lock);
	return;

clear_table:
	data->freq_table = NULL;
free_table:
	kfree(table);
unlock:
	mutex_unlock(&data->lock);
}

static int devfreq_cdev_link_supplier(struct device *consumer,
				      struct device *supplier)
{
	int ret;

	if (!device_link_add(consumer, supplier, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	/* Do not wait on a supplier lock while probing the consumer. */
	if (!device_trylock(supplier))
		return -EPROBE_DEFER;
	ret = device_is_bound(supplier) ? 0 : -EPROBE_DEFER;
	device_unlock(supplier);
	/* The core drops the managed link if this consumer defers probing. */
	return ret;
}

static int devfreq_cdev_probe(struct platform_device *pdev)
{
	struct devfreq_cdev_device *data;
	struct platform_device *supplier;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->np = of_parse_phandle(pdev->dev.of_node, "qcom,devfreq", 0);
	if (!data->np)
		return -EINVAL;
	supplier = of_find_device_by_node(data->np);
	if (!supplier) {
		ret = -EPROBE_DEFER;
		goto put_node;
	}
	data->dev = &supplier->dev;
	ret = devfreq_cdev_link_supplier(&pdev->dev, data->dev);
	if (ret)
		goto put_device;
	mutex_init(&data->lock);
	data->retry_cnt = MAX_RETRY_CNT;
	INIT_DEFERRABLE_WORK(&data->register_work, devfreq_cdev_work);
	platform_set_drvdata(pdev, data);
	queue_delayed_work(system_highpri_wq, &data->register_work, 0);
	return 0;

put_device:
	put_device(data->dev);
put_node:
	of_node_put(data->np);
	return ret;
}

static void devfreq_cdev_remove(struct platform_device *pdev)
{
	struct devfreq_cdev_device *data = platform_get_drvdata(pdev);

	mutex_lock(&data->lock);
	data->stopping = true;
	mutex_unlock(&data->lock);
	cancel_delayed_work_sync(&data->register_work);
	if (data->cdev)
		thermal_cooling_device_unregister(data->cdev);
	if (dev_pm_qos_request_active(&data->qos_max_freq_req))
		dev_pm_qos_remove_request(&data->qos_max_freq_req);
	kfree(data->freq_table);
	put_device(data->dev);
	of_node_put(data->np);
}

static const struct of_device_id devfreq_cdev_match[] = {
	{.compatible = "qcom,devfreq-cdev"},
	{}
};

static struct platform_driver devfreq_cdev_driver = {
	.probe          = devfreq_cdev_probe,
	.remove         = devfreq_cdev_remove,
	.driver         = {
		.name   = DEVFREQ_CDEV,
		.of_match_table = devfreq_cdev_match,
	},
};

static int __init devfreq_cdev_init(void)
{
	return platform_driver_register(&devfreq_cdev_driver);
}
module_init(devfreq_cdev_init);

static void __exit devfreq_cdev_exit(void)
{
	platform_driver_unregister(&devfreq_cdev_driver);
}
module_exit(devfreq_cdev_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. devfreq cooling device driver");
MODULE_LICENSE("GPL");
