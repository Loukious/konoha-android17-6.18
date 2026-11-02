// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msm_sysstats.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/pid_namespace.h>
#include <net/genetlink.h>
#include <linux/atomic.h>
#include <linux/sched/cputime.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/fdtable.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/srcu.h>
#include <linux/mutex.h>

#include <linux/qcom_dma_heap.h>

struct tgid_iter {
	unsigned int tgid;
	struct task_struct *task;
};

static struct genl_family family;

static u64 (*sysstats_kgsl_get_stats)(pid_t pid) __rcu;
DEFINE_STATIC_SRCU(sysstats_kgsl_srcu);
static DEFINE_MUTEX(sysstats_kgsl_lock);

#define SYSSTATS_CMD_ATTR_MAX 3
static const struct nla_policy sysstats_cmd_get_policy[SYSSTATS_CMD_ATTR_MAX + 1] = {
	[SYSSTATS_TASK_CMD_ATTR_PID]  = { .type = NLA_U32 },
	[SYSSTATS_TASK_CMD_ATTR_FOREACH]  = { .type = NLA_U32 },
	[SYSSTATS_TASK_CMD_ATTR_PIDS_OF_NAME] = {
		.type = NLA_NUL_STRING, .len = TASK_COMM_LEN - 1 },
};
static u64 sysstats_kgsl_stats(pid_t pid)
{
	u64 (*callback)(pid_t pid);
	u64 bytes = 0;
	int index;

	index = srcu_read_lock(&sysstats_kgsl_srcu);
	callback = srcu_dereference(sysstats_kgsl_get_stats, &sysstats_kgsl_srcu);
	if (callback)
		bytes = callback(pid);
	srcu_read_unlock(&sysstats_kgsl_srcu, index);
	return bytes;
}

void sysstats_register_kgsl_stats_cb(u64 (*cb)(pid_t pid))
{
	mutex_lock(&sysstats_kgsl_lock);
	rcu_assign_pointer(sysstats_kgsl_get_stats, cb);
	/* Replacing a callback must also finish every invocation of the old one. */
	synchronize_srcu(&sysstats_kgsl_srcu);
	mutex_unlock(&sysstats_kgsl_lock);
}
EXPORT_SYMBOL_GPL(sysstats_register_kgsl_stats_cb);

void sysstats_unregister_kgsl_stats_cb(void)
{
	sysstats_register_kgsl_stats_cb(NULL);
}
EXPORT_SYMBOL_GPL(sysstats_unregister_kgsl_stats_cb);



static int send_reply(struct sk_buff *skb, struct genl_info *info)
{
	struct genlmsghdr *genlhdr = nlmsg_data(nlmsg_hdr(skb));
	void *reply = genlmsg_data(genlhdr);

	genlmsg_end(skb, reply);

	return genlmsg_reply(skb, info);
}

static int prepare_reply(struct genl_info *info, u8 cmd, struct sk_buff **skbp,
				size_t size)
{
	struct sk_buff *skb;
	void *reply;

	skb = genlmsg_new(size, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	reply = genlmsg_put_reply(skb, info, &family, 0, cmd);
	if (reply == NULL) {
		nlmsg_free(skb);
		return -EINVAL;
	}

	*skbp = skb;
	return 0;
}

static struct task_struct *find_lock_task_mm(struct task_struct *p)
{
	struct task_struct *t;

	rcu_read_lock();

	for_each_thread(p, t) {
		task_lock(t);
		if (likely(t->mm))
			goto found;
		task_unlock(t);
	}
	t = NULL;
found:
	rcu_read_unlock();

	return t;
}

static struct sighand_struct *sysstats_lock_task_sighand(struct task_struct *tsk,
					   unsigned long *flags)
{
	struct sighand_struct *sighand;

	rcu_read_lock();
	for (;;) {
		sighand = rcu_dereference(tsk->sighand);
		if (unlikely(sighand == NULL))
			break;

		spin_lock_irqsave(&sighand->siglock, *flags);
		if (likely(sighand == tsk->sighand))
			break;
		spin_unlock_irqrestore(&sighand->siglock, *flags);
	}
	rcu_read_unlock();

	return sighand;
}

static bool is_system_dmabufheap(struct dma_buf *dmabuf)
{
	if (!strcmp(dmabuf->exp_name, "qcom,system") ||
		!strcmp(dmabuf->exp_name, "qcom,system-uncached") ||
		!strcmp(dmabuf->exp_name, "system-secure") ||
		!strcmp(dmabuf->exp_name, "qcom,secure-pixel") ||
		!strcmp(dmabuf->exp_name, "qcom,secure-non-pixel"))
		return true;
	return false;
}

static int get_dma_info(const void *data, struct file *file, unsigned int n)
{
	struct dma_buf *dmabuf;
	unsigned long *size = (unsigned long *)data;

	if (!qcom_is_dma_buf_file(file))
		return 0;

	dmabuf = (struct dma_buf *)file->private_data;
	if (is_system_dmabufheap(dmabuf))
		*size += dmabuf->size;
	return 0;
}

static unsigned long get_task_unreclaimable_info(struct task_struct *task)
{
	struct task_struct *thread;
	struct files_struct *files, *leader_files;
	unsigned long size = 0;

	/* Never nest two task locks: parallel dump requests can visit the same group. */
	rcu_read_lock();
	task_lock(task->group_leader);
	leader_files = task->group_leader->files;
	task_unlock(task->group_leader);
	for_each_thread(task, thread) {
		task_lock(thread);
		files = thread->files;
		if (files && (files != leader_files || thread == task->group_leader))
			iterate_fd(files, 0, get_dma_info, &size);
		task_unlock(thread);
	}
	rcu_read_unlock();
	return size >> PAGE_SHIFT;
}

static unsigned long get_system_unreclaimble_info(void)
{
	struct task_struct *task;
	unsigned long size = 0;

	rcu_read_lock();
	for_each_process(task)
		size += get_task_unreclaimable_info(task);
	rcu_read_unlock();

	/* Account the kgsl information. */
	size += sysstats_kgsl_stats(-1) >> PAGE_SHIFT;

	return size;
}
static char *nla_strdup_cust(const struct nlattr *nla, gfp_t flags)
{
	size_t srclen = nla_len(nla);
	char *src = nla_data(nla), *dst;

	if (srclen > 0 && src[srclen - 1] == '\0')
		srclen--;

	dst = kmalloc(srclen + 1, flags);
	if (dst != NULL) {
		memcpy(dst, src, srclen);
		dst[srclen] = '\0';
	}
	return dst;
}

static int sysstats_task_cmd_attr_pid(struct genl_info *info)
{
	struct sysstats_task *stats;
	struct sk_buff *rep_skb;
	struct nlattr *ret;
	struct task_struct *tsk;
	struct task_struct *p;
	size_t size;
	u32 pid;
	int rc;
	u64 utime, stime;
	const struct cred *tcred;
#ifdef CONFIG_CPUSETS
	struct cgroup_subsys_state *css;
#endif
	unsigned long flags;
	struct signal_struct *sig;

	size = nla_total_size_64bit(sizeof(struct sysstats_task));

	rc = prepare_reply(info, SYSSTATS_TASK_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return rc;

	rc = -EINVAL;
	pid = nla_get_u32(info->attrs[SYSSTATS_TASK_CMD_ATTR_PID]);

	ret = nla_reserve_64bit(rep_skb, SYSSTATS_TASK_TYPE_STATS,
				sizeof(struct sysstats_task), SYSSTATS_TYPE_NULL);
	if (!ret)
		goto err;

	stats = nla_data(ret);

	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (tsk)
		get_task_struct(tsk);
	rcu_read_unlock();
	if (!tsk) {
		rc = -ESRCH;
		goto err;
	}
	memset(stats, 0, sizeof(*stats));
	stats->pid = task_pid_nr_ns(tsk, task_active_pid_ns(current));
	p = find_lock_task_mm(tsk);
	if (p) {
		__acquire(p->alloc_lock);
#define K(x) ((x) << (PAGE_SHIFT - 10))
		stats->anon_rss = K(get_mm_counter(p->mm, MM_ANONPAGES));
		stats->file_rss = K(get_mm_counter(p->mm, MM_FILEPAGES));
		stats->shmem_rss = K(get_mm_counter(p->mm, MM_SHMEMPAGES));
		stats->swap_rss = K(get_mm_counter(p->mm, MM_SWAPENTS));
#undef K
		task_unlock(p);
	}

	stats->unreclaimable = get_task_unreclaimable_info(tsk) << (PAGE_SHIFT - 10);
	stats->unreclaimable += sysstats_kgsl_stats(task_tgid_nr(tsk)) >> 10;

	task_cputime(tsk, &utime, &stime);
	stats->utime = div_u64(utime, NSEC_PER_USEC);
	stats->stime = div_u64(stime, NSEC_PER_USEC);

	if (sysstats_lock_task_sighand(tsk, &flags)) {
		sig = tsk->signal;
		stats->cutime = div_u64(sig->cutime, NSEC_PER_USEC);
		stats->cstime = div_u64(sig->cstime, NSEC_PER_USEC);
		unlock_task_sighand(tsk, &flags);
	}

	rcu_read_lock();
	tcred = __task_cred(tsk);
	stats->uid = from_kuid_munged(current_user_ns(), tcred->uid);
	stats->ppid = pid_alive(tsk) ?
		task_tgid_nr_ns(rcu_dereference(tsk->real_parent),
			task_active_pid_ns(current)) : 0;
	rcu_read_unlock();

	task_lock(tsk);
	strscpy(stats->name, tsk->comm, sizeof(stats->name));
	task_unlock(tsk);

#ifdef CONFIG_CPUSETS
	css = task_get_css(tsk, cpuset_cgrp_id);
	cgroup_path_ns(css->cgroup, stats->state, sizeof(stats->state),
				current->nsproxy->cgroup_ns);
	css_put(css);
#endif

	put_task_struct(tsk);

	return send_reply(rep_skb, info);
err:
	nlmsg_free(rep_skb);
	return rc;
}

static int sysstats_task_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	if (info->attrs[SYSSTATS_TASK_CMD_ATTR_PID])
		return sysstats_task_cmd_attr_pid(info);
	else
		return -EINVAL;
}

static struct tgid_iter next_tgid(struct pid_namespace *ns, struct tgid_iter iter)
{
	struct pid *pid;

	if (iter.task)
		put_task_struct(iter.task);
	rcu_read_lock();
retry:
	iter.task = NULL;
	pid = idr_get_next(&ns->idr, &iter.tgid);
	if (pid) {
		iter.tgid = pid_nr_ns(pid, ns);
		iter.task = pid_task(pid, PIDTYPE_TGID);
		if (!iter.task) {
			iter.tgid += 1;
			goto retry;
		}
		get_task_struct(iter.task);
	}
	rcu_read_unlock();
	return iter;
}

static int sysstats_all_pids_of_name(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct pid_namespace *ns = task_active_pid_ns(current);
	struct tgid_iter iter;
	void *reply;
	struct nlattr *attr;
	struct nlattr *nla;
	struct sysstats_pid *stats;
	char *comm;
	char task_comm[TASK_COMM_LEN];

	nla = genl_info_dump(cb)->attrs[SYSSTATS_TASK_CMD_ATTR_PIDS_OF_NAME];
	if (!nla)
		goto out;

	comm = nla_strdup_cust(nla, GFP_KERNEL);
	if (!comm)
		goto out;

	iter.tgid = cb->args[0];
	iter.task = NULL;
	for (iter = next_tgid(ns, iter); iter.task;
			iter.tgid += 1, iter = next_tgid(ns, iter)) {

		get_task_comm(task_comm, iter.task);
		if (strcmp(task_comm, comm))
			continue;
		reply = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
			cb->nlh->nlmsg_seq, &family, NLM_F_MULTI, SYSSTATS_PIDS_CMD_GET);
		if (reply == NULL) {
			put_task_struct(iter.task);
			break;
		}
		attr = nla_reserve(skb, SYSSTATS_PID_TYPE_STATS,
				sizeof(struct sysstats_pid));
		if (!attr) {
			put_task_struct(iter.task);
			genlmsg_cancel(skb, reply);
			break;
		}
		stats = nla_data(attr);
		memset(stats, 0, sizeof(struct sysstats_pid));
		rcu_read_lock();
		stats->pid = task_pid_nr_ns(iter.task,
						task_active_pid_ns(current));
		rcu_read_unlock();
		genlmsg_end(skb, reply);
	}
	cb->args[0] = iter.tgid;
	kfree(comm);
out:
	return skb->len;
}

static int sysstats_task_foreach(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct pid_namespace *ns = task_active_pid_ns(current);
	struct tgid_iter iter;
	void *reply;
	struct nlattr *attr;
	struct nlattr *nla;
	struct sysstats_task *stats;
	struct task_struct *p;
	short oom_score;
	short oom_score_min;
	short oom_score_max;
	u32 buf;

	nla = genl_info_dump(cb)->attrs[SYSSTATS_TASK_CMD_ATTR_FOREACH];

	if (!nla)
		goto out;

	buf  = nla_get_u32(nla);
	oom_score_min = (short) (buf & 0xFFFF);
	oom_score_max = (short) ((buf >> 16) & 0xFFFF);

	iter.tgid = cb->args[0];
	iter.task = NULL;
	for (iter = next_tgid(ns, iter); iter.task;
			iter.tgid += 1, iter = next_tgid(ns, iter)) {

		if (iter.task->flags & PF_KTHREAD)
			continue;

		oom_score = iter.task->signal->oom_score_adj;
		if ((oom_score < oom_score_min)
			|| (oom_score > oom_score_max))
			continue;

		reply = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
			cb->nlh->nlmsg_seq, &family, NLM_F_MULTI, SYSSTATS_TASK_CMD_GET);
		if (reply == NULL) {
			put_task_struct(iter.task);
			break;
		}
		attr = nla_reserve(skb, SYSSTATS_TASK_TYPE_FOREACH,
				sizeof(struct sysstats_task));
		if (!attr) {
			put_task_struct(iter.task);
			genlmsg_cancel(skb, reply);
			break;
		}
		stats = nla_data(attr);
		memset(stats, 0, sizeof(struct sysstats_task));
		rcu_read_lock();
		stats->pid = task_pid_nr_ns(iter.task,
						task_active_pid_ns(current));
		stats->oom_score = iter.task->signal->oom_score_adj;
		rcu_read_unlock();
		p = find_lock_task_mm(iter.task);
		if (p) {
#define K(x) ((x) << (PAGE_SHIFT - 10))
			__acquire(p->alloc_lock);
			stats->anon_rss =
				K(get_mm_counter(p->mm, MM_ANONPAGES));
			stats->file_rss =
				K(get_mm_counter(p->mm, MM_FILEPAGES));
			stats->shmem_rss =
				K(get_mm_counter(p->mm, MM_SHMEMPAGES));
			stats->swap_rss =
				K(get_mm_counter(p->mm, MM_SWAPENTS));
			task_unlock(p);
#undef K
		}
		stats->unreclaimable = get_task_unreclaimable_info(iter.task) << (PAGE_SHIFT - 10);
		genlmsg_end(skb, reply);
	}

	cb->args[0] = iter.tgid;
out:
	return skb->len;
}

#define K(x) ((x) << (PAGE_SHIFT - 10))
#ifndef CONFIG_NUMA
static void sysstats_fill_zoneinfo(struct sysstats_mem *stats)
{
	pg_data_t *pgdat;
	struct zone *zone;
	struct zone *node_zones;
	unsigned long zspages = 0;

	pgdat = NODE_DATA(0);
	node_zones = pgdat->node_zones;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (!populated_zone(zone))
			continue;

		zspages += zone_page_state(zone, NR_ZSPAGES);
		if (!strcmp(zone->name, "DMA")) {
			stats->dma_nr_free =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->dma_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->dma_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->dma_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->dma_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		} else if (!strcmp(zone->name, "Normal")) {
			stats->normal_nr_free =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->normal_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->normal_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->normal_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->normal_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		} else if (!strcmp(zone->name, "HighMem")) {
			stats->highmem_nr_free =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->highmem_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->highmem_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->highmem_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->highmem_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		} else if (!strcmp(zone->name, "Movable")) {
			stats->movable_nr_free =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->movable_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->movable_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->movable_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->movable_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		}
	}
	stats->zram_compressed = K(zspages);
}
#else
static void sysstats_fill_zoneinfo(struct sysstats_mem *stats)
{
}
#endif

static void sysstats_build(struct sysstats_mem *stats)
{
	struct sysinfo i;

	si_meminfo(&i);
#ifndef CONFIG_MSM_SYSSTATS_STUB_NONEXPORTED_SYMBOLS
	si_swapinfo(&i);
	stats->swap_used = K(i.totalswap - i.freeswap);
	stats->swap_total = K(i.totalswap);
	stats->vmalloc_total = K(vmalloc_nr_pages());
#else
	stats->swap_used = 0;
	stats->swap_total = 0;
	stats->vmalloc_total = 0;
#endif
	stats->memtotal = K(i.totalram);
	stats->misc_reclaimable =
		K(global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE));
	stats->unreclaimable = K(get_system_unreclaimble_info());
	stats->buffer = K(i.bufferram);
	stats->swapcache = K(total_swapcache_pages());
	stats->slab_reclaimable =
		K(global_node_page_state_pages(NR_SLAB_RECLAIMABLE_B));
	stats->slab_unreclaimable =
		K(global_node_page_state_pages(NR_SLAB_UNRECLAIMABLE_B));
	stats->free_cma = K(global_zone_page_state(NR_FREE_CMA_PAGES));
	stats->file_mapped = K(global_node_page_state(NR_FILE_MAPPED));
	stats->kernelstack = global_node_page_state(NR_KERNEL_STACK_KB);
	stats->pagetable = K(global_node_page_state(NR_PAGETABLE));
	stats->shmem = K(i.sharedram);
	sysstats_fill_zoneinfo(stats);
}
#undef K

static int sysstats_meminfo_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	int rc = 0;
	struct sk_buff *rep_skb;
	struct sysstats_mem *stats;
	struct nlattr *na;
	size_t size;

	size = nla_total_size(sizeof(struct sysstats_mem));

	rc = prepare_reply(info, SYSSTATS_MEMINFO_CMD_NEW, &rep_skb,
				size);
	if (rc < 0)
		goto err;

	na = nla_reserve(rep_skb, SYSSTATS_MEMINFO_TYPE_STATS,
				sizeof(struct sysstats_mem));
	if (na == NULL) {
		nlmsg_free(rep_skb);
		rc = -EMSGSIZE;
		goto err;
	}

	stats = nla_data(na);
	memset(stats, 0, sizeof(*stats));

	sysstats_build(stats);

	rc = send_reply(rep_skb, info);
err:
	return rc;
}

static const struct genl_ops sysstats_ops[] = {
	{
		.cmd		= SYSSTATS_TASK_CMD_GET,
		.validate = GENL_DONT_VALIDATE_STRICT,
		.doit		= sysstats_task_user_cmd,
		.dumpit		= sysstats_task_foreach,
	},
	{
		.cmd		= SYSSTATS_MEMINFO_CMD_GET,
		.validate = GENL_DONT_VALIDATE_STRICT,
		.doit		= sysstats_meminfo_user_cmd,
	},
	{
		.cmd		= SYSSTATS_PIDS_CMD_GET,
		.validate	= GENL_DONT_VALIDATE_STRICT,
		.dumpit		= sysstats_all_pids_of_name,
	}
};

static struct genl_family family __ro_after_init = {
	.name		= SYSSTATS_GENL_NAME,
	.version	= SYSSTATS_GENL_VERSION,
	.maxattr	= SYSSTATS_CMD_ATTR_MAX,
	.module		= THIS_MODULE,
	.ops		= sysstats_ops,
	.n_ops		= ARRAY_SIZE(sysstats_ops),
	.policy		= sysstats_cmd_get_policy,
	.resv_start_op	= SYSSTATS_PIDS_CMD_GET + 1,
};

static int __init sysstats_init(void)
{
	int rc;

	rc = genl_register_family(&family);
	if (rc)
		return rc;

	pr_info("registered sysstats version %d\n", SYSSTATS_GENL_VERSION);
	return 0;
}

static void __exit sysstats_exit(void)
{
	genl_unregister_family(&family);
	sysstats_unregister_kgsl_stats_cb();
}

module_init(sysstats_init);
module_exit(sysstats_exit);
MODULE_IMPORT_NS("MINIDUMP");
MODULE_DESCRIPTION("Onyx system and task statistics");
MODULE_LICENSE("GPL");
