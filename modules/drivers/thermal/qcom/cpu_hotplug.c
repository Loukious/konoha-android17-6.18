// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/workqueue.h>

struct cpu_hot_ctrl;

struct cpu_hot_cdev {
	struct list_head node;
	struct cpu_hot_ctrl *ctrl;
	unsigned int cpu_id;
	bool requested;
	bool offlined_by_us;
	struct thermal_cooling_device *cdev;
	struct device_node *np;
};

struct cpu_hot_ctrl {
	struct device *dev;
	struct list_head cdevs;
	struct workqueue_struct *wq;
	struct work_struct work;
	unsigned long generation;
	int hp_state;
	bool removing;
};

/* Never hold this lock across a CPU hotplug operation or cooling unregister. */
static DEFINE_MUTEX(cpu_hot_lock);
static struct cpu_hot_ctrl *hot_owner;

static int cpu_hot_hp_online(unsigned int cpu)
{
	struct cpu_hot_cdev *entry;
	int ret = 0;

	mutex_lock(&cpu_hot_lock);
	if (hot_owner && !hot_owner->removing) {
		list_for_each_entry(entry, &hot_owner->cdevs, node) {
			if (entry->cpu_id == cpu && entry->requested) {
				ret = -EBUSY;
				break;
			}
		}
	}
	mutex_unlock(&cpu_hot_lock);
	return ret;
}

static void cpu_hot_execute_cdev(struct work_struct *work)
{
	struct cpu_hot_ctrl *ctrl = container_of(work, struct cpu_hot_ctrl, work);
	struct cpu_hot_cdev *entry;
	unsigned long generation;
	int ret;

	mutex_lock(&cpu_hot_lock);
	if (ctrl->removing) {
		mutex_unlock(&cpu_hot_lock);
		return;
	}
	generation = ctrl->generation;
	list_for_each_entry(entry, &ctrl->cdevs, node) {
		bool offline = entry->requested;

		/* Preserve CPUs that were already offline for an unrelated reason. */
		if (offline && !cpu_online(entry->cpu_id))
			continue;
		if (!offline && !entry->offlined_by_us)
			continue;
		mutex_unlock(&cpu_hot_lock);
		ret = offline ? remove_cpu(entry->cpu_id) : add_cpu(entry->cpu_id);
		mutex_lock(&cpu_hot_lock);
		if (!ret)
			entry->offlined_by_us = offline;
		else
			dev_err_ratelimited(ctrl->dev, "CPU%u %s failed: %d\n", entry->cpu_id,
					    offline ? "offline" : "online", ret);
		if (ctrl->removing)
			break;
	}
	if (!ctrl->removing && generation != ctrl->generation)
		queue_work(ctrl->wq, &ctrl->work);
	mutex_unlock(&cpu_hot_lock);
}

static int cpu_hot_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct cpu_hot_cdev *entry = cdev->devdata;
	struct cpu_hot_ctrl *ctrl = entry->ctrl;
	int ret = 0;

	if (state > 1)
		return -EINVAL;
	mutex_lock(&cpu_hot_lock);
	if (ctrl->removing) {
		ret = -ENODEV;
	} else {
		entry->requested = state;
		ctrl->generation++;
		queue_work(ctrl->wq, &ctrl->work);
	}
	mutex_unlock(&cpu_hot_lock);
	return ret;
}

static int cpu_hot_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct cpu_hot_cdev *entry = cdev->devdata;

	*state = !cpu_online(entry->cpu_id);
	return 0;
}

static int cpu_hot_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = 1;
	return 0;
}

static const struct thermal_cooling_device_ops cpu_hot_cooling_ops = {
	.get_max_state = cpu_hot_get_max_state,
	.get_cur_state = cpu_hot_get_cur_state,
	.set_cur_state = cpu_hot_set_cur_state,
};

static void cpu_hot_cleanup(struct cpu_hot_ctrl *ctrl)
{
	struct cpu_hot_cdev *entry;
	int ret;

	mutex_lock(&cpu_hot_lock);
	ctrl->removing = true;
	mutex_unlock(&cpu_hot_lock);
	if (ctrl->hp_state >= 0)
		cpuhp_remove_state_nocalls(ctrl->hp_state);
	list_for_each_entry(entry, &ctrl->cdevs, node) {
		if (entry->cdev)
			thermal_cooling_device_unregister(entry->cdev);
	}
	cancel_work_sync(&ctrl->work);
	list_for_each_entry(entry, &ctrl->cdevs, node) {
		if (entry->offlined_by_us) {
			ret = add_cpu(entry->cpu_id);
			if (ret)
				dev_err(ctrl->dev, "Cannot restore CPU%u: %d\n",
					entry->cpu_id, ret);
		}
		of_node_put(entry->np);
	}
	destroy_workqueue(ctrl->wq);
	mutex_lock(&cpu_hot_lock);
	hot_owner = NULL;
	mutex_unlock(&cpu_hot_lock);
}

static int cpu_hot_probe(struct platform_device *pdev)
{
	struct cpu_hot_ctrl *ctrl;
	struct device_node *child;
	cpumask_t seen;
	int ret;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dev = &pdev->dev;
	ctrl->hp_state = -1;
	INIT_LIST_HEAD(&ctrl->cdevs);
	INIT_WORK(&ctrl->work, cpu_hot_execute_cdev);
	cpumask_clear(&seen);
	ctrl->wq = alloc_ordered_workqueue("cpu_hotplug_cooling", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!ctrl->wq)
		return -ENOMEM;
	mutex_lock(&cpu_hot_lock);
	if (hot_owner) {
		mutex_unlock(&cpu_hot_lock);
		destroy_workqueue(ctrl->wq);
		return -EBUSY;
	}
	hot_owner = ctrl;
	mutex_unlock(&cpu_hot_lock);
	platform_set_drvdata(pdev, ctrl);

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		struct cpu_hot_cdev *entry;
		struct device_node *node;
		unsigned int cpu;
		const char *alias;
		char name[THERMAL_NAME_LENGTH];

		node = of_parse_phandle(child, "qcom,cpu", 0);
		if (!node) {
			ret = -EINVAL;
			goto fail_child;
		}
		for_each_possible_cpu(cpu) {
			struct device *dev = get_cpu_device(cpu);

			if (dev && dev->of_node == node)
				break;
		}
		of_node_put(node);
		if (cpu >= nr_cpu_ids || cpumask_test_and_set_cpu(cpu, &seen)) {
			ret = -EINVAL;
			goto fail_child;
		}
		entry = devm_kzalloc(&pdev->dev, sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			ret = -ENOMEM;
			goto fail_child;
		}
		entry->ctrl = ctrl;
		entry->cpu_id = cpu;
		entry->np = of_node_get(child);
		if (of_property_read_string(child, "qcom,cdev-alias", &alias)) {
			snprintf(name, sizeof(name), "cpu-hotplug%u", cpu);
			alias = name;
		}
		mutex_lock(&cpu_hot_lock);
		list_add_tail(&entry->node, &ctrl->cdevs);
		mutex_unlock(&cpu_hot_lock);
		entry->cdev = thermal_of_cooling_device_register(child, alias, entry,
								&cpu_hot_cooling_ops);
		if (IS_ERR(entry->cdev)) {
			ret = PTR_ERR(entry->cdev);
			entry->cdev = NULL;
			goto fail_child;
		}
	}
	if (list_empty(&ctrl->cdevs)) {
		ret = -ENODEV;
		goto failed;
	}
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "cpu-hotplug/cdev:online",
					cpu_hot_hp_online, NULL);
	if (ret < 0)
		goto failed;
	ctrl->hp_state = ret;
	return 0;
fail_child:
	of_node_put(child);
failed:
	cpu_hot_cleanup(ctrl);
	return ret;
}

static void cpu_hot_remove(struct platform_device *pdev)
{
	cpu_hot_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id cpu_hot_match[] = {
	{ .compatible = "qcom,cpu-hotplug" },
	{ }
};
MODULE_DEVICE_TABLE(of, cpu_hot_match);

static struct platform_driver cpu_hot_driver = {
	.probe = cpu_hot_probe,
	.remove = cpu_hot_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = cpu_hot_match,
	},
};
module_platform_driver(cpu_hot_driver);
MODULE_DESCRIPTION("Onyx CPU hotplug cooling device driver");
MODULE_LICENSE("GPL");
