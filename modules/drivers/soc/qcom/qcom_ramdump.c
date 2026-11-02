// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/elf.h>
#include <linux/wait.h>
#include <linux/cdev.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/overflow.h>
#include <soc/qcom/qcom_ramdump.h>
#include <linux/devcoredump.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/devcoredump.h>
#include <linux/soc/qcom/mdt_loader.h>

#define RAMDUMP_TIMEOUT 120000

#define SIZEOF_ELF_STRUCT(__xhdr) \
static inline size_t sizeof_elf_##__xhdr(unsigned char class) \
{ \
	if (class == ELFCLASS32) \
		return sizeof(struct elf32_##__xhdr); \
	else \
		return sizeof(struct elf64_##__xhdr); \
}

SIZEOF_ELF_STRUCT(phdr)
SIZEOF_ELF_STRUCT(hdr)

#define set_xhdr_property(__xhdr, arg, class, member, value) \
do { \
	if (class == ELFCLASS32) \
		((struct elf32_##__xhdr *)arg)->member = value; \
	else \
		((struct elf64_##__xhdr *)arg)->member = value; \
} while (0)

#define set_ehdr_property(arg, class, member, value) \
	set_xhdr_property(hdr, arg, class, member, value)
#define set_phdr_property(arg, class, member, value) \
	set_xhdr_property(phdr, arg, class, member, value)

#define RAMDUMP_NUM_DEVICES	256
#define RAMDUMP_NAME		"ramdump"

static struct class *ramdump_class;
static dev_t ramdump_dev;
static DEFINE_MUTEX(rd_minor_mutex);
static DEFINE_IDA(rd_minor_id);
static bool ramdump_devnode_inited;

struct ramdump_device {
	struct cdev cdev;
	struct device dev;
};

struct qcom_ramdump_desc {
	void *data;
	struct completion dump_done;
};

static int enable_dump_collection;
module_param(enable_dump_collection, int, 0644);

bool dump_enabled(void)
{
	return enable_dump_collection;
}
EXPORT_SYMBOL(dump_enabled);

static ssize_t qcom_devcd_readv(char *buffer, loff_t offset, size_t count,
			   void *data, size_t datalen)
{
	struct qcom_ramdump_desc *desc = data;

	return memory_read_from_buffer(buffer, count, &offset, desc->data, datalen);
}

static void qcom_devcd_freev(void *data)
{
	struct qcom_ramdump_desc *desc = data;

	vfree(desc->data);
	complete_all(&desc->dump_done);
}

static int qcom_devcd_dump(struct device *dev, void *data, size_t datalen, gfp_t gfp)
{
	struct qcom_ramdump_desc desc;

	desc.data = data;
	init_completion(&desc.dump_done);

	dev_coredumpm(dev, THIS_MODULE, &desc, datalen, gfp, qcom_devcd_readv, qcom_devcd_freev);

	wait_for_completion(&desc.dump_done);

	return !completion_done(&desc.dump_done);
}

int qcom_dump(struct list_head *segs, struct device *dev)
{
	struct qcom_dump_segment *segment;
	void *data;
	void __iomem *ptr;
	size_t data_size = 0;
	size_t offset = 0;

	if (!segs || list_empty(segs))
		return -EINVAL;

	list_for_each_entry(segment, segs, node) {
		pr_info("Got segment size %zd\n", segment->size);
		if (check_add_overflow(data_size, segment->size, &data_size))
			return -EOVERFLOW;
	}

	data = vmalloc(data_size);
	if (!data)
		return -ENOMEM;

	list_for_each_entry(segment, segs, node) {
		if (segment->va)
			memcpy(data + offset, segment->va, segment->size);
		else {
			ptr = ioremap(segment->da, segment->size);
			if (!ptr) {
				dev_err(dev,
					"invalid coredump segment (%pad, %zu)\n",
					&segment->da, segment->size);
				memset(data + offset, 0xff, segment->size);
			} else {
				memcpy_fromio(data + offset, ptr, segment->size);
				iounmap(ptr);
			}
		}
		offset += segment->size;
	}

	return qcom_devcd_dump(dev, data, data_size, GFP_KERNEL);
}
EXPORT_SYMBOL(qcom_dump);

/* Since the elf32 and elf64 identification is identical
 * apart from the class we use elf32 by default.
 */
static void init_elf_identification(struct elf32_hdr *ehdr, unsigned char class)
{
	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = class;
	ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELFOSABI_NONE;
}

int qcom_elf_dump(struct list_head *segs, struct device *dev, unsigned char class)
{
	struct qcom_dump_segment *segment;
	void *phdr;
	void *ehdr;
	size_t data_size;
	size_t offset;
	int phnum = 0;
	void *data;
	void __iomem *ptr;


	if (!segs || list_empty(segs) ||
	    (class != ELFCLASS32 && class != ELFCLASS64))
		return -EINVAL;

	data_size = sizeof_elf_hdr(class);
	list_for_each_entry(segment, segs, node) {
		if (++phnum >= PN_XNUM ||
		    check_add_overflow(data_size, sizeof_elf_phdr(class), &data_size) ||
		    check_add_overflow(data_size, segment->size, &data_size) ||
		    (class == ELFCLASS32 &&
		     (segment->da > U32_MAX || segment->size > U32_MAX || data_size > U32_MAX)))
			return -EOVERFLOW;
	}

	data = vmalloc(data_size);
	if (!data)
		return -ENOMEM;

	pr_debug("Creating elf with size %zd\n", data_size);
	ehdr = data;

	memset(ehdr, 0, sizeof_elf_hdr(class));
	init_elf_identification(ehdr, class);
	set_ehdr_property(ehdr, class, e_type, ET_CORE);
	set_ehdr_property(ehdr, class, e_machine, EM_NONE);
	set_ehdr_property(ehdr, class, e_version, EV_CURRENT);
	set_ehdr_property(ehdr, class, e_phoff, sizeof_elf_hdr(class));
	set_ehdr_property(ehdr, class, e_ehsize, sizeof_elf_hdr(class));
	set_ehdr_property(ehdr, class, e_phentsize, sizeof_elf_phdr(class));
	set_ehdr_property(ehdr, class, e_phnum, phnum);

	phdr = data + sizeof_elf_hdr(class);
	offset = sizeof_elf_hdr(class) + sizeof_elf_phdr(class) * phnum;
	list_for_each_entry(segment, segs, node) {
		memset(phdr, 0, sizeof_elf_phdr(class));
		set_phdr_property(phdr, class, p_type, PT_LOAD);
		set_phdr_property(phdr, class, p_offset, offset);
		set_phdr_property(phdr, class, p_vaddr, segment->da);
		set_phdr_property(phdr, class, p_paddr, segment->da);
		set_phdr_property(phdr, class, p_filesz, segment->size);
		set_phdr_property(phdr, class, p_memsz, segment->size);
		set_phdr_property(phdr, class, p_flags, PF_R | PF_W | PF_X);
		set_phdr_property(phdr, class, p_align, 0);

		if (segment->va)
			memcpy(data + offset, segment->va, segment->size);
		else {
			ptr = ioremap(segment->da, segment->size);
			if (!ptr) {
				dev_err(dev,
					"invalid coredump segment (%pad, %zu)\n",
					&segment->da, segment->size);
				memset(data + offset, 0xff, segment->size);
			} else {
				memcpy_fromio(data + offset, ptr, segment->size);
				iounmap(ptr);
			}
		}

		offset += segment->size;
		phdr += sizeof_elf_phdr(class);
	}

	return qcom_devcd_dump(dev, data, data_size, GFP_KERNEL);
}
EXPORT_SYMBOL(qcom_elf_dump);

static bool qcom_ramdump_elf32_valid(const struct firmware *fw)
{
	const struct elf32_hdr *ehdr;

	if (!fw || !fw->data || fw->size < sizeof(*ehdr))
		return false;
	ehdr = (const struct elf32_hdr *)fw->data;
	return !memcmp(ehdr->e_ident, ELFMAG, SELFMAG) &&
		ehdr->e_ident[EI_CLASS] == ELFCLASS32 &&
		ehdr->e_ident[EI_DATA] == ELFDATA2LSB &&
		ehdr->e_phentsize == sizeof(struct elf32_phdr) &&
		ehdr->e_phoff >= sizeof(*ehdr) && ehdr->e_phoff <= fw->size &&
		IS_ALIGNED(ehdr->e_phoff, __alignof__(struct elf32_phdr)) &&
		ehdr->e_phnum < PN_XNUM &&
		ehdr->e_phnum <= (fw->size - ehdr->e_phoff) / sizeof(struct elf32_phdr);
}

int qcom_fw_elf_dump(struct firmware *fw, struct device *dev)
{
	const struct elf32_hdr *ehdr;
	const struct elf32_phdr *phdrs, *phdr;
	struct qcom_dump_segment *segment, *tmp;
	LIST_HEAD(head);
	int i, ret;

	if (!qcom_ramdump_elf32_valid(fw))
		return -EINVAL;
	ehdr = (const struct elf32_hdr *)fw->data;
	phdrs = (const struct elf32_phdr *)(fw->data + ehdr->e_phoff);
	for (i = 0; i < ehdr->e_phnum; i++) {
		phdr = &phdrs[i];
		if (phdr->p_type != PT_LOAD || !phdr->p_memsz ||
		    (phdr->p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH)
			continue;
		segment = kzalloc(sizeof(*segment), GFP_KERNEL);
		if (!segment) {
			ret = -ENOMEM;
			goto free_segments;
		}
		segment->da = phdr->p_paddr;
		segment->size = phdr->p_memsz;
		list_add_tail(&segment->node, &head);
	}
	ret = qcom_elf_dump(&head, dev, ELFCLASS32);
free_segments:
	list_for_each_entry_safe(segment, tmp, &head, node) {
		list_del(&segment->node);
		kfree(segment);
	}
	return ret;
}
EXPORT_SYMBOL(qcom_fw_elf_dump);

static int ramdump_devnode_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&ramdump_dev, 0, RAMDUMP_NUM_DEVICES, RAMDUMP_NAME);
	if (ret)
		return ret;
	ramdump_class = class_create(RAMDUMP_NAME);
	if (IS_ERR(ramdump_class)) {
		ret = PTR_ERR(ramdump_class);
		unregister_chrdev_region(ramdump_dev, RAMDUMP_NUM_DEVICES);
		return ret;
	}
	ramdump_devnode_inited = true;
	return 0;
}

/* Dump data is provided by devcoredump; these nodes identify the dump source. */
static const struct file_operations ramdump_fops = {
	.owner = THIS_MODULE,
};

static void ramdump_device_release(struct device *dev)
{
	struct ramdump_device *rd = container_of(dev, struct ramdump_device, dev);

	ida_free(&rd_minor_id, MINOR(dev->devt));
	kfree(rd);
	module_put(THIS_MODULE);
}

void *qcom_create_ramdump_device(const char *name, struct device *parent)
{
	struct ramdump_device *rd;
	int minor, ret;

	mutex_lock(&rd_minor_mutex);
	ret = ramdump_devnode_inited ? 0 : ramdump_devnode_init();
	mutex_unlock(&rd_minor_mutex);
	if (ret)
		return ERR_PTR(ret);

	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return ERR_PTR(-ENOMEM);
	minor = ida_alloc_range(&rd_minor_id, 0, RAMDUMP_NUM_DEVICES - 1, GFP_KERNEL);
	if (minor < 0) {
		kfree(rd);
		return ERR_PTR(minor);
	}
	/* A devcoredump child may retain this device after its creator detaches. */
	__module_get(THIS_MODULE);
	device_initialize(&rd->dev);
	rd->dev.class = ramdump_class;
	rd->dev.parent = parent;
	rd->dev.devt = MKDEV(MAJOR(ramdump_dev), minor);
	rd->dev.release = ramdump_device_release;
	dev_set_drvdata(&rd->dev, rd);
	ret = dev_set_name(&rd->dev, "%s", name);
	if (ret)
		goto put_device;
	cdev_init(&rd->cdev, &ramdump_fops);
	rd->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&rd->cdev, &rd->dev);
	if (ret)
		goto put_device;
	return &rd->dev;
put_device:
	put_device(&rd->dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(qcom_create_ramdump_device);

void qcom_destroy_ramdump_device(void *dev)
{
	struct ramdump_device *rd;

	if (IS_ERR_OR_NULL(dev))
		return;
	rd = dev_get_drvdata(dev);
	cdev_device_del(&rd->cdev, &rd->dev);
	put_device(&rd->dev);
}
EXPORT_SYMBOL(qcom_destroy_ramdump_device);

static void __exit qcom_ramdump_exit(void)
{
	if (ramdump_devnode_inited) {
		class_destroy(ramdump_class);
		unregister_chrdev_region(ramdump_dev, RAMDUMP_NUM_DEVICES);
	}
	ida_destroy(&rd_minor_id);
}
module_exit(qcom_ramdump_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Ramdump driver");
MODULE_LICENSE("GPL");

