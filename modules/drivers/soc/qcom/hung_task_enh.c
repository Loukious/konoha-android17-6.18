// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysctl.h>
#include <linux/atomic.h>
#include <linux/pid.h>
#include <linux/sched/walt.h>

#include <trace/hooks/hung_task.h>

#define DEFAULT_MAX_IOWAIT_TASK 5

enum {
	TASK_DEFAULT = 0,
	TASK_IN_ALLOWLIST,
	TASK_IN_BLOCKLIST,
};

enum {
	HUNG_TASK_MODE_ALLOWLIST = 0,
	HUNG_TASK_MODE_BLOCKLIST,
};

/**
 * struct hung_task_enh_data
 * @read_pid: PID of the process will be read
 * @global_detect_mode: The global detect mode switch, 0-Whitelist/1-Blacklist
 * @curr_iowait_task_cnt: Count the number of iowait processes in one scan cycle
 * @max_iowait_task_cnt: Max number of iowait processes
 * @curr_iowait_timeout_cnt: Count how many times the iowait condition is met
 * @max_iowait_timeout_cnt: Max times of the iowait condition is met
 * @ctl_table_hdr: hung_task_enh ctl table header
 */
struct hung_task_enh_data {
	int read_pid;
	int global_detect_mode;
	atomic_t curr_iowait_task_cnt;
	int max_iowait_task_cnt;
	atomic_t curr_iowait_timeout_cnt;
	int max_iowait_timeout_cnt;
	struct ctl_table_header *ctl_table_hdr;
};

static struct hung_task_enh_data hung_task_enh;

/* A disabled panic threshold must not allow signed scan-count overflow. */
static int hung_task_saturating_inc(atomic_t *counter)
{
	int old = atomic_read(counter);

	while (old < INT_MAX) {
		int seen = atomic_cmpxchg(counter, old, old + 1);

		if (seen == old)
			return old + 1;
		old = seen;
	}
	return INT_MAX;
}

static void qcom_before_check_tasks(void *ignore, struct task_struct *t, unsigned long timeout,
							bool *need_check)
{
	struct walt_task_struct *wts = task_walt(t);

	if ((READ_ONCE(hung_task_enh.global_detect_mode) == HUNG_TASK_MODE_ALLOWLIST &&
			READ_ONCE(wts->hung_detect_status) != TASK_IN_ALLOWLIST) ||
			(READ_ONCE(hung_task_enh.global_detect_mode) == HUNG_TASK_MODE_BLOCKLIST &&
			READ_ONCE(wts->hung_detect_status) == TASK_IN_BLOCKLIST)) {
		*need_check = false;
		return;
	}

	*need_check = true;

	if (unlikely(t->in_iowait) && (t->__state == TASK_UNINTERRUPTIBLE ||
			t->__state == TASK_STOPPED || t->__state == TASK_TRACED) &&
			t->last_switch_time != 0 &&
			time_is_before_jiffies(t->last_switch_time + timeout * HZ) &&
			(t->mm != NULL && t == t->group_leader))
		hung_task_saturating_inc(&hung_task_enh.curr_iowait_task_cnt);
}

static void qcom_check_tasks_done(void *ignore, void *extra)
{
	int count = atomic_xchg(&hung_task_enh.curr_iowait_task_cnt, 0);
	int limit = READ_ONCE(hung_task_enh.max_iowait_timeout_cnt);
	int consecutive;

	if (count >= READ_ONCE(hung_task_enh.max_iowait_task_cnt))
		consecutive = hung_task_saturating_inc(&hung_task_enh.curr_iowait_timeout_cnt);
	else {
		atomic_set(&hung_task_enh.curr_iowait_timeout_cnt, 0);
		consecutive = 0;
	}
	if (limit && consecutive >= limit)
		panic("Detect IO wait too long time for multiple tasks!\n");
}

static DEFINE_MUTEX(readpid_mutex);
static int read_pid_handler(const struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&readpid_mutex);
	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	mutex_unlock(&readpid_mutex);

	return ret;
}

static int hung_task_handler(const struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	struct task_struct *task;
	struct pid *pid;
	struct walt_task_struct *wts;
	int pid_and_val[2] = {-1, -1};

	struct ctl_table tmp = {
		.data	= &pid_and_val,
		.maxlen	= sizeof(pid_and_val),
		.mode	= table->mode,
	};

	mutex_lock(&readpid_mutex);

	if (!write) {
		pid = find_get_pid(hung_task_enh.read_pid);
		task = get_pid_task(pid, PIDTYPE_PID);
		put_pid(pid);
		if (!task) {
			ret = -ENOENT;
			goto unlock_mutex;
		}
		wts = task_walt(task);
		pid_and_val[0] = hung_task_enh.read_pid;
		pid_and_val[1] = READ_ONCE(wts->hung_detect_status);
		ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
		goto put_task;
	}

	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (ret)
		goto unlock_mutex;

	if (pid_and_val[0] <= 0 || pid_and_val[1] < 0 || pid_and_val[1] > 1) {
		ret = -ENOENT;
		goto unlock_mutex;
	}

	pid = find_get_pid(pid_and_val[0]);
	task = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);
	if (!task) {
		ret = -ENOENT;
		goto unlock_mutex;
	}

	wts = task_walt(task);
	if (pid_and_val[1] == 1) {
		if (READ_ONCE(hung_task_enh.global_detect_mode) == HUNG_TASK_MODE_ALLOWLIST)
			WRITE_ONCE(wts->hung_detect_status, TASK_IN_ALLOWLIST);
		else
			WRITE_ONCE(wts->hung_detect_status, TASK_IN_BLOCKLIST);
	} else {
		WRITE_ONCE(wts->hung_detect_status, TASK_DEFAULT);
	}

put_task:
	put_task_struct(task);
unlock_mutex:
	mutex_unlock(&readpid_mutex);

	return ret;
}

static const struct ctl_table hung_task_table[] = {
	{
		.procname	= "global_detect_mode",
		.data		= &hung_task_enh.global_detect_mode,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "per_task_detect_mode",
		.data		= (int *) 0,
		.maxlen		= sizeof(unsigned int) * 2,
		.mode		= 0644,
		.proc_handler	= hung_task_handler,
	},
	{
		.procname	= "max_iowait_timeout_cnt",
		.data		= &hung_task_enh.max_iowait_timeout_cnt,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_INT_MAX,
	},
	{
		.procname	= "max_iowait_task_cnt",
		.data		= &hung_task_enh.max_iowait_task_cnt,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ONE,
		.extra2		= SYSCTL_INT_MAX,
	},
	{
		.procname	= "read_pid",
		.data		= &hung_task_enh.read_pid,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= read_pid_handler,
		.extra1		= SYSCTL_ONE,
		.extra2		= SYSCTL_INT_MAX,
	},
};

static int __init hung_task_enh_init(void)
{
	int ret;

	/* The native WALT provider owns and validates the GKI task extension. */
	if (!walt_task_data_available())
		return -ENODEV;
	atomic_set(&hung_task_enh.curr_iowait_task_cnt, 0);
	atomic_set(&hung_task_enh.curr_iowait_timeout_cnt, 0);
	hung_task_enh.max_iowait_task_cnt = DEFAULT_MAX_IOWAIT_TASK;

	ret = register_trace_android_vh_check_uninterruptible_tasks(
						qcom_before_check_tasks, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_vh_check_uninterruptible_tasks_dn(
						qcom_check_tasks_done, NULL);
	if (ret) {
		unregister_trace_android_vh_check_uninterruptible_tasks(
						qcom_before_check_tasks, NULL);
		tracepoint_synchronize_unregister();
		return ret;
	}

	hung_task_enh.ctl_table_hdr = register_sysctl("hung_task_enh",
						hung_task_table);
	if (!hung_task_enh.ctl_table_hdr) {
		unregister_trace_android_vh_check_uninterruptible_tasks(
						qcom_before_check_tasks, NULL);
		unregister_trace_android_vh_check_uninterruptible_tasks_dn(
						qcom_check_tasks_done, NULL);
		tracepoint_synchronize_unregister();
		return -ENOMEM;
	}

	return ret;
}
late_initcall(hung_task_enh_init);

static void __exit hung_task_enh_exit(void)
{
	unregister_sysctl_table(hung_task_enh.ctl_table_hdr);
	unregister_trace_android_vh_check_uninterruptible_tasks(
						qcom_before_check_tasks, NULL);
	unregister_trace_android_vh_check_uninterruptible_tasks_dn(
						qcom_check_tasks_done, NULL);
	tracepoint_synchronize_unregister();
}
module_exit(hung_task_enh_exit);

MODULE_DESCRIPTION("QCOM Hung Task Enhancement");
MODULE_LICENSE("GPL");
