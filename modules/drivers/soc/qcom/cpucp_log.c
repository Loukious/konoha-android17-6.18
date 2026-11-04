// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/scmi_protocol.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/debugfs.h>

#define MAX_PRINT_SIZE		1024
#define MAX_BUF_NUM		4
#define MAX_RESIDUAL_SIZE	MAX_PRINT_SIZE
#define SIZE_ADJUST		4
#define SRC_OFFSET		4
#define CREATE_TRACE_POINTS
#include "trace_cpucp.h"
#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
#define CPUCP_CTRL_ALGO_STR (0x435055435043544c) /* CPUCPCTL ASCII */
#define LOG_NAME_MAX_LENGTH 32

enum cpucp_ctrl_param_ids {
	LOGBUF_IDX = 1,
	DDR_LOGBUF_FLUSH = 3,
	MODULE_LOG_LEVEL = 5,
};

struct __packed scmi_module_log_level_t {
	uint32_t module_id;
	uint32_t log_level;
};

struct __packed scmi_module_log_data_t {
	uint32_t module_id;
	char module_name[LOG_NAME_MAX_LENGTH];
	uint32_t remaining_modules;
};
#endif

enum cpucp_log_type {
	CPUCP,
	PDP0,
	PDP1,
	NUM_CPUCP_LOG_TYPES
};

static const char * const cpucp_log_names[NUM_CPUCP_LOG_TYPES] = {
	[CPUCP]		= "cpucp",
	[PDP0]		= "pdp0",
	[PDP1]		= "pdp1",
};

struct remote_mem {
	void __iomem *start;
	u32 size;
	u32 phys_addr;
};

struct cpucp_buf {
	struct list_head node;
	char *buf;
	u32 size;
	u32 cpy_idx;
};

struct cpucp_log_info {
	struct remote_mem *rmem;
	struct mbox_client cl;
	struct mbox_chan *ch;
	struct mbox_chan *tx_ch;
	struct delayed_work work;
	struct device *dev;
	enum cpucp_log_type type;
	u32 enabled;
	struct mutex control_lock;
	bool stopping;
	struct workqueue_struct *wq;
	struct dentry *dir;
	bool pdp_parent;
#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
	struct scmi_device *sdev;
	struct scmi_protocol_handle *ph;
	const struct qcom_scmi_vendor_ops *ops;
#endif
	void __iomem *base;
	unsigned int rmem_idx;
	unsigned int num_bufs;
	unsigned int total_buf_size;
	char *rem_buf;
	char *glb_buf;
	int  rem_len;
	struct list_head free_buffers_list;
	struct list_head full_buffers_list;
	spinlock_t free_list_lock;
	spinlock_t full_list_lock;
};

static DEFINE_MUTEX(cpucp_dirs_lock);
static unsigned int pdp_dir_users;
static struct dentry *pdp_log_dir;

#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
static int flush_cpucp_log(void *data, u64 val)
{
	struct cpucp_log_info *info = data;
	u64 wire_value = cpu_to_le64(val);
	int ret;

	mutex_lock(&info->control_lock);
	if (info->stopping || !info->ops) {
		ret = -ENODEV;
		goto out;
	}
	ret = info->ops->set_param(info->ph, &wire_value, CPUCP_CTRL_ALGO_STR,
				   DDR_LOGBUF_FLUSH, sizeof(wire_value));
out:
	mutex_unlock(&info->control_lock);
	return ret;
}


static int set_log_level(void *data, u64 val)
{
	struct cpucp_log_info *info = data;
	u64 wire_value = cpu_to_le64(val);
	int ret;

	mutex_lock(&info->control_lock);
	if (info->stopping || !info->ops) {
		ret = -ENODEV;
		goto out;
	}
	ret = info->ops->set_param(info->ph, &wire_value, CPUCP_CTRL_ALGO_STR,
				   LOGBUF_IDX, sizeof(wire_value));
out:
	mutex_unlock(&info->control_lock);
	return ret;
}


static int get_log_level(void *data, u64 *val)
{
	struct cpucp_log_info *info = data;
	u64 wire_value = 0;
	int ret;

	mutex_lock(&info->control_lock);
	if (info->stopping || !info->ops) {
		ret = -ENODEV;
		goto out;
	}
	ret = info->ops->get_param(info->ph, &wire_value, CPUCP_CTRL_ALGO_STR,
				   LOGBUF_IDX, 0, sizeof(wire_value));
	if (!ret)
		*val = le64_to_cpu(wire_value);
out:
	mutex_unlock(&info->control_lock);
	return ret;
}


static ssize_t get_module_ids(struct file *file, char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	struct cpucp_log_info *info = file->private_data;
	struct scmi_module_log_data_t response = { };
	char *buffer;
	size_t bytes;
	unsigned int cursor;
	ssize_t ret;

	buffer = kzalloc(8192, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	mutex_lock(&info->control_lock);
	if (info->stopping || !info->ops) {
		ret = -ENODEV;
		goto out;
	}
	bytes = scnprintf(buffer, 8192, "Module ID Module Name\n");
	for (cursor = 0; cursor < 128; cursor++) {
		memset(&response, 0, sizeof(response));
		response.module_id = cpu_to_le32(cursor);
		ret = info->ops->get_param(info->ph, &response, CPUCP_CTRL_ALGO_STR,
			MODULE_LOG_LEVEL, sizeof(response.module_id), sizeof(response));
		if (ret)
			goto out;
		bytes += scnprintf(buffer + bytes, 8192 - bytes, "%9u %.*s\n",
			le32_to_cpu(response.module_id), LOG_NAME_MAX_LENGTH, response.module_name);
		if (!le32_to_cpu(response.remaining_modules))
			break;
	}
	if (cursor == 128) {
		ret = -EOVERFLOW;
		goto out;
	}
	ret = simple_read_from_buffer(user_buf, count, ppos, buffer, bytes);
out:
	mutex_unlock(&info->control_lock);
	kfree(buffer);
	return ret;
}


static ssize_t set_module_log_level(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct cpucp_log_info *info = file->private_data;
	struct scmi_module_log_level_t configuration = { };
	u32 module, level;
	char *buffer, *first, *second;
	ssize_t ret;

	if (!count || count > PAGE_SIZE)
		return -EINVAL;
	buffer = memdup_user_nul(user_buf, count);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	first = strim(buffer);
	second = strpbrk(first, " 	");
	if (!second) {
		ret = -EINVAL;
		goto free;
	}
	*second++ = '\0';
	second = strim(second);
	ret = kstrtou32(first, 10, &module);
	if (ret)
		goto free;
	ret = kstrtou32(second, 10, &level);
	if (ret)
		goto free;
	configuration.module_id = cpu_to_le32(module);
	configuration.log_level = cpu_to_le32(level);
	mutex_lock(&info->control_lock);
	if (info->stopping || !info->ops)
		ret = -ENODEV;
	else
		ret = info->ops->set_param(info->ph, &configuration, CPUCP_CTRL_ALGO_STR,
					  MODULE_LOG_LEVEL, sizeof(configuration));
	mutex_unlock(&info->control_lock);
free:
	kfree(buffer);
	return ret < 0 ? ret : count;
}


DEFINE_DEBUGFS_ATTRIBUTE(log_level_ops, get_log_level, set_log_level, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(flush_log_ops, NULL, flush_cpucp_log, "%llu\n");

static const struct file_operations avbl_module_ops = {
	.owner = THIS_MODULE,
	.read = get_module_ids,
	.open = simple_open,
	.llseek = default_llseek,
};

static const struct file_operations set_module_log_level_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = set_module_log_level,
};
#endif

static int set_log_enabled(void *data, u64 val)
{
	struct cpucp_log_info *info = data;
	u32 enable = val ? 1 : 0;
	int ret;

	mutex_lock(&info->control_lock);
	if (info->stopping) {
		ret = -ENODEV;
		goto out;
	}
	ret = mbox_send_message(info->tx_ch, &enable);
	if (ret < 0)
		goto out;
	mbox_client_txdone(info->tx_ch, 0);
	info->enabled = enable;
	ret = 0;
out:
	mutex_unlock(&info->control_lock);
	return ret;
}


static int get_log_enabled(void *data, u64 *val)
{
	struct cpucp_log_info *info = data;
	int ret = 0;
	mutex_lock(&info->control_lock);
	if (info->stopping)
		ret = -ENODEV;
	else
		*val = info->enabled;
	mutex_unlock(&info->control_lock);
	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(log_enabled_ops, get_log_enabled, set_log_enabled, "%llu\n");

static inline bool get_last_newline(char *buf, int size, int *cnt)
{
	int i;

	for (i = (size - 1); i >= 0 ; i--) {
		if (buf[i] == '\n') {
			buf[i] = '\0';
			*cnt = i + 1;
			return true;
		}
	}

	*cnt = size;
	return false;
}

#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
static int scmi_cpucp_log_create_fs_entries(struct cpucp_log_info *info)
{
	struct dentry *entry;
	info->dir = debugfs_create_dir("cpucp_log", NULL);
	if (IS_ERR_OR_NULL(info->dir))
		return info->dir ? PTR_ERR(info->dir) : -ENOMEM;
	entry = debugfs_create_file("log_level", 0644, info->dir, info, &log_level_ops);
	if (IS_ERR_OR_NULL(entry))
		return entry ? PTR_ERR(entry) : -ENOMEM;
	entry = debugfs_create_file("flush_log", 0200, info->dir, info, &flush_log_ops);
	if (IS_ERR_OR_NULL(entry))
		return entry ? PTR_ERR(entry) : -ENOMEM;
	entry = debugfs_create_file("module_log_level", 0200, info->dir, info, &set_module_log_level_fops);
	if (IS_ERR_OR_NULL(entry))
		return entry ? PTR_ERR(entry) : -ENOMEM;
	entry = debugfs_create_file("available_modules", 0400, info->dir, info, &avbl_module_ops);
	return IS_ERR_OR_NULL(entry) ? (entry ? PTR_ERR(entry) : -ENOMEM) : 0;
}

#endif

static int pdp_log_create_fs_entries(struct cpucp_log_info *info)
{
	struct dentry *entry;
	int ret = 0;

	mutex_lock(&cpucp_dirs_lock);
	if (!pdp_log_dir) {
		pdp_log_dir = debugfs_create_dir("pdp_log", NULL);
		if (IS_ERR_OR_NULL(pdp_log_dir)) {
			ret = pdp_log_dir ? PTR_ERR(pdp_log_dir) : -ENOMEM;
			pdp_log_dir = NULL;
			goto out;
		}
	}
	info->dir = debugfs_create_dir(cpucp_log_names[info->type], pdp_log_dir);
	if (IS_ERR_OR_NULL(info->dir)) {
		ret = info->dir ? PTR_ERR(info->dir) : -ENOMEM;
		goto out;
	}
	pdp_dir_users++;
	info->pdp_parent = true;
	entry = debugfs_create_file("log_enabled", 0644, info->dir, info, &log_enabled_ops);
	if (IS_ERR_OR_NULL(entry))
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
out:
	if (ret && !pdp_dir_users) {
		debugfs_remove_recursive(pdp_log_dir);
		pdp_log_dir = NULL;
	}
	mutex_unlock(&cpucp_dirs_lock);
	return ret;
}


static inline void print_to_trace(enum cpucp_log_type type, char *str)
{
	switch (type) {
	case CPUCP:
		trace_cpucp_log(str);
		break;
	case PDP0:
		trace_pdp0_log(str);
		break;
	case PDP1:
		trace_pdp1_log(str);
		break;
	default:
		break;
	}
}

static void cpucp_log_work(struct work_struct *work)
{
	struct cpucp_log_info *info = container_of(work, struct cpucp_log_info, work.work);
	struct cpucp_buf *buffer;
	unsigned long flags;
	char text[MAX_PRINT_SIZE + 1], *source;
	unsigned int remaining, chunk;
	int consumed;
	bool newline;

	for (;;) {
		spin_lock_irqsave(&info->full_list_lock, flags);
		if (list_empty(&info->full_buffers_list)) {
			spin_unlock_irqrestore(&info->full_list_lock, flags);
			return;
		}
		buffer = list_first_entry(&info->full_buffers_list, struct cpucp_buf, node);
		list_del(&buffer->node);
		spin_unlock_irqrestore(&info->full_list_lock, flags);
		source = buffer->buf + buffer->cpy_idx - info->rem_len;
		memcpy(source, info->rem_buf, info->rem_len);
		remaining = buffer->size + info->rem_len;
		info->rem_len = 0;
		while (remaining) {
			chunk = min_t(unsigned int, remaining, MAX_PRINT_SIZE);
			newline = get_last_newline(source, chunk, &consumed);
			if (!newline && remaining < MAX_PRINT_SIZE) {
				info->rem_len = remaining;
				memcpy(info->rem_buf, source, remaining);
				break;
			}
			memcpy(text, source, consumed);
			text[consumed] = '\0';
			print_to_trace(info->type, text);
			source += consumed;
			remaining -= consumed;
		}
		spin_lock_irqsave(&info->free_list_lock, flags);
		list_add_tail(&buffer->node, &info->free_buffers_list);
		spin_unlock_irqrestore(&info->free_list_lock, flags);
	}
}


static struct cpucp_buf *get_free_buffer(struct cpucp_log_info *info)
{
	struct cpucp_buf *buf_node;
	unsigned long flags;

	spin_lock_irqsave(&info->free_list_lock, flags);
	if (list_empty(&info->free_buffers_list)) {
		spin_unlock_irqrestore(&info->free_list_lock, flags);
		return NULL;
	}

	buf_node = list_first_entry(&info->free_buffers_list,
					struct cpucp_buf, node);
	list_del(&buf_node->node);
	spin_unlock_irqrestore(&info->free_list_lock, flags);
	return buf_node;
}

static void cpucp_log_rx(struct mbox_client *client, void *message)
{
	struct cpucp_log_info *info = dev_get_drvdata(client->dev);
	struct cpucp_buf *buffer;
	void __iomem *source;
	u32 marker, length, start, capacity;
	u64 data;
	unsigned int index;
	unsigned long flags;

	if (READ_ONCE(info->stopping))
		return;
	switch (info->type) {
	case CPUCP:
		marker = readl(info->rmem[0].start);
		if (marker >= SRC_OFFSET && marker <= info->rmem[0].size) {
			source = info->rmem[0].start + SRC_OFFSET;
			length = marker - SIZE_ADJUST;
		} else if (marker > info->rmem[0].size && marker <= info->total_buf_size) {
			source = info->rmem[1].start;
			length = marker - info->rmem[0].size;
		} else {
			dev_err_ratelimited(info->dev, "Invalid CPUCP log marker %u\n", marker);
			return;
		}
		break;
	case PDP0:
	case PDP1:
		if (!message)
			return;
		memcpy(&data, message, sizeof(data));
		data >>= 32;
		start = (data & GENMASK(23, 0)) << 8;
		capacity = (((data >> 24) & 0xff) + 1) << 8;
		for (index = 0; index < info->num_bufs; index++)
			if (start == info->rmem[index].phys_addr)
				break;
		if (index == info->num_bufs)
			return;
		source = info->rmem[index].start;
		length = min(capacity, info->rmem[index].size);
		break;
	default:
		return;
	}
	if (!length)
		return;
	buffer = get_free_buffer(info);
	if (!buffer) {
		dev_err_ratelimited(info->dev, "Log buffer pool full\n");
		return;
	}
	memcpy_fromio(buffer->buf + buffer->cpy_idx, source, length);
	buffer->size = length;
	spin_lock_irqsave(&info->full_list_lock, flags);
	list_add_tail(&buffer->node, &info->full_buffers_list);
	spin_unlock_irqrestore(&info->full_list_lock, flags);
	queue_delayed_work(info->wq, &info->work, 0);
}


static int populate_free_buffers(struct platform_device *pdev, struct cpucp_log_info *info)
{
	struct device *dev = &pdev->dev;
	struct cpucp_buf *buffers;
	struct resource *resource;
	u32 size = 0;
	size_t allocation;
	unsigned int index;

	for (index = 0; platform_get_resource(pdev, IORESOURCE_MEM, index); index++)
		;
	if (!index || (info->type == CPUCP && index != 2))
		return -EINVAL;
	info->num_bufs = index;
	info->rmem = devm_kcalloc(dev, index, sizeof(*info->rmem), GFP_KERNEL);
	if (!info->rmem)
		return -ENOMEM;
	for (index = 0; index < info->num_bufs; index++) {
		resource = platform_get_resource(pdev, IORESOURCE_MEM, index);
		if (!resource_size(resource) || resource_size(resource) > U32_MAX ||
		    (size && resource_size(resource) != size) || resource->start > U32_MAX)
			return -EINVAL;
		size = resource_size(resource);
		if (info->type == CPUCP && size < SIZE_ADJUST)
			return -EINVAL;
		info->rmem[index].phys_addr = resource->start;
		info->rmem[index].size = size;
		info->rmem[index].start = devm_ioremap(dev, resource->start, size);
		if (!info->rmem[index].start)
			return -ENOMEM;
		if (check_add_overflow(info->total_buf_size, size, &info->total_buf_size))
			return -EOVERFLOW;
	}
	if (check_mul_overflow((size_t)MAX_BUF_NUM, (size_t)size + MAX_PRINT_SIZE, &allocation))
		return -EOVERFLOW;
	info->glb_buf = devm_kzalloc(dev, allocation, GFP_KERNEL);
	info->rem_buf = devm_kzalloc(dev, MAX_RESIDUAL_SIZE, GFP_KERNEL);
	buffers = devm_kcalloc(dev, MAX_BUF_NUM, sizeof(*buffers), GFP_KERNEL);
	if (!info->glb_buf || !info->rem_buf || !buffers)
		return -ENOMEM;
	INIT_LIST_HEAD(&info->free_buffers_list);
	INIT_LIST_HEAD(&info->full_buffers_list);
	for (index = 0; index < MAX_BUF_NUM; index++) {
		buffers[index].buf = info->glb_buf + index * ((size_t)size + MAX_PRINT_SIZE);
		buffers[index].cpy_idx = MAX_PRINT_SIZE;
		list_add_tail(&buffers[index].node, &info->free_buffers_list);
	}
	return 0;
}


static void cpucp_log_debugfs_cleanup(void *arg)
{
	struct cpucp_log_info *info = arg;
	if (!IS_ERR_OR_NULL(info->dir))
		debugfs_remove_recursive(info->dir);
	info->dir = NULL;
	mutex_lock(&cpucp_dirs_lock);
	if (info->pdp_parent) {
		info->pdp_parent = false;
		if (!--pdp_dir_users) {
			debugfs_remove_recursive(pdp_log_dir);
			pdp_log_dir = NULL;
		}
	}
	mutex_unlock(&cpucp_dirs_lock);
}

static void cpucp_log_stop(void *arg)
{
	struct cpucp_log_info *info = arg;
	mutex_lock(&info->control_lock);
	WRITE_ONCE(info->stopping, true);
	mutex_unlock(&info->control_lock);
	if (!IS_ERR_OR_NULL(info->tx_ch) && info->tx_ch != info->ch)
		mbox_free_channel(info->tx_ch);
	if (!IS_ERR_OR_NULL(info->ch))
		mbox_free_channel(info->ch);
	info->ch = info->tx_ch = NULL;
	if (info->wq) {
		cancel_delayed_work_sync(&info->work);
		destroy_workqueue(info->wq);
		info->wq = NULL;
	}
}

#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
static void cpucp_log_protocol_put(void *arg)
{
	struct cpucp_log_info *info = arg;
	info->sdev->handle->devm_protocol_put(info->sdev, QCOM_SCMI_VENDOR_PROTOCOL);
	put_device(&info->sdev->dev);
	info->sdev = NULL;
	info->ops = NULL;
}
#endif

static int cpucp_log_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cpucp_log_info *info;
	u32 type;
	int ret, channels;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	ret = of_property_read_u32(dev->of_node, "qcom,log-type", &type);
	/* Pinned Sun uses type zero for qcom,cpucp-log; Tuna omits that field. */
	if (ret && !of_find_property(dev->of_node, "qcom,log-type", NULL) &&
	    of_device_is_compatible(dev->of_node, "qcom,cpucp-log")) {
		type = CPUCP;
		ret = 0;
	}
	if (ret || type >= NUM_CPUCP_LOG_TYPES ||
	    (type == CPUCP) != of_device_is_compatible(dev->of_node, "qcom,cpucp-log"))
		return -EINVAL;
	info->type = type;
	info->dev = dev;
	mutex_init(&info->control_lock);
	spin_lock_init(&info->free_list_lock);
	spin_lock_init(&info->full_list_lock);
	INIT_DEFERRABLE_WORK(&info->work, cpucp_log_work);
	dev_set_drvdata(dev, info);
#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
	if (info->type == CPUCP) {
		info->sdev = get_qcom_scmi_device();
		if (IS_ERR(info->sdev))
			return PTR_ERR(info->sdev);
		if (!info->sdev || !info->sdev->handle)
			return -ENODEV;
		get_device(&info->sdev->dev);
		info->ops = info->sdev->handle->devm_protocol_get(info->sdev, QCOM_SCMI_VENDOR_PROTOCOL, &info->ph);
		if (IS_ERR(info->ops)) {
			ret = PTR_ERR(info->ops);
			info->ops = NULL;
			put_device(&info->sdev->dev);
			return ret;
		}
		ret = devm_add_action_or_reset(dev, cpucp_log_protocol_put, info);
		if (ret)
			return ret;
	}
#endif
	ret = populate_free_buffers(pdev, info);
	if (ret)
		return ret;
	info->wq = alloc_workqueue("cpucp_%s", WQ_FREEZABLE | WQ_UNBOUND, 1,
				   cpucp_log_names[info->type]);
	if (!info->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, cpucp_log_stop, info);
	if (ret)
		return ret;
	info->cl.dev = dev;
	info->cl.tx_block = false;
	info->cl.knows_txdone = true;
	info->cl.rx_callback = cpucp_log_rx;
	channels = of_count_phandle_with_args(dev->of_node, "mboxes", "#mbox-cells");
	if (channels < 1 || channels > 2)
		return channels < 0 ? channels : -EINVAL;
	info->ch = mbox_request_channel(&info->cl, 0);
	if (IS_ERR(info->ch))
		return dev_err_probe(dev, PTR_ERR(info->ch), "RX mailbox unavailable\n");
	if (channels == 2) {
		info->tx_ch = mbox_request_channel(&info->cl, 1);
		if (IS_ERR(info->tx_ch))
			return dev_err_probe(dev, PTR_ERR(info->tx_ch), "TX mailbox unavailable\n");
	} else
		info->tx_ch = info->ch;
	/* Register cleanup before partial debugfs construction can fail. */
	ret = devm_add_action_or_reset(dev, cpucp_log_debugfs_cleanup, info);
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
	if (info->type == CPUCP)
		return scmi_cpucp_log_create_fs_entries(info);
#endif
	if (info->type != CPUCP)
		return pdp_log_create_fs_entries(info);
	return 0;
}


static void cpucp_log_remove(struct platform_device *pdev)
{
	/* Devres removes/drains debugfs, stops RX/work, then releases maps/protocol. */
}


static const struct of_device_id cpucp_log[] = {
	{ .compatible = "qcom,cpucp-log" },
	{ .compatible = "qcom,pdp-log" },
	{},
};

static struct platform_driver cpucp_log_driver = {
	.driver = {
		.name = "cpucp-log",
		.of_match_table = cpucp_log,
	},
	.probe = cpucp_log_probe,
	.remove = cpucp_log_remove,
};
module_platform_driver(cpucp_log_driver);
MODULE_DEVICE_TABLE(of, cpucp_log);
MODULE_SOFTDEP("pre: qcom_cpucp qcom_scmi_client");

MODULE_LICENSE("GPL");
