// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012,2014-2017,2019-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mempool.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/version.h>
#include <linux/srcu.h>
#include <linux/mutex.h>
#include <linux/limits.h>
#include "cnss_common.h"
#ifdef CONFIG_CNSS_OUT_OF_TREE
#include "cnss_prealloc.h"
#else
#include <net/cnss_prealloc.h>
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CNSS prealloc driver");

#ifdef CONFIG_CNSS2_DEBUG
#define CNSS_ASSERT(_condition) do {					\
		if (!(_condition)) {					\
			pr_err("ASSERT at line %d\n",			\
			       __LINE__);				\
			BUG();						\
		}							\
	} while (0)
#else
#define CNSS_ASSERT(_condition) do {					\
		if (!(_condition)) {					\
			pr_err("ASSERT at line %d\n",			\
			       __LINE__);				\
			WARN_ON(1);					\
		}							\
	} while (0)
#endif


/* cnss preallocation scheme is a memory pool that always tries to keep a
 * list of free memory for use in emergencies. It is implemented on kernel
 * features: memorypool and kmem cache.
 */

struct cnss_pool {
	size_t size;
	int min;
	const char name[50];
	mempool_t *mp;
	struct kmem_cache *cache;
	void **pool_ptrs;
	int table_capacity;
};

/**
 * Memory pool
 * -----------
 *
 * How to update this table:
 *
 *  1. Add a new row with following elements
 *      size  : Size of one allocation unit in bytes.
 *      min   : Minimum units to be reserved. Used only if a regular
 *              allocation fails.
 *      name  : Name of the cache/pool. Will be displayed in /proc/slabinfo
 *              if not merged with another pool.
 *      mp    : A pointer to memory pool. Updated during init.
 *      cache : A pointer to cache. Updated during init.
 *      pool_ptrs: A table to keep track of memory allocated from the pool.
 *      table_capacity: Total capacity of the tracker table for the pool.
 * 2. Always keep the table in increasing order
 * 3. Please keep the reserve pool as minimum as possible as it's always
 *    preallocated.
 * 4. Always profile with different use cases after updating this table.
 * 5. A dynamic view of this pool can be viewed at /proc/slabinfo.
 * 6. Each pool has a sys node at /sys/kernel/slab/<name>
 *
 */

/* size, min pool reserve, name, memorypool handler, cache handler*/
static struct cnss_pool cnss_pools_default[] = {
	{8 * 1024, 16, "cnss-pool-8k", NULL, NULL, NULL},
	{16 * 1024, 16, "cnss-pool-16k", NULL, NULL, NULL},
	{32 * 1024, 22, "cnss-pool-32k", NULL, NULL, NULL},
	{64 * 1024, 38, "cnss-pool-64k", NULL, NULL, NULL},
	{128 * 1024, 10, "cnss-pool-128k", NULL, NULL, NULL},
	{256 * 1024, 2, "cnss-pool-256k", NULL, NULL, NULL},
};

static struct cnss_pool cnss_pools_adrastea[] = {
	{8 * 1024, 2, "cnss-pool-8k", NULL, NULL, NULL},
	{16 * 1024, 10, "cnss-pool-16k", NULL, NULL, NULL},
	{32 * 1024, 8, "cnss-pool-32k", NULL, NULL, NULL},
	{64 * 1024, 4, "cnss-pool-64k", NULL, NULL, NULL},
	{128 * 1024, 2, "cnss-pool-128k", NULL, NULL, NULL},
};

static struct cnss_pool cnss_pools_wcn6750[] = {
	{8 * 1024, 2, "cnss-pool-8k", NULL, NULL, NULL},
	{16 * 1024, 8, "cnss-pool-16k", NULL, NULL, NULL},
	{32 * 1024, 11, "cnss-pool-32k", NULL, NULL, NULL},
	{64 * 1024, 15, "cnss-pool-64k", NULL, NULL, NULL},
	{128 * 1024, 4, "cnss-pool-128k", NULL, NULL, NULL},
};

static struct cnss_pool cnss_pools_wcn7750[] = {
	{8 * 1024, 2, "cnss-pool-8k", NULL, NULL},
	{16 * 1024, 8, "cnss-pool-16k", NULL, NULL},
	{32 * 1024, 11, "cnss-pool-32k", NULL, NULL},
	{64 * 1024, 15, "cnss-pool-64k", NULL, NULL},
	{128 * 1024, 4, "cnss-pool-128k", NULL, NULL},
};

static struct cnss_pool *cnss_pools;
static unsigned int cnss_prealloc_pool_size = ARRAY_SIZE(cnss_pools_default);
static DEFINE_SPINLOCK(pool_table_lock);
static DEFINE_MUTEX(pool_lifecycle_lock);
DEFINE_STATIC_SRCU(pool_srcu);
static bool mempool_initialization_done;
static bool pool_storage_available;
static unsigned long pool_device_id;

/**
 * cnss_pool_alloc_threshold() - Allocation threshold
 *
 * Minimum memory size to be part of cnss pool.
 *
 * Return: Size
 *
 */
static inline size_t cnss_pool_alloc_threshold(void)
{
	return cnss_pools[0].size;
}

/**
 * cnss_pool_int() - Initialize memory pools.
 *
 * Create cnss pools as configured by cnss_pools[]. It is the responsibility of
 * the caller to invoke cnss_pool_deinit() routine to clean it up. This
 * function needs to be called at early boot to preallocate minimum buffers in
 * the pool.
 *
 * Return: 0 - success, otherwise error code.
 *
 */
static int cnss_pool_init(void)
{
	int i;

	for (i = 0; i < cnss_prealloc_pool_size; i++) {
		struct cnss_pool *pool = &cnss_pools[i];

		pool->cache = kmem_cache_create_usercopy(pool->name, pool->size,
							 0, SLAB_ACCOUNT, 0, pool->size, NULL);
		if (!pool->cache)
			goto fail;
		pool->mp = mempool_create(pool->min, mempool_alloc_slab,
					  mempool_free_slab, pool->cache);
		if (!pool->mp)
			goto fail;
		pool->pool_ptrs = kcalloc(pool->min, sizeof(void *), GFP_KERNEL);
		if (!pool->pool_ptrs)
			goto fail;
		pool->table_capacity = pool->min;
	}
	return 0;
fail:
	for (; i >= 0; i--) {
		struct cnss_pool *pool = &cnss_pools[i];

		mempool_destroy(pool->mp);
		kmem_cache_destroy(pool->cache);
		kfree(pool->pool_ptrs);
		pool->mp = NULL;
		pool->cache = NULL;
		pool->pool_ptrs = NULL;
		pool->table_capacity = 0;
	}
	return -ENOMEM;
}

/**
 * cnss_pool_deinit() - Free memory pools.
 *
 * Free the memory pools and return resources back to the system. It warns
 * if there is any pending element in memory pool or cache.
 *
 */
static void cnss_pool_deinit(void)
{
	int i, j;
	unsigned long flags;
	bool outstanding = false;

	if (!pool_storage_available)
		return;
	/* Close allocation access before draining old borrowers. */
	smp_store_release(&mempool_initialization_done, false);
	synchronize_srcu(&pool_srcu);
	spin_lock_irqsave(&pool_table_lock, flags);
	for (i = 0; i < cnss_prealloc_pool_size; i++)
		for (j = 0; j < cnss_pools[i].table_capacity; j++)
			if (cnss_pools[i].pool_ptrs[j])
				outstanding = true;
	spin_unlock_irqrestore(&pool_table_lock, flags);
	if (outstanding) {
		pr_warn("cnss_prealloc: retaining pools until borrowers return storage\n");
		return;
	}
	/* Withdraw return-side access before freeing pool storage. */
	smp_store_release(&pool_storage_available, false);
	synchronize_srcu(&pool_srcu);
	for (i = 0; i < cnss_prealloc_pool_size; i++) {
		mempool_destroy(cnss_pools[i].mp);
		kmem_cache_destroy(cnss_pools[i].cache);
		kfree(cnss_pools[i].pool_ptrs);
		cnss_pools[i].mp = NULL;
		cnss_pools[i].cache = NULL;
		cnss_pools[i].pool_ptrs = NULL;
		cnss_pools[i].table_capacity = 0;
	}
}

static void cnss_assign_prealloc_pool(unsigned long device_id)
{
	pr_info("cnss_prealloc: assign cnss pool for device id 0x%lx", device_id);

	switch (device_id) {
	case ADRASTEA_DEVICE_ID:
		cnss_pools = cnss_pools_adrastea;
		cnss_prealloc_pool_size = ARRAY_SIZE(cnss_pools_adrastea);
		break;
	case WCN6750_DEVICE_ID:
		cnss_pools = cnss_pools_wcn6750;
		cnss_prealloc_pool_size = ARRAY_SIZE(cnss_pools_wcn6750);
		break;
	case WCN7750_DEVICE_ID:
		cnss_pools = cnss_pools_wcn7750;
		cnss_prealloc_pool_size = ARRAY_SIZE(cnss_pools_wcn7750);
		break;
	case WCN6450_DEVICE_ID:
	case QCA6390_DEVICE_ID:
	case QCA6490_DEVICE_ID:
	case MANGO_DEVICE_ID:
	case PEACH_DEVICE_ID:
	case KIWI_DEVICE_ID:
	case FIG_DEVICE_ID:
	default:
		cnss_pools = cnss_pools_default;
		cnss_prealloc_pool_size = ARRAY_SIZE(cnss_pools_default);
	}
}

void cnss_initialize_prealloc_pool(unsigned long device_id)
{
	mutex_lock(&pool_lifecycle_lock);
	if (pool_storage_available) {
		if (device_id == pool_device_id) {
			/* Reopen allocation after publishing the retained pool state. */
			smp_store_release(&mempool_initialization_done, true);
		} else {
			pr_warn("cnss_prealloc: cannot replace pools with active borrowers\n");
		}
		goto out;
	}
	cnss_assign_prealloc_pool(device_id);
	if (cnss_pool_init()) {
		pr_err("cnss_prealloc: pool initialization failed\n");
		goto out;
	}
	pool_device_id = device_id;
	/* Publish fully initialized tables to return-side SRCU readers. */
	smp_store_release(&pool_storage_available, true);
	/* Publish fully initialized pools to allocation-side SRCU readers. */
	smp_store_release(&mempool_initialization_done, true);
out:
	mutex_unlock(&pool_lifecycle_lock);
}
EXPORT_SYMBOL(cnss_initialize_prealloc_pool);

void cnss_deinitialize_prealloc_pool(void)
{
	mutex_lock(&pool_lifecycle_lock);
	cnss_pool_deinit();
	mutex_unlock(&pool_lifecycle_lock);
}
EXPORT_SYMBOL(cnss_deinitialize_prealloc_pool);

void wcnss_check_pool_lists(void)
{
	int i, j, idx;
	unsigned long flags;

	idx = srcu_read_lock(&pool_srcu);
	/* Pair with storage publication before inspecting pool pointers. */
	if (!smp_load_acquire(&pool_storage_available))
		goto out;
	spin_lock_irqsave(&pool_table_lock, flags);
	for (i = 0; i < cnss_prealloc_pool_size; i++)
		for (j = 0; j < cnss_pools[i].table_capacity; j++)
			if (cnss_pools[i].pool_ptrs[j])
				pr_warn("cnss_prealloc: outstanding pointer in %s slot %d\n",
					cnss_pools[i].name, j);
	spin_unlock_irqrestore(&pool_table_lock, flags);
out:
	srcu_read_unlock(&pool_srcu, idx);
}
EXPORT_SYMBOL(wcnss_check_pool_lists);

static int wcnss_find_pool_table_slot(int pool, void *mem)
{
	struct cnss_pool *entry = &cnss_pools[pool];
	void **new_table;
	int i, new_capacity;

	for (i = 0; i < entry->table_capacity; i++) {
		if (!entry->pool_ptrs[i]) {
			entry->pool_ptrs[i] = mem;
			return 0;
		}
	}
	if (entry->table_capacity == INT_MAX)
		return -EOVERFLOW;
	new_capacity = entry->table_capacity + 1;
	new_table = krealloc_array(entry->pool_ptrs, new_capacity,
				   sizeof(void *), GFP_ATOMIC);
	if (!new_table)
		return -ENOMEM;
	new_table[i] = mem;
	entry->pool_ptrs = new_table;
	entry->table_capacity = new_capacity;
	return 0;
}

static int wcnss_free_pool_table_slot(struct cnss_pool mempool, void *mem)
{
	void **pool_table;
	size_t ptr_idx;

	pool_table = mempool.pool_ptrs;
	for (ptr_idx = 0; ptr_idx < mempool.table_capacity; ptr_idx++) {
		if (pool_table[ptr_idx] == mem) {
			pool_table[ptr_idx] = NULL;
			return ptr_idx;
		}
	}

	pr_debug("wcnss prealloc put ptr %p not found in %s pool mem addr %p\n",
		 mem, mempool.name, mempool.pool_ptrs);

	return -EPERM;
}

/**
 * wcnss_prealloc_get() - Get preallocated memory from a pool
 * @size: Size to allocate
 *
 * Memory pool is chosen based on the size. If memory is not available in a
 * given pool it goes to next higher sized pool until it succeeds.
 *
 * Return: A void pointer to allocated memory
 */
void *wcnss_prealloc_get(size_t size)
{
	void *mem = NULL;
	gfp_t gfp_mask;
	unsigned long irq_flags;
	int i, idx;

	idx = srcu_read_lock(&pool_srcu);
	/* Pair with pool publication before borrowing its allocator state. */
	if (!smp_load_acquire(&mempool_initialization_done))
		goto out;
	if (!size || size < cnss_pool_alloc_threshold())
		goto out;
	gfp_mask = in_interrupt() || !preemptible() || rcu_preempt_depth() ?
		GFP_ATOMIC : GFP_KERNEL;
	for (i = 0; i < cnss_prealloc_pool_size; i++) {
		if (cnss_pools[i].size < size)
			continue;
		mem = mempool_alloc(cnss_pools[i].mp, gfp_mask);
		if (!mem)
			continue;
		spin_lock_irqsave(&pool_table_lock, irq_flags);
		if (wcnss_find_pool_table_slot(i, mem)) {
			spin_unlock_irqrestore(&pool_table_lock, irq_flags);
			mempool_free(mem, cnss_pools[i].mp);
			mem = NULL;
			break;
		}
		spin_unlock_irqrestore(&pool_table_lock, irq_flags);
		memset(mem, 0, cnss_pools[i].size);
		__module_get(THIS_MODULE);
		break;
	}
out:
	srcu_read_unlock(&pool_srcu, idx);
	return mem;
}
EXPORT_SYMBOL(wcnss_prealloc_get);

int wcnss_prealloc_put(void *mem)
{
	int i, idx, ret = 0;
	unsigned long irq_flags;

	if (!mem)
		return 0;
	idx = srcu_read_lock(&pool_srcu);
	/* Pair with storage publication before inspecting pool pointers. */
	if (!smp_load_acquire(&pool_storage_available))
		goto out;
	for (i = 0; i < cnss_prealloc_pool_size; i++) {
		spin_lock_irqsave(&pool_table_lock, irq_flags);
		ret = wcnss_free_pool_table_slot(cnss_pools[i], mem);
		spin_unlock_irqrestore(&pool_table_lock, irq_flags);
		if (ret >= 0) {
			mempool_free(mem, cnss_pools[i].mp);
			ret = 1;
			goto out;
		}
	}
	ret = 0;
out:
	srcu_read_unlock(&pool_srcu, idx);
	if (ret == 1)
		module_put(THIS_MODULE);
	return ret;
}
EXPORT_SYMBOL(wcnss_prealloc_put);


/* Not implemented. Make use of Linux SLAB features. */
void wcnss_prealloc_check_memory_leak(void) {}
EXPORT_SYMBOL(wcnss_prealloc_check_memory_leak);

/* Not implemented. Make use of Linux SLAB features. */
int wcnss_pre_alloc_reset(void) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(wcnss_pre_alloc_reset);

/**
 * cnss_prealloc_is_valid_dt_node_found - Check if valid device tree node
 *                                        present
 *
 * Valid device tree node means a node with "qcom,wlan" property present
 * and "status" property not disabled.
 *
 * Return: true if valid device tree node found, false if not found
 */
static bool cnss_prealloc_is_valid_dt_node_found(void)
{
	struct device_node *dn = NULL;

	for_each_node_with_property(dn, "qcom,wlan") {
		if (of_device_is_available(dn))
			break;
	}

	if (dn) {
		of_node_put(dn);
		return true;
	}

	return false;
}

static int __init cnss_prealloc_init(void)
{
	if (!cnss_prealloc_is_valid_dt_node_found())
		return -ENODEV;

	return 0;
}

static void __exit cnss_prealloc_exit(void)
{
	cnss_deinitialize_prealloc_pool();
}

module_init(cnss_prealloc_init);
module_exit(cnss_prealloc_exit);

