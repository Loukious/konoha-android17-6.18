/* Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/ipc_logging.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <ipc/gpr-lite.h>
#include <dsp/spf-core.h>
#include <dsp/msm_audio_ion.h>

#define MODULE_NAME "audio-pkt"
#define AUDPKT_DRIVER_NAME "aud_pasthru_adsp"
#define MAX_PACKET_SIZE 4096
#define AUDIO_PKT_BACKUP_BUFFERS_NUM 10
#define AUDIO_PKT_IPC_LOG_PAGE_CNT 2
#define APM_CMD_SHARED_MEM_MAP_REGIONS 0x0100100C
#define APM_MEMORY_MAP_BIT_MASK_IS_OFFSET_MODE 0x00000004UL
#define APM_CMD_GRAPH_CLOSE 0x01001004
#define APM_CMD_GRAPH_OPEN 0x01001000
#define APM_CMD_SET_CFG 0x01001006
#define APM_CMD_GRAPH_STOP 0x01001003
#define APM_CMD_GRAPH_START 0x01001002

static int audio_pkt_debug_mask;
module_param_named(debug_mask, audio_pkt_debug_mask, int, 0664);

struct audio_pkt_priv {
	struct device chardev;
	struct device *parent;
	struct class *class;
	struct cdev cdev;
	dev_t devt;
	/* Serialize file sessions and hold the GPR binding through sends. */
	struct mutex lock;
	struct gpr_device *adev;
	struct sk_buff_head queue;
	struct sk_buff_head backups;
	wait_queue_head_t readq;
	struct work_struct backup_work;
	void *log;
	unsigned int opens;
	bool bound;
	bool ever_bound;
	bool stopping;
};

struct spf_cmd_basic_rsp {
	u32 opcode;
	s32 status;
};

struct audio_pkt_apm_cmd_shared_mem_map_regions_t {
	u16 mem_pool_id;
	u16 num_regions;
	u32 property_flag;
};

struct audio_pkt_apm_shared_map_region_payload_t {
	u32 shm_addr_lsw;
	u32 shm_addr_msw;
	u32 mem_size_bytes;
};

static DEFINE_MUTEX(audio_pkt_provider_lock);
static struct audio_pkt_priv *audio_pkt_provider;
static bool audio_pkt_provider_transition;

static struct audio_pkt_priv *audio_pkt_provider_get(void)
{
	struct audio_pkt_priv *priv;

	mutex_lock(&audio_pkt_provider_lock);
	priv = audio_pkt_provider;
	if (priv)
		get_device(&priv->chardev);
	mutex_unlock(&audio_pkt_provider_lock);
	return priv;
}

static int audio_pkt_validate_header(struct gpr_hdr *hdr, size_t count,
				     u32 *header_bytes)
{
	u32 bytes = GPR_PKT_GET_PACKET_BYTE_SIZE(hdr->header);
	u32 header = GPR_PKT_GET_HEADER_BYTE_SIZE(hdr->header);

	if (count < GPR_HDR_SIZE || count > MAX_PACKET_SIZE || bytes != count ||
	    header < GPR_HDR_SIZE || header > bytes)
		return -EINVAL;
	*header_bytes = header;
	return 0;
}

static int audio_pkt_update_regions(struct gpr_hdr *hdr, size_t count, u32 header)
{
	struct audio_pkt_apm_cmd_shared_mem_map_regions_t map;
	struct audio_pkt_apm_shared_map_region_payload_t region;
	size_t needed, regions_bytes, length;
	dma_addr_t address;
	u8 *payload = (u8 *)hdr + header;
	unsigned int i;
	int ret;

	if (count - header < sizeof(map))
		return -EINVAL;
	memcpy(&map, payload, sizeof(map));
	if (!map.num_regions ||
	    check_mul_overflow((size_t)map.num_regions, sizeof(region), &regions_bytes) ||
	    check_add_overflow(sizeof(map), regions_bytes, &needed) ||
	    needed > count - header)
		return -EINVAL;
	if (!(map.property_flag & APM_MEMORY_MAP_BIT_MASK_IS_OFFSET_MODE))
		return 0;
	for (i = 0; i < map.num_regions; i++) {
		u8 *data = payload + sizeof(map) + i * sizeof(region);

		memcpy(&region, data, sizeof(region));
		if (region.shm_addr_msw || region.shm_addr_lsw > INT_MAX ||
		    !region.mem_size_bytes)
			return -EINVAL;
		ret = msm_audio_get_phy_addr((int)region.shm_addr_lsw, &address, &length);
		if (ret)
			return ret;
		if (region.mem_size_bytes > length)
			return -EINVAL;
		region.shm_addr_lsw = lower_32_bits(address);
		region.shm_addr_msw = upper_32_bits(address);
		memcpy(data, &region, sizeof(region));
	}
	return 0;
}

static int audio_pkt_open(struct inode *inode, struct file *file)
{
	struct audio_pkt_priv *priv =
		container_of(inode->i_cdev, struct audio_pkt_priv, cdev);
	int ret = 0;

	mutex_lock(&priv->lock);
	if (READ_ONCE(priv->stopping)) {
		ret = -ENODEV;
	} else if (priv->opens == UINT_MAX) {
		ret = -EMFILE;
	} else {
		get_device(&priv->chardev);
		priv->opens++;
		file->private_data = priv;
	}
	mutex_unlock(&priv->lock);
	return ret;
}

static int audio_pkt_release(struct inode *inode, struct file *file)
{
	struct audio_pkt_priv *priv = file->private_data;

	mutex_lock(&priv->lock);
	priv->opens--;
	if (!priv->opens && priv->ever_bound && !READ_ONCE(priv->stopping)) {
		skb_queue_purge(&priv->queue);
		spf_core_apm_close_all();
		msm_audio_ion_crash_handler();
	}
	mutex_unlock(&priv->lock);
	file->private_data = NULL;
	put_device(&priv->chardev);
	return 0;
}

static ssize_t audio_pkt_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct audio_pkt_priv *priv = file->private_data;
	struct sk_buff *skb;
	unsigned long flags;
	ssize_t ret;

	if (!count)
		return 0;
	for (;;) {
		spin_lock_irqsave(&priv->queue.lock, flags);
		skb = skb_peek(&priv->queue);
		if (skb && count < skb->len) {
			spin_unlock_irqrestore(&priv->queue.lock, flags);
			return -EMSGSIZE;
		}
		if (skb)
			skb = __skb_dequeue(&priv->queue);
		spin_unlock_irqrestore(&priv->queue.lock, flags);
		if (skb)
			break;
		if (READ_ONCE(priv->stopping) || !READ_ONCE(priv->bound))
			return -ENETRESET;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(priv->readq,
					       !skb_queue_empty(&priv->queue) ||
					       READ_ONCE(priv->stopping) ||
					       !READ_ONCE(priv->bound));
		if (ret)
			return ret;
	}
	ret = skb->len;
	if (copy_to_user(buf, skb->data, skb->len))
		ret = -EFAULT;
	kfree_skb(skb);
	return ret;
}

static ssize_t audio_pkt_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct audio_pkt_priv *priv = file->private_data;
	struct gpr_hdr *hdr;
	u32 header;
	int ret;

	if (!count)
		return 0;
	if (count < GPR_HDR_SIZE || count > MAX_PACKET_SIZE)
		return -EINVAL;
	hdr = memdup_user(buf, count);
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);
	ret = audio_pkt_validate_header(hdr, count, &header);
	if (ret)
		goto free;
	if (hdr->opcode == APM_CMD_SHARED_MEM_MAP_REGIONS) {
		ret = audio_pkt_update_regions(hdr, count, header);
		if (ret)
			goto free;
	}
	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		goto free;
	if (READ_ONCE(priv->stopping) || !READ_ONCE(priv->bound) || !priv->adev) {
		ret = -ENETRESET;
	} else {
		ret = gpr_send_pkt(priv->adev, (struct gpr_pkt *)hdr);
		if (ret == -ECONNRESET)
			ret = -ENETRESET;
	}
	mutex_unlock(&priv->lock);
free:
	kfree(hdr);
	return ret < 0 ? ret : count;
}

static __poll_t audio_pkt_poll(struct file *file, poll_table *wait)
{
	struct audio_pkt_priv *priv = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &priv->readq, wait);
	if (!skb_queue_empty(&priv->queue))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(priv->stopping) || !READ_ONCE(priv->bound))
		mask |= EPOLLERR | EPOLLHUP;
	return mask;
}

static const struct file_operations audio_pkt_fops = {
	.owner = THIS_MODULE,
	.open = audio_pkt_open,
	.release = audio_pkt_release,
	.read = audio_pkt_read,
	.write = audio_pkt_write,
	.poll = audio_pkt_poll,
};

static void audio_pkt_alloc_backup(struct work_struct *work)
{
	struct audio_pkt_priv *priv =
		container_of(work, struct audio_pkt_priv, backup_work);
	struct sk_buff *skb;
	unsigned int i;

	for (i = 0; i < AUDIO_PKT_BACKUP_BUFFERS_NUM &&
	     skb_queue_len(&priv->backups) < AUDIO_PKT_BACKUP_BUFFERS_NUM &&
	     !READ_ONCE(priv->stopping); i++) {
		skb = alloc_skb(MAX_PACKET_SIZE, GFP_KERNEL);
		if (!skb)
			break;
		skb_queue_tail(&priv->backups, skb);
	}
}

static int audio_pkt_srvc_callback(struct gpr_device *adev, void *data)
{
	struct audio_pkt_priv *priv = dev_get_drvdata(&adev->dev);
	struct spf_cmd_basic_rsp basic;
	struct gpr_hdr *hdr = data;
	struct sk_buff *skb;
	u32 bytes, header;
	int ret;

	if (!priv || !hdr || READ_ONCE(priv->stopping) || !READ_ONCE(priv->bound))
		return -ENODEV;
	bytes = GPR_PKT_GET_PACKET_BYTE_SIZE(hdr->header);
	ret = audio_pkt_validate_header(hdr, bytes, &header);
	if (ret)
		return ret;
	if (hdr->opcode == GPR_IBASIC_RSP_RESULT) {
		if (bytes - header < sizeof(basic))
			return -EINVAL;
		memcpy(&basic, (u8 *)data + header, sizeof(basic));
		if (basic.opcode == APM_CMD_GRAPH_OPEN || basic.opcode == APM_CMD_GRAPH_CLOSE ||
		    basic.opcode == APM_CMD_SET_CFG || basic.opcode == APM_CMD_GRAPH_STOP ||
		    basic.opcode == APM_CMD_GRAPH_START)
			ipc_log_string(priv->log, "audio-pkt callback opcode:0x%x token:0x%x",
				       basic.opcode, hdr->token);
	}
	skb = alloc_skb(bytes, GFP_ATOMIC);
	if (!skb) {
		skb = skb_dequeue(&priv->backups);
		queue_work(system_unbound_wq, &priv->backup_work);
		if (!skb)
			return -ENOMEM;
		if (skb_tailroom(skb) < bytes) {
			kfree_skb(skb);
			return -EMSGSIZE;
		}
	}
	skb_put_data(skb, data, bytes);
	skb_queue_tail(&priv->queue, skb);
	if (READ_ONCE(audio_pkt_debug_mask) & 1)
		ipc_log_string(priv->log, "received %u bytes", bytes);
	wake_up_interruptible(&priv->readq);
	return 0;
}

static int audio_pkt_probe(struct gpr_device *adev)
{
	struct audio_pkt_priv *priv = audio_pkt_provider_get();
	int ret = 0;

	if (!priv)
		return -EPROBE_DEFER;
	mutex_lock(&priv->lock);
	if (READ_ONCE(priv->stopping) || priv->adev) {
		ret = -EBUSY;
	} else {
		priv->adev = adev;
		priv->ever_bound = true;
		dev_set_drvdata(&adev->dev, priv);
		WRITE_ONCE(priv->bound, true);
	}
	mutex_unlock(&priv->lock);
	if (ret)
		put_device(&priv->chardev);
	else
		wake_up_interruptible(&priv->readq);
	return ret;
}

static int audio_pkt_remove(struct gpr_device *adev)
{
	struct audio_pkt_priv *priv = dev_get_drvdata(&adev->dev);

	if (!priv)
		return 0;
	/* Wake blocked readers before taking the command and binding mutex. */
	WRITE_ONCE(priv->bound, false);
	wake_up_interruptible(&priv->readq);
	mutex_lock(&priv->lock);
	skb_queue_purge(&priv->queue);
	dev_set_drvdata(&adev->dev, NULL);
	priv->adev = NULL;
	mutex_unlock(&priv->lock);
	put_device(&priv->chardev);
	return 0;
}

static const struct of_device_id audio_pkt_match_table[] = {
	{ .compatible = "qcom,audio-pkt" },
	{},
};
MODULE_DEVICE_TABLE(of, audio_pkt_match_table);

static struct gpr_driver audio_pkt_driver = {
	.probe = audio_pkt_probe,
	.remove = audio_pkt_remove,
	.callback = audio_pkt_srvc_callback,
	.driver = {
		.name = MODULE_NAME,
		.of_match_table = of_match_ptr(audio_pkt_match_table),
	},
};

static void audio_pkt_device_release(struct device *dev)
{
	struct audio_pkt_priv *priv = container_of(dev, struct audio_pkt_priv, chardev);

	if (priv->log)
		ipc_log_context_destroy(priv->log);
	put_device(priv->parent);
	kfree(priv);
}

static void audio_pkt_platform_stop(struct audio_pkt_priv *priv)
{
	struct sk_buff *skb;

	mutex_lock(&audio_pkt_provider_lock);
	audio_pkt_provider = NULL;
	audio_pkt_provider_transition = true;
	WRITE_ONCE(priv->stopping, true);
	mutex_unlock(&audio_pkt_provider_lock);
	wake_up_interruptible(&priv->readq);
	cancel_work_sync(&priv->backup_work);
	while ((skb = skb_dequeue(&priv->queue)))
		kfree_skb(skb);
	skb_queue_purge(&priv->backups);
}

static void audio_pkt_end_transition(void)
{
	mutex_lock(&audio_pkt_provider_lock);
	audio_pkt_provider_transition = false;
	mutex_unlock(&audio_pkt_provider_lock);
}

static int audio_pkt_platform_driver_probe(struct platform_device *pdev)
{
	struct audio_pkt_priv *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->parent = get_device(&pdev->dev);
	device_initialize(&priv->chardev);
	priv->chardev.release = audio_pkt_device_release;
	priv->chardev.parent = &pdev->dev;
	mutex_init(&priv->lock);
	skb_queue_head_init(&priv->queue);
	skb_queue_head_init(&priv->backups);
	init_waitqueue_head(&priv->readq);
	INIT_WORK(&priv->backup_work, audio_pkt_alloc_backup);
	priv->log = ipc_log_context_create(AUDIO_PKT_IPC_LOG_PAGE_CNT, AUDPKT_DRIVER_NAME, 0);
	if (!priv->log) {
		ret = -ENOMEM;
		goto put;
	}
	mutex_lock(&audio_pkt_provider_lock);
	if (audio_pkt_provider || audio_pkt_provider_transition) {
		mutex_unlock(&audio_pkt_provider_lock);
		ret = -EBUSY;
		goto put;
	}
	audio_pkt_provider = priv;
	mutex_unlock(&audio_pkt_provider_lock);
	ret = alloc_chrdev_region(&priv->devt, 0, 1, AUDPKT_DRIVER_NAME);
	if (ret)
		goto stop;
	priv->class = class_create(AUDPKT_DRIVER_NAME);
	if (IS_ERR(priv->class)) {
		ret = PTR_ERR(priv->class);
		goto unregister;
	}
	priv->chardev.class = priv->class;
	priv->chardev.devt = priv->devt;
	ret = dev_set_name(&priv->chardev, "%s", AUDPKT_DRIVER_NAME);
	if (ret)
		goto destroy_class;
	cdev_init(&priv->cdev, &audio_pkt_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&priv->cdev, &priv->chardev);
	if (ret)
		goto destroy_class;
	queue_work(system_unbound_wq, &priv->backup_work);
	ret = gpr_driver_register(&audio_pkt_driver);
	if (ret)
		goto delete_device;
	platform_set_drvdata(pdev, priv);
	return 0;
delete_device:
	cdev_device_del(&priv->cdev, &priv->chardev);
destroy_class:
	class_destroy(priv->class);
unregister:
	unregister_chrdev_region(priv->devt, 1);
stop:
	audio_pkt_platform_stop(priv);
	audio_pkt_end_transition();
put:
	put_device(&priv->chardev);
	return ret;
}

static void audio_pkt_platform_driver_remove(struct platform_device *pdev)
{
	struct audio_pkt_priv *priv = platform_get_drvdata(pdev);

	mutex_lock(&audio_pkt_provider_lock);
	audio_pkt_provider = NULL;
	audio_pkt_provider_transition = true;
	WRITE_ONCE(priv->stopping, true);
	mutex_unlock(&audio_pkt_provider_lock);
	wake_up_interruptible(&priv->readq);
	gpr_driver_unregister(&audio_pkt_driver);
	cancel_work_sync(&priv->backup_work);
	skb_queue_purge(&priv->queue);
	skb_queue_purge(&priv->backups);
	mutex_lock(&priv->lock);
	if (priv->ever_bound) {
		spf_core_apm_close_all();
		msm_audio_ion_crash_handler();
	}
	mutex_unlock(&priv->lock);
	cdev_device_del(&priv->cdev, &priv->chardev);
	class_destroy(priv->class);
	unregister_chrdev_region(priv->devt, 1);
	platform_set_drvdata(pdev, NULL);
	put_device(&priv->chardev);
	audio_pkt_end_transition();
}

static const struct of_device_id audio_pkt_platform_match_table[] = {
	{ .compatible = "qcom,audio-pkt-core-platform" },
	{},
};
MODULE_DEVICE_TABLE(of, audio_pkt_platform_match_table);

static struct platform_driver audio_pkt_core_platform_driver = {
	.probe = audio_pkt_platform_driver_probe,
	.remove = audio_pkt_platform_driver_remove,
	.driver = {
		.name = MODULE_NAME,
		.of_match_table = of_match_ptr(audio_pkt_platform_match_table),
	},
};

module_platform_driver(audio_pkt_core_platform_driver);
MODULE_DESCRIPTION("MSM Audio Packet Driver");
MODULE_LICENSE("GPL v2");
