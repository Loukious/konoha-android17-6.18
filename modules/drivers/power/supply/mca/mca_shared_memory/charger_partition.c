// SPDX-License-Identifier: GPL-2.0
/* Xiaomi charger partition format, accessed through the public block API. */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_sysfs.h>
#include <mca/shared_memory/charger_partition_class.h>
#include <mca/strategy/strategy_class.h>
#include "inc/charger_partition.h"

struct charger_partition_device {
	struct device *dev;
	struct file *file;
	struct delayed_work discover_work;
	unsigned int retries;
	bool eu_valid;
	bool eu_model;
};

/* alloc/read/write/dealloc is one same-task transaction, including scratch. */
static DEFINE_MUTEX(partition_lock);
static struct charger_partition_device *partition;
static struct task_struct *transaction_owner;
static struct page *header_page;
static struct page *data_page;
static u8 transaction_host;
static u8 transaction_info;
static u32 transaction_size;

static bool charger_partition_request_valid(u8 host, u8 info, u32 size)
{
	return host < CHARGER_PARTITION_HOST_LAST &&
	       info > CHARGER_PARTITION_HEADER &&
	       info < CHARGER_PARTITION_INFO_LAST && size &&
	       size <= CHARGER_PARTITION_RWSIZE;
}

static int charger_partition_io(struct file *file, struct page *page, u8 info,
				bool write)
{
	struct block_device *bdev = file_bdev(file);
	struct bio *bio;
	sector_t sector =
		(sector_t)info * (CHARGER_PARTITION_RWSIZE >> SECTOR_SHIFT);
	blk_opf_t op = write ? REQ_OP_WRITE | REQ_SYNC | REQ_FUA : REQ_OP_READ;
	int ret;

	if (info >= CHARGER_PARTITION_INFO_LAST ||
	    bdev_nr_sectors(bdev) <
		    sector + (CHARGER_PARTITION_RWSIZE >> SECTOR_SHIFT))
		return -ERANGE;
	bio = bio_alloc(bdev, 1, op, GFP_KERNEL);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = sector;
	if (bio_add_page(bio, page, CHARGER_PARTITION_RWSIZE, 0) !=
	    CHARGER_PARTITION_RWSIZE) {
		bio_put(bio);
		return -EIO;
	}
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

int charger_partition_alloc(u8 host, u8 info, u32 size)
{
	charger_partition_header *header;
	int ret;

	if (!charger_partition_request_valid(host, info, size))
		return -EINVAL;
	if (READ_ONCE(transaction_owner) == current)
		return -EDEADLK;
	mutex_lock(&partition_lock);
	if (!partition || !partition->file) {
		ret = -ENODEV;
		goto unlock;
	}
	header_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	data_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!header_page || !data_page) {
		ret = -ENOMEM;
		goto free;
	}
	ret = charger_partition_io(partition->file, header_page,
				   CHARGER_PARTITION_HEADER, false);
	if (ret)
		goto free;
	header = page_address(header_page);
	if (header->magic != CHARGER_PARTITION_MAGIC || !header->initialized) {
		ret = -EUCLEAN;
		goto free;
	}
	if (!header->available) {
		ret = -EBUSY;
		goto free;
	}
	/* Preserve the firmware's header handshake and all unknown fields. */
	header->available = 0;
	ret = charger_partition_io(partition->file, header_page,
				   CHARGER_PARTITION_HEADER, true);
	if (ret)
		goto free;
	__module_get(THIS_MODULE);
	transaction_host = host;
	transaction_info = info;
	transaction_size = size;
	WRITE_ONCE(transaction_owner, current);
	return 0;
free:
	if (data_page)
		__free_page(data_page);
	if (header_page)
		__free_page(header_page);
	data_page = NULL;
	header_page = NULL;
unlock:
	mutex_unlock(&partition_lock);
	return ret;
}
EXPORT_SYMBOL(charger_partition_alloc);

static bool charger_partition_transaction_matches(u8 host, u8 info, u32 size)
{
	return READ_ONCE(transaction_owner) == current &&
	       host == transaction_host && info == transaction_info &&
	       size == transaction_size;
}

int charger_partition_dealloc(u8 host, u8 info, u32 size)
{
	charger_partition_header *header;
	int ret;

	if (!charger_partition_transaction_matches(host, info, size))
		return -EPERM;
	header = page_address(header_page);
	header->available = 1;
	ret = charger_partition_io(partition->file, header_page,
				   CHARGER_PARTITION_HEADER, true);
	__free_page(data_page);
	__free_page(header_page);
	data_page = NULL;
	header_page = NULL;
	WRITE_ONCE(transaction_owner, NULL);
	mutex_unlock(&partition_lock);
	module_put(THIS_MODULE);
	return ret;
}
EXPORT_SYMBOL(charger_partition_dealloc);

void *charger_partition_read(u8 host, u8 info, u32 size)
{
	if (!charger_partition_transaction_matches(host, info, size))
		return NULL;
	if (charger_partition_io(partition->file, data_page, info, false))
		return NULL;
	return page_address(data_page);
}
EXPORT_SYMBOL(charger_partition_read);

int charger_partition_write(u8 host, u8 info, void *buf, u32 size)
{
	void *copy;
	int ret;

	if (!buf || !charger_partition_transaction_matches(host, info, size))
		return -EINVAL;
	/* buf may point into the scratch returned by charger_partition_read(). */
	copy = kmemdup(buf, size, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	ret = charger_partition_io(partition->file, data_page, info, false);
	if (!ret) {
		memcpy(page_address(data_page), copy, size);
		ret = charger_partition_io(partition->file, data_page, info,
					   true);
	}
	kfree(copy);
	return ret;
}
EXPORT_SYMBOL(charger_partition_write);

static int charger_partition_field(u8 info, size_t offset, u32 *value,
				   size_t size, bool write)
{
	u32 record_size = info == CHARGER_PARTITION_INFO_1 ?
				  sizeof(charger_partition_info_1) :
				  sizeof(charger_partition_info_2);
	void *record;
	int ret, end_ret;

	if (!value || offset > record_size || size > record_size - offset)
		return -EINVAL;
	ret = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL, info,
				      record_size);
	if (ret)
		return ret;
	record = charger_partition_read(CHARGER_PARTITION_HOST_KERNEL, info,
					record_size);
	if (!record) {
		ret = -EIO;
		goto end;
	}
	if (write) {
		memcpy(record + offset, value, size);
		/* data_page already contains a complete, preserved record block. */
		ret = charger_partition_io(partition->file, data_page, info,
					   true);
	} else {
		memcpy(value, record + offset, size);
	}
	if (!ret && info == CHARGER_PARTITION_INFO_2) {
		partition->eu_model =
			!!((charger_partition_info_2 *)record)->eu_mode;
		partition->eu_valid = true;
	}
end:
	end_ret = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, info,
					    record_size);
	return ret ? ret : end_ret;
}

int charger_partition_get_eu_model(bool *eu_model)
{
	int ret = 0;

	if (!eu_model)
		return -EINVAL;
	if (READ_ONCE(transaction_owner) == current)
		return -EDEADLK;
	guard(mutex)(&partition_lock);
	if (!partition || !partition->eu_valid)
		ret = -EAGAIN;
	else
		*eu_model = partition->eu_model;
	return ret;
}
EXPORT_SYMBOL(charger_partition_get_eu_model);

int charger_partition_read_double85(int *value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						double85),
				       (u32 *)value, sizeof(int), false);
}
EXPORT_SYMBOL(charger_partition_read_double85);

int charger_partition_write_double85(int value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						double85),
				       (u32 *)&value, sizeof(int), true);
}
EXPORT_SYMBOL(charger_partition_write_double85);

int charger_partition_read_remove_temp_limit(int *value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						remove_temp_limit),
				       (u32 *)value, sizeof(int), false);
}
EXPORT_SYMBOL(charger_partition_read_remove_temp_limit);

int charger_partition_write_remove_temp_limit(int value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						remove_temp_limit),
				       (u32 *)&value, sizeof(int), true);
}
EXPORT_SYMBOL(charger_partition_write_remove_temp_limit);

int charger_partition_read_memory_test(int *value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						memory_test),
				       (u32 *)value, sizeof(int), false);
}
EXPORT_SYMBOL(charger_partition_read_memory_test);

int charger_partition_write_memory_test(int value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						memory_test),
				       (u32 *)&value, sizeof(int), true);
}
EXPORT_SYMBOL(charger_partition_write_memory_test);

int charger_partition_read_soc_limit(int *value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						soc_limit),
				       (u32 *)value, sizeof(int), false);
}
EXPORT_SYMBOL(charger_partition_read_soc_limit);

int charger_partition_write_soc_limit(int value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_1,
				       offsetof(charger_partition_info_1,
						soc_limit),
				       (u32 *)&value, sizeof(int), true);
}
EXPORT_SYMBOL(charger_partition_write_soc_limit);

int charger_partition_get_mishow(bool *mishow)
{
	u32 value;
	int ret;

	if (!mishow)
		return -EINVAL;
	ret = charger_partition_field(CHARGER_PARTITION_INFO_1,
				      offsetof(charger_partition_info_1,
					       mishow),
				      &value, sizeof(value), false);
	if (!ret)
		*mishow = !!value;
	return ret;
}
EXPORT_SYMBOL(charger_partition_get_mishow);

int charger_partition_read_ocd_count(u32 *v0, u32 *v1)
{
	u32 values[2];
	int ret;

	if (!v0 || !v1)
		return -EINVAL;
	ret = charger_partition_field(CHARGER_PARTITION_INFO_2,
				      offsetof(charger_partition_info_2,
					       ocd_count),
				      values, sizeof(values), false);
	if (!ret) {
		*v0 = values[0];
		*v1 = values[1];
	}
	return ret;
}
EXPORT_SYMBOL(charger_partition_read_ocd_count);

int charger_partition_write_ocd_count(u32 v0, u32 v1)
{
	u32 values[] = { v0, v1 };

	return charger_partition_field(CHARGER_PARTITION_INFO_2,
				       offsetof(charger_partition_info_2,
						ocd_count),
				       values, sizeof(values), true);
}
EXPORT_SYMBOL(charger_partition_write_ocd_count);

int charger_partition_read_cuv_count(u32 *v0, u32 *v1)
{
	u32 values[2];
	int ret;

	if (!v0 || !v1)
		return -EINVAL;
	ret = charger_partition_field(CHARGER_PARTITION_INFO_2,
				      offsetof(charger_partition_info_2,
					       cuv_count),
				      values, sizeof(values), false);
	if (!ret) {
		*v0 = values[0];
		*v1 = values[1];
	}
	return ret;
}
EXPORT_SYMBOL(charger_partition_read_cuv_count);

int charger_partition_write_cuv_count(u32 v0, u32 v1)
{
	u32 values[] = { v0, v1 };

	return charger_partition_field(CHARGER_PARTITION_INFO_2,
				       offsetof(charger_partition_info_2,
						cuv_count),
				       values, sizeof(values), true);
}
EXPORT_SYMBOL(charger_partition_write_cuv_count);

int charger_partition_read_hscd_count(u32 *value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_2,
				       offsetof(charger_partition_info_2,
						hscd_count),
				       value, sizeof(*value), false);
}
EXPORT_SYMBOL(charger_partition_read_hscd_count);

int charger_partition_write_hcsd_count(u32 value)
{
	return charger_partition_field(CHARGER_PARTITION_INFO_2,
				       offsetof(charger_partition_info_2,
						hscd_count),
				       &value, sizeof(value), true);
}
EXPORT_SYMBOL(charger_partition_write_hcsd_count);

static ssize_t charger_partition_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf);
static ssize_t charger_partition_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count);

static struct mca_sysfs_attr_info charger_partition_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(charger_partition_sysfs, 0664,
			  MCA_PROP_CHARGER_PARTITION_MISHOW,
			  charger_partition_mishow),
	mca_sysfs_attr_rw(charger_partition_sysfs, 0664,
			  MCA_PROP_CHARGER_PARTITION_POWEROFFMODE,
			  charger_partition_poweroffmode),
	mca_sysfs_attr_rw(charger_partition_sysfs, 0664,
			  MCA_PROP_CHARGER_PARTITION_PROP_EU_MODE,
			  charger_partition_prop_eu_mode),
};

#define CHARGER_PARTITION_SYSFS_ATTRS_SIZE \
	ARRAY_SIZE(charger_partition_sysfs_field_tbl)
static struct attribute
	*charger_partition_sysfs_attrs[CHARGER_PARTITION_SYSFS_ATTRS_SIZE + 1];
static const struct attribute_group charger_partition_sysfs_attr_group = {
	.attrs = charger_partition_sysfs_attrs,
};

static int charger_partition_sysfs_value(struct device_attribute *attr,
					 u32 *value, bool write)
{
	struct mca_sysfs_attr_info *entry;
	size_t offset;
	u8 info = CHARGER_PARTITION_INFO_1;

	entry = mca_sysfs_lookup_attr(attr->attr.name,
				      charger_partition_sysfs_field_tbl,
				      CHARGER_PARTITION_SYSFS_ATTRS_SIZE);
	if (!entry)
		return -EINVAL;
	switch (entry->sysfs_attr_name) {
	case MCA_PROP_CHARGER_PARTITION_MISHOW:
		offset = offsetof(charger_partition_info_1, mishow);
		break;
	case MCA_PROP_CHARGER_PARTITION_POWEROFFMODE:
		offset = offsetof(charger_partition_info_1, power_off_mode);
		break;
	case MCA_PROP_CHARGER_PARTITION_PROP_EU_MODE:
		info = CHARGER_PARTITION_INFO_2;
		offset = offsetof(charger_partition_info_2, eu_mode);
		if (write && *value > 1)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	return charger_partition_field(info, offset, value, sizeof(*value),
				       write);
}

static ssize_t charger_partition_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	u32 value;
	int ret = charger_partition_sysfs_value(attr, &value, false);

	return ret ? ret : sysfs_emit(buf, "%u\n", value);
}

static ssize_t charger_partition_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	u32 value;
	int ret;

	ret = kstrtou32(buf, 0, &value);
	if (!ret)
		ret = charger_partition_sysfs_value(attr, &value, true);
	return ret ? ret : count;
}

static void charger_partition_publish(const charger_partition_info_1 *info,
				      u32 eu)
{
	if (info->double85)
		mca_event_block_notify(MCA_EVENT_TYPE_SUBPMIC_INFO,
				       MCA_EVENT_DEBUG_CTRL_DOUBLE85,
				       (void *)&info->double85);
	if (info->remove_temp_limit)
		mca_event_block_notify(MCA_EVENT_TYPE_SUBPMIC_INFO,
				       MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT,
				       (void *)&info->remove_temp_limit);
	if (info->memory_test)
		mca_event_block_notify(MCA_EVENT_TYPE_SUBPMIC_INFO,
				       MCA_EVENT_DEBUG_CTRL_MEMORY_TEST,
				       (void *)&info->memory_test);
	if (info->soc_limit)
		mca_event_block_notify(MCA_EVENT_TYPE_SUBPMIC_INFO,
				       MCA_EVENT_DEBUG_CTRL_SOC_LIMIT,
				       (void *)&info->soc_limit);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG, MCA_EVENT_IS_EU_MODEL,
				  !!eu);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_IS_EU_MODEL, !!eu);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				  MCA_EVENT_IS_EU_MODEL, !!eu);
}

static void charger_partition_discover(struct work_struct *work)
{
	struct charger_partition_device *cp =
		container_of(to_delayed_work(work),
			     struct charger_partition_device, discover_work);
	charger_partition_info_1 info;
	struct block_device *bdev;
	struct file *file;
	u32 eu;
	int ret;

	mutex_lock(&partition_lock);
	if (!cp->file) {
		file = bdev_file_open_by_path("/dev/block/by-name/charger",
					      BLK_OPEN_READ | BLK_OPEN_WRITE,
					      NULL, NULL);
		if (IS_ERR(file)) {
			ret = PTR_ERR(file);
			mutex_unlock(&partition_lock);
			goto retry;
		}
		bdev = file_bdev(file);
		if (!bdev_is_partition(bdev) || !bdev->bd_meta_info ||
		    strcmp(bdev->bd_meta_info->volname, PARTITION_NAME) ||
		    bdev_nr_bytes(bdev) < CHARGER_PARTITION_INFO_LAST *
						  CHARGER_PARTITION_RWSIZE ||
		    bdev_logical_block_size(bdev) > CHARGER_PARTITION_RWSIZE) {
			bdev_fput(file);
			mutex_unlock(&partition_lock);
			ret = -EINVAL;
			goto retry;
		}
		cp->file = file;
	}
	mutex_unlock(&partition_lock);
	ret = charger_partition_field(CHARGER_PARTITION_INFO_2,
				      offsetof(charger_partition_info_2,
					       eu_mode),
				      &eu, sizeof(eu), false);
	if (ret)
		goto retry;
	ret = charger_partition_field(CHARGER_PARTITION_INFO_1, 0, (u32 *)&info,
				      sizeof(info), false);
	if (ret)
		goto retry;
	/* Publish only after releasing the transaction; listeners may read again. */
	charger_partition_publish(&info, eu);
	return;
retry:
	if (cp->retries++ < CHARGER_PARTITION_RETRY_TIMES)
		schedule_delayed_work(&cp->discover_work,
				      msecs_to_jiffies(CHARGER_WORK_DELAY_MS));
	else
		dev_err(cp->dev, "charger partition unavailable: %d\n", ret);
}

static int charger_partition_probe(struct platform_device *pdev)
{
	struct charger_partition_device *cp;
	int ret;

	cp = devm_kzalloc(&pdev->dev, sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;
	cp->dev = &pdev->dev;
	INIT_DELAYED_WORK(&cp->discover_work, charger_partition_discover);
	mutex_lock(&partition_lock);
	if (partition) {
		mutex_unlock(&partition_lock);
		return -EBUSY;
	}
	partition = cp;
	mutex_unlock(&partition_lock);
	mca_sysfs_init_attrs(charger_partition_sysfs_attrs,
			     charger_partition_sysfs_field_tbl,
			     CHARGER_PARTITION_SYSFS_ATTRS_SIZE);
	ret = mca_sysfs_create_link_group("charger", "charger_partition",
					  cp->dev,
					  &charger_partition_sysfs_attr_group);
	if (ret) {
		guard(mutex)(&partition_lock);
		partition = NULL;
		return ret;
	}
	platform_set_drvdata(pdev, cp);
	schedule_delayed_work(&cp->discover_work, 0);
	return 0;
}

static void charger_partition_remove(struct platform_device *pdev)
{
	struct charger_partition_device *cp = platform_get_drvdata(pdev);

	mca_sysfs_remove_link_group("charger", "charger_partition", cp->dev,
				    &charger_partition_sysfs_attr_group);
	cancel_delayed_work_sync(&cp->discover_work);
	mutex_lock(&partition_lock);
	partition = NULL;
	if (cp->file)
		bdev_fput(cp->file);
	mutex_unlock(&partition_lock);
}

static const struct of_device_id charger_partition_match[] = {
	{ .compatible = "xiaomi,charger_partition" },
	{}
};
MODULE_DEVICE_TABLE(of, charger_partition_match);

static struct platform_driver charger_partition_driver = {
	.driver = {
		.name = "charger_partition",
		.of_match_table = charger_partition_match,
	},
	.probe = charger_partition_probe,
	.remove = charger_partition_remove,
};
module_platform_driver(charger_partition_driver);

MODULE_DESCRIPTION("Xiaomi charger partition storage");
MODULE_LICENSE("GPL");
