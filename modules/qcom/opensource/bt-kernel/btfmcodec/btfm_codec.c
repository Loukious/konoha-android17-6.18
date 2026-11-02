// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/refcount.h>
#include <linux/idr.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include "btfm_codec_interface.h"
#include "btfm_codec.h"
#include "btfm_codec_pkt.h"
#include "btfm_codec_btadv_interface.h"

#define dev_to_btfmcodec(_dev) container_of(_dev, struct btfmcodec_data, dev)

DEFINE_SRCU(btfmcodec_endpoint_srcu);
static struct class *dev_class;
static dev_t dev_major;
struct btfmcodec_data *btfmcodec;
struct device_driver driver = {.name = "btfmcodec-driver", .owner = THIS_MODULE};
struct btfmcodec_char_device *btfmcodec_dev;
bool is_cp_supported = true;

#define cdev_to_btfmchardev(_cdev) container_of(_cdev, struct btfmcodec_char_device, cdev)
#define MIN_PKT_LEN  0x9

char *coverttostring(enum btfmcodec_states state) {
	switch (state) {
	case IDLE:
		return "IDLE";
		break;
	case BT_Connected:
		return "BT_CONNECTED";
		break;
	case BT_Connecting:
		return "BT_CONNECTING";
		break;
	case BTADV_AUDIO_Connected:
		return "BTADV_AUDIO_CONNECTED";
		break;
	case BTADV_AUDIO_Connecting:
		return "BTADV_AUDIO_CONNECTING";
		break;
	default:
		return "INVALID_STATE";
		break;
	}
}

/*
 * btfmcodec_dev_open() - open() syscall for the btfmcodec dev node
 * inode:	Pointer to the inode structure.
 * file:	Pointer to the file structure.
 *
 * This function is used to open the btfmcodec char device when
 * userspace client do a open() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 * Note: btfmcodec dev node works on nonblocking mode.
 */
static int btfmcodec_dev_open(struct inode *inode, struct file *file)
{
	struct btfmcodec_char_device *btfmcodec_dev = cdev_to_btfmchardev(inode->i_cdev);
	struct btfmcodec_data *btfmcodec = (struct btfmcodec_data *)btfmcodec_dev->btfmcodec;
	unsigned int active_clients;

	mutex_lock(&btfmcodec_dev->lock);
	if (!btfmcodec_dev->ready) {
		mutex_unlock(&btfmcodec_dev->lock);
		return -ENODEV;
	}
	active_clients = refcount_read(&btfmcodec_dev->active_clients);

	BTFMCODEC_INFO("for %s by %s:%d active_clients[%d]\n",
		       btfmcodec_dev->dev_name, current->comm,
		       task_pid_nr(current), refcount_read(&btfmcodec_dev->active_clients));
	/* Don't allow a new client if already one is active. */
	if (active_clients > 1) {
		BTFMCODEC_WARN("%s: Not honoring open as other client is active", __func__);
		mutex_unlock(&btfmcodec_dev->lock);
		return -EACCES;
	}
	/* for now have btfmcodec and later we can think of having it btfmcodec_dev */
	file->private_data = btfmcodec;
	get_device(&btfmcodec->dev);
	WRITE_ONCE(btfmcodec_dev->closing, false);
	refcount_inc(&btfmcodec_dev->active_clients);

	mutex_lock(&btfmcodec->states.state_machine_lock);
	BTFMCODEC_INFO("current_state %s prev_state %s",
			coverttostring(btfmcodec->states.current_state),
			coverttostring(btfmcodec->states.prev_state));
	/* Reset state if they are BTADV_AUDIO_CONNECTED or BTADV_AUDIO_CONNECTED
	 * as these states should be moved to IDLE in previous iteration.
	 */
	if (btfmcodec->states.current_state == BTADV_AUDIO_Connected ||
	    btfmcodec->states.current_state == BTADV_AUDIO_Connecting) {
		btfmcodec->states.current_state = IDLE;
	}

	btfmcodec->states.prev_state = IDLE;
	mutex_unlock(&btfmcodec->states.state_machine_lock);
	mutex_unlock(&btfmcodec_dev->lock);
	return 0;
}

/*
 * btfmcodec_pkt_release() - release operation on btfmcodec device
 * inode:	Pointer to the inode structure.
 * file:	Pointer to the file structure.
 *
 * This function is used to release the btfmcodec dev node when
 * userspace client do a close() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static int btfmcodec_dev_release(struct inode *inode, struct file *file)
{
	struct btfmcodec_data *codec = file->private_data;
	struct btfmcodec_char_device *chardev = codec->btfmcodec_dev;
	unsigned long flags;

	mutex_lock(&chardev->lock);
	/* Finish previously admitted producers before closing the queue. */
	spin_lock_irqsave(&chardev->tx_queue_lock, flags);
	WRITE_ONCE(chardev->closing, true);
	spin_unlock_irqrestore(&chardev->tx_queue_lock, flags);
	mutex_unlock(&chardev->lock);
	btfmcodec_stop_commands(codec);
	btfmcodec_drain_commands(codec);
	mutex_lock(&chardev->read_lock);
	skb_queue_purge(&chardev->txq);
	mutex_unlock(&chardev->read_lock);
	mutex_lock(&chardev->lock);
	refcount_dec(&chardev->active_clients);
	mutex_unlock(&chardev->lock);
	put_device(&codec->dev);
	return 0;
}

static btm_opcode STREAM_TO_UINT32(struct sk_buff *skb)
{
	return get_unaligned_le32(skb->data);
}

static void btfmcodec_dev_rxwork_impl(struct work_struct *work)
{
	struct btfmcodec_char_device *btfmcodec_dev = container_of(work, struct btfmcodec_char_device, rx_work);
	struct sk_buff *skb;
	struct btfmcodec_state_machine *state = &btfmcodec->states;
	uint32_t len;
	u8 status = 0xff;
	int idx;
	uint8_t *bearer_switch_ind, *dma_rsp;

	BTFMCODEC_DBG("start");
	while ((skb = skb_dequeue(&btfmcodec_dev->rxq))) {
		btm_opcode opcode;

		if (skb->len < BTM_HEADER_LEN)
			goto discard;
		opcode = STREAM_TO_UINT32(skb);
		skb_pull(skb, sizeof(btm_opcode));
		len = STREAM_TO_UINT32(skb);
		skb_pull(skb, sizeof(len));
		if (len != skb->len)
			goto discard;
		switch (opcode) {
		case BTM_BTFMCODEC_PREPARE_AUDIO_BEARER_SWITCH_REQ:
			idx = BTM_PKT_TYPE_PREPARE_REQ;
			BTFMCODEC_DBG("BTM_BTFMCODEC_PREPARE_AUDIO_BEARER_SWITCH_REQ");
			if (len == BTM_PREPARE_AUDIO_BEARER_SWITCH_REQ_LEN) {
				/* Reset bearer switch ind flag */
				bearer_switch_ind =
					&btfmcodec_dev->status[BTM_PKT_TYPE_BEARER_SWITCH_IND];
				*bearer_switch_ind = BTM_WAITING_RSP;
				btfmcodec_enqueue_transport(btfmcodec_dev, skb->data[0]);
				if (skb->data[0] == NONE &&
					btfmcodec_get_current_transport(state) == BT_Connecting &&
					btfmcodec_get_prev_transport(state) ==
					BTADV_AUDIO_Connected) {
					BTFMCODEC_INFO("KP might be awaiting for codec dma rsp");
					idx = BTM_PKT_TYPE_DMA_CONFIG_RSP;
					dma_rsp = &btfmcodec_dev->status[idx];
					*dma_rsp = BTM_FAIL_RESP_RECV;
					wake_up_interruptible(&btfmcodec_dev->rsp_wait_q[idx]);
				}
				queue_work(btfmcodec_dev->workqueue,
					&btfmcodec_dev->wq_prepare_bearer);
			} else {
				BTFMCODEC_ERR("wrong packet format with len:%d", len);
			}
			break;
		case BTM_BTFMCODEC_MASTER_CONFIG_RSP:
			idx = BTM_PKT_TYPE_MASTER_CONFIG_RSP;
			if (len == BTM_MASTER_CONFIG_RSP_LEN) {
				status = skb->data[1];
				if (status == MSG_SUCCESS)
				  btfmcodec_dev->status[idx] = BTM_RSP_RECV;
				else
				  btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			} else {
				BTFMCODEC_ERR("wrong packet format with len:%d", len);
				btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			}
			BTFMCODEC_INFO("Rx BTM_BTFMCODEC_MASTER_CONFIG_RSP status:%d",
				status);
			wake_up_interruptible(&btfmcodec_dev->rsp_wait_q[idx]);
			break;
		case BTM_BTFMCODEC_CODEC_CONFIG_DMA_RSP:
			idx = BTM_PKT_TYPE_DMA_CONFIG_RSP;
			if (len == BTM_CODEC_CONFIG_DMA_RSP_LEN) {
				status = skb->data[1];
				if (status == MSG_SUCCESS)
					btfmcodec_dev->status[idx] = BTM_RSP_RECV;
				else
					btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			} else {
				BTFMCODEC_ERR("wrong packet format with len:%d", len);
				btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			}
			BTFMCODEC_INFO("Rx BTM_BTFMCODEC_CODEC_CONFIG_DMA_RSP status:%d",
				status);
			wake_up_interruptible(&btfmcodec_dev->rsp_wait_q[idx]);
			break;
		case BTM_BTFMCODEC_CTRL_MASTER_SHUTDOWN_RSP:
			idx = BTM_PKT_TYPE_MASTER_SHUTDOWN_RSP;
			if (len == BTM_MASTER_CONFIG_RSP_LEN) {
				status = skb->data[1];
				if (status == MSG_SUCCESS)
				  btfmcodec_dev->status[idx] = BTM_RSP_RECV;
				else
				  btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			} else {
				BTFMCODEC_ERR("wrong packet format with len:%d", len);
				btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			}
			BTFMCODEC_INFO("Rx BTM_BTFMCODEC_CTRL_MASTER_SHUTDOWN_RSP status:%d",
				status);
			wake_up_interruptible(&btfmcodec_dev->rsp_wait_q[idx]);
			BTFMCODEC_INFO("%s: waiting to cancel prepare bearer wq", __func__);
			/* The bearer worker may need subsequent RX responses. */
			BTFMCODEC_INFO("%s: prepare bearer wq canceled", __func__);
			break;
		case BTM_BTFMCODEC_BEARER_SWITCH_IND:
			idx = BTM_PKT_TYPE_BEARER_SWITCH_IND;
			if (len == BTM_BEARER_SWITCH_IND_LEN) {
				status = skb->data[0];
				if (status == MSG_SUCCESS)
				  btfmcodec_dev->status[idx] = BTM_RSP_RECV;
				else
				  btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			} else {
				BTFMCODEC_ERR("wrong packet format with len:%d", len);
				btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			}
			BTFMCODEC_INFO("Rx BTM_BTFMCODEC_BEARER_SWITCH_IND status:%d",
				status);
			wake_up_interruptible(&btfmcodec_dev->rsp_wait_q[idx]);
			break;
		case BTM_BTFMCODEC_CTRL_LOG_LVL_IND:
			if (len == BTM_LOG_LVL_IND_LEN) {
				log_lvl = skb->data[0];
			} else {
				BTFMCODEC_ERR("wrong packet format with len:%d", len);
			}
			BTFMCODEC_INFO("Rx BTM_BTFMCODEC_CTRL_LOG_LVL_IND status:%d",
					log_lvl);
			break;
		case BTM_BTFMCODEC_USECASE_START_RSP:
			idx = BTM_PKT_TYPE_USECASE_START_RSP;
			if (len == BTM_USECASE_START_RSP_LEN) {
				status = skb->data[0];
				if (status == MSG_SUCCESS)
					btfmcodec_dev->status[idx] = BTM_RSP_RECV;
				else
					btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			} else {
				BTFMCODEC_ERR("wrong packet format with len:%d", len);
				btfmcodec_dev->status[idx] = BTM_FAIL_RESP_RECV;
			}
			BTFMCODEC_INFO("Rx BTM_BTFMCODEC_USECASE_START_RSP status:%d",
				status);
			wake_up_interruptible(&btfmcodec_dev->rsp_wait_q[idx]);
			break;
		default:
			BTFMCODEC_ERR("wrong opcode:%08x", opcode);
		}
discard:
		kfree_skb(skb);
	}
	BTFMCODEC_DBG("end");
}

static void btfmcodec_dev_rxwork(struct work_struct *work)
{
	int idx = btfmcodec_endpoint_enter();

	if (idx < 0) {
		struct btfmcodec_char_device *chardev = container_of(work,
				struct btfmcodec_char_device, rx_work);
		skb_queue_purge(&chardev->rxq);
		return;
	}
	btfmcodec_dev_rxwork_impl(work);
	btfmcodec_endpoint_exit(idx);
}

/*
 * btfmcodec_pkt_write() - write() syscall for the btfmcodec_pkt device
 * file:	Pointer to the file structure.
 * buf:		Pointer to the userspace buffer.
 * count:	Number bytes to read from the file.
 * ppos:	Pointer to the position into the file.
 *
 * This function is used to write the data to btfmcodec dev node when
 * userspace client do a write() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static ssize_t btfmcodec_dev_write(struct file *file,
			const char __user *buf, size_t count, loff_t *ppos)
{
	struct btfmcodec_data *codec = file->private_data;
	struct btfmcodec_char_device *chardev = codec->btfmcodec_dev;
	struct sk_buff *skb;
	int ret;

	if (!count)
		return 0;
	if (count < MIN_PKT_LEN || count > 4096)
		return -EMSGSIZE;
	if (mutex_lock_interruptible(&chardev->lock))
		return -ERESTARTSYS;
	if (READ_ONCE(chardev->closing)) {
		ret = -ENETRESET;
		goto unlock;
	}
	if (skb_queue_len(&chardev->rxq) >= 64) {
		ret = -ENOSPC;
		goto unlock;
	}
	skb = alloc_skb(count, GFP_KERNEL);
	if (!skb) {
		ret = -ENOMEM;
		goto unlock;
	}
	if (copy_from_user(skb_put(skb, count), buf, count)) {
		ret = -EFAULT;
		goto free_skb;
	}
	if (get_unaligned_le32(skb->data + sizeof(u32)) != count - BTM_HEADER_LEN) {
		ret = -EINVAL;
		goto free_skb;
	}
	skb_queue_tail(&chardev->rxq, skb);
	schedule_work(&chardev->rx_work);
	mutex_unlock(&chardev->lock);
	return count;
free_skb:
	kfree_skb(skb);
unlock:
	mutex_unlock(&chardev->lock);
	return ret;
}

int btfmcodec_dev_enqueue_pkt(struct btfmcodec_char_device *btfmcodec_dev, void *buf, int len)
{
	struct sk_buff *skb;
	unsigned long flags;
	uint8_t *cmd = buf;

	if (!buf || len < 1 || len > 4096)
		return -EINVAL;
	BTFMCODEC_DBG("start");
	spin_lock_irqsave(&btfmcodec_dev->tx_queue_lock, flags);
	if (refcount_read(&btfmcodec_dev->active_clients) == 1 ||
	    READ_ONCE(btfmcodec_dev->closing) || skb_queue_len(&btfmcodec_dev->txq) >= 64) {
		BTFMCODEC_WARN("no active clients discarding the packet");
		spin_unlock_irqrestore(&btfmcodec_dev->tx_queue_lock, flags);
		return -EINVAL;
	}
	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb) {
		BTFMCODEC_ERR("failed to allocate memory");
		spin_unlock_irqrestore(&btfmcodec_dev->tx_queue_lock, flags);
		return -ENOMEM;
	}

	skb_put_data(skb, cmd, len);
	skb_queue_tail(&btfmcodec_dev->txq, skb);
	wake_up_interruptible(&btfmcodec_dev->readq);
	spin_unlock_irqrestore(&btfmcodec_dev->tx_queue_lock, flags);
	BTFMCODEC_DBG("end");
	return 0;
}

int btfmcodec_enqueue_transport(struct btfmcodec_char_device *btfmcodec_dev,
				uint8_t transport)
{
	struct sk_buff *skb;

	if (transport < BT || transport > NONE)
		return -EINVAL;
	mutex_lock(&btfmcodec_dev->trans_lock);
	if (skb_queue_len(&btfmcodec_dev->trans_rxq) >= 64) {
		mutex_unlock(&btfmcodec_dev->trans_lock);
		return -ENOSPC;
	}
	skb = alloc_skb(1, GFP_KERNEL);
	if (!skb) {
		BTFMCODEC_ERR("failed to allocate memory");
		mutex_unlock(&btfmcodec_dev->trans_lock);
		return -ENOMEM;
	}

	skb_put_data(skb, &transport, 1);
	skb_queue_tail(&btfmcodec_dev->trans_rxq, skb);
	mutex_unlock(&btfmcodec_dev->trans_lock);
	wake_up_interruptible(&btfmcodec_dev->rsp_wait_q[BTM_PKT_TYPE_BEARER_SWITCH_IND]);
	return 0;
}

int btfmcodec_dequeue_transport(struct btfmcodec_char_device *btfmcodec_dev)
{
	uint8_t transport = 0xFF;
	struct sk_buff *skb;

	mutex_lock(&btfmcodec_dev->trans_lock);
	skb = skb_dequeue(&btfmcodec_dev->trans_rxq);
	if (!skb) {
		mutex_unlock(&btfmcodec_dev->trans_lock);
		return transport;
	}
	transport = skb->data[0];
	skb_pull(skb, 1);
	kfree_skb(skb);
	mutex_unlock(&btfmcodec_dev->trans_lock);
	return transport;
}

/*
 * btfmcodec_pkt_poll() - poll() syscall for the btfmcodec device
 * file:	Pointer to the file structure.
 * wait:	pointer to Poll table.
 *
 * This function is used to poll on the btfmcodec dev node when
 * userspace client do a poll() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static __poll_t btfmcodec_dev_poll(struct file *file, poll_table *wait)
{
	struct btfmcodec_data *btfmcodec = file->private_data;
	struct btfmcodec_char_device *btfmcodec_dev= NULL;
	__poll_t mask = 0;
	unsigned long flags;

	BTFMCODEC_DBG("start");
	if (!btfmcodec || !btfmcodec->btfmcodec_dev || refcount_read(&btfmcodec->btfmcodec_dev->active_clients) == 1) {
		BTFMCODEC_INFO("%s: %d\n", current->comm, task_pid_nr(current));
		return EPOLLERR;
	} else {
		btfmcodec_dev = btfmcodec->btfmcodec_dev;
	}

	/* Wait here for timeout or for a wakeup signal on readq */
	poll_wait(file, &btfmcodec_dev->readq, wait);

	mutex_lock(&btfmcodec_dev->lock);
	/* recheck if the client has released by the driver */
	if (refcount_read(&btfmcodec_dev->active_clients) == 1) {
		BTFMCODEC_WARN("port has been closed already");
		mutex_unlock(&btfmcodec_dev->lock);
		return POLLHUP;
	}

	spin_lock_irqsave(&btfmcodec_dev->tx_queue_lock, flags);
	/* Set flags if data is avilable to read */
	if (!skb_queue_empty(&btfmcodec_dev->txq))
		mask |= POLLIN | POLLRDNORM;

	spin_unlock_irqrestore(&btfmcodec_dev->tx_queue_lock, flags);
	mutex_unlock(&btfmcodec_dev->lock);

	BTFMCODEC_DBG("end with reason %d", mask);
	return mask;
}

/*
 * btfmcodec_dev_read() - read() syscall for the btfmcodec dev node
 * file:	Pointer to the file structure.
 * buf:		Pointer to the userspace buffer.
 * count:	Number bytes to read from the file.
 * ppos:	Pointer to the position into the file.
 *
 * This function is used to Read the data from btfmcodec pkt device when
 * userspace client do a read() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static ssize_t btfmcodec_dev_read(struct file *file,
			char __user *buf, size_t count, loff_t *ppos)
{
	struct btfmcodec_data *codec = file->private_data;
	struct btfmcodec_char_device *chardev = codec->btfmcodec_dev;
	struct sk_buff *skb;
	ssize_t ret;
	size_t used;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&chardev->read_lock))
		return -ERESTARTSYS;
	while (!(skb = skb_dequeue(&chardev->txq))) {
		if (READ_ONCE(chardev->closing)) {
			ret = -ENETRESET;
			goto unlock;
		}
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto unlock;
		}
		if (wait_event_interruptible(chardev->readq,
					     !skb_queue_empty(&chardev->txq) ||
					     READ_ONCE(chardev->closing))) {
			ret = -ERESTARTSYS;
			goto unlock;
		}
	}
	used = min_t(size_t, count, skb->len);
	if (copy_to_user(buf, skb->data, used)) {
		ret = -EFAULT;
	} else {
		skb_pull(skb, used);
		ret = used;
	}
	if (skb->len)
		skb_queue_head(&chardev->txq, skb);
	else
		kfree_skb(skb);
unlock:
	mutex_unlock(&chardev->read_lock);
	return ret;
}

bool btfmcodec_cp_supported(void)
{
	return is_cp_supported;
}

static long btfmcodec_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct btfmcodec_data *btfmcodec = file->private_data;
	struct hwep_data *hwep_info;

	if (cmd != BTM_CP_UPDATE)
		return -ENOTTY;
	if (arg > 1)
		return -EINVAL;
	BTFMCODEC_INFO("%s: command %04x", __func__, cmd);

	mutex_lock(&btfmcodec->hwep_drv_lock);
	hwep_info = btfmcodec->hwep_info;
	if (!hwep_info) {
		BTFMCODEC_WARN("%s: HWEP is not registered with btfmcodec", __func__);
		BTFMCODEC_WARN("%s: caching required info", __func__);
		is_cp_supported = ((int)arg == 1) ? true : false;
		mutex_unlock(&btfmcodec->hwep_drv_lock);
		return 0;
	}

	switch (cmd) {
	case BTM_CP_UPDATE: {
		if ((int)arg == 1) {
			if (!strcmp(hwep_info->driver_name, "btfmslim"))
				set_bit(BTADV_AUDIO_MASTER_CONFIG, &hwep_info->flags);
			else if (!strcmp(hwep_info->driver_name, "btfmswr_slave"))
				set_bit(BTADV_CONFIGURE_DMA, &hwep_info->flags);
			BTFMCODEC_INFO("%s: This target support CP hwep %s",
					__func__, hwep_info->driver_name);
		} else {
			clear_bit(BTADV_AUDIO_MASTER_CONFIG, &hwep_info->flags);
			clear_bit(BTADV_CONFIGURE_DMA, &hwep_info->flags);
			BTFMCODEC_INFO("%s: This target support doesn't CP", __func__);
		}

	BTFMCODEC_INFO("%s: mastr %d dma codec %d", __func__,
			(int)test_bit(BTADV_AUDIO_MASTER_CONFIG, &hwep_info->flags),
			(int)test_bit(BTADV_CONFIGURE_DMA, &hwep_info->flags));
		break;
	} default: {
		BTFMCODEC_ERR("%s unhandled cmd %04x", __func__, cmd);
	}
	}

	mutex_unlock(&btfmcodec->hwep_drv_lock);
	return 0;
}
static const struct file_operations btfmcodec_fops = {
	.owner = THIS_MODULE,
	.open = btfmcodec_dev_open,
	.release = btfmcodec_dev_release,
	.write = btfmcodec_dev_write,
	.poll = btfmcodec_dev_poll,
	.read = btfmcodec_dev_read,
	/* For Now add no hookups for below callbacks */
	.unlocked_ioctl = btfmcodec_ioctl,
	.compat_ioctl = btfmcodec_ioctl,
};

static ssize_t btfmcodec_attributes_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t n)
{
	struct btfmcodec_data *btfmcodec = dev_to_btfmcodec(dev);
	struct btfmcodec_char_device *btfmcodec_dev = btfmcodec->btfmcodec_dev;
	long tmp;

	mutex_lock(&btfmcodec_dev->lock);
	if (kstrtol(buf, 0, &tmp)) {
		mutex_unlock(&btfmcodec_dev->lock);
		return -EINVAL;
	}
	mutex_unlock(&btfmcodec_dev->lock);

	return n;
}

static ssize_t btfmcodec_attributes_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
//	struct btfmcodec_get_current_transport *btfmcodec_dev = dev_to_btfmcodec(dev);

	return 0;
}

struct btfmcodec_data *btfm_get_btfmcodec(void)
{
	struct btfmcodec_data *codec = READ_ONCE(btfmcodec);

	return codec && READ_ONCE(codec->btfmcodec_dev->ready) ? codec : NULL;
}
EXPORT_SYMBOL(btfm_get_btfmcodec);

bool btfmcodec_commands_stopped(struct btfmcodec_char_device *chardev)
{
	struct btfmcodec_data *codec = chardev->btfmcodec;

	return READ_ONCE(chardev->closing) || READ_ONCE(codec->hwep_stopping);
}

int btfmcodec_wait_response(struct btfmcodec_char_device *chardev, int index,
			    unsigned int timeout_ms)
{
	int ret;

	ret = wait_event_interruptible_timeout(chardev->rsp_wait_q[index],
					       READ_ONCE(chardev->status[index]) !=
					       BTM_WAITING_RSP ||
		btfmcodec_commands_stopped(chardev), msecs_to_jiffies(timeout_ms));
	if (btfmcodec_commands_stopped(chardev))
		return -ESHUTDOWN;
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;
	return READ_ONCE(chardev->status[index]) == BTM_RSP_RECV ? 0 : -EIO;
}

void btfmcodec_stop_commands(struct btfmcodec_data *codec)
{
	struct btfmcodec_char_device *chardev = codec->btfmcodec_dev;
	int i;

	for (i = 0; i < BTM_PKT_TYPE_MAX; i++) {
		WRITE_ONCE(chardev->status[i], BTM_RSP_NOT_RECV_CLIENT_KILLED);
		wake_up_interruptible(&chardev->rsp_wait_q[i]);
	}
	wake_up_interruptible(&chardev->readq);
}

void btfmcodec_drain_commands(struct btfmcodec_data *codec)
{
	struct btfmcodec_char_device *chardev = codec->btfmcodec_dev;

	cancel_work_sync(&chardev->rx_work);
	cancel_work_sync(&chardev->wq_prepare_bearer);
	cancel_work_sync(&chardev->wq_hwep_shutdown);
	cancel_work_sync(&chardev->wq_hwep_configure);
	skb_queue_purge(&chardev->rxq);
	mutex_lock(&chardev->trans_lock);
	skb_queue_purge(&chardev->trans_rxq);
	mutex_unlock(&chardev->trans_lock);
}

int btfmcodec_endpoint_enter(void)
{
	int idx = srcu_read_lock(&btfmcodec_endpoint_srcu);

	if (!btfmcodec || READ_ONCE(btfmcodec->hwep_stopping) ||
	    !READ_ONCE(btfmcodec->hwep_info)) {
		srcu_read_unlock(&btfmcodec_endpoint_srcu, idx);
		return -ENODEV;
	}
	return idx;
}

void btfmcodec_endpoint_exit(int idx)
{
	srcu_read_unlock(&btfmcodec_endpoint_srcu, idx);
}

static DEVICE_ATTR_RW(btfmcodec_attributes);
static struct attribute *btfmcodec_attrs[] = {
	&dev_attr_btfmcodec_attributes.attr,
	NULL,
};
ATTRIBUTE_GROUPS(btfmcodec);

static void btfmcodec_device_release(struct device *dev)
{
	struct btfmcodec_data *codec = dev_to_btfmcodec(dev);

	kfree(codec->btfmcodec_dev);
	kfree(codec);
}

static int __init btfmcodec_init(void)
{
	struct btfmcodec_data *codec;
	struct btfmcodec_char_device *chardev;
	int ret, i;

	codec = kzalloc(sizeof(*codec), GFP_KERNEL);
	if (!codec)
		return -ENOMEM;
	chardev = kzalloc(sizeof(*chardev), GFP_KERNEL);
	if (!chardev) {
		kfree(codec);
		return -ENOMEM;
	}
	codec->btfmcodec_dev = chardev;
	chardev->btfmcodec = codec;
	mutex_init(&codec->hwep_drv_lock);
	mutex_init(&codec->config_lock);
	mutex_init(&codec->states.state_machine_lock);
	INIT_LIST_HEAD(&codec->config_head);
	mutex_init(&chardev->lock);
	mutex_init(&chardev->trans_lock);
	mutex_init(&chardev->read_lock);
	refcount_set(&chardev->active_clients, 1);
	skb_queue_head_init(&chardev->rxq);
	skb_queue_head_init(&chardev->txq);
	skb_queue_head_init(&chardev->trans_rxq);
	spin_lock_init(&chardev->tx_queue_lock);
	init_waitqueue_head(&chardev->readq);
	for (i = 0; i < BTM_PKT_TYPE_MAX; i++)
		init_waitqueue_head(&chardev->rsp_wait_q[i]);
	INIT_WORK(&chardev->rx_work, btfmcodec_dev_rxwork);
	INIT_WORK(&chardev->wq_hwep_shutdown, btfmcodec_wq_hwep_shutdown);
	INIT_WORK(&chardev->wq_prepare_bearer, btfmcodec_wq_prepare_bearer);
	INIT_WORK(&chardev->wq_hwep_configure, btfmcodec_wq_hwep_configure);
	chardev->workqueue = alloc_ordered_workqueue("btfmcodec_wq", 0);
	if (!chardev->workqueue) {
		ret = -ENOMEM;
		goto free_alloc;
	}
	ret = alloc_chrdev_region(&dev_major, 0, 1, "btfmcodec");
	if (ret)
		goto destroy_workqueue;
	dev_class = class_create("btfmcodec");
	if (IS_ERR(dev_class)) {
		ret = PTR_ERR(dev_class);
		goto unregister_region;
	}
	device_initialize(&codec->dev);
	codec->dev.release = btfmcodec_device_release;
	codec->dev.driver = &driver;
	codec->dev.class = dev_class;
	codec->dev.groups = btfmcodec_groups;
	codec->dev.devt = dev_major;
	dev_set_drvdata(&codec->dev, codec);
	strscpy(chardev->dev_name, "btfmcodec_dev", sizeof(chardev->dev_name));
	ret = dev_set_name(&codec->dev, "%s", chardev->dev_name);
	if (ret)
		goto put_dev;
	cdev_init(&chardev->cdev, &btfmcodec_fops);
	chardev->cdev.owner = THIS_MODULE;
	/* All queues, locks and work are ready before either interface is visible. */
	btfmcodec = codec;
	ret = cdev_device_add(&chardev->cdev, &codec->dev);
	if (ret)
		goto clear_global;
	mutex_lock(&chardev->lock);
	WRITE_ONCE(chardev->ready, true);
	mutex_unlock(&chardev->lock);
	return 0;
clear_global:
	btfmcodec = NULL;
put_dev:
	class_destroy(dev_class);
	unregister_chrdev_region(dev_major, 1);
	destroy_workqueue(chardev->workqueue);
	put_device(&codec->dev);
	return ret;
unregister_region:
	unregister_chrdev_region(dev_major, 1);
destroy_workqueue:
	destroy_workqueue(chardev->workqueue);
free_alloc:
	kfree(chardev);
	kfree(codec);
	return ret;
}

static void __exit btfmcodec_deinit(void)
{
	struct btfmcodec_data *codec = btfmcodec;
	struct btfmcodec_char_device *chardev = codec->btfmcodec_dev;
	unsigned long flags;

	/* Consumers and open file operations hold the normal module reference. */
	mutex_lock(&chardev->lock);
	WRITE_ONCE(chardev->ready, false);
	/* Finish previously admitted producers before closing the queue. */
	spin_lock_irqsave(&chardev->tx_queue_lock, flags);
	WRITE_ONCE(chardev->closing, true);
	spin_unlock_irqrestore(&chardev->tx_queue_lock, flags);
	WRITE_ONCE(codec->hwep_stopping, true);
	mutex_unlock(&chardev->lock);
	cdev_device_del(&chardev->cdev, &codec->dev);
	btfmcodec_stop_commands(codec);
	synchronize_srcu(&btfmcodec_endpoint_srcu);
	btfmcodec_drain_commands(codec);
	destroy_workqueue(chardev->workqueue);
	skb_queue_purge(&chardev->txq);
	WRITE_ONCE(btfmcodec, NULL);
	class_destroy(dev_class);
	unregister_chrdev_region(dev_major, 1);
	put_device(&codec->dev);
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM Bluetooth FM CODEC driver");

module_init(btfmcodec_init);
module_exit(btfmcodec_deinit);

