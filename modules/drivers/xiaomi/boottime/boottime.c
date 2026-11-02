#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/proc_fs.h>
#include <linux/printk.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/tracepoint.h>
#include <trace/events/initcall.h>
#include <linux/mutex.h>
#include "boottime.h"

/**
 * Data structures to store tracepoints information
 */
struct tracepoints_table {
	const char *name;
	void *func;
	struct tracepoint *tp;
	void *data;
	bool init;
};

/**
 * Data structures to store initcall start time info
 */
struct initcall_list_t {
	pid_t pid;
	pid_t tid;
	u64 timestamp;
	struct list_head dev_entry;
};

static void tp_deinit(void);

/* Parameters */
struct log_t *bootprof[BUF_COUNT];
unsigned long log_count;
DEFINE_SPINLOCK(bootprof_lock);
unsigned long boottime_ok;

static DEFINE_MUTEX(bootprof_switch_lock);
static struct proc_dir_entry *bootprof_entry;
static bool enabled;
static u64 timestamp_on, timestamp_off;
static bool boot_finish;

static struct list_head initcall_list;
static DEFINE_SPINLOCK(initcall_lock);
atomic_t initcall_num = ATOMIC_INIT(0);

static char pbl_time[32] = { 0 };
module_param_string(pblt, pbl_time, 32, 0644);

static char xbl_time[32] = { 0 };
module_param_string(xblt, xbl_time, 32, 0644);

static char abl_time[32] = { 0 };
module_param_string(ablt, abl_time, 32, 0644);

EXPORT_SYMBOL_GPL(bootprof_lock);
EXPORT_SYMBOL_GPL(boottime_ok);

unsigned long get_log_count(void)
{
	/* Published entries are immutable until module teardown. */
	return smp_load_acquire(&log_count);
}
EXPORT_SYMBOL_GPL(get_log_count);

struct log_t **get_bootprof_pointer(void)
{
	return bootprof;
}
EXPORT_SYMBOL_GPL(get_bootprof_pointer);

bool mt_boot_finish(void)
{
	return READ_ONCE(boot_finish);
}
EXPORT_SYMBOL_GPL(mt_boot_finish);

static long long msec_high(unsigned long long nsec)
{
	if ((long long)nsec < 0) {
		nsec = -nsec;
		do_div(nsec, 1000000);
		return -nsec;
	}
	do_div(nsec, 1000000);

	return nsec;
}

static unsigned long msec_low(unsigned long long nsec)
{
	if ((long long)nsec < 0)
		nsec = -nsec;

	return do_div(nsec, 1000000);
}

void bootprof_log_boot(char *str)
{
	unsigned long flags;
	u64 ts = sched_clock();
	struct log_t *entry;
	char comm[TASK_COMM_LEN];
	size_t len;
	unsigned int block;

	if (!str)
		return;
	len = strnlen(str, BOOT_STR_SIZE - 1);
	get_task_comm(comm, current);
	spin_lock_irqsave(&bootprof_lock, flags);
	if (!enabled)
		goto out;
	if (log_count >= LOGS_PER_BUF * BUF_COUNT) {
		enabled = false;
		goto out;
	}
	block = log_count / LOGS_PER_BUF;
	if (!bootprof[block]) {
		bootprof[block] = kcalloc(LOGS_PER_BUF, sizeof(struct log_t),
					GFP_ATOMIC | __GFP_NORETRY | __GFP_NOWARN);
		if (!bootprof[block])
			goto out;
	}
	entry = &bootprof[block][log_count % LOGS_PER_BUF];
	entry->comm_event = kzalloc(TASK_COMM_LEN + len + 1,
				   GFP_ATOMIC | __GFP_NORETRY | __GFP_NOWARN);
	if (!entry->comm_event)
		goto out;
	entry->timestamp = ts;
	entry->pid = current->pid;
	memcpy(entry->comm_event, comm, TASK_COMM_LEN);
	memcpy(entry->comm_event + TASK_COMM_LEN, str, len);
	/* Readers may inspect a completed entry after acquiring this count. */
	smp_store_release(&log_count, log_count + 1);
out:
	spin_unlock_irqrestore(&bootprof_lock, flags);
}
EXPORT_SYMBOL_GPL(bootprof_log_boot);

static void bootprof_initcall(initcall_t fn, unsigned long long ts)
{
	/* log more than threshold initcalls */
	unsigned long msec_rem;
	char msgbuf[MSG_SIZE];
	int len;

	atomic_inc(&initcall_num);

	if (ts > BOOTPROF_THRESHOLD) {
		msec_rem = do_div(ts, NSEC_PER_MSEC);
		len = scnprintf(msgbuf, sizeof(msgbuf),
				"initcall: %ps %5llu.%06lums", fn, ts,
				msec_rem);
		if (len < 0)
			pr_err("BOOTTIME: initcall - Invalid argument.\n");
		bootprof_log_boot(msgbuf);
	}
}

#ifndef MODULE
/*Build-in*/
void bootprof_probe(unsigned long long ts, struct device *dev,
		    struct device_driver *drv, unsigned long probe)
{
	/* log more than threshold probes*/
	unsigned long msec_rem;
	char msgbuf[MSG_SIZE];
	int pos, len;

	if (ts <= BOOTPROF_THRESHOLD)
		return;
	msec_rem = do_div(ts, NSEC_PER_MSEC);

	pos = scnprintf(msgbuf, sizeof(msgbuf), "probe: probe=%ps",
			(void *)probe);
	if (pos < 0)
		pos = 0;

	if (drv) {
		len = scnprintf(msgbuf + pos, sizeof(msgbuf) - pos,
				" drv=%s(%ps)", drv->name ? drv->name : "",
				(void *)drv);
		if (len >= 0)
			pos += len;
	}

	if (dev && dev->init_name) {
		len = scnprintf(msgbuf + pos, sizeof(msgbuf) - pos,
				" dev=%s(%ps)", dev->init_name, (void *)dev);
		if (len >= 0)
			pos += len;
	}

	scnprintf(msgbuf + pos, sizeof(msgbuf) - pos, " %5llu.%06lums", ts,
		  msec_rem);
	bootprof_log_boot(msgbuf);
}
EXPORT_SYMBOL_GPL(bootprof_probe);

void bootprof_pdev_register(unsigned long long ts, struct platform_device *pdev)
{
	/* log more than threshold register*/
	unsigned long msec_rem;
	char msgbuf[MSG_SIZE];
	int len;

	if (ts <= BOOTPROF_THRESHOLD || !pdev)
		return;
	msec_rem = do_div(ts, NSEC_PER_MSEC);
	len = scnprintf(msgbuf, sizeof(msgbuf),
			"probe: pdev=%s(%ps) %5llu.%06lums", pdev->name,
			(void *)pdev, ts, msec_rem);
	if (len < 0)
		pr_err("BOOTTIME: pdev - Invalid argument.\n");

	bootprof_log_boot(msgbuf);
}
EXPORT_SYMBOL_GPL(bootprof_pdev_register);
#endif /*MODULE END*/

/*  initcalls tracepoint cb while initcall_debug=1 */
static __init_or_module void tp_initcall_start_cb(void *data, initcall_t fn)
{
	unsigned long flags;
	struct initcall_list_t *obj;
	struct initcall_list_t *pos, *next;
	struct list_head err_list;

	INIT_LIST_HEAD(&err_list);

	obj = kzalloc(sizeof(struct initcall_list_t),
		      GFP_ATOMIC | __GFP_NORETRY | __GFP_NOWARN);
	if (!obj)
		return;

	obj->pid = task_pid_nr(current);
	obj->tid = task_pid_vnr(current);
	obj->timestamp = sched_clock();

	/*Check if there is duplicated enrty.*/
	spin_lock_irqsave(&initcall_lock, flags);
	if (!list_empty(&initcall_list)) {
		list_for_each_entry_safe(pos, next, &initcall_list, dev_entry) {
			if ((pos->pid == obj->pid) && (pos->tid == obj->tid)) {
				list_del(&pos->dev_entry);
				/*Add duplicated enrty into err list*/
				list_add_tail(&pos->dev_entry, &err_list);
			}
		}
	}
	list_add_tail(&obj->dev_entry, &initcall_list);
	spin_unlock_irqrestore(&initcall_lock, flags);

	/*release entry of err list*/
	if (!list_empty(&err_list)) {
		list_for_each_entry_safe(pos, next, &err_list, dev_entry) {
			pr_err("[BOOTTIME] Warn:duplicated entry.(pid:%d, tid:%d)\n",
			       pos->pid, pos->tid);
			list_del(&pos->dev_entry);
			kfree(pos);
		}
	}
}

static __init_or_module void tp_initcall_finish_cb(void *data, initcall_t fn,
						   int ret)
{
	unsigned long flags;
	struct initcall_list_t *pos, *next;
	unsigned long long start_ts = 0;
	struct list_head memfree_list;
	unsigned long long end_ts = sched_clock();
	unsigned long long duration;

	INIT_LIST_HEAD(&memfree_list);

	spin_lock_irqsave(&initcall_lock, flags);
	list_for_each_entry_safe(pos, next, &initcall_list, dev_entry) {
		if ((pos->pid == task_pid_nr(current)) &&
		    (pos->tid == task_pid_vnr(current))) {
			start_ts = pos->timestamp;
			list_del(&pos->dev_entry);
			list_add_tail(&pos->dev_entry, &memfree_list);
			break;
		}
	}
	spin_unlock_irqrestore(&initcall_lock, flags);

	/*release entry*/
	if (!list_empty(&memfree_list)) {
		list_for_each_entry_safe(pos, next, &memfree_list, dev_entry) {
			list_del(&pos->dev_entry);
			kfree(pos);
		}
	}

	/* start time of current module is 0.*/
	if (start_ts == 0) {
#ifdef MODULE
		/* if bootprof is first loading module.*/
		bootprof_log_boot("Kernel_init_done");
#endif
		return;
	}
	duration = end_ts - start_ts;
	bootprof_initcall(fn, duration);
}

static struct tracepoints_table interests[] = {
	{ .name = "initcall_start", .func = tp_initcall_start_cb },
	{ .name = "initcall_finish", .func = tp_initcall_finish_cb },
};

#define FOR_EACH_INTEREST(i)                                                  \
	for (i = 0; i < sizeof(interests) / sizeof(struct tracepoints_table); \
	     i++)

/* Find the struct tracepoint associated */
/* with a given tracepointname.          */
static void tp_lookup(struct tracepoint *tp, void *ignore)
{
	unsigned int i;

	if (!tp || !tp->name)
		return;

	FOR_EACH_INTEREST(i)
	{
		if (strcmp(interests[i].name, tp->name) == 0)
			interests[i].tp = tp;
	}
}

/* Unregister initcalls tracepoints */
static void tp_deinit(void)
{
	unsigned int i;
	unsigned long flags;
	struct initcall_list_t *pos, *next;

	FOR_EACH_INTEREST(i)
	{
		if (interests[i].init) {
			tracepoint_probe_unregister(interests[i].tp,
						    interests[i].func,
						    interests[i].data);
			interests[i].init = false;
		}
	}

	tracepoint_synchronize_unregister();
	spin_lock_irqsave(&initcall_lock, flags);
	if (!list_empty(&initcall_list)) {
		list_for_each_entry_safe(pos, next, &initcall_list, dev_entry) {
			if (pos) {
				list_del(&pos->dev_entry);
				kfree(pos);
			}
		}
	}
	spin_unlock_irqrestore(&initcall_lock, flags);
	pr_info("BOOTTIME: Unregister initcalls tracepoint.\n");
}

/* Register initcalls tracepoints */
static int tp_init(void)
{
	unsigned int i;
	int ret;

	INIT_LIST_HEAD(&initcall_list);

	/* Install the tracepoints */
	for_each_kernel_tracepoint(tp_lookup, NULL);

	FOR_EACH_INTEREST(i)
	{
		if (!interests[i].tp) {
			pr_err("[BOOTTIME]TP: %s not found\n",
			       interests[i].name);
			/* Unload previously loaded */
			tp_deinit();
			return -ENOENT;
		}
		ret = tracepoint_probe_register(interests[i].tp, interests[i].func,
						interests[i].data);
		if (ret) {
			tp_deinit();
			return ret;
		}
		interests[i].init = true;
	}
	return 0;
}

static void mt_bootprof_switch(int on)
{
	unsigned long flags;
	bool finish = false;
	u64 ts = sched_clock();

	mutex_lock(&bootprof_switch_lock);
	spin_lock_irqsave(&bootprof_lock, flags);
	if (enabled != !!on) {
		enabled = !!on;
		if (on) {
			timestamp_on = ts;
		} else {
			timestamp_off = ts;
		}
	}
	/* A full log may already have disabled recording before this request. */
	if (!on && !boot_finish) {
		boot_finish = true;
		finish = true;
		timestamp_off = ts;
	}
	spin_unlock_irqrestore(&bootprof_lock, flags);
	if (finish)
		tp_deinit();
	mutex_unlock(&bootprof_switch_lock);
}

static ssize_t mt_bootprof_write(struct file *filp, const char __user *ubuf,
				 size_t cnt, loff_t *data)
{
	char buf[BOOT_STR_SIZE];
	size_t copy_size = cnt;

	if (cnt >= sizeof(buf))
		copy_size = BOOT_STR_SIZE - 1;

	if (copy_from_user(&buf, ubuf, copy_size))
		return -EFAULT;

	if (cnt == 1 && buf[0] == '1') {
		mt_bootprof_switch(1);
		return 1;
	} else if (cnt == 1 && buf[0] == '0') {
		mt_bootprof_switch(0);
		return 1;
	}

	buf[copy_size] = 0;
	bootprof_log_boot(buf);

	return cnt;
}

static int mt_bootprof_show(struct seq_file *m, void *v)
{
	unsigned long i, count, flags;
	u64 on, off;
	bool active;
	struct log_t *p;

	if (!m) {
		pr_err("seq_file is Null.\n");
		return 0;
	}
	spin_lock_irqsave(&bootprof_lock, flags);
	count = log_count;
	on = timestamp_on;
	off = timestamp_off;
	active = enabled;
	spin_unlock_irqrestore(&bootprof_lock, flags);
	seq_puts(m, "----------------------------------------\n");
	seq_printf(m, "BOOT PROF (unit:msec) %-10d\n", active);
	seq_printf(m, "Kernel Module Total %-10d\n",
		   atomic_read(&initcall_num));
	seq_puts(m, "----------------------------------------\n");

	seq_printf(m, "bootloader time consume\n");
	seq_printf(m, "%s: %-10sms\n", "plb time", pbl_time);
	seq_printf(m, "%s: %-10sms\n", "xbl time", xbl_time);
	seq_printf(m, "%s: %-10sms\n", "abl time", abl_time);
	seq_puts(m, "----------------------------------------\n");

	seq_printf(m, "%10lld.%06ld : ON (Threshold:%5lldms)\n",
		   msec_high(on), msec_low(on),
		   msec_high(BOOTPROF_THRESHOLD));

	for (i = 0; i < count; i++) {
		p = &bootprof[i / LOGS_PER_BUF][i % LOGS_PER_BUF];
		if (!p->comm_event)
			continue;

		seq_printf(m, "%10llu.%06lu :%5d-%-16s: %s\n",
			   msec_high(p->timestamp), msec_low(p->timestamp),
			   p->pid, p->comm_event,
			   p->comm_event + TASK_COMM_LEN);
	}

	seq_printf(m, "%10lld.%06ld : OFF\n", msec_high(off),
		   msec_low(off));
	seq_puts(m, "----------------------------------------\n");
	return 0;
}

/*** Seq operation of mtprof ****/
static int mt_bootprof_open(struct inode *inode, struct file *file)
{
	return single_open(file, mt_bootprof_show, inode->i_private);
}

static const struct proc_ops mt_bootprof_fops = {
	.proc_open = mt_bootprof_open,
	.proc_write = mt_bootprof_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static void bootprof_free_logs(void)
{
	unsigned int i, j;

	for (i = 0; i < BUF_COUNT; i++) {
		if (!bootprof[i])
			continue;
		for (j = 0; j < LOGS_PER_BUF; j++)
			kfree(bootprof[i][j].comm_event);
		kfree(bootprof[i]);
		bootprof[i] = NULL;
	}
}

static int __init bootprof_init(void)
{
	int ret;

	bootprof[0] = kcalloc(LOGS_PER_BUF, sizeof(struct log_t), GFP_KERNEL);
	if (!bootprof[0])
		return -ENOMEM;
	ret = tp_init();
	if (ret)
		goto free_logs;
	mt_bootprof_switch(1);
	bootprof_entry = proc_create("bootprof", 0664, NULL, &mt_bootprof_fops);
	if (!bootprof_entry) {
		mt_bootprof_switch(0);
		ret = -ENOMEM;
		goto free_logs;
	}
	WRITE_ONCE(boottime_ok, 1);
	return 0;
free_logs:
	bootprof_free_logs();
	return ret;
}

static void __exit bootprof_exit(void)
{
	unsigned long flags;

	/* proc_remove drains active proc operations before storage is freed. */
	proc_remove(bootprof_entry);
	mutex_lock(&bootprof_switch_lock);
	WRITE_ONCE(boottime_ok, 0);
	spin_lock_irqsave(&bootprof_lock, flags);
	enabled = false;
	spin_unlock_irqrestore(&bootprof_lock, flags);
	tp_deinit();
	bootprof_free_logs();
	mutex_unlock(&bootprof_switch_lock);
}

early_initcall(bootprof_init);
module_exit(bootprof_exit);
MODULE_DESCRIPTION("Xiaomi boot-time profiler");
MODULE_LICENSE("GPL v2");
