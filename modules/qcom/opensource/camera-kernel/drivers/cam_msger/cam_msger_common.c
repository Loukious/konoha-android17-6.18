// MIUI ADD: Camera_CameraOpt
#include "cam_msger_common.h"
#include <linux/string_helpers.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>

#define PID_ARRAY_SIZE 10

int find_pid_cmdline(char *cmdline)
{
	struct task_struct *task, *tasks[PID_ARRAY_SIZE];
	char comm[TASK_COMM_LEN], *candidate;
	size_t length;
	int count = 0, idx, ret = -1;

	if (!cmdline)
		return -1;
	length = strnlen(cmdline, CMDLINE_MAX_LEN);
	if (!length || length == CMDLINE_MAX_LEN)
		return -1;
	if (cmdline[length - 1] == '\n')
		length--;
	if (!length)
		return -1;
	rcu_read_lock();
	for_each_process(task) {
		if (count == PID_ARRAY_SIZE)
			break;
		get_task_comm(comm, task);
		if (length < TASK_COMM_LEN && strlen(comm) > length)
			continue;
		if (!strstr(cmdline, comm))
			continue;
		get_task_struct(task);
		tasks[count++] = task;
	}
	rcu_read_unlock();
	for (idx = 0; idx < count; idx++) {
		if (ret < 0) {
			candidate = kstrdup_quotable_cmdline(tasks[idx], GFP_KERNEL);
			if (candidate && !strncmp(candidate, cmdline, length))
				ret = tasks[idx]->tgid;
			kfree(candidate);
		}
		put_task_struct(tasks[idx]);
	}
	return ret;
}
