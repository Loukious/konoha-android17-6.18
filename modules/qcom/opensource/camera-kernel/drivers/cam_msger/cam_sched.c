// MIUI ADD: Camera_CameraOpt
#include "cam_sched.h"
#include <trace/hooks/sched.h>
#include <linux/string_helpers.h>
#include <linux/sched/signal.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include "cam_msger_common.h"

#define WRITE_ERR_USR_BUFF_NULL -2000
#define WRITE_ERR_CONT_2_SHORT -2001
#define WRITE_ERR_FORMAT_INVALID -2002
#define WRITE_ERR_CPUMASK_INVALID -2003
#define WRITE_ERR_CSCHED_MALLOC_FAILED -2004
#define WRITE_ERR_KERL_ALLOC_FAILE -2005

/* Published configurations are immutable; the messenger serializes writers. */
static struct cam_sched_data __rcu *csdata;
static unsigned int registered_hooks;
static bool sched_ready;

static void sched_dump(void)
{
	struct cam_sched_data *data;
	int i;

	rcu_read_lock();
	data = rcu_dereference(csdata);
	if (!data)
		goto out;
	pr_info("camera scheduler mask: %*pbl\n", cpumask_pr_args(&data->ctl_cpus));
	for (i = 0; i < data->offset; i++)
		pr_info("camera scheduler pid: %d\n", data->pids[i]);
out:
	rcu_read_unlock();
}

void cam_sched_ioctl(struct file *filp, unsigned int cmd, unsigned long arg, u8 *hit)
{
	if (cmd == IOC_CMD_SCHED_DUMP) {
		sched_dump();
		*hit = 1;
	}
}

int cam_sched_w_pids(struct file *fp, char *buff, int length, u8 *hit)
{
	struct cam_sched_data *data, *old;
	char *rest = buff, *token;
	pid_t pids[MONITOR_MAX_PID] = { 0 };
	unsigned int mask;
	int flag, count = 0, pid, cpu, i, j;

	*hit = 0;
	if (!buff)
		return WRITE_ERR_USR_BUFF_NULL;
	if (!(READ_ONCE(msger_siwtches) & SCD_ENABLE) || strncmp(buff, "ctl|", 4))
		return -1;
	*hit = 1;
	if (length <= WRITE_CONTENT_MIN_LEN || length >= WRITE_CONTENT_MAX_LEN)
		return WRITE_ERR_FORMAT_INVALID;
	strsep(&rest, SPLIT_DELIM);
	token = strsep(&rest, SPLIT_DELIM);
	if (!token || kstrtouint(token, 10, &mask) || mask >= BIT(CPU_NR))
		return WRITE_ERR_CPUMASK_INVALID;
	flag = TISOT_DEFAULT;
	if (mask) {
		token = strsep(&rest, SPLIT_DELIM);
		if (token && sysfs_streq(token, TISOT_ALLOW_STR))
			flag = TISOT_ALLOW;
		else if (token && sysfs_streq(token, TISOT_NRALLOW_STR))
			flag = TISOT_NEVERALLOW;
		else
			return WRITE_ERR_FORMAT_INVALID;
		while ((token = strsep(&rest, SPLIT_DELIM))) {
			if (!*token)
				continue;
			if (count == MONITOR_MAX_PID)
				return WRITE_ERR_FORMAT_INVALID;
			pid = find_pid_cmdline(token);
			if (pid > 0)
				pids[count++] = pid;
		}
	}
	for (i = 0; i < count; i++)
		for (j = i + 1; j < count; j++)
			if (pids[i] < pids[j])
				swap(pids[i], pids[j]);
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return WRITE_ERR_CSCHED_MALLOC_FAILED;
	for (cpu = 0; cpu < CPU_NR; cpu++) {
		if (!(mask & BIT(cpu)))
			continue;
		if (cpu >= nr_cpu_ids || !cpu_possible(cpu)) {
			kfree(data);
			return WRITE_ERR_CPUMASK_INVALID;
		}
		cpumask_set_cpu(cpu, &data->ctl_cpus);
		data->flag[cpu] = flag;
	}
	memcpy(data->pids, pids, sizeof(pids));
	data->offset = count;
	old = rcu_replace_pointer(csdata, data, true);
	synchronize_rcu();
	kfree(old);
	return length;
}

/* Never turn an existing core-scheduler rejection into an allowance. */
static bool effect_cpu_action(const struct cam_sched_data *data,
			      struct task_struct *p, int cpu)
{
	cpumask_t fallback;
	bool listed = false;
	int i;

	if (!data || !p || !p->mm || cpu < 0 || cpu >= CPU_NR || cpu >= nr_cpu_ids ||
	    !cpu_active(cpu) || p->nr_cpus_allowed <= 2 || !data->flag[cpu])
		return false;
	/* Keep a runnable CPU when the task affinity is entirely controlled. */
	cpumask_andnot(&fallback, p->cpus_ptr, &data->ctl_cpus);
	cpumask_and(&fallback, &fallback, cpu_active_mask);
	if (cpumask_empty(&fallback))
		return false;
	if (!data->offset)
		return true;
	for (i = 0; i < data->offset; i++)
		if (data->pids[i] == p->tgid)
			listed = true;
	return data->flag[cpu] == TISOT_ALLOW ? !listed : listed;
}

static void filter_cpu(const struct cam_sched_data *data, struct task_struct *p,
		       int prev_cpu, int *cpu)
{
	cpumask_t allowed;

	cpumask_andnot(&allowed, p->cpus_ptr, &data->ctl_cpus);
	cpumask_and(&allowed, &allowed, cpu_active_mask);
	if (cpumask_empty(&allowed))
		return;
	if (prev_cpu >= 0 && prev_cpu < nr_cpu_ids && cpumask_test_cpu(prev_cpu, &allowed))
		*cpu = prev_cpu;
	else
		*cpu = cpumask_first(&allowed);
}

static void xmi_rvh_is_cpu_allowed(void *unused, struct task_struct *p, int cpu,
				   bool *allowed)
{
	if (!READ_ONCE(sched_ready) || !(READ_ONCE(msger_siwtches) & SCD_ENABLE))
		return;
	rcu_read_lock();
	if (effect_cpu_action(rcu_dereference(csdata), p, cpu))
		*allowed = false;
	rcu_read_unlock();
}

static void xmi_select_task_rq_fair(void *unused, struct task_struct *p,
				    int prev_cpu, int sd_flag, int wake_flags, int *new_cpu)
{
	struct cam_sched_data *data;

	if (!READ_ONCE(sched_ready) || !(READ_ONCE(msger_siwtches) & SCD_ENABLE))
		return;
	rcu_read_lock();
	data = rcu_dereference(csdata);
	if (effect_cpu_action(data, p, *new_cpu))
		filter_cpu(data, p, prev_cpu, new_cpu);
	rcu_read_unlock();
}

static void xmi_can_migrate_task(void *unused, struct task_struct *p, int dst_cpu,
				 int *can_migrate)
{
	if (!READ_ONCE(sched_ready) || !(READ_ONCE(msger_siwtches) & SCD_ENABLE))
		return;
	rcu_read_lock();
	if (effect_cpu_action(rcu_dereference(csdata), p, dst_cpu))
		*can_migrate = 0;
	rcu_read_unlock();
}

static void xmi_newidle_balance(void *unused, struct rq *this_rq,
				struct rq_flags *rf, int *pulled_task, int *done)
{
	struct cam_sched_data *data;
	unsigned int cpu = raw_smp_processor_id();

	if (!READ_ONCE(sched_ready) || !(READ_ONCE(msger_siwtches) & SCD_ENABLE) ||
	    *pulled_task == 1 || cpu >= CPU_NR)
		return;
	rcu_read_lock();
	data = rcu_dereference(csdata);
	if (data && data->flag[cpu] != TISOT_DEFAULT)
		*done = 1;
	rcu_read_unlock();
}

int cam_sched_enable(void)
{
	int ret;

	/* Restricted hooks cannot be unregistered. Retry only missing registrations. */
	if (!(registered_hooks & BIT(0))) {
		ret = register_trace_android_rvh_is_cpu_allowed(xmi_rvh_is_cpu_allowed, NULL);
		if (ret)
			return ret;
		registered_hooks |= BIT(0);
	}
	if (!(registered_hooks & BIT(1))) {
		ret = register_trace_android_rvh_can_migrate_task(xmi_can_migrate_task, NULL);
		if (ret)
			return ret;
		registered_hooks |= BIT(1);
	}
	if (!(registered_hooks & BIT(2))) {
		ret = register_trace_android_rvh_select_task_rq_fair(xmi_select_task_rq_fair, NULL);
		if (ret)
			return ret;
		registered_hooks |= BIT(2);
	}
	if (!(registered_hooks & BIT(3))) {
		ret = register_trace_android_rvh_sched_newidle_balance(xmi_newidle_balance, NULL);
		if (ret)
			return ret;
		registered_hooks |= BIT(3);
	}
	WRITE_ONCE(sched_ready, true);
	return 0;
}

int cam_sched_init(void)
{
	struct cam_sched_data *data;
	int ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	rcu_assign_pointer(csdata, data);
	ret = cam_sched_enable();
	if (ret && !registered_hooks) {
		RCU_INIT_POINTER(csdata, NULL);
		kfree(data);
		return ret;
	}
	/* Keep resident callbacks valid after a partial failure. swh can retry. */
	if (ret)
		pr_err("camera scheduler hooks incomplete: %d; enable command may retry\n", ret);
	return 0;
}
