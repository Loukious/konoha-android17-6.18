// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/slab.h>

#include <linux/qcom_dma_heap.h>
#include <linux/qcom_tvm_heap.h>
#include "qcom_cma_heap.h"
#include "qcom_dt_parser.h"
#include "qcom_system_heap.h"
#include "qcom_carveout_heap.h"
#include "qcom_secure_system_heap.h"
#include "qcom_dma_heap_priv.h"
#include "qcom_system_movable_heap.h"

/* The registered heaps and their callbacks remain for the kernel lifetime. */
struct qcom_heap_stats {
	struct list_head list;
	struct dma_heap *heap;
	long (*get_pool_size)(struct dma_heap *heap);
};

static LIST_HEAD(heap_stats_list);
static DEFINE_MUTEX(heap_stats_lock);
static struct kobject *dma_heap_kobject;
static const struct file_operations *dma_buf_cached_fops;

struct dma_heap *qcom_dma_heap_add(const struct dma_heap_export_info *exp_info,
				   long (*get_pool_size)(struct dma_heap *heap))
{
	struct qcom_heap_stats *stats;
	struct dma_heap *heap;

	stats = kzalloc(sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return ERR_PTR(-ENOMEM);

	heap = dma_heap_add(exp_info);
	if (IS_ERR(heap)) {
		kfree(stats);
		return heap;
	}

	stats->heap = heap;
	stats->get_pool_size = get_pool_size;
	mutex_lock(&heap_stats_lock);
	list_add_tail(&stats->list, &heap_stats_list);
	mutex_unlock(&heap_stats_lock);
	return heap;
}

static u64 qcom_heap_pool_size_kb(void)
{
	struct qcom_heap_stats *stats;
	u64 total = 0;

	lockdep_assert_held(&heap_stats_lock);
	list_for_each_entry(stats, &heap_stats_list, list)
		total += stats->get_pool_size(stats->heap);

	return total / 1024;
}

long dma_heap_try_get_pool_size_kb(void)
{
	long total;

	if (!mutex_trylock(&heap_stats_lock))
		return -1;

	total = qcom_heap_pool_size_kb();
	mutex_unlock(&heap_stats_lock);
	return total;
}
EXPORT_SYMBOL_GPL(dma_heap_try_get_pool_size_kb);

static ssize_t total_pools_kb_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	u64 total;

	mutex_lock(&heap_stats_lock);
	total = qcom_heap_pool_size_kb();
	mutex_unlock(&heap_stats_lock);
	return sysfs_emit(buf, "%llu\n", total);
}

static struct kobj_attribute total_pools_kb_attr = __ATTR_RO(total_pools_kb);

static int qcom_dma_heap_probe(struct platform_device *pdev)
{
	int ret = 0;
	int i;
	struct platform_data *heaps;

	if (!mem_buf_is_ready())
		return -EPROBE_DEFER;

	heaps = parse_heap_dt(pdev);
	if (IS_ERR(heaps))
		return PTR_ERR(heaps);

	qcom_system_heap_create("qcom,system", "system", false);
#ifdef CONFIG_QCOM_DMABUF_HEAPS_SYSTEM_UNCACHED
	qcom_system_heap_create("qcom,system-uncached", NULL, true);
#endif
	qcom_secure_system_heap_create("qcom,secure-pixel", NULL,
				       QCOM_DMA_HEAP_FLAG_CP_PIXEL);
	qcom_secure_system_heap_create("qcom,secure-non-pixel", NULL,
				       QCOM_DMA_HEAP_FLAG_CP_NON_PIXEL);
	qcom_sys_movable_heap_create();

	for (i = 0; i < heaps->nr; i++) {
		struct platform_heap *heap_data = &heaps->heaps[i];

		switch (heap_data->type) {
		case HEAP_TYPE_SECURE_CARVEOUT:
			ret = qcom_secure_carveout_heap_create(heap_data);
			break;
		case HEAP_TYPE_CARVEOUT:
			ret = qcom_carveout_heap_create(heap_data);
			break;
		case HEAP_TYPE_CMA:
			ret = qcom_add_cma_heap(heap_data);
			break;
		case HEAP_TYPE_TVM_CARVEOUT:
			ret = qcom_tvm_carveout_heap_create(heap_data);
			break;
		default:
			pr_err("%s: Unknown heap type %u\n", __func__, heap_data->type);
			break;
		}

		if (ret)
			pr_err("%s: DMA-BUF Heap: Failed to create %s, error is %d\n",
			       __func__, heap_data->name, ret);
		else
			pr_info("%s: DMA-BUF Heap: Created %s\n", __func__,
				heap_data->name);
	}

	free_pdata(heaps);

	return ret;
}

void qcom_store_dma_buf_fops(struct file *file)
{
	cmpxchg(&dma_buf_cached_fops, NULL, file->f_op);
}
EXPORT_SYMBOL(qcom_store_dma_buf_fops);

bool qcom_is_dma_buf_file(struct file *file)
{
	return file->f_op == READ_ONCE(dma_buf_cached_fops);
}
EXPORT_SYMBOL(qcom_is_dma_buf_file);

static const struct of_device_id qcom_dma_heap_match_table[] = {
	{.compatible = "qcom,dma-heaps"},
	{},
};

static struct platform_driver qcom_dma_heap_driver = {
	.probe = qcom_dma_heap_probe,
	.driver = {
		.name = "qcom-dma-heap",
		.suppress_bind_attrs = true,
		.of_match_table = qcom_dma_heap_match_table,
	},
};

static int __init init_heap_driver(void)
{
	int ret;

	dma_heap_kobject = kobject_create_and_add("dma_heap", kernel_kobj);
	if (!dma_heap_kobject)
		return -ENOMEM;

	ret = sysfs_create_file(dma_heap_kobject, &total_pools_kb_attr.attr);
	if (ret)
		goto put_kobject;

	ret = platform_driver_register(&qcom_dma_heap_driver);
	if (!ret)
		return 0;

	sysfs_remove_file(dma_heap_kobject, &total_pools_kb_attr.attr);
put_kobject:
	kobject_put(dma_heap_kobject);
	return ret;
}
module_init(init_heap_driver);

MODULE_DESCRIPTION("Qualcomm DMA-BUF heaps");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_IMPORT_NS("DMA_BUF_HEAP");
