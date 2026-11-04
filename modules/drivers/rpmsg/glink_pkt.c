// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/platform_device.h>
#include <linux/ipc_logging.h>
#include <linux/refcount.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/skbuff.h>
#include <linux/rpmsg.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/idr.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/termios.h>
#include <linux/string.h>
#include <linux/sched/mm.h>
#include <linux/vmalloc.h>
#include <linux/limits.h>
#include <linux/workqueue.h>

#include "qcom_glink_native.h"

/* Define IPC Logging Macros */
#define GLINK_PKT_IPC_LOG_PAGE_CNT 32
static void *glink_pkt_ilctxt;

#define GLINK_PKT_INFO(x, ...)						     \
	ipc_log_string(glink_pkt_ilctxt, "[%s]: "x, __func__, ##__VA_ARGS__)

#define GLINK_PKT_ERR(x, ...)						     \
do {									     \
	printk_ratelimited("%s[%s]: " x, KERN_ERR, __func__, ##__VA_ARGS__); \
	ipc_log_string(glink_pkt_ilctxt, "%s[%s]: " x, "", __func__,         \
			##__VA_ARGS__);\
} while (0)

#define GLINK_PKT_IOCTL_MAGIC (0xC3)

#define GLINK_PKT_IOCTL_QUEUE_RX_INTENT \
	_IOW(GLINK_PKT_IOCTL_MAGIC, 0, unsigned int)

struct glink_pkt_zerocopy_receive {
	__u64 address;      /* in: address of mapping */
	__u32 length;       /* out: number of bytes to map/mapped */
	__u32 offset;   /* out: amount of bytes to skip */
};

#define GLINK_PKT_IOCTL_ZC_RECV \
	_IOWR(GLINK_PKT_IOCTL_MAGIC, 1, struct glink_pkt_zerocopy_receive)

#define GLINK_PKT_IOCTL_ZC_DONE \
	_IOWR(GLINK_PKT_IOCTL_MAGIC, 2, struct glink_pkt_zerocopy_receive)

#define MODULE_NAME "glink_pkt"
static dev_t glink_pkt_major;
static struct class *glink_pkt_class;
#define GLINK_PKT_MAX_DEVS 256

static DEFINE_IDA(glink_pkt_minor_ida);

/**
 * struct glink_pkt - driver context, relates rpdev to cdev
 * @dev:	glink pkt device
 * @cdev:	cdev for the glink pkt device
 * @lock:	synchronization of @rpdev and @open_tout modifications
 * @ch_open:	wait object for opening the glink channel
 * @refcount:	count how many userspace clients have handles
 * @rpdev:	underlaying rpmsg device
 * @rx_done:	cache whether rpdev can support external rx done
 * @queue_lock:	synchronization of @queue operations
 * @queue:	incoming message queue
 * @readq:	wait object for incoming queue
 * @sig_change:	flag to indicate serial signal change
 * @fragmented_read: set from dt node for partial read
 * @enable_ch_close: set from dt node for unregister driver on close syscall
 * @drv_lock:	lock to protect rpmsg driver variable
 * @drv:	rpmsg driver for registering to rpmsg bus
 * @drv_registered: status of rpmsg driver
 * @dev_name:	/dev/@dev_name for glink_pkt device
 * @ch_name:	glink channel to match to
 * @edge:	glink edge to match to
 * @open_tout:	timeout for open syscall, configurable in sysfs
 * @rskb_read_lock: Lock to protect rskb during read syscalls
 * @rskb:       current skb being read
 * @rdata:      data pointer in current skb
 * @rdata_len:  remaining data to be read from skb
 */
struct glink_pkt_device {
	struct list_head owner_node;
	bool removing;
	bool disconnecting;
	bool unregistering;
	struct completion drv_idle;
	struct work_struct idle_work;
	u64 read_generation;
	u64 mapping_cookie;
	struct device dev;
	struct cdev cdev;

	struct mutex lock;
	struct completion ch_open;
	refcount_t refcount;
	struct rpmsg_device *rpdev;
	bool rx_done;

	spinlock_t queue_lock;
	struct sk_buff_head queue;
	struct sk_buff_head pending;
	wait_queue_head_t readq;
	int sig_change;
	bool fragmented_read;
	bool enable_ch_close;

	struct mutex drv_lock;
	struct rpmsg_driver drv;
	bool drv_registered;

	const char *dev_name;
	const char *ch_name;
	const char *edge;
	int open_tout;

	struct mutex rskb_read_lock;
	struct sk_buff *rskb;
	unsigned char *rdata;
	size_t rdata_len;
};

/* Exactly fits skb->cb on both supported native word sizes. */
struct glink_pkt_zerocopy_cb {
	void *external;
	struct mm_struct *mm;
	unsigned long leading_page;
	unsigned long trailing_page;
	unsigned long address;
	u64 cookie;
};

struct glink_pkt_owner {
	struct list_head devices;
};

static const struct vm_operations_struct glink_pkt_vm_ops;
static void glink_pkt_revoke_pending(struct glink_pkt_device *gpdev);

static unsigned long glink_pkt_mapped_length(struct sk_buff *skb)
{
	struct glink_pkt_zerocopy_cb *cb = (void *)skb->cb;

	return PAGE_ALIGN(offset_in_page(cb->external) + skb->len);
}

static unsigned char *glink_pkt_skb_data(struct sk_buff *skb)
{
	struct glink_pkt_zerocopy_cb *cb = (void *)skb->cb;

	return cb->external ? cb->external : skb->data;
}

#define dev_to_gpdev(_dev) container_of(_dev, struct glink_pkt_device, dev)
#define cdev_to_gpdev(_cdev) container_of(_cdev, struct glink_pkt_device, cdev)
#define drv_to_rpdrv(_drv) container_of(_drv, struct rpmsg_driver, drv)
#define rpdrv_to_gpdev(_rdrv) container_of(_rdrv, struct glink_pkt_device, drv)

static ssize_t open_timeout_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct glink_pkt_device *gpdev = dev_to_gpdev(dev);
	int value, ret;

	ret = kstrtoint(buf, 0, &value);
	if (ret)
		return ret;
	if (value < 0 || value > INT_MAX / 1000)
		return -ERANGE;
	mutex_lock(&gpdev->lock);
	gpdev->open_tout = value;
	mutex_unlock(&gpdev->lock);
	return n;
}

static ssize_t open_timeout_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct glink_pkt_device *gpdev = dev_to_gpdev(dev);
	ssize_t ret;

	mutex_lock(&gpdev->lock);
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", gpdev->open_tout);
	mutex_unlock(&gpdev->lock);

	return ret;
}

static DEVICE_ATTR_RW(open_timeout);

static void glink_pkt_kfree_skb(struct glink_pkt_device *gpdev, struct sk_buff *skb)
{
	struct glink_pkt_zerocopy_cb *cb = (void *)skb->cb;

	/* All mappings have already been revoked before returning native data. */
	if (cb->leading_page)
		free_page(cb->leading_page);
	if (cb->trailing_page)
		free_page(cb->trailing_page);
	if (cb->mm)
		mmdrop(cb->mm);
	if (cb->external) {
		int ret = qcom_glink_rx_done(gpdev->rpdev->ept, cb->external);

		if (ret)
			GLINK_PKT_ERR("RX acknowledgement failed on %s: %d\n", gpdev->ch_name, ret);
	}
	/* skb->head still belongs to alloc_skb, never to the transport. */
	kfree_skb(skb);
}

static void glink_pkt_clear_queues(struct glink_pkt_device *gpdev)
{
	struct sk_buff *skb;
	unsigned long flags;

	/* RX_DONE may wait on transport TX space; never send it under queue_lock. */
	gpdev->read_generation++;
	if (gpdev->rskb) {
		glink_pkt_kfree_skb(gpdev, gpdev->rskb);
		gpdev->rskb = NULL;
		gpdev->rdata = NULL;
		gpdev->rdata_len = 0;
	}
	for (;;) {
		spin_lock_irqsave(&gpdev->queue_lock, flags);
		skb = skb_dequeue(&gpdev->queue);
		spin_unlock_irqrestore(&gpdev->queue_lock, flags);
		if (!skb)
			break;
		glink_pkt_kfree_skb(gpdev, skb);
	}
	WARN_ON_ONCE(!skb_queue_empty(&gpdev->pending));
}

static int glink_pkt_rpdev_no_copy_cb(struct rpmsg_device *rpdev, void *buf,
		int len, void *priv, u32 addr)
{
	struct glink_pkt_device *gpdev = dev_get_drvdata(&rpdev->dev);
	struct sk_buff *skb;
	unsigned long flags;

	if (!gpdev)
		return -ENETRESET;
	if (len < 0 || (len && !buf))
		return -EINVAL;
	skb = alloc_skb(0, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	memset(skb->cb, 0, sizeof(skb->cb));
	((struct glink_pkt_zerocopy_cb *)skb->cb)->external = buf;
	skb->len = len;
	spin_lock_irqsave(&gpdev->queue_lock, flags);
	if (gpdev->disconnecting || gpdev->removing) {
		spin_unlock_irqrestore(&gpdev->queue_lock, flags);
		/* Framework acknowledges an unaccepted external buffer itself. */
		kfree_skb(skb);
		return -ENETRESET;
	}
	skb_queue_tail(&gpdev->queue, skb);
	spin_unlock_irqrestore(&gpdev->queue_lock, flags);
	wake_up_interruptible(&gpdev->readq);
	return RPMSG_DEFER;
}

static int glink_pkt_rpdev_copy_cb(struct rpmsg_device *rpdev, void *buf,
		int len, void *priv, u32 addr)
{
	struct glink_pkt_device *gpdev = dev_get_drvdata(&rpdev->dev);
	struct sk_buff *skb;
	unsigned long flags;

	if (!gpdev)
		return -ENETRESET;
	if (len < 0 || (len && !buf))
		return -EINVAL;
	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	memset(skb->cb, 0, sizeof(skb->cb));
	skb_put_data(skb, buf, len);
	spin_lock_irqsave(&gpdev->queue_lock, flags);
	if (gpdev->disconnecting || gpdev->removing) {
		spin_unlock_irqrestore(&gpdev->queue_lock, flags);
		/* Framework acknowledges an unaccepted external buffer itself. */
		kfree_skb(skb);
		return -ENETRESET;
	}
	skb_queue_tail(&gpdev->queue, skb);
	spin_unlock_irqrestore(&gpdev->queue_lock, flags);
	wake_up_interruptible(&gpdev->readq);
	return 0;
}

static int glink_pkt_rpdev_cb(struct rpmsg_device *rpdev, void *buf, int len,
		void *priv, u32 addr)
{
	struct glink_pkt_device *gpdev = dev_get_drvdata(&rpdev->dev);

	if (!gpdev)
		return -ENETRESET;
	return gpdev->rx_done ? glink_pkt_rpdev_no_copy_cb(rpdev, buf, len, priv, addr) :
		glink_pkt_rpdev_copy_cb(rpdev, buf, len, priv, addr);
}

static int glink_pkt_rpdev_sigs(struct rpmsg_device *rpdev, void *priv, u32 old, u32 new)
{
	struct glink_pkt_device *gpdev = dev_get_drvdata(&rpdev->dev);
	unsigned long flags;

	if (!gpdev)
		return -ENETRESET;
	spin_lock_irqsave(&gpdev->queue_lock, flags);
	if (!gpdev->disconnecting && !gpdev->removing)
		gpdev->sig_change = true;
	spin_unlock_irqrestore(&gpdev->queue_lock, flags);
	wake_up_interruptible(&gpdev->readq);
	return 0;
}

static int glink_pkt_rpdev_probe(struct rpmsg_device *rpdev)
{
	struct glink_pkt_device *gpdev = rpdrv_to_gpdev(drv_to_rpdrv(rpdev->dev.driver));
	unsigned long flags;

	mutex_lock(&gpdev->lock);
	if (gpdev->removing) {
		mutex_unlock(&gpdev->lock);
		return -ENODEV;
	}
	gpdev->rpdev = rpdev;
	gpdev->rx_done = strcmp(gpdev->edge, "slate") && qcom_glink_rx_done_supported(rpdev->ept);
	dev_set_drvdata(&rpdev->dev, gpdev);
	spin_lock_irqsave(&gpdev->queue_lock, flags);
	gpdev->disconnecting = false;
	spin_unlock_irqrestore(&gpdev->queue_lock, flags);
	if (strcmp(gpdev->edge, "slate"))
		qcom_glink_register_signals_cb(rpdev->ept, glink_pkt_rpdev_sigs);
	complete_all(&gpdev->ch_open);
	mutex_unlock(&gpdev->lock);
	return 0;
}

static void glink_pkt_rpdev_remove(struct rpmsg_device *rpdev)
{
	struct glink_pkt_device *gpdev = dev_get_drvdata(&rpdev->dev);
	unsigned long flags;

	if (!gpdev)
		return;
	/* Detach waits for any atomic signal callback before clearing drvdata. */
	if (strcmp(gpdev->edge, "slate"))
		qcom_glink_register_signals_cb(rpdev->ept, NULL);
	mutex_lock(&gpdev->lock);
	spin_lock_irqsave(&gpdev->queue_lock, flags);
	gpdev->disconnecting = true;
	spin_unlock_irqrestore(&gpdev->queue_lock, flags);
	reinit_completion(&gpdev->ch_open);
	mutex_unlock(&gpdev->lock);
	wake_up_interruptible(&gpdev->readq);
	/* MM lock must precede gpdev->lock, including VMA close callbacks. */
	glink_pkt_revoke_pending(gpdev);
	mutex_lock(&gpdev->lock);
	glink_pkt_clear_queues(gpdev);
	gpdev->rpdev = NULL;
	dev_set_drvdata(&rpdev->dev, NULL);
	mutex_unlock(&gpdev->lock);
}

static int glink_pkt_drv_try_register(struct glink_pkt_device *gpdev)
{
	int ret = 0;

retry:
	mutex_lock(&gpdev->drv_lock);
	if (gpdev->removing) {
		ret = -ENODEV;
		goto out;
	}
	if (gpdev->unregistering) {
		mutex_unlock(&gpdev->drv_lock);
		ret = wait_for_completion_interruptible(&gpdev->drv_idle);
		if (ret)
			return ret;
		goto retry;
	}
	if (!gpdev->drv_registered) {
		ret = register_rpmsg_driver(&gpdev->drv);
		if (!ret)
			gpdev->drv_registered = true;
	}
out:
	mutex_unlock(&gpdev->drv_lock);
	return ret;
}

static bool glink_pkt_begin_unregister(struct glink_pkt_device *gpdev)
{
	/* Caller holds drv_lock. Opens wait for drv_idle before re-registering. */
	if (!gpdev->drv_registered || gpdev->unregistering)
		return false;
	gpdev->drv_registered = false;
	gpdev->unregistering = true;
	reinit_completion(&gpdev->drv_idle);
	return true;
}

static void glink_pkt_finish_unregister(struct glink_pkt_device *gpdev)
{
	/* No drv_lock or MM lock: remove must revoke mappings in other tasks. */
	unregister_rpmsg_driver(&gpdev->drv);
	mutex_lock(&gpdev->drv_lock);
	gpdev->unregistering = false;
	complete_all(&gpdev->drv_idle);
	mutex_unlock(&gpdev->drv_lock);
}

static void glink_pkt_drv_try_unregister(struct glink_pkt_device *gpdev)
{
	bool unregister;

	mutex_lock(&gpdev->drv_lock);
	unregister = glink_pkt_begin_unregister(gpdev);
	mutex_unlock(&gpdev->drv_lock);
	if (unregister)
		glink_pkt_finish_unregister(gpdev);
}

static void glink_pkt_idle_work(struct work_struct *work)
{
	struct glink_pkt_device *gpdev = container_of(work, struct glink_pkt_device, idle_work);
	bool unregister = false;
	unsigned long flags;

	mutex_lock(&gpdev->drv_lock);
	if (!gpdev->removing && refcount_read(&gpdev->refcount) == 1) {
		/* An admitted open cannot arrive between the idle check and transition. */
		mutex_lock(&gpdev->lock);
		if (gpdev->rpdev)
			glink_pkt_clear_queues(gpdev);
		spin_lock_irqsave(&gpdev->queue_lock, flags);
		gpdev->sig_change = false;
		spin_unlock_irqrestore(&gpdev->queue_lock, flags);
		mutex_unlock(&gpdev->lock);
		if (gpdev->enable_ch_close)
			unregister = glink_pkt_begin_unregister(gpdev);
	}
	mutex_unlock(&gpdev->drv_lock);
	if (unregister)
		glink_pkt_finish_unregister(gpdev);
	put_device(&gpdev->dev);
}

static void glink_pkt_queue_idle(struct glink_pkt_device *gpdev)
{
	/* Caller holds drv_lock, including open admission and platform removal. */
	if (gpdev->removing || refcount_read(&gpdev->refcount) != 1)
		return;
	get_device(&gpdev->dev);
	if (!schedule_work(&gpdev->idle_work))
		put_device(&gpdev->dev);
}

static void glink_pkt_cancel_idle(struct glink_pkt_device *gpdev)
{
	/* A cancelled queued instance cannot release its own device reference. */
	if (cancel_work_sync(&gpdev->idle_work))
		put_device(&gpdev->dev);
}

/**
 * glink_pkt_open() - open() syscall for the glink_pkt device
 * inode:	Pointer to the inode structure.
 * file:	Pointer to the file structure.
 *
 * This function is used to open the glink pkt device when
 * userspace client do a open() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static int glink_pkt_open(struct inode *inode, struct file *file)
{
	struct glink_pkt_device *gpdev = cdev_to_gpdev(inode->i_cdev);
	unsigned long tout;
	int ret;

	mutex_lock(&gpdev->drv_lock);
	if (gpdev->removing) {
		mutex_unlock(&gpdev->drv_lock);
		return -ENODEV;
	}
	get_device(&gpdev->dev);
	refcount_inc(&gpdev->refcount);
	mutex_unlock(&gpdev->drv_lock);
	tout = msecs_to_jiffies((unsigned int)READ_ONCE(gpdev->open_tout) * 1000);
	if (gpdev->enable_ch_close) {
		ret = glink_pkt_drv_try_register(gpdev);
		if (ret)
			goto fail;
	}
	ret = wait_for_completion_interruptible_timeout(&gpdev->ch_open, tout);
	if (READ_ONCE(gpdev->removing))
		ret = -ENODEV;
	else if (!ret)
		ret = -ETIMEDOUT;
	if (ret < 0)
		goto fail;
	file->private_data = gpdev;
	return 0;
fail:
	mutex_lock(&gpdev->drv_lock);
	refcount_dec(&gpdev->refcount);
	glink_pkt_queue_idle(gpdev);
	mutex_unlock(&gpdev->drv_lock);
	put_device(&gpdev->dev);
	return ret;
}

/**
 * glink_pkt_release() - release operation on glink_pkt device
 * inode:	Pointer to the inode structure.
 * file:	Pointer to the file structure.
 *
 * This function is used to release the glink pkt device when
 * userspace client do a close() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static int glink_pkt_release(struct inode *inode, struct file *file)
{
	struct glink_pkt_device *gpdev = file->private_data;

	mutex_lock(&gpdev->drv_lock);
	refcount_dec(&gpdev->refcount);
	glink_pkt_queue_idle(gpdev);
	mutex_unlock(&gpdev->drv_lock);
	wake_up_interruptible(&gpdev->readq);
	put_device(&gpdev->dev);
	return 0;
}

/**
 * glink_pkt_read() - read() syscall for the glink_pkt device
 * file:	Pointer to the file structure.
 * buf:		Pointer to the userspace buffer.
 * count:	Number bytes to read from the file.
 * ppos:	Pointer to the position into the file.
 *
 * This function is used to Read the data from glink pkt device when
 * userspace client do a read() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static ssize_t glink_pkt_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct glink_pkt_device *gpdev = file->private_data;
	struct sk_buff *skb;
	unsigned char *copy;
	u64 generation;
	size_t used;
	ssize_t ret;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&gpdev->rskb_read_lock))
		return -ERESTARTSYS;
retry:
	if (mutex_lock_interruptible(&gpdev->lock)) {
		ret = -ERESTARTSYS;
		goto read_unlock;
	}
	if (!completion_done(&gpdev->ch_open) || gpdev->disconnecting || !gpdev->rpdev || gpdev->removing) {
		ret = -ENETRESET;
		goto unlock;
	}
	spin_lock_irq(&gpdev->queue_lock);
	if (!gpdev->rskb)
		gpdev->rskb = skb_dequeue(&gpdev->queue);
	spin_unlock_irq(&gpdev->queue_lock);
	if (!gpdev->rskb) {
		mutex_unlock(&gpdev->lock);
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto read_unlock;
		}
		spin_lock_irq(&gpdev->queue_lock);
		ret = wait_event_interruptible_lock_irq(gpdev->readq,
			!skb_queue_empty(&gpdev->queue) || !completion_done(&gpdev->ch_open) ||
			READ_ONCE(gpdev->removing), gpdev->queue_lock);
		spin_unlock_irq(&gpdev->queue_lock);
		if (ret)
			goto read_unlock;
		goto retry;
	}
	skb = gpdev->rskb;
	if (!gpdev->rdata) {
		gpdev->rdata = glink_pkt_skb_data(skb);
		gpdev->rdata_len = skb->len;
	}
	used = min_t(size_t, count, gpdev->rdata_len);
	generation = gpdev->read_generation;
	copy = kmemdup(gpdev->rdata, used, GFP_KERNEL);
	if (!copy) {
		ret = -ENOMEM;
		goto unlock;
	}
	/* Faulting user access must not invert MM -> gpdev in VMA .close. */
	mutex_unlock(&gpdev->lock);
	ret = copy_to_user(buf, copy, used) ? -EFAULT : used;
	kfree(copy);
	mutex_lock(&gpdev->lock);
	if (gpdev->read_generation != generation || gpdev->rskb != skb) {
		if (ret >= 0)
			ret = -ENETRESET;
		goto unlock;
	}
	if (ret < 0)
		goto unlock;
	gpdev->rdata += used;
	gpdev->rdata_len -= used;
	if (!gpdev->fragmented_read || !gpdev->rdata_len) {
		gpdev->rskb = NULL;
		gpdev->rdata = NULL;
		gpdev->rdata_len = 0;
		glink_pkt_kfree_skb(gpdev, skb);
	}
unlock:
	mutex_unlock(&gpdev->lock);
read_unlock:
	mutex_unlock(&gpdev->rskb_read_lock);
	return ret;
}

/**
 * glink_pkt_write() - write() syscall for the glink_pkt device
 * file:	Pointer to the file structure.
 * buf:		Pointer to the userspace buffer.
 * count:	Number bytes to read from the file.
 * ppos:	Pointer to the position into the file.
 *
 * This function is used to write the data to glink pkt device when
 * userspace client do a write() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static ssize_t glink_pkt_write(struct file *file,
			const char __user *buf, size_t count, loff_t *ppos)
{
	struct glink_pkt_device *gpdev = file->private_data;
	void *kbuf;
	int ret;

	gpdev = file->private_data;
	if (!gpdev || refcount_read(&gpdev->refcount) == 1) {
		GLINK_PKT_ERR("invalid device handle\n");
		return -EINVAL;
	}

	if (count > INT_MAX)
		return -EMSGSIZE;

	GLINK_PKT_INFO("begin to %s buffer_size %zu\n", gpdev->ch_name, count);
	kbuf = vmemdup_user(buf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	if (mutex_lock_interruptible(&gpdev->lock)) {
		ret = -ERESTARTSYS;
		goto free_kbuf;
	}
	if (!completion_done(&gpdev->ch_open) || !gpdev->rpdev || gpdev->disconnecting || gpdev->removing) {
		GLINK_PKT_INFO("%s channel in reset\n", gpdev->ch_name);
		ret = -ENETRESET;
		goto unlock_ch;
	}

	if (file->f_flags & O_NONBLOCK)
		ret = rpmsg_trysend(gpdev->rpdev->ept, kbuf, count);
	else
		ret = rpmsg_send(gpdev->rpdev->ept, kbuf, count);

unlock_ch:
	mutex_unlock(&gpdev->lock);

free_kbuf:
	kvfree(kbuf);
	GLINK_PKT_INFO("finish to %s ret %d\n", gpdev->ch_name, ret);
	return ret < 0 ? ret : count;
}

/**
 * glink_pkt_poll() - poll() syscall for the glink_pkt device
 * file:	Pointer to the file structure.
 * wait:	pointer to Poll table.
 *
 * This function is used to poll on the glink pkt device when
 * userspace client do a poll() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static __poll_t glink_pkt_poll(struct file *file, poll_table *wait)
{
	struct glink_pkt_device *gpdev = file->private_data;
	__poll_t mask = 0;
	unsigned long flags;

	gpdev = file->private_data;
	if (!gpdev || refcount_read(&gpdev->refcount) == 1) {
		GLINK_PKT_ERR("invalid device handle\n");
		return POLLERR;
	}
	if (!completion_done(&gpdev->ch_open)) {
		GLINK_PKT_INFO("%s channel in reset\n", gpdev->ch_name);
		return POLLHUP | POLLPRI;
	}

	GLINK_PKT_INFO("Wait for pkt on channel:%s\n", gpdev->ch_name);
	poll_wait(file, &gpdev->readq, wait);

	mutex_lock(&gpdev->lock);

	if (!completion_done(&gpdev->ch_open) || !gpdev->rpdev || gpdev->disconnecting || gpdev->removing) {
		GLINK_PKT_INFO("%s channel reset after wait\n", gpdev->ch_name);
		mutex_unlock(&gpdev->lock);
		return POLLHUP;
	}

	spin_lock_irqsave(&gpdev->queue_lock, flags);
	if (!skb_queue_empty(&gpdev->queue) || gpdev->rskb)
		mask |= POLLIN | POLLRDNORM;

	if (gpdev->sig_change)
		mask |= POLLPRI;
	spin_unlock_irqrestore(&gpdev->queue_lock, flags);

	mask |= rpmsg_poll(gpdev->rpdev->ept, file, wait);

	mutex_unlock(&gpdev->lock);

	GLINK_PKT_INFO("Exit channel:%s\n", gpdev->ch_name);

	return mask;
}

/**
 * glink_pkt_tiocmset() - set the signals for glink_pkt device
 * devp:	Pointer to the glink_pkt device structure.
 * cmd:		IOCTL command.
 * arg:		Arguments to the ioctl call.
 *
 * This function is used to set the signals on the glink pkt device
 * when userspace client do a ioctl() system call with TIOCMBIS,
 * TIOCMBIC and TICOMSET.
 */
static int glink_pkt_tiocmset(struct glink_pkt_device *gpdev, unsigned int cmd, u32 val)
{
	u32 set = 0, clear = 0;

	if (!strcmp(gpdev->edge, "slate"))
		return -ENOIOCTLCMD;
	switch (cmd) {
	case TIOCMBIS:
		set = val;
		break;
	case TIOCMBIC:
		clear = val;
		break;
	case TIOCMSET:
		set = val;
		clear = ~val;
		break;
	}
	set &= TIOCM_DTR | TIOCM_RTS | TIOCM_CD | TIOCM_RI;
	clear &= TIOCM_DTR | TIOCM_RTS | TIOCM_CD | TIOCM_RI;
	return qcom_glink_set_signals(gpdev->rpdev->ept, set, clear);
}


static bool glink_pkt_mapping_vma(struct glink_pkt_device *gpdev, struct vm_area_struct *vma)
{
	return vma && vma->vm_ops == &glink_pkt_vm_ops && vma->vm_private_data == gpdev;
}

/* Caller holds the MM lock. Partial unmaps/splits must never zap unrelated VMAs. */
static void glink_pkt_zap_mapping(struct glink_pkt_device *gpdev,
		struct sk_buff *skb)
{
	struct glink_pkt_zerocopy_cb *cb = (void *)skb->cb;
	unsigned long address = cb->address, end = address + glink_pkt_mapped_length(skb);
	struct vm_area_struct *vma;

	while (address < end) {
		vma = find_vma(cb->mm, address);
		if (!vma || vma->vm_start >= end)
			break;
		address = max(address, vma->vm_start);
		if (glink_pkt_mapping_vma(gpdev, vma))
			zap_page_range_single(vma, address, min(end, vma->vm_end) - address, NULL);
		address = vma->vm_end;
	}
}

static void glink_pkt_revoke_pending(struct glink_pkt_device *gpdev)
{
	struct sk_buff *skb;
	struct glink_pkt_zerocopy_cb *cb;

	for (;;) {
		mutex_lock(&gpdev->lock);
		skb = skb_dequeue(&gpdev->pending);
		mutex_unlock(&gpdev->lock);
		if (!skb)
			break;
		cb = (void *)skb->cb;
		mmap_write_lock(cb->mm);
		glink_pkt_zap_mapping(gpdev, skb);
		mmap_write_unlock(cb->mm);
		mutex_lock(&gpdev->lock);
		glink_pkt_kfree_skb(gpdev, skb);
		mutex_unlock(&gpdev->lock);
	}
}

static void glink_pkt_vma_close(struct vm_area_struct *vma)
{
	struct glink_pkt_device *gpdev = vma->vm_private_data;
	struct sk_buff *skb, *next;
	struct glink_pkt_zerocopy_cb *cb;

	/* MM lock is already held; this ordering also covers file release at munmap. */
	mutex_lock(&gpdev->lock);
	skb_queue_walk_safe(&gpdev->pending, skb, next) {
		cb = (void *)skb->cb;
		if (cb->mm != vma->vm_mm || cb->address >= vma->vm_end ||
		    cb->address + glink_pkt_mapped_length(skb) <= vma->vm_start)
			continue;
		skb_unlink(skb, &gpdev->pending);
		glink_pkt_zap_mapping(gpdev, skb);
		glink_pkt_kfree_skb(gpdev, skb);
	}
	mutex_unlock(&gpdev->lock);
}

static int glink_pkt_vma_mremap(struct vm_area_struct *vma)
{
	/* IOCTL addresses name fixed native mappings, never relocatable aliases. */
	return -EINVAL;
}

static const struct vm_operations_struct glink_pkt_vm_ops = {
	.close = glink_pkt_vma_close,
	.mremap = glink_pkt_vma_mremap,
};

static int glink_pkt_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vma->vm_flags & (VM_WRITE | VM_EXEC))
		return -EPERM;
	vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_private_data = file->private_data;
	vma->vm_ops = &glink_pkt_vm_ops;
	return 0;
}

static int glink_pkt_zerocopy_done(struct glink_pkt_device *gpdev,
		struct glink_pkt_zerocopy_receive *zc)
{
	unsigned long address = zc->address;
	struct sk_buff *skb, *selected = NULL;
	struct glink_pkt_zerocopy_cb *cb;
	int ret = -EINVAL;

	if (!PAGE_ALIGNED(address) || address != zc->address)
		return -EINVAL;
	mmap_read_lock(current->mm);
	mutex_lock(&gpdev->lock);
	if (!gpdev->rpdev || gpdev->disconnecting || gpdev->removing) {
		ret = -ENETRESET;
		goto out;
	}
	if (!gpdev->rx_done)
		goto out;
	skb_queue_walk(&gpdev->pending, skb) {
		cb = (void *)skb->cb;
		if (cb->mm == current->mm && cb->address == address) {
			selected = skb;
			break;
		}
	}
	if (!selected)
		goto out;
	skb_unlink(selected, &gpdev->pending);
	glink_pkt_zap_mapping(gpdev, selected);
	glink_pkt_kfree_skb(gpdev, selected);
	ret = 0;
out:
	mutex_unlock(&gpdev->lock);
	mmap_read_unlock(current->mm);
	return ret;
}

static struct page *glink_pkt_vaddr_to_page(void *cpu_addr)
{
	return is_vmalloc_addr(cpu_addr) ? vmalloc_to_page(cpu_addr) : virt_to_page(cpu_addr);
}

/* Native packet extent, including byte offset in the first read-only page. */
static int glink_pkt_mapping_extent(unsigned long data, u32 length,
		unsigned long *offset, unsigned long *mapped)
{
	if (!length)
		return -EINVAL;
	*offset = offset_in_page(data);
	if (length > ULONG_MAX - *offset - (PAGE_SIZE - 1))
		return -EOVERFLOW;
	*mapped = PAGE_ALIGN(*offset + length);
	return 0;
}

static int glink_pkt_zerocopy_receive(struct glink_pkt_device *gpdev,
		struct glink_pkt_zerocopy_receive *zc, u64 *cookie)
{
	unsigned long address = zc->address, offset, mapped, cursor;
	unsigned long leading = 0, trailing = 0, consumed = 0;
	struct glink_pkt_zerocopy_cb *cb;
	struct vm_area_struct *vma;
	struct sk_buff *skb;
	unsigned char *data;
	int ret = -EINVAL;

	if (!PAGE_ALIGNED(address) || address != zc->address)
		return -EINVAL;
	*cookie = 0;
	zc->length = zc->offset = 0;
	mmap_read_lock(current->mm);
	mutex_lock(&gpdev->lock);
	if (!gpdev->rpdev || gpdev->disconnecting || gpdev->removing) {
		ret = -ENETRESET;
		goto out;
	}
	if (!gpdev->rx_done)
		goto out;
	vma = vma_lookup(current->mm, address);
	if (!glink_pkt_mapping_vma(gpdev, vma))
		goto out;
	spin_lock_irq(&gpdev->queue_lock);
	skb = skb_peek(&gpdev->queue);
	spin_unlock_irq(&gpdev->queue_lock);
	if (!skb) {
		ret = -EAGAIN;
		goto out;
	}
	data = glink_pkt_skb_data(skb);
	ret = glink_pkt_mapping_extent((unsigned long)data, skb->len, &offset, &mapped);
	if (ret)
		goto out;
	if (mapped > vma->vm_end - address) {
		ret = -ENOSPC;
		goto out;
	}
	skb_queue_walk(&gpdev->pending, skb) {
		cb = (void *)skb->cb;
		if (cb->mm == current->mm && address < cb->address + glink_pkt_mapped_length(skb) &&
		    cb->address < address + mapped) {
			ret = -EBUSY;
			goto out;
		}
	}
	if (gpdev->mapping_cookie == U64_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	skb = skb_dequeue(&gpdev->queue);
	data = glink_pkt_skb_data(skb);
	zap_page_range_single(vma, address, mapped, NULL);
	for (cursor = 0; cursor < mapped; cursor += PAGE_SIZE) {
		unsigned long skip = cursor ? 0 : offset;
		unsigned long used = min_t(unsigned long, skb->len - consumed, PAGE_SIZE - skip);
		unsigned long boundary = 0;
		struct page *page;

		/* Copy only partial boundary pages; full native pages remain zero-copy. */
		if (skip || used < PAGE_SIZE) {
			boundary = get_zeroed_page(GFP_KERNEL);
			if (!boundary) {
				ret = -ENOMEM;
				goto rollback;
			}
			memcpy((void *)(boundary + skip), data + consumed, used);
			if (!cursor)
				leading = boundary;
			else
				trailing = boundary;
			page = virt_to_page(boundary);
		} else {
			page = glink_pkt_vaddr_to_page(data + consumed);
		}
		{
			vm_fault_t fault = vmf_insert_pfn(vma, address + cursor, page_to_pfn(page));

			if (fault & VM_FAULT_ERROR) {
				ret = fault & VM_FAULT_OOM ? -ENOMEM : -EFAULT;
				goto rollback;
			}
		}
		consumed += used;
	}
	cb = (void *)skb->cb;
	cb->mm = current->mm;
	mmgrab(cb->mm);
	cb->address = address;
	cb->cookie = ++gpdev->mapping_cookie;
	*cookie = cb->cookie;
	cb->leading_page = leading;
	cb->trailing_page = trailing;
	skb_queue_tail(&gpdev->pending, skb);
	zc->offset = offset;
	zc->length = skb->len;
	ret = 0;
	goto out;
rollback:
	zap_page_range_single(vma, address, mapped, NULL);
	if (leading)
		free_page(leading);
	if (trailing)
		free_page(trailing);
	skb_queue_head(&gpdev->queue, skb);
	wake_up_interruptible(&gpdev->readq);
out:
	mutex_unlock(&gpdev->lock);
	mmap_read_unlock(current->mm);
	return ret;
}


static void glink_pkt_zerocopy_rollback(struct glink_pkt_device *gpdev,
		struct glink_pkt_zerocopy_receive *zc, u64 cookie)
{
	struct sk_buff *skb, *selected = NULL;
	struct glink_pkt_zerocopy_cb *cb;
	unsigned long flags;
	void *external;

	if (!cookie)
		return;
	mmap_read_lock(current->mm);
	mutex_lock(&gpdev->lock);
	skb_queue_walk(&gpdev->pending, skb) {
		cb = (void *)skb->cb;
		if (cb->mm == current->mm && cb->address == zc->address && cb->cookie == cookie) {
			selected = skb;
			break;
		}
	}
	if (!selected)
		goto out;
	cb = (void *)selected->cb;
	skb_unlink(selected, &gpdev->pending);
	glink_pkt_zap_mapping(gpdev, selected);
	if (cb->leading_page)
		free_page(cb->leading_page);
	if (cb->trailing_page)
		free_page(cb->trailing_page);
	mmdrop(cb->mm);
	external = cb->external;
	memset(selected->cb, 0, sizeof(selected->cb));
	((struct glink_pkt_zerocopy_cb *)selected->cb)->external = external;
	/* Output faults do not consume or acknowledge a genuine firmware packet. */
	spin_lock_irqsave(&gpdev->queue_lock, flags);
	skb_queue_head(&gpdev->queue, selected);
	spin_unlock_irqrestore(&gpdev->queue_lock, flags);
	wake_up_interruptible(&gpdev->readq);
out:
	mutex_unlock(&gpdev->lock);
	mmap_read_unlock(current->mm);
}

/**
 * glink_pkt_ioctl() - ioctl() syscall for the glink_pkt device
 * file:	Pointer to the file structure.
 * cmd:		IOCTL command.
 * arg:		Arguments to the ioctl call.
 *
 * This function is used to ioctl on the glink pkt device when
 * userspace client do a ioctl() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static long glink_pkt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct glink_pkt_device *gpdev = file->private_data;
	struct glink_pkt_zerocopy_receive zc;
	u64 cookie = 0;
	u32 val = 0;
	unsigned long flags;
	int ret;

	if (!gpdev)
		return -EINVAL;
	if (cmd == GLINK_PKT_IOCTL_ZC_RECV || cmd == GLINK_PKT_IOCTL_ZC_DONE) {
		if (copy_from_user(&zc, (void __user *)arg, sizeof(zc)))
			return -EFAULT;
		if (cmd == GLINK_PKT_IOCTL_ZC_DONE)
			return glink_pkt_zerocopy_done(gpdev, &zc);
		ret = glink_pkt_zerocopy_receive(gpdev, &zc, &cookie);
		if (copy_to_user((void __user *)arg, &zc, sizeof(zc))) {
			glink_pkt_zerocopy_rollback(gpdev, &zc, cookie);
			return -EFAULT;
		}
		return ret;
	}
	if (cmd == TIOCMSET || cmd == TIOCMBIS || cmd == TIOCMBIC) {
		if (get_user(val, (u32 __user *)arg))
			return -EFAULT;
	}
	if (mutex_lock_interruptible(&gpdev->lock))
		return -ERESTARTSYS;
	if (!completion_done(&gpdev->ch_open) || !gpdev->rpdev || gpdev->disconnecting || gpdev->removing) {
		ret = -ENETRESET;
		goto out;
	}
	switch (cmd) {
	case TIOCMGET:
		if (!strcmp(gpdev->edge, "slate")) {
			ret = -ENOIOCTLCMD;
			break;
		}
		spin_lock_irqsave(&gpdev->queue_lock, flags);
		gpdev->sig_change = false;
		spin_unlock_irqrestore(&gpdev->queue_lock, flags);
		ret = qcom_glink_get_signals(gpdev->rpdev->ept);
		break;
	case TIOCMSET:
	case TIOCMBIS:
	case TIOCMBIC:
		ret = glink_pkt_tiocmset(gpdev, cmd, val);
		break;
	case GLINK_PKT_IOCTL_QUEUE_RX_INTENT:
		/* Native compatibility ABI; GLINK allocates/advertises intents itself. */
		ret = 0;
		break;
	default:
		ret = -ENOIOCTLCMD;
	}
out:
	mutex_unlock(&gpdev->lock);
	if (cmd == TIOCMGET && ret >= 0)
		return put_user(ret, (int __user *)arg);
	return ret;
}

static const struct file_operations glink_pkt_fops = {
	.owner = THIS_MODULE,
	.open = glink_pkt_open,
	.release = glink_pkt_release,
	.read = glink_pkt_read,
	.write = glink_pkt_write,
	.poll = glink_pkt_poll,
	.unlocked_ioctl = glink_pkt_ioctl,
	.mmap = glink_pkt_mmap,
	.compat_ioctl = glink_pkt_ioctl,
};

static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct glink_pkt_device *gpdev = dev_to_gpdev(dev);

	return scnprintf(buf, RPMSG_NAME_SIZE, "%s\n", gpdev->ch_name);
}
static DEVICE_ATTR_RO(name);

static struct attribute *glink_pkt_device_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(glink_pkt_device);

/**
 * parse_glinkpkt_devicetree() - parse device tree binding for a subnode
 *
 * np:		pointer to a device tree node
 * gpdev:	pointer to GLINK PACKET device
 *
 * Return:	0 on success, standard Linux error codes on error.
 */
static int glink_pkt_parse_devicetree(struct device_node *np,
				      struct glink_pkt_device *gpdev)
{
	char *key;
	int ret;

	key = "qcom,glinkpkt-edge";
	ret = of_property_read_string(np, key, &gpdev->edge);
	if (ret < 0)
		goto error;

	key = "qcom,glinkpkt-ch-name";
	ret = of_property_read_string(np, key, &gpdev->ch_name);
	if (ret < 0)
		goto error;

	key = "qcom,glinkpkt-dev-name";
	ret = of_property_read_string(np, key, &gpdev->dev_name);
	if (ret < 0)
		goto error;

	key = "qcom,glinkpkt-enable-ch-close";
	gpdev->enable_ch_close = of_property_read_bool(np, key);

	key = "qcom,glinkpkt-fragmented-read";
	gpdev->fragmented_read = of_property_read_bool(np, key);

	GLINK_PKT_INFO(
	"Parsed %s:%s /dev/%s enable channel close:%d fragmented-read:%d\n",
		      gpdev->edge, gpdev->ch_name, gpdev->dev_name,
		      gpdev->enable_ch_close, gpdev->fragmented_read);

	return 0;

error:
	GLINK_PKT_ERR("%s: missing key: %s\n", __func__, key);
	return ret;
}

static void glink_pkt_release_device(struct device *dev)
{
	struct glink_pkt_device *gpdev = dev_to_gpdev(dev);

	of_node_put(dev->of_node);
	ida_free(&glink_pkt_minor_ida, MINOR(dev->devt));
	kfree(gpdev);
}

static int glink_pkt_init_rpmsg(struct glink_pkt_device *gpdev)
{
	struct rpmsg_driver *rpdrv = &gpdev->drv;
	struct device *dev = &gpdev->dev;
	struct rpmsg_device_id *match;
	char *drv_name;

	match = devm_kzalloc(dev, sizeof(*match) * 2, GFP_KERNEL);
	if (!match)
		return -ENOMEM;
	if (strscpy(match->name, gpdev->ch_name, RPMSG_NAME_SIZE) < 0)
		return -ENAMETOOLONG;

	drv_name = devm_kasprintf(dev, GFP_KERNEL,
				   "%s_%s", "glink_pkt", gpdev->dev_name);
	if (!drv_name)
		return -ENOMEM;

	rpdrv->probe = glink_pkt_rpdev_probe;
	rpdrv->remove = glink_pkt_rpdev_remove;
	rpdrv->callback = glink_pkt_rpdev_cb;
	rpdrv->id_table = match;
	rpdrv->drv.name = drv_name;

	return glink_pkt_drv_try_register(gpdev);
}

/**
 * glink_pkt_add_device() - Create glink packet device and add cdev
 * parent:	pointer to the parent device of this glink packet device
 * np:		pointer to device node this glink packet device represents
 *
 * return:	0 for success, Standard Linux errors
 */
static int glink_pkt_create_device(struct device *parent, struct device_node *np,
		struct glink_pkt_owner *owner)
{
	struct glink_pkt_device *gpdev;
	struct device *dev;
	int ret, minor;

	gpdev = kzalloc(sizeof(*gpdev), GFP_KERNEL);
	if (!gpdev)
		return -ENOMEM;
	minor = ida_alloc_range(&glink_pkt_minor_ida, 0, GLINK_PKT_MAX_DEVS - 1, GFP_KERNEL);
	if (minor < 0) {
		kfree(gpdev);
		return minor;
	}
	dev = &gpdev->dev;
	device_initialize(dev);
	dev->devt = MKDEV(MAJOR(glink_pkt_major), minor);
	dev->release = glink_pkt_release_device;
	dev->of_node = of_node_get(np);
	ret = glink_pkt_parse_devicetree(np, gpdev);
	if (ret)
		goto put_dev;
	if (!*gpdev->ch_name || strlen(gpdev->ch_name) >= RPMSG_NAME_SIZE ||
	    !*gpdev->dev_name || strchr(gpdev->dev_name, '/') || !*gpdev->edge) {
		ret = -EINVAL;
		goto put_dev;
	}
	mutex_init(&gpdev->lock);
	mutex_init(&gpdev->drv_lock);
	mutex_init(&gpdev->rskb_read_lock);
	refcount_set(&gpdev->refcount, 1);
	init_completion(&gpdev->ch_open);
	init_completion(&gpdev->drv_idle);
	INIT_WORK(&gpdev->idle_work, glink_pkt_idle_work);
	complete_all(&gpdev->drv_idle);
	gpdev->open_tout = 120;
	gpdev->disconnecting = true;
	spin_lock_init(&gpdev->queue_lock);
	skb_queue_head_init(&gpdev->queue);
	skb_queue_head_init(&gpdev->pending);
	init_waitqueue_head(&gpdev->readq);
	dev->class = glink_pkt_class;
	dev->parent = parent;
	dev->groups = glink_pkt_device_groups;
	dev_set_drvdata(dev, gpdev);
	ret = dev_set_name(dev, "%s", gpdev->dev_name);
	if (ret)
		goto put_dev;
	cdev_init(&gpdev->cdev, &glink_pkt_fops);
	gpdev->cdev.owner = THIS_MODULE;
	ret = glink_pkt_init_rpmsg(gpdev);
	if (ret)
		goto put_dev;
	ret = cdev_device_add(&gpdev->cdev, dev);
	if (ret)
		goto unregister;
	ret = device_create_file(dev, &dev_attr_open_timeout);
	if (ret) {
		cdev_device_del(&gpdev->cdev, dev);
		goto unregister;
	}
	list_add_tail(&gpdev->owner_node, &owner->devices);
	return 0;
unregister:
	mutex_lock(&gpdev->drv_lock);
	WRITE_ONCE(gpdev->removing, true);
	mutex_unlock(&gpdev->drv_lock);
	glink_pkt_cancel_idle(gpdev);
	glink_pkt_drv_try_unregister(gpdev);
	wait_for_completion(&gpdev->drv_idle);
	complete_all(&gpdev->ch_open);
	wake_up_interruptible(&gpdev->readq);
put_dev:
	put_device(dev);
	return ret;
}

/**
 * glink_pkt_deinit() - De-initialize this module
 *
 * This function frees all the memory and unregisters the char device region.
 */
static void glink_pkt_deinit(void)
{
	class_destroy(glink_pkt_class);
	unregister_chrdev_region(glink_pkt_major, GLINK_PKT_MAX_DEVS);
	ida_destroy(&glink_pkt_minor_ida);
	ipc_log_context_destroy(glink_pkt_ilctxt);
}

/**
 * glink_pkt_probe() - Probe a GLINK packet device
 *
 * pdev:	Pointer to platform device.
 *
 * return:	0 on success, standard Linux error codes on error.
 *
 * This function is called when the underlying device tree driver registers
 * a platform device, mapped to a G-Link packet device.
 */
static void glink_pkt_remove(struct platform_device *pdev)
{
	struct glink_pkt_owner *owner = platform_get_drvdata(pdev);
	struct glink_pkt_device *gpdev, *next;

	list_for_each_entry_safe(gpdev, next, &owner->devices, owner_node) {
		list_del(&gpdev->owner_node);
		mutex_lock(&gpdev->drv_lock);
		WRITE_ONCE(gpdev->removing, true);
		mutex_unlock(&gpdev->drv_lock);
		cdev_device_del(&gpdev->cdev, &gpdev->dev);
		glink_pkt_cancel_idle(gpdev);
		glink_pkt_drv_try_unregister(gpdev);
		wait_for_completion(&gpdev->drv_idle);
		complete_all(&gpdev->ch_open);
		wake_up_interruptible(&gpdev->readq);
		put_device(&gpdev->dev);
	}
}

static int glink_pkt_probe(struct platform_device *pdev)
{
	struct glink_pkt_owner *owner;
	struct device_node *child;
	int ret;

	owner = devm_kzalloc(&pdev->dev, sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return -ENOMEM;
	INIT_LIST_HEAD(&owner->devices);
	platform_set_drvdata(pdev, owner);
	for_each_available_child_of_node(pdev->dev.of_node, child) {
		ret = glink_pkt_create_device(&pdev->dev, child, owner);
		if (ret) {
			of_node_put(child);
			glink_pkt_remove(pdev);
			return ret;
		}
	}
	return 0;
}

static const struct of_device_id glink_pkt_match_table[] = {
	{ .compatible = "qcom,glinkpkt" },
	{},
};

static struct platform_driver glink_pkt_driver = {
	.probe = glink_pkt_probe,
	.remove = glink_pkt_remove,
	.driver = {
		.name = MODULE_NAME,
		.of_match_table = glink_pkt_match_table,
		.suppress_bind_attrs = true,
	 },
};

/**
 * glink_pkt_init() - Initialization function for this module
 *
 * returns:	0 on success, standard Linux error code otherwise.
 */
static int __init glink_pkt_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct glink_pkt_zerocopy_cb) > sizeof_field(struct sk_buff, cb));
	glink_pkt_ilctxt = ipc_log_context_create(GLINK_PKT_IPC_LOG_PAGE_CNT, "glink_pkt", 0);
	if (!glink_pkt_ilctxt)
		return -ENOMEM;
	ret = alloc_chrdev_region(&glink_pkt_major, 0, GLINK_PKT_MAX_DEVS, "glinkpkt");
	if (ret)
		goto destroy_log;
	glink_pkt_class = class_create("glinkpkt");
	if (IS_ERR(glink_pkt_class)) {
		ret = PTR_ERR(glink_pkt_class);
		goto release_region;
	}
	ret = platform_driver_register(&glink_pkt_driver);
	if (!ret)
		return 0;
	class_destroy(glink_pkt_class);
release_region:
	unregister_chrdev_region(glink_pkt_major, GLINK_PKT_MAX_DEVS);
destroy_log:
	ipc_log_context_destroy(glink_pkt_ilctxt);
	return ret;
}

/**
 * glink_pkt_exit() - Exit function for this module
 *
 * This function is used to cleanup the module during the exit.
 */
static void __exit glink_pkt_exit(void)
{
	platform_driver_unregister(&glink_pkt_driver);
	glink_pkt_deinit();
}

module_init(glink_pkt_init);
module_exit(glink_pkt_exit);

MODULE_DEVICE_TABLE(of, glink_pkt_match_table);
MODULE_DESCRIPTION("MSM G-Link Packet Port");
MODULE_LICENSE("GPL");
