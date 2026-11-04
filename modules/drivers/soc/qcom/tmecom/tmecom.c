// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"tmecom: [%s][%d]:" fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mailbox_client.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/mailbox/qmp.h>
#include <linux/uaccess.h>
#include <linux/mailbox_controller.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <linux/unaligned.h>

#include "tmecom.h"

struct tmecom {
	struct device *dev;
	struct mbox_client cl;
	struct mbox_chan *chan;
	spinlock_t rx_lock;
	struct qmp_pkt tx_pkt;
	wait_queue_head_t waitq;
	void *txbuf;
	void *rxbuf;
	size_t rxsize;
	int rx_error;
	bool rx_done;
	bool waiting;
	bool desynchronized;
	u32 transaction;
	int tx_error;
};

/* Serialize requests and pin the device until each request has finished. */
static DEFINE_MUTEX(tmecom_lock);
/* Shared by reprobes. Never reuse an ID within this module lifetime. */
static u32 next_transaction;

#if IS_ENABLED(CONFIG_DEBUG_FS) && IS_ENABLED(CONFIG_QTI_HW_KEY_MANAGER)
#include <linux/tme_hwkm_master_defs.h>
#include <linux/tme_hwkm_master.h>

char dpkt[MBOX_MAX_MSG_LEN + 1];
struct dentry *debugfs_file;
#endif /* CONFIG_DEBUG_FS */

static struct tmecom *tmedev;

/**
 * tmecom_msg_hdr - Request/Response message header between HLOS and TME.
 *
 * This header is proceeding any request specific parameters.
 * The transaction id is used to match request with response.
 *
 * Note: glink/QMP layer provides the rx/tx data size, so user payload size
 * is calculated by reducing the header size.
 */
struct tmecom_msg_hdr {
	unsigned int reserved; /* for future use */
	unsigned int txnid;    /* transaction id */
} __packed;
#define TMECOM_TX_HDR_SIZE sizeof(struct tmecom_msg_hdr)
#define CBOR_NUM_BYTES (sizeof(unsigned int))
#define TMECOM_RX_HDR_SIZE (TMECOM_TX_HDR_SIZE + CBOR_NUM_BYTES)

/* Use the native transaction header and clear reserved/alignment bytes. */
static size_t tmecom_encode(struct tmecom *tdev, const void *reqbuf, size_t size)
{
	size_t frame_size = ALIGN(size + TMECOM_TX_HDR_SIZE, sizeof(u32));

	memset(tdev->txbuf, 0, frame_size);
	put_unaligned_le32(tdev->transaction, (u8 *)tdev->txbuf + sizeof(u32));
	memcpy((u8 *)tdev->txbuf + TMECOM_TX_HDR_SIZE, reqbuf, size);
	return frame_size;
}

int tmecom_process_request(const void *reqbuf, size_t reqsize, void *respbuf,
		size_t *respsize)
{
	struct tmecom *tdev;
	size_t capacity, payload_size;
	unsigned long flags;
	long time_left;
	int ret;

	if (!reqbuf || !reqsize || reqsize > MBOX_MAX_MSG_LEN - TMECOM_TX_HDR_SIZE ||
	    !respbuf || !respsize)
		return -EINVAL;
	capacity = *respsize;
	if (!capacity || capacity > MBOX_MAX_MSG_LEN)
		return -EINVAL;
	*respsize = 0;
	ret = mutex_lock_interruptible(&tmecom_lock);
	if (ret)
		return ret;
	tdev = tmedev;
	if (!tdev) {
		ret = -ENODEV;
		goto unlock;
	}
	spin_lock_irqsave(&tdev->rx_lock, flags);
	if (tdev->desynchronized) {
		spin_unlock_irqrestore(&tdev->rx_lock, flags);
		ret = -EPIPE;
		goto unlock;
	}
	if (!next_transaction)
		next_transaction = get_random_u32() ?: 1;
	if (next_transaction == U32_MAX) {
		spin_unlock_irqrestore(&tdev->rx_lock, flags);
		ret = -EOVERFLOW;
		goto unlock;
	}
	tdev->transaction = next_transaction++;
	tdev->tx_error = 0;
	tdev->waiting = true;
	tdev->rx_done = false;
	tdev->rx_error = 0;
	tdev->rxsize = 0;
	spin_unlock_irqrestore(&tdev->rx_lock, flags);
	tdev->tx_pkt.size = tmecom_encode(tdev, reqbuf, reqsize);
	tdev->tx_pkt.data = tdev->txbuf;
	ret = mbox_send_message(tdev->chan, &tdev->tx_pkt);
	if (ret < 0)
		goto finish;
	if (READ_ONCE(tdev->tx_error)) {
		ret = READ_ONCE(tdev->tx_error);
		goto finish;
	}
	time_left = wait_event_interruptible_timeout(tdev->waitq,
			smp_load_acquire(&tdev->rx_done), msecs_to_jiffies(tdev->cl.tx_tout));
	if (time_left < 0)
		ret = time_left;
	else if (!time_left)
		ret = -ETIMEDOUT;
	else
		ret = 0;
finish:
	spin_lock_irqsave(&tdev->rx_lock, flags);
	tdev->waiting = false;
	if (ret == -ETIME) {
		/* The mailbox core can leave an unsubmitted packet queued on TX timeout.
		 * Keep this packet stable until the channel has been shut down/drained.
		 * Response timeouts/interrupted waits are recoverable through new IDs.
		 */
		tdev->desynchronized = true;
	} else if (!ret && tdev->rx_error) {
		ret = tdev->rx_error;
	} else if (!ret) {
		payload_size = tdev->rxsize - TMECOM_RX_HDR_SIZE;
		if (payload_size > capacity) {
			*respsize = payload_size;
			ret = -EMSGSIZE;
		} else {
			memcpy(respbuf, (u8 *)tdev->rxbuf + TMECOM_RX_HDR_SIZE, payload_size);
			*respsize = payload_size;
		}
	}
	spin_unlock_irqrestore(&tdev->rx_lock, flags);
unlock:
	mutex_unlock(&tmecom_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tmecom_process_request);

#if IS_ENABLED(CONFIG_DEBUG_FS) && IS_ENABLED(CONFIG_QTI_HW_KEY_MANAGER)
static ssize_t tmecom_debugfs_write(struct file *file,
		const char __user *userstr, size_t len, loff_t *pos)
{
	int ret = 0;
	size_t rxlen = MBOX_MAX_MSG_LEN;
	struct tme_ext_err_info *err_info = (struct tme_ext_err_info *)dpkt;

	if (!len || (len > MBOX_MAX_MSG_LEN)) {
		pr_err("invalid message length\n");
		return -EINVAL;
	}

	memset(dpkt, 0, sizeof(dpkt));
	ret = copy_from_user(dpkt, userstr, len);
	if (ret) {
		pr_err("%s copy from user failed, ret=%d\n", __func__, ret);
		return -EFAULT;
	}

	ret = tmecom_process_request(dpkt, len, dpkt, &rxlen);
	if (ret)
		return ret;

	print_hex_dump_bytes("tmecom decoded bytes : ",
			DUMP_PREFIX_ADDRESS, dpkt, rxlen);

	pr_debug("calling TME_HWKM_CMD_BROADCAST_TP_KEY api\n");
	ret = tme_hwkm_master_broadcast_transportkey(err_info);

	if (ret == 0)
		pr_debug("%s successful\n", __func__);

	return len;
}

static const struct file_operations tmecom_debugfs_ops = {
	.open = simple_open,
	.write = tmecom_debugfs_write,
};
#endif /* CONFIG_DEBUG_FS */

static void tmecom_receive_message(struct mbox_client *client, void *message)
{
	struct tmecom *tdev = container_of(client, struct tmecom, cl);
	const struct qmp_pkt *pkt = message;
	unsigned long flags;

	spin_lock_irqsave(&tdev->rx_lock, flags);
	if (tdev->desynchronized || !tdev->waiting)
		goto unlock;
	if (tdev->rx_done)
		goto unlock;
	/* Only the response to the outstanding native transaction may complete it. */
	if (!pkt || !pkt->data || pkt->size < TMECOM_TX_HDR_SIZE ||
	    get_unaligned_le32((const u8 *)pkt->data + sizeof(u32)) != tdev->transaction)
		goto unlock;
	if (pkt->size <= TMECOM_RX_HDR_SIZE ||
	    pkt->size > MBOX_MAX_MSG_LEN) {
		tdev->rx_error = -EPROTO;
	} else {
		/* The controller reuses pkt->data as soon as this callback returns. */
		memcpy(tdev->rxbuf, pkt->data, pkt->size);
		tdev->rxsize = pkt->size;
	}
	smp_store_release(&tdev->rx_done, true);
unlock:
	spin_unlock_irqrestore(&tdev->rx_lock, flags);
	wake_up_interruptible(&tdev->waitq);
}

static void tmecom_transmit_done(struct mbox_client *client, void *message, int error)
{
	struct tmecom *tdev = container_of(client, struct tmecom, cl);

	WRITE_ONCE(tdev->tx_error, error);
}

static int tmecom_probe(struct platform_device *pdev)
{
	struct tmecom *tdev;
	int ret = 0;

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;
	tdev->txbuf = devm_kzalloc(&pdev->dev, MBOX_MAX_MSG_LEN, GFP_KERNEL);
	tdev->rxbuf = devm_kzalloc(&pdev->dev, MBOX_MAX_MSG_LEN, GFP_KERNEL);
	if (!tdev->txbuf || !tdev->rxbuf)
		return -ENOMEM;
	tdev->dev = &pdev->dev;
	spin_lock_init(&tdev->rx_lock);
	init_waitqueue_head(&tdev->waitq);
	tdev->cl.dev = &pdev->dev;
	tdev->cl.tx_block = true;
	tdev->cl.tx_tout = 500;
	tdev->cl.knows_txdone = false;
	tdev->cl.rx_callback = tmecom_receive_message;
	tdev->cl.tx_done = tmecom_transmit_done;
	platform_set_drvdata(pdev, tdev);

	mutex_lock(&tmecom_lock);
	if (tmedev) {
		ret = -EBUSY;
		goto unlock;
	}
	tdev->chan = mbox_request_channel(&tdev->cl, 0);
	if (IS_ERR(tdev->chan)) {
		ret = PTR_ERR(tdev->chan);
		goto unlock;
	}
	/* QMP carries no channel ID in RX; shared mailboxes are transmit-only. */
	if (tdev->chan->mbox->num_chans != 1) {
		mbox_free_channel(tdev->chan);
		ret = -EOPNOTSUPP;
		goto unlock;
	}
#if IS_ENABLED(CONFIG_DEBUG_FS) && IS_ENABLED(CONFIG_QTI_HW_KEY_MANAGER)
	debugfs_file = debugfs_create_file("tmecom_send_message", 0220, NULL, tdev,
			&tmecom_debugfs_ops);
	if (IS_ERR_OR_NULL(debugfs_file)) {
		ret = debugfs_file ? PTR_ERR(debugfs_file) : -ENOMEM;
		mbox_free_channel(tdev->chan);
		goto unlock;
	}
#endif
	tmedev = tdev;
unlock:
	mutex_unlock(&tmecom_lock);
	return ret;
}

static void tmecom_remove(struct platform_device *pdev)
{
	struct tmecom *tdev = platform_get_drvdata(pdev);

#if IS_ENABLED(CONFIG_DEBUG_FS) && IS_ENABLED(CONFIG_QTI_HW_KEY_MANAGER)
	debugfs_remove(debugfs_file);
#endif
	/* Stop new callers and wait for the serialized request before shutdown. */
	mutex_lock(&tmecom_lock);
	if (tmedev == tdev)
		tmedev = NULL;
	mbox_free_channel(tdev->chan);
	mutex_unlock(&tmecom_lock);
}

static const struct of_device_id tmecom_match_tbl[] = {
	{.compatible = "qcom,tmecom-qmp-client"},
	{},
};

static struct platform_driver tmecom_driver = {
	.probe = tmecom_probe,
	.remove = tmecom_remove,
	.driver = {
		.name = "tmecom-qmp-client",
		.suppress_bind_attrs = true,
		.of_match_table = tmecom_match_tbl,
	},
};
module_platform_driver(tmecom_driver);

MODULE_DESCRIPTION("MSM TMECom QTI mailbox protocol client");
MODULE_LICENSE("GPL");
