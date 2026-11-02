// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/moduleparam.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <trace/events/power.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/sched/walt.h>
#include <soc/qcom/msm_performance.h>
#include <soc/qcom/pmu_lib.h>
#include <linux/spinlock.h>
#include <linux/circ_buf.h>
#include <linux/ktime.h>
#include <linux/perf_event.h>
#include <linux/errno.h>
#include <linux/topology.h>

#include <linux/scmi_protocol.h>
#include <trace/events/power.h>

#define POLL_INT 25
#define NODE_NAME_MAX_CHARS 16

#define QUEUE_POOL_SIZE 512 /*2^8 always keep in 2^x */
#define INST_EV 0x08 /* 0th event*/
#define CYC_EV 0x11 /* 1st event*/
#define INIT "Init"
#define CPU_CYCLE_THRESHOLD 650000

static DEFINE_PER_CPU(bool, cpu_is_hp);
static DEFINE_MUTEX(perfevent_lock);
static DEFINE_MUTEX(freq_pmqos_lock);
static DEFINE_MUTEX(gfx_read_lock);
static DEFINE_MUTEX(core_ctl_lock);
static bool perf_stopping = true;
static int perf_hp_state = -1;
static bool param_group_created, events_group_created, notify_group_created;

enum event_idx {
	INST_EVENT,
	CYC_EVENT,
	NO_OF_EVENT
};

enum cpu_clusters {
	MIN = 0,
	MID = 1,
	MAX = 2,
	CLUSTER_MAX
};

static struct kset *msm_perf_kset;
static struct kobject *param_kobj;

static ssize_t get_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t set_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count);
static ssize_t get_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t set_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count);
static ssize_t get_cpu_total_instruction(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t get_core_ctl_register(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t set_core_ctl_register(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count);
static ssize_t get_game_start_pid(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t set_game_start_pid(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count);
#ifdef CONFIG_QTI_PLH
static ssize_t get_plh_log_level(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t set_plh_log_level(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count);
static ssize_t get_splh_notif(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t set_splh_notif(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count);
#endif
static struct kobj_attribute cpu_min_freq_attr =
	__ATTR(cpu_min_freq, 0644, get_cpu_min_freq, set_cpu_min_freq);
static struct kobj_attribute cpu_max_freq_attr =
	__ATTR(cpu_max_freq, 0644, get_cpu_max_freq, set_cpu_max_freq);
static struct kobj_attribute inst_attr =
	__ATTR(inst, 0444, get_cpu_total_instruction, NULL);
static struct kobj_attribute core_ctl_register_attr =
	__ATTR(core_ctl_register, 0644, get_core_ctl_register,
	set_core_ctl_register);
static struct kobj_attribute evnt_gplaf_pid_attr =
	__ATTR(evnt_gplaf_pid, 0644, get_game_start_pid, set_game_start_pid);
#ifdef CONFIG_QTI_PLH
static struct kobj_attribute plh_log_level_attr =
	__ATTR(plh_log_level, 0644, get_plh_log_level, set_plh_log_level);
static struct kobj_attribute splh_notify_attr =
	__ATTR(splh_notify, 0644, get_splh_notif, set_splh_notif);
#endif

static struct attribute *param_attrs[] = {
	&cpu_min_freq_attr.attr,
	&cpu_max_freq_attr.attr,
	&inst_attr.attr,
	&core_ctl_register_attr.attr,
	&evnt_gplaf_pid_attr.attr,
#ifdef CONFIG_QTI_PLH
	&plh_log_level_attr.attr,
	&splh_notify_attr.attr,
#endif
	NULL,
};

static struct attribute_group param_attr_group = {
	.attrs = param_attrs,
};

static int add_module_params(void)
{
	int ret;
	struct kobject *module_kobj;

	module_kobj = &msm_perf_kset->kobj;

	param_kobj = kobject_create_and_add("parameters", module_kobj);
	if (!param_kobj) {
		pr_err("msm_perf: Failed to add param_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(param_kobj, &param_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create sysfs\n");
		return ret;
	}
	param_group_created = true;
	return 0;
}

/* To handle cpufreq min/max request */
struct cpu_status {
	struct cpufreq_policy *policy;
	unsigned int min;
	unsigned int max;
};
static DEFINE_PER_CPU(struct cpu_status, msm_perf_cpu_stats);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_min);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_max);

static cpumask_var_t limit_mask_min;
static cpumask_var_t limit_mask_max;

static DECLARE_COMPLETION(gfx_evt_arrival);

struct gpu_data {
	pid_t pid;
	int ctx_id;
	unsigned int timestamp;
	ktime_t arrive_ts;
	int evt_typ;
};

static struct gpu_data gpu_circ_buff[QUEUE_POOL_SIZE];

struct queue_indicies {
	int head;
	int tail;
};
static struct queue_indicies curr_pos;

static DEFINE_SPINLOCK(gfx_circ_buff_lock);

struct event_data {
	u32 event_id;
	u64 prev_count;
	u64 cur_delta;
	u64 cached_total_count;
};
static struct event_data **pmu_events;
static unsigned long min_cpu_capacity = ULONG_MAX;

struct events {
	spinlock_t cpu_hotplug_lock;
	bool cpu_hotplug;
	bool init_success;
};
static struct events events_group;
static struct task_struct *events_notify_thread;

static unsigned int aggr_big_nr;
static unsigned int aggr_top_load;
static unsigned int top_load[CLUSTER_MAX];
static unsigned int curr_cap[CLUSTER_MAX];
static atomic_t game_status_pid;
static bool ready_for_freq_updates;

static int freq_qos_request_init(void)
{
	unsigned int cpu;
	int ret;

	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: Failed to get cpufreq policy for cpu%d\n",
				__func__, cpu);
			ret = -EAGAIN;
			goto cleanup;
		}
		per_cpu(msm_perf_cpu_stats, cpu).min = 0;
		req = &per_cpu(qos_req_min, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add min freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		per_cpu(msm_perf_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
		req = &per_cpu(qos_req_max, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add max freq constraint (%d)\n",
				__func__, ret);
			freq_qos_remove_request(&per_cpu(qos_req_min, cpu));
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		per_cpu(msm_perf_cpu_stats, cpu).policy = policy;
	}
	return 0;

cleanup:
	for_each_present_cpu(cpu) {
		req = &per_cpu(qos_req_min, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);


		req = &per_cpu(qos_req_max, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		if (per_cpu(msm_perf_cpu_stats, cpu).policy) {
			cpufreq_cpu_put(per_cpu(msm_perf_cpu_stats, cpu).policy);
			per_cpu(msm_perf_cpu_stats, cpu).policy = NULL;
		}
		per_cpu(msm_perf_cpu_stats, cpu).min = 0;
		per_cpu(msm_perf_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	return ret;
}

static int msm_perf_parse_freq(const char *buf, size_t count, cpumask_t *mask,
			       unsigned int *values, bool maximum)
{
	char *copy, *cursor, *token, *value;
	unsigned int cpu, frequency;
	int ret = -EINVAL;

	copy = kmemdup_nul(buf, count, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	cursor = copy;
	cpumask_clear(mask);
	while ((token = strsep(&cursor, " \t\n"))) {
		if (!*token)
			continue;
		value = strchr(token, ':');
		if (!value)
			goto out;
		*value++ = '\0';
		if (kstrtouint(token, 10, &cpu) || kstrtouint(value, 10, &frequency) ||
		    cpu >= nr_cpu_ids || !cpu_present(cpu))
			goto out;
		if (maximum)
			frequency = min_t(unsigned int, frequency, FREQ_QOS_MAX_DEFAULT_VALUE);
		else if (frequency > INT_MAX)
			goto out;
		values[cpu] = frequency;
		cpumask_set_cpu(cpu, mask);
	}
	if (!cpumask_empty(mask))
		ret = 0;
out:
	kfree(copy);
	return ret;
}

static ssize_t msm_perf_set_freq(const char *buf, size_t count, bool maximum)
{
	unsigned int *values, cpu;
	struct cpu_status *stats;
	struct freq_qos_request *req;
	cpumask_t *mask = maximum ? limit_mask_max : limit_mask_min;
	int ret;

	values = kcalloc(nr_cpu_ids, sizeof(*values), GFP_KERNEL);
	if (!values)
		return -ENOMEM;
	mutex_lock(&freq_pmqos_lock);
	cpus_read_lock();
	if (READ_ONCE(perf_stopping)) {
		ret = -ENODEV;
		goto out;
	}
	ret = msm_perf_parse_freq(buf, count, mask, values, maximum);
	if (ret)
		goto out;
	if (!ready_for_freq_updates) {
		ret = freq_qos_request_init();
		if (ret)
			goto out;
		ready_for_freq_updates = true;
	}
	for_each_cpu(cpu, mask) {
		stats = &per_cpu(msm_perf_cpu_stats, cpu);
		req = maximum ? &per_cpu(qos_req_max, cpu) : &per_cpu(qos_req_min, cpu);
		ret = freq_qos_update_request(req, values[cpu]);
		if (ret < 0)
			goto out;
		if (maximum)
			stats->max = values[cpu];
		else
			stats->min = values[cpu];
	}
	ret = 0;
out:
	cpus_read_unlock();
	mutex_unlock(&freq_pmqos_lock);
	kfree(values);
	return ret < 0 ? ret : count;
}

/*******************************sysfs start************************************/
static ssize_t set_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	return msm_perf_set_freq(buf, count, false);
}

static ssize_t get_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int cnt = 0, cpu;

	mutex_lock(&freq_pmqos_lock);
	for_each_present_cpu(cpu) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu,
				per_cpu(msm_perf_cpu_stats, cpu).min);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	mutex_unlock(&freq_pmqos_lock);
	return cnt;
}

static ssize_t set_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	return msm_perf_set_freq(buf, count, true);
}

static ssize_t get_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int cnt = 0, cpu;

	mutex_lock(&freq_pmqos_lock);
	for_each_present_cpu(cpu) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu,
				per_cpu(msm_perf_cpu_stats, cpu).max);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	mutex_unlock(&freq_pmqos_lock);
	return cnt;
}

static struct kobject *events_kobj;

static ssize_t show_cpu_hotplug(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "\n");
}
static struct kobj_attribute cpu_hotplug_attr =
__ATTR(cpu_hotplug, 0444, show_cpu_hotplug, NULL);

static struct attribute *events_attrs[] = {
	&cpu_hotplug_attr.attr,
	NULL,
};

static struct attribute_group events_attr_group = {
	.attrs = events_attrs,
};

static ssize_t show_perf_gfx_evts(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	/* Every formatted record is below 96 bytes, including integer maxima. */
	struct gpu_data batch[PAGE_SIZE / 96];
	unsigned long flags;
	unsigned int nr = 0, i;
	bool more;
	ssize_t ret = 0;

	mutex_lock(&gfx_read_lock);
	for (;;) {
		if (READ_ONCE(perf_stopping)) {
			ret = -ENODEV;
			goto out;
		}
		ret = wait_for_completion_interruptible(&gfx_evt_arrival);
		if (ret)
			goto out;
		spin_lock_irqsave(&gfx_circ_buff_lock, flags);
		while (curr_pos.tail != curr_pos.head && nr < ARRAY_SIZE(batch)) {
			batch[nr++] = gpu_circ_buff[curr_pos.tail];
			curr_pos.tail = (curr_pos.tail + 1) % QUEUE_POOL_SIZE;
		}
		more = curr_pos.tail != curr_pos.head;
		spin_unlock_irqrestore(&gfx_circ_buff_lock, flags);
		if (nr)
			break;
	}
	if (more)
		complete(&gfx_evt_arrival);
	ret = 0;
	for (i = 0; i < nr; i++)
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%d %d %u %d %lld :",
			batch[i].pid, batch[i].ctx_id, batch[i].timestamp, batch[i].evt_typ,
			ktime_to_us(batch[i].arrive_ts));
out:
	mutex_unlock(&gfx_read_lock);
	return ret;
}

static struct kobj_attribute gfx_event_info_attr =
__ATTR(gfx_evt, 0444, show_perf_gfx_evts, NULL);

static ssize_t show_big_nr(struct kobject *kobj,
			   struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(aggr_big_nr));
}

static struct kobj_attribute big_nr_attr =
__ATTR(aggr_big_nr, 0444, show_big_nr, NULL);

static ssize_t show_top_load(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(aggr_top_load));
}

static struct kobj_attribute top_load_attr =
__ATTR(aggr_top_load, 0444, show_top_load, NULL);


static ssize_t show_top_load_cluster(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u %u %u\n",
					top_load[MIN], top_load[MID],
					top_load[MAX]);
}

static struct kobj_attribute cluster_top_load_attr =
__ATTR(top_load_cluster, 0444, show_top_load_cluster, NULL);

static ssize_t show_curr_cap_cluster(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u %u %u\n",
					curr_cap[MIN], curr_cap[MID],
					curr_cap[MAX]);
}

static struct kobj_attribute cluster_curr_cap_attr =
__ATTR(curr_cap_cluster, 0444, show_curr_cap_cluster, NULL);

static struct attribute *notify_attrs[] = {
	&big_nr_attr.attr,
	&top_load_attr.attr,
	&cluster_top_load_attr.attr,
	&cluster_curr_cap_attr.attr,
	&gfx_event_info_attr.attr,
	NULL,
};

static struct attribute_group notify_attr_group = {
	.attrs = notify_attrs,
};
static struct kobject *notify_kobj;

/*******************************sysfs ends************************************/

/*****************PMU Data Collection*****************/


static void free_pmu_counters(unsigned int cpu)
{
	int i = 0;

	if (!cpu_possible(cpu))
		return;

	for (i = 0; i < NO_OF_EVENT; i++) {
		pmu_events[i][cpu].prev_count = 0;
		pmu_events[i][cpu].cur_delta = 0;
		pmu_events[i][cpu].cached_total_count = 0;
	}
}

static void release_pmu_counters(void)
{
	int i;

	if (!pmu_events)
		return;
	for (i = 0; i < NO_OF_EVENT; i++)
		kfree(pmu_events[i]);
	kfree(pmu_events);
	pmu_events = NULL;
}

static int init_pmu_counter(void)
{
	unsigned int cpu;
	int i;

	pmu_events = kcalloc(NO_OF_EVENT, sizeof(*pmu_events), GFP_KERNEL);
	if (!pmu_events)
		return -ENOMEM;
	for (i = 0; i < NO_OF_EVENT; i++) {
		pmu_events[i] = kcalloc(nr_cpu_ids, sizeof(**pmu_events), GFP_KERNEL);
		if (!pmu_events[i]) {
			release_pmu_counters();
			return -ENOMEM;
		}
	}
	for_each_possible_cpu(cpu) {
		pmu_events[INST_EVENT][cpu].event_id = INST_EV;
		pmu_events[CYC_EVENT][cpu].event_id = CYC_EV;
		min_cpu_capacity = min(min_cpu_capacity, arch_scale_cpu_capacity(cpu));
	}
	return 0;
}

/* Caller serializes a complete sample against readers and CPU hotplug. */
static int msm_perf_read_event(struct event_data *event, int cpu)
{
	u64 total;
	int ret;

	if (!per_cpu(cpu_is_hp, cpu)) {
		ret = qcom_pmu_read(cpu, event->event_id, &total);
		if (ret)
			return ret;
	} else {
		total = event->cached_total_count;
	}
	event->cur_delta = total >= event->prev_count ? total - event->prev_count : total;
	event->prev_count = total;
	return 0;
}

static ssize_t get_cpu_total_instruction(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	u64 instruction = 0;
	u64 cycles = 0;
	u64 total_inst_big = 0;
	u64 total_inst_little = 0;
	u64 ipc_big = 0;
	u64 ipc_little = 0;
	int cnt = 0, cpu, ret;

	cpus_read_lock();
	mutex_lock(&perfevent_lock);
	for_each_possible_cpu(cpu) {
		/* Read Instruction event */
		ret = msm_perf_read_event(&pmu_events[INST_EVENT][cpu], cpu);
		if (ret)
			goto error;
		/* Read Cycle event */
		ret = msm_perf_read_event(&pmu_events[CYC_EVENT][cpu], cpu);
		if (ret)
			goto error;
		instruction = pmu_events[INST_EVENT][cpu].cur_delta;
		cycles = pmu_events[CYC_EVENT][cpu].cur_delta;
		/* collecting max inst and ipc for max cap and min cap cpus */
		if (arch_scale_cpu_capacity(cpu) > min_cpu_capacity) {
			if (cycles && cycles >= CPU_CYCLE_THRESHOLD)
				ipc_big = max(ipc_big,
						mul_u64_u64_div_u64(instruction, 100, cycles));
			total_inst_big += instruction;
		} else {
			if (cycles)
				ipc_little = max(ipc_little,
						mul_u64_u64_div_u64(instruction, 100, cycles));
			total_inst_little += instruction;
		}
	}

	cnt += scnprintf(buf, PAGE_SIZE, "%llu:%llu:%llu:%llu\n",
			total_inst_big, ipc_big,
			total_inst_little, ipc_little);

	mutex_unlock(&perfevent_lock);
	cpus_read_unlock();
	return cnt;
error:
	mutex_unlock(&perfevent_lock);
	cpus_read_unlock();
	return ret;
}

static int hotplug_notify_down(unsigned int cpu)
{
	mutex_lock(&perfevent_lock);
	per_cpu(cpu_is_hp, cpu) = true;
	free_pmu_counters(cpu);
	mutex_unlock(&perfevent_lock);

	return 0;
}

static int hotplug_notify_up(unsigned int cpu)
{
	unsigned long flags;

	mutex_lock(&perfevent_lock);
	per_cpu(cpu_is_hp, cpu) = false;
	mutex_unlock(&perfevent_lock);

	if (events_group.init_success) {
		spin_lock_irqsave(&(events_group.cpu_hotplug_lock), flags);
		events_group.cpu_hotplug = true;
		spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock), flags);
		wake_up_process(events_notify_thread);
	}

	return 0;
}

static int events_notify_userspace(void *data)
{
	unsigned long flags;
	bool notify_change;

	while (!kthread_should_stop()) {

		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&(events_group.cpu_hotplug_lock), flags);

		if (!events_group.cpu_hotplug) {
			spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock),
									flags);

			schedule();
			if (kthread_should_stop())
				break;
			spin_lock_irqsave(&(events_group.cpu_hotplug_lock),
									flags);
		}

		set_current_state(TASK_RUNNING);
		notify_change = events_group.cpu_hotplug;
		events_group.cpu_hotplug = false;
		spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock), flags);

		if (notify_change)
			sysfs_notify(events_kobj, NULL, "cpu_hotplug");
	}

	__set_current_state(TASK_RUNNING);
	return 0;
}

static int init_notify_group(void)
{
	int ret;
	struct kobject *module_kobj = &msm_perf_kset->kobj;

	notify_kobj = kobject_create_and_add("notify", module_kobj);
	if (!notify_kobj) {
		pr_err("msm_perf: Failed to add notify_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(notify_kobj, &notify_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create sysfs\n");
		return ret;
	}
	notify_group_created = true;
	return 0;
}

static int init_events_group(void)
{
	int ret;
	struct kobject *module_kobj = &msm_perf_kset->kobj;

	events_kobj = kobject_create_and_add("events", module_kobj);
	if (!events_kobj) {
		pr_err("msm_perf: Failed to add events_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(events_kobj, &events_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create sysfs\n");
		return ret;
	}

	events_group_created = true;
	spin_lock_init(&(events_group.cpu_hotplug_lock));
	events_notify_thread = kthread_run(events_notify_userspace,
					NULL, "msm_perf:events_notify");
	if (IS_ERR(events_notify_thread)) {
		ret = PTR_ERR(events_notify_thread);
		events_notify_thread = NULL;
		return ret;
	}

	events_group.init_success = true;

	return 0;
}

static void nr_notify_userspace(struct work_struct *work)
{
	sysfs_notify(notify_kobj, NULL, "aggr_top_load");
	sysfs_notify(notify_kobj, NULL, "aggr_big_nr");
	sysfs_notify(notify_kobj, NULL, "top_load_cluster");
	sysfs_notify(notify_kobj, NULL, "curr_cap_cluster");
}

static DECLARE_WORK(sysfs_notify_work, nr_notify_userspace);

static int msm_perf_core_ctl_notify(struct notifier_block *nb,
					unsigned long unused,
					void *data)
{
	struct core_ctl_notif_data *d = data;
	int cluster = 0;

	if (!d || READ_ONCE(perf_stopping))
		return NOTIFY_DONE;

	for (cluster = 0; cluster < CLUSTER_MAX; cluster++) {
		WRITE_ONCE(top_load[cluster], d->ta_util_pct[cluster]);
		WRITE_ONCE(curr_cap[cluster], d->cur_cap_pct[cluster]);
	}
	WRITE_ONCE(aggr_top_load, d->coloc_load_pct);
	WRITE_ONCE(aggr_big_nr, d->nr_big);

	schedule_work(&sysfs_notify_work);
	return NOTIFY_OK;
}

static struct notifier_block msm_perf_nb = {
	.notifier_call = msm_perf_core_ctl_notify
};

static bool core_ctl_register;
static ssize_t get_core_ctl_register(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%c\n", READ_ONCE(core_ctl_register) ? 'Y' : 'N');
}

static ssize_t set_core_ctl_register(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	bool value;
	int ret;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;
	mutex_lock(&core_ctl_lock);
	if (READ_ONCE(perf_stopping)) {
		ret = -ENODEV;
	} else if (value != core_ctl_register) {
		if (value)
			core_ctl_notifier_register(&msm_perf_nb);
		else
			core_ctl_notifier_unregister(&msm_perf_nb);
		core_ctl_register = value;
	}
	mutex_unlock(&core_ctl_lock);
	return ret ? ret : count;
}

void msm_perf_events_update(enum evt_update_t update_typ,
			enum gfx_evt_t evt_typ, pid_t pid,
			u32 ctx_id, u32 timestamp, bool end_of_frame)
{
	unsigned long flags;
	struct gpu_data *event;
	int next;

	if (update_typ != MSM_PERF_GFX || evt_typ <= MSM_PERF_INVAL ||
	    evt_typ > MSM_PERF_RETIRED || pid != atomic_read(&game_status_pid) ||
	    !timestamp || !end_of_frame)
		return;
	spin_lock_irqsave(&gfx_circ_buff_lock, flags);
	if (perf_stopping) {
		spin_unlock_irqrestore(&gfx_circ_buff_lock, flags);
		return;
	}
	next = (curr_pos.head + 1) % QUEUE_POOL_SIZE;
	/* A full ring drops the oldest record, never a partially written one. */
	if (next == curr_pos.tail)
		curr_pos.tail = (curr_pos.tail + 1) % QUEUE_POOL_SIZE;
	event = &gpu_circ_buff[curr_pos.head];
	event->pid = pid;
	event->ctx_id = ctx_id;
	event->timestamp = timestamp;
	event->evt_typ = evt_typ;
	event->arrive_ts = ktime_get();
	curr_pos.head = next;
	spin_unlock_irqrestore(&gfx_circ_buff_lock, flags);
	if (evt_typ == MSM_PERF_QUEUE || evt_typ == MSM_PERF_RETIRED)
		complete(&gfx_evt_arrival);
}
EXPORT_SYMBOL_GPL(msm_perf_events_update);

static ssize_t set_game_start_pid(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	long usr_val = 0;
	int ret;

	ret = kstrtol(buf, 0, &usr_val);
	if (ret) {
		pr_err("msm_perf: kstrtol failed, ret=%d\n", ret);
		return ret;
	}
	if (usr_val < 0 || usr_val > INT_MAX)
		return -EINVAL;
	atomic_set(&game_status_pid, usr_val);
	return count;
}
static ssize_t get_game_start_pid(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	long usr_val  = atomic_read(&game_status_pid);

	return scnprintf(buf, PAGE_SIZE, "%ld\n", usr_val);
}

/*******************************GFX Call************************************/
#ifdef CONFIG_QTI_PLH
static struct scmi_handle *plh_handle;
void rimps_plh_init(struct scmi_handle *handle)
{
	if (handle)
		plh_handle = handle;
}
EXPORT_SYMBOL_GPL(rimps_plh_init);

static int splh_notif, splh_init_done, plh_log_level;

#define PLH_MIN_LOG_LEVEL			0
#define PLH_MAX_LOG_LEVEL			0xF
#define SPLH_FPS_MAX_CNT			8
#define SPLH_IPC_FREQ_VTBL_MAX_CNT		5 /* ipc freq pair */
#define SPLH_INIT_IPC_FREQ_TBL_PARAMS	\
			(2 + SPLH_FPS_MAX_CNT * (1 + (2 * SPLH_IPC_FREQ_VTBL_MAX_CNT)))

static ssize_t get_plh_log_level(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", plh_log_level);
}

static ssize_t set_plh_log_level(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count)
{
	int ret, log_val_backup;
	struct scmi_plh_vendor_ops *ops;

	if (!plh_handle || !plh_handle->plh_ops) {
		pr_err("msm_perf: plh scmi handle or vendor ops null\n");
		return -EINVAL;
	}

	ops = plh_handle->plh_ops;

	log_val_backup = plh_log_level;

	ret = sscanf(buf, "%du", &plh_log_level);

	if (ret < 0) {
		pr_err("msm_perf: getting new plh_log_level failed, ret=%d\n", ret);
		return ret;
	}

	plh_log_level = clamp(plh_log_level, PLH_MIN_LOG_LEVEL, PLH_MAX_LOG_LEVEL);
	ret = ops->set_plh_log_level(plh_handle, plh_log_level);
	if (ret < 0) {
		plh_log_level = log_val_backup;
		pr_err("msm_perf: setting new plh_log_level failed, ret=%d\n", ret);
		return ret;
	}
	return count;
}

static int init_splh_notif(const char *buf)
{
	int i, j, ret;
	u16 tmp[SPLH_INIT_IPC_FREQ_TBL_PARAMS];
	u16 *ptmp = tmp, ntokens, nfps, n_ipc_freq_pair, tmp_valid_len = 0;
	const char *cp, *cp1;
	struct scmi_plh_vendor_ops *ops;

	/* buf contains the init info from user */
	if (buf == NULL || !plh_handle || !plh_handle->plh_ops)
		return -EINVAL;

	cp = buf;
	ntokens = 0;
	while ((cp = strpbrk(cp + 1, ":")))
		ntokens++;

	/* format of cmd nfps, n_ipc_freq_pair, <fps0, <ipc0, freq0>,...>,... */
	cp = buf;
	if (sscanf(cp, INIT ":%hu", &nfps)) {
		if ((nfps != ntokens-1) || (nfps == 0) || (nfps > SPLH_FPS_MAX_CNT))
			return -EINVAL;

		cp = strnchr(cp, strlen(cp), ':');	/* skip INIT */
		cp++;
		cp = strnchr(cp, strlen(cp), ':');	/* skip nfps */

		*ptmp++ = nfps;		/* nfps is first cmd param */
		tmp_valid_len++;
		cp1 = cp;
		ntokens = 0;
		/* get count of nfps * n_ipc_freq_pair * <ipc freq pair values> */
		while ((cp1 = strpbrk(cp1 + 1, ",")))
			ntokens++;

		if (ntokens % (2 * nfps)) /* ipc freq pair values should be multiple of nfps */
			return -EINVAL;

		n_ipc_freq_pair = ntokens / (2 * nfps); /* ipc_freq pair values for each FPS */
		if ((n_ipc_freq_pair == 0) || (n_ipc_freq_pair > SPLH_IPC_FREQ_VTBL_MAX_CNT))
			return -EINVAL;

		*ptmp++ = n_ipc_freq_pair; /* n_ipc_freq_pair is second cmd param */
		tmp_valid_len++;
		cp1 = cp;
		for (i = 0; i < nfps; i++) {
			if (sscanf(cp1, ":%hu", ptmp) != 1)
				return -EINVAL;

			ptmp++;		/* increment after storing FPS val */
			tmp_valid_len++;
			cp1 = strnchr(cp1, strlen(cp1), ','); /* move to ,ipc */
			for (j = 0; j < 2 * n_ipc_freq_pair; j++) {
				if (sscanf(cp1, ",%hu", ptmp) != 1)
					return -EINVAL;

				ptmp++;	/* increment after storing ipc or freq */
				tmp_valid_len++;
				cp1++;
				if (j != (2 * n_ipc_freq_pair - 1))
					cp1 = strnchr(cp1, strlen(cp1), ','); /* move to next */
			}

			if (i != (nfps - 1))
				cp1 = strnchr(cp1, strlen(cp1), ':'); /* move to next FPS val */

		}
	} else {
		return -EINVAL;
	}

	ops = plh_handle->plh_ops;
	ret = ops->init_splh_ipc_freq_tbl(plh_handle, tmp, tmp_valid_len);
	if (ret < 0)
		return -EINVAL;

	pr_info("msm_perf: nfps=%u n_ipc_freq_pair=%u last_freq_val=%u len=%u\n",
		nfps, n_ipc_freq_pair, *--ptmp, tmp_valid_len);
	splh_init_done = 1;
	return 0;
}

static void activate_splh_notif(void)
{
	int ret;
	struct scmi_plh_vendor_ops *ops;
	/* received event notification here */
	if (!plh_handle || !plh_handle->plh_ops) {
		pr_err("msm_perf: splh not supported\n");
		return;
	}
	ops = plh_handle->plh_ops;

	if (splh_notif)
		ret = ops->start_splh(plh_handle, splh_notif); /* splh_notif is fps */
	else
		ret = ops->stop_splh(plh_handle);

	if (ret < 0) {
		pr_err("msm_perf: splh start or stop failed, ret=%d\n", ret);
		return;
	}
}

static ssize_t get_splh_notif(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", splh_notif);
}

static ssize_t set_splh_notif(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf,
	size_t count)
{
	int ret;

	if (strnstr(buf, INIT, sizeof(INIT)) != NULL) {
		splh_init_done = 0;
		ret = init_splh_notif(buf);
		if (ret < 0)
			pr_err("msm_perf: splh ipc freq tbl init failed, ret=%d\n", ret);

		return ret;
	}

	if (!splh_init_done) {
		pr_err("msm_perf: splh ipc freq tbl not initialized\n");
		return -EINVAL;
	}

	ret = sscanf(buf, "%du", &splh_notif);
	if (ret < 0)
		return ret;

	activate_splh_notif();

	return count;
}
#endif /* CONFIG_QTI_PLH */

static void msm_performance_cleanup(void)
{
	unsigned long flags;
	unsigned int cpu;

	spin_lock_irqsave(&gfx_circ_buff_lock, flags);
	WRITE_ONCE(perf_stopping, true);
	spin_unlock_irqrestore(&gfx_circ_buff_lock, flags);
	complete_all(&gfx_evt_arrival);
	mutex_lock(&core_ctl_lock);
	if (core_ctl_register) {
		core_ctl_notifier_unregister(&msm_perf_nb);
		core_ctl_register = false;
	}
	mutex_unlock(&core_ctl_lock);
	if (perf_hp_state >= 0) {
		cpuhp_remove_state_nocalls(perf_hp_state);
		perf_hp_state = -1;
	}
	if (events_notify_thread) {
		kthread_stop(events_notify_thread);
		events_notify_thread = NULL;
	}
	events_group.init_success = false;
	cancel_work_sync(&sysfs_notify_work);
	if (param_kobj) {
		if (param_group_created)
			sysfs_remove_group(param_kobj, &param_attr_group);
		param_group_created = false;
		kobject_put(param_kobj);
		param_kobj = NULL;
	}
	if (events_kobj) {
		if (events_group_created)
			sysfs_remove_group(events_kobj, &events_attr_group);
		events_group_created = false;
		kobject_put(events_kobj);
		events_kobj = NULL;
	}
	if (notify_kobj) {
		if (notify_group_created)
			sysfs_remove_group(notify_kobj, &notify_attr_group);
		notify_group_created = false;
		kobject_put(notify_kobj);
		notify_kobj = NULL;
	}
	for_each_possible_cpu(cpu) {
		if (freq_qos_request_active(&per_cpu(qos_req_min, cpu)))
			freq_qos_remove_request(&per_cpu(qos_req_min, cpu));
		if (freq_qos_request_active(&per_cpu(qos_req_max, cpu)))
			freq_qos_remove_request(&per_cpu(qos_req_max, cpu));
		if (per_cpu(msm_perf_cpu_stats, cpu).policy) {
			cpufreq_cpu_put(per_cpu(msm_perf_cpu_stats, cpu).policy);
			per_cpu(msm_perf_cpu_stats, cpu).policy = NULL;
		}
	}
	release_pmu_counters();
	if (msm_perf_kset) {
		kset_unregister(msm_perf_kset);
		msm_perf_kset = NULL;
	}
	free_cpumask_var(limit_mask_min);
	free_cpumask_var(limit_mask_max);
}

static int __init msm_performance_init(void)
{
	unsigned int cpu;
	int ret;

	if (!alloc_cpumask_var(&limit_mask_min, GFP_KERNEL))
		return -ENOMEM;
	if (!alloc_cpumask_var(&limit_mask_max, GFP_KERNEL)) {
		free_cpumask_var(limit_mask_min);
		return -ENOMEM;
	}
	for_each_possible_cpu(cpu) {
		per_cpu(msm_perf_cpu_stats, cpu).min = 0;
		per_cpu(msm_perf_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	ret = init_pmu_counter();
	if (ret)
		goto error;
	msm_perf_kset = kset_create_and_add("msm_performance", NULL, kernel_kobj);
	if (!msm_perf_kset) {
		ret = -ENOMEM;
		goto error;
	}
	ret = add_module_params();
	if (ret)
		goto error;
	ret = init_events_group();
	if (ret)
		goto error;
	ret = init_notify_group();
	if (ret)
		goto error;
	cpus_read_lock();
	for_each_possible_cpu(cpu)
		per_cpu(cpu_is_hp, cpu) = !cpu_online(cpu);
	ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN,
			"msm_performance_cpu_hotplug", hotplug_notify_up, hotplug_notify_down);
	cpus_read_unlock();
	if (ret < 0)
		goto error;
	perf_hp_state = ret;
	WRITE_ONCE(perf_stopping, false);
	return 0;
error:
	msm_performance_cleanup();
	return ret;
}

static void __exit msm_performance_exit(void)
{
	msm_performance_cleanup();
}
MODULE_LICENSE("GPL");
late_initcall(msm_performance_init);
module_exit(msm_performance_exit);
MODULE_DESCRIPTION("Onyx performance controls and GPU events");
