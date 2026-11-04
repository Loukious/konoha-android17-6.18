// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/cpumask.h>
#include <linux/cpu_pm.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rculist.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sched/idle.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tick.h>
#include <linux/time64.h>

#include <trace/hooks/pm_domain.h>
#include <linux/overflow.h>

#define CREATE_TRACE_POINTS
#include "trace-cluster-lpm.h"
#include "qcom-lpm.h"

static LIST_HEAD(cluster_dev_list);
static DEFINE_MUTEX(cluster_dev_lock);
static bool cluster_prediction_enabled;
static struct tracepoint *allow_domain_state_tp;

static struct lpm_cluster *to_cluster(struct generic_pm_domain *genpd)
{
	struct lpm_cluster *cluster_gov;

	list_for_each_entry_rcu(cluster_gov, &cluster_dev_list, list)
		if (cluster_gov->genpd == genpd)
			return cluster_gov;

	return NULL;
}

/**
 * clusttimer_fn() - Will be executed when cluster prediction timer expires
 * @h:      Cluster prediction timer
 */
static enum hrtimer_restart clusttimer_fn(struct hrtimer *h)
{
	struct lpm_cluster *cluster_gov = container_of(h,
						struct lpm_cluster, histtimer);

	if (cluster_gov->need_timer_requeue)
		goto exit;

	cluster_gov->history_invalid = true;
	cluster_gov->htmr_wkup = true;
	cluster_gov->predicted = false;
	cluster_gov->restrict_idx = -1;
	cluster_gov->pred_residency = 0;
exit:
	cluster_gov->is_timer_expired = true;
	cluster_gov->is_timer_queued = false;

	return HRTIMER_NORESTART;
}

/**
 * clusttimer_start()  - Programs the hrtimer with given timer value
 * @time_ns:      Value to be program
 */
static void clusttimer_start(struct lpm_cluster *cluster_gov, s64 time_ns)
{
	struct hrtimer *timer = &cluster_gov->histtimer;

	hrtimer_start(timer, time_ns, HRTIMER_MODE_REL_PINNED);
}

/**
 * clusttimer_cancel()  - Cancel the hrtimr after cluster wakeup from sleep
 * @cluster_gov: Targeted cluster's lpm data structure
 */
static void clusttimer_cancel(struct lpm_cluster *cluster_gov)
{
	ktime_t time_rem;

	time_rem = hrtimer_get_remaining(&cluster_gov->histtimer);
	if (time_rem > 0)
		hrtimer_try_to_cancel(&cluster_gov->histtimer);
}

/**
 * cluster_predict() - Predict the cluster's next wakeup.
 * @cluster_gov:  Targeted cluster's lpm data structure
 */
static void cluster_predict(struct lpm_cluster *cluster_gov)
{
	struct generic_pm_domain *genpd = cluster_gov->genpd;
	int i, j, idx = genpd->state_idx;
	u64 avg_residency = 0;

	if (prediction_disabled || cluster_gov->use_bias_timer)
		return;

	cluster_gov->pred_wakeup = KTIME_MAX;
	cluster_gov->predicted = false;
	cluster_gov->pred_residency = 0;

	/*
	 * Samples are marked invalid when woken-up due to timer,
	 * so do not predict.
	 */
	if (cluster_gov->history_invalid) {
		cluster_gov->history_invalid = false;
		return;
	}

	/*
	 * Cluster wakes up whenever any core of the cluster wakes up.
	 * Since for the last cluster LPM exit, there could be multiple core(s)
	 * LPMs. So, consider only recent history for the cluster.
	 */
	if (cluster_gov->nsamp == MAXSAMPLES) {
		for (i = 0; i < MAXSAMPLES; i++) {
			if ((cluster_gov->now - cluster_gov->history[i].entry_time)
					> cluster_gov->samples_invalid_time)
				cluster_gov->nsamp--;
		}
	}

	/* Predict only when all the samples are collected. */
	if (cluster_gov->nsamp < MAXSAMPLES)
		return;

	/*
	 * If cluster's last entered mode is shallower state then calculate
	 * the next predicted wakeup as avg of previous samples
	 */
	if (idx < genpd->state_count - 1) {
		avg_residency = 0;

		for (i = 0; i < MAXSAMPLES; i++)
			avg_residency += cluster_gov->history[i].residency;
		do_div(avg_residency, MAXSAMPLES);
		cluster_gov->pred_residency = avg_residency;
		cluster_gov->predicted = true;

		if (avg_residency <= genpd->states[genpd->state_count - 1].residency_ns)
			cluster_gov->restrict_idx = genpd->state_count - 1;
		else
			cluster_gov->restrict_idx = -1;

		return;
	}

	/*
	 * Find the number of premature exits for each of the mode,
	 * and if they are more than fifty percent restrict that and
	 * deeper modes.
	 */
	for (j = 0; j < genpd->state_count; j++) {
		u32 count = 0;

		avg_residency = 0;

		for (i = 0; i < MAXSAMPLES; i++) {

			if ((cluster_gov->history[i].mode == j) &&
			    (cluster_gov->history[i].residency < genpd->states[j].residency_ns)) {
				count++;
				avg_residency +=
					cluster_gov->history[i].residency;
			}
		}

		if (count >= cluster_gov->pred_premature_cnt) {
			do_div(avg_residency, count);
			cluster_gov->pred_residency = avg_residency;
			cluster_gov->predicted = true;
			cluster_gov->restrict_idx = j;
			return;
		}
	}
}

/**
 * clear_cluster_history() - Clears the stored previous samples data.
 *			   It will be called when APSS going to deep sleep.
 * @cluster_gov: Targeted cluster's lpm data structure
 */
static void clear_cluster_history(struct lpm_cluster *cluster_gov)
{
	int i;

	for (i = 0; i < MAXSAMPLES; i++) {
		cluster_gov->history[i].residency  = 0;
		cluster_gov->history[i].mode = -1;
		cluster_gov->history[i].entry_time = 0;
	}

	cluster_gov->samples_idx = 0;
	cluster_gov->pre_timer = false;
	cluster_gov->nsamp = 0;
	cluster_gov->history_invalid = false;
	cluster_gov->htmr_wkup = false;
	cluster_gov->predicted = false;
	cluster_gov->restrict_idx = -1;
}

/**
 * update_cluster_history() - Update the smaples history data every time when
 *			    cluster exit from sleep.
 * @cluster_gov: Targeted cluster's lpm data structure
 */
static void update_cluster_history(struct lpm_cluster *cluster_gov)
{
	bool tmr = false;
	s64 residency = 0;
	struct generic_pm_domain *genpd = cluster_gov->genpd;
	int idx = genpd->state_idx, samples_idx = cluster_gov->samples_idx;

	if (prediction_disabled || cluster_gov->entry_idx != idx || cluster_gov->use_bias_timer)
		return;

	residency = ktime_sub(cluster_gov->now, cluster_gov->entry_time);
	cluster_gov->history[samples_idx].entry_time = cluster_gov->entry_time;

	if (cluster_gov->htmr_wkup) {
		cluster_gov->history[samples_idx].residency = residency;
		cluster_gov->htmr_wkup = false;
		cluster_gov->pre_timer = false;
		tmr = true;
	} else
		cluster_gov->history[samples_idx].residency = residency;

	cluster_gov->history[samples_idx].mode = idx;
	cluster_gov->entry_idx = INT_MIN;
	cluster_gov->entry_time = 0;
	if (cluster_gov->nsamp < MAXSAMPLES)
		cluster_gov->nsamp++;
	trace_cluster_pred_hist(cluster_gov->history[samples_idx].mode,
				cluster_gov->history[samples_idx].residency,
				samples_idx, tmr);
	samples_idx++;
	if (samples_idx >= MAXSAMPLES)
		samples_idx = 0;

	cluster_gov->samples_idx = samples_idx;
}

/**
 * get_cluster_sleep_time() - It returns the aggregated next_wakeup of all cpus
 *			    which are in online for this cluster domain.
 * @cluster_gov: Targeted cluster's lpm data structure
 */
static ktime_t get_cluster_sleep_time(struct lpm_cluster *cluster_gov)
{
	int cpu;
	ktime_t next_wakeup, next_cpu_wakeup;
	struct generic_pm_domain *genpd = cluster_gov->genpd;

	next_wakeup = KTIME_MAX;
	for_each_cpu_and(cpu, genpd->cpus, cpu_online_mask) {
		next_cpu_wakeup = *per_cpu_ptr(cluster_gov->cpu_next_wakeup, cpu);
		if (ktime_before(next_cpu_wakeup, next_wakeup))
			next_wakeup = next_cpu_wakeup;
	}

	return next_wakeup;
}

/**
 * cluster_power_down() - Will be called when cluster domain going to power off.
 *			If this entry's next wakeup was predicted it programs
 *			the cluster prediction timer and stores the idx entering
 *			and entry time of this lpm into clusters private data
 *			structure.
 * @cluster_gov:  cluster's lpm data structure
 */
static int cluster_power_down(struct lpm_cluster *cluster_gov)
{
	struct generic_pm_domain *genpd = cluster_gov->genpd;
	struct genpd_governor_data *gd = genpd->gd;
	int idx = genpd->state_idx;
	ktime_t cpu_wakeup;
	s64 residency, cpus_qos;
	int i;

	if (idx < 0 || idx >= genpd->state_count)
		return -EINVAL;

	cluster_gov->entry_time = cluster_gov->now;
	cluster_gov->entry_idx = idx;
	trace_cluster_pred_select(genpd->state_idx, gd->next_wakeup, cluster_gov->restrict_idx,
				  cluster_gov->predicted, cluster_gov->pred_residency);

	cpus_qos = get_cpus_qos(cluster_gov->genpd->cpus);
	for (i = 0; i < genpd->state_count; i++) {
		if (idx == i &&
		    cpus_qos < genpd->states[i].power_on_latency_ns)
			return -1;
	}

	if (!cluster_bias_disabled &&
	    cluster_gov->use_bias_timer &&
	    num_possible_cpus() != cpumask_weight(cluster_gov->genpd->cpus)) {
		if (!cluster_gov->is_timer_expired && !cluster_gov->is_timer_queued) {
			cluster_gov->need_timer_requeue = false;
			clusttimer_cancel(cluster_gov);
			clusttimer_start(cluster_gov, NSEC_PER_MSEC *
					 CLUST_BIAS_TIME_MSEC);
			cluster_gov->is_timer_queued = true;
			cluster_gov->timer_cpu = smp_processor_id();
			return -1;
		}
		if (cluster_gov->is_timer_queued) {
			cluster_gov->need_timer_requeue = true;
			return -1;
		}

		cluster_gov->htmr_wkup = false;
		cluster_gov->is_timer_expired = false;

		if (cluster_gov->need_timer_requeue) {
			cluster_gov->need_timer_requeue = false;
			clusttimer_cancel(cluster_gov);
			clusttimer_start(cluster_gov, NSEC_PER_MSEC *
					 CLUST_BIAS_TIME_MSEC);
			cluster_gov->is_timer_queued = true;
			cluster_gov->timer_cpu = smp_processor_id();
			return -1;
		}

		cluster_gov->timer_cpu = -1;
		return 0;
	}

	if (cluster_gov->need_timer_requeue)
		goto exit;

	cluster_gov->timer_cpu = -1;
	if ((idx == genpd->state_count - 1 && cluster_gov->restrict_idx == -1) ||
	    !cluster_gov->predicted) {
		if (num_possible_cpus() == cpumask_weight(cluster_gov->genpd->cpus))
			return 0;

		cpu_wakeup = *this_cpu_ptr(cluster_gov->cpu_next_wakeup);
		if (cpu_wakeup == get_cluster_sleep_time(cluster_gov)) {
			residency = cpu_wakeup - genpd->states[idx].power_on_latency_ns
				    - cluster_gov->now;
			if (!cluster_gov->is_timer_expired)
				clusttimer_cancel(cluster_gov);

			clusttimer_start(cluster_gov, residency);
			cluster_gov->is_timer_expired = false;
			cluster_gov->need_timer_requeue = false;
			cluster_gov->timer_cpu = smp_processor_id();
			cluster_gov->pre_timer = true;
		}
		return 0;
	}

	if (cluster_gov->pred_wakeup != KTIME_MAX &&
	    ktime_before(cluster_gov->next_wakeup, cluster_gov->pred_wakeup))
		return 0;

exit:
	if (!cluster_gov->is_timer_expired)
		clusttimer_cancel(cluster_gov);

	if (idx != genpd->state_count - 1)
		residency = genpd->states[idx + 1].residency_ns;
	else
		residency = genpd->states[idx].residency_ns;

	clusttimer_start(cluster_gov, residency + PRED_TIMER_ADD * NSEC_PER_USEC);
	cluster_gov->is_timer_queued = true;
	cluster_gov->is_timer_expired = false;
	cluster_gov->need_timer_requeue = false;
	cluster_gov->timer_cpu = smp_processor_id();

	return -1;
}

/**
 * cluster_power_cb() - It will be called when cluster domain power_off/power_on
 * @nb:   notifier block of the cluster
 * @action:  action i.e power_off/power_on
 * @data:  pointer to private data structure
 *
 * It returns the NOTIFY_OK/NOTIFY_BAD to notify the notifier call chain
 */
static int cluster_power_event(struct notifier_block *nb,
			    unsigned long action, void *data)
{
	struct lpm_cluster *cluster_gov = container_of(nb, struct lpm_cluster, genpd_nb);
	struct generic_pm_domain *pd = cluster_gov->genpd;
	struct genpd_power_state *state;
	struct lpm_cpu *cpu_gov;
	int cpu, ret = 0;
	u32 suspend_param;
	unsigned long flags;

	if (!READ_ONCE(cluster_gov->initialized))
		return NOTIFY_OK;
	if (pd->state_idx < 0 || pd->state_idx >= pd->state_count)
		return action == GENPD_NOTIFY_PRE_OFF ? NOTIFY_BAD : NOTIFY_OK;
	state = &pd->states[pd->state_idx];
	suspend_param = state->data ? *(u32 *)state->data : 0;

	switch (action) {
	case GENPD_NOTIFY_ON:
		trace_cluster_exit(raw_smp_processor_id(), pd->state_idx, suspend_param);
		if (cluster_gov->genpd->suspended_count != 0)
			break;

		cluster_gov->now = ktime_get();
		clusttimer_cancel(cluster_gov);
		update_cluster_history(cluster_gov);
		cluster_predict(cluster_gov);
		break;
	case GENPD_NOTIFY_PRE_OFF:
		if (!pd->gd)
			return NOTIFY_BAD;

		if (!cpumask_intersects(cluster_gov->genpd->cpus, cpu_online_mask))
			return NOTIFY_OK;

		if (!READ_ONCE(cluster_gov->state_allowed[pd->state_idx]))
			return NOTIFY_BAD;

		if (cluster_gov->genpd->suspended_count != 0) {
			clear_cpu_predict_history();
			clear_cluster_history(cluster_gov);
			break;
		}

		for_each_cpu(cpu, cluster_gov->genpd->cpus) {
			if (cpu_online(cpu)) {
				cpu_gov = per_cpu_ptr(&lpm_cpu_data, cpu);
				if (!cpu_gov->enable)
					continue;
				if (spin_trylock_irqsave(&cpu_gov->lock, flags)) {
					if (cpu_gov->ipi_pending) {
						spin_unlock_irqrestore(&cpu_gov->lock, flags);
						return NOTIFY_BAD;
					}
					spin_unlock_irqrestore(&cpu_gov->lock, flags);
				}
			}
		}

		cluster_gov->now = ktime_get();
		ret = cluster_power_down(cluster_gov);
		if (ret)
			return NOTIFY_BAD;

		if (cluster_gov->restrict_idx != -1 &&
		    pd->state_idx >= cluster_gov->restrict_idx)
			return NOTIFY_BAD;

		break;
	case GENPD_NOTIFY_OFF:
		trace_cluster_enter(raw_smp_processor_id(), pd->state_idx, suspend_param);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static int cluster_power_cb(struct notifier_block *nb,
			    unsigned long action, void *data)
{
	int ret = NOTIFY_OK;

	rcu_read_lock();
	if (READ_ONCE(cluster_prediction_enabled))
		ret = cluster_power_event(nb, action, data);
	rcu_read_unlock();
	return ret;
}



/**
 * update_cluster_next_wakeup() - Update the this cluster device next wakeup with
 *				aggregated next_wakeup of all cpus which are in
 *				lpm for this cluster or this clusters predicted
 *				next wakeup whichever is earlier.
 * @cluster_gov: Targeted cluster's lpm data structure
 */
static void update_cluster_next_wakeup(struct lpm_cluster *cluster_gov)
{
	cluster_gov->next_wakeup = get_cluster_sleep_time(cluster_gov);

	if (ktime_before(cluster_gov->next_wakeup, ktime_get()))
		cluster_gov->next_wakeup = KTIME_MAX;

	dev_pm_genpd_set_next_wakeup(cluster_gov->dev, cluster_gov->next_wakeup);
}

/**
 * cluster_gov_reflect() - This will be called when cpu exiting lpm to update
 *			   its cluster governor.
 * @cpu_gov: CPU's lpm data structure.
 */
static void cluster_gov_reflect(struct lpm_cpu *cpu_gov)
{
	struct generic_pm_domain *genpd;
	struct lpm_cluster *cluster_gov;
	int cpu = cpu_gov->cpu;

	rcu_read_lock();
	list_for_each_entry_rcu(cluster_gov, &cluster_dev_list, list) {
		if (!READ_ONCE(cluster_gov->initialized))
			continue;

		genpd = cluster_gov->genpd;
		if (cpumask_test_cpu(cpu, genpd->cpus)) {
			spin_lock(&cluster_gov->lock);
			if (cluster_gov->is_timer_queued) {
				if (cluster_gov->use_bias_timer || cluster_gov->timer_cpu != cpu)
					cluster_gov->need_timer_requeue = true;
			}
			spin_unlock(&cluster_gov->lock);
		}
	}
	rcu_read_unlock();
}

/**
 * update_cluster_select() - This will be called when cpu is going to lpm to update
 *			   its next wakeup value to corresponding cluster domain device.
 * @cpu_gov: CPU's lpm data structure.
 */
static void update_cluster_select(struct lpm_cpu *cpu_gov)
{
	struct generic_pm_domain *genpd;
	struct lpm_cluster *cluster_gov;
	int cpu = cpu_gov->cpu;

	rcu_read_lock();
	list_for_each_entry_rcu(cluster_gov, &cluster_dev_list, list) {
		if (!READ_ONCE(cluster_gov->initialized))
			continue;

		genpd = cluster_gov->genpd;
		if (cpumask_test_cpu(cpu, genpd->cpus)) {
			spin_lock(&cluster_gov->lock);
			cluster_gov->now = cpu_gov->now;
			*per_cpu_ptr(cluster_gov->cpu_next_wakeup, cpu) = cpu_gov->next_wakeup;
			update_cluster_next_wakeup(cluster_gov);
			spin_unlock(&cluster_gov->lock);
		}
	}
	rcu_read_unlock();
}

static void android_vh_allow_domain_state(void *unused,
					  struct generic_pm_domain *genpd,
					  uint32_t idx, bool *allow)
{
	struct lpm_cluster *cluster_gov;

	rcu_read_lock();
	cluster_gov = to_cluster(genpd);
	if (cluster_gov && READ_ONCE(cluster_gov->initialized))
		*allow = idx < genpd->state_count && idx < MAX_CLUSTER_STATES &&
			 READ_ONCE(cluster_gov->state_allowed[idx]);
	rcu_read_unlock();
}

static void cluster_gov_disable(void)
{
	struct lpm_cluster *cluster_gov;

	WRITE_ONCE(cluster_prediction_enabled, false);
	tracepoint_probe_unregister(allow_domain_state_tp, android_vh_allow_domain_state, NULL);
	tracepoint_synchronize_unregister();
	synchronize_rcu();
	mutex_lock(&cluster_dev_lock);
	list_for_each_entry(cluster_gov, &cluster_dev_list, list) {
		hrtimer_cancel(&cluster_gov->histtimer);
		cluster_gov->is_timer_queued = false;
		cluster_gov->is_timer_expired = false;
		cluster_gov->need_timer_requeue = false;
	}
	mutex_unlock(&cluster_dev_lock);
}

static int cluster_gov_enable(void)
{
	int ret;

	ret = tracepoint_probe_register(allow_domain_state_tp, android_vh_allow_domain_state, NULL);
	if (!ret)
		WRITE_ONCE(cluster_prediction_enabled, true);
	return ret;
}

static struct cluster_governor gov_ops = {
	.select = update_cluster_select,
	.enable = cluster_gov_enable,
	.disable = cluster_gov_disable,
	.reflect = cluster_gov_reflect,
};

static void cleanup_cluster_attrs(struct lpm_cluster *cluster_gov, int max_idx)
{
	int i;

	for (i = 0; i <= max_idx && i < cluster_gov->genpd->state_count; i++) {
		struct qcom_cluster_node *d = cluster_gov->dev_node[i];

		if (d) {
			if (d->usage_attr[i].attr.name) {
				sysfs_remove_file(d->kobj, &d->usage_attr[i].attr);
				kfree(d->usage_attr[i].attr.name);
				d->usage_attr[i].attr.name = NULL;
			}
			if (d->idle_time_attr[i].attr.name) {
				sysfs_remove_file(d->kobj, &d->idle_time_attr[i].attr);
				kfree(d->idle_time_attr[i].attr.name);
				d->idle_time_attr[i].attr.name = NULL;
			}
			if (d->rejected_attr[i].attr.name) {
				sysfs_remove_file(d->kobj, &d->rejected_attr[i].attr);
				kfree(d->rejected_attr[i].attr.name);
				d->rejected_attr[i].attr.name = NULL;
			}
		}
	}
}

static void lpm_cluster_gov_remove(struct platform_device *pdev)
{
	struct lpm_cluster *cluster_gov = platform_get_drvdata(pdev);

	mutex_lock(&cluster_dev_lock);
	WRITE_ONCE(cluster_gov->initialized, false);
	list_del_rcu(&cluster_gov->list);
	mutex_unlock(&cluster_dev_lock);
	dev_pm_genpd_remove_notifier(cluster_gov->dev);
	synchronize_rcu();
	hrtimer_cancel(&cluster_gov->histtimer);
	cleanup_cluster_attrs(cluster_gov, cluster_gov->genpd->state_count - 1);
	remove_cluster_sysfs_nodes(cluster_gov);
	if (cluster_gov->min_residency_added)
		cluster_gov->genpd->flags &= ~GENPD_FLAG_MIN_RESIDENCY;
	pm_runtime_disable(&pdev->dev);
}

static ssize_t usage_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct kobj_attribute *k_attr = (struct kobj_attribute *)attr;
	struct qcom_cluster_node *d;
	struct generic_pm_domain *genpd;
	int i;

	if (sscanf(attr->attr.name, "Usage_D%d", &i) != 1)
		return -EINVAL;

	if (i < 0 || i >= MAX_CLUSTER_STATES)
		return -EINVAL;

	d = container_of(k_attr, struct qcom_cluster_node, usage_attr[i]);
	genpd = d->cluster->genpd;

	if (i >= genpd->state_count)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%llu\n", genpd->states[i].usage);
}

static ssize_t idle_time_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct kobj_attribute *k_attr = (struct kobj_attribute *)attr;
	struct qcom_cluster_node *d;
	struct generic_pm_domain *genpd;
	int i;

	if (sscanf(attr->attr.name, "Time_spent_D%d", &i) != 1)
		return -EINVAL;

	if (i < 0 || i >= MAX_CLUSTER_STATES)
		return -EINVAL;


	d = container_of(k_attr, struct qcom_cluster_node, idle_time_attr[i]);
	genpd = d->cluster->genpd;

	if (i >= genpd->state_count)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%llu\n", ktime_to_us(genpd->states[i].idle_time));
}

static ssize_t rejected_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct kobj_attribute *k_attr = (struct kobj_attribute *)attr;
	struct qcom_cluster_node *d;
	struct generic_pm_domain *genpd;
	int i;

	if (sscanf(attr->attr.name, "Rejected_D%d", &i) != 1)
		return -EINVAL;

	if (i < 0 || i >= MAX_CLUSTER_STATES)
		return -EINVAL;

	d = container_of(k_attr, struct qcom_cluster_node, rejected_attr[i]);
	genpd = d->cluster->genpd;

	if (i >= genpd->state_count)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%llu\n", genpd->states[i].rejected);
}

static int create_cluster_stats_nodes(struct lpm_cluster *cluster_gov)
{
	int ret, i;

	for (i = 0; i < cluster_gov->genpd->state_count; i++) {
		char attr_name[32];
		struct qcom_cluster_node *d = cluster_gov->dev_node[i];


		if (!d) {
			ret = -EINVAL;
			goto cleanup_attrs;
		}
		/* Usage parameter sysfs node creation */
		scnprintf(attr_name, sizeof(attr_name), "Usage_D%d", i);
		d->usage_attr[i].attr.mode = 0444;
		sysfs_attr_init(&d->usage_attr[i].attr);
		d->usage_attr[i].attr.name = kstrdup(attr_name, GFP_KERNEL);
		if (!d->usage_attr[i].attr.name) {
			ret = -ENOMEM;
			goto cleanup_attrs;
		}
		d->usage_attr[i].show = usage_show;

		ret = sysfs_create_file(d->kobj, &d->usage_attr[i].attr);
		if (ret)
			goto cleanup_attrs;

		/* Idle time parameter sysfs node creation */
		scnprintf(attr_name, sizeof(attr_name), "Time_spent_D%d", i);
		d->idle_time_attr[i].attr.mode = 0444;
		sysfs_attr_init(&d->idle_time_attr[i].attr);
		d->idle_time_attr[i].attr.name = kstrdup(attr_name, GFP_KERNEL);
		if (!d->idle_time_attr[i].attr.name) {
			ret = -ENOMEM;
			goto cleanup_attrs;
		}
		d->idle_time_attr[i].show = idle_time_show;

		ret = sysfs_create_file(d->kobj, &d->idle_time_attr[i].attr);
		if (ret)
			goto cleanup_attrs;

		/* Rejected parameter sysfs node creation */
		scnprintf(attr_name, sizeof(attr_name), "Rejected_D%d", i);
		d->rejected_attr[i].attr.mode = 0444;
		sysfs_attr_init(&d->rejected_attr[i].attr);
		d->rejected_attr[i].attr.name = kstrdup(attr_name, GFP_KERNEL);
		if (!d->rejected_attr[i].attr.name) {
			ret = -ENOMEM;
			goto cleanup_attrs;
		}
		d->rejected_attr[i].show = rejected_show;

		ret = sysfs_create_file(d->kobj, &d->rejected_attr[i].attr);
		if (ret)
			goto cleanup_attrs;
	}

	return 0;
cleanup_attrs:
	cleanup_cluster_attrs(cluster_gov, i);
	return ret;
}

static int lpm_cluster_gov_probe(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node;
	struct lpm_cluster *cluster_gov;
	int ret, i;

	cluster_gov = devm_kzalloc(&pdev->dev, sizeof(*cluster_gov), GFP_KERNEL);
	if (!cluster_gov)
		return -ENOMEM;
	if (!pdev->dev.pm_domain)
		return -EPROBE_DEFER;
	cluster_gov->genpd = pd_to_genpd(pdev->dev.pm_domain);
	if (IS_ERR_OR_NULL(cluster_gov->genpd))
		return -ENODEV;
	if (!cluster_gov->genpd->states ||
	    !cluster_gov->genpd->state_count ||
	    cluster_gov->genpd->state_count > MAX_CLUSTER_STATES)
		return -EINVAL;

	ret = of_property_read_u32(dn, "qcom,pred-prem-cnt",
				   &cluster_gov->pred_premature_cnt);
	if (ret)
		cluster_gov->pred_premature_cnt = PRED_PREMATURE_CNT;
	if (!cluster_gov->pred_premature_cnt || cluster_gov->pred_premature_cnt > MAXSAMPLES)
		return -EINVAL;
	ret = of_property_read_u64(dn, "qcom,sample-invalid-time",
				   &cluster_gov->samples_invalid_time);
	if (ret)
		cluster_gov->samples_invalid_time = CLUST_SMPL_INVLD_TIME;
	if (cluster_gov->samples_invalid_time > S64_MAX / NSEC_PER_USEC)
		return -ERANGE;
	cluster_gov->samples_invalid_time *= NSEC_PER_USEC;
	if (!cpumask_available(cluster_gov->genpd->cpus) || cpumask_empty(cluster_gov->genpd->cpus))
		return -EINVAL;
	cluster_gov->cpu_next_wakeup = devm_alloc_percpu(&pdev->dev, ktime_t);
	if (!cluster_gov->cpu_next_wakeup)
		return -ENOMEM;
	for_each_possible_cpu(i)
		*per_cpu_ptr(cluster_gov->cpu_next_wakeup, i) = KTIME_MAX;
	cluster_gov->timer_cpu = -1;
	cluster_gov->use_bias_timer = of_property_read_bool(dn, "qcom,use-cluster-bias-timer");
	spin_lock_init(&cluster_gov->lock);
	cluster_gov->dev = &pdev->dev;
	cluster_gov->pred_wakeup = KTIME_MAX;
	cluster_gov->next_wakeup = KTIME_MAX;
	cluster_gov->entry_idx = INT_MIN;
	cluster_gov->restrict_idx = -1;
	hrtimer_setup(&cluster_gov->histtimer, clusttimer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	for (i = 0; i < cluster_gov->genpd->state_count; i++)
		cluster_gov->state_allowed[i] = true;
	cluster_gov->genpd_nb.notifier_call = cluster_power_cb;
	cluster_gov->genpd_nb.priority = INT_MAX;
	cluster_gov->min_residency_added = !(cluster_gov->genpd->flags & GENPD_FLAG_MIN_RESIDENCY);
	cluster_gov->genpd->flags |= GENPD_FLAG_MIN_RESIDENCY;
	pm_runtime_enable(&pdev->dev);
	ret = dev_pm_genpd_add_notifier(cluster_gov->dev, &cluster_gov->genpd_nb);
	if (ret)
		goto disable_runtime;
	ret = create_cluster_sysfs_nodes(cluster_gov);
	if (ret)
		goto remove_notifier;
	ret = create_cluster_stats_nodes(cluster_gov);
	if (ret) {
		remove_cluster_sysfs_nodes(cluster_gov);
		goto remove_notifier;
	}
	platform_set_drvdata(pdev, cluster_gov);
	mutex_lock(&cluster_dev_lock);
	WRITE_ONCE(cluster_gov->initialized, true);
	list_add_tail_rcu(&cluster_gov->list, &cluster_dev_list);
	mutex_unlock(&cluster_dev_lock);
	return 0;

remove_notifier:
	dev_pm_genpd_remove_notifier(cluster_gov->dev);
disable_runtime:
	if (cluster_gov->min_residency_added)
		cluster_gov->genpd->flags &= ~GENPD_FLAG_MIN_RESIDENCY;
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static const struct of_device_id qcom_cluster_lpm[] = {
	{ .compatible = "qcom,lpm-cluster-dev" },
	{ }
};

static struct platform_driver qcom_cluster_lpm_driver = {
	.probe = lpm_cluster_gov_probe,
	.remove = lpm_cluster_gov_remove,
	.driver = {
		.name = "qcom-cluster-lpm-gov",
		.of_match_table = qcom_cluster_lpm,
		.suppress_bind_attrs = true,
	},
};

void qcom_cluster_lpm_governor_deinit(void)
{
	 platform_driver_unregister(&qcom_cluster_lpm_driver);
}

static void find_allow_domain_state(struct tracepoint *tp, void *unused)
{
	if (!strcmp(tp->name, "android_vh_allow_domain_state"))
		allow_domain_state_tp = tp;
}

int qcom_cluster_lpm_governor_init(void)
{
	/* Use the kernel's exported tracepoint API, retaining the native hook. */
	check_trace_callback_type_android_vh_allow_domain_state(android_vh_allow_domain_state);
	for_each_kernel_tracepoint(find_allow_domain_state, NULL);
	if (!allow_domain_state_tp)
		return -ENOENT;
	register_cluster_governor_ops(&gov_ops);
	return platform_driver_register(&qcom_cluster_lpm_driver);
}
