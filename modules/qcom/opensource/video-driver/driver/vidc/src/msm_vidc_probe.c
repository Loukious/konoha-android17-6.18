// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2022, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/component.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/qcom-iommu-util.h>
#include <linux/version.h>
#include <linux/stringify.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
#include <linux/dma-iommu.h>
#endif
#ifdef CONFIG_MSM_MMRM
#include <linux/soc/qcom/msm_mmrm.h>
#endif

#include "msm_vidc_internal.h"
#include "msm_vidc_driver.h"
#include "msm_vidc_debug.h"
#include "msm_vidc_state.h"
#include "msm_vidc_platform.h"
#include "msm_vidc_core.h"
#include "msm_vidc_memory.h"
#include "msm_vidc_fence.h"
#include "venus_hfi.h"

#define BASE_DEVICE_NUMBER 32

struct msm_vidc_core *g_core;
static DEFINE_MUTEX(msm_vidc_registration_lock);

static inline bool is_video_device(struct device *dev)
{
	return !!(of_device_is_compatible(dev->of_node, "qcom,sm8450-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,sm8550-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,sm8550-vidc-v2") ||
		of_device_is_compatible(dev->of_node, "qcom,sm8650-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,sm8650-vidc-v2") ||
		of_device_is_compatible(dev->of_node, "qcom,cliffs-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,volcano-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,sm8750-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,sm8750-vidc-v2") ||
		of_device_is_compatible(dev->of_node, "qcom,sm8750-vidc-v3") ||
		of_device_is_compatible(dev->of_node, "qcom,tuna-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,kera-vidc") ||
		of_device_is_compatible(dev->of_node, "qcom,kera-vidc-v2"));
}

static inline bool is_video_context_bank_device_node(struct device_node *of_node)
{
	return !!(of_device_is_compatible(of_node, "qcom,vidc,cb-sec-pxl") ||
		of_device_is_compatible(of_node, "qcom,vidc,cb-sec-bitstream") ||
		of_device_is_compatible(of_node, "qcom,vidc,cb-sec-non-pxl") ||
		of_device_is_compatible(of_node, "qcom,vidc,cb-ns") ||
		of_device_is_compatible(of_node, "qcom,vidc,cb-ns-pxl"));
}

static inline bool is_video_context_bank_device(struct device *dev)
{
	return is_video_context_bank_device_node(dev->of_node);
}

static int msm_vidc_init_resources(struct msm_vidc_core *core)
{
	int rc = 0;

	rc = call_res_op(core, init, core);
	if (rc) {
		d_vpr_e("%s: Failed to init resources: %d\n", __func__, rc);
		return rc;
	}

	return 0;
}

static ssize_t sku_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct msm_vidc_core *core;

	/*
	 * Default sku version: 0
	 * driver possibly not probed yet or not the main device.
	 */
	if (!dev || !dev->driver)
		return 0;

	core = dev_get_drvdata(dev);
	if (!core || !core->platform) {
		d_vpr_e("%s: invalid core\n", __func__);
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d",
			core->platform->data.sku_version);
}

static DEVICE_ATTR_RO(sku_version);

static struct attribute *msm_vidc_core_attrs[] = {
	&dev_attr_sku_version.attr,
	NULL
};

static struct attribute_group msm_vidc_core_attr_group = {
	.attrs = msm_vidc_core_attrs,
};

static const struct of_device_id msm_vidc_dt_match[] = {
	{.compatible = "qcom,sm8450-vidc"},
	{.compatible = "qcom,sm8550-vidc"},
	{.compatible = "qcom,sm8550-vidc-v2"},
	{.compatible = "qcom,sm8650-vidc"},
	{.compatible = "qcom,sm8650-vidc-v2"},
	{.compatible = "qcom,sm8750-vidc"},
	{.compatible = "qcom,sm8750-vidc-v2"},
	{.compatible = "qcom,sm8750-vidc-v3"},
	{.compatible = "qcom,cliffs-vidc"},
	{.compatible = "qcom,volcano-vidc"},
	{.compatible = "qcom,tuna-vidc"},
	{.compatible = "qcom,kera-vidc"},
	{.compatible = "qcom,kera-vidc-v2"},
	{.compatible = "qcom,vidc,cb-ns-pxl"},
	{.compatible = "qcom,vidc,cb-ns"},
	{.compatible = "qcom,vidc,cb-sec-non-pxl"},
	{.compatible = "qcom,vidc,cb-sec-bitstream"},
	{.compatible = "qcom,vidc,cb-sec-pxl"},
	MSM_VIDC_EMPTY_BRACE
};
MODULE_DEVICE_TABLE(of, msm_vidc_dt_match);

static void msm_vidc_release_video_device(struct video_device *vdev)
{
	struct msm_video_device *video = container_of(vdev,
		struct msm_video_device, vdev);

	complete_all(&video->released);
}

static void msm_vidc_unpublish_video_device(struct msm_video_device *video)
{
	if (video->registered && !video->unregistering) {
		video->unregistering = true;
		video_unregister_device(&video->vdev);
	}
}

static void msm_vidc_wait_video_device(struct msm_video_device *video)
{
	if (video->registered)
		wait_for_completion(&video->released);
}

static void msm_vidc_unregister_video_device(struct msm_vidc_core *core,
		enum msm_vidc_domain_type type)
{
	struct msm_video_device *video;
	int index;

	if (type == MSM_VIDC_DECODER)
		index = 0;
	else if (type == MSM_VIDC_ENCODER)
		index = 1;
	else
		return;
	video = &core->vdev[index];
	if (!video->registered)
		return;

	msm_vidc_unpublish_video_device(video);
	msm_vidc_wait_video_device(video);
#ifdef CONFIG_MEDIA_CONTROLLER
	if (video->m2m_registered) {
		v4l2_m2m_unregister_media_controller(video->m2m_dev);
		v4l2_m2m_release(video->m2m_dev);
		video->m2m_registered = false;
		video->m2m_dev = NULL;
	}
#endif
	video_set_drvdata(&video->vdev, NULL);
	video->registered = false;
}

static int msm_vidc_register_video_device(struct msm_vidc_core *core,
		enum msm_vidc_domain_type type, int nr)
{
	int rc = 0;
	int index, media_index;

	d_vpr_h("%s: domain %d\n", __func__, type);

	if (type == MSM_VIDC_DECODER) {
		index = 0;
		media_index = MEDIA_ENT_F_PROC_VIDEO_DECODER;
	} else if (type == MSM_VIDC_ENCODER) {
		index = 1;
		media_index = MEDIA_ENT_F_PROC_VIDEO_ENCODER;
	} else {
		return -EINVAL;
	}

	memset(&core->vdev[index], 0, sizeof(core->vdev[index]));
	init_completion(&core->vdev[index].released);
	core->vdev[index].vdev.release =
		msm_vidc_release_video_device;
	core->vdev[index].vdev.fops = core->v4l2_file_ops;
	if (type == MSM_VIDC_DECODER)
		core->vdev[index].vdev.ioctl_ops = core->v4l2_ioctl_ops_dec;
	else
		core->vdev[index].vdev.ioctl_ops = core->v4l2_ioctl_ops_enc;
	core->vdev[index].vdev.vfl_dir = VFL_DIR_M2M;
	core->vdev[index].type = type;
	core->vdev[index].vdev.v4l2_dev = &core->v4l2_dev;
	core->vdev[index].vdev.device_caps = core->capabilities[DEVICE_CAPS].value;
	video_set_drvdata(&core->vdev[index].vdev, core);
	rc = video_register_device(&core->vdev[index].vdev,
					VFL_TYPE_VIDEO, nr);
	if (rc) {
		d_vpr_e("Failed to register the video device\n");
		return rc;
	}
	core->vdev[index].registered = true;
	//rc = device_create_file(&core->vdev[index].vdev.dev, &dev_attr_link_name);
	if (rc) {
		d_vpr_e("Failed to create video device file\n");
		goto video_reg_failed;
	}
#ifdef CONFIG_MEDIA_CONTROLLER
	core->vdev[index].m2m_dev = v4l2_m2m_init(core->v4l2_m2m_ops);
	if (IS_ERR(core->vdev[index].m2m_dev)) {
		d_vpr_e("Failed to initialize V4L2 M2M device\n");
		rc = PTR_ERR(core->vdev[index].m2m_dev);
		goto m2m_init_failed;
	}
	rc = v4l2_m2m_register_media_controller(core->vdev[index].m2m_dev,
			&core->vdev[index].vdev, media_index);
	if (rc) {
		d_vpr_e("%s: m2m_dev controller register failed for session type %d\n",
			__func__, index);
		goto m2m_mc_failed;
	}
	core->vdev[index].m2m_registered = true;
#endif

	return 0;
#ifdef CONFIG_MEDIA_CONTROLLER
m2m_mc_failed:
	v4l2_m2m_release(core->vdev[index].m2m_dev);
m2m_init_failed:
#endif
video_reg_failed:
	msm_vidc_unpublish_video_device(&core->vdev[index]);
	msm_vidc_wait_video_device(&core->vdev[index]);
	video_set_drvdata(&core->vdev[index].vdev, NULL);
	core->vdev[index].registered = false;

	return rc;
}

static int msm_vidc_initialize_media(struct msm_vidc_core *core)
{
	int rc = 0, nr = BASE_DEVICE_NUMBER;

	rc = v4l2_device_register(&core->pdev->dev, &core->v4l2_dev);
	if (rc) {
		d_vpr_e("Failed to register v4l2 device\n");
		return rc;
	}

#ifdef CONFIG_MEDIA_CONTROLLER
	core->media_dev.dev = &core->pdev->dev;
	strscpy(core->media_dev.model, "msm_vidc_media", sizeof(core->media_dev.model));
	media_device_init(&core->media_dev);
	core->media_dev.ops = core->media_device_ops;
	core->v4l2_dev.mdev = &core->media_dev;
#endif

	/* setup the decoder device */
	rc = msm_vidc_register_video_device(core, MSM_VIDC_DECODER, nr);
	if (rc) {
		d_vpr_e("Failed to register video decoder\n");
		goto dec_reg_failed;
	}

	/* setup the encoder device */
	rc = msm_vidc_register_video_device(core, MSM_VIDC_ENCODER, nr + 1);
	if (rc) {
		d_vpr_e("Failed to register video encoder\n");
		goto enc_reg_failed;
	}
#ifdef CONFIG_MEDIA_CONTROLLER
	rc = media_device_register(&core->media_dev);
	if (rc) {
		d_vpr_e("%s: media_device_register failed with %d\n", __func__, rc);
		goto media_reg_failed;
	}
#endif

	return rc;
#ifdef CONFIG_MEDIA_CONTROLLER
media_reg_failed:
#endif
	msm_vidc_unregister_video_device(core, MSM_VIDC_ENCODER);
enc_reg_failed:
	msm_vidc_unregister_video_device(core, MSM_VIDC_DECODER);
dec_reg_failed:
#ifdef CONFIG_MEDIA_CONTROLLER
	media_device_cleanup(&core->media_dev);
#endif
	v4l2_device_unregister(&core->v4l2_dev);

	return rc;
}

static int msm_vidc_deinitialize_media(struct msm_vidc_core *core)
{
	/* Keep all shared resources until files and delayed references drain. */
	msm_vidc_unpublish_video_device(&core->vdev[0]);
	msm_vidc_unpublish_video_device(&core->vdev[1]);
	msm_vidc_wait_video_device(&core->vdev[0]);
	msm_vidc_wait_video_device(&core->vdev[1]);
	wait_event(core->instances_wait, !atomic_read(&core->live_instances));
	flush_workqueue(core->release_workq);
	msm_vidc_unregister_video_device(core, MSM_VIDC_ENCODER);
	msm_vidc_unregister_video_device(core, MSM_VIDC_DECODER);
#ifdef CONFIG_MEDIA_CONTROLLER
	media_device_unregister(&core->media_dev);
	media_device_cleanup(&core->media_dev);
#endif
	v4l2_device_unregister(&core->v4l2_dev);
	return 0;
}

#ifdef CONFIG_MSM_MMRM
static int msm_vidc_check_mmrm_support(struct msm_vidc_core *core)
{
	int rc = 0;

	if (!is_mmrm_supported(core))
		goto exit;

	if (!mmrm_client_check_scaling_supported(MMRM_CLIENT_CLOCK, 0)) {
		d_vpr_e("%s: MMRM not supported\n", __func__);
		core->platform->data.supports_mmrm = 0;
	}

exit:
	d_vpr_h("%s: %d\n", __func__, is_mmrm_supported(core));
	return rc;
}
#else
static int msm_vidc_check_mmrm_support(struct msm_vidc_core *core)
{
	core->platform->data.supports_mmrm = 0;

	return 0;
}
#endif

static int msm_vidc_deinitialize_core(struct msm_vidc_core *core)
{
	int rc = 0;

	if (!core) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	d_vpr_h("%s()\n", __func__);

	msm_vidc_update_core_state(core, MSM_VIDC_CORE_DEINIT, __func__);

	if (core->batch_workq)
		destroy_workqueue(core->batch_workq);

	if (core->pm_workq)
		destroy_workqueue(core->pm_workq);

	if (core->release_workq)
		destroy_workqueue(core->release_workq);
	core->release_workq = NULL;
	mutex_destroy(&core->lock);
	core->batch_workq = NULL;
	core->pm_workq = NULL;

	return rc;
}

static int msm_vidc_initialize_core(struct msm_vidc_core *core)
{
	int rc = 0;

	d_vpr_h("%s()\n", __func__);

	msm_vidc_update_core_state(core, MSM_VIDC_CORE_DEINIT, __func__);

	core->pm_workq = create_singlethread_workqueue("pm_workq");
	if (!core->pm_workq) {
		d_vpr_e("%s: create pm workq failed\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	core->batch_workq = create_singlethread_workqueue("batch_workq");
	if (!core->batch_workq) {
		d_vpr_e("%s: create batch workq failed\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	core->release_workq = alloc_ordered_workqueue("vidc_release", WQ_MEM_RECLAIM);
	if (!core->release_workq) {
		rc = -ENOMEM;
		goto exit;
	}
	atomic_set(&core->live_instances, 0);
	init_waitqueue_head(&core->instances_wait);

	core->packet_size = VIDC_IFACEQ_VAR_HUGE_PKT_SIZE;
	core->packet = devm_kzalloc(&core->pdev->dev, core->packet_size, GFP_KERNEL);
	if (!core->packet) {
		d_vpr_e("%s: failed to alloc core packet\n", __func__);
		rc = -ENOMEM;
		goto exit;
	}

	core->response_packet = devm_kzalloc(&core->pdev->dev, core->packet_size, GFP_KERNEL);
	if (!core->response_packet) {
		d_vpr_e("%s: failed to alloc core response packet\n", __func__);
		rc = -ENOMEM;
		goto exit;
	}

	mutex_init(&core->lock);
	INIT_LIST_HEAD(&core->instances);
	INIT_LIST_HEAD(&core->dangling_instances);

	INIT_DELAYED_WORK(&core->pm_work, venus_hfi_pm_work_handler);
	INIT_DELAYED_WORK(&core->fw_unload_work, msm_vidc_fw_unload_handler);
	INIT_WORK(&core->ssr_work, msm_vidc_ssr_handler);

	return 0;
exit:
	if (core->release_workq)
		destroy_workqueue(core->release_workq);
	core->release_workq = NULL;
	if (core->batch_workq)
		destroy_workqueue(core->batch_workq);
	if (core->pm_workq)
		destroy_workqueue(core->pm_workq);
	core->batch_workq = NULL;
	core->pm_workq = NULL;

	return rc;
}

static void msm_vidc_devm_deinit_core(void *res)
{
	struct msm_vidc_core *core = res;

	d_vpr_h("%s()\n", __func__);
	msm_vidc_deinitialize_core(core);
}

static int msm_vidc_devm_init_core(struct device *dev, struct msm_vidc_core *core)
{
	int rc = 0;

	if (!dev || !core) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	rc = msm_vidc_initialize_core(core);
	if (rc) {
		d_vpr_e("%s: init failed with %d\n", __func__, rc);
		return rc;
	}

	rc = devm_add_action_or_reset(dev, msm_vidc_devm_deinit_core, (void *)core);
	if (rc)
		return -EINVAL;

	return rc;
}

static void msm_vidc_devm_debugfs_put(void *res)
{
	struct dentry *parent = res;

	d_vpr_h("%s()\n", __func__);
	debugfs_remove_recursive(parent);
}

static struct dentry *msm_vidc_devm_debugfs_get(struct device *dev)
{
	struct dentry *parent = NULL;
	int rc = 0;

	if (!dev) {
		d_vpr_e("%s: invalid params\n", __func__);
		return NULL;
	}

	parent = msm_vidc_debugfs_init_drv();
	if (!parent)
		return NULL;

	rc = devm_add_action_or_reset(dev, msm_vidc_devm_debugfs_put, (void *)parent);
	if (rc)
		return NULL;

	return parent;
}

static void msm_vidc_release_context_bank(struct context_bank_info *cb)
{
	struct device *dev = cb->dev;

	if (!dev)
		return;
	if (cb->fault_registered) {
		/* The same provider locks dispatch and withdrawal, draining callbacks. */
		qcom_iommu_set_fault_handler(cb->domain, NULL, NULL);
		cb->fault_registered = false;
	}
	dev->dma_parms->max_segment_size = cb->saved_max_segment_size;
	dev->dma_parms->segment_boundary_mask = cb->saved_segment_boundary_mask;
	if (cb->dma_parms_owned) {
		kfree(dev->dma_parms);
		dev->dma_parms = NULL;
		cb->dma_parms_owned = false;
	}
	cb->dev = NULL;
	cb->domain = NULL;
}

static int msm_vidc_setup_context_bank(struct msm_vidc_core *core,
	struct device *dev)
{
	struct context_bank_info *cb = NULL;
	struct iommu_domain *domain;
	int rc = 0;

	cb = msm_vidc_get_context_bank_for_device(core, dev);
	if (!cb) {
		d_vpr_e("%s: Failed to get context bank device for %s\n",
			 __func__, dev_name(dev));
		return -EIO;
	}

	if (cb->dev)
		return -EBUSY;
	domain = iommu_get_domain_for_dev(dev);
	if (!domain) {
		d_vpr_e("%s: Failed to get iommu domain for %s\n", __func__, dev_name(dev));
		return -EIO;
	}

	if (cb->dma_mask) {
		rc = dma_set_mask_and_coherent(dev, cb->dma_mask);
		if (rc) {
			d_vpr_e("%s: dma_set_mask_and_coherent failed\n", __func__);
			return rc;
		}
	}

	/*
	 * When memory is fragmented, below configuration increases the
	 * possibility to get a mapping for buffer in the configured CB.
	 */

	/* remove kernel version condition once below api is whitelisted in pineapple */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
	iommu_dma_enable_best_fit_algo(dev);
#endif

	/*
	 * configure device segment size and segment boundary to ensure
	 * iommu mapping returns one mapping (which is required for partial
	 * cache operations)
	 */
	if (!dev->dma_parms) {
		dev->dma_parms = kzalloc(sizeof(*dev->dma_parms), GFP_KERNEL);
		if (!dev->dma_parms)
			return -ENOMEM;
		cb->dma_parms_owned = true;
	}
	cb->saved_max_segment_size = dev->dma_parms->max_segment_size;
	cb->saved_segment_boundary_mask = dev->dma_parms->segment_boundary_mask;
	cb->dev = dev;
	cb->domain = domain;
	dma_set_max_seg_size(dev, (unsigned int)DMA_BIT_MASK(32));
	dma_set_seg_boundary(dev, (unsigned long)DMA_BIT_MASK(64));

	/* Use the Qualcomm provider's synchronized registration on DMA domains. */
	rc = qcom_iommu_set_fault_handler(domain, msm_vidc_smmu_fault_handler, core);
	if (rc)
		goto release;
	cb->fault_registered = true;

	d_vpr_h(
		"%s: name %s addr start %x size %x secure %d dma_coherant %d "
		"region %d dev_name %s domain %pK dma_mask %llu\n",
		__func__, cb->name, cb->addr_range.start,
		cb->addr_range.size, cb->secure, cb->dma_coherant,
		cb->region, dev_name(cb->dev), cb->domain, cb->dma_mask);

	return rc;

release:
	msm_vidc_release_context_bank(cb);
	return rc;
}

static int msm_vidc_component_compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static void msm_vidc_component_release_of(struct device *dev, void *data)
{
	d_vpr_h("%s(): %s\n", __func__, of_node_full_name(data));
	of_node_put(data);
}

static int msm_vidc_component_bind(struct device *dev,
	struct device *parent, void *data)
{
	struct msm_vidc_core *core;
	int rc = 0;

	if (!dev || !parent || !data) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	core = (struct msm_vidc_core *)data;

	if (!dev->parent || dev->parent != parent) {
		d_vpr_e("%s: failed to find a parent for %s\n",
			__func__, dev_name(dev));
		return -ENODEV;
	}

	rc = msm_vidc_setup_context_bank(core, dev);
	if (rc) {
		d_vpr_e("%s: Failed to probe context bank - %s\n",
			__func__, dev_name(dev));
		return rc;
	}

	d_vpr_h("%s: Successfully probed context bank - %s\n",
		__func__, dev_name(dev));

	return rc;
}

static void msm_vidc_component_unbind(struct device *dev,
	struct device *parent, void *data)
{
	struct context_bank_info *cb;

	d_vpr_h("%s(): %s\n", __func__, dev_name(dev));
	cb = msm_vidc_get_context_bank_for_device(data, dev);
	if (cb && cb->dev == dev)
		msm_vidc_release_context_bank(cb);
}

static int msm_vidc_component_master_bind(struct device *dev)
{
	struct msm_vidc_core *core = dev_get_drvdata(dev);
	int rc = 0;

	d_vpr_h("%s(): %s\n", __func__, dev_name(dev));

	WRITE_ONCE(core->stopping, false);
	rc = component_bind_all(dev, core);
	if (rc) {
		d_vpr_e("%s: sub-device bind failed. rc %d\n", __func__, rc);
		WRITE_ONCE(core->stopping, true);
		return rc;
	}

	if (core->capabilities[SUPPORTS_SYNX_FENCE].value) {
		if (msm_vidc_synx_fence_enable) {
			/* register for synx fence */
			rc = call_fence_op(core, fence_register, core);
			if (rc) {
				d_vpr_e("%s: failed to register synx fence\n",
					__func__);
				goto unbind_components;
			}
			core->synx_registered = true;
		} else {
			/* override synx fence ops with dma fence ops */
			core->fence_ops = get_dma_fence_ops();
			if (!core->fence_ops) {
				d_vpr_e("%s: invalid dma fence ops\n", __func__);
				rc = -EINVAL;
				goto unbind_components;
			}
			core->capabilities[SUPPORTS_SYNX_FENCE].value = 0;
		}
	}

	rc = msm_vidc_initialize_media(core);
	if (rc) {
		d_vpr_e("%s: media initialization failed\n", __func__);
		goto deregister_fence;
	}

	rc = venus_hfi_queue_init(core);
	if (rc) {
		d_vpr_e("%s: interface queues init failed\n", __func__);
		goto queues_deinit;
	}

	rc = msm_vidc_core_init(core);
	if (rc) {
		d_vpr_e("%s: sys init failed\n", __func__);
		goto queues_deinit;
	}

ready:
	core->debugfs_root = msm_vidc_debugfs_init_core(core);
	/* Publish initialized core/media state before file opens acquire it. */
	smp_store_release(&core->ready, true);
	d_vpr_h("%s(): succssful\n", __func__);
	return 0;

queues_deinit:
	/* Failed core init stops hardware; drain handlers before freeing queues. */
	if (core->irq_requested)
		synchronize_irq(core->irq);
	cancel_delayed_work_sync(&core->pm_work);
	venus_hfi_queue_deinit(core);
	/**
	 * queues and core can be inited again during session_open.
	 * So don't declare as probe failure.
	 */
	goto ready;

deregister_fence:
	if (core->synx_registered) {
		call_fence_op(core, fence_deregister, core);
		core->synx_registered = false;
	}
unbind_components:
	WRITE_ONCE(core->stopping, true);
	component_unbind_all(dev, core);
	return rc;
}

static void msm_vidc_component_master_unbind(struct device *dev)
{
	struct msm_vidc_core *core = dev_get_drvdata(dev);

	d_vpr_h("%s(): %s\n", __func__, dev_name(dev));

	WRITE_ONCE(core->ready, false);
	core_lock(core, __func__);
	WRITE_ONCE(core->stopping, true);
	core_unlock(core, __func__);
	msm_vidc_deinitialize_media(core);
	debugfs_remove_recursive(core->debugfs_root);
	core->debugfs_root = NULL;
	cancel_work_sync(&core->ssr_work);
	cancel_delayed_work_sync(&core->fw_unload_work);
	msm_vidc_core_deinit(core, true);
	if (core->irq_requested)
		synchronize_irq(core->irq);
	cancel_delayed_work_sync(&core->pm_work);
	venus_hfi_queue_deinit(core);
	if (core->synx_registered) {
		call_fence_op(core, fence_deregister, core);
		core->synx_registered = false;
	}
	component_unbind_all(dev, core);

	d_vpr_h("%s(): succssful\n", __func__);
}

static const struct component_ops msm_vidc_component_ops = {
	.bind           = msm_vidc_component_bind,
	.unbind         = msm_vidc_component_unbind,
};

static const struct component_master_ops msm_vidc_component_master_ops = {
	.bind           = msm_vidc_component_master_bind,
	.unbind         = msm_vidc_component_master_unbind,
};

static int msm_vidc_remove_video_device(struct platform_device *pdev)
{
	struct msm_vidc_core *core;

	if (!pdev) {
		d_vpr_e("%s: invalid input %pK", __func__, pdev);
		return -EINVAL;
	}

	core = dev_get_drvdata(&pdev->dev);
	if (!core) {
		d_vpr_e("%s: invalid core\n", __func__);
		return -EINVAL;
	}

	d_vpr_h("%s()\n", __func__);

	mutex_lock(&msm_vidc_registration_lock);
	WRITE_ONCE(core->ready, false);
	WRITE_ONCE(core->stopping, true);

	/* destroy component master and deallocate match data */
	component_master_del(&pdev->dev, &msm_vidc_component_master_ops);

	d_vpr_h("depopulating sub devices\n");
	/*
	 * Trigger remove for each sub-device i.e. qcom,context-bank,xxxx
	 * When msm_vidc_remove is called for each sub-device, destroy
	 * context-bank mappings.
	 */
	of_platform_depopulate(&pdev->dev);

	sysfs_remove_group(&pdev->dev.kobj, &msm_vidc_core_attr_group);

	dev_set_drvdata(&pdev->dev, NULL);
	g_core = NULL;
	mutex_unlock(&msm_vidc_registration_lock);
	d_vpr_h("%s(): succssful\n", __func__);

	return 0;
}

static int msm_vidc_remove_context_bank(struct platform_device *pdev)
{
	d_vpr_h("%s(): %s\n", __func__, dev_name(&pdev->dev));

	component_del(&pdev->dev, &msm_vidc_component_ops);

	return 0;
}

static void msm_vidc_remove(struct platform_device *pdev)
{
	if (is_video_device(&pdev->dev))
		msm_vidc_remove_video_device(pdev);
	else if (is_video_context_bank_device(&pdev->dev))
		msm_vidc_remove_context_bank(pdev);
	else
		WARN_ON(1);
}

static int msm_vidc_probe_video_device(struct platform_device *pdev)
{
	int rc = 0;
	struct component_match *match = NULL;
	struct msm_vidc_core *core = NULL;
	struct device_node *child = NULL;
	int cb_count = 0;

	d_vpr_h("%s: %s\n", __func__, dev_name(&pdev->dev));

	mutex_lock(&msm_vidc_registration_lock);
	if (g_core) {
		mutex_unlock(&msm_vidc_registration_lock);
		return -EBUSY;
	}

	core = devm_kzalloc(&pdev->dev, sizeof(struct msm_vidc_core), GFP_KERNEL);
	if (!core) {
		d_vpr_e("%s: failed to alloc memory for core\n", __func__);
		mutex_unlock(&msm_vidc_registration_lock);
		return -ENOMEM;
	}
	core->stopping = true;
	g_core = core;

	core->pdev = pdev;
	dev_set_drvdata(&pdev->dev, core);

	core->debugfs_parent = msm_vidc_devm_debugfs_get(&pdev->dev);
	if (!core->debugfs_parent)
		d_vpr_h("Failed to create debugfs for msm_vidc\n");

	rc = msm_vidc_devm_init_core(&pdev->dev, core);
	if (rc) {
		d_vpr_e("%s: init core failed with %d\n", __func__, rc);
		goto init_core_failed;
	}

	rc = msm_vidc_init_platform(core);
	if (rc) {
		d_vpr_e("%s: init platform failed with %d\n", __func__, rc);
		rc = -EINVAL;
		goto init_plat_failed;
	}

	rc = msm_vidc_init_resources(core);
	if (rc) {
		d_vpr_e("%s: init resource failed with %d\n", __func__, rc);
		goto init_res_failed;
	}

	rc = msm_vidc_init_core_caps(core);
	if (rc) {
		d_vpr_e("%s: init core caps failed with %d\n", __func__, rc);
		goto init_res_failed;
	}

	rc = msm_vidc_init_instance_caps(core);
	if (rc) {
		d_vpr_e("%s: init inst cap failed with %d\n", __func__, rc);
		goto init_inst_caps_fail;
	}

	rc = sysfs_create_group(&pdev->dev.kobj, &msm_vidc_core_attr_group);
	if (rc) {
		d_vpr_e("Failed to create attributes\n");
		goto init_group_failed;
	}

	rc = msm_vidc_check_mmrm_support(core);
	if (rc) {
		d_vpr_e("Failed to check MMRM scaling support\n");
		rc = 0; /* Ignore error */
	}

	/* registering sub-device with component model framework */
	for_each_available_child_of_node(pdev->dev.of_node, child) {
		/* consider only context bank devices */
		if (!is_video_context_bank_device_node(child))
			continue;

		/* take refcount on device node */
		of_node_get(child);

		/* add entry into component_match array */
		component_match_add_release(&pdev->dev, &match, msm_vidc_component_release_of,
			msm_vidc_component_compare_of, child);
		if (IS_ERR(match)) {
			/* Neither the failed entry nor the iterator retains this node. */
			of_node_put(child);
			of_node_put(child);
			rc = PTR_ERR(match) ? PTR_ERR(match) : -ENOMEM;
			d_vpr_e("%s: component match add release failed\n", __func__);
			goto sub_dev_failed;
		}

		/* count context bank devices */
		cb_count++;
	}

	d_vpr_h("populating sub devices. count %d\n", cb_count);
	/*
	 * Trigger probe for each sub-device i.e. qcom,msm-vidc,context-bank.
	 * When msm_vidc_probe is called for each sub-device, parse the
	 * context-bank details.
	 */
	rc = of_platform_populate(pdev->dev.of_node, msm_vidc_dt_match, NULL,
			&pdev->dev);
	if (rc) {
		d_vpr_e("Failed to trigger probe for sub-devices\n");
		goto master_add_failed;
	}

	/**
	 * create and try to bring up aggregate device for master.
	 * match is a component_match_array and acts as a placeholder for
	 * components added via component_add().
	 */
	rc = component_master_add_with_match(&pdev->dev, &msm_vidc_component_master_ops, match);
	if (rc) {
		d_vpr_e("%s: component master add with match failed\n", __func__);
		goto master_add_failed;
	}

	d_vpr_h("%s(): succssful\n", __func__);

	mutex_unlock(&msm_vidc_registration_lock);
	return rc;

master_add_failed:
	of_platform_depopulate(&pdev->dev);
sub_dev_failed:
	sysfs_remove_group(&pdev->dev.kobj, &msm_vidc_core_attr_group);
init_group_failed:
init_inst_caps_fail:
init_res_failed:
init_plat_failed:
init_core_failed:
	dev_set_drvdata(&pdev->dev, NULL);
	g_core = NULL;

	mutex_unlock(&msm_vidc_registration_lock);
	return rc;
}

static int msm_vidc_probe_context_bank(struct platform_device *pdev)
{
	d_vpr_h("%s(): %s\n", __func__, dev_name(&pdev->dev));

	return component_add(&pdev->dev, &msm_vidc_component_ops);
}

static int msm_vidc_probe(struct platform_device *pdev)
{
	d_vpr_h("%s()\n", __func__);

	if (!pdev) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/*
	 * Sub devices probe will be triggered by of_platform_populate() towards
	 * the end of the probe function after msm-vidc device probe is
	 * completed. Return immediately after completing sub-device probe.
	 */
	if (is_video_device(&pdev->dev))
		return msm_vidc_probe_video_device(pdev);
	else if (is_video_context_bank_device(&pdev->dev))
		return msm_vidc_probe_context_bank(pdev);

	/* How did we end up here? */
	WARN_ON(1);
	return -EINVAL;
}

static int msm_vidc_pm_suspend(struct device *dev)
{
	int rc = 0;
	struct msm_vidc_core *core;
	enum msm_vidc_allow allow = MSM_VIDC_DISALLOW;

	/*
	 * Bail out if
	 * - driver possibly not probed yet
	 * - not the main device. We don't support power management on
	 *   subdevices (e.g. context banks)
	 */
	if (!dev || !dev->driver || !is_video_device(dev))
		return 0;

	core = dev_get_drvdata(dev);
	if (!core) {
		d_vpr_e("%s: invalid core\n", __func__);
		return -EINVAL;
	}

	core_lock(core, __func__);
	allow = msm_vidc_allow_pm_suspend(core);

	if (allow == MSM_VIDC_IGNORE) {
		d_vpr_h("%s: pm already suspended\n", __func__);
		msm_vidc_change_core_sub_state(core, 0, CORE_SUBSTATE_PM_SUSPEND, __func__);
		rc = 0;
		goto unlock;
	} else if (allow != MSM_VIDC_ALLOW) {
		d_vpr_h("%s: pm suspend not allowed\n", __func__);
		rc = 0;
		goto unlock;
	}

	d_vpr_h("%s\n", __func__);
	rc = msm_vidc_suspend(core);
	if (rc == -ENOTSUPP)
		rc = 0;
	else if (rc)
		d_vpr_e("Failed to suspend: %d\n", rc);
	else
		msm_vidc_change_core_sub_state(core, 0, CORE_SUBSTATE_PM_SUSPEND, __func__);

unlock:
	core_unlock(core, __func__);
	return rc;
}

static int msm_vidc_pm_resume(struct device *dev)
{
	struct msm_vidc_core *core;

	/*
	 * Bail out if
	 * - driver possibly not probed yet
	 * - not the main device. We don't support power management on
	 *   subdevices (e.g. context banks)
	 */
	if (!dev || !dev->driver || !is_video_device(dev))
		return 0;

	core = dev_get_drvdata(dev);
	if (!core) {
		d_vpr_e("%s: invalid core\n", __func__);
		return -EINVAL;
	}

	d_vpr_h("%s\n", __func__);

	/* remove PM suspend from core sub_state */
	core_lock(core, __func__);
	msm_vidc_change_core_sub_state(core, CORE_SUBSTATE_PM_SUSPEND, 0, __func__);
	core_unlock(core, __func__);

	return 0;
}

static const struct dev_pm_ops msm_vidc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(msm_vidc_pm_suspend, msm_vidc_pm_resume)
};

struct platform_driver msm_vidc_driver = {
	.probe = msm_vidc_probe,
	.remove = msm_vidc_remove,
	.driver = {
		.name = "msm_vidc_v4l2",
		.of_match_table = msm_vidc_dt_match,
		.pm = &msm_vidc_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static int __init msm_vidc_init(void)
{
	int rc = 0;

	d_vpr_h("%s\n", __func__);

	rc = platform_driver_register(&msm_vidc_driver);
	if (rc) {
		d_vpr_e("Failed to register platform driver\n");
		return rc;
	}
	d_vpr_h("%s(): succssful\n", __func__);

	return 0;
}

static void __exit msm_vidc_exit(void)
{
	d_vpr_h("%s()\n", __func__);

	platform_driver_unregister(&msm_vidc_driver);
	d_vpr_h("%s(): succssful\n", __func__);
}

module_init(msm_vidc_init);
module_exit(msm_vidc_exit);

MODULE_SOFTDEP("pre: subsys-pil-tz msm-mmrm");
MODULE_DESCRIPTION("Qualcomm native video decoder and encoder");
MODULE_LICENSE("GPL v2");
