/*
 * block2mtd.c - create an mtd from a block device
 *
 * Copyright (C) 2001,2002 Simon Evans <spse@secret.org.uk>
 * Copyright (C) 2004-2006 Joern Engel <joern@wh.fh-wedel.de>
 *
 * Licence: GPL
 *
 * Native Xiaomi numeric recovery mode and synchronous archive writes are
 * retained from SM8735 revision 10570812a4878b644e31b47c2f0bd052664cf86d.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/major.h>
#include <linux/kthread.h>
#include <linux/sched/task.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/ctype.h>

#define MTD_DEFAULT_TIMEOUT 5
#define BLOCK2MTD_PARAM_MAX_COUNT 3
#define BLOCK2MTD_NAME_MAX 80
#define BLOCK2MTD_PARAM_MAX 192
#define BLOCK2MTD_MAX_DEVICES 32

struct block2mtd_dev {
	struct list_head list;
	struct file *bdev_file;
	struct mtd_info mtd;
	struct mutex write_mutex;
};

struct device_init_info {
	char devname[BLOCK2MTD_NAME_MAX];
	u32 erase_size;
	unsigned int label;
};

struct block2mtd_retry {
	struct list_head list;
	struct device_init_info info;
	struct task_struct *task;
	int result;
};

static LIST_HEAD(blkmtd_device_list);
static LIST_HEAD(block2mtd_retry_list);
static DEFINE_MUTEX(block2mtd_devices_lock);
static struct device_init_info block2mtd_pending[BLOCK2MTD_MAX_DEVICES];
static unsigned int block2mtd_pending_count;
static unsigned int block2mtd_device_count;
static bool block2mtd_initialized;
static bool block2mtd_stopping;

static struct page *page_read(struct address_space *mapping, pgoff_t index)
{
	return read_mapping_page(mapping, index, NULL);
}

static int block2mtd_bounds(struct block2mtd_dev *dev, loff_t offset,
			   size_t len)
{
	loff_t size = bdev_nr_bytes(file_bdev(dev->bdev_file));

	if (size < 0 || size < dev->mtd.size)
		return -ENXIO;
	if (offset < 0 || (u64)offset > dev->mtd.size ||
	    len > dev->mtd.size - (u64)offset)
		return -EINVAL;
	return 0;
}

/* All bounds are checked before the first page is changed. */
static int _block2mtd_erase(struct block2mtd_dev *dev, loff_t to, size_t len)
{
	struct address_space *mapping = dev->bdev_file->f_mapping;
	pgoff_t index;
	struct page *page;
	void *address;
	size_t pages;
	int ret;

	ret = block2mtd_bounds(dev, to, len);
	if (ret)
		return ret;
	if ((to & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)))
		return -EINVAL;
	index = to >> PAGE_SHIFT;
	pages = len >> PAGE_SHIFT;
	while (pages--) {
		page = page_read(mapping, index++);
		if (IS_ERR(page))
			return PTR_ERR(page);
		lock_page(page);
		address = kmap_local_page(page);
		memset(address, 0xff, PAGE_SIZE);
		kunmap_local(address);
		set_page_dirty(page);
		unlock_page(page);
		put_page(page);
		balance_dirty_pages_ratelimited(mapping);
	}
	return 0;
}

static int block2mtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct block2mtd_dev *dev = mtd->priv;
	int ret;

	if (!instr || !mtd->erasesize || instr->addr > LLONG_MAX ||
	    instr->len > SIZE_MAX || instr->addr % mtd->erasesize ||
	    instr->len % mtd->erasesize)
		return -EINVAL;
	instr->fail_addr = MTD_FAIL_ADDR_UNKNOWN;
	mutex_lock(&dev->write_mutex);
	ret = _block2mtd_erase(dev, instr->addr, instr->len);
	if (ret)
		instr->fail_addr = instr->addr;
	mutex_unlock(&dev->write_mutex);
	return ret;
}

static int block2mtd_read(struct mtd_info *mtd, loff_t from, size_t len,
			 size_t *retlen, u_char *buf)
{
	struct block2mtd_dev *dev = mtd->priv;
	struct address_space *mapping = dev->bdev_file->f_mapping;
	pgoff_t index;
	struct page *page;
	void *address;
	size_t offset, copy;
	int ret;

	if (retlen)
		*retlen = 0;
	ret = block2mtd_bounds(dev, from, len);
	if (ret || (!buf && len))
		return ret ?: -EINVAL;
	index = from >> PAGE_SHIFT;
	offset = from & (PAGE_SIZE - 1);
	while (len) {
		copy = min_t(size_t, len, PAGE_SIZE - offset);
		page = page_read(mapping, index++);
		if (IS_ERR(page))
			return PTR_ERR(page);
		lock_page(page);
		address = kmap_local_page(page);
		memcpy(buf, address + offset, copy);
		kunmap_local(address);
		unlock_page(page);
		put_page(page);
		if (retlen)
			*retlen += copy;
		buf += copy;
		len -= copy;
		offset = 0;
	}
	return 0;
}

static int _block2mtd_write(struct block2mtd_dev *dev, const u_char *buf,
			   loff_t to, size_t len, size_t *retlen)
{
	struct address_space *mapping = dev->bdev_file->f_mapping;
	pgoff_t index;
	struct page *page;
	void *address;
	size_t offset, copy;
	bool changed;
	int ret;

	if (retlen)
		*retlen = 0;
	ret = block2mtd_bounds(dev, to, len);
	if (ret || (!buf && len))
		return ret ?: -EINVAL;
	index = to >> PAGE_SHIFT;
	offset = to & (PAGE_SIZE - 1);
	while (len) {
		copy = min_t(size_t, len, PAGE_SIZE - offset);
		page = page_read(mapping, index++);
		if (IS_ERR(page))
			return PTR_ERR(page);
		lock_page(page);
		address = kmap_local_page(page);
		changed = memcmp(address + offset, buf, copy) != 0;
		if (changed) {
			memcpy(address + offset, buf, copy);
			set_page_dirty(page);
		}
		kunmap_local(address);
		unlock_page(page);
		put_page(page);
		if (changed)
			balance_dirty_pages_ratelimited(mapping);
		if (retlen)
			*retlen += copy;
		buf += copy;
		len -= copy;
		offset = 0;
	}
	return 0;
}

static int block2mtd_write(struct mtd_info *mtd, loff_t to, size_t len,
			  size_t *retlen, const u_char *buf)
{
	struct block2mtd_dev *dev = mtd->priv;
	size_t written = 0;
	int ret, sync_ret;

	if (retlen)
		*retlen = 0;
	mutex_lock(&dev->write_mutex);
	ret = _block2mtd_write(dev, buf, to, len, &written);
	/* Native synchronous writes are retained, including partial I/O. */
	sync_ret = written ? sync_blockdev(file_bdev(dev->bdev_file)) : 0;
	mutex_unlock(&dev->write_mutex);
	if (retlen)
		*retlen = written;
	return ret ?: sync_ret;
}

static void block2mtd_sync(struct mtd_info *mtd)
{
	struct block2mtd_dev *dev = mtd->priv;
	int ret;

	mutex_lock(&dev->write_mutex);
	ret = sync_blockdev(file_bdev(dev->bdev_file));
	mutex_unlock(&dev->write_mutex);
	if (ret)
		pr_err("mtd%d: sync failed: %d\n", mtd->index, ret);
}

static void block2mtd_free_device(struct block2mtd_dev *dev)
{
	if (!dev)
		return;
	if (dev->bdev_file)
		bdev_fput(dev->bdev_file);
	kfree(dev->mtd.name);
	mutex_destroy(&dev->write_mutex);
	kfree(dev);
}

/* Caller holds block2mtd_devices_lock across registration/publication. */
static struct block2mtd_dev *add_device(const struct device_init_info *info)
{
	const blk_mode_t mode = BLK_OPEN_READ | BLK_OPEN_WRITE;
	struct block2mtd_dev *dev;
	struct block_device *bdev;
	loff_t size;
	int ret;

	if (block2mtd_stopping)
		return ERR_PTR(-ESHUTDOWN);
	if (!info || !info->devname[0] || info->label > 1 ||
	    !info->erase_size || info->erase_size % PAGE_SIZE)
		return ERR_PTR(-EINVAL);
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	mutex_init(&dev->write_mutex);
	/* A non-NULL holder claims exclusive access against filesystem users. */
	dev->bdev_file = bdev_file_open_by_path(info->devname, mode, dev, NULL);
	if (IS_ERR(dev->bdev_file)) {
		ret = PTR_ERR(dev->bdev_file);
		dev->bdev_file = NULL;
		goto fail;
	}
	bdev = file_bdev(dev->bdev_file);
	if (MAJOR(bdev->bd_dev) == MTD_BLOCK_MAJOR) {
		ret = -EINVAL;
		goto fail;
	}
	size = bdev_nr_bytes(bdev);
	if (size <= 0 || size % PAGE_SIZE || size % info->erase_size) {
		ret = -EINVAL;
		goto fail;
	}
	dev->mtd.name = kasprintf(GFP_KERNEL, "block2mtd: %s", info->devname);
	if (!dev->mtd.name) {
		ret = -ENOMEM;
		goto fail;
	}
	dev->mtd.size = size;
	dev->mtd.erasesize = info->erase_size;
	dev->mtd.writesize = 1;
	dev->mtd.writebufsize = PAGE_SIZE;
	dev->mtd.type = MTD_RAM;
	dev->mtd.flags = MTD_CAP_RAM;
	dev->mtd._erase = block2mtd_erase;
	dev->mtd._read = block2mtd_read;
	dev->mtd._write = block2mtd_write;
	dev->mtd._sync = block2mtd_sync;
	dev->mtd.priv = dev;
	dev->mtd.owner = THIS_MODULE;
	ret = mtd_device_register(&dev->mtd, NULL, 0);
	if (ret)
		goto fail;
	list_add_tail(&dev->list, &blkmtd_device_list);
	pr_info("mtd%d: [%s] erase_size=%u mode=%u\n", dev->mtd.index,
		info->devname, info->erase_size, info->label);
	return dev;
fail:
	block2mtd_free_device(dev);
	return ERR_PTR(ret);
}

/* Accept native Ki/Mi/Gi suffixes without truncation or arithmetic overflow. */
static int parse_num(size_t *number, const char *token)
{
	char digits[32];
	const char *suffix;
	size_t length, multiplier = 1;
	unsigned long value;
	int ret;

	if (!token || !*token || !number || !isdigit(*token))
		return -EINVAL;
	length = strcspn(token, "kKMG");
	if (!length || length >= sizeof(digits))
		return -ERANGE;
	memcpy(digits, token, length);
	digits[length] = '\0';
	if (strpbrk(digits, " \t\r\n\v\f"))
		return -EINVAL;
	suffix = token + length;
	if (*suffix) {
		switch (*suffix++) {
		case 'k': case 'K': multiplier = 1024; break;
		case 'M': multiplier = 1024 * 1024; break;
		case 'G': multiplier = 1024 * 1024 * 1024; break;
		default: return -EINVAL;
		}
		if (*suffix == 'i')
			suffix++;
		if (*suffix == 'B')
			suffix++;
		if (*suffix)
			return -EINVAL;
	}
	ret = kstrtoul(digits, 0, &value);
	if (ret)
		return ret;
	if (value > SIZE_MAX / multiplier)
		return -ERANGE;
	*number = value * multiplier;
	return 0;
}

static int block2mtd_parse(const char *val, struct device_init_info *info)
{
	char buffer[BLOCK2MTD_PARAM_MAX], *cursor = buffer;
	char *token[BLOCK2MTD_PARAM_MAX_COUNT];
	size_t length, erase_size = PAGE_SIZE, label = 0;
	unsigned int i;
	int ret;

	if (!val || !info)
		return -EINVAL;
	length = strnlen(val, sizeof(buffer));
	if (!length || length == sizeof(buffer))
		return -EINVAL;
	memcpy(buffer, val, length + 1);
	if (buffer[length - 1] == '\n')
		buffer[--length] = '\0';
	for (i = 0; i < BLOCK2MTD_PARAM_MAX_COUNT; i++)
		token[i] = strsep(&cursor, ",");
	if (cursor || !token[0] || !*token[0] || token[0][0] != '/' ||
	    strlen(token[0]) >= BLOCK2MTD_NAME_MAX)
		return -EINVAL;
	for (i = 0; token[0][i]; i++)
		if (isspace(token[0][i]))
			return -EINVAL;
	if (token[1] && *token[1]) {
		ret = parse_num(&erase_size, token[1]);
		if (ret)
			return ret;
	}
	if (token[2] && *token[2]) {
		/* Native third field is the numeric recovery mode, not a name. */
		ret = parse_num(&label, token[2]);
		if (ret)
			return ret;
	}
	if (erase_size < PAGE_SIZE || erase_size > U32_MAX ||
	    erase_size % PAGE_SIZE || label > 1)
		return -EINVAL;
	memset(info, 0, sizeof(*info));
	strscpy(info->devname, token[0], sizeof(info->devname));
	info->erase_size = erase_size;
	info->label = label;
	return 0;
}

static bool block2mtd_retryable(int error)
{
	return error == -ENOENT || error == -ENODEV || error == -ENXIO ||
		error == -EPROBE_DEFER;
}

static int retry_add_device_thread_func(void *data)
{
	struct block2mtd_retry *retry = data;
	struct block2mtd_dev *dev;
	unsigned int attempt, wait;
	int ret = -ENODEV;

	/* Native recovery performs two retry rounds, bounded by its timeout. */
	for (attempt = 0; attempt < 2; attempt++) {
		for (wait = 0; wait <= MTD_DEFAULT_TIMEOUT; wait++) {
			if (kthread_should_stop() || READ_ONCE(block2mtd_stopping))
				return -ESHUTDOWN;
			if (wait && msleep_interruptible(2000))
				continue;
			mutex_lock(&block2mtd_devices_lock);
			dev = add_device(&retry->info);
			mutex_unlock(&block2mtd_devices_lock);
			if (!IS_ERR(dev)) {
				retry->result = 0;
				return 0;
			}
			ret = PTR_ERR(dev);
			if (!block2mtd_retryable(ret))
				goto done;
		}
	}
done:
	retry->result = ret;
	pr_err("recovery attach %s failed: %d\n", retry->info.devname, ret);
	return ret;
}

/* Caller holds block2mtd_devices_lock; thread cannot run until publication. */
static int block2mtd_attach(const struct device_init_info *info)
{
	struct block2mtd_retry *retry;
	struct block2mtd_dev *dev;
	int ret;

	if (block2mtd_device_count >= BLOCK2MTD_MAX_DEVICES)
		return -E2BIG;
	dev = add_device(info);
	if (!IS_ERR(dev)) {
		block2mtd_device_count++;
		return 0;
	}
	ret = PTR_ERR(dev);
	if (!info->label || !block2mtd_retryable(ret))
		return ret;
	retry = kzalloc(sizeof(*retry), GFP_KERNEL);
	if (!retry)
		return -ENOMEM;
	retry->info = *info;
	retry->result = ret;
	retry->task = kthread_create(retry_add_device_thread_func, retry,
				     "block2mtd_retry");
	if (IS_ERR(retry->task)) {
		ret = PTR_ERR(retry->task);
		kfree(retry);
		return ret;
	}
	/* Keep the task valid even if it exits before module teardown. */
	get_task_struct(retry->task);
	list_add_tail(&retry->list, &block2mtd_retry_list);
	block2mtd_device_count++;
	wake_up_process(retry->task);
	return 0;
}

static int block2mtd_setup(const char *val, const struct kernel_param *kp)
{
	struct device_init_info info;
	int ret;

	ret = block2mtd_parse(val, &info);
	if (ret)
		return ret;
	mutex_lock(&block2mtd_devices_lock);
	if (block2mtd_stopping)
		ret = -ESHUTDOWN;
	else if (block2mtd_initialized)
		ret = block2mtd_attach(&info);
	else if (block2mtd_pending_count >= BLOCK2MTD_MAX_DEVICES)
		ret = -E2BIG;
	else
		block2mtd_pending[block2mtd_pending_count++] = info;
	mutex_unlock(&block2mtd_devices_lock);
	return ret;
}

module_param_call(block2mtd, block2mtd_setup, NULL, NULL, 0200);
MODULE_PARM_DESC(block2mtd, "Device to use: <dev>[,<erasesize>[,<mode:0|1>]]");

static void block2mtd_cleanup(void)
{
	struct block2mtd_retry *retry, *retry_next;
	struct block2mtd_dev *dev, *next;
	int ret;

	mutex_lock(&block2mtd_devices_lock);
	block2mtd_stopping = true;
	block2mtd_initialized = false;
	mutex_unlock(&block2mtd_devices_lock);
	/* Do not hold the registry lock while joining a registry user. */
	list_for_each_entry_safe(retry, retry_next, &block2mtd_retry_list, list) {
		kthread_stop(retry->task);
		put_task_struct(retry->task);
		list_del(&retry->list);
		kfree(retry);
	}
	mutex_lock(&block2mtd_devices_lock);
	list_for_each_entry_safe(dev, next, &blkmtd_device_list, list) {
		ret = mtd_device_unregister(&dev->mtd);
		if (ret) {
			/* Live MTD users pin .owner; never free a registered object. */
			pr_err("mtd%d: unregister failed: %d; retaining backing\n",
				dev->mtd.index, ret);
			continue;
		}
		block2mtd_sync(&dev->mtd);
		list_del(&dev->list);
		block2mtd_free_device(dev);
	}
	block2mtd_device_count = 0;
	mutex_unlock(&block2mtd_devices_lock);
}

static int __init block2mtd_init(void)
{
	unsigned int i;
	int ret = 0;

	mutex_lock(&block2mtd_devices_lock);
	for (i = 0; i < block2mtd_pending_count; i++) {
		ret = block2mtd_attach(&block2mtd_pending[i]);
		if (ret)
			break;
	}
	if (!ret)
		block2mtd_initialized = true;
	mutex_unlock(&block2mtd_devices_lock);
	if (ret)
		block2mtd_cleanup();
	return ret;
}

static void __exit block2mtd_exit(void)
{
	block2mtd_cleanup();
}

late_initcall(block2mtd_init);
module_exit(block2mtd_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joern Engel <joern@lazybastard.org>");
MODULE_DESCRIPTION("Emulate an MTD using a block device");
