// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "va-minidump: %s: " fmt, __func__

#include <linux/init.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/overflow.h>
#include <linux/rculist.h>
#include <linux/uaccess.h>
#include <linux/elf.h>
#include <linux/slab.h>
#include <linux/panic_notifier.h>
#include <soc/qcom/minidump.h>
#include "elf.h"

struct va_md_tree_node {
	struct va_md_entry entry;
	int lindex;
	int rindex;
};

struct va_md_elf_info {
	unsigned long ehdr;
	unsigned long shdr_cnt;
	unsigned long phdr_cnt;
	unsigned long pload_size;
	unsigned long str_tbl_size;
};

#define VA_MD_VADDR_MARKER	-1
#define VA_MD_CB_MARKER		-2

struct va_minidump_data {
	phys_addr_t mem_phys_addr;
	unsigned int total_mem_size;
	unsigned long elf_mem;
	unsigned int num_sections;
	unsigned long str_tbl_idx;
	struct va_md_elf_info elf;
	struct md_region md_entry;
	bool in_oops_handler;
	bool collecting;
	int capture_cpu;
	size_t elf_size;
	size_t calc_size;
	dma_addr_t dma_handle;
	bool va_md_minidump_reg;
	bool va_md_init;
	struct list_head va_md_list;
	struct kset *va_md_kset;
};

/*
 * Incase, client uses stack memory to allocate
 * notifier block, we have to make a copy.
 */
struct notifier_block_list {
	struct notifier_block nb;
	struct list_head nb_list;
};

struct va_md_s_data {
	struct kobject s_kobj;
	struct atomic_notifier_head va_md_s_notif_list;
	struct list_head va_md_s_list;
	struct list_head va_md_s_nb_list;
	bool enable;
};

static struct va_minidump_data va_md_data;
static DEFINE_RAW_SPINLOCK(va_md_region_lock);
static DEFINE_MUTEX(va_md_lock);

#define to_va_md_attr(_attr) container_of(_attr, struct va_md_attribute, attr)
#define to_va_md_s_data(obj) container_of(obj, struct va_md_s_data, s_kobj)

struct va_md_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
					char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
					const char *buf, size_t count);
};

static ssize_t attr_show(struct kobject *kobj, struct attribute *attr,
							char *buf)
{
	struct va_md_attribute *va_md_attr = to_va_md_attr(attr);
	ssize_t ret = -EIO;

	if (va_md_attr->show)
		ret = va_md_attr->show(kobj, attr, buf);

	return ret;
}

static ssize_t attr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	struct va_md_attribute *va_md_attr = to_va_md_attr(attr);
	ssize_t ret = -EIO;

	if (va_md_attr->store)
		ret = va_md_attr->store(kobj, attr, buf, count);

	return ret;
}

static const struct sysfs_ops va_md_sysfs_ops = {
	.show   = attr_show,
	.store  = attr_store,
};

static void va_md_kobj_release(struct kobject *kobj)
{
	kfree(to_va_md_s_data(kobj));
}

static const struct kobj_type va_md_kobj_type = {
	.release = va_md_kobj_release,
	.sysfs_ops      = &va_md_sysfs_ops,
};

static ssize_t enable_show(struct kobject *kobj, struct attribute *this, char *buf)
{
	struct va_md_s_data *vamd_sdata = to_va_md_s_data(kobj);

	return scnprintf(buf, PAGE_SIZE, "enable: %u\n", READ_ONCE(vamd_sdata->enable));
}

static ssize_t enable_store(struct kobject *kobj, struct attribute *this,
				const char *buf, size_t count)
{
	struct va_md_s_data *vamd_sdata = to_va_md_s_data(kobj);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret) {
		pr_err("Invalid value passed\n");
		return ret;
	}

	WRITE_ONCE(vamd_sdata->enable, val);
	return count;
}

static struct va_md_attribute va_md_s_attr = __ATTR_RW(enable);
static struct attribute *va_md_s_attrs[] = {
	&va_md_s_attr.attr,
	NULL
};

static struct attribute_group va_md_s_attr_group = {
	.attrs = va_md_s_attrs,
};

bool qcom_va_md_enabled(void)
{
	/*
	 * Ensure that minidump is enabled and va-minidump is initialized
	 * before we start registration.
	 */
	return msm_minidump_enabled() && smp_load_acquire(&va_md_data.va_md_init);
}
EXPORT_SYMBOL_GPL(qcom_va_md_enabled);
int qcom_va_md_register(const char *name, struct notifier_block *nb)
{
	struct va_md_s_data *sdata;
	struct notifier_block_list *nbl, *pos;
	struct kobject *kobj = NULL;
	bool created = false;
	int ret;

	if (!name || !*name || strchr(name, '/') || !nb || !nb->notifier_call)
		return -EINVAL;
	if (!qcom_va_md_enabled())
		return -ENODEV;
	nbl = kzalloc(sizeof(*nbl), GFP_KERNEL);
	if (!nbl)
		return -ENOMEM;
	nbl->nb.notifier_call = nb->notifier_call;
	nbl->nb.priority = nb->priority;
	mutex_lock(&va_md_lock);
	if (!qcom_va_md_enabled()) {
		ret = -ENODEV;
		goto free_notifier;
	}
	kobj = kset_find_obj(va_md_data.va_md_kset, name);
	if (kobj) {
		sdata = to_va_md_s_data(kobj);
	} else {
		sdata = kzalloc(sizeof(*sdata), GFP_KERNEL);
		if (!sdata) {
			ret = -ENOMEM;
			goto free_notifier;
		}
		ATOMIC_INIT_NOTIFIER_HEAD(&sdata->va_md_s_notif_list);
		INIT_LIST_HEAD(&sdata->va_md_s_nb_list);
		INIT_LIST_HEAD(&sdata->va_md_s_list);
		sdata->s_kobj.kset = va_md_data.va_md_kset;
		ret = kobject_init_and_add(&sdata->s_kobj, &va_md_kobj_type,
					   &va_md_data.va_md_kset->kobj, "%s", name);
		if (ret) {
			kobject_put(&sdata->s_kobj);
			goto free_notifier;
		}
		ret = sysfs_create_group(&sdata->s_kobj, &va_md_s_attr_group);
		if (ret) {
			kobject_put(&sdata->s_kobj);
			goto free_notifier;
		}
		created = true;
	}
	list_for_each_entry(pos, &sdata->va_md_s_nb_list, nb_list) {
		if (pos->nb.notifier_call == nb->notifier_call) {
			ret = -EEXIST;
			goto free_notifier;
		}
	}
	ret = atomic_notifier_chain_register(&sdata->va_md_s_notif_list, &nbl->nb);
	if (ret) {
		if (created) {
			sysfs_remove_group(&sdata->s_kobj, &va_md_s_attr_group);
			kobject_put(&sdata->s_kobj);
		}
		goto free_notifier;
	}
	list_add_tail(&nbl->nb_list, &sdata->va_md_s_nb_list);
	if (created) {
		list_add_tail_rcu(&sdata->va_md_s_list, &va_md_data.va_md_list);
		kobject_uevent(&sdata->s_kobj, KOBJ_ADD);
	}
	ret = 0;
	goto out;
free_notifier:
	kfree(nbl);
out:
	if (kobj)
		kobject_put(kobj);
	mutex_unlock(&va_md_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_va_md_register);
int qcom_va_md_unregister(const char *name, struct notifier_block *nb)
{
	struct va_md_s_data *sdata;
	struct notifier_block_list *nbl;
	struct kobject *kobj;
	int ret = -EINVAL;

	if (!name || !*name || !nb || !nb->notifier_call)
		return -EINVAL;
	mutex_lock(&va_md_lock);
	/* Teardown must still drain callbacks if the base minidump is disabled. */
	if (!smp_load_acquire(&va_md_data.va_md_init)) {
		ret = -ENODEV;
		goto out;
	}
	kobj = kset_find_obj(va_md_data.va_md_kset, name);
	if (!kobj)
		goto out;
	sdata = to_va_md_s_data(kobj);
	list_for_each_entry(nbl, &sdata->va_md_s_nb_list, nb_list) {
		if (nbl->nb.notifier_call != nb->notifier_call)
			continue;
		ret = atomic_notifier_chain_unregister(&sdata->va_md_s_notif_list,
						       &nbl->nb);
		if (ret)
			break;
		list_del(&nbl->nb_list);
		kfree(nbl);
		if (list_empty(&sdata->va_md_s_nb_list)) {
			list_del_rcu(&sdata->va_md_s_list);
			synchronize_rcu();
			sysfs_remove_group(kobj, &va_md_s_attr_group);
			kobject_del(kobj);
			kobject_put(kobj); /* Drop the subsystem's owning reference. */
		}
		break;
	}
	kobject_put(kobj); /* Drop kset_find_obj's lookup reference. */
out:
	mutex_unlock(&va_md_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_va_md_unregister);
static void va_md_add_entry(struct va_md_entry *entry)
{
	struct va_md_tree_node *dst = (struct va_md_tree_node *)va_md_data.elf_mem +
				     va_md_data.num_sections;

	dst->entry = *entry;
	dst->entry.owner[MAX_OWNER_STRING - 1] = '\0';
	dst->lindex = entry->vaddr ? VA_MD_VADDR_MARKER : VA_MD_CB_MARKER;
	dst->rindex = dst->lindex;
	va_md_data.num_sections++;
}

static bool va_md_check_overlap(struct va_md_entry *entry, unsigned int index)
{
	struct va_md_tree_node *node = (struct va_md_tree_node *)va_md_data.elf_mem;
	unsigned long node_start = node[index].entry.vaddr;
	unsigned long node_end = node_start + node[index].entry.size - 1;
	unsigned long end = entry->vaddr + entry->size - 1;

	return entry->vaddr <= node_end && node_start <= end;
}

static int va_md_tree_insert(struct va_md_entry *entry)
{
	struct va_md_tree_node *tree = (struct va_md_tree_node *)va_md_data.elf_mem;
	unsigned int i;

	/* A bounded flat scan avoids stale static tree state between panic runs. */
	if (entry->vaddr) {
		for (i = 0; i < va_md_data.num_sections; i++) {
			if (tree[i].entry.vaddr && va_md_check_overlap(entry, i))
				return -EINVAL;
		}
	}
	va_md_add_entry(entry);
	return 0;
}

static bool va_md_overflow_check(void)
{
	return va_md_data.num_sections >=
		va_md_data.total_mem_size / sizeof(struct va_md_tree_node);
}

int qcom_va_md_add_region(struct va_md_entry *entry)
{
	unsigned long flags, end;
	int ret;

	if (!entry || (!entry->vaddr == !entry->cb) || !entry->size)
		return -EINVAL;
	if (entry->vaddr && check_add_overflow(entry->vaddr,
					       (unsigned long)entry->size - 1, &end))
		return -EINVAL;
	if (!READ_ONCE(va_md_data.collecting) ||
	    raw_smp_processor_id() != READ_ONCE(va_md_data.capture_cpu))
		return -EINVAL;
	raw_spin_lock_irqsave(&va_md_region_lock, flags);
	if (!va_md_data.collecting)
		ret = -EINVAL;
	else if (va_md_data.num_sections >= SHN_LORESERVE - 3)
		ret = -ENOSPC;
	else if (va_md_overflow_check())
		ret = -ENOMEM;
	else
		ret = va_md_tree_insert(entry);
	raw_spin_unlock_irqrestore(&va_md_region_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_va_md_add_region);
static void qcom_va_md_minidump_registration(void)
{
	strscpy(va_md_data.md_entry.name, "KVA_DUMP", sizeof(va_md_data.md_entry.name));
	va_md_data.md_entry.virt_addr = va_md_data.elf.ehdr;
	va_md_data.md_entry.phys_addr = va_md_data.mem_phys_addr +
				      (va_md_data.elf.ehdr - va_md_data.elf_mem);
	va_md_data.md_entry.size = va_md_data.elf_size;
	va_md_data.va_md_minidump_reg =
		msm_minidump_add_region(&va_md_data.md_entry) >= 0;
	if (!va_md_data.va_md_minidump_reg)
		pr_err("Failed to register VA minidump region\n");
}

static inline unsigned long set_sec_name(struct elfhdr *ehdr, const char *name)
{
	char *strtab = elf_str_table(ehdr);
	size_t len = strlen(name) + 1;
	unsigned long idx = va_md_data.str_tbl_idx;

	if (!strtab || idx > va_md_data.elf.str_tbl_size ||
	    len > va_md_data.elf.str_tbl_size - idx)
		return 0;
	memcpy(strtab + idx, name, len - 1);
	strtab[idx + len - 1] = '\0';
	va_md_data.str_tbl_idx += len;
	return idx;
}


static void qcom_va_add_elf_hdr(void)
{
	struct elfhdr *ehdr = (struct elfhdr *)va_md_data.elf.ehdr;
	unsigned long phdr_off;

	phdr_off = sizeof(*ehdr) + (sizeof(struct elf_shdr) * va_md_data.elf.shdr_cnt);

	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELF_CLASS;
	ehdr->e_ident[EI_DATA] = ELF_DATA;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELF_OSABI;
	ehdr->e_type = ET_CORE;
	ehdr->e_machine  = ELF_ARCH;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_shoff = sizeof(*ehdr);
	ehdr->e_shentsize = sizeof(struct elf_shdr);
	ehdr->e_shstrndx = 1;
	ehdr->e_phentsize = sizeof(struct elf_phdr);
	ehdr->e_phoff = phdr_off;
}

static void qcom_va_add_hdrs(void)
{
	struct elf_shdr *shdr;
	struct elf_phdr *phdr;
	unsigned long strtbl_off, offset, i;
	struct elfhdr *ehdr = (struct elfhdr *)va_md_data.elf.ehdr;
	struct va_md_tree_node *arr = (struct va_md_tree_node *)va_md_data.elf_mem;

	strtbl_off = ehdr->e_phoff + (sizeof(*phdr) * va_md_data.elf.phdr_cnt);

	/* First section header is NULL */
	shdr = elf_section(ehdr, ehdr->e_shnum);
	ehdr->e_shnum++;

	/* String table section */
	va_md_data.str_tbl_idx = 1;
	shdr = elf_section(ehdr, ehdr->e_shnum);
	ehdr->e_shnum++;

	shdr->sh_type = SHT_STRTAB;
	shdr->sh_offset = strtbl_off;
	shdr->sh_name = set_sec_name(ehdr, "STR_TBL");
	shdr->sh_size = va_md_data.elf.str_tbl_size;

	offset = strtbl_off + va_md_data.elf.str_tbl_size;
	for (i = 0; i < (va_md_data.elf.shdr_cnt - 2); i++) {
		/* section header */
		shdr = elf_section(ehdr, ehdr->e_shnum);
		shdr->sh_type = SHT_PROGBITS;
		shdr->sh_name = set_sec_name(ehdr, (const char *)arr[i].entry.owner);
		shdr->sh_size = arr[i].entry.size;
		shdr->sh_flags = SHF_WRITE;
		shdr->sh_offset = offset;

		/* program header */
		phdr = elf_program(ehdr, ehdr->e_phnum);
		phdr->p_type = PT_LOAD;
		phdr->p_offset = offset;
		phdr->p_filesz = phdr->p_memsz = arr[i].entry.size;
		phdr->p_flags = PF_R | PF_W;

		if (arr[i].entry.vaddr) {
			shdr->sh_addr =  phdr->p_vaddr = arr[i].entry.vaddr;
			if (copy_from_kernel_nofault((void *)(va_md_data.elf.ehdr + offset),
						     (void *)shdr->sh_addr, shdr->sh_size))
				memset((void *)(va_md_data.elf.ehdr + offset), 0, shdr->sh_size);
		} else {
			shdr->sh_addr =  phdr->p_vaddr = va_md_data.elf.ehdr + offset;
			arr[i].entry.cb((void *)(va_md_data.elf.ehdr + offset),
				shdr->sh_size);
		}

		offset += shdr->sh_size;
		ehdr->e_shnum++;
		ehdr->e_phnum++;
	}
}

static int qcom_va_md_calc_size(unsigned int index)
{
	struct va_md_tree_node *arr = (struct va_md_tree_node *)va_md_data.elf_mem;
	size_t size, total;
	unsigned int len = strnlen((const char *)arr[index].entry.owner, MAX_OWNER_STRING);

	size = sizeof(struct elf_shdr) + sizeof(struct elf_phdr) + len + 1;
	if (check_add_overflow(size, (size_t)arr[index].entry.size, &size) ||
	    check_add_overflow(va_md_data.calc_size, size, &total) ||
	    total > SIZE_MAX - 3 || ALIGN(total, 4) > va_md_data.total_mem_size)
		return -ENOSPC;
	va_md_data.calc_size = total;
	va_md_data.elf.shdr_cnt++;
	va_md_data.elf.phdr_cnt++;
	va_md_data.elf.pload_size += arr[index].entry.size;
	va_md_data.elf.str_tbl_size += len + 1;
	return 0;
}

static int qcom_va_md_calc_elf_size(void)
{
	size_t nodes_size, base;
	unsigned int i;
	int ret;

	nodes_size = (size_t)va_md_data.num_sections * sizeof(struct va_md_tree_node);
	base = sizeof(struct elfhdr) + 2 * sizeof(struct elf_shdr) + sizeof("STR_TBL") + 1;
	if (nodes_size > va_md_data.total_mem_size ||
	    base > va_md_data.total_mem_size - nodes_size)
		return -ENOSPC;
	va_md_data.elf.ehdr = va_md_data.elf_mem + nodes_size;
	va_md_data.elf.shdr_cnt = 2;
	va_md_data.elf.phdr_cnt = 0;
	va_md_data.elf.pload_size = 0;
	va_md_data.elf.str_tbl_size = sizeof("STR_TBL") + 1;
	va_md_data.calc_size = nodes_size + base;
	for (i = 0; i < va_md_data.num_sections; i++) {
		ret = qcom_va_md_calc_size(i);
		if (ret)
			break;
	}
	if (!va_md_data.elf.phdr_cnt)
		return -ENOSPC;
	va_md_data.elf_size = ALIGN(va_md_data.calc_size, 4) - nodes_size;
	return 0;
}

static int qcom_va_md_panic_handler(struct notifier_block *this,
				  unsigned long event, void *ptr)
{
	struct va_md_s_data *sdata;
	unsigned long flags;

	if (!qcom_va_md_enabled() || READ_ONCE(va_md_data.in_oops_handler))
		return NOTIFY_DONE;
	raw_spin_lock_irqsave(&va_md_region_lock, flags);
	if (va_md_data.in_oops_handler || va_md_data.va_md_minidump_reg) {
		raw_spin_unlock_irqrestore(&va_md_region_lock, flags);
		return NOTIFY_DONE;
	}
	va_md_data.in_oops_handler = true;
	va_md_data.num_sections = 0;
	va_md_data.capture_cpu = raw_smp_processor_id();
	WRITE_ONCE(va_md_data.collecting, true);
	raw_spin_unlock_irqrestore(&va_md_region_lock, flags);
	/* The outer panic atomic notifier's RCU section also spans ELF copying.
	 * Unregister therefore drains users of regions submitted by its callback.
	 */
	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &va_md_data.va_md_list, va_md_s_list) {
		if (READ_ONCE(sdata->enable))
			atomic_notifier_call_chain(&sdata->va_md_s_notif_list, 0, NULL);
	}
	rcu_read_unlock();
	raw_spin_lock_irqsave(&va_md_region_lock, flags);
	WRITE_ONCE(va_md_data.collecting, false);
	raw_spin_unlock_irqrestore(&va_md_region_lock, flags);
	if (!va_md_data.num_sections || qcom_va_md_calc_elf_size())
		return NOTIFY_DONE;
	memset((void *)va_md_data.elf.ehdr, 0, va_md_data.elf_size);
	qcom_va_md_minidump_registration();
	return NOTIFY_DONE;
}

static int qcom_va_md_elf_panic_handler(struct notifier_block *this,
				      unsigned long event, void *ptr)
{
	if (!READ_ONCE(va_md_data.in_oops_handler) ||
	    raw_smp_processor_id() != READ_ONCE(va_md_data.capture_cpu))
		return NOTIFY_DONE;
	if (va_md_data.va_md_minidump_reg) {
		qcom_va_add_elf_hdr();
		qcom_va_add_hdrs();
	}
	WRITE_ONCE(va_md_data.in_oops_handler, false);
	return NOTIFY_DONE;
}


static struct notifier_block qcom_va_md_panic_blk = {
	.notifier_call = qcom_va_md_panic_handler,
	.priority = INT_MAX - 4,
};

static struct notifier_block qcom_va_md_elf_panic_blk = {
	.notifier_call = qcom_va_md_elf_panic_handler,
	.priority = INT_MAX - 5,
};
static int qcom_va_md_reserve_mem(struct device *dev)
{
	struct device_node *node;
	struct reserved_mem *rmem;
	int ret;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node)
		return -EINVAL;
	rmem = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!rmem || !rmem->size || rmem->size > UINT_MAX ||
	    rmem->size < sizeof(struct va_md_tree_node) + sizeof(struct elfhdr) +
			 3 * sizeof(struct elf_shdr) + sizeof(struct elf_phdr) + MAX_OWNER_STRING)
		return -EINVAL;
	ret = of_reserved_mem_device_init_by_idx(dev, dev->of_node, 0);
	if (!ret)
		va_md_data.total_mem_size = rmem->size;
	return ret;
}

static void qcom_va_md_driver_remove(struct platform_device *pdev)
{
	struct va_md_s_data *sdata, *tmp;
	struct notifier_block_list *nbl, *next;
	bool keep_memory = false;

	mutex_lock(&va_md_lock);
	/* Withdraw readiness before draining panic and client callbacks. */
	smp_store_release(&va_md_data.va_md_init, false);
	/* These unregister calls drain the complete outer panic RCU traversal. */
	atomic_notifier_chain_unregister(&panic_notifier_list, &qcom_va_md_panic_blk);
	atomic_notifier_chain_unregister(&panic_notifier_list, &qcom_va_md_elf_panic_blk);
	list_for_each_entry_safe(sdata, tmp, &va_md_data.va_md_list, va_md_s_list) {
		list_del_rcu(&sdata->va_md_s_list);
		synchronize_rcu();
		list_for_each_entry_safe(nbl, next, &sdata->va_md_s_nb_list, nb_list) {
			atomic_notifier_chain_unregister(&sdata->va_md_s_notif_list, &nbl->nb);
			list_del(&nbl->nb_list);
			kfree(nbl);
		}
		sysfs_remove_group(&sdata->s_kobj, &va_md_s_attr_group);
		kobject_put(&sdata->s_kobj);
	}
	kset_unregister(va_md_data.va_md_kset);
	if (va_md_data.va_md_minidump_reg &&
	    msm_minidump_remove_region(&va_md_data.md_entry) < 0) {
		/* Retain a still-advertised physical buffer instead of freeing it. */
		dev_err(&pdev->dev, "Retaining VA dump memory after withdrawal failure\n");
		keep_memory = true;
	}
	if (!keep_memory) {
		dma_free_coherent(&pdev->dev, va_md_data.total_mem_size,
				  (void *)va_md_data.elf_mem, va_md_data.dma_handle);
		of_reserved_mem_device_release(&pdev->dev);
		memset(&va_md_data, 0, sizeof(va_md_data));
	}
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&va_md_lock);
}

static int qcom_va_md_driver_probe(struct platform_device *pdev)
{
	void *vaddr;
	int ret;

	mutex_lock(&va_md_lock);
	if (va_md_data.elf_mem) {
		ret = -EBUSY;
		goto out;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto out;
	ret = qcom_va_md_reserve_mem(&pdev->dev);
	if (ret)
		goto out;
	vaddr = dma_alloc_coherent(&pdev->dev, va_md_data.total_mem_size,
				   &va_md_data.dma_handle, GFP_KERNEL);
	if (!vaddr) {
		ret = -ENOMEM;
		goto release_memory;
	}
	/* Keep ownership of the coherent allocation for the entire binding. */
	va_md_data.elf_mem = (unsigned long)vaddr;
	va_md_data.mem_phys_addr = dma_to_phys(&pdev->dev, va_md_data.dma_handle);
	INIT_LIST_HEAD(&va_md_data.va_md_list);
	va_md_data.va_md_kset = kset_create_and_add("va-minidump", NULL, kernel_kobj);
	if (!va_md_data.va_md_kset) {
		ret = -ENOMEM;
		goto free_buffer;
	}
	ret = atomic_notifier_chain_register(&panic_notifier_list, &qcom_va_md_panic_blk);
	if (ret)
		goto unregister_kset;
	ret = atomic_notifier_chain_register(&panic_notifier_list, &qcom_va_md_elf_panic_blk);
	if (ret)
		goto unregister_panic;
	platform_set_drvdata(pdev, &va_md_data);
	/* Publish only after DMA memory, sysfs and both panic hooks are ready. */
	smp_store_release(&va_md_data.va_md_init, true);
	ret = 0;
	goto out;
unregister_panic:
	atomic_notifier_chain_unregister(&panic_notifier_list, &qcom_va_md_panic_blk);
unregister_kset:
	kset_unregister(va_md_data.va_md_kset);
free_buffer:
	dma_free_coherent(&pdev->dev, va_md_data.total_mem_size, vaddr,
			  va_md_data.dma_handle);
release_memory:
	of_reserved_mem_device_release(&pdev->dev);
	memset(&va_md_data, 0, sizeof(va_md_data));
out:
	mutex_unlock(&va_md_lock);
	return ret;
}


static const struct of_device_id qcom_va_md_of_match[] = {
	{.compatible = "qcom,va-minidump"},
	{}
};

MODULE_DEVICE_TABLE(of, qcom_va_md_of_match);

static struct platform_driver qcom_va_md_driver = {
	.driver = {
		   .name = "qcom-va-minidump",
		   .of_match_table = qcom_va_md_of_match,
		   },
	.probe = qcom_va_md_driver_probe,
	.remove = qcom_va_md_driver_remove,
};

module_platform_driver(qcom_va_md_driver);

MODULE_DESCRIPTION("Qcom VA Minidump Driver");
MODULE_LICENSE("GPL");
