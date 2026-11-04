// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/qcom-dma-mapping.h>
#include <linux/qcom-iommu-util.h>
#include "qcom-iommu-debug.h"

struct iommu_debug_user_map {
	struct list_head node;
	dma_addr_t iova;
	size_t size;
	unsigned long attrs;
	bool dma;
	bool uncertain;
};

static bool iommu_debug_range_valid(unsigned long addr, size_t size,
				    unsigned long base, size_t capacity)
{
	return size && capacity <= ULONG_MAX - base && addr >= base &&
		size <= capacity && addr - base <= capacity - size;
}

static bool iommu_debug_iova_valid(u64 addr, u64 size)
{
	return size && size <= SIZE_MAX && addr <= ULONG_MAX &&
		size - 1 <= ULONG_MAX - addr &&
		IS_ALIGNED(addr, PAGE_SIZE) && IS_ALIGNED(size, PAGE_SIZE);
}

static int iommu_debug_parse_values(const char __user *buf, size_t count,
				    u64 *values, unsigned int nr)
{
	char input[100], *cursor = input, *word;
	unsigned int i;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	if (memchr(input, '\0', count))
		return -EINVAL;
	input[count] = '\0';
	for (i = 0; i < nr; i++) {
		word = strsep(&cursor, ",");
		if (!word || kstrtou64(word, 0, &values[i]))
			return -EINVAL;
	}
	return cursor ? -EINVAL : 0;
}

static int iommu_debug_dma_attrs(u64 attr, unsigned long *attrs)
{
	switch (attr) {
	case 0: *attrs = 0; break;
	case 1: *attrs = DMA_ATTR_FORCE_COHERENT; break;
	case 2: *attrs = DMA_ATTR_FORCE_NON_COHERENT; break;
	case 3: *attrs = DMA_ATTR_SYS_CACHE; break;
	default: return -EINVAL;
	}
	return 0;
}

static bool iommu_debug_user_maps_pending(struct iommu_debug_device *ddev)
{
	struct iommu_debug_user_map *map;

	list_for_each_entry(map, &ddev->user_maps, node)
		if (map->uncertain)
			return true;
	return false;
}

/* Caller holds state_lock and has drained debugfs on removal. */
int iommu_debug_user_cleanup(struct iommu_debug_device *ddev, bool removing)
{
	struct iommu_debug_user_map *map, *tmp;
	bool residual = false;

	list_for_each_entry_safe(map, tmp, &ddev->user_maps, node) {
		if (map->dma) {
			dma_unmap_single_attrs(ddev->test_dev, map->iova, map->size,
					       DMA_TO_DEVICE, map->attrs);
		} else {
			/* IOMMU unmaps must match an original map, including retries. */
			size_t done = iommu_unmap(ddev->domain, map->iova, map->size);

			if (done != map->size) {
				map->uncertain = true;
				residual = true;
				if (!removing)
					continue;
			}
		}
		list_del(&map->node);
		kfree(map);
	}
	if (residual && !removing)
		return -EBUSY;
	/* An unresponsive provider may still reference the test buffer through
	 * a raw mapping. Never release its backing pages in that failure case.
	 */
	if (residual)
		dev_warn(ddev->self, "Unmap made no progress; retaining debug buffer until reboot\n");
	else
		kfree(ddev->test_virt_addr);
	ddev->test_virt_addr = NULL;
	WRITE_ONCE(ddev->iova, 0);
	return 0;
}

static ssize_t iommu_debug_dma_atos_read(struct file *file, char __user *ubuf,
					 size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	struct iommu_fwspec *fwspec;
	struct qcom_iommu_atos_txn txn;
	phys_addr_t phys;
	char buf[100];
	int len;

	if (*offset)
		return 0;
	mutex_lock(&ddev->state_lock);
	if (!ddev->domain) {
		len = -EINVAL;
		goto out;
	}
	fwspec = dev_iommu_fwspec_get(ddev->test_dev);
	if (!fwspec || !fwspec->num_ids) {
		len = -EINVAL;
		goto out;
	}
	txn.addr = ddev->iova;
	txn.flags = IOMMU_TRANS_DEFAULT;
	txn.id = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[0]);
	phys = qcom_iommu_iova_to_phys_hard(ddev->domain, &txn);
	len = phys ? scnprintf(buf, sizeof(buf), "%pa\n", &phys) :
		strscpy(buf, "FAIL\n", sizeof(buf));
out:
	mutex_unlock(&ddev->state_lock);
	return len < 0 ? len : simple_read_from_buffer(ubuf, count, offset, buf, len);
}

static ssize_t iommu_debug_atos_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	u64 iova;
	phys_addr_t phys;
	int ret = iommu_debug_parse_values(ubuf, count, &iova, 1);

	if (ret)
		return ret;
	mutex_lock(&ddev->state_lock);
	if (!ddev->domain) {
		ret = -EINVAL;
		goto out;
	}
	phys = iommu_iova_to_phys(ddev->domain, iova);
	if (!pfn_valid(__phys_to_pfn(phys))) {
		ret = -EINVAL;
		goto out;
	}
	WRITE_ONCE(ddev->iova, iova);
out:
	mutex_unlock(&ddev->state_lock);
	return ret ? ret : count;
}

const struct file_operations iommu_debug_atos_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = iommu_debug_atos_write,
	.read = iommu_debug_dma_atos_read,
};

static ssize_t iommu_debug_map_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	struct iommu_debug_user_map *map;
	u64 values[4];
	int ret = iommu_debug_parse_values(ubuf, count, values, ARRAY_SIZE(values));

	if (ret)
		return ret;
	if (!iommu_debug_iova_valid(values[0], values[2]) ||
	    !iommu_debug_iova_valid(values[1], values[2]) || values[3] > INT_MAX)
		return -EINVAL;
	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	mutex_lock(&ddev->state_lock);
	if (!ddev->domain) {
		ret = -EINVAL;
		goto out;
	}
	if (iommu_debug_user_maps_pending(ddev)) {
		ret = -EBUSY;
		goto out;
	}
	ret = iommu_map(ddev->domain, values[0], values[1], values[2], values[3], GFP_KERNEL);
	if (!ret) {
		map->iova = values[0];
		map->size = values[2];
		list_add_tail(&map->node, &ddev->user_maps);
		map = NULL;
	}
out:
	mutex_unlock(&ddev->state_lock);
	kfree(map);
	return ret ? ret : count;
}

const struct file_operations iommu_debug_map_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = iommu_debug_map_write,
};

static ssize_t iommu_debug_unmap_write(struct file *file, const char __user *ubuf,
				       size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	struct iommu_debug_user_map *map;
	u64 values[2];
	int ret = iommu_debug_parse_values(ubuf, count, values, ARRAY_SIZE(values));

	if (ret)
		return ret;
	if (!iommu_debug_iova_valid(values[0], values[1]))
		return -EINVAL;
	mutex_lock(&ddev->state_lock);
	ret = -EINVAL;
	if (!ddev->domain)
		goto out;
	list_for_each_entry(map, &ddev->user_maps, node) {
		if (!map->dma && map->iova == values[0] && map->size == values[1]) {
			size_t done = iommu_unmap(ddev->domain, map->iova, map->size);

			if (done == map->size) {
				list_del(&map->node);
				kfree(map);
				ret = 0;
			} else {
				map->uncertain = true;
				ret = -EIO;
			}
			break;
		}
	}
out:
	mutex_unlock(&ddev->state_lock);
	return ret ? ret : count;
}

const struct file_operations iommu_debug_unmap_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = iommu_debug_unmap_write,
};

static ssize_t iommu_debug_dma_map_write(struct file *file, const char __user *ubuf,
					 size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	struct iommu_debug_user_map *map;
	u64 values[3];
	unsigned long attrs;
	int ret = iommu_debug_parse_values(ubuf, count, values, ARRAY_SIZE(values));

	if (ret)
		return ret;
	ret = iommu_debug_dma_attrs(values[2], &attrs);
	if (ret)
		return ret;
	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	mutex_lock(&ddev->state_lock);
	ret = -EINVAL;
	if (!ddev->domain || !ddev->test_virt_addr ||
	    !iommu_debug_range_valid(values[0], values[1],
				    (unsigned long)ddev->test_virt_addr, SZ_1M))
		goto out;
	if (iommu_debug_user_maps_pending(ddev)) {
		ret = -EBUSY;
		goto out;
	}
	map->iova = dma_map_single_attrs(ddev->test_dev, (void *)(unsigned long)values[0],
					values[1], DMA_TO_DEVICE, attrs);
	if (dma_mapping_error(ddev->test_dev, map->iova)) {
		ret = -EIO;
		goto out;
	}
	map->size = values[1];
	map->attrs = attrs;
	map->dma = true;
	WRITE_ONCE(ddev->iova, map->iova);
	list_add_tail(&map->node, &ddev->user_maps);
	map = NULL;
	ret = 0;
out:
	mutex_unlock(&ddev->state_lock);
	kfree(map);
	return ret ? ret : count;
}

static ssize_t iommu_debug_dma_map_read(struct file *file, char __user *ubuf,
					 size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	dma_addr_t iova = READ_ONCE(ddev->iova);
	char buf[100];
	int len;

	if (*offset)
		return 0;
	len = scnprintf(buf, sizeof(buf), "%pa\n", &iova);
	return simple_read_from_buffer(ubuf, count, offset, buf, len);
}

const struct file_operations iommu_debug_dma_map_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = iommu_debug_dma_map_read,
	.write = iommu_debug_dma_map_write,
};

static ssize_t iommu_debug_dma_unmap_write(struct file *file, const char __user *ubuf,
					   size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	struct iommu_debug_user_map *map;
	u64 values[3];
	unsigned long attrs;
	int ret = iommu_debug_parse_values(ubuf, count, values, ARRAY_SIZE(values));

	if (ret)
		return ret;
	ret = iommu_debug_dma_attrs(values[2], &attrs);
	if (ret)
		return ret;
	mutex_lock(&ddev->state_lock);
	ret = -EINVAL;
	if (!ddev->domain)
		goto out;
	list_for_each_entry(map, &ddev->user_maps, node) {
		if (map->dma && map->iova == values[0] && map->size == values[1] && map->attrs == attrs) {
			dma_unmap_single_attrs(ddev->test_dev, map->iova, map->size,
					       DMA_TO_DEVICE, map->attrs);
			list_del(&map->node);
			kfree(map);
			ret = 0;
			break;
		}
	}
out:
	mutex_unlock(&ddev->state_lock);
	return ret ? ret : count;
}

const struct file_operations iommu_debug_dma_unmap_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = iommu_debug_dma_unmap_write,
};

static int iommu_debug_build_phoney_sg_table(struct device *dev,
					     struct sg_table *table,
					     unsigned long total_size,
					     unsigned long chunk_size)
{
	unsigned long nents = total_size / chunk_size;
	struct scatterlist *sg;
	int i, j;
	struct page *page;

	if (!IS_ALIGNED(total_size, SZ_4K))
		return -EINVAL;
	if (!IS_ALIGNED(total_size, chunk_size))
		return -EINVAL;
	if (sg_alloc_table(table, nents, GFP_KERNEL))
		return -EINVAL;

	for_each_sg(table->sgl, sg, table->nents, i) {
		page = alloc_pages(GFP_KERNEL, get_order(chunk_size));
		if (!page)
			goto free_pages;
		sg_set_page(sg, page, chunk_size, 0);
	}

	return 0;
free_pages:
	for_each_sg(table->sgl, sg, i, j)
		__free_pages(sg_page(sg), get_order(chunk_size));
	sg_free_table(table);
	return -ENOMEM;
}

static void iommu_debug_destroy_phoney_sg_table(struct device *dev,
						struct sg_table *table,
						unsigned long chunk_size)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(table->sgl, sg, table->nents, i)
		__free_pages(sg_page(sg), get_order(chunk_size));
	sg_free_table(table);
}

#define ps_printf(name, s, fmt, ...) ({						\
			pr_err("%s: " fmt, name, ##__VA_ARGS__);		\
			seq_printf(s, fmt, ##__VA_ARGS__);			\
		})

static int __functional_dma_api_alloc_test(struct device *dev,
					   struct seq_file *s,
					   struct iommu_domain *domain,
					   void *ignored)
{
	size_t size = SZ_1K * 742;
	int ret = 0;
	u8 *data;
	dma_addr_t iova;

	/* Make sure we can allocate and use a buffer */
	ps_printf(dev_name(dev), s, "Allocating coherent buffer");
	data = dma_alloc_coherent(dev, size, &iova, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
	} else {
		int i;

		ps_printf(dev_name(dev), s, "  -> SUCCEEDED\n");
		ps_printf(dev_name(dev), s, "Using coherent buffer");
		for (i = 0; i < 742; ++i) {
			int ind = SZ_1K * i;
			u8 *p = data + ind;
			u8 val = i % 255;

			memset(data, 0xa5, size);
			*p = val;
			(*p)++;
			if ((*p) != val + 1) {
				ps_printf(dev_name(dev), s,
					  "  -> FAILED on iter %d since %d != %d\n",
					  i, *p, val + 1);
				ret = -EINVAL;
			}
		}
		if (!ret)
			ps_printf(dev_name(dev), s, "  -> SUCCEEDED\n");
		dma_free_coherent(dev, size, data, iova);
	}

	return ret;
}

static int __functional_dma_api_basic_test(struct device *dev,
					   struct seq_file *s,
					   struct iommu_domain *domain,
					   void *ignored)
{
	size_t size = 1518;
	int i, j, ret = 0;
	u8 *data;
	dma_addr_t iova;

	ps_printf(dev_name(dev), s, "Basic DMA API test");
	/* Make sure we can allocate and use a buffer */
	for (i = 0; i < 1000; ++i) {
		data = kmalloc(size, GFP_KERNEL);
		if (!data) {
			ret = -ENOMEM;
			goto out;
		}
		memset(data, 0xa5, size);
		iova = dma_map_single(dev, data, size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, iova)) {
			kfree(data);
			ret = -EIO;
			goto out;
		}
		ret = iommu_debug_check_mapping_fast(dev, iova, size, virt_to_phys(data));
		dma_unmap_single(dev, iova, size, DMA_TO_DEVICE);
		if (ret) {
			kfree(data);
			goto out;
		}
		for (j = 0; j < size; ++j) {
			if (data[j] != 0xa5) {
				dev_err_ratelimited(dev, "data[%d] != 0xa5\n", j);
				ret = -EINVAL;
				kfree(data);
				goto out;
			}
		}
		kfree(data);
	}

out:
	if (ret)
		ps_printf(dev_name(dev), s, "  -> FAILED\n");
	else
		ps_printf(dev_name(dev), s, "  -> SUCCEEDED\n");

	return ret;
}

static int __functional_dma_api_map_sg_test(struct device *dev, struct seq_file *s,
					    struct iommu_domain *domain, size_t sizes[])
{
	const size_t *sz;
	int ret = 0, count = 0;

	ps_printf(dev_name(dev), s, "Map SG DMA API test");

	for (sz = sizes; *sz; ++sz) {
		size_t size = *sz;
		struct sg_table table;
		unsigned long chunk_size = SZ_4K;

		/* Build us a table */
		ret = iommu_debug_build_phoney_sg_table(dev, &table, size, chunk_size);
		if (ret) {
			seq_puts(s, "couldn't build phoney sg table! bailing...\n");
			goto out;
		}

		count = dma_map_sg(dev, table.sgl, table.nents, DMA_BIDIRECTIONAL);
		if (!count) {
			ret = -EINVAL;
			goto destroy_table;
		}

		/* Check mappings... */
		ret = iommu_debug_check_mapping_sg_fast(dev, table.sgl, 0, count, table.nents);

		dma_unmap_sg(dev, table.sgl, table.nents, DMA_BIDIRECTIONAL);
destroy_table:
		iommu_debug_destroy_phoney_sg_table(dev, &table, chunk_size);
		if (ret)
			goto out;
	}
out:
	if (ret)
		ps_printf(dev_name(dev), s, "  -> FAILED\n");
	else
		ps_printf(dev_name(dev), s, "  -> SUCCEEDED\n");

	return ret;
}

static int iommu_debug_functional_arm_dma_api_show(struct seq_file *s,
						   void *ignored)
{
	struct iommu_debug_device *ddev = s->private;
	struct device *dev;
	size_t sizes[] = {SZ_4K, SZ_64K, SZ_2M, SZ_1M * 12, 0};
	int ret = -EINVAL;

	mutex_lock(&ddev->state_lock);
	if (!iommu_debug_usecase_reset(ddev))
		goto out;
	dev = ddev->test_dev;

	ret = __functional_dma_api_alloc_test(dev, s, ddev->domain, sizes);
	ret |= __functional_dma_api_basic_test(dev, s, ddev->domain, sizes);
	ret |= __functional_dma_api_map_sg_test(dev, s, ddev->domain, sizes);

out:
	mutex_unlock(&ddev->state_lock);
	if (ret)
		seq_printf(s, "FAIL %d\n", ret);
	else
		seq_puts(s, "SUCCESS\n");

	return 0;
}

static int iommu_debug_functional_arm_dma_api_open(struct inode *inode,
						   struct file *file)
{
	return single_open(file, iommu_debug_functional_arm_dma_api_show,
			   inode->i_private);
}

const struct file_operations iommu_debug_functional_arm_dma_api_fops = {
	.owner = THIS_MODULE,
	.open	 = iommu_debug_functional_arm_dma_api_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

/* Creates a fresh fast mapping and applies @fn to it */
static int __apply_to_new_mapping(struct seq_file *s,
				  int (*fn)(struct device *dev,
					    struct seq_file *s,
					    struct iommu_domain *domain,
					    void *priv),
				  void *priv)
{
	struct iommu_domain *domain;
	struct iommu_debug_device *ddev = s->private;
	struct device *dev;
	int ret = -EINVAL;

	lockdep_assert_held(&ddev->state_lock);
	if (!iommu_debug_usecase_reset(ddev))
		goto out;

	domain = ddev->domain;
	dev = ddev->test_dev;

	ret = fn(dev, s, domain, priv);

out:
	seq_printf(s, "%s\n", ret ? "FAIL" : "SUCCESS");
	return ret;
}

static const char *_size_to_string(unsigned long size)
{
	switch (size) {
	case SZ_4K:
		return "4K";
	case SZ_8K:
		return "8K";
	case SZ_16K:
		return "16K";
	case SZ_64K:
		return "64K";
	case SZ_1M:
		return "1M";
	case SZ_2M:
		return "2M";
	case SZ_1M * 12:
		return "12M";
	case SZ_1M * 20:
		return "20M";
	case SZ_1M * 24:
		return "24M";
	case SZ_1M * 32:
		return "32M";
	}

	pr_err("unknown size, please add to %s\n", __func__);
	return "unknown size";
}

static int __check_mapping(struct device *dev, struct iommu_domain *domain,
			   dma_addr_t iova, phys_addr_t expected)
{
	struct iommu_fwspec *fwspec;
	phys_addr_t res, res2;
	struct qcom_iommu_atos_txn txn;

	fwspec = dev_iommu_fwspec_get(dev);
	if (!fwspec || !fwspec->num_ids) {
		dev_err_ratelimited(dev, "No fwspec.\n");
		return -EINVAL;
	}

	txn.addr = iova;
	txn.flags = IOMMU_TRANS_DEFAULT;
	txn.id = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[0]);

	res = qcom_iommu_iova_to_phys_hard(domain, &txn);
	res2 = iommu_iova_to_phys(domain, iova);

	if (res != expected || res2 != expected) {
		dev_err_ratelimited(dev, "Bad translation for %pa! Expected: %pa Got: %pa (ATOS) %pa (walk)\n",
				    &iova, &expected, &res, &res2);
		return -EINVAL;
	}

	return 0;
}

/* Each successful DMA map is retired with its original address and size. */
#define IOMMU_DEBUG_VA_LIMIT (SZ_1G * 4ULL)
#define IOMMU_DEBUG_DMA_GRANULE SZ_4K
#define IOMMU_DEBUG_DMA_CONT 0xff

struct iommu_debug_dma_ledger {
	u8 *slots;
	struct device *dev;
	unsigned int nr_maps;
};

static int iommu_debug_dma_ledger_init(struct iommu_debug_dma_ledger *ledger,
					     struct device *dev)
{
	ledger->slots = kvcalloc(IOMMU_DEBUG_VA_LIMIT / IOMMU_DEBUG_DMA_GRANULE,
				 sizeof(*ledger->slots), GFP_KERNEL);
	ledger->dev = dev;
	ledger->nr_maps = 0;
	return ledger->slots ? 0 : -ENOMEM;
}

/* U64_MAX permits any free IOVA during the refill phase. */
static int iommu_debug_dma_ledger_map(struct iommu_debug_dma_ledger *ledger,
					    void *virt, size_t size, u64 expected)
{
	dma_addr_t addr;
	u64 slot, nr, i;

	if (!size || size % IOMMU_DEBUG_DMA_GRANULE ||
	    size / IOMMU_DEBUG_DMA_GRANULE >= IOMMU_DEBUG_DMA_CONT)
		return -EINVAL;
	addr = dma_map_single(ledger->dev, virt, size, DMA_TO_DEVICE);
	if (dma_mapping_error(ledger->dev, addr))
		return -ENOMEM;
	if ((expected != U64_MAX && addr != expected) ||
	    addr % IOMMU_DEBUG_DMA_GRANULE || addr >= IOMMU_DEBUG_VA_LIMIT ||
	    size > IOMMU_DEBUG_VA_LIMIT - addr) {
		dma_unmap_single(ledger->dev, addr, size, DMA_TO_DEVICE);
		return -EINVAL;
	}
	slot = addr / IOMMU_DEBUG_DMA_GRANULE;
	nr = size / IOMMU_DEBUG_DMA_GRANULE;
	for (i = 0; i < nr; i++) {
		if (ledger->slots[slot + i]) {
			/* A DMA provider must never return an already-owned IOVA. */
			dev_err(ledger->dev, "DMA allocator returned overlapping IOVA %pad\n", &addr);
			return -EIO;
		}
	}
	ledger->slots[slot] = nr;
	for (i = 1; i < nr; i++)
		ledger->slots[slot + i] = IOMMU_DEBUG_DMA_CONT;
	ledger->nr_maps++;
	return 0;
}

static int iommu_debug_dma_ledger_unmap(struct iommu_debug_dma_ledger *ledger,
					      u64 addr, size_t size)
{
	u64 slot;

	if (addr >= IOMMU_DEBUG_VA_LIMIT || addr % IOMMU_DEBUG_DMA_GRANULE ||
	    !size || size % IOMMU_DEBUG_DMA_GRANULE ||
	    size > IOMMU_DEBUG_VA_LIMIT - addr)
		return -EINVAL;
	slot = addr / IOMMU_DEBUG_DMA_GRANULE;
	if (ledger->slots[slot] != size / IOMMU_DEBUG_DMA_GRANULE)
		return -EINVAL;
	dma_unmap_single(ledger->dev, addr, size, DMA_TO_DEVICE);
	memset(ledger->slots + slot, 0, size / IOMMU_DEBUG_DMA_GRANULE);
	ledger->nr_maps--;
	return 0;
}

static void iommu_debug_dma_ledger_destroy(struct iommu_debug_dma_ledger *ledger)
{
	u64 slot;

	for (slot = 0; slot < IOMMU_DEBUG_VA_LIMIT / IOMMU_DEBUG_DMA_GRANULE; slot++) {
		u8 nr = ledger->slots[slot];

		if (!(slot & 1023))
			cond_resched();
		if (!nr || nr == IOMMU_DEBUG_DMA_CONT)
			continue;
		iommu_debug_dma_ledger_unmap(ledger, slot * IOMMU_DEBUG_DMA_GRANULE,
					   nr * IOMMU_DEBUG_DMA_GRANULE);
	}
	kvfree(ledger->slots);
	ledger->slots = NULL;
}

static bool iommu_debug_msi_iova(u64 addr)
{
	return addr >= MSI_IOVA_BASE && addr < MSI_IOVA_BASE + MSI_IOVA_LENGTH;
}

static int iommu_debug_dma_expect_full(struct iommu_debug_dma_ledger *ledger,
					     void *virt, size_t size)
{
	dma_addr_t addr = dma_map_single(ledger->dev, virt, size, DMA_TO_DEVICE);

	if (dma_mapping_error(ledger->dev, addr))
		return 0;
	/* Even an unexpected successful probe must have a matching unmap. */
	dma_unmap_single(ledger->dev, addr, size, DMA_TO_DEVICE);
	return -EINVAL;
}

static int __full_va_sweep(struct device *dev, struct seq_file *s,
			   struct iommu_domain *domain, void *priv)
{
	const size_t size = (size_t)priv;
	struct iommu_debug_dma_ledger ledger;
	void *virt;
	phys_addr_t phys;
	u64 iova;
	int ret;

	virt = (void *)__get_free_pages(GFP_KERNEL, get_order(size));
	if (!virt)
		return -ENOMEM;
	ret = iommu_debug_dma_ledger_init(&ledger, dev);
	if (ret)
		goto free_buffer;
	phys = virt_to_phys(virt);
	for (iova = 0; iova < IOMMU_DEBUG_VA_LIMIT; iova += size) {
		cond_resched();
		if (iommu_debug_msi_iova(iova))
			continue;
		ret = iommu_debug_dma_ledger_map(&ledger, virt, size, iova);
		if (ret)
			goto out;
	}
	if (domain) {
		for (iova = 0; iova < min_t(u64, SZ_2M * 3, IOMMU_DEBUG_VA_LIMIT); iova += size) {
			u64 high = IOMMU_DEBUG_VA_LIMIT - size - iova;

			if (!iommu_debug_msi_iova(iova)) {
				ret = __check_mapping(dev, domain, iova, phys);
				if (ret)
					goto out;
			}
			if (!iommu_debug_msi_iova(high)) {
				ret = __check_mapping(dev, domain, high, phys);
				if (ret)
					goto out;
			}
		}
	}
	ret = iommu_debug_dma_expect_full(&ledger, virt, size);
out:
	iommu_debug_dma_ledger_destroy(&ledger);
free_buffer:
	free_pages((unsigned long)virt, get_order(size));
	return ret;
}

static int __tlb_stress_sweep(struct device *dev, struct seq_file *s,
			      struct iommu_domain *domain, void *unused)
{
	struct iommu_debug_dma_ledger ledger;
	void *virt;
	u64 iova;
	int ret;

	virt = (void *)__get_free_pages(GFP_KERNEL, get_order(SZ_8K));
	if (!virt)
		return -ENOMEM;
	ret = iommu_debug_dma_ledger_init(&ledger, dev);
	if (ret)
		goto free_buffer;
	/* Two initial 4K mappings allow exact DMA unmaps for the 4K hole. */
	ret = iommu_debug_dma_ledger_map(&ledger, virt, SZ_4K, 0);
	if (ret)
		goto out;
	ret = iommu_debug_dma_ledger_map(&ledger, virt, SZ_4K, SZ_4K);
	if (ret)
		goto out;
	for (iova = SZ_8K; iova < IOMMU_DEBUG_VA_LIMIT; iova += SZ_8K) {
		cond_resched();
		if (iommu_debug_msi_iova(iova))
			continue;
		ret = iommu_debug_dma_ledger_map(&ledger, virt, SZ_8K, iova);
		if (ret)
			goto out;
	}
	ret = iommu_debug_dma_expect_full(&ledger, virt, SZ_4K);
	if (ret)
		goto out;
	ret = iommu_debug_dma_ledger_unmap(&ledger, 0, SZ_4K);
	if (ret)
		goto out;
	ret = iommu_debug_dma_ledger_unmap(&ledger, SZ_8K, SZ_8K);
	if (ret)
		goto out;
	/* An 8K request must skip the 4K hole, then the 4K request fills it. */
	ret = iommu_debug_dma_ledger_map(&ledger, virt, SZ_8K, SZ_8K);
	if (ret)
		goto out;
	ret = iommu_debug_dma_ledger_map(&ledger, virt, SZ_4K, 0);
	if (ret)
		goto out;
	ret = iommu_debug_dma_expect_full(&ledger, virt, SZ_4K);
out:
	iommu_debug_dma_ledger_destroy(&ledger);
free_buffer:
	free_pages((unsigned long)virt, get_order(SZ_8K));
	return ret;
}

static int __rand_va_sweep(struct device *dev, struct seq_file *s,
			   struct iommu_domain *domain, void *priv)
{
	const size_t size = (size_t)priv;
	struct iommu_debug_dma_ledger ledger;
	void *virt;
	u64 iova, prev = 1, cur = 1;
	unsigned int unmapped = 0, remapped = 0;
	int ret;

	virt = (void *)__get_free_pages(GFP_KERNEL, get_order(size));
	if (!virt)
		return -ENOMEM;
	ret = iommu_debug_dma_ledger_init(&ledger, dev);
	if (ret)
		goto free_buffer;
	for (iova = 0; iova < IOMMU_DEBUG_VA_LIMIT; iova += size) {
		cond_resched();
		if (iommu_debug_msi_iova(iova))
			continue;
		ret = iommu_debug_dma_ledger_map(&ledger, virt, size, iova);
		if (ret)
			goto out;
	}
	for (;;) {
		u64 next = prev + cur, other;

		prev = cur;
		cur = next;
		if (cur >= (IOMMU_DEBUG_VA_LIMIT - size) / size)
			break;
		iova = cur * size;
		other = IOMMU_DEBUG_VA_LIMIT - size - iova;
		if (!iommu_debug_msi_iova(iova) &&
		    ledger.slots[iova / IOMMU_DEBUG_DMA_GRANULE]) {
			ret = iommu_debug_dma_ledger_unmap(&ledger, iova, size);
			if (ret)
				goto out;
			unmapped++;
		}
		if (!iommu_debug_msi_iova(other) &&
		    ledger.slots[other / IOMMU_DEBUG_DMA_GRANULE]) {
			ret = iommu_debug_dma_ledger_unmap(&ledger, other, size);
			if (ret)
				goto out;
			unmapped++;
		}
	}
	while (remapped < unmapped) {
		ret = iommu_debug_dma_ledger_map(&ledger, virt, size, U64_MAX);
		if (ret)
			goto out;
		remapped++;
	}
	ret = iommu_debug_dma_expect_full(&ledger, virt, size);
out:
	iommu_debug_dma_ledger_destroy(&ledger);
free_buffer:
	free_pages((unsigned long)virt, get_order(size));
	return ret;
}

static int __functional_dma_api_va_test(struct seq_file *s)
{
	int ret = 0;
	size_t *sz;
	size_t sizes[] = {SZ_4K, SZ_8K, SZ_16K, SZ_64K, 0};
	struct iommu_debug_device *ddev = s->private;
	char *usecase_name;

	/*
	 * dev_name() cannot be used to get the usecase name as ddev->test_dev
	 * will be NULL in case __apply_to_new_mapping() fails. Since
	 * ddev->test_dev changes across calls to __apply_to_new_mapping(), we
	 * also can't hold a reference to its name by caching the result of
	 * dev_name() initially.
	 */
	lockdep_assert_held(&ddev->state_lock);
	if (!ddev->test_dev) {
		return -ENODEV;
	}

	usecase_name = kstrdup(dev_name(ddev->test_dev), GFP_KERNEL);
	if (!usecase_name)
		return -ENOMEM;

	for (sz = sizes; *sz; ++sz) {
		ps_printf(usecase_name, s, "Full VA sweep @%s:", _size_to_string(*sz));
		if (__apply_to_new_mapping(s, __full_va_sweep, (void *)*sz)) {
			ps_printf(usecase_name, s, "  -> FAILED\n");
			ret = -EINVAL;
		} else {
			ps_printf(usecase_name, s, "  -> SUCCEEDED\n");
		}
	}

	ps_printf(usecase_name, s, "bonus map:");
	if (__apply_to_new_mapping(s, __full_va_sweep, (void *)SZ_4K)) {
		ps_printf(usecase_name, s, "  -> FAILED\n");
		ret = -EINVAL;
	} else {
		ps_printf(usecase_name, s, "  -> SUCCEEDED\n");
	}

	for (sz = sizes; *sz; ++sz) {
		ps_printf(usecase_name, s, "Rand VA sweep @%s:", _size_to_string(*sz));
		if (__apply_to_new_mapping(s, __rand_va_sweep, (void *)*sz)) {
			ps_printf(usecase_name, s, "  -> FAILED\n");
			ret = -EINVAL;
		} else {
			ps_printf(usecase_name, s, "  -> SUCCEEDED\n");
		}
	}

	ps_printf(usecase_name, s, "TLB stress sweep:");
	if (__apply_to_new_mapping(s, __tlb_stress_sweep, NULL)) {
		ps_printf(usecase_name, s, "  -> FAILED\n");
		ret = -EINVAL;
	} else {
		ps_printf(usecase_name, s, "  -> SUCCEEDED\n");
	}

	ps_printf(usecase_name, s, "second bonus map:");
	if (__apply_to_new_mapping(s, __full_va_sweep, (void *)SZ_4K)) {
		ps_printf(usecase_name, s, "  -> FAILED\n");
		ret = -EINVAL;
	} else {
		ps_printf(usecase_name, s, "  -> SUCCEEDED\n");
	}

	kfree(usecase_name);
	return ret;
}

static int iommu_debug_functional_fast_dma_api_show(struct seq_file *s,
						    void *ignored)
{
	int ret = 0;
	struct iommu_debug_device *ddev = s->private;

	mutex_lock(&ddev->state_lock);
	if (!ddev->test_dev) {
		ret = -EINVAL;
		goto out;
	}
	if (!ddev->fastmap_usecase) {
		ps_printf(dev_name(ddev->test_dev), s, "Not a fastmap usecase\n");
		goto out;
	}
	if (!IS_ENABLED(CONFIG_IOMMU_IO_PGTABLE_FAST)) {
		ps_printf(dev_name(ddev->test_dev), s, "CONFIG_IOMMU_IO_PGTABLE_FAST not enabled\n");
		goto out;
	}
	/* Keep the selected device/usecase stable across every reset and subtest. */
	ret = __apply_to_new_mapping(s, __functional_dma_api_alloc_test, NULL);
	ret |= __apply_to_new_mapping(s, __functional_dma_api_basic_test, NULL);
	ret |= __functional_dma_api_va_test(s);
out:
	mutex_unlock(&ddev->state_lock);
	return ret;
}

static int iommu_debug_functional_fast_dma_api_open(struct inode *inode,
						    struct file *file)
{
	return single_open(file, iommu_debug_functional_fast_dma_api_show,
			   inode->i_private);
}

const struct file_operations iommu_debug_functional_fast_dma_api_fops = {
	.owner = THIS_MODULE,
	.open	 = iommu_debug_functional_fast_dma_api_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static ssize_t iommu_debug_test_virt_addr_read(struct file *file,
					       char __user *ubuf,
					       size_t count, loff_t *offset)
{
	struct iommu_debug_device *ddev = file->private_data;
	char buf[100];
	int len;

	if (*offset)
		return 0;
	mutex_lock(&ddev->state_lock);
	if (!ddev->test_virt_addr)
		ddev->test_virt_addr = kzalloc(SZ_1M, GFP_KERNEL);
	if (!ddev->test_virt_addr)
		len = strscpy(buf, "FAIL\n", sizeof(buf));
	else
		/* This root-only interface supplies the address used by dma_map. */
		len = scnprintf(buf, sizeof(buf), "0x%px\n", ddev->test_virt_addr);
	mutex_unlock(&ddev->state_lock);
	return simple_read_from_buffer(ubuf, count, offset, buf, len);
}

const struct file_operations iommu_debug_test_virt_addr_fops = {
	.owner = THIS_MODULE,
	.open	= simple_open,
	.read	= iommu_debug_test_virt_addr_read,
};

#ifdef CONFIG_IOMMU_IOVA_ALIGNMENT
static unsigned long iommu_debug_get_align_mask(size_t size)
{
	unsigned long align_mask = ~0UL;

	align_mask <<= min_t(unsigned long, CONFIG_IOMMU_IOVA_ALIGNMENT + PAGE_SHIFT,
			     fls_long(size - 1));
	return ~align_mask;
}
#else
static unsigned long iommu_debug_get_align_mask(size_t size)
{
	unsigned long align_mask = ~0UL;

	align_mask <<= fls_long(size - 1);
	return ~align_mask;
}
#endif

struct iommu_debug_retained_mapping {
	struct list_head node;
	struct iommu_domain *domain;
	unsigned long iova;
	size_t size;
	struct sg_table table;
};

/* The range must have been empty before this test's mapping attempt. */
static bool iommu_debug_owned_range_empty(struct iommu_domain *domain,
					 unsigned long iova, size_t size)
{
	size_t offset;

	for (offset = 0; offset < size; offset += SZ_4K)
		if (iommu_iova_to_phys(domain, iova + offset))
			return false;
	return true;
}

static int iommu_debug_retained_unmap(struct iommu_debug_retained_mapping *record)
{
	if (iommu_debug_owned_range_empty(record->domain, record->iova, record->size))
		return 0;
	/* The IOMMU API forbids subdividing a prior mapping. Retry its exact tuple. */
	iommu_unmap(record->domain, record->iova, record->size);
	return iommu_debug_owned_range_empty(record->domain, record->iova, record->size)
		? 0 : -EBUSY;
}

/* state_lock held; a failed unmap blocks reset or retains backing at removal. */
int iommu_debug_retained_cleanup(struct iommu_debug_device *ddev, bool removing)
{
	struct iommu_debug_retained_mapping *record, *next;
	int ret = 0;

	list_for_each_entry_safe(record, next, &ddev->retained_sg, node) {
		int err = iommu_debug_retained_unmap(record);

		if (err && !removing) {
			ret = err;
			continue;
		}
		list_del(&record->node);
		if (record->table.sgl) {
			if (err) {
				/* An uncertain IOMMU mapping must never point at freed pages. */
				dev_err(ddev->self, "IOMMU unmap failed; retaining test backing pages until reboot\n");
				sg_free_table(&record->table);
			} else {
				iommu_debug_destroy_phoney_sg_table(ddev->self, &record->table, SZ_4K);
			}
		} else if (err) {
			dev_err(ddev->self, "IOMMU unmap failed for diagnostic physical range\n");
		}
		kfree(record);
	}
	return ret;
}

static int iommu_debug_device_profiling(struct seq_file *s, struct iommu_debug_device *ddev,
					const size_t sizes[])
{
	struct iommu_domain *domain;
	struct device *dev;
	const unsigned long base_iova = 0x10000;
	const phys_addr_t base_paddr = 0x80000000;
	const size_t *sz;
	u32 iterations;
	int ret = -EINVAL, kind;

	mutex_lock(&ddev->state_lock);
	if (!iommu_debug_usecase_reset(ddev))
		goto out;
	domain = ddev->domain;
	dev = ddev->test_dev;
	iterations = READ_ONCE(ddev->nr_iters);
	if (!iterations)
		goto out;
	seq_printf(s, "(average over %u iterations)\n", iterations);
	for (kind = 0; kind < 2; kind++) {
		seq_printf(s, "%8s %19s %16s\n", "size", kind ? "iommu_map_sg" : "iommu_map", "iommu_unmap");
		for (sz = sizes; *sz; sz++) {
			struct iommu_debug_retained_mapping *record;
			size_t size = *sz;
			u64 map_elapsed_ns = 0, unmap_elapsed_ns = 0;
			u64 map_elapsed_us, unmap_elapsed_us;
			u32 map_rem, unmap_rem, i;

			record = kzalloc(sizeof(*record), GFP_KERNEL);
			if (!record) {
				ret = -ENOMEM;
				goto out;
			}
			record->domain = domain;
			record->iova = __ALIGN_MASK(base_iova, iommu_debug_get_align_mask(size));
			record->size = size;
			if (kind) {
				ret = iommu_debug_build_phoney_sg_table(dev, &record->table, size, SZ_4K);
				if (ret) {
					kfree(record);
					goto out;
				}
			}
			/* Never acquire ownership of an existing mapping at this IOVA. */
			if (!iommu_debug_owned_range_empty(domain, record->iova, size)) {
				if (kind)
					iommu_debug_destroy_phoney_sg_table(dev, &record->table, SZ_4K);
				kfree(record);
				ret = -EBUSY;
				goto out;
			}
			list_add_tail(&record->node, &ddev->retained_sg);
			for (i = 0; i < iterations; i++) {
				ktime_t before;
				size_t unmapped;
				ssize_t mapped;

				before = ktime_get();
				if (kind) {
					mapped = iommu_map_sgtable(domain, record->iova,
							   &record->table, IOMMU_READ | IOMMU_WRITE);
					ret = mapped == size ? 0 : mapped < 0 ? mapped : -EIO;
				} else {
					ret = iommu_map(domain, record->iova, ALIGN(base_paddr, size),
							size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
				}
				if (ret)
					goto mapping_failed;
				map_elapsed_ns += ktime_to_ns(ktime_sub(ktime_get(), before));
				before = ktime_get();
				unmapped = iommu_unmap(domain, record->iova, size);
				if (unmapped != size) {
					ret = -EIO;
					goto mapping_failed;
				}
				unmap_elapsed_ns += ktime_to_ns(ktime_sub(ktime_get(), before));
				cond_resched();
			}
			map_elapsed_ns = div_u64(map_elapsed_ns, iterations);
			unmap_elapsed_ns = div_u64(unmap_elapsed_ns, iterations);
			map_elapsed_us = div_u64_rem(map_elapsed_ns, 1000, &map_rem);
			unmap_elapsed_us = div_u64_rem(unmap_elapsed_ns, 1000, &unmap_rem);
			seq_printf(s, "%8s %12llu.%03u us %9llu.%03u us\n",
				   _size_to_string(size), map_elapsed_us, map_rem, unmap_elapsed_us, unmap_rem);
			ret = iommu_debug_retained_cleanup(ddev, false);
			if (ret)
				goto out;
		}
		seq_putc(s, '\n');
	}
	ret = 0;
	goto out;
mapping_failed:
	seq_printf(s, "Mapping/unmapping failed: %d\n", ret);
	/* Cleanup may retain pages and block a reset if the provider cannot unmap. */
	iommu_debug_retained_cleanup(ddev, false);
out:
	mutex_unlock(&ddev->state_lock);
	return ret;
}

static int iommu_debug_profiling_show(struct seq_file *s, void *ignored)
{
	struct iommu_debug_device *ddev = s->private;
	const size_t sizes[] = { SZ_4K, SZ_64K, SZ_1M, SZ_2M, SZ_1M * 12,
				SZ_1M * 24, SZ_1M * 32, 0 };

	return iommu_debug_device_profiling(s, ddev, sizes);
}

static int iommu_debug_profiling_open(struct inode *inode, struct file *file)
{
	return single_open(file, iommu_debug_profiling_show, inode->i_private);
}

const struct file_operations iommu_debug_profiling_fops = {
	.owner = THIS_MODULE,
	.open	 = iommu_debug_profiling_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};
