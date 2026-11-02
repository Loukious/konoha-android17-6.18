// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "cam_sync_synx.h"
#include "cam_sync_util.h"
#include "cam_mem_mgr_api.h"

extern unsigned long cam_sync_monitor_mask;

struct cam_synx_callback {
	struct dma_fence *fence;
	struct dma_fence_cb fence_cb;
	struct work_struct work;
	struct cam_synx_obj_device *dev;
	uint32_t synx_obj;
	int row_idx;
	u64 sync_generation;
};

/**
 * struct cam_synx_obj_row - Synx obj row
 */
struct cam_synx_obj_row {
	char                            name[CAM_SYNX_OBJ_NAME_LEN];
	uint32_t                        synx_obj;
	enum cam_synx_obj_state         state;
	cam_sync_callback_for_synx_obj  sync_cb;
	bool                            cb_registered_for_sync;
	bool                            sync_signal_synx;
	int32_t                         sync_obj;
	struct cam_synx_callback        *callback;
	bool                            releasing;
};

/**
 * struct cam_synx_obj_device - Synx obj device
 */
struct cam_synx_obj_device {
	struct cam_synx_obj_row rows[CAM_SYNX_MAX_OBJS];
	spinlock_t row_spinlocks[CAM_SYNX_MAX_OBJS];
	struct mutex row_locks[CAM_SYNX_MAX_OBJS];
	struct workqueue_struct *work_queue;
	struct synx_session *session_handle;
	struct mutex dev_lock;
	DECLARE_BITMAP(bitmap, CAM_SYNX_MAX_OBJS);
	struct cam_generic_fence_monitor_data **monitor_data;
};

static struct cam_synx_obj_device *g_cam_synx_obj_dev;
static char cam_synx_session_name[64] = "Camera_Generic_Synx_Session";


static inline struct cam_generic_fence_monitor_entry *
	__cam_synx_obj_get_monitor_entries(int idx)
{
	struct cam_generic_fence_monitor_data *monitor_data;

	monitor_data = CAM_GENERIC_MONITOR_GET_DATA(
		g_cam_synx_obj_dev->monitor_data, idx);
	if (monitor_data->swap_monitor_entries)
		return monitor_data->prev_monitor_entries;
	else
		return monitor_data->monitor_entries;
}

static inline struct cam_generic_fence_monitor_entry *
	__cam_synx_obj_get_prev_monitor_entries(int idx)
{
	struct cam_generic_fence_monitor_data *monitor_data;

	monitor_data = CAM_GENERIC_MONITOR_GET_DATA(
		g_cam_synx_obj_dev->monitor_data, idx);
	if (monitor_data->swap_monitor_entries)
		return monitor_data->monitor_entries;
	else
		return monitor_data->prev_monitor_entries;
}

static int __cam_synx_obj_map_sync_status_util(uint32_t sync_status,
	uint32_t *out_synx_status)
{
	if (!out_synx_status)
		return -EINVAL;

	switch (sync_status) {
	case CAM_SYNC_STATE_SIGNALED_SUCCESS:
		*out_synx_status = SYNX_STATE_SIGNALED_SUCCESS;
		break;
	case CAM_SYNC_STATE_SIGNALED_CANCEL:
	default:
		*out_synx_status = SYNX_STATE_SIGNALED_CANCEL;
		break;
	}

	return 0;
}

static void __cam_synx_obj_save_previous_monitor_data(int32_t row_idx)
{
	struct cam_generic_fence_monitor_data *row_mon_data;
	struct cam_synx_obj_row *row;

	if (!g_cam_synx_obj_dev->monitor_data ||
		!g_cam_synx_obj_dev->monitor_data[row_idx / CAM_GENERIC_MONITOR_TABLE_ENTRY_SZ])
		return;

	row = &g_cam_synx_obj_dev->rows[row_idx];
	row_mon_data = CAM_GENERIC_MONITOR_GET_DATA(
		g_cam_synx_obj_dev->monitor_data, row_idx);

	/* save current usage details into prev variables */
	strscpy(row_mon_data->prev_name, row->name, CAM_SYNX_OBJ_NAME_LEN);
	row_mon_data->prev_obj_id          = row->synx_obj;
	row_mon_data->prev_sync_id         = row->sync_obj;
	row_mon_data->prev_state           = row->state;
	row_mon_data->prev_monitor_head    = atomic64_read(&row_mon_data->monitor_head);
	row_mon_data->swap_monitor_entries = !row_mon_data->swap_monitor_entries;
}

static void __cam_synx_obj_dump_monitor_array(int32_t row_idx)
{
	struct cam_generic_fence_monitor_obj_info obj_info;
	struct cam_synx_obj_row *row;

	if (row_idx <= 0 || row_idx >= CAM_SYNX_MAX_OBJS ||
		!g_cam_synx_obj_dev->monitor_data ||
		!g_cam_synx_obj_dev->monitor_data[row_idx / CAM_GENERIC_MONITOR_TABLE_ENTRY_SZ] ||
		!test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ, &cam_sync_monitor_mask))
		return;

	if (!CAM_GENERIC_MONITOR_GET_DATA(g_cam_synx_obj_dev->monitor_data,
		row_idx)->prev_obj_id)
		return;

	row = &g_cam_synx_obj_dev->rows[row_idx];

	obj_info.name = row->name;
	obj_info.obj_id = row->synx_obj;
	obj_info.state = row->state;
	obj_info.monitor_data = CAM_GENERIC_MONITOR_GET_DATA(
		g_cam_synx_obj_dev->monitor_data, row_idx);
	obj_info.fence_type = CAM_GENERIC_FENCE_TYPE_SYNX_OBJ;
	obj_info.sync_id = row->sync_obj;
	obj_info.monitor_entries =
		__cam_synx_obj_get_monitor_entries(row_idx);
	obj_info.prev_monitor_entries =
		__cam_synx_obj_get_prev_monitor_entries(row_idx);
	cam_generic_fence_dump_monitor_array(&obj_info);
}

static void __cam_synx_obj_signal_work(struct work_struct *work)
{
	struct cam_synx_callback *callback = container_of(work, struct cam_synx_callback, work);
	struct cam_synx_obj_device *dev = callback->dev;
	struct cam_synx_obj_row *row = &dev->rows[callback->row_idx];
	struct cam_synx_obj_signal_sync_obj signal_sync_obj;
	cam_sync_callback_for_synx_obj sync_cb = NULL;
	int32_t sync_obj = 0;
	int status;

	/*
	 * The row retains the handle until this worker is drained. Read the
	 * completed backing fence: synx_get_status() returns cached core state,
	 * which can lag behind an imported fence's IRQ callback.
	 */
	status = synx_wait(dev->session_handle, callback->synx_obj, 0);
	spin_lock_bh(&dev->row_spinlocks[callback->row_idx]);
	if (row->callback == callback && !row->releasing && !row->sync_signal_synx &&
		row->cb_registered_for_sync) {
		signal_sync_obj.synx_obj = callback->synx_obj;
		signal_sync_obj.sync_generation = callback->sync_generation;
		switch (status) {
		case SYNX_STATE_SIGNALED_SUCCESS:
			signal_sync_obj.status = CAM_SYNC_STATE_SIGNALED_SUCCESS;
			break;
		case SYNX_STATE_SIGNALED_CANCEL:
			signal_sync_obj.status = CAM_SYNC_STATE_SIGNALED_CANCEL;
			break;
		default:
			signal_sync_obj.status = CAM_SYNC_STATE_SIGNALED_ERROR;
			break;
		}
		row->state = CAM_SYNX_OBJ_STATE_SIGNALED;
		sync_cb = row->sync_cb;
		sync_obj = row->sync_obj;
		if (test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ, &cam_sync_monitor_mask))
			cam_generic_fence_update_monitor_array(callback->row_idx,
				&dev->dev_lock, dev->monitor_data,
				CAM_FENCE_OP_UNREGISTER_ON_SIGNAL);
	}
	spin_unlock_bh(&dev->row_spinlocks[callback->row_idx]);
	if (sync_cb)
		sync_cb(sync_obj, &signal_sync_obj);
}

static void __cam_synx_obj_signal_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct cam_synx_callback *callback = container_of(cb, struct cam_synx_callback, fence_cb);

	/* No camera or Synx locks may be acquired under the fence IRQ lock. */
	queue_work(callback->dev->work_queue, &callback->work);
}

/*
 * Synx APIs need to be invoked in non atomic context,
 * all these utils invoke synx driver
 */
static inline int __cam_synx_signal_util(
	uint32_t synx_hdl, uint32_t signal_status)
{
	return synx_signal(g_cam_synx_obj_dev->session_handle, synx_hdl, signal_status);
}



static inline int __cam_synx_create_hdl_util(
	struct synx_create_params *params)
{
	return synx_create(g_cam_synx_obj_dev->session_handle, params);
}

static inline int __cam_synx_release_hdl_util(uint32_t synx_hdl)
{
	return synx_release(g_cam_synx_obj_dev->session_handle, synx_hdl);
}

static inline int __cam_synx_import_hdl_util(
	struct synx_import_params *params)
{
	return synx_import(g_cam_synx_obj_dev->session_handle, params);
}



static int __cam_synx_obj_release_status(int32_t row_idx, bool cancel)
{
	struct cam_synx_obj_device *dev = g_cam_synx_obj_dev;
	struct cam_synx_obj_row *row = &dev->rows[row_idx];
	struct cam_synx_callback *callback;
	uint32_t handle;
	bool signal;
	int rc;

	mutex_lock(&dev->row_locks[row_idx]);
	spin_lock_bh(&dev->row_spinlocks[row_idx]);
	if (row->state == CAM_SYNX_OBJ_STATE_INVALID) {
		spin_unlock_bh(&dev->row_spinlocks[row_idx]);
		mutex_unlock(&dev->row_locks[row_idx]);
		return -EINVAL;
	}
	row->releasing = true;
	row->sync_signal_synx = true;
	row->cb_registered_for_sync = false;
	callback = row->callback;
	row->callback = NULL;
	handle = row->synx_obj;
	signal = cancel && row->state != CAM_SYNX_OBJ_STATE_SIGNALED;
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);

	if (callback) {
		dma_fence_remove_callback(callback->fence, &callback->fence_cb);
		cancel_work_sync(&callback->work);
		dma_fence_put(callback->fence);
		kfree(callback);
	}
	if (signal)
		__cam_synx_signal_util(handle, SYNX_STATE_SIGNALED_CANCEL);
	rc = __cam_synx_release_hdl_util(handle);
	spin_lock_bh(&dev->row_spinlocks[row_idx]);
	if (test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ, &cam_sync_monitor_mask)) {
		cam_generic_fence_update_monitor_array(row_idx, &dev->dev_lock,
			dev->monitor_data, CAM_FENCE_OP_DESTROY);
		if (test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ_DUMP, &cam_sync_monitor_mask))
			__cam_synx_obj_dump_monitor_array(row_idx);
		__cam_synx_obj_save_previous_monitor_data(row_idx);
	}
	memset(row, 0, sizeof(*row));
	clear_bit(row_idx, dev->bitmap);
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);
	mutex_unlock(&dev->row_locks[row_idx]);
	return rc;
}

static int __cam_synx_obj_release(int32_t row_idx)
{
	return __cam_synx_obj_release_status(row_idx, false);
}

static int __cam_synx_obj_find_free_idx(uint32_t *idx)
{
	int rc = 0;
	bool bit;

	do {
		*idx = find_first_zero_bit(g_cam_synx_obj_dev->bitmap, CAM_SYNX_MAX_OBJS);
		if (*idx >= CAM_SYNX_MAX_OBJS) {
			CAM_ERR(CAM_SYNC,
				"Error: Unable to create synx, no free index");
			rc = -ENOMEM;
			break;
		}

		bit = test_and_set_bit(*idx, g_cam_synx_obj_dev->bitmap);
	} while (bit);

	return rc;
}

static void __cam_synx_obj_init_row(uint32_t idx, const char *name,
	uint32_t synx_obj)
{
	struct cam_synx_obj_row *row;

	spin_lock_bh(&g_cam_synx_obj_dev->row_spinlocks[idx]);
	row = &g_cam_synx_obj_dev->rows[idx];
	row->synx_obj = synx_obj;
	row->state = CAM_SYNX_OBJ_STATE_ACTIVE;
	strscpy(row->name, name, CAM_SYNX_OBJ_NAME_LEN);
	if (test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ, &cam_sync_monitor_mask)) {
		cam_generic_fence_update_monitor_array(idx,
			&g_cam_synx_obj_dev->dev_lock, g_cam_synx_obj_dev->monitor_data,
			CAM_FENCE_OP_CREATE);
	}
	spin_unlock_bh(&g_cam_synx_obj_dev->row_spinlocks[idx]);
}

static int __cam_synx_obj_release_row(int32_t row_idx)
{
	if ((row_idx < 0) || (row_idx >= CAM_SYNX_MAX_OBJS)) {
		CAM_ERR(CAM_SYNX, "synx row idx: %d is invalid", row_idx);
		return -EINVAL;
	}

	return __cam_synx_obj_release(row_idx);
}

int cam_synx_obj_find_obj_in_table(uint32_t synx_obj, int32_t *idx)
{
	int i, rc = -EINVAL;
	struct cam_synx_obj_row *row = NULL;

	for (i = 0; i < CAM_SYNX_MAX_OBJS; i++) {
		spin_lock_bh(&g_cam_synx_obj_dev->row_spinlocks[i]);
		row = &g_cam_synx_obj_dev->rows[i];
		if (row->state != CAM_SYNX_OBJ_STATE_INVALID && !row->releasing &&
			row->synx_obj == synx_obj) {
			*idx = i;
			spin_unlock_bh(&g_cam_synx_obj_dev->row_spinlocks[i]);
			rc = 0;
			break;
		}
		spin_unlock_bh(&g_cam_synx_obj_dev->row_spinlocks[i]);
	}

	return rc;
}

static int __cam_synx_obj_release_obj(uint32_t synx_obj, int32_t *idx)
{
	if (cam_synx_obj_find_obj_in_table(synx_obj, idx)) {
		CAM_ERR(CAM_SYNX, "Failed to find synx obj: %d", synx_obj);
		return -EINVAL;
	}

	return __cam_synx_obj_release(*idx);
}

static int __cam_synx_obj_import(const char *name,
	struct synx_import_params *params, int32_t *row_idx)
{
	int rc = -1;
	uint32_t idx;

	if (__cam_synx_obj_find_free_idx(&idx))
		goto end;

	rc = __cam_synx_import_hdl_util(params);
	if (rc) {
		CAM_ERR(CAM_SYNX, "Synx import failed for fence : %p",
			params->indv.fence);
		goto free_idx;
	}

	*row_idx = idx;
	__cam_synx_obj_init_row(idx, name, *params->indv.new_h_synx);

	CAM_DBG(CAM_SYNX, "Imported synx obj handle: %d[%s] row_idx: %u",
		*params->indv.new_h_synx, name, idx);

	return rc;

free_idx:
	clear_bit(idx, g_cam_synx_obj_dev->bitmap);
end:
	return rc;
}

static int __cam_synx_map_generic_flags_to_create(uint32_t generic_flags,
	struct synx_create_params *params)
{
	if (!params) {
		CAM_ERR(CAM_SYNX, "Create parameters missing");
		return -EINVAL;
	}

	/*
	 * Create Global Always - remove after userspace optimizes and
	 * determines when global Vs local is needed
	 */
	params->flags |= SYNX_CREATE_GLOBAL_FENCE;

	return 0;
}

static int __cam_synx_map_generic_flags_to_import(uint32_t generic_flags,
	struct synx_import_indv_params *params)
{
	if (!params) {
		CAM_ERR(CAM_SYNX, "Import parameters missing");
		return -EINVAL;
	}

	/*
	 * Create Global Always - remove after userspace optimizes and
	 * determines when global Vs local is needed
	 */
	params->flags |= SYNX_IMPORT_GLOBAL_FENCE;

	return 0;
}

int cam_synx_obj_create(const char *name, uint32_t flags, uint32_t *synx_obj,
	int32_t *row_idx)
{
	int rc = -1;
	uint32_t idx;
	struct synx_create_params params;

	if (__cam_synx_obj_find_free_idx(&idx))
		goto end;

	params.fence = NULL;
	params.name = name;
	params.flags = 0;
	params.h_synx = synx_obj;

	rc = __cam_synx_map_generic_flags_to_create(flags, &params);
	if (rc) {
		CAM_ERR(CAM_SYNX, "Failed to generate create flags");
		goto free_idx;
	}

	rc = __cam_synx_create_hdl_util(&params);
	if (rc) {
		CAM_ERR(CAM_SYNX, "Failed to create new synx handle rc: %d", rc);
		goto free_idx;
	}

	*row_idx = idx;
	__cam_synx_obj_init_row(idx, name, *synx_obj);

	CAM_DBG(CAM_SYNX, "Created synx obj handle: %d[%s] row_idx: %u",
		*synx_obj, name, idx);

	return rc;

free_idx:
	clear_bit(idx, g_cam_synx_obj_dev->bitmap);
end:
	return rc;
}

int cam_synx_obj_import_dma_fence(const char *name, uint32_t flags, void *fence,
	uint32_t *synx_obj, int32_t *row_idx)
{
	struct synx_import_params params;

	if (!fence) {
		CAM_ERR(CAM_SYNX,
			"Importing DMA fence failed - fence pointer is NULL");
		return -EINVAL;
	}

	params.indv.flags = 0;
	params.indv.fence = fence;
	params.indv.new_h_synx = synx_obj;
	params.type = SYNX_IMPORT_INDV_PARAMS;
	params.indv.flags |= SYNX_IMPORT_DMA_FENCE;

	if (__cam_synx_map_generic_flags_to_import(flags, &params.indv)) {
		CAM_ERR(CAM_SYNX,
			"Importing DMA fence failed - invalid synx import flags");
		return -EINVAL;
	}

	return __cam_synx_obj_import(name, &params, row_idx);
}

int cam_synx_obj_internal_signal(int32_t row_idx,
	struct cam_synx_obj_signal *signal_synx_obj)
{
	struct cam_synx_obj_device *dev = g_cam_synx_obj_dev;
	struct cam_synx_obj_row *row;
	uint32_t signal_status;
	int rc;

	if (row_idx <= 0 || row_idx >= CAM_SYNX_MAX_OBJS || !signal_synx_obj)
		return -EINVAL;
	mutex_lock(&dev->row_locks[row_idx]);
	spin_lock_bh(&dev->row_spinlocks[row_idx]);
	row = &dev->rows[row_idx];
	if (row->synx_obj != signal_synx_obj->synx_obj) {
		rc = 0;
		goto unlock;
	}
	if (row->state != CAM_SYNX_OBJ_STATE_ACTIVE) {
		rc = -EINVAL;
		goto unlock;
	}
	rc = __cam_synx_obj_map_sync_status_util(signal_synx_obj->status, &signal_status);
	if (rc)
		goto unlock;
	row->sync_signal_synx = true;
	row->state = CAM_SYNX_OBJ_STATE_SIGNALED;
	if (test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ, &cam_sync_monitor_mask))
		cam_generic_fence_update_monitor_array(row_idx, &dev->dev_lock,
			dev->monitor_data, CAM_FENCE_OP_SIGNAL);
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);
	rc = __cam_synx_signal_util(signal_synx_obj->synx_obj, signal_status);
	mutex_unlock(&dev->row_locks[row_idx]);
	return rc;
unlock:
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);
	mutex_unlock(&dev->row_locks[row_idx]);
	return rc;
}

int cam_synx_obj_release(struct cam_synx_obj_release_params *release_params)
{
	int rc;
	int32_t idx = -1;

	if (release_params->use_row_idx) {
		rc = __cam_synx_obj_release_row(release_params->u.synx_row_idx);
		if (rc < 0)
			__cam_synx_obj_dump_monitor_array(release_params->u.synx_row_idx);
	} else {
		rc = __cam_synx_obj_release_obj(release_params->u.synx_obj, &idx);
		if ((rc < 0) && (idx >= 0))
			__cam_synx_obj_dump_monitor_array(idx);
	}
	return rc;
}

int cam_synx_obj_signal_obj(struct cam_synx_obj_signal *signal_synx_obj)
{
	int rc;
	int32_t idx;

	rc = cam_synx_obj_find_obj_in_table(signal_synx_obj->synx_obj, &idx);
	if (rc) {
		CAM_ERR(CAM_SYNX, "Failed to find synx obj: %u", signal_synx_obj->synx_obj);
		return -EINVAL;
	}

	return cam_synx_obj_internal_signal(idx, signal_synx_obj);
}

int cam_synx_obj_register_cb(int32_t *sync_obj, int32_t row_idx,
	cam_sync_callback_for_synx_obj sync_cb)
{
	struct cam_synx_obj_device *dev = g_cam_synx_obj_dev;
	struct cam_synx_obj_row *row;
	struct cam_synx_callback *callback;
	u64 generation;
	uint32_t handle;
	int rc = 0;

	if (!sync_obj || !sync_cb || row_idx <= 0 || row_idx >= CAM_SYNX_MAX_OBJS)
		return -EINVAL;
	generation = cam_sync_get_generation(*sync_obj);
	if (!generation)
		return -EINVAL;
	callback = kzalloc(sizeof(*callback), GFP_KERNEL);
	if (!callback)
		return -ENOMEM;
	mutex_lock(&dev->row_locks[row_idx]);
	spin_lock_bh(&dev->row_spinlocks[row_idx]);
	row = &dev->rows[row_idx];
	if (row->state != CAM_SYNX_OBJ_STATE_ACTIVE) {
		rc = -EINVAL;
		goto unlock_row;
	}
	if (row->cb_registered_for_sync)
		goto unlock_row;
	handle = row->synx_obj;
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);
	callback->dev = dev;
	callback->row_idx = row_idx;
	callback->synx_obj = handle;
	callback->sync_generation = generation;
	INIT_WORK(&callback->work, __cam_synx_obj_signal_work);
	callback->fence = synx_get_fence(dev->session_handle, handle);
	if (IS_ERR_OR_NULL(callback->fence)) {
		rc = IS_ERR(callback->fence) ? PTR_ERR(callback->fence) : -EINVAL;
		goto unlock;
	}
	/*
	 * Native Synx records the APSS global waiter before checking the timeout.
	 * Subscribe once with a zero-duration wait, then wait on its retained fence.
	 * A live fence returns -ETIMEDOUT here; it is not a polling loop. Keeping
	 * the wait is essential: synx_get_fence() alone does not request remote
	 * notifications. The backing-fence callback can be synchronously removed,
	 * unlike synx_cancel_async_wait() on imported external fences.
	 */
	rc = synx_wait(dev->session_handle, handle, 0);
	if (rc < 0 && rc != -ETIMEDOUT)
		goto put_fence;

	spin_lock_bh(&dev->row_spinlocks[row_idx]);
	row->callback = callback;
	row->sync_cb = sync_cb;
	row->sync_obj = *sync_obj;
	row->cb_registered_for_sync = true;
	rc = dma_fence_add_callback(callback->fence, &callback->fence_cb,
		__cam_synx_obj_signal_cb);
	if (rc == -ENOENT) {
		queue_work(dev->work_queue, &callback->work);
		rc = 0;
	}
	if (rc) {
		row->callback = NULL;
		row->sync_cb = NULL;
		row->sync_obj = 0;
		row->cb_registered_for_sync = false;
		spin_unlock_bh(&dev->row_spinlocks[row_idx]);
		goto put_fence;
	}
	if (test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ, &cam_sync_monitor_mask))
		cam_generic_fence_update_monitor_array(row_idx, &dev->dev_lock,
			dev->monitor_data, CAM_FENCE_OP_REGISTER_CB);
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);
	mutex_unlock(&dev->row_locks[row_idx]);
	return 0;
unlock_row:
	spin_unlock_bh(&dev->row_spinlocks[row_idx]);
	goto unlock;
put_fence:
	dma_fence_put(callback->fence);
unlock:
	mutex_unlock(&dev->row_locks[row_idx]);
	kfree(callback);
	return rc;
}

int cam_synx_core_recovery(
	enum cam_sync_synx_supported_cores cam_core_id)
{
	int rc;
	enum synx_client_id client_id = SYNX_CLIENT_MAX;

	switch (cam_core_id) {
	case CAM_ICP_0_SYNX_CORE:
		client_id = SYNX_CLIENT_ICP_CTX0;
		break;
	default:
		rc = -EINVAL;
		goto err;
	}

	rc = synx_recover(client_id);
	if (rc)
		goto err;

	CAM_DBG(CAM_SYNX, "Synx recovery for synx_client: %d[%d] success",
		client_id, cam_core_id);

	return rc;

err:
	CAM_ERR(CAM_SYNX, "Failed to recover for synx_client: %d rc: %d",
			client_id, rc);
	return rc;
}

static int __cam_synx_init_session(void)
{
	struct synx_queue_desc queue_desc = { 0 };
	struct synx_initialization_params params = { 0 };
	struct synx_session *session;

	params.name = cam_synx_session_name;
	params.ptr = &queue_desc;
	params.flags = SYNX_INIT_MAX;
	params.id = SYNX_CLIENT_NATIVE;
	session = synx_initialize(&params);
	if (IS_ERR_OR_NULL(session))
		return IS_ERR(session) ? PTR_ERR(session) : -ENODEV;
	g_cam_synx_obj_dev->session_handle = session;
	return 0;
}

void cam_synx_obj_open(void)
{
	mutex_lock(&g_cam_synx_obj_dev->dev_lock);
	if (test_bit(CAM_GENERIC_FENCE_TYPE_SYNX_OBJ, &cam_sync_monitor_mask)) {
		g_cam_synx_obj_dev->monitor_data = CAM_MEM_ZALLOC(
			sizeof(struct cam_generic_fence_monitor_data *) *
			CAM_SYNX_TABLE_SZ, GFP_KERNEL);
		if (!g_cam_synx_obj_dev->monitor_data) {
			CAM_WARN(CAM_DMA_FENCE, "Failed to allocate memory %d",
				sizeof(struct cam_generic_fence_monitor_data *) *
				CAM_SYNX_TABLE_SZ);
		}
	}
	mutex_unlock(&g_cam_synx_obj_dev->dev_lock);
}

void cam_synx_obj_quiesce(void)
{
	struct cam_synx_obj_device *dev = g_cam_synx_obj_dev;
	struct cam_synx_callback *callback;
	struct cam_synx_obj_row *row;
	int i;

	for (i = 1; i < CAM_SYNX_MAX_OBJS; i++) {
		mutex_lock(&dev->row_locks[i]);
		spin_lock_bh(&dev->row_spinlocks[i]);
		row = &dev->rows[i];
		callback = row->callback;
		row->callback = NULL;
		row->cb_registered_for_sync = false;
		spin_unlock_bh(&dev->row_spinlocks[i]);
		if (callback) {
			dma_fence_remove_callback(callback->fence, &callback->fence_cb);
			cancel_work_sync(&callback->work);
			dma_fence_put(callback->fence);
			kfree(callback);
		}
		mutex_unlock(&dev->row_locks[i]);
	}
}

void cam_synx_obj_close(void)
{
	struct cam_synx_obj_device *dev = g_cam_synx_obj_dev;
	int i;

	for (i = 1; i < CAM_SYNX_MAX_OBJS; i++)
		__cam_synx_obj_release_status(i, true);
	flush_workqueue(dev->work_queue);
	mutex_lock(&dev->dev_lock);
	if (dev->monitor_data)
		for (i = 0; i < CAM_SYNX_TABLE_SZ; i++)
			CAM_MEM_FREE(dev->monitor_data[i]);
	CAM_MEM_FREE(dev->monitor_data);
	dev->monitor_data = NULL;
	mutex_unlock(&dev->dev_lock);
}

int cam_synx_obj_driver_init(void)
{
	int i, rc;

	g_cam_synx_obj_dev = CAM_MEM_ZALLOC(sizeof(*g_cam_synx_obj_dev), GFP_KERNEL);
	if (!g_cam_synx_obj_dev)
		return -ENOMEM;
	mutex_init(&g_cam_synx_obj_dev->dev_lock);
	for (i = 0; i < CAM_SYNX_MAX_OBJS; i++) {
		spin_lock_init(&g_cam_synx_obj_dev->row_spinlocks[i]);
		mutex_init(&g_cam_synx_obj_dev->row_locks[i]);
	}
	bitmap_zero(g_cam_synx_obj_dev->bitmap, CAM_SYNX_MAX_OBJS);
	set_bit(0, g_cam_synx_obj_dev->bitmap);
	g_cam_synx_obj_dev->work_queue = alloc_workqueue("cam_synx_fence",
		WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!g_cam_synx_obj_dev->work_queue) {
		rc = -ENOMEM;
		goto free_locks;
	}
	rc = __cam_synx_init_session();
	if (!rc)
		return 0;
	destroy_workqueue(g_cam_synx_obj_dev->work_queue);
free_locks:
	for (i = 0; i < CAM_SYNX_MAX_OBJS; i++)
		mutex_destroy(&g_cam_synx_obj_dev->row_locks[i]);
	mutex_destroy(&g_cam_synx_obj_dev->dev_lock);
	CAM_MEM_FREE(g_cam_synx_obj_dev);
	g_cam_synx_obj_dev = NULL;
	return rc;
}

void cam_synx_obj_driver_deinit(void)
{
	struct cam_synx_obj_device *dev = g_cam_synx_obj_dev;
	int i, rc;

	if (!dev)
		return;
	/* Withdraw backing-fence callbacks before invalidating their Synx session. */
	cam_synx_obj_close();
	rc = synx_uninitialize(dev->session_handle);
	if (rc)
		CAM_ERR(CAM_SYNX, "Synx session uninitialize failed: %d", rc);
	destroy_workqueue(dev->work_queue);
	for (i = 0; i < CAM_SYNX_MAX_OBJS; i++)
		mutex_destroy(&dev->row_locks[i]);
	mutex_destroy(&dev->dev_lock);
	CAM_MEM_FREE(dev);
	g_cam_synx_obj_dev = NULL;
}
