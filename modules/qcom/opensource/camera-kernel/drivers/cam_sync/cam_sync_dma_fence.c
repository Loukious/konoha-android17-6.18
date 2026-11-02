// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "cam_sync_dma_fence.h"
#include "cam_sync_util.h"
#include "cam_mem_mgr_api.h"

extern unsigned long cam_sync_monitor_mask;

/* The base must be first: dma_fence_free() frees it after an RCU grace period. */
struct cam_dma_fence {
	struct dma_fence base;
	spinlock_t lock;
};

struct cam_dma_fence_callback {
	struct dma_fence_cb cb;
	struct work_struct work;
	struct cam_dma_fence_device *dev;
	int row_idx;
	int status;
	u64 sync_generation;
};

/**
 * struct cam_dma_fence_row - DMA fence row
 */
struct cam_dma_fence_row {
	char                            name[CAM_DMA_FENCE_NAME_LEN];
	struct dma_fence               *fence;
	int32_t                         fd;
	enum cam_dma_fence_state        state;
	struct cam_dma_fence_callback  *callback;
	bool                            releasing;
	int32_t                         sync_obj;
	cam_sync_callback_for_dma_fence sync_cb;
	bool                            cb_registered_for_sync;
	bool                            ext_dma_fence;
	bool                            sync_signal_dma;
};

/**
 * struct cam_dma_fence_device - DMA fence device
 */
struct cam_dma_fence_device {
	uint64_t dma_fence_context;
	struct cam_dma_fence_row rows[CAM_DMA_FENCE_MAX_FENCES];
	spinlock_t row_spinlocks[CAM_DMA_FENCE_MAX_FENCES];
	struct mutex dev_lock;
	struct workqueue_struct *work_queue;
	DECLARE_BITMAP(bitmap, CAM_DMA_FENCE_MAX_FENCES);
	struct cam_generic_fence_monitor_data **monitor_data;
};

static atomic64_t g_cam_dma_fence_seq_no;
static struct cam_dma_fence_device *g_cam_dma_fence_dev;

static bool __cam_dma_fence_enable_signaling(
	struct dma_fence *fence)
{
	return true;
}

static const char *__cam_dma_fence_get_driver_name(
	struct dma_fence *fence)
{
	return "Camera DMA fence driver";
}

static void __cam_dma_fence_free(struct dma_fence *fence)
{
	/* A sync_file can retain this fence after the camera device is removed. */
	dma_fence_free(fence);
	module_put(THIS_MODULE);
}

static struct dma_fence_ops cam_sync_dma_fence_ops = {
	.enable_signaling = __cam_dma_fence_enable_signaling,
	.get_driver_name = __cam_dma_fence_get_driver_name,
	.get_timeline_name = __cam_dma_fence_get_driver_name,
	.release = __cam_dma_fence_free,
};

static inline struct cam_generic_fence_monitor_entry *
	__cam_dma_fence_get_monitor_entries(int idx)
{
	struct cam_generic_fence_monitor_data *monitor_data;

	monitor_data = CAM_GENERIC_MONITOR_GET_DATA(g_cam_dma_fence_dev->monitor_data, idx);
	if (monitor_data->swap_monitor_entries)
		return monitor_data->prev_monitor_entries;
	else
		return monitor_data->monitor_entries;
}

static inline struct cam_generic_fence_monitor_entry *
	__cam_dma_fence_get_prev_monitor_entries(int idx)
{
	struct cam_generic_fence_monitor_data *monitor_data;

	monitor_data = CAM_GENERIC_MONITOR_GET_DATA(g_cam_dma_fence_dev->monitor_data, idx);
	if (monitor_data->swap_monitor_entries)
		return monitor_data->monitor_entries;
	else
		return monitor_data->prev_monitor_entries;
}

static void __cam_dma_fence_print_table(void)
{
	int i;
	struct cam_dma_fence_row *row;
	struct dma_fence *fence;

	/* Index zero is marked as an invalid slot */
	for (i = 1; i < CAM_DMA_FENCE_MAX_FENCES; i++) {
		spin_lock_bh(&g_cam_dma_fence_dev->row_spinlocks[i]);
		row = &g_cam_dma_fence_dev->rows[i];

		/* free slots starting this index */
		if (row->state == CAM_DMA_FENCE_STATE_INVALID) {
			spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[i]);
			return;
		}

		fence = row->fence;
		CAM_INFO(CAM_DMA_FENCE,
			"Idx: %d seqno: %llu name: %s state: %d",
			i, fence->seqno, row->name, row->state);
		spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[i]);
	}
}

static int __cam_dma_fence_find_free_idx(uint32_t *idx)
{
	int rc = 0;
	bool bit;

	do {
		*idx = find_first_zero_bit(g_cam_dma_fence_dev->bitmap, CAM_DMA_FENCE_MAX_FENCES);
		if (*idx >=  CAM_DMA_FENCE_MAX_FENCES) {
			rc = -ENOMEM;
			break;
		}

		bit = test_and_set_bit(*idx, g_cam_dma_fence_dev->bitmap);
	} while (bit);

	if (rc) {
		CAM_ERR(CAM_DMA_FENCE, "No free idx, printing dma fence table......");
		__cam_dma_fence_print_table();
	}

	return rc;
}

static struct dma_fence *__cam_dma_fence_find_fence_in_table(
	int32_t fd, int32_t *idx)
{
	int i;
	struct dma_fence *fence = NULL;
	struct cam_dma_fence_row *row = NULL;

	for (i = 0; i < CAM_DMA_FENCE_MAX_FENCES; i++) {
		spin_lock_bh(&g_cam_dma_fence_dev->row_spinlocks[i]);

		row = &g_cam_dma_fence_dev->rows[i];
		if (row->state != CAM_DMA_FENCE_STATE_INVALID && !row->releasing &&
			row->fd == fd) {
			*idx = i;
			fence = row->fence;
			spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[i]);
			break;
		}
		spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[i]);
	}

	return fence;
}

static void __cam_dma_fence_init_row(const char *name,
	struct dma_fence *dma_fence, int32_t fd, uint32_t idx,
	bool ext_dma_fence)
{
	struct cam_dma_fence_row *row;

	spin_lock_bh(&g_cam_dma_fence_dev->row_spinlocks[idx]);
	row = &g_cam_dma_fence_dev->rows[idx];
	row->fence = dma_fence;
	row->fd = fd;
	row->state = CAM_DMA_FENCE_STATE_ACTIVE;
	row->ext_dma_fence = ext_dma_fence;
	strscpy(row->name, name, CAM_DMA_FENCE_NAME_LEN);

	if (test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE, &cam_sync_monitor_mask)) {
		cam_generic_fence_update_monitor_array(idx,
			&g_cam_dma_fence_dev->dev_lock, g_cam_dma_fence_dev->monitor_data,
			CAM_FENCE_OP_CREATE);
	}
	spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[idx]);
}

static void __cam_dma_fence_signal_work(struct work_struct *work)
{
	struct cam_dma_fence_callback *callback =
		container_of(work, struct cam_dma_fence_callback, work);
	struct cam_dma_fence_device *dev = callback->dev;
	struct cam_dma_fence_row *row = &dev->rows[callback->row_idx];
	struct cam_dma_fence_signal_sync_obj signal_sync_obj;
	cam_sync_callback_for_dma_fence sync_cb = NULL;
	int32_t sync_obj = 0;

	spin_lock_bh(&dev->row_spinlocks[callback->row_idx]);
	if (row->callback == callback && !row->sync_signal_dma &&
		!row->releasing && row->cb_registered_for_sync) {
		row->state = CAM_DMA_FENCE_STATE_SIGNALED;
		signal_sync_obj.fd = row->fd;
		signal_sync_obj.status = callback->status;
		signal_sync_obj.sync_generation = callback->sync_generation;
		sync_obj = row->sync_obj;
		sync_cb = row->sync_cb;
		if (test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE, &cam_sync_monitor_mask))
			cam_generic_fence_update_monitor_array(callback->row_idx,
				&dev->dev_lock, dev->monitor_data,
				CAM_FENCE_OP_UNREGISTER_ON_SIGNAL);
	}
	spin_unlock_bh(&dev->row_spinlocks[callback->row_idx]);

	if (sync_cb)
		sync_cb(sync_obj, &signal_sync_obj);
}

static void __cam_dma_fence_signal_cb(struct dma_fence *fence,
	struct dma_fence_cb *cb)
{
	struct cam_dma_fence_callback *callback =
		container_of(cb, struct cam_dma_fence_callback, cb);

	/* Fence callbacks run with the fence IRQ lock held. Do not take camera locks. */
	callback->status = dma_fence_get_status_locked(fence);
	queue_work(callback->dev->work_queue, &callback->work);
}

static void __cam_dma_fence_dump_monitor_array(int dma_row_idx)
{
	struct dma_fence *fence;
	struct cam_generic_fence_monitor_obj_info obj_info;
	struct cam_dma_fence_row *row;

	if (dma_row_idx <= 0 || dma_row_idx >= CAM_DMA_FENCE_MAX_FENCES ||
		!g_cam_dma_fence_dev->monitor_data ||
		!g_cam_dma_fence_dev->monitor_data[
			dma_row_idx / CAM_GENERIC_MONITOR_TABLE_ENTRY_SZ] ||
		!test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE, &cam_sync_monitor_mask))
		return;

	if (!CAM_GENERIC_MONITOR_GET_DATA(g_cam_dma_fence_dev->monitor_data,
		dma_row_idx)->prev_obj_id)
		return;

	row = &g_cam_dma_fence_dev->rows[dma_row_idx];
	fence = row->fence;

	obj_info.name = row->name;
	obj_info.obj_id = row->fd;
	obj_info.state = row->state;
	obj_info.ref_cnt = kref_read(&fence->refcount);
	obj_info.monitor_data = CAM_GENERIC_MONITOR_GET_DATA(
		g_cam_dma_fence_dev->monitor_data, dma_row_idx);
	obj_info.fence_type = CAM_GENERIC_FENCE_TYPE_DMA_FENCE;
	obj_info.sync_id = row->sync_obj;
	obj_info.monitor_entries =
		__cam_dma_fence_get_monitor_entries(dma_row_idx);
	obj_info.prev_monitor_entries =
		__cam_dma_fence_get_prev_monitor_entries(dma_row_idx);
	cam_generic_fence_dump_monitor_array(&obj_info);
}

struct dma_fence *cam_dma_fence_get_row_ref(int32_t row_idx, int32_t fd)
{
	struct cam_dma_fence_row *row;
	struct dma_fence *fence = ERR_PTR(-EINVAL);

	if (row_idx <= 0 || row_idx >= CAM_DMA_FENCE_MAX_FENCES)
		return fence;
	spin_lock_bh(&g_cam_dma_fence_dev->row_spinlocks[row_idx]);
	row = &g_cam_dma_fence_dev->rows[row_idx];
	if (row->state != CAM_DMA_FENCE_STATE_INVALID && !row->releasing && row->fd == fd)
		fence = dma_fence_get(row->fence);
	spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[row_idx]);
	return fence;
}

int cam_dma_fence_get_put_ref(bool get_or_put, int32_t row_idx)
{
	struct cam_dma_fence_row *row;
	struct dma_fence *fence;

	if (row_idx <= 0 || row_idx >= CAM_DMA_FENCE_MAX_FENCES)
		return -EINVAL;
	spin_lock_bh(&g_cam_dma_fence_dev->row_spinlocks[row_idx]);
	row = &g_cam_dma_fence_dev->rows[row_idx];
	if (row->state == CAM_DMA_FENCE_STATE_INVALID || row->releasing) {
		spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[row_idx]);
		return -EINVAL;
	}
	fence = row->fence;
	if (get_or_put)
		dma_fence_get(fence);
	else
		dma_fence_put(fence);
	spin_unlock_bh(&g_cam_dma_fence_dev->row_spinlocks[row_idx]);
	return 0;
}



struct dma_fence *cam_dma_fence_get_fence_from_fd(int32_t fd, int32_t *row_idx)
{
	struct cam_dma_fence_device *dev = g_cam_dma_fence_dev;
	struct dma_fence *fence;
	struct cam_dma_fence_row *row;
	uint32_t idx;
	int i, rc;

	if (!row_idx)
		return ERR_PTR(-EINVAL);
	/* An fd number can be reused while an earlier table entry still exists. */
	fence = sync_file_get_fence(fd);
	if (IS_ERR_OR_NULL(fence))
		return ERR_PTR(-EINVAL);
	mutex_lock(&dev->dev_lock);
	for (i = 1; i < CAM_DMA_FENCE_MAX_FENCES; i++) {
		spin_lock_bh(&dev->row_spinlocks[i]);
		row = &dev->rows[i];
		if (row->state != CAM_DMA_FENCE_STATE_INVALID && !row->releasing &&
			row->fd == fd && row->fence == fence) {
			*row_idx = i;
			spin_unlock_bh(&dev->row_spinlocks[i]);
			mutex_unlock(&dev->dev_lock);
			return fence;
		}
		spin_unlock_bh(&dev->row_spinlocks[i]);
	}
	rc = __cam_dma_fence_find_free_idx(&idx);
	if (rc) {
		mutex_unlock(&dev->dev_lock);
		dma_fence_put(fence);
		return ERR_PTR(rc);
	}
	__cam_dma_fence_init_row(fence->ops->get_driver_name(fence), fence, fd, idx, true);
	/* Keep separate table and caller references for a first-time import. */
	dma_fence_get(fence);
	*row_idx = idx;
	mutex_unlock(&dev->dev_lock);
	return fence;
}

int cam_dma_fence_register_cb(int32_t *sync_obj, int32_t *dma_fence_idx,
	cam_sync_callback_for_dma_fence sync_cb)
{
	struct cam_dma_fence_callback *callback;
	struct cam_dma_fence_device *dev = g_cam_dma_fence_dev;
	struct cam_dma_fence_row *row;
	u64 generation;
	int idx, rc = 0;

	if (!sync_obj || !dma_fence_idx || !sync_cb)
		return -EINVAL;
	idx = *dma_fence_idx;
	if (idx <= 0 || idx >= CAM_DMA_FENCE_MAX_FENCES)
		return -EINVAL;
	generation = cam_sync_get_generation(*sync_obj);
	if (!generation)
		return -EINVAL;
	callback = kzalloc(sizeof(*callback), GFP_KERNEL);
	if (!callback)
		return -ENOMEM;
	callback->dev = dev;
	callback->row_idx = idx;
	callback->sync_generation = generation;
	INIT_WORK(&callback->work, __cam_dma_fence_signal_work);

	spin_lock_bh(&dev->row_spinlocks[idx]);
	row = &dev->rows[idx];
	if (row->state != CAM_DMA_FENCE_STATE_ACTIVE || row->releasing) {
		rc = -EINVAL;
		goto out;
	}
	if (row->cb_registered_for_sync)
		goto out;

	/* Publish the complete callback before the provider can queue its work. */
	row->callback = callback;
	row->sync_obj = *sync_obj;
	row->sync_cb = sync_cb;
	row->cb_registered_for_sync = true;
	rc = dma_fence_add_callback(row->fence, &callback->cb, __cam_dma_fence_signal_cb);
	if (rc == -ENOENT) {
		/* Already-signaled imports must still notify the new sync object. */
		callback->status = dma_fence_get_status(row->fence);
		queue_work(dev->work_queue, &callback->work);
		rc = 0;
	}
	if (rc) {
		row->callback = NULL;
		row->sync_obj = 0;
		row->sync_cb = NULL;
		row->cb_registered_for_sync = false;
		goto out;
	}
	if (test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE, &cam_sync_monitor_mask))
		cam_generic_fence_update_monitor_array(idx, &dev->dev_lock,
			dev->monitor_data, CAM_FENCE_OP_REGISTER_CB);
	spin_unlock_bh(&dev->row_spinlocks[idx]);
	return 0;
out:
	spin_unlock_bh(&dev->row_spinlocks[idx]);
	kfree(callback);
	return rc;
}

static int __cam_dma_fence_signal_fence(
	struct dma_fence *dma_fence,
	int32_t status)
{
	int rc;
	bool fence_signaled = false;

	spin_lock_bh(dma_fence->lock);
	fence_signaled = dma_fence_is_signaled_locked(dma_fence);
	if (fence_signaled) {
		rc = -EPERM;
		CAM_DBG(CAM_DMA_FENCE,
			"dma fence seqno: %llu is already signaled",
			dma_fence->seqno);
		goto end;
	}

	if (status)
		dma_fence_set_error(dma_fence, status);

	rc = dma_fence_signal_locked(dma_fence);

end:
	spin_unlock_bh(dma_fence->lock);
	return rc;
}

int cam_dma_fence_internal_signal(int32_t row_idx,
	struct cam_dma_fence_signal *signal_dma_fence, struct dma_fence *expected)
{
	struct cam_dma_fence_device *dev = g_cam_dma_fence_dev;
	struct cam_dma_fence_row *row;
	struct dma_fence *fence;
	int rc;

	if (row_idx <= 0 || row_idx >= CAM_DMA_FENCE_MAX_FENCES || !signal_dma_fence)
		return -EINVAL;
	spin_lock_bh(&dev->row_spinlocks[row_idx]);
	row = &dev->rows[row_idx];
	if (row->state == CAM_DMA_FENCE_STATE_INVALID || row->releasing ||
		row->fd != signal_dma_fence->dma_fence_fd ||
		(expected && row->fence != expected)) {
		spin_unlock_bh(&dev->row_spinlocks[row_idx]);
		return -EINVAL;
	}
	row->sync_signal_dma = true;
	if (row->state == CAM_DMA_FENCE_STATE_SIGNALED) {
		spin_unlock_bh(&dev->row_spinlocks[row_idx]);
		return 0;
	}
	row->state = CAM_DMA_FENCE_STATE_SIGNALED;
	fence = dma_fence_get(row->fence);
	if (test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE, &cam_sync_monitor_mask))
		cam_generic_fence_update_monitor_array(row_idx, &dev->dev_lock,
			dev->monitor_data, CAM_FENCE_OP_SIGNAL);
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);

	/* The IRQ callback only queues work, including when a sync row lock is held. */
	rc = __cam_dma_fence_signal_fence(fence, signal_dma_fence->status);
	if (rc)
		CAM_DBG(CAM_DMA_FENCE, "Fence fd %d already signaled: %d",
			signal_dma_fence->dma_fence_fd, rc);
	dma_fence_put(fence);
	return 0;
}

int cam_dma_fence_signal_fd(struct cam_dma_fence_signal *signal_dma_fence)
{
	int32_t idx;
	struct dma_fence *dma_fence = NULL;

	dma_fence = __cam_dma_fence_find_fence_in_table(
		signal_dma_fence->dma_fence_fd, &idx);

	if (IS_ERR_OR_NULL(dma_fence)) {
		CAM_ERR(CAM_DMA_FENCE, "Failed to find dma fence for fd: %d",
			signal_dma_fence->dma_fence_fd);
		return -EINVAL;
	}

	return cam_dma_fence_internal_signal(idx, signal_dma_fence, NULL);
}

static int __cam_dma_fence_get_fd(int32_t *row_idx,
	const char *name)
{
	int fd = -1;
	uint32_t idx;
	struct dma_fence *dma_fence = NULL;
	struct cam_dma_fence *owned_fence;
	struct sync_file *sync_file = NULL;

	if (__cam_dma_fence_find_free_idx(&idx))
		goto end;

	owned_fence = kzalloc(sizeof(*owned_fence), GFP_KERNEL);
	if (!owned_fence)
		goto free_idx;
	if (!try_module_get(THIS_MODULE)) {
		kfree(owned_fence);
		goto free_idx;
	}
	dma_fence = &owned_fence->base;
	spin_lock_init(&owned_fence->lock);
	dma_fence_init(dma_fence, &cam_sync_dma_fence_ops, &owned_fence->lock,
		g_cam_dma_fence_dev->dma_fence_context,
		atomic64_inc_return(&g_cam_dma_fence_seq_no));
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		CAM_ERR(CAM_DMA_FENCE, "failed to get a unused fd: %d", fd);
		dma_fence_put(dma_fence);
		goto free_idx;
	}

	sync_file = sync_file_create(dma_fence);
	if (!sync_file) {
		put_unused_fd(fd);
		fd = -1;
		dma_fence_put(dma_fence);
		goto free_idx;
	}

	*row_idx = idx;
	__cam_dma_fence_init_row(name, dma_fence, fd, idx, false);
	fd_install(fd, sync_file->file);

	CAM_DBG(CAM_DMA_FENCE, "Created dma fence fd: %d[%s] seqno: %llu row_idx: %u ref_cnt: %u",
		fd, name, dma_fence->seqno, idx, kref_read(&dma_fence->refcount));

	return fd;

free_idx:
	clear_bit(idx, g_cam_dma_fence_dev->bitmap);
end:
	return fd;
}

int cam_dma_fence_create_fd(
	int32_t *dma_fence_fd, int32_t *dma_fence_row_idx, const char *name)
{
	int fd = -1, rc = 0;

	if (!dma_fence_fd || !dma_fence_row_idx) {
		CAM_ERR(CAM_DMA_FENCE, "Invalid args fd: %pK dma_fence_row_idx: %pK",
			dma_fence_fd, dma_fence_row_idx);
		return -EINVAL;
	}

	fd = __cam_dma_fence_get_fd(dma_fence_row_idx, name);
	if (fd < 0) {
		rc = -EBADFD;
		goto end;
	}

	*dma_fence_fd = fd;

end:
	return rc;
}

static void __cam_dma_fence_save_previous_monitor_data(int dma_row_idx)
{
	struct cam_generic_fence_monitor_data *row_mon_data;
	struct cam_dma_fence_row *row;

	if (!g_cam_dma_fence_dev->monitor_data ||
		!g_cam_dma_fence_dev->monitor_data[
			dma_row_idx / CAM_GENERIC_MONITOR_TABLE_ENTRY_SZ])
		return;

	row = &g_cam_dma_fence_dev->rows[dma_row_idx];
	row_mon_data = CAM_GENERIC_MONITOR_GET_DATA(
	g_cam_dma_fence_dev->monitor_data, dma_row_idx);

	/* save current usage details into prev variables */
	strscpy(row_mon_data->prev_name, row->name, CAM_DMA_FENCE_NAME_LEN);
	row_mon_data->prev_obj_id          = row->fd;
	row_mon_data->prev_sync_id         = row->sync_obj;
	row_mon_data->prev_state           = row->state;
	row_mon_data->swap_monitor_entries = !row_mon_data->swap_monitor_entries;
	row_mon_data->prev_monitor_head    = atomic64_read(&row_mon_data->monitor_head);
}

static int __cam_dma_fence_release_status(int32_t idx, int status)
{
	struct cam_dma_fence_device *dev = g_cam_dma_fence_dev;
	struct cam_dma_fence_row *row = &dev->rows[idx];
	struct cam_dma_fence_callback *callback;
	struct dma_fence *fence;
	bool external;

	spin_lock_bh(&dev->row_spinlocks[idx]);
	if (row->state == CAM_DMA_FENCE_STATE_INVALID || row->releasing) {
		spin_unlock_bh(&dev->row_spinlocks[idx]);
		return -EINVAL;
	}
	row->releasing = true;
	row->sync_signal_dma = true;
	row->cb_registered_for_sync = false;
	callback = row->callback;
	row->callback = NULL;
	fence = row->fence;
	external = row->ext_dma_fence;
	spin_unlock_bh(&dev->row_spinlocks[idx]);

	/* Removal drains the IRQ callback; then drain any work it queued. */
	if (callback) {
		dma_fence_remove_callback(fence, &callback->cb);
		cancel_work_sync(&callback->work);
		kfree(callback);
	}
	if (!external)
		__cam_dma_fence_signal_fence(fence, status);

	spin_lock_bh(&dev->row_spinlocks[idx]);
	if (test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE, &cam_sync_monitor_mask)) {
		cam_generic_fence_update_monitor_array(idx, &dev->dev_lock,
			dev->monitor_data, CAM_FENCE_OP_DESTROY);
		if (test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE_DUMP, &cam_sync_monitor_mask))
			__cam_dma_fence_dump_monitor_array(idx);
		__cam_dma_fence_save_previous_monitor_data(idx);
	}
	memset(row, 0, sizeof(*row));
	clear_bit(idx, dev->bitmap);
	spin_unlock_bh(&dev->row_spinlocks[idx]);
	dma_fence_put(fence);
	return 0;
}

static int __cam_dma_fence_release(int32_t idx)
{
	return __cam_dma_fence_release_status(idx, -ECANCELED);
}

static int __cam_dma_fence_release_fd(int fd)
{
	int32_t idx;
	struct dma_fence *dma_fence = NULL;

	dma_fence = __cam_dma_fence_find_fence_in_table(fd, &idx);
	if (IS_ERR_OR_NULL(dma_fence)) {
		CAM_ERR(CAM_DMA_FENCE, "Failed to find dma fence for fd: %d", fd);
		return -EINVAL;
	}

	return __cam_dma_fence_release(idx);
}

static int __cam_dma_fence_release_row(
	int32_t dma_fence_row_idx)
{
	if ((dma_fence_row_idx < 0) ||
		(dma_fence_row_idx >= CAM_DMA_FENCE_MAX_FENCES)) {
		CAM_ERR(CAM_DMA_FENCE, "dma fence idx: %d is invalid",
			dma_fence_row_idx);
		return -EINVAL;
	}

	return __cam_dma_fence_release(dma_fence_row_idx);
}

int cam_dma_fence_release(
	struct cam_dma_fence_release_params *release_params)
{
	if (release_params->use_row_idx)
		return __cam_dma_fence_release_row(release_params->u.dma_row_idx);
	else
		return __cam_dma_fence_release_fd(release_params->u.dma_fence_fd);
}

void cam_dma_fence_quiesce(void)
{
	struct cam_dma_fence_device *dev = g_cam_dma_fence_dev;
	struct cam_dma_fence_callback *callback;
	struct cam_dma_fence_row *row;
	struct dma_fence *fence;
	int i;

	for (i = 1; i < CAM_DMA_FENCE_MAX_FENCES; i++) {
		spin_lock_bh(&dev->row_spinlocks[i]);
		row = &dev->rows[i];
		callback = row->callback;
		row->callback = NULL;
		row->cb_registered_for_sync = false;
		fence = callback ? dma_fence_get(row->fence) : NULL;
		spin_unlock_bh(&dev->row_spinlocks[i]);
		if (!callback)
			continue;
		dma_fence_remove_callback(fence, &callback->cb);
		cancel_work_sync(&callback->work);
		kfree(callback);
		dma_fence_put(fence);
	}
}

void cam_dma_fence_close(void)
{
	struct cam_dma_fence_device *dev = g_cam_dma_fence_dev;
	int i;

	/* Last file close excludes new table operations. Keep storage until drain. */
	for (i = 1; i < CAM_DMA_FENCE_MAX_FENCES; i++)
		__cam_dma_fence_release_status(i, -EADV);
	flush_workqueue(dev->work_queue);
	mutex_lock(&dev->dev_lock);
	if (dev->monitor_data)
		for (i = 0; i < CAM_DMA_FENCE_TABLE_SZ; i++)
			CAM_MEM_FREE(dev->monitor_data[i]);
	CAM_MEM_FREE(dev->monitor_data);
	dev->monitor_data = NULL;
	mutex_unlock(&dev->dev_lock);
}

void cam_dma_fence_open(void)
{
	mutex_lock(&g_cam_dma_fence_dev->dev_lock);

	if (test_bit(CAM_GENERIC_FENCE_TYPE_DMA_FENCE, &cam_sync_monitor_mask)) {
		g_cam_dma_fence_dev->monitor_data = CAM_MEM_ZALLOC(
			sizeof(struct cam_generic_fence_monitor_data *) *
			CAM_DMA_FENCE_TABLE_SZ, GFP_KERNEL);
		if (!g_cam_dma_fence_dev->monitor_data) {
			CAM_WARN(CAM_DMA_FENCE, "Failed to allocate memory %d",
				sizeof(struct cam_generic_fence_monitor_data *) *
				CAM_DMA_FENCE_TABLE_SZ);
		}
	}

	/* Context/sequence pairs remain unique across camera file reopen. */
	mutex_unlock(&g_cam_dma_fence_dev->dev_lock);
	CAM_DBG(CAM_DMA_FENCE, "Camera DMA fence driver opened");
}

int cam_dma_fence_driver_init(void)
{
	int i;

	g_cam_dma_fence_dev = CAM_MEM_ZALLOC(sizeof(struct cam_dma_fence_device), GFP_KERNEL);
	if (!g_cam_dma_fence_dev)
		return -ENOMEM;

	g_cam_dma_fence_dev->work_queue = alloc_workqueue("cam_dma_fence",
		WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!g_cam_dma_fence_dev->work_queue) {
		CAM_MEM_FREE(g_cam_dma_fence_dev);
		g_cam_dma_fence_dev = NULL;
		return -ENOMEM;
	}
	mutex_init(&g_cam_dma_fence_dev->dev_lock);
	for (i = 0; i < CAM_DMA_FENCE_MAX_FENCES; i++)
		spin_lock_init(&g_cam_dma_fence_dev->row_spinlocks[i]);

	bitmap_zero(g_cam_dma_fence_dev->bitmap, CAM_DMA_FENCE_MAX_FENCES);

	/* zero will be considered an invalid slot */
	set_bit(0, g_cam_dma_fence_dev->bitmap);

	g_cam_dma_fence_dev->dma_fence_context = dma_fence_context_alloc(1);

	CAM_DBG(CAM_DMA_FENCE, "Camera DMA fence driver initialized");
	return 0;
}

void cam_dma_fence_driver_deinit(void)
{
	if (!g_cam_dma_fence_dev)
		return;
	cam_dma_fence_close();
	destroy_workqueue(g_cam_dma_fence_dev->work_queue);
	mutex_destroy(&g_cam_dma_fence_dev->dev_lock);
	CAM_MEM_FREE(g_cam_dma_fence_dev);
	g_cam_dma_fence_dev = NULL;
}
