// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023, 2024,  Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define MAX_RETRY_CNT 20
#define RETRY_DELAY msecs_to_jiffies(1000)
#define CPUFREQ_CDEV "qcom-cpufreq-cdev"

struct cpufreq_cdev_device {
	struct list_head node;
	struct thermal_cooling_device *cdev;
	int cpu;
	unsigned long cur_state;
	unsigned long max_state;
	unsigned int *freq_table;
	int retry_cnt;
	struct freq_qos_request qos_max_freq_req;
	char cdev_name[THERMAL_NAME_LENGTH];
	struct cpufreq_policy *policy;
	struct delayed_work register_work;
	struct cpufreq_cdev_controller *controller;
};

static DEFINE_MUTEX(qti_cpufreq_cdev_lock);
struct cpufreq_cdev_controller {
	struct list_head devices;
	bool stopping;
	int hp_state;
};

static struct cpufreq_cdev_controller *cpufreq_controller;

static unsigned int state_to_cpufreq(struct cpufreq_cdev_device *cdev_data,
					   unsigned long state)
{
	return cdev_data->freq_table ?
			cdev_data->freq_table[state] : UINT_MAX;
}

static int cpufreq_cdev_set_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct cpufreq_cdev_device *cdev_data = cdev->devdata;
	int ret = 0;
	unsigned int freq;

	if (state > cdev_data->max_state)
		return -EINVAL;
	if (state == cdev_data->cur_state)
		return 0;

	if (freq_qos_request_active(&cdev_data->qos_max_freq_req)) {
		freq = state_to_cpufreq(cdev_data, state);
		pr_debug("cdev:%s Limit:%u\n", cdev->type, freq);
		ret = freq_qos_update_request(&cdev_data->qos_max_freq_req,
						freq);
		if (ret < 0) {
			pr_err("Error placing qos request:%u. cdev:%s err:%d\n",
				freq, cdev->type, ret);
			return ret;
		}
	}
	cdev_data->cur_state = state;

	return 0;
}

static int cpufreq_cdev_get_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct cpufreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->cur_state;
	return 0;
}

static int cpufreq_cdev_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct cpufreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->max_state;
	return 0;
}

static struct thermal_cooling_device_ops cpufreq_cdev_ops = {
	.set_cur_state = cpufreq_cdev_set_state,
	.get_cur_state = cpufreq_cdev_get_state,
	.get_max_state = cpufreq_cdev_get_max_state,
};

static int cpufreq_compare(const void *a, const void *b)
{
	unsigned int first = *(const unsigned int *)a;
	unsigned int second = *(const unsigned int *)b;

	return (first < second) - (first > second);
}

static void cpufreq_cdev_register(struct work_struct *work)
{
	struct cpufreq_cdev_device *data = container_of(work,
			struct cpufreq_cdev_device, register_work.work);
	struct cpufreq_frequency_table *entry;
	struct cpufreq_policy *policy;
	int count, i = 0, ret;

	mutex_lock(&qti_cpufreq_cdev_lock);
	if (data->controller->stopping || data->cdev) {
		mutex_unlock(&qti_cpufreq_cdev_lock);
		return;
	}
	mutex_unlock(&qti_cpufreq_cdev_lock);

	policy = cpufreq_cpu_get(data->cpu);
	if (!policy) {
		mutex_lock(&qti_cpufreq_cdev_lock);
		if (!data->controller->stopping && data->retry_cnt > 0 &&
		    --data->retry_cnt > 0)
			queue_delayed_work(system_highpri_wq, &data->register_work,
					   RETRY_DELAY);
		mutex_unlock(&qti_cpufreq_cdev_lock);
		return;
	}
	count = cpufreq_table_count_valid_entries(policy);
	if (count <= 0)
		goto put_policy;
	data->freq_table = kmalloc_array(count, sizeof(*data->freq_table),
					 GFP_KERNEL);
	if (!data->freq_table)
		goto put_policy;
	cpufreq_for_each_valid_entry(entry, policy->freq_table) {
		if (i == count || !entry->frequency || entry->frequency > S32_MAX)
			goto free_table;
		data->freq_table[i++] = entry->frequency;
	}
	if (i != count)
		goto free_table;
	sort(data->freq_table, count, sizeof(*data->freq_table), cpufreq_compare, NULL);
	data->max_state = count - 1;
	data->cur_state = 0;
	ret = freq_qos_add_request(&policy->constraints, &data->qos_max_freq_req,
				   FREQ_QOS_MAX, state_to_cpufreq(data, 0));
	if (ret < 0)
		goto free_table;
	data->cdev = thermal_cooling_device_register(data->cdev_name, data,
						     &cpufreq_cdev_ops);
	if (IS_ERR(data->cdev)) {
		data->cdev = NULL;
		freq_qos_remove_request(&data->qos_max_freq_req);
		goto free_table;
	}
	data->policy = policy;
	return;

free_table:
	kfree(data->freq_table);
	data->freq_table = NULL;
put_policy:
	cpufreq_cpu_put(policy);
}

static int cpufreq_cdev_hp_online(unsigned int online_cpu)
{
	struct cpufreq_cdev_device *data;

	mutex_lock(&qti_cpufreq_cdev_lock);
	if (cpufreq_controller && !cpufreq_controller->stopping) {
		list_for_each_entry(data, &cpufreq_controller->devices, node) {
			if (data->cpu != online_cpu)
				continue;
			data->retry_cnt = MAX_RETRY_CNT;
			queue_delayed_work(system_highpri_wq, &data->register_work, 0);
		}
	}
	mutex_unlock(&qti_cpufreq_cdev_lock);
	return 0;
}

static void cpufreq_cdev_cleanup(struct cpufreq_cdev_controller *controller)
{
	struct cpufreq_cdev_device *data;

	mutex_lock(&qti_cpufreq_cdev_lock);
	controller->stopping = true;
	mutex_unlock(&qti_cpufreq_cdev_lock);
	/* Hotplug callbacks take the registry lock; remove them outside it. */
	if (controller->hp_state >= 0)
		cpuhp_remove_state_nocalls(controller->hp_state);
	list_for_each_entry(data, &controller->devices, node) {
		cancel_delayed_work_sync(&data->register_work);
		if (data->cdev)
			thermal_cooling_device_unregister(data->cdev);
		if (freq_qos_request_active(&data->qos_max_freq_req))
			freq_qos_remove_request(&data->qos_max_freq_req);
		if (data->policy)
			cpufreq_cpu_put(data->policy);
		kfree(data->freq_table);
	}
	mutex_lock(&qti_cpufreq_cdev_lock);
	cpufreq_controller = NULL;
	mutex_unlock(&qti_cpufreq_cdev_lock);
}

static int cpufreq_cdev_probe(struct platform_device *pdev)
{
	struct cpufreq_cdev_controller *controller;
	struct cpufreq_cdev_device *data;
	struct device_node *child, *cpu_node;
	struct device *cpu_dev;
	int cpu, index, ret = 0;

	controller = devm_kzalloc(&pdev->dev, sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return -ENOMEM;
	INIT_LIST_HEAD(&controller->devices);
	controller->hp_state = -1;
	mutex_lock(&qti_cpufreq_cdev_lock);
	if (cpufreq_controller) {
		mutex_unlock(&qti_cpufreq_cdev_lock);
		return -EBUSY;
	}
	cpufreq_controller = controller;
	mutex_unlock(&qti_cpufreq_cdev_lock);

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		cpu = nr_cpu_ids;
		for (index = 0; ; index++) {
			cpu_node = of_parse_phandle(child, "qcom,cpus", index);
			if (!cpu_node)
				break;
			for_each_possible_cpu(cpu) {
				cpu_dev = get_cpu_device(cpu);
				if (cpu_dev && cpu_dev->of_node == cpu_node)
					break;
			}
			of_node_put(cpu_node);
			if (cpu < nr_cpu_ids)
				break;
		}
		if (cpu >= nr_cpu_ids)
			continue;
		data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
		if (!data) {
			ret = -ENOMEM;
			goto put_child;
		}
		ret = strscpy(data->cdev_name, child->name, sizeof(data->cdev_name));
		if (ret < 0)
			goto put_child;
		data->cpu = cpu;
		data->retry_cnt = MAX_RETRY_CNT;
		data->controller = controller;
		INIT_DEFERRABLE_WORK(&data->register_work, cpufreq_cdev_register);
		list_add_tail(&data->node, &controller->devices);
	}
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "thermal-cpu/cdev:online",
				cpufreq_cdev_hp_online, NULL);
	if (ret < 0)
		goto cleanup;
	controller->hp_state = ret;
	platform_set_drvdata(pdev, controller);
	return 0;

put_child:
	of_node_put(child);
cleanup:
	cpufreq_cdev_cleanup(controller);
	return ret;
}

static void cpufreq_cdev_remove(struct platform_device *pdev)
{
	cpufreq_cdev_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id cpufreq_cdev_match[] = {
	{.compatible = "qcom,cpufreq-cdev"},
	{}
};

static struct platform_driver cpufreq_cdev_driver = {
	.probe          = cpufreq_cdev_probe,
	.remove         = cpufreq_cdev_remove,
	.driver         = {
		.name   = CPUFREQ_CDEV,
		.of_match_table = cpufreq_cdev_match,
	},
};

static int __init cpufreq_cdev_init(void)
{
	return platform_driver_register(&cpufreq_cdev_driver);
}
module_init(cpufreq_cdev_init);

static void __exit cpufreq_cdev_exit(void)
{
	platform_driver_unregister(&cpufreq_cdev_driver);
}
module_exit(cpufreq_cdev_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. cpufreq cooling driver");
MODULE_LICENSE("GPL");
