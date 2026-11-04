// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/bitmap.h>
#include <linux/cpufreq.h>
#include <linux/gunyah/gh_errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <asm/smp.h>

#include "gh_rm_drv_private.h"
#include "gh_rm_booster.h"

#define BOOSTER_TIMEOUT_MSEC 5000
#define BOOSTER_VM_COUNT (U16_MAX + 1U)

struct gh_rm_booster_dev {
	u32 boost_cnt;
	DECLARE_BITMAP(active_vms, BOOSTER_VM_COUNT);
	unsigned long boost_deadline;
	bool stopping;
	bool affinity_changed;
	gh_vmid_t vmid;
	unsigned int orig_cpu;
	u64 orig_cpu_mpidr;
	u64 target_cpu_mpidr;
	struct device *dev;
	struct cpufreq_policy *boost_policy;
	struct freq_qos_request gh_rm_boost_req;
	struct notifier_block gh_rm_boost_nb;
	struct delayed_work booster_release_work;
	gh_capid_t vcpu_cap_id;
};

/* The parameter and every per-device transition use this same lock. */
static struct gh_rm_booster_dev *rm_status;
static DEFINE_MUTEX(rm_booster_lock);
static unsigned int target_cpu = UINT_MAX;

static int gh_rm_booster_get_mpidr(unsigned int cpu, u64 *mpidr)
{
	if (cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return -EINVAL;
	*mpidr = cpu_logical_map(cpu);
	return *mpidr == INVALID_HWID ? -ENODEV : 0;
}

static int rm_boost_target_cpu_set(const char *val, const struct kernel_param *kp)
{
	unsigned int cpu;
	u64 mpidr;
	int ret;

	ret = kstrtouint(val, 10, &cpu);
	if (ret)
		return ret;
	ret = gh_rm_booster_get_mpidr(cpu, &mpidr);
	if (ret)
		return ret;
	mutex_lock(&rm_booster_lock);
	if (rm_status && rm_status->boost_cnt) {
		ret = -EBUSY;
		goto out;
	}
	ret = param_set_uint(val, kp);
	if (!ret && rm_status)
		rm_status->target_cpu_mpidr = mpidr;
out:
	mutex_unlock(&rm_booster_lock);
	return ret;
}

static int rm_boost_target_cpu_get(char *buf, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&rm_booster_lock);
	ret = param_get_uint(buf, kp);
	mutex_unlock(&rm_booster_lock);
	return ret;
}

static const struct kernel_param_ops target_cpu_ops = {
	.set = rm_boost_target_cpu_set,
	.get = rm_boost_target_cpu_get,
};
module_param_cb(target_cpu, &target_cpu_ops, &target_cpu, 0644);
MODULE_PARM_DESC(target_cpu, "Choose the target core for Resource Manager");

static int gh_set_rm_affinity(struct gh_rm_booster_dev *st, unsigned int cpu, u64 mpidr)
{
	int ret;

	ret = gh_hcall_change_rm_affinity_mpidr(st->vcpu_cap_id, mpidr);
	if (ret == GH_ERROR_ARG_INVAL) {
		dev_dbg(st->dev, "MPIDR affinity unsupported; use logical CPU\n");
		ret = gh_hcall_change_rm_affinity(st->vcpu_cap_id, cpu);
	}
	return gh_remap_error(ret);
}

static int gh_boost_rmfreq(struct gh_rm_booster_dev *st, unsigned int cpu)
{
	struct cpufreq_policy *policy;
	int ret;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -ENODEV;
	ret = freq_qos_add_request(&policy->constraints, &st->gh_rm_boost_req,
				  FREQ_QOS_MIN, policy->max);
	if (ret < 0) {
		cpufreq_cpu_put(policy);
		return ret;
	}
	/* Retain the policy until its embedded constraint request is removed. */
	st->boost_policy = policy;
	return 0;
}

static void gh_resume_rm_status(struct gh_rm_booster_dev *st)
{
	int ret;

	if (st->boost_policy) {
		ret = freq_qos_remove_request(&st->gh_rm_boost_req);
		if (ret < 0)
			dev_err(st->dev, "Failed to remove RM frequency vote: %d\n", ret);
		cpufreq_cpu_put(st->boost_policy);
		st->boost_policy = NULL;
	}
	if (st->affinity_changed) {
		ret = gh_set_rm_affinity(st, st->orig_cpu, st->orig_cpu_mpidr);
		if (ret)
			dev_err(st->dev, "Failed to restore RM affinity: %d\n", ret);
		else
			st->affinity_changed = false;
	}
}

static void gh_configure_rm(struct gh_rm_booster_dev *st,
			    const struct gh_rm_notif_vm_status_payload *status)
{
	unsigned int dest_cpu;
	int ret;

	mutex_lock(&rm_booster_lock);
	if (st->stopping)
		goto out;
	switch (status->vm_status) {
	case GH_RM_VM_STATUS_AUTH:
		st->boost_deadline = jiffies + msecs_to_jiffies(BOOSTER_TIMEOUT_MSEC);
		mod_delayed_work(system_wq, &st->booster_release_work,
				 msecs_to_jiffies(BOOSTER_TIMEOUT_MSEC));
		/* Repeated AUTH and unrelated completion must not corrupt the count. */
		if (test_and_set_bit(status->vmid, st->active_vms))
			break;
		if (++st->boost_cnt > 1)
			break;
		dest_cpu = target_cpu;
		if (dest_cpu != st->orig_cpu) {
			ret = st->vcpu_cap_id == GH_CAPID_INVAL ? -ENODEV :
				gh_set_rm_affinity(st, dest_cpu, st->target_cpu_mpidr);
			if (ret) {
				dest_cpu = st->orig_cpu;
				dev_info(st->dev, "Use original RM CPU%u after affinity error %d\n",
					 dest_cpu, ret);
			} else {
				st->affinity_changed = true;
			}
		}
		ret = gh_boost_rmfreq(st, dest_cpu);
		if (ret) {
			dev_err(st->dev, "Failed to boost RM frequency: %d\n", ret);
			gh_resume_rm_status(st);
		}
		break;
	case GH_RM_VM_STATUS_INIT_FAILED:
	case GH_RM_VM_STATUS_RUNNING:
		if (!test_and_clear_bit(status->vmid, st->active_vms))
			break;
		if (--st->boost_cnt)
			break;
		gh_resume_rm_status(st);
		/* The worker uses this lock; never wait synchronously under it. */
		cancel_delayed_work(&st->booster_release_work);
		break;
	default:
		break;
	}
out:
	mutex_unlock(&rm_booster_lock);
}

static int gh_rm_boost_nb_handler(struct notifier_block *nb, unsigned long cmd, void *data)
{
	struct gh_rm_booster_dev *st = container_of(nb, struct gh_rm_booster_dev, gh_rm_boost_nb);

	if (cmd == GH_RM_NOTIF_VM_STATUS && data)
		gh_configure_rm(st, data);
	return NOTIFY_DONE;
}

static void gh_rm_booster_release_func(struct work_struct *work)
{
	struct gh_rm_booster_dev *st = container_of(to_delayed_work(work),
						  struct gh_rm_booster_dev, booster_release_work);

	mutex_lock(&rm_booster_lock);
	if (st->stopping || !st->boost_cnt)
		goto out;
	/* A running old timeout may have been extended by a new AUTH. */
	if (time_before(jiffies, st->boost_deadline)) {
		mod_delayed_work(system_wq, &st->booster_release_work, st->boost_deadline - jiffies);
		goto out;
	}
	gh_resume_rm_status(st);
	bitmap_zero(st->active_vms, BOOSTER_VM_COUNT);
	st->boost_cnt = 0;
out:
	mutex_unlock(&rm_booster_lock);
}

static void gh_rm_booster_populate_res(struct gh_rm_booster_dev *st)
{
	struct gh_vm_get_hyp_res_resp_entry *entries;
	u32 count, i;

	entries = gh_rm_vm_get_hyp_res(st->vmid, &count);
	if (IS_ERR_OR_NULL(entries)) {
		dev_warn(st->dev, "RM vCPU resource unavailable; retain original CPU\n");
		return;
	}
	for (i = 0; i < count; i++) {
		if (entries[i].res_type == GH_RM_RES_TYPE_VCPU) {
			st->vcpu_cap_id = (u64)entries[i].cap_id_high << 32 | entries[i].cap_id_low;
			break;
		}
	}
	kfree(entries);
}

static int gh_rm_booster_probe(struct platform_device *pdev)
{
	struct gh_rm_booster_dev *st;
	u32 vmid;
	int ret;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->dev = &pdev->dev;
	ret = of_property_read_u32(pdev->dev.of_node, "qcom,rm-vmid", &vmid);
	if (ret)
		goto fail;
	if (vmid > U16_MAX) {
		ret = -EINVAL;
		goto fail;
	}
	st->vmid = vmid;
	ret = of_property_read_u32(pdev->dev.of_node, "qcom,rm-affinity-default", &st->orig_cpu);
	if (ret)
		goto fail;
	ret = gh_rm_booster_get_mpidr(st->orig_cpu, &st->orig_cpu_mpidr);
	if (ret)
		goto fail;
	INIT_DELAYED_WORK(&st->booster_release_work, gh_rm_booster_release_func);
	st->vcpu_cap_id = GH_CAPID_INVAL;
	gh_rm_booster_populate_res(st);
	st->gh_rm_boost_nb.notifier_call = gh_rm_boost_nb_handler;
	st->gh_rm_boost_nb.priority = INT_MAX;

	mutex_lock(&rm_booster_lock);
	if (rm_status) {
		ret = -EBUSY;
		goto unlock_fail;
	}
	if (target_cpu == UINT_MAX)
		target_cpu = cpumask_last(cpu_possible_mask);
	ret = gh_rm_booster_get_mpidr(target_cpu, &st->target_cpu_mpidr);
	if (ret)
		goto unlock_fail;
	rm_status = st;
	ret = gh_rm_register_notifier(&st->gh_rm_boost_nb);
	if (ret) {
		rm_status = NULL;
		goto unlock_fail;
	}
	platform_set_drvdata(pdev, st);
	mutex_unlock(&rm_booster_lock);
	return 0;
unlock_fail:
	mutex_unlock(&rm_booster_lock);
fail:
	kfree(st);
	return ret;
}

static void gh_rm_booster_remove(struct platform_device *pdev)
{
	struct gh_rm_booster_dev *st = platform_get_drvdata(pdev);

	mutex_lock(&rm_booster_lock);
	st->stopping = true;
	mutex_unlock(&rm_booster_lock);
	/* SRCU unregistration drains callbacks that already have the device. */
	gh_rm_unregister_notifier(&st->gh_rm_boost_nb);
	cancel_delayed_work_sync(&st->booster_release_work);
	mutex_lock(&rm_booster_lock);
	gh_resume_rm_status(st);
	/* Keep the singleton reserved until the old affinity/vote is restored. */
	rm_status = NULL;
	mutex_unlock(&rm_booster_lock);
	kfree(st);
}

static const struct of_device_id gh_rm_booster_match_table[] = {
	{ .compatible = "qcom,gh-rm-booster" },
	{}
};

static struct platform_driver gh_rm_booster_driver = {
	.driver = {
		.name = "gh_rm_booster",
		.of_match_table = gh_rm_booster_match_table,
	},
	.probe = gh_rm_booster_probe,
	.remove = gh_rm_booster_remove,
};
module_platform_driver(gh_rm_booster_driver);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Gunyah RM booster Driver");
MODULE_LICENSE("GPL");
