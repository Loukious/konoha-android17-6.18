// SPDX-License-Identifier: GPL-2.0-only
/*
 * MTD reboot/poweroff/long-power-key logger, including Xiaomi pmsg records.
 * Copyright (C) 2007 Nokia Corporation
 * Author: Richard Purdie <rpurdie@openedhand.com>
 *
 * Native Xiaomi event policy is intentional: the registered panic/oops
 * callback is a no-op. Sleepable shutdown and threaded power-key callbacks
 * archive synchronously, before UFS shutdown can make queued writes fail.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>

#define MTDOOPS_MAX_MTD_SIZE (16 * 1024 * 1024)
#define MTDOOPS_KERNMSG_MAGIC_v1 0x5d005d00
#define MTDOOPS_KERNMSG_MAGIC_v2 0x5d005e00
#define MTDOOPS_PMSG_SIG 0x43474244
#define MAX_CMDLINE_PARAM_LEN 256

/* The provider serializes these with threaded raw-notifier dispatch. */
extern int pwrkey_register_irq_notifier(struct notifier_block *nb);
extern int pwrkey_unregister_irq_notifier(struct notifier_block *nb);

static DEFINE_MUTEX(mtdoops_lock);
static unsigned long record_size = 4096;
module_param(record_size, ulong, 0400);
MODULE_PARM_DESC(record_size, "record size in bytes (default 4096)");
static char mtddev[80];
module_param_string(mtddev, mtddev, sizeof(mtddev), 0400);
MODULE_PARM_DESC(mtddev, "name or index of the MTD device");
static int dump_oops = 1;
module_param(dump_oops, int, 0600);
MODULE_PARM_DESC(dump_oops, "native oops policy parameter (default 1)");
static unsigned long lkmsg_record_size = 512 * 1024;
static char build_fingerprint[MAX_CMDLINE_PARAM_LEN];
static int boot_mode;
static int boot_index;

/* Keep native writable metadata parameters without racing archival. */
static int mtdoops_set_int(const char *value, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&mtdoops_lock);
	ret = param_set_int(value, kp);
	mutex_unlock(&mtdoops_lock);
	return ret;
}

static int mtdoops_get_int(char *value, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&mtdoops_lock);
	ret = param_get_int(value, kp);
	mutex_unlock(&mtdoops_lock);
	return ret;
}

static int mtdoops_set_fingerprint(const char *value,
				 const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&mtdoops_lock);
	ret = param_set_copystring(value, kp);
	mutex_unlock(&mtdoops_lock);
	return ret;
}

static int mtdoops_get_fingerprint(char *value, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&mtdoops_lock);
	ret = param_get_string(value, kp);
	mutex_unlock(&mtdoops_lock);
	return ret;
}

static const struct kernel_param_ops mtdoops_int_ops = {
	.set = mtdoops_set_int, .get = mtdoops_get_int,
};
static const struct kernel_param_ops mtdoops_fingerprint_ops = {
	.set = mtdoops_set_fingerprint, .get = mtdoops_get_fingerprint,
};
static struct kparam_string fingerprint_param = {
	.maxlen = sizeof(build_fingerprint), .string = build_fingerprint,
};
module_param_cb(boot_mode, &mtdoops_int_ops, &boot_mode, 0600);
module_param_cb(boot_index, &mtdoops_int_ops, &boot_index, 0600);
module_param_cb(fingerprint, &mtdoops_fingerprint_ops, &fingerprint_param, 0644);

enum mtd_dump_reason {
	MTD_DUMP_UNDEF, MTD_DUMP_PANIC, MTD_DUMP_OOPS, MTD_DUMP_EMERG,
	MTD_DUMP_SHUTDOWN, MTD_DUMP_RESTART, MTD_DUMP_POWEROFF,
	MTD_DUMP_LONG_PRESS, MTD_DUMP_MAX,
};

static const char * const kdump_reason[] = {
	"Unknown", "Kernel Panic", "Oops!", "Emerg", "Shut Down",
	"Restart", "PowerOff", "Long Press",
};

/* Standard v2 ABI: payload starts after all 16 bytes, never at byte 8. */
struct mtdoops_hdr {
	u32 seq;
	u32 magic;
	ktime_t timestamp;
} __packed;

struct pmsg_buffer_hdr {
	u32 sig;
	atomic_t start;
	atomic_t size;
	u8 data[];
};

struct mtdoops_context {
	struct kmsg_dumper dump;
	struct notifier_block reboot_nb;
	struct notifier_block pwrkey_nb;
	struct mtd_info *mtd;
	int mtd_index;
	unsigned int oops_pages;
	unsigned int nextpage;
	u32 nextcount;
	/* Dirtiness is conservative; it does not mean a write committed. */
	unsigned long *dirty;
	u8 *oops_buf;
	struct platform_device *pmsg_dev;
	void *pmsg_vaddr;
	struct pmsg_buffer_hdr *pmsg;
	size_t pmsg_capacity;
	bool stopping;
};

static struct mtdoops_context oops_cxt;

static u32 mtdoops_next_seq(u32 seq)
{
	seq++;
	return seq == U32_MAX ? 0 : seq;
}

static bool mtdoops_seq_after(u32 seq, u32 previous)
{
	return (s32)(seq - previous) > 0;
}

static bool mtdoops_record_size_valid(unsigned long size)
{
	return size >= 4096 && !(size & 4095) &&
	       size <= MTDOOPS_MAX_MTD_SIZE / 2;
}

static bool mtdoops_geometry_valid(struct mtd_info *mtd)
{
	return mtdoops_record_size_valid(record_size) && mtd->erasesize &&
	       mtd->writesize && mtd->writesize <= sizeof(u32) &&
	       !(sizeof(u32) % mtd->writesize) && !mtd->numeraseregions &&
	       (mtd->flags & MTD_WRITEABLE) &&
	       mtd->size <= MTDOOPS_MAX_MTD_SIZE &&
	       mtd->size >= (u64)mtd->erasesize * 2 &&
	       mtd->erasesize >= record_size &&
	       !(mtd->erasesize % record_size) &&
	       !(mtd->size % mtd->erasesize) &&
	       !(record_size % mtd->writesize);
}

/* Read-only attach, including dirty records with an erased-looking header. */
static int mtdoops_find_next(struct mtdoops_context *cxt)
{
	bool found = false;
	u32 newest = 0;
	unsigned int newest_page = 0, page;
	struct mtdoops_hdr hdr;
	size_t retlen;
	int ret;

	for (page = 0; page < cxt->oops_pages; page++) {
		set_bit(page, cxt->dirty);
		ret = mtd_block_isbad(cxt->mtd, (u64)page * record_size);
		if (ret < 0)
			return ret;
		if (ret)
			continue;
		retlen = 0;
		ret = mtd_read(cxt->mtd, (u64)page * record_size,
			       record_size, &retlen, cxt->oops_buf);
		if (retlen != record_size || (ret < 0 && !mtd_is_bitflip(ret)))
			return ret < 0 ? ret : -EIO;
		if (!memchr_inv(cxt->oops_buf, 0xff, record_size))
			clear_bit(page, cxt->dirty);
		memcpy(&hdr, cxt->oops_buf, sizeof(hdr));
		if (hdr.seq == U32_MAX ||
		    (hdr.magic != MTDOOPS_KERNMSG_MAGIC_v1 &&
		     hdr.magic != MTDOOPS_KERNMSG_MAGIC_v2))
			continue;
		if (!found || mtdoops_seq_after(hdr.seq, newest)) {
			found = true;
			newest = hdr.seq;
			newest_page = page;
		}
	}
	cxt->nextpage = found ? (newest_page + 1) % cxt->oops_pages : 0;
	cxt->nextcount = found ? mtdoops_next_seq(newest) : 1;
	return 0;
}

/* Called only for an actual archival event, with the logger mutex held. */
static int mtdoops_prepare_slot(struct mtdoops_context *cxt)
{
	unsigned int per_block = cxt->mtd->erasesize / record_size;
	unsigned int blocks = cxt->oops_pages / per_block;
	unsigned int tried = 0, page, i;
	struct erase_info erase = { 0 };
	int ret, retry;

	while (tried++ <= blocks) {
		page = cxt->nextpage;
		ret = mtd_block_isbad(cxt->mtd, (u64)page * record_size);
		if (ret < 0)
			return ret;
		if (ret || (test_bit(page, cxt->dirty) && page % per_block)) {
			cxt->nextpage = ((page / per_block + 1) * per_block) %
					cxt->oops_pages;
			continue;
		}
		if (!test_bit(page, cxt->dirty))
			return 0;
		erase.addr = (u64)page * record_size;
		erase.len = cxt->mtd->erasesize;
		for (retry = 0; retry < 3; retry++) {
			ret = mtd_erase(cxt->mtd, &erase);
			if (!ret)
				break;
		}
		if (ret)
			return ret;
		for (i = page; i < page + per_block; i++)
			clear_bit(i, cxt->dirty);
		return 0;
	}
	return -ENOSPC;
}

static size_t mtdoops_add_reason(char *dst, size_t capacity,
			       enum mtd_dump_reason reason, u32 seq)
{
	struct timespec64 now;
	struct tm ts;
	const char *mode = "unknown";

	if ((unsigned int)reason >= ARRAY_SIZE(kdump_reason))
		reason = MTD_DUMP_UNDEF;
	if (boot_mode == 0)
		mode = "normal";
	else if (boot_mode == 1)
		mode = "recovery";
	else if (boot_mode == 2)
		mode = "poweroff_charger";
	ktime_get_coarse_real_ts64(&now);
	time64_to_tm(now.tv_sec, 8 * 60 * 60, &ts);
	return scnprintf(dst, capacity,
		"\n ---mtdoops report start--- \n\n```\n## Boot_Index: %d\n"
		"## Oops_Index: %u\n### Build: %s\n## REASON: %s\n"
		"#### LOG TYPE:LAST KMSG\n## BOOT MODE:%s\n"
		"##### %04ld-%02d-%02d %02d:%02d:%02d\n```c\n",
		boot_index, seq, build_fingerprint, kdump_reason[reason], mode,
		ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, ts.tm_hour,
		ts.tm_min, ts.tm_sec);
}

static size_t mtdoops_add_pmsg_head(char *dst, size_t capacity)
{
	struct timespec64 now;
	struct tm ts;

	ktime_get_coarse_real_ts64(&now);
	time64_to_tm(now.tv_sec, 8 * 60 * 60, &ts);
	return scnprintf(dst, capacity,
		"\n```\n#### LOG TYPE:LAST LOGCAT\n"
		"#####%04ld-%02d-%02d %02d:%02d:%02d\n```\n",
		ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, ts.tm_hour,
		ts.tm_min, ts.tm_sec);
}

/*
 * The producer's start is the next write position. A partially filled ring
 * must have start == size; a full ring may have any start below capacity.
 * Return the newest suffix in chronological order, never parity/header bytes.
 */
static int mtdoops_pmsg_range(u32 sig, int start, int size, size_t capacity,
			     size_t limit, size_t *first, size_t *length,
			     size_t *first_length)
{
	size_t end, count;

	if (sig != MTDOOPS_PMSG_SIG || !capacity || capacity > INT_MAX ||
	    start < 0 || size < 0 || (size_t)size > capacity ||
	    (size_t)start > (size_t)size ||
	    ((size_t)size < capacity && start != size) ||
	    ((size_t)size == capacity && (size_t)start >= capacity))
		return -EINVAL;
	count = min_t(size_t, size, limit);
	end = (size_t)start % capacity;
	*first = end >= count ? end - count : capacity - (count - end);
	*length = count;
	*first_length = min(count, capacity - *first);
	return 0;
}

static size_t mtdoops_copy_pmsg(struct mtdoops_context *cxt, u8 *dst,
			      size_t capacity)
{
	struct pmsg_buffer_hdr *hdr = cxt->pmsg;
	size_t first, count, first_count;
	u32 sig;
	int start, size, attempt;

	if (!hdr || !capacity)
		return 0;
	for (attempt = 0; attempt < 3; attempt++) {
		sig = READ_ONCE(hdr->sig);
		start = atomic_read(&hdr->start);
		size = atomic_read(&hdr->size);
		if (mtdoops_pmsg_range(sig, start, size, cxt->pmsg_capacity,
				       capacity, &first, &count, &first_count))
			return 0;
		memcpy(dst, hdr->data + first, first_count);
		memcpy(dst + first_count, hdr->data, count - first_count);
		/* Header stability is best effort, not a coherent payload guarantee. */
		smp_rmb();
		if (sig == READ_ONCE(hdr->sig) &&
		    start == atomic_read(&hdr->start) &&
		    size == atomic_read(&hdr->size))
			return count;
	}
	/* A failed snapshot must not expose bytes from an earlier attempt. */
	memset(dst, 0xff, capacity);
	return 0;
}

static void mtdoops_do_dump(enum mtd_dump_reason reason)
{
	struct mtdoops_context *cxt = &oops_cxt;
	struct mtdoops_hdr hdr;
	struct kmsg_dump_iter iter;
	u32 magic = MTDOOPS_KERNMSG_MAGIC_v2;
	u64 record_offset;
	size_t offset, limit, len = 0, retlen = 0, pmsg_head, pmsg_len;
	int ret;

	if (reason != MTD_DUMP_RESTART && reason != MTD_DUMP_POWEROFF &&
	    reason != MTD_DUMP_LONG_PRESS)
		return;
	mutex_lock(&mtdoops_lock);
	if (cxt->stopping || !cxt->mtd)
		goto out;
	ret = mtdoops_prepare_slot(cxt);
	if (ret) {
		pr_err("cannot prepare archival slot: %d\n", ret);
		goto out;
	}
	memset(cxt->oops_buf, 0xff, record_size);
	hdr.seq = cxt->nextcount;
	/* Publish magic separately: a partial payload must remain uncommitted. */
	hdr.magic = U32_MAX;
	hdr.timestamp = ktime_get_real();
	memcpy(cxt->oops_buf, &hdr, sizeof(hdr));
	offset = sizeof(hdr);
	offset += mtdoops_add_reason((char *)cxt->oops_buf + offset,
				    record_size - offset, reason, hdr.seq);
	limit = min_t(size_t, record_size, lkmsg_record_size);
	if (offset < limit) {
		kmsg_dump_rewind(&iter);
		kmsg_dump_get_buffer(&iter, true, (char *)cxt->oops_buf + offset,
				     limit - offset, &len);
		/* Treat an impossible producer length as failure, never an offset. */
		if (len > limit - offset)
			len = 0;
		offset += len;
	}
	if (cxt->pmsg && offset < record_size) {
		pmsg_head = mtdoops_add_pmsg_head((char *)cxt->oops_buf + offset,
						record_size - offset);
		pmsg_len = mtdoops_copy_pmsg(cxt, cxt->oops_buf + offset + pmsg_head,
					   record_size - offset - pmsg_head);
		if (!pmsg_len)
			memset(cxt->oops_buf + offset, 0xff, record_size - offset);
	}
	record_offset = (u64)cxt->nextpage * record_size;
	ret = mtd_write(cxt->mtd, record_offset,
			record_size, &retlen, cxt->oops_buf);
	set_bit(cxt->nextpage, cxt->dirty);
	if (ret || retlen != record_size) {
		pr_err("archival write failed at page %u (%zu/%lu): %d\n",
		       cxt->nextpage, retlen, record_size, ret);
		goto out;
	}
	mtd_sync(cxt->mtd);
	retlen = 0;
	ret = mtd_write(cxt->mtd,
			record_offset + offsetof(struct mtdoops_hdr, magic),
			sizeof(magic), &retlen, (const u8 *)&magic);
	if (ret || retlen != sizeof(magic)) {
		pr_err("archival commit failed at page %u (%zu/%zu): %d\n",
		       cxt->nextpage, retlen, sizeof(magic), ret);
		goto out;
	}
	mtd_sync(cxt->mtd);
	pr_info("archived %s, sequence %u, page %u\n",
		kdump_reason[reason], cxt->nextcount, cxt->nextpage);
	/* Each successful event gets a record; failures never commit sequence. */
	cxt->nextpage = (cxt->nextpage + 1) % cxt->oops_pages;
	cxt->nextcount = mtdoops_next_seq(cxt->nextcount);
out:
	mutex_unlock(&mtdoops_lock);
}

static int mtdoops_reboot(struct notifier_block *nb, unsigned long event,
			  void *ptr)
{
	if (event == SYS_RESTART)
		mtdoops_do_dump(MTD_DUMP_RESTART);
	else if (event == SYS_POWER_OFF)
		mtdoops_do_dump(MTD_DUMP_POWEROFF);
	return NOTIFY_OK;
}

static int mtdoops_long_press(struct notifier_block *nb, unsigned long event,
			    void *ptr)
{
	mtdoops_do_dump(MTD_DUMP_LONG_PRESS);
	return NOTIFY_DONE;
}

static void mtdoops_do_null(struct kmsg_dumper *dumper,
			    struct kmsg_dump_detail *detail)
{
	/* Preserve the native registered panic/oops callback's no-op policy. */
}

/*
 * MTD notification holds mtd_table_mutex. Lifetime is bounded by notification:
 * removal cannot finish while a synchronous operation holds mtdoops_lock.
 * Do not take get/put_mtd_device (or unregister event callbacks) here: those
 * paths can invert table/notifier/logger lock order and deadlock removal.
 */
static void mtdoops_notify_add(struct mtd_info *mtd)
{
	struct mtdoops_context *cxt = &oops_cxt;
	int ret;

	if ((cxt->mtd_index < 0 && strcmp(mtd->name, mtddev)) ||
	    (cxt->mtd_index >= 0 && mtd->index != cxt->mtd_index))
		return;
	mutex_lock(&mtdoops_lock);
	if (cxt->stopping || cxt->mtd)
		goto out;
	if (!mtdoops_geometry_valid(mtd)) {
		pr_err("mtd%d has unsupported archival geometry\n", mtd->index);
		goto out;
	}
	cxt->oops_pages = div_u64(mtd->size, record_size);
	cxt->dirty = bitmap_zalloc(cxt->oops_pages, GFP_KERNEL);
	if (!cxt->dirty)
		goto out;
	cxt->mtd = mtd;
	ret = mtdoops_find_next(cxt);
	if (ret) {
		pr_err("cannot scan mtd%d: %d\n", mtd->index, ret);
		cxt->mtd = NULL;
		bitmap_free(cxt->dirty);
		cxt->dirty = NULL;
		goto out;
	}
	pr_info("attached to mtd%d, %u records of %lu bytes (read-only scan)\n",
		mtd->index, cxt->oops_pages, record_size);
out:
	mutex_unlock(&mtdoops_lock);
}

static void mtdoops_notify_remove(struct mtd_info *mtd)
{
	struct mtdoops_context *cxt = &oops_cxt;

	mutex_lock(&mtdoops_lock);
	if (cxt->mtd == mtd) {
		cxt->mtd = NULL;
		bitmap_free(cxt->dirty);
		cxt->dirty = NULL;
		cxt->oops_pages = 0;
	}
	mutex_unlock(&mtdoops_lock);
}

static struct mtd_notifier mtdoops_notifier = {
	.add = mtdoops_notify_add, .remove = mtdoops_notify_remove,
};

static int mtdoops_read_u32(struct device_node *np, const char *name, u32 *value)
{
	*value = 0;
	if (!of_find_property(np, name, NULL))
		return 0;
	return of_property_read_u32(np, name, value);
}

/* Match the actual reserved ramoops producer, including cache type and ECC. */
static struct device_node *mtdoops_find_ramoops(struct resource *res)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	for_each_compatible_node(np, NULL, "ramoops") {
		rmem = of_reserved_mem_lookup(np);
		if (of_device_is_available(np) && rmem &&
		    rmem->base == res->start && rmem->size == resource_size(res) &&
		    !of_property_read_bool(np, "reusable") &&
		    !of_property_read_bool(np, "no-map"))
			return np;
	}
	return NULL;
}

static int mtdoops_pmsg_probe(struct platform_device *pdev)
{
	struct mtdoops_context *cxt = &oops_cxt;
	struct resource *res;
	struct device_node *producer;
	struct page **pages;
	phys_addr_t start;
	void *mapping;
	size_t page_count, capacity, raw_capacity, ecc_blocks, ecc_total;
	u32 console, pmsg, producer_pmsg, producer_console, ftrace, ecc, memtype;
	unsigned int i;
	pgprot_t prot;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->end < res->start)
		return -EINVAL;
	if (mtdoops_read_u32(pdev->dev.of_node, "console-size", &console) ||
	    mtdoops_read_u32(pdev->dev.of_node, "pmsg-size", &pmsg) ||
	    pmsg <= sizeof(struct pmsg_buffer_hdr) || pmsg > MTDOOPS_MAX_MTD_SIZE ||
	    pmsg > resource_size(res) || console > resource_size(res) - pmsg)
		return -EINVAL;
	producer = mtdoops_find_ramoops(res);
	if (!producer)
		return -EINVAL;
	ret = -EINVAL;
	if (mtdoops_read_u32(producer, "pmsg-size", &producer_pmsg) ||
	    mtdoops_read_u32(producer, "console-size", &producer_console) ||
	    mtdoops_read_u32(producer, "ftrace-size", &ftrace) ||
	    mtdoops_read_u32(producer, "ecc-size", &ecc) ||
	    producer_pmsg != pmsg || producer_console != console || ftrace ||
	    !is_power_of_2(pmsg))
		goto put_node;
	memtype = of_property_read_bool(producer, "unbuffered") ? 1 : 0;
	if (of_find_property(producer, "mem-type", NULL) &&
	    of_property_read_u32(producer, "mem-type", &memtype))
		goto put_node;
	switch (memtype) {
	case 0:
		prot = pgprot_writecombine(PAGE_KERNEL);
		break;
	case 1:
		prot = pgprot_noncached(PAGE_KERNEL);
		break;
	case 2:
		prot = PAGE_KERNEL;
		break;
	default:
		goto put_node;
	}
	raw_capacity = pmsg - sizeof(struct pmsg_buffer_hdr);
	capacity = raw_capacity;
	/* Match persistent_ram_init_ecc's default 128-byte data blocks. */
	if (ecc) {
		if (ecc > INT_MAX || ecc >= raw_capacity)
			goto put_node;
		ecc_blocks = DIV_ROUND_UP(raw_capacity - ecc, 128 + (size_t)ecc);
		if (check_mul_overflow(ecc_blocks + 1, (size_t)ecc, &ecc_total) ||
		    ecc_total >= raw_capacity)
			goto put_node;
		capacity -= ecc_total;
	}
	if (check_add_overflow((phys_addr_t)res->start,
			       (phys_addr_t)(resource_size(res) - pmsg), &start) ||
	    !IS_ALIGNED(start, PAGE_SIZE) || !IS_ALIGNED(pmsg, PAGE_SIZE))
		goto put_node;
	page_count = pmsg / PAGE_SIZE;
	pages = kmalloc_array(page_count, sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto put_node;
	}
	for (i = 0; i < page_count; i++) {
		unsigned long pfn = PHYS_PFN(start) + i;

		if (!pfn_valid(pfn) || !PageReserved(pfn_to_page(pfn)))
			goto free_pages;
		pages[i] = pfn_to_page(pfn);
	}
	mapping = vmap(pages, page_count, VM_MAP | VM_IOREMAP, prot);
	if (!mapping) {
		ret = -ENOMEM;
		goto free_pages;
	}
	mutex_lock(&mtdoops_lock);
	if (cxt->stopping || cxt->pmsg_dev) {
		mutex_unlock(&mtdoops_lock);
		vunmap(mapping);
		ret = -EBUSY;
		goto free_pages;
	}
	cxt->pmsg_dev = pdev;
	cxt->pmsg_vaddr = mapping;
	cxt->pmsg = mapping;
	cxt->pmsg_capacity = capacity;
	mutex_unlock(&mtdoops_lock);
	ret = 0;
	pr_info("mapped verified reserved pmsg (%zu data bytes, ecc=%u)\n",
		capacity, ecc);
free_pages:
	kfree(pages);
put_node:
	of_node_put(producer);
	return ret;
}

static void mtdoops_pmsg_remove(struct platform_device *pdev)
{
	struct mtdoops_context *cxt = &oops_cxt;

	mutex_lock(&mtdoops_lock);
	if (cxt->pmsg_dev == pdev) {
		cxt->pmsg = NULL;
		cxt->pmsg_dev = NULL;
		cxt->pmsg_capacity = 0;
		vunmap(cxt->pmsg_vaddr);
		cxt->pmsg_vaddr = NULL;
	}
	mutex_unlock(&mtdoops_lock);
}

static const struct of_device_id mtdoops_dt_match[] = {
	{ .compatible = "xiaomi,mtdoops_pmsg" }, {},
};
MODULE_DEVICE_TABLE(of, mtdoops_dt_match);

static struct platform_driver mtdoops_pmsg_driver = {
	.probe = mtdoops_pmsg_probe, .remove = mtdoops_pmsg_remove,
	.driver = { .name = "mtdoops_pmsg", .of_match_table = mtdoops_dt_match },
};

static int __init mtdoops_init(void)
{
	struct mtdoops_context *cxt = &oops_cxt;
	int ret, index;

	BUILD_BUG_ON(sizeof(struct mtdoops_hdr) != 16);
	BUILD_BUG_ON(sizeof(struct pmsg_buffer_hdr) != 12);
	if (!mtddev[0] || !mtdoops_record_size_valid(record_size))
		return -EINVAL;
	cxt->mtd_index = -1;
	if (!kstrtoint(mtddev, 0, &index) && index >= 0)
		cxt->mtd_index = index;
	cxt->stopping = false;
	cxt->oops_buf = kvmalloc(record_size, GFP_KERNEL);
	if (!cxt->oops_buf)
		return -ENOMEM;
	ret = platform_driver_register(&mtdoops_pmsg_driver);
	if (ret)
		goto free_buffer;
	cxt->dump.max_reason = KMSG_DUMP_MAX;
	cxt->dump.dump = mtdoops_do_null;
	ret = kmsg_dump_register(&cxt->dump);
	if (ret)
		goto unregister_platform;
	cxt->reboot_nb.notifier_call = mtdoops_reboot;
	cxt->reboot_nb.priority = 255;
	ret = register_reboot_notifier(&cxt->reboot_nb);
	if (ret)
		goto unregister_dump;
	cxt->pwrkey_nb.notifier_call = mtdoops_long_press;
	cxt->pwrkey_nb.priority = 255;
	ret = pwrkey_register_irq_notifier(&cxt->pwrkey_nb);
	if (ret)
		goto unregister_reboot;
	register_mtd_user(&mtdoops_notifier);
	return 0;
unregister_reboot:
	unregister_reboot_notifier(&cxt->reboot_nb);
unregister_dump:
	kmsg_dump_unregister(&cxt->dump);
unregister_platform:
	platform_driver_unregister(&mtdoops_pmsg_driver);
free_buffer:
	kvfree(cxt->oops_buf);
	cxt->oops_buf = NULL;
	return ret;
}

static void __exit mtdoops_exit(void)
{
	struct mtdoops_context *cxt = &oops_cxt;

	mutex_lock(&mtdoops_lock);
	cxt->stopping = true;
	mutex_unlock(&mtdoops_lock);
	/* Drain callbacks outside both the logger and MTD table mutexes. */
	pwrkey_unregister_irq_notifier(&cxt->pwrkey_nb);
	unregister_reboot_notifier(&cxt->reboot_nb);
	kmsg_dump_unregister(&cxt->dump);
	unregister_mtd_user(&mtdoops_notifier);
	platform_driver_unregister(&mtdoops_pmsg_driver);
	kvfree(cxt->oops_buf);
	cxt->oops_buf = NULL;
}

module_init(mtdoops_init);
module_exit(mtdoops_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard Purdie <rpurdie@openedhand.com>");
MODULE_DESCRIPTION("Xiaomi MTD synchronous shutdown and power-key archival");
