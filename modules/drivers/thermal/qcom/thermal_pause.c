// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021, The Linux Foundation. All rights reserved. */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sched/walt.h>
#include <linux/thermal_pause.h>

#define THERMAL_PAUSE_RETRIES 5

struct thermal_pause_ctrl;

struct thermal_pause_cdev {
	struct list_head node;
	struct thermal_pause_ctrl *ctrl;
	cpumask_t cpu_mask;
	bool requested;
	bool applied;
	struct thermal_cooling_device *cdev;
	struct device_node *np;
};

struct thermal_pause_ctrl {
	struct device *dev;
	struct list_head cdevs;
	struct workqueue_struct *wq;
	struct delayed_work work;
	unsigned long generation;
	unsigned int retries;
	bool removing;
};

/* Protect the singleton, cooling requests and the exported applied mask. */
static DEFINE_MUTEX(cpus_pause_lock);
static struct thermal_pause_ctrl *pause_owner;
static cpumask_t cpus_in_max_cooling_level;
static BLOCKING_NOTIFIER_HEAD(thermal_pause_notifier);

void thermal_pause_notifier_register(struct notifier_block *n)
{
	blocking_notifier_chain_register(&thermal_pause_notifier, n);
}
EXPORT_SYMBOL_GPL(thermal_pause_notifier_register);

void thermal_pause_notifier_unregister(struct notifier_block *n)
{
	blocking_notifier_chain_unregister(&thermal_pause_notifier, n);
}
EXPORT_SYMBOL_GPL(thermal_pause_notifier_unregister);

const struct cpumask *thermal_paused_cpumask(void)
{
	return &cpus_in_max_cooling_level;
}
EXPORT_SYMBOL_GPL(thermal_paused_cpumask);

/* The controller's ordered worker is the only writer of WALT thermal votes. */
static int thermal_pause_apply_cpu(struct thermal_pause_ctrl *ctrl, int cpu, bool pause)
{
	cpumask_t mask;
	int ret;

	cpumask_clear(&mask);
	cpumask_set_cpu(cpu, &mask);
	ret = pause ? walt_pause_cpus(&mask, PAUSE_THERMAL) :
		      walt_resume_cpus(&mask, PAUSE_THERMAL);
	if (ret)
		return ret;

	mutex_lock(&cpus_pause_lock);
	if (pause)
		cpumask_set_cpu(cpu, &cpus_in_max_cooling_level);
	else
		cpumask_clear_cpu(cpu, &cpus_in_max_cooling_level);
	mutex_unlock(&cpus_pause_lock);
	blocking_notifier_call_chain(&thermal_pause_notifier, pause, (void *)(long)cpu);
	return 0;
}

static void thermal_pause_target(struct thermal_pause_ctrl *ctrl, cpumask_t *target)
{
	struct thermal_pause_cdev *entry;

	cpumask_clear(target);
	list_for_each_entry(entry, &ctrl->cdevs, node) {
		if (entry->requested)
			cpumask_or(target, target, &entry->cpu_mask);
	}
}

static void thermal_pause_update_work(struct work_struct *work)
{
	struct thermal_pause_ctrl *ctrl = container_of(to_delayed_work(work),
						      struct thermal_pause_ctrl, work);
	struct thermal_pause_cdev *entry;
	cpumask_t desired, pause, resume;
	unsigned long generation;
	int cpu, ret, error = 0;

	mutex_lock(&cpus_pause_lock);
	if (ctrl->removing) {
		mutex_unlock(&cpus_pause_lock);
		return;
	}
	generation = ctrl->generation;
	thermal_pause_target(ctrl, &desired);
	cpumask_andnot(&pause, &desired, &cpus_in_max_cooling_level);
	cpumask_andnot(&resume, &cpus_in_max_cooling_level, &desired);
	mutex_unlock(&cpus_pause_lock);

	/* Per-CPU deltas retain votes shared by overlapping cooling devices. */
	for_each_cpu(cpu, &pause) {
		ret = thermal_pause_apply_cpu(ctrl, cpu, true);
		if (ret)
			error = ret;
	}
	for_each_cpu(cpu, &resume) {
		ret = thermal_pause_apply_cpu(ctrl, cpu, false);
		if (ret)
			error = ret;
	}

	mutex_lock(&cpus_pause_lock);
	list_for_each_entry(entry, &ctrl->cdevs, node)
		entry->applied = cpumask_subset(&entry->cpu_mask,
						&cpus_in_max_cooling_level);
	if (!ctrl->removing) {
		if (generation != ctrl->generation) {
			ctrl->retries = 0;
			mod_delayed_work(ctrl->wq, &ctrl->work, 0);
		} else if (error && ctrl->retries++ < THERMAL_PAUSE_RETRIES) {
			mod_delayed_work(ctrl->wq, &ctrl->work, msecs_to_jiffies(20));
		} else if (error) {
			dev_err_ratelimited(ctrl->dev, "CPU pause request failed: %d\n", error);
		} else {
			ctrl->retries = 0;
		}
	}
	mutex_unlock(&cpus_pause_lock);
}

static int thermal_pause_set_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long level)
{
	struct thermal_pause_cdev *entry = cdev->devdata;
	struct thermal_pause_ctrl *ctrl = entry->ctrl;
	int ret = 0;

	if (level > 1)
		return -EINVAL;
	mutex_lock(&cpus_pause_lock);
	if (ctrl->removing) {
		ret = -ENODEV;
	} else {
		entry->requested = level;
		ctrl->generation++;
		ctrl->retries = 0;
		mod_delayed_work(ctrl->wq, &ctrl->work, 0);
	}
	mutex_unlock(&cpus_pause_lock);
	return ret;
}

static int thermal_pause_get_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long *level)
{
	struct thermal_pause_cdev *entry = cdev->devdata;

	mutex_lock(&cpus_pause_lock);
	*level = entry->applied;
	mutex_unlock(&cpus_pause_lock);
	return 0;
}

static int thermal_pause_get_max_state(struct thermal_cooling_device *cdev,
				       unsigned long *level)
{
	*level = 1;
	return 0;
}

static const struct thermal_cooling_device_ops thermal_pause_cooling_ops = {
	.get_max_state = thermal_pause_get_max_state,
	.get_cur_state = thermal_pause_get_cur_state,
	.set_cur_state = thermal_pause_set_cur_state,
};

static void thermal_pause_cleanup(struct thermal_pause_ctrl *ctrl)
{
	struct thermal_pause_cdev *entry;
	cpumask_t paused;
	int cpu, ret;

	mutex_lock(&cpus_pause_lock);
	ctrl->removing = true;
	mutex_unlock(&cpus_pause_lock);
	/* Stop cooling callbacks before draining the work they can schedule. */
	list_for_each_entry(entry, &ctrl->cdevs, node) {
		if (entry->cdev)
			thermal_cooling_device_unregister(entry->cdev);
	}
	cancel_delayed_work_sync(&ctrl->work);
	cpumask_copy(&paused, &cpus_in_max_cooling_level);
	for_each_cpu(cpu, &paused) {
		ret = thermal_pause_apply_cpu(ctrl, cpu, false);
		if (ret)
			dev_err(ctrl->dev, "Cannot release CPU%d thermal pause: %d\n", cpu, ret);
	}
	list_for_each_entry(entry, &ctrl->cdevs, node)
		of_node_put(entry->np);
	destroy_workqueue(ctrl->wq);
	mutex_lock(&cpus_pause_lock);
	pause_owner = NULL;
	mutex_unlock(&cpus_pause_lock);
}

static int thermal_pause_parse_cpus(struct device_node *np, cpumask_t *mask)
{
	int count = of_count_phandle_with_args(np, "qcom,cpus", NULL);
	int index, cpu;

	if (count <= 0)
		return -EINVAL;
	cpumask_clear(mask);
	for (index = 0; index < count; index++) {
		struct device_node *node = of_parse_phandle(np, "qcom,cpus", index);

		if (!node)
			return -EINVAL;
		for_each_possible_cpu(cpu) {
			struct device *dev = get_cpu_device(cpu);

			if (dev && dev->of_node == node)
				break;
		}
		of_node_put(node);
		if (cpu >= nr_cpu_ids)
			return -EINVAL;
		cpumask_set_cpu(cpu, mask);
	}
	return 0;
}

static int thermal_pause_probe(struct platform_device *pdev)
{
	struct thermal_pause_ctrl *ctrl;
	struct device_node *child;
	int ret;

	if (!walt_task_data_available())
		return -EPROBE_DEFER;
	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dev = &pdev->dev;
	INIT_LIST_HEAD(&ctrl->cdevs);
	INIT_DELAYED_WORK(&ctrl->work, thermal_pause_update_work);
	ctrl->wq = alloc_ordered_workqueue("thermal_pause", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!ctrl->wq)
		return -ENOMEM;
	mutex_lock(&cpus_pause_lock);
	if (pause_owner) {
		mutex_unlock(&cpus_pause_lock);
		destroy_workqueue(ctrl->wq);
		return -EBUSY;
	}
	pause_owner = ctrl;
	mutex_unlock(&cpus_pause_lock);
	platform_set_drvdata(pdev, ctrl);

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		struct thermal_pause_cdev *entry;
		const char *alias;
		char name[THERMAL_NAME_LENGTH];

		entry = devm_kzalloc(&pdev->dev, sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			ret = -ENOMEM;
			goto fail_child;
		}
		ret = thermal_pause_parse_cpus(child, &entry->cpu_mask);
		if (ret)
			goto fail_child;
		entry->ctrl = ctrl;
		entry->np = of_node_get(child);
		if (of_property_read_string(child, "qcom,cdev-alias", &alias)) {
			snprintf(name, sizeof(name), "thermal-pause-%lX",
				 cpumask_bits(&entry->cpu_mask)[0]);
			alias = name;
		}
		mutex_lock(&cpus_pause_lock);
		list_add_tail(&entry->node, &ctrl->cdevs);
		mutex_unlock(&cpus_pause_lock);
		entry->cdev = thermal_of_cooling_device_register(child, alias, entry,
								&thermal_pause_cooling_ops);
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
	return 0;
fail_child:
	of_node_put(child);
failed:
	thermal_pause_cleanup(ctrl);
	return ret;
}

static void thermal_pause_remove(struct platform_device *pdev)
{
	thermal_pause_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id thermal_pause_match[] = {
	{ .compatible = "qcom,thermal-pause" },
	{ }
};
MODULE_DEVICE_TABLE(of, thermal_pause_match);

static struct platform_driver thermal_pause_driver = {
	.probe = thermal_pause_probe,
	.remove = thermal_pause_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = thermal_pause_match,
	},
};
module_platform_driver(thermal_pause_driver);
MODULE_DESCRIPTION("Onyx CPU pause cooling with shared group votes");
MODULE_LICENSE("GPL v2");
