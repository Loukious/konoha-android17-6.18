// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"UCSI: %s: " fmt, __func__

#include <clocksource/arm_arch_timer.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/ipc_logging.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qti_pmic_glink.h>
#include <linux/usb/typec.h>
#include <linux/usb/ucsi_glink.h>

#include "ucsi.h"

/* PPM specific definitions */
#define MSG_OWNER_UC			32779
#define MSG_TYPE_REQ_RESP		1
#define UCSI_BUF_SIZE			48

#define UC_NOTIFY_RECEIVER_UCSI		0x0
#define UC_UCSI_READ_BUF_REQ		0x11
#define UC_UCSI_WRITE_BUF_REQ		0x12
#define UC_UCSI_USBC_NOTIFY_IND		0x13

/* Generic definitions */
#define UCSI_LOG_BUF_SIZE		256
#define NUM_LOG_PAGES			10
#define UCSI_WAIT_TIME_MS		5000

#define ucsi_dbg(fmt, ...) \
	do { \
		ipc_log_string(udev->ipc_log, fmt, ##__VA_ARGS__); \
		pr_debug(fmt, ##__VA_ARGS__); \
	} while (0)

struct ucsi_read_buf_req_msg {
	struct pmic_glink_hdr	hdr;
};

struct ucsi_read_buf_resp_msg {
	struct pmic_glink_hdr	hdr;
	u8			buf[UCSI_BUF_SIZE];
	u32			ret_code;
};

struct ucsi_write_buf_req_msg {
	struct pmic_glink_hdr	hdr;
	u8			buf[UCSI_BUF_SIZE];
	u32			reserved;
};

struct ucsi_write_buf_resp_msg {
	struct pmic_glink_hdr	hdr;
	u32			ret_code;
};

struct ucsi_notify_ind_msg {
	struct pmic_glink_hdr	hdr;
	u32			notification;
	u32			receiver;
	u32			reserved;
};

struct constat_info_entry {
	struct list_head		node;
	struct ucsi_glink_constat_info	constat_info;
};

struct ucsi_dev {
	struct device *dev;
	struct ucsi *ucsi;
	struct pmic_glink_client *client;
	struct dentry *debugfs_dir;
	void *ipc_log;
	struct mutex io_lock;
	struct mutex state_lock;
	spinlock_t response_lock;
	struct completion ack;
	bool pending;
	u32 pending_opcode;
	int result;
	u8 read_buf[UCSI_BUF_SIZE];
	atomic_t state;
	bool stopping;
	bool restart;
	bool remoteproc_timestamp_en;
	struct work_struct state_work;
	struct work_struct notify_work;
	struct mutex notify_lock;
	struct list_head constat_info_list;
};

struct remoteproc_ts {
	u32	hh;
	u32	mm;
	u32	ss;
	u32	dec;
};

static BLOCKING_NOTIFIER_HEAD(ucsi_glink_notifier);

int register_ucsi_glink_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&ucsi_glink_notifier, nb);
}
EXPORT_SYMBOL(register_ucsi_glink_notifier);

int unregister_ucsi_glink_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&ucsi_glink_notifier, nb);
}
EXPORT_SYMBOL(unregister_ucsi_glink_notifier);

static char *offset_to_name(unsigned int offset)
{
	char *type;

	switch (offset) {
	case UCSI_VERSION:
		type = "VER:";
		break;
	case UCSI_CCI:
		type = "CCI:";
		break;
	case UCSI_CONTROL:
		type = "CONTROL:";
		break;
	case UCSI_MESSAGE_IN:
		type = "MSG_IN:";
		break;
	case UCSI_MESSAGE_OUT:
		type = "MSG_OUT:";
		break;
	default:
		type = "UNKNOWN:";
		break;
	}

	return type;
}

#define CLK_FREQ_KHZ		19200

static void get_remoteproc_timestamp(struct remoteproc_ts *ts)
{
	u64 us = (arch_timer_read_counter() * 1000 / CLK_FREQ_KHZ);
	u64 ss = div64_u64(us, 1000000);

	ts->dec = (u32)(us - (ss * 1000000llu));
	ts->hh = (u32)div64_u64(ss, 3600);
	ts->mm = (u32)(div64_u64(ss, 60) - (ts->hh * 60));
	ts->ss = (u32)(ss - (ts->hh * 3600 + ts->mm * 60));
}

static void ucsi_log(struct ucsi_dev *udev, const char *prefix,
			unsigned int offset, u8 *buf, size_t len)
{
	char str[UCSI_LOG_BUF_SIZE] = { 0 };
	u32 i, pos = 0;
	struct remoteproc_ts ts;

	for (i = 0; i < len && pos < sizeof(str) - 1; i++)
		pos += scnprintf(str + pos, sizeof(str) - pos, "%02x ", buf[i]);

	str[pos] = '\0';

	if (udev->remoteproc_timestamp_en) {
		get_remoteproc_timestamp(&ts);
		ucsi_dbg("%s %s %s (%02u:%02u:%02u.%06u)\n", prefix, offset_to_name(offset),
				str, ts.hh, ts.mm, ts.ss, ts.dec);
	} else {
		ucsi_dbg("%s %s %s\n", prefix, offset_to_name(offset), str);
	}
}

/* The firmware has no transaction cookie: serialize read and write requests. */
static int ucsi_qti_xfer(struct ucsi_dev *udev, unsigned int offset,
		       void *value, size_t size, bool write)
{
	struct ucsi_write_buf_req_msg req = {};
	unsigned long flags;
	int ret;

	if (!value || !size || offset >= UCSI_BUF_SIZE ||
	    size > UCSI_BUF_SIZE - offset)
		return -EINVAL;
	req.hdr.owner = MSG_OWNER_UC;
	req.hdr.type = MSG_TYPE_REQ_RESP;
	req.hdr.opcode = write ? UC_UCSI_WRITE_BUF_REQ : UC_UCSI_READ_BUF_REQ;
	if (write)
		memcpy(req.buf + offset, value, size);

	mutex_lock(&udev->io_lock);
	spin_lock_irqsave(&udev->response_lock, flags);
	if (atomic_read(&udev->state) != PMIC_GLINK_STATE_UP || udev->stopping) {
		spin_unlock_irqrestore(&udev->response_lock, flags);
		ret = -ENOTCONN;
		goto unlock;
	}
	reinit_completion(&udev->ack);
	udev->pending = true;
	udev->pending_opcode = req.hdr.opcode;
	udev->result = -ETIMEDOUT;
	spin_unlock_irqrestore(&udev->response_lock, flags);

	ret = pmic_glink_write(udev->client, &req,
			      write ? sizeof(req) : sizeof(req.hdr));
	if (!ret) {
		wait_for_completion_timeout(&udev->ack,
					    msecs_to_jiffies(UCSI_WAIT_TIME_MS));
		spin_lock_irqsave(&udev->response_lock, flags);
		ret = udev->result;
		if (!ret && !write)
			memcpy(value, udev->read_buf + offset, size);
		spin_unlock_irqrestore(&udev->response_lock, flags);
	}
	spin_lock_irqsave(&udev->response_lock, flags);
	udev->pending = false;
	/* A late reply must not complete a later transaction. Recover on reconnect. */
	if (ret == -ETIMEDOUT)
		atomic_set(&udev->state, PMIC_GLINK_STATE_DOWN);
	spin_unlock_irqrestore(&udev->response_lock, flags);
	if (!ret)
		ucsi_log(udev, write ? "write:" : "read:", offset, value, size);
unlock:
	mutex_unlock(&udev->io_lock);
	return ret;
}

static int ucsi_qti_read_version(struct ucsi *ucsi, u16 *version)
{
	int ret;

	ret = ucsi_qti_xfer(ucsi_get_drvdata(ucsi), UCSI_VERSION,
			    version, sizeof(*version), false);
	/* The vendor transport has only the UCSI 1.x 48-byte register window. */
	if (!ret && *version >= UCSI_VERSION_2_0)
		ret = -EOPNOTSUPP;
	return ret;
}

static int ucsi_qti_read_cci(struct ucsi *ucsi, u32 *cci)
{
	return ucsi_qti_xfer(ucsi_get_drvdata(ucsi), UCSI_CCI,
			     cci, sizeof(*cci), false);
}

static int ucsi_qti_read_message_in(struct ucsi *ucsi, void *value, size_t size)
{
	return ucsi_qti_xfer(ucsi_get_drvdata(ucsi), UCSI_MESSAGE_IN,
			     value, size, false);
}

static int ucsi_qti_async_control(struct ucsi *ucsi, u64 command)
{
	return ucsi_qti_xfer(ucsi_get_drvdata(ucsi), UCSI_CONTROL,
			     &command, sizeof(command), true);
}

static void ucsi_qti_notify_work(struct work_struct *work)
{
	struct ucsi_dev *udev = container_of(work, struct ucsi_dev, notify_work);
	struct constat_info_entry *entry;

	mutex_lock(&udev->notify_lock);
	while (!list_empty(&udev->constat_info_list)) {
		entry = list_first_entry(&udev->constat_info_list,
					struct constat_info_entry, node);
		list_del(&entry->node);
		mutex_unlock(&udev->notify_lock);
		blocking_notifier_call_chain(&ucsi_glink_notifier, 0, &entry->constat_info);
		kfree(entry);
		mutex_lock(&udev->notify_lock);
	}
	mutex_unlock(&udev->notify_lock);
}

static void ucsi_qti_connector_status(struct ucsi_connector *con)
{
	struct ucsi_dev *udev = ucsi_get_drvdata(con->ucsi);
	struct constat_info_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return;
	switch (UCSI_CONSTAT(con, PARTNER_TYPE)) {
	case UCSI_CONSTAT_PARTNER_TYPE_AUDIO:
		entry->constat_info.acc = TYPEC_ACCESSORY_AUDIO;
		break;
	case UCSI_CONSTAT_PARTNER_TYPE_DEBUG:
		entry->constat_info.acc = TYPEC_ACCESSORY_DEBUG;
		break;
	default:
		entry->constat_info.acc = TYPEC_ACCESSORY_NONE;
		break;
	}
	mutex_lock(&udev->notify_lock);
	list_add_tail(&entry->node, &udev->constat_info_list);
	schedule_work(&udev->notify_work);
	mutex_unlock(&udev->notify_lock);
}

static const struct ucsi_operations ucsi_qti_ops = {
	.read_version = ucsi_qti_read_version,
	.read_cci = ucsi_qti_read_cci,
	.poll_cci = ucsi_qti_read_cci,
	.read_message_in = ucsi_qti_read_message_in,
	.sync_control = ucsi_sync_control_common,
	.async_control = ucsi_qti_async_control,
	.connector_status = ucsi_qti_connector_status,
};

static int ucsi_callback(void *priv, void *data, size_t len)
{
	struct ucsi_dev *udev = priv;
	const struct pmic_glink_hdr *hdr = data;
	const struct ucsi_read_buf_resp_msg *read = data;
	const struct ucsi_write_buf_resp_msg *write = data;
	const struct ucsi_notify_ind_msg *notify = data;
	unsigned long flags;
	int ret;

	if (!data || len < sizeof(*hdr) || hdr->owner != MSG_OWNER_UC)
		return -EINVAL;
	if (hdr->opcode == UC_UCSI_USBC_NOTIFY_IND) {
		if (len != sizeof(*notify) || notify->receiver != UC_NOTIFY_RECEIVER_UCSI)
			return -EINVAL;
		mutex_lock(&udev->state_lock);
		if (udev->ucsi && !udev->stopping &&
		    atomic_read(&udev->state) == PMIC_GLINK_STATE_UP)
			ucsi_notify_common(udev->ucsi, notify->notification);
		mutex_unlock(&udev->state_lock);
		return 0;
	}
	if (hdr->opcode != UC_UCSI_READ_BUF_REQ && hdr->opcode != UC_UCSI_WRITE_BUF_REQ)
		return -EINVAL;
	spin_lock_irqsave(&udev->response_lock, flags);
	if (!udev->pending || udev->pending_opcode != hdr->opcode) {
		spin_unlock_irqrestore(&udev->response_lock, flags);
		return 0;
	}
	if (hdr->opcode == UC_UCSI_READ_BUF_REQ) {
		ret = len != sizeof(*read) ? -EMSGSIZE : read->ret_code ? -EREMOTEIO : 0;
		if (!ret)
			memcpy(udev->read_buf, read->buf, sizeof(udev->read_buf));
	} else {
		ret = len != sizeof(*write) ? -EMSGSIZE : write->ret_code ? -EREMOTEIO : 0;
	}
	udev->result = ret;
	udev->pending = false;
	complete(&udev->ack);
	spin_unlock_irqrestore(&udev->response_lock, flags);
	return ret;
}

/* One worker owns registration; detach callbacks before unregister can free connectors. */
static void ucsi_qti_state_work(struct work_struct *work)
{
	struct ucsi_dev *udev = container_of(work, struct ucsi_dev, state_work);
	struct ucsi *ucsi;
	bool up, restart;
	int ret;

	mutex_lock(&udev->state_lock);
	up = !udev->stopping && atomic_read(&udev->state) == PMIC_GLINK_STATE_UP;
	restart = udev->restart;
	udev->restart = false;
	ucsi = udev->ucsi;
	if (ucsi && (!up || restart))
		udev->ucsi = NULL;
	mutex_unlock(&udev->state_lock);
	if (ucsi && (!up || restart)) {
		ucsi_unregister(ucsi);
		ucsi_destroy(ucsi);
		ucsi = NULL;
	}
	if (!up || ucsi)
		return;
	ucsi = ucsi_create(udev->dev, &ucsi_qti_ops);
	if (IS_ERR(ucsi)) {
		dev_err(udev->dev, "failed to create UCSI: %ld\n", PTR_ERR(ucsi));
		return;
	}
	ucsi_set_drvdata(ucsi, udev);
	mutex_lock(&udev->state_lock);
	udev->ucsi = ucsi;
	mutex_unlock(&udev->state_lock);
	ret = ucsi_register(ucsi);
	if (ret) {
		mutex_lock(&udev->state_lock);
		udev->ucsi = NULL;
		mutex_unlock(&udev->state_lock);
		ucsi_destroy(ucsi);
		dev_err(udev->dev, "failed to register UCSI: %d\n", ret);
	}
}

static void ucsi_qti_state_cb(void *priv, enum pmic_glink_state state)
{
	struct ucsi_dev *udev = priv;
	unsigned long flags;

	mutex_lock(&udev->state_lock);
	spin_lock_irqsave(&udev->response_lock, flags);
	atomic_set(&udev->state, state);
	if (state == PMIC_GLINK_STATE_DOWN) {
		udev->restart = true;
		if (udev->pending) {
			udev->result = -ENOTCONN;
			udev->pending = false;
			complete(&udev->ack);
		}
	}
	spin_unlock_irqrestore(&udev->response_lock, flags);
	if (!udev->stopping)
		schedule_work(&udev->state_work);
	mutex_unlock(&udev->state_lock);
}

static void ucsi_qti_stop(struct ucsi_dev *udev)
{
	struct ucsi *ucsi;
	struct constat_info_entry *entry, *tmp;
	unsigned long flags;

	mutex_lock(&udev->state_lock);
	spin_lock_irqsave(&udev->response_lock, flags);
	udev->stopping = true;
	atomic_set(&udev->state, PMIC_GLINK_STATE_DOWN);
	udev->result = -ENOTCONN;
	udev->pending = false;
	complete(&udev->ack);
	spin_unlock_irqrestore(&udev->response_lock, flags);
	mutex_unlock(&udev->state_lock);
	mutex_lock(&udev->io_lock);
	mutex_unlock(&udev->io_lock);
	pmic_glink_unregister_client(udev->client);
	cancel_work_sync(&udev->state_work);
	mutex_lock(&udev->state_lock);
	ucsi = udev->ucsi;
	udev->ucsi = NULL;
	mutex_unlock(&udev->state_lock);
	if (ucsi) {
		ucsi_unregister(ucsi);
		ucsi_destroy(ucsi);
	}
	cancel_work_sync(&udev->notify_work);
	list_for_each_entry_safe(entry, tmp, &udev->constat_info_list, node) {
		list_del(&entry->node);
		kfree(entry);
	}
}

static int ucsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pmic_glink_client_data client_data = {};
	struct ucsi_dev *udev;
	int ret;

	udev = devm_kzalloc(dev, sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;
	udev->dev = dev;
	mutex_init(&udev->io_lock);
	mutex_init(&udev->state_lock);
	mutex_init(&udev->notify_lock);
	spin_lock_init(&udev->response_lock);
	init_completion(&udev->ack);
	INIT_LIST_HEAD(&udev->constat_info_list);
	INIT_WORK(&udev->state_work, ucsi_qti_state_work);
	INIT_WORK(&udev->notify_work, ucsi_qti_notify_work);
	atomic_set(&udev->state, PMIC_GLINK_STATE_UP);
	udev->ipc_log = ipc_log_context_create(NUM_LOG_PAGES, "ucsi", 0);
	client_data.id = MSG_OWNER_UC;
	client_data.name = "ucsi";
	client_data.msg_cb = ucsi_callback;
	client_data.priv = udev;
	client_data.state_cb = ucsi_qti_state_cb;
	/* State callbacks can start immediately; hold the lock until client publication. */
	mutex_lock(&udev->state_lock);
	udev->client = pmic_glink_register_client(dev, &client_data);
	if (IS_ERR(udev->client)) {
		ret = PTR_ERR(udev->client);
		mutex_unlock(&udev->state_lock);
		ipc_log_context_destroy(udev->ipc_log);
		return ret;
	}
	schedule_work(&udev->state_work);
	mutex_unlock(&udev->state_lock);
	platform_set_drvdata(pdev, udev);
	udev->debugfs_dir = debugfs_create_dir("ucsi_glink", NULL);
	if (!IS_ERR_OR_NULL(udev->debugfs_dir))
		debugfs_create_bool("remoteproc_timestamp_en", 0600, udev->debugfs_dir,
				    &udev->remoteproc_timestamp_en);
	return 0;
}

static void ucsi_remove(struct platform_device *pdev)
{
	struct ucsi_dev *udev = platform_get_drvdata(pdev);

	debugfs_remove_recursive(udev->debugfs_dir);
	ucsi_qti_stop(udev);
	ipc_log_context_destroy(udev->ipc_log);
}

static const struct of_device_id ucsi_match_table[] = {
	{ .compatible = "qcom,ucsi-glink" },
	{},
};
MODULE_DEVICE_TABLE(of, ucsi_match_table);

static struct platform_driver ucsi_driver = {
	.driver = {
		.name = "ucsi_glink",
		.of_match_table = ucsi_match_table,
	},
	.probe = ucsi_probe,
	.remove = ucsi_remove,
};
module_platform_driver(ucsi_driver);

MODULE_DESCRIPTION("QTI UCSI Glink driver");
MODULE_LICENSE("GPL");
