// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/msm_audio.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <soc/qcom/secure_buffer.h>
#include <dsp/msm_audio_ion.h>

MODULE_IMPORT_NS("DMA_BUF");

struct msm_audio_ion_private {
	struct device chardev;
	struct device *cb_dev;
	struct class *class;
	struct cdev cdev;
	dev_t devt;
	bool smmu_enabled;
	bool stopping;
	u64 sid_bits;
};

struct msm_audio_map_ref {
	struct list_head node;
	struct file *file;
};

struct msm_audio_alloc_data {
	struct list_head node;
	struct list_head files;
	struct msm_audio_ion_private *priv;
	struct dma_buf *buffer;
	struct dma_buf_attachment *attach;
	struct sg_table *table;
	dma_addr_t address;
	phys_addr_t physical;
	size_t length;
	bool hyp_assigned;
};

/* Protect the registry, DMA/SCM transitions, file owners and provider removal. */
static DEFINE_MUTEX(msm_audio_ion_lock);
static LIST_HEAD(msm_audio_allocations);

static struct msm_audio_alloc_data *msm_audio_find_buffer(struct dma_buf *buffer)
{
	struct msm_audio_alloc_data *data;

	list_for_each_entry(data, &msm_audio_allocations, node)
		if (data->buffer == buffer)
			return data;
	return NULL;
}

static struct msm_audio_map_ref *
msm_audio_find_file(struct msm_audio_alloc_data *data, struct file *file)
{
	struct msm_audio_map_ref *ref;

	list_for_each_entry(ref, &data->files, node)
		if (ref->file == file)
			return ref;
	return NULL;
}

/* A single address may be advertised only for a contiguous mapped span. */
static int msm_audio_dma_span(struct sg_table *table, size_t length,
			      dma_addr_t *address)
{
	struct scatterlist *sg;
	dma_addr_t base, next;
	size_t covered = 0;
	unsigned int i;

	if (!table || !table->sgl || !table->nents || !length)
		return -EINVAL;
	base = sg_dma_address(table->sgl);
	for_each_sgtable_dma_sg(table, sg, i) {
		if (check_add_overflow(base, (dma_addr_t)covered, &next) ||
		    sg_dma_address(sg) != next || !sg_dma_len(sg) ||
		    check_add_overflow(covered, (size_t)sg_dma_len(sg), &covered))
			return -EINVAL;
		if (covered >= length) {
			if (check_add_overflow(base, (dma_addr_t)length - 1, &next))
				return -EOVERFLOW;
			*address = base;
			return 0;
		}
	}
	return -EINVAL;
}

static int msm_audio_physical_span(struct sg_table *table, size_t length,
				   phys_addr_t *address)
{
	struct scatterlist *sg;
	phys_addr_t base, next;
	size_t covered = 0;
	unsigned int i;

	if (!table->orig_nents)
		return -EINVAL;
	base = sg_phys(table->sgl);
	for_each_sgtable_sg(table, sg, i) {
		if (check_add_overflow(base, (phys_addr_t)covered, &next) ||
		    sg_phys(sg) != next || !sg->length ||
		    check_add_overflow(covered, (size_t)sg->length, &covered))
			return -EINVAL;
		if (covered >= length) {
			if (check_add_overflow(base, (phys_addr_t)length - 1, &next))
				return -EOVERFLOW;
			*address = base;
			return 0;
		}
	}
	return -EINVAL;
}

static int msm_audio_hyp_unassign(struct msm_audio_alloc_data *data)
{
	struct qcom_scm_vmperm destinations[] = {
		{ QCOM_SCM_VMID_HLOS, PERM_READ | PERM_WRITE | PERM_EXEC },
	};
	u64 sources = BIT_ULL(VMID_LPASS) | BIT_ULL(VMID_ADSP_HEAP);
	int ret;

	if (!data->hyp_assigned)
		return 0;
	ret = qcom_scm_assign_mem(data->physical, data->length, &sources,
				  destinations, ARRAY_SIZE(destinations));
	if (!ret)
		data->hyp_assigned = false;
	return ret;
}

/* Do not release a mapping while its memory remains assigned to the DSP. */
static int msm_audio_destroy_mapping(struct msm_audio_alloc_data *data)
{
	struct msm_audio_map_ref *ref, *next;
	struct device *dev = &data->priv->chardev;
	int ret;

	ret = msm_audio_hyp_unassign(data);
	if (ret)
		return ret;
	list_for_each_entry_safe(ref, next, &data->files, node) {
		list_del(&ref->node);
		kfree(ref);
	}
	list_del(&data->node);
	dma_buf_unmap_attachment_unlocked(data->attach, data->table, DMA_BIDIRECTIONAL);
	dma_buf_detach(data->buffer, data->attach);
	dma_buf_put(data->buffer);
	kfree(data);
	put_device(dev);
	module_put(THIS_MODULE);
	return 0;
}

static int msm_audio_map_fd(struct msm_audio_ion_private *priv,
			    struct file *file, int fd)
{
	struct msm_audio_alloc_data *data;
	struct msm_audio_map_ref *ref;
	struct dma_buf *buffer;
	int ret;

	buffer = dma_buf_get(fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	data = msm_audio_find_buffer(buffer);
	if (data) {
		if (data->priv != priv || list_empty(&data->files)) {
			ret = -EBUSY;
			goto put_buffer;
		}
		if (msm_audio_find_file(data, file)) {
			ret = 0;
			goto put_buffer;
		}
		ref = kzalloc(sizeof(*ref), GFP_KERNEL);
		if (!ref) {
			ret = -ENOMEM;
			goto put_buffer;
		}
		ref->file = file;
		list_add_tail(&ref->node, &data->files);
		ret = 0;
		goto put_buffer;
	}
	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref) {
		ret = -ENOMEM;
		goto put_buffer;
	}
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto free_ref;
	}
	data->priv = priv;
	data->buffer = buffer;
	data->length = buffer->size;
	INIT_LIST_HEAD(&data->files);
	data->attach = dma_buf_attach(buffer, priv->cb_dev);
	if (IS_ERR(data->attach)) {
		ret = PTR_ERR(data->attach);
		goto free_data;
	}
	data->table = dma_buf_map_attachment_unlocked(data->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(data->table)) {
		ret = PTR_ERR(data->table);
		goto detach;
	}
	ret = msm_audio_dma_span(data->table, data->length, &data->address);
	if (ret)
		goto unmap;
	if (priv->smmu_enabled) {
		if (data->address >> 32 || (data->length - 1) >> 32 ||
		    data->address + data->length - 1 > U32_MAX) {
			ret = -EOVERFLOW;
			goto unmap;
		}
		data->address |= priv->sid_bits;
	} else {
		ret = msm_audio_physical_span(data->table, data->length, &data->physical);
		if (ret)
			goto unmap;
	}
	if (!try_module_get(THIS_MODULE)) {
		ret = -ENODEV;
		goto unmap;
	}
	get_device(&priv->chardev);
	ref->file = file;
	list_add_tail(&ref->node, &data->files);
	list_add_tail(&data->node, &msm_audio_allocations);
	return 0;
unmap:
	dma_buf_unmap_attachment_unlocked(data->attach, data->table, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(buffer, data->attach);
free_data:
	kfree(data);
free_ref:
	kfree(ref);
put_buffer:
	dma_buf_put(buffer);
	return ret;
}

static int msm_audio_unmap_fd(struct msm_audio_ion_private *priv,
			      struct file *file, int fd)
{
	struct msm_audio_alloc_data *data;
	struct msm_audio_map_ref *ref;
	struct dma_buf *buffer;
	int ret = -ENOENT;

	buffer = dma_buf_get(fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	data = msm_audio_find_buffer(buffer);
	if (!data || data->priv != priv)
		goto put_buffer;
	ref = msm_audio_find_file(data, file);
	if (!ref)
		goto put_buffer;
	if (list_is_singular(&data->files)) {
		ret = msm_audio_destroy_mapping(data);
	} else {
		list_del(&ref->node);
		kfree(ref);
		ret = 0;
	}
put_buffer:
	dma_buf_put(buffer);
	return ret;
}

static int msm_audio_hyp_fd(struct msm_audio_ion_private *priv,
			    struct file *file, int fd, bool assign)
{
	struct qcom_scm_vmperm destinations[] = {
		{ VMID_LPASS, PERM_READ | PERM_WRITE },
		{ VMID_ADSP_HEAP, PERM_READ | PERM_WRITE },
	};
	struct msm_audio_alloc_data *data;
	struct dma_buf *buffer;
	u64 sources = BIT_ULL(QCOM_SCM_VMID_HLOS);
	int ret = -ENOENT;

	buffer = dma_buf_get(fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	data = msm_audio_find_buffer(buffer);
	if (!data || data->priv != priv || !msm_audio_find_file(data, file))
		goto put_buffer;
	if (!assign) {
		ret = msm_audio_hyp_unassign(data);
		goto put_buffer;
	}
	if (priv->smmu_enabled || !IS_ALIGNED(data->physical, SZ_4K) ||
	    !IS_ALIGNED(data->length, SZ_4K)) {
		ret = -EINVAL;
		goto put_buffer;
	}
	if (data->hyp_assigned) {
		ret = 0;
		goto put_buffer;
	}
	ret = qcom_scm_assign_mem(data->physical, data->length, &sources,
				  destinations, ARRAY_SIZE(destinations));
	if (!ret)
		data->hyp_assigned = true;
put_buffer:
	dma_buf_put(buffer);
	return ret;
}

int msm_audio_get_phy_addr(int fd, dma_addr_t *paddr, size_t *pa_len)
{
	struct msm_audio_alloc_data *data;
	struct dma_buf *buffer;
	int ret = -ENOENT;

	if (fd < 0 || !paddr || !pa_len)
		return -EINVAL;
	buffer = dma_buf_get(fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	mutex_lock(&msm_audio_ion_lock);
	data = msm_audio_find_buffer(buffer);
	if (data && !data->priv->stopping && !list_empty(&data->files)) {
		*paddr = data->address;
		*pa_len = data->length;
		ret = 0;
	}
	mutex_unlock(&msm_audio_ion_lock);
	dma_buf_put(buffer);
	return ret;
}
EXPORT_SYMBOL(msm_audio_get_phy_addr);

void msm_audio_ion_crash_handler(void)
{
	struct msm_audio_alloc_data *data, *next;
	int ret;

	mutex_lock(&msm_audio_ion_lock);
	list_for_each_entry_safe(data, next, &msm_audio_allocations, node) {
		ret = msm_audio_destroy_mapping(data);
		if (ret)
			dev_err(&data->priv->chardev, "secure buffer retained: %d\n", ret);
	}
	mutex_unlock(&msm_audio_ion_lock);
}
EXPORT_SYMBOL(msm_audio_ion_crash_handler);

static int msm_audio_ion_open(struct inode *inode, struct file *file)
{
	struct msm_audio_ion_private *priv =
		container_of(inode->i_cdev, struct msm_audio_ion_private, cdev);
	int ret = 0;

	mutex_lock(&msm_audio_ion_lock);
	if (priv->stopping) {
		ret = -ENODEV;
	} else {
		get_device(&priv->chardev);
		file->private_data = priv;
	}
	mutex_unlock(&msm_audio_ion_lock);
	return ret;
}

static int msm_audio_ion_release(struct inode *inode, struct file *file)
{
	struct msm_audio_ion_private *priv = file->private_data;
	struct msm_audio_alloc_data *data, *next;
	struct msm_audio_map_ref *ref;
	int ret;

	mutex_lock(&msm_audio_ion_lock);
	list_for_each_entry_safe(data, next, &msm_audio_allocations, node) {
		ref = msm_audio_find_file(data, file);
		if (!ref)
			continue;
		if (list_is_singular(&data->files)) {
			ret = msm_audio_destroy_mapping(data);
			if (!ret)
				continue;
			dev_err(&priv->chardev, "secure buffer retained on close: %d\n", ret);
		}
		list_del(&ref->node);
		kfree(ref);
	}
	mutex_unlock(&msm_audio_ion_lock);
	file->private_data = NULL;
	put_device(&priv->chardev);
	return 0;
}

static long msm_audio_ion_ioctl(struct file *file, unsigned int command,
				unsigned long argument)
{
	struct msm_audio_ion_private *priv = file->private_data;
	int fd = (int)argument;
	long ret;

	if (fd < 0 || (unsigned long)fd != argument)
		return -EINVAL;
	mutex_lock(&msm_audio_ion_lock);
	if (priv->stopping && command != IOCTL_UNMAP_PHYS_ADDR &&
	    command != IOCTL_UNMAP_HYP_ASSIGN) {
		ret = -ENODEV;
		goto unlock;
	}
	switch (command) {
	case IOCTL_MAP_PHYS_ADDR:
		ret = msm_audio_map_fd(priv, file, fd);
		break;
	case IOCTL_UNMAP_PHYS_ADDR:
		ret = msm_audio_unmap_fd(priv, file, fd);
		break;
	case IOCTL_MAP_HYP_ASSIGN:
		ret = msm_audio_hyp_fd(priv, file, fd, true);
		break;
	case IOCTL_UNMAP_HYP_ASSIGN:
		ret = msm_audio_hyp_fd(priv, file, fd, false);
		break;
	default:
		ret = -EINVAL;
		break;
	}
unlock:
	mutex_unlock(&msm_audio_ion_lock);
	return ret;
}

static const struct file_operations msm_audio_ion_fops = {
	.owner = THIS_MODULE,
	.open = msm_audio_ion_open,
	.release = msm_audio_ion_release,
	.unlocked_ioctl = msm_audio_ion_ioctl,
	.compat_ioctl = msm_audio_ion_ioctl,
};

static void msm_audio_ion_device_release(struct device *dev)
{
	struct msm_audio_ion_private *priv =
		container_of(dev, struct msm_audio_ion_private, chardev);

	put_device(priv->cb_dev);
	kfree(priv);
}

static int msm_audio_ion_probe(struct platform_device *pdev)
{
	struct msm_audio_ion_private *priv;
	struct of_phandle_args spec;
	const char *name;
	u64 mask = U64_MAX, sid;
	u32 version;
	int ret;

	if (!pdev->dev.of_node)
		return -EINVAL;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->cb_dev = get_device(&pdev->dev);
	priv->smmu_enabled = of_property_read_bool(pdev->dev.of_node, "qcom,smmu-enabled");
	device_initialize(&priv->chardev);
	priv->chardev.release = msm_audio_ion_device_release;
	priv->chardev.parent = &pdev->dev;
	if (priv->smmu_enabled) {
		ret = of_property_read_u32(pdev->dev.of_node, "qcom,smmu-version", &version);
		if (ret)
			goto put_device;
		of_property_read_u64(pdev->dev.of_node, "qcom,smmu-sid-mask", &mask);
		ret = of_parse_phandle_with_args(pdev->dev.of_node, "iommus",
						 "#iommu-cells", 0, &spec);
		if (ret)
			goto put_device;
		if (!spec.args_count) {
			of_node_put(spec.np);
			ret = -EINVAL;
			goto put_device;
		}
		sid = spec.args[0] & mask;
		of_node_put(spec.np);
		priv->sid_bits = sid << 32;
	}
	name = priv->smmu_enabled ? "msm_audio_ion" : "msm_audio_ion_cma";
	ret = alloc_chrdev_region(&priv->devt, 0, 1, name);
	if (ret)
		goto put_device;
	priv->class = class_create(name);
	if (IS_ERR(priv->class)) {
		ret = PTR_ERR(priv->class);
		goto unregister;
	}
	priv->chardev.class = priv->class;
	priv->chardev.devt = priv->devt;
	ret = dev_set_name(&priv->chardev, "%s", name);
	if (ret)
		goto destroy_class;
	cdev_init(&priv->cdev, &msm_audio_ion_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&priv->cdev, &priv->chardev);
	if (ret) {
		struct msm_audio_alloc_data *data, *next;

		mutex_lock(&msm_audio_ion_lock);
		priv->stopping = true;
		list_for_each_entry_safe(data, next, &msm_audio_allocations, node)
			if (data->priv == priv)
				msm_audio_destroy_mapping(data);
		mutex_unlock(&msm_audio_ion_lock);
		goto destroy_class;
	}
	platform_set_drvdata(pdev, priv);
	return 0;
destroy_class:
	class_destroy(priv->class);
unregister:
	unregister_chrdev_region(priv->devt, 1);
put_device:
	put_device(&priv->chardev);
	return ret;
}

static void msm_audio_ion_remove(struct platform_device *pdev)
{
	struct msm_audio_ion_private *priv = platform_get_drvdata(pdev);
	struct msm_audio_alloc_data *data, *next;
	int ret;

	mutex_lock(&msm_audio_ion_lock);
	priv->stopping = true;
	list_for_each_entry_safe(data, next, &msm_audio_allocations, node) {
		if (data->priv != priv)
			continue;
		ret = msm_audio_destroy_mapping(data);
		if (ret)
			dev_err(&priv->chardev, "secure buffer retained on remove: %d\n", ret);
	}
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&msm_audio_ion_lock);
	cdev_device_del(&priv->cdev, &priv->chardev);
	class_destroy(priv->class);
	unregister_chrdev_region(priv->devt, 1);
	put_device(&priv->chardev);
}

static const struct of_device_id msm_audio_ion_dt_match[] = {
	{ .compatible = "qcom,msm-audio-ion" },
	{ .compatible = "qcom,msm-audio-ion-cma" },
	{},
};
MODULE_DEVICE_TABLE(of, msm_audio_ion_dt_match);

static struct platform_driver msm_audio_ion_driver = {
	.driver = {
		.name = "msm-audio-ion",
		.owner = THIS_MODULE,
		.of_match_table = msm_audio_ion_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_audio_ion_probe,
	.remove = msm_audio_ion_remove,
};

module_platform_driver(msm_audio_ion_driver);
MODULE_DESCRIPTION("MSM Audio ION module");
MODULE_LICENSE("GPL v2");
