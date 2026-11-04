// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"PMIC_GLINK: %s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/ipc_logging.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/workqueue.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/qti_pmic_glink.h>

#define NUM_LOG_PAGES		10

#define pmic_glink_dbg(pgdev, fmt, ...) \
	do { \
		ipc_log_string(pgdev->ipc_log, fmt, ##__VA_ARGS__); \
		pr_debug(fmt, ##__VA_ARGS__); \
	} while (0)

/**
 * struct pmic_glink_dev - Top level data structure for pmic_glink device
 * @rpdev:		rpmsg device from rpmsg framework
 * @dev:		pmic_glink parent device for all child devices
 * @debugfs_dir:	Debugfs directory handle
 * @channel_name:	Glink channel name used by rpmsg device
 * @ipc_log:		ipc logging handle
 * @client_idr:		idr list for the clients
 * @client_lock:	mutex lock when idr APIs are used on client_idr
 * @rpdev_sem:		read-write semaphore to synchronize glink channel
 *			availability and rpmsg transactions
 * @rx_lock:		spinlock to be used when rx_list is modified
 * @rx_work:		worker for handling rx messages
 * @init_work:		worker to instantiate child devices under pdev
 * @rx_wq:		workqueue for rx messages
 * @rx_list:		list for rx messages
 * @dev_list:		list for pmic_glink_dev_list
 * @state:		indicates when remote subsystem is up/down
 * @prev_state:		previous state of remote subsystem
 * @child_probed:	indicates when the children are probed
 * @log_filter:		message owner filter for logging
 * @log_enable:		enables message logging
 * @client_dev_list:	list of client devices to be notified on state
 *			transition during an SSR or PDR
 * @ssr_nb:		notifier block for subsystem notifier
 * @subsys_name:	subsystem name from which SSR notifications should
 *			be handled and notified to the clients
 * @subsys_handle:	handle to subsystem notifier
 * @pdr_handle:		handle to PDR notifier
 * @pdr_service_name:	protection domain service name
 * @pdr_path_name:	protection domain path name
 * @pdr_state:		protection domain service state
 */
struct pmic_glink_dev {
	struct rpmsg_device	*rpdev;
	struct device		*dev;
	struct dentry		*debugfs_dir;
	const char		*channel_name;
	void			*ipc_log;
	struct idr		client_idr;
	struct mutex		client_lock;
	struct srcu_struct	client_srcu;
	struct list_head reply_fences;
	bool			removing;
	bool			rx_enabled;
	struct rw_semaphore	rpdev_sem;
	spinlock_t		rx_lock;
	struct work_struct	rx_work;
	struct work_struct	init_work;
	struct workqueue_struct	*rx_wq;
	struct list_head	rx_list;
	struct list_head	dev_list;
	atomic_t		state;
	atomic_t		prev_state;
	bool			child_probed;
	u32			log_filter;
	bool			log_enable;
	struct list_head	client_dev_list;
	struct notifier_block	ssr_nb;
	const char		*subsys_name;
	void			*subsys_handle;
	void			*pdr_handle;
	const char		*pdr_service_name;
	const char		*pdr_path_name;
	atomic_t		pdr_state;
};

/**
 * struct pmic_glink_client - pmic_glink client device
 * @pgdev:	pmic_glink device for the client device
 * @name:	Client name
 * @id:		Unique id for client for communication
 * @lock:	lock for sending data
 * @priv:	private data for client
 * @msg_cb:	callback function for client to receive the messages that
 *		are intended to be delivered to it over PMIC Glink
 * @node:	list node to be added in client_dev_list of pmic_glink device
 * @state_cb:	callback function to notify pmic glink state in the event of
 *		a subsystem restart (SSR) or a protection domain restart (PDR)
 */
struct pmic_glink_client {
	struct pmic_glink_dev	*pgdev;
	const char		*name;
	u32			id;
	struct mutex		lock;
	void			*priv;
	int			(*msg_cb)(void *priv, void *data, size_t len);
	struct list_head	node;
	void			(*state_cb)(void *priv,
					  enum pmic_glink_state state);
};

struct pmic_glink_buf {
	struct list_head	node;
	size_t			len;
	u8			buf[];
};

static LIST_HEAD(pmic_glink_dev_list);
static DEFINE_MUTEX(pmic_glink_dev_lock);

/* Native replies have an owner/opcode but no transaction ID. Preserve an
 * unanswered request across client unregister so the same owner/opcode cannot
 * be reused until its late reply arrives or the actual channel is drained.
 * All fence list operations hold client_lock.
 */
struct pmic_glink_reply_fence {
	struct list_head node;
	struct pmic_glink_client *client;
	u32 owner, opcode, type;
	size_t min_len, max_len;
	bool received;
	bool keyed;
	size_t key_offset;
	u32 key;
};

static int pmic_glink_request_begin_internal(struct pmic_glink_client *client, u32 opcode,
			    u32 type, size_t min_len, size_t max_len,
			    bool keyed, size_t key_offset, u32 key)
{
	struct pmic_glink_reply_fence *fence, *old;
	struct pmic_glink_dev *pgdev;
	int ret = 0;

	if (!client || !client->pgdev || min_len < sizeof(struct pmic_glink_hdr) ||
	    max_len < min_len || (keyed && (min_len < sizeof(key) || key_offset > min_len - sizeof(key))))
		return -EINVAL;
	pgdev = client->pgdev;
	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return -ENOMEM;
	down_read(&pgdev->rpdev_sem);
	if (pgdev->removing || !pgdev->rpdev || !atomic_read(&pgdev->state)) {
		ret = -ENOTCONN;
		goto unlock_transport;
	}
	mutex_lock(&pgdev->client_lock);
	if (idr_find(&pgdev->client_idr, client->id) != client) {
		ret = -ESHUTDOWN;
		goto unlock_clients;
	}
	list_for_each_entry(old, &pgdev->reply_fences, node) {
		if (old->owner == client->id && old->opcode == opcode) {
			ret = -EAGAIN;
			goto unlock_clients;
		}
	}
	fence->client = client;
	fence->owner = client->id;
	fence->opcode = opcode;
	fence->type = type;
	fence->min_len = min_len;
	fence->max_len = max_len;
	fence->keyed = keyed;
	fence->key_offset = key_offset;
	fence->key = key;
	list_add_tail(&fence->node, &pgdev->reply_fences);
	fence = NULL;
unlock_clients:
	mutex_unlock(&pgdev->client_lock);
unlock_transport:
	up_read(&pgdev->rpdev_sem);
	kfree(fence);
	return ret;
}
int pmic_glink_request_begin(struct pmic_glink_client *client, u32 opcode,
			    u32 type, size_t min_len, size_t max_len)
{
	return pmic_glink_request_begin_internal(client, opcode, type, min_len, max_len,
					 false, 0, 0);
}
EXPORT_SYMBOL_GPL(pmic_glink_request_begin);

int pmic_glink_request_begin_keyed(struct pmic_glink_client *client, u32 opcode,
			    u32 type, size_t min_len, size_t max_len,
			    size_t key_offset, u32 key)
{
	return pmic_glink_request_begin_internal(client, opcode, type, min_len, max_len,
					 true, key_offset, key);
}
EXPORT_SYMBOL_GPL(pmic_glink_request_begin_keyed);

/* Finish only this client's record. An old callback must never remove a
 * replacement client's newer record for the same owner/opcode.
 */
void pmic_glink_request_end(struct pmic_glink_client *client, u32 opcode)
{
	struct pmic_glink_reply_fence *fence, *tmp;
	struct pmic_glink_dev *pgdev = client->pgdev;

	mutex_lock(&pgdev->client_lock);
	list_for_each_entry_safe(fence, tmp, &pgdev->reply_fences, node) {
		if (fence->client == client && fence->opcode == opcode) {
			list_del(&fence->node);
			kfree(fence);
		}
	}
	mutex_unlock(&pgdev->client_lock);
}
EXPORT_SYMBOL_GPL(pmic_glink_request_end);

void pmic_glink_request_reject(struct pmic_glink_client *client, u32 opcode)
{
	struct pmic_glink_reply_fence *fence;
	struct pmic_glink_dev *pgdev = client->pgdev;

	mutex_lock(&pgdev->client_lock);
	list_for_each_entry(fence, &pgdev->reply_fences, node)
		if (fence->client == client && fence->opcode == opcode)
			fence->received = false;
	mutex_unlock(&pgdev->client_lock);
}
EXPORT_SYMBOL_GPL(pmic_glink_request_reject);

static void pmic_glink_orphan_requests(struct pmic_glink_dev *pgdev,
				     struct pmic_glink_client *client)
{
	struct pmic_glink_reply_fence *fence, *tmp;

	list_for_each_entry_safe(fence, tmp, &pgdev->reply_fences, node) {
		if (fence->client != client)
			continue;
		if (fence->received) {
			list_del(&fence->node);
			kfree(fence);
		} else {
			fence->client = NULL;
		}
	}
}

static bool pmic_glink_filter_reply(struct pmic_glink_dev *pgdev,
				   const void *data, size_t len)
{
	struct pmic_glink_hdr hdr;
	struct pmic_glink_reply_fence *fence, *tmp;
	bool valid;

	if (len < sizeof(hdr))
		return false;
	memcpy(&hdr, data, sizeof(hdr));
	list_for_each_entry_safe(fence, tmp, &pgdev->reply_fences, node) {
		if (fence->owner != hdr.owner || fence->opcode != hdr.opcode)
			continue;
		valid = hdr.type == fence->type && len >= fence->min_len && len <= fence->max_len;
		if (valid && fence->keyed) {
			u32 key;

			memcpy(&key, (const u8 *)data + fence->key_offset, sizeof(key));
			valid = key == fence->key;
		}
		if (!fence->client) {
			if (valid) {
				list_del(&fence->node);
				kfree(fence);
			}
			return true;
		}
		/* Unregister has detached this client but is still draining its
		 * callbacks. Do not route its reply to a newly registered owner.
		 */
		if (idr_find(&pgdev->client_idr, fence->owner) != fence->client)
			return true;
		if (valid)
			fence->received = true;
		return false;
	}
	return false;
}

static void pmic_glink_clear_requests(struct pmic_glink_dev *pgdev)
{
	struct pmic_glink_reply_fence *fence, *tmp;

	mutex_lock(&pgdev->client_lock);
	list_for_each_entry_safe(fence, tmp, &pgdev->reply_fences, node) {
		list_del(&fence->node);
		kfree(fence);
	}
	mutex_unlock(&pgdev->client_lock);
}

static void pmic_glink_notify_clients(struct pmic_glink_dev *pgdev,
				     enum pmic_glink_state state)
{
	struct pmic_glink_client *pos;
	int idx;

	pm_stay_awake(pgdev->dev);
	idx = srcu_read_lock(&pgdev->client_srcu);
	list_for_each_entry_rcu(pos, &pgdev->client_dev_list, node,
			       srcu_read_lock_held(&pgdev->client_srcu))
		pos->state_cb(pos->priv, state);
	srcu_read_unlock(&pgdev->client_srcu, idx);
	pm_relax(pgdev->dev);
}


static int pmic_glink_ssr_notifier_cb(struct notifier_block *nb,
				unsigned long code, void *data)
{
	struct pmic_glink_dev *pgdev = container_of(nb, struct pmic_glink_dev,
						ssr_nb);

	pmic_glink_dbg(pgdev, "code: %lu\n", code);

	switch (code) {
	case QCOM_SSR_BEFORE_SHUTDOWN:
		atomic_set(&pgdev->prev_state, code);
		pmic_glink_notify_clients(pgdev, PMIC_GLINK_STATE_DOWN);
		break;
	case QCOM_SSR_AFTER_POWERUP:
		/*
		 * Do not notify PMIC Glink clients here but rather from
		 * pmic_glink_init_work which will be run only after rpmsg
		 * driver is probed and Glink communication is up.
		 */
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static void pmic_glink_pdr_notifier_cb(int state, char *service_name,
					void *priv)
{
	struct pmic_glink_dev *pgdev = priv;

	pmic_glink_dbg(pgdev, "PDR state: %x\n", state);

	switch (state) {
	case SERVREG_SERVICE_STATE_DOWN:
		pmic_glink_dbg(pgdev, "PD state down for %s\n",
				pgdev->pdr_service_name);
		pmic_glink_notify_clients(pgdev, PMIC_GLINK_STATE_DOWN);
		atomic_set(&pgdev->pdr_state, state);
		break;
	case SERVREG_SERVICE_STATE_UP:
		/*
		 * Do not notify PMIC Glink clients here but rather from
		 * pmic_glink_init_work which will be run only after rpmsg
		 * driver is probed and Glink communication is up.
		 */
		pmic_glink_dbg(pgdev, "PD state up for %s\n",
				pgdev->pdr_service_name);
		break;
	default:
		break;
	}
}

static struct pmic_glink_dev *get_pmic_glink_from_dev(struct device *dev)
{
	struct pmic_glink_dev *tmp, *pos;

	mutex_lock(&pmic_glink_dev_lock);
	list_for_each_entry_safe(pos, tmp, &pmic_glink_dev_list, dev_list) {
		if (pos->dev == dev) {
			mutex_unlock(&pmic_glink_dev_lock);
			return pos;
		}
	}
	mutex_unlock(&pmic_glink_dev_lock);

	return NULL;
}

static struct pmic_glink_dev *get_pmic_glink_from_rpdev(struct rpmsg_device *rpdev)
{
	struct pmic_glink_dev *pos, *pgdev = NULL;
	struct device_link *link;

	mutex_lock(&pmic_glink_dev_lock);
	list_for_each_entry(pos, &pmic_glink_dev_list, dev_list) {
		if (strcmp(rpdev->id.name, pos->channel_name))
			continue;
		link = device_link_add(&rpdev->dev, pos->dev,
				       DL_FLAG_AUTOREMOVE_CONSUMER);
		if (!link)
			break;
		if (READ_ONCE(link->status) != DL_STATE_CONSUMER_PROBE &&
		    READ_ONCE(link->status) != DL_STATE_ACTIVE) {
			device_link_del(link);
			break;
		}
		pgdev = pos;
		break;
	}
	mutex_unlock(&pmic_glink_dev_lock);
	return pgdev;
}


/**
 * pmic_glink_write() - Send data from client to remote subsystem
 *
 * @client: Client device pointer that is registered already
 * @data: Pointer to data that needs to be sent
 * @len: Length of data
 *
 * Return: 0 if success, negative on error.
 */
int pmic_glink_write(struct pmic_glink_client *client, void *data,
			size_t len)
{
	int rc;

	if (!client || !client->pgdev || !client->name)
		return -ENODEV;
	if (!data || len < (int)sizeof(struct pmic_glink_hdr) || len > INT_MAX)
		return -EINVAL;

	down_read(&client->pgdev->rpdev_sem);

	if (!client->pgdev->rpdev || !atomic_read(&client->pgdev->state)) {
		pr_err("Error in sending data for client %s\n", client->name);
		up_read(&client->pgdev->rpdev_sem);
		return -ENOTCONN;
	}

	mutex_lock(&client->lock);
	rc = rpmsg_send(client->pgdev->rpdev->ept, data, len);
	mutex_unlock(&client->lock);
	up_read(&client->pgdev->rpdev_sem);

	if (rc < 0)
		pr_err("Failed to send data [%*ph] for client %s, rc=%d\n",
			(int)len, data, client->name, rc);

	if (!rc && client->pgdev->log_enable) {
		struct pmic_glink_hdr *hdr = data;

		if (client->pgdev->log_filter == hdr->owner)
			pr_info("Tx data: %*ph\n", (int)len, data);
		else if (client->pgdev->log_filter == 65535)
			pr_info("[%u] Tx data: %*ph\n", hdr->owner, (int)len,
				data);
	}

	return rc;
}
EXPORT_SYMBOL(pmic_glink_write);

/**
 * pmic_glink_register_client() - Register a PMIC Glink client
 *
 * @dev: Device pointer of child device
 * @client_data: Client device data pointer
 *
 * Return: Valid client pointer upon success or ERR_PTR(-ERRNO)
 *
 * This function should be called by a client with a unique id, name and
 * callback function so that the pmic_glink driver can route the messages
 * to the client.
 */
struct pmic_glink_client *pmic_glink_register_client(struct device *dev,
			const struct pmic_glink_client_data *client_data)
{
	int rc;
	struct pmic_glink_dev *pgdev;
	struct pmic_glink_client *client;

	if (!dev || !dev->parent)
		return ERR_PTR(-ENODEV);

	if (!client_data || !client_data->id || client_data->id >= INT_MAX ||
	    !client_data->msg_cb || !client_data->name)
		return ERR_PTR(-EINVAL);

	pgdev = get_pmic_glink_from_dev(dev->parent);
	if (!pgdev) {
		pr_err("Failed to get pmic_glink_dev for %s\n",
			client_data->name);
		return ERR_PTR(-ENODEV);
	}

	down_read(&pgdev->rpdev_sem);
	if (READ_ONCE(pgdev->removing) || !atomic_read(&pgdev->state)) {
		up_read(&pgdev->rpdev_sem);
		pr_err("pmic_glink is not up\n");
		return ERR_PTR(-EPROBE_DEFER);
	}
	up_read(&pgdev->rpdev_sem);

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->name = kstrdup(client_data->name, GFP_KERNEL);
	if (!client->name) {
		kfree(client);
		return ERR_PTR(-ENOMEM);
	}

	mutex_init(&client->lock);
	client->id = client_data->id;
	client->msg_cb = client_data->msg_cb;
	client->priv = client_data->priv;
	client->pgdev = pgdev;
	client->state_cb = client_data->state_cb;

	mutex_lock(&pgdev->client_lock);
	rc = idr_alloc(&pgdev->client_idr, client, client->id, client->id + 1,
			GFP_KERNEL);
	if (rc < 0) {
		pr_err("Error in allocating idr for client %s, rc=%d\n",
			client->name, rc);
		mutex_unlock(&pgdev->client_lock);
		kfree(client->name);
		kfree(client);
		return ERR_PTR(rc);
	}

	if (client->state_cb) {
		INIT_LIST_HEAD(&client->node);
		list_add_tail_rcu(&client->node, &pgdev->client_dev_list);
	}
	mutex_unlock(&pgdev->client_lock);

	pmic_glink_dbg(pgdev, "Registered client %s\n", client->name);
	return client;
}
EXPORT_SYMBOL(pmic_glink_register_client);

int pmic_glink_unregister_client(struct pmic_glink_client *client)
{
	struct pmic_glink_dev *pgdev;

	if (!client || !client->pgdev)
		return -ENODEV;
	pgdev = client->pgdev;
	mutex_lock(&pgdev->client_lock);
	if (client->state_cb)
		list_del_rcu(&client->node);
	idr_remove(&pgdev->client_idr, client->id);
	mutex_unlock(&pgdev->client_lock);
	/* Callbacks may sleep, and no callback can retain freed client data. */
	synchronize_srcu(&pgdev->client_srcu);
	/* A running callback must validate/reject its reply before the record
	 * is classified as answered or orphaned.
	 */
	mutex_lock(&pgdev->client_lock);
	pmic_glink_orphan_requests(pgdev, client);
	mutex_unlock(&pgdev->client_lock);
	kfree(client->name);
	kfree(client);
	return 0;
}
EXPORT_SYMBOL(pmic_glink_unregister_client);

static void pmic_glink_rx_callback(struct pmic_glink_dev *pgdev,
				   struct pmic_glink_buf *pbuf)
{
	struct pmic_glink_hdr *hdr = (void *)pbuf->buf;
	struct pmic_glink_client *client;
	int idx;

	idx = srcu_read_lock(&pgdev->client_srcu);
	mutex_lock(&pgdev->client_lock);
	client = pmic_glink_filter_reply(pgdev, pbuf->buf, pbuf->len) ? NULL :
		idr_find(&pgdev->client_idr, hdr->owner);
	mutex_unlock(&pgdev->client_lock);
	if (client) {
		if (pgdev->log_enable &&
		    (pgdev->log_filter == hdr->owner || pgdev->log_filter == 65535))
			pr_info("[%u] Rx data: %*ph\n", hdr->owner, (int)pbuf->len, pbuf->buf);
		client->msg_cb(client->priv, pbuf->buf, pbuf->len);
	}
	srcu_read_unlock(&pgdev->client_srcu, idx);
}


static void pmic_glink_rx_work(struct work_struct *work)
{
	struct pmic_glink_dev *pgdev = container_of(work, struct pmic_glink_dev, rx_work);
	struct pmic_glink_buf *pbuf;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&pgdev->rx_lock, flags);
		if (list_empty(&pgdev->rx_list)) {
			spin_unlock_irqrestore(&pgdev->rx_lock, flags);
			break;
		}
		pbuf = list_first_entry(&pgdev->rx_list, struct pmic_glink_buf, node);
		list_del(&pbuf->node);
		spin_unlock_irqrestore(&pgdev->rx_lock, flags);
		if (READ_ONCE(pgdev->rx_enabled))
			pmic_glink_rx_callback(pgdev, pbuf);
		kfree(pbuf);
	}
}


static int pmic_glink_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
				int len, void *priv, u32 addr)
{
	struct pmic_glink_dev *pdev = dev_get_drvdata(&rpdev->dev);
	struct pmic_glink_buf *pbuf;
	unsigned long flags;

	if (!pdev || !data || len < (int)sizeof(struct pmic_glink_hdr)) {
		pr_err("Received length %d less than header size: %zu\n", len,
			sizeof(struct pmic_glink_hdr));
		return -EINVAL;
	}

	pbuf = kzalloc(struct_size(pbuf, buf, len), GFP_ATOMIC);
	if (!pbuf)
		return -ENOMEM;

	pbuf->len = len;
	memcpy(pbuf->buf, data, len);

	spin_lock_irqsave(&pdev->rx_lock, flags);
	if (!pdev->rx_enabled) {
		spin_unlock_irqrestore(&pdev->rx_lock, flags);
		kfree(pbuf);
		return -ENOTCONN;
	}
	list_add_tail(&pbuf->node, &pdev->rx_list);
	queue_work(pdev->rx_wq, &pdev->rx_work);
	spin_unlock_irqrestore(&pdev->rx_lock, flags);
	return 0;
}

static void pmic_glink_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct pmic_glink_dev *pgdev = dev_get_drvdata(&rpdev->dev);
	unsigned long flags;

	if (!pgdev)
		return;
	down_write(&pgdev->rpdev_sem);
	atomic_set(&pgdev->state, 0);
	pgdev->rpdev = NULL;
	up_write(&pgdev->rpdev_sem);
	spin_lock_irqsave(&pgdev->rx_lock, flags);
	pgdev->rx_enabled = false;
	spin_unlock_irqrestore(&pgdev->rx_lock, flags);
	cancel_work_sync(&pgdev->init_work);
	flush_workqueue(pgdev->rx_wq);
	pmic_glink_clear_requests(pgdev);
	pmic_glink_notify_clients(pgdev, PMIC_GLINK_STATE_DOWN);
	dev_set_drvdata(&rpdev->dev, NULL);
}


static int pmic_glink_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct pmic_glink_dev *pgdev = NULL;
	unsigned long flags;

	pgdev = get_pmic_glink_from_rpdev(rpdev);
	if (!pgdev) {
		pr_err("Failed to get pmic_glink_dev for %s\n", rpdev->id.name);
		return -EPROBE_DEFER;
	}

	down_write(&pgdev->rpdev_sem);
	if (pgdev->removing || pgdev->rpdev) {
		up_write(&pgdev->rpdev_sem);
		return -EBUSY;
	}
	spin_lock_irqsave(&pgdev->rx_lock, flags);
	pgdev->rx_enabled = true;
	spin_unlock_irqrestore(&pgdev->rx_lock, flags);
	dev_set_drvdata(&rpdev->dev, pgdev);
	pgdev->rpdev = rpdev;
	atomic_set(&pgdev->state, 1);
	up_write(&pgdev->rpdev_sem);
	schedule_work(&pgdev->init_work);
	pmic_glink_dbg(pgdev, "%s probed\n", rpdev->id.name);

	return 0;
}

static const struct rpmsg_device_id pmic_glink_rpmsg_match[] = {
	{ "PMIC_RTR_ADSP_APPS" },
	{ "PMIC_LOGS_ADSP_APPS" },
	{}
};

MODULE_DEVICE_TABLE(rpmsg, pmic_glink_rpmsg_match);

static struct rpmsg_driver pmic_glink_rpmsg_driver = {
	.id_table = pmic_glink_rpmsg_match,
	.probe = pmic_glink_rpmsg_probe,
	.remove = pmic_glink_rpmsg_remove,
	.callback = pmic_glink_rpmsg_callback,
	.drv = {
		.name = "pmic_glink_rpmsg",
	},
};

#ifdef CONFIG_DEBUG_FS
static void pmic_glink_add_debugfs(struct pmic_glink_dev *pgdev)
{
	struct dentry *dir;

	dir = debugfs_create_dir(dev_name(pgdev->dev), NULL);
	if (IS_ERR(dir)) {
		pr_err("Failed to create pmic_glink debugfs directory rc=%ld\n",
			PTR_ERR(dir));
		return;
	}

	pgdev->debugfs_dir = dir;
	debugfs_create_u32("filter", 0600, dir, &pgdev->log_filter);
	debugfs_create_bool("enable", 0600, dir, &pgdev->log_enable);
}
#else
static inline void pmic_glink_add_debugfs(struct pmic_glink_dev *pgdev)
{ }
#endif

static void pmic_glink_init_work(struct work_struct *work)
{
	struct pmic_glink_dev *pgdev = container_of(work, struct pmic_glink_dev,
					init_work);
	struct device *dev = pgdev->dev;
	int rc;

	if (READ_ONCE(pgdev->removing) || !atomic_read(&pgdev->state))
		return;

	if (atomic_read(&pgdev->pdr_state) == SERVREG_SERVICE_STATE_DOWN ||
	    atomic_read(&pgdev->prev_state) == QCOM_SSR_BEFORE_SHUTDOWN) {
		pmic_glink_notify_clients(pgdev, PMIC_GLINK_STATE_UP);
		atomic_set(&pgdev->pdr_state, SERVREG_SERVICE_STATE_UP);
		atomic_set(&pgdev->prev_state, QCOM_SSR_AFTER_POWERUP);
	}

	if (READ_ONCE(pgdev->removing) || !atomic_read(&pgdev->state) || pgdev->child_probed)
		return;

	rc = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (rc < 0)
		pr_err("Failed to create devices rc=%d\n", rc);
	else
		pgdev->child_probed = true;
}

static void pmic_glink_dev_add(struct pmic_glink_dev *pgdev)
{
	mutex_lock(&pmic_glink_dev_lock);
	list_add(&pgdev->dev_list, &pmic_glink_dev_list);
	mutex_unlock(&pmic_glink_dev_lock);
}

static void pmic_glink_dev_remove(struct pmic_glink_dev *pgdev)
{
	struct pmic_glink_dev *pos, *tmp;

	mutex_lock(&pmic_glink_dev_lock);
	list_for_each_entry_safe(pos, tmp, &pmic_glink_dev_list, dev_list) {
		if (pos == pgdev)
			list_del(&pgdev->dev_list);
	}
	mutex_unlock(&pmic_glink_dev_lock);
}

static int pmic_glink_probe(struct platform_device *pdev)
{
	struct pmic_glink_dev *pgdev;
	struct device *dev = &pdev->dev;
	struct pdr_service *service;
	int rc;

	pgdev = devm_kzalloc(dev, sizeof(*pgdev), GFP_KERNEL);
	if (!pgdev)
		return -ENOMEM;

	rc = of_property_read_string(dev->of_node, "qcom,pmic-glink-channel",
					&pgdev->channel_name);
	if (rc < 0) {
		pr_err("Error in reading qcom,pmic-glink-channel rc=%d\n", rc);
		return rc;
	}

	if (strlen(pgdev->channel_name) >= RPMSG_NAME_SIZE) {
		pr_err("pmic glink channel name %s exceeds length\n",
			pgdev->channel_name);
		return -EINVAL;
	}

	of_property_read_string(dev->of_node, "qcom,subsys-name",
				&pgdev->subsys_name);

	if (of_find_property(dev->of_node, "qcom,protection-domain", NULL)) {
		rc = of_property_read_string_index(dev->of_node,
				"qcom,protection-domain", 0,
				&pgdev->pdr_service_name);
		if (rc) {
			pr_err("Failed to get PDR service name rc=%d\n", rc);
			return rc;
		} else if (strlen(pgdev->pdr_service_name) >
				SERVREG_NAME_LENGTH) {
			pr_err("PDR service name %s is too long\n",
				pgdev->pdr_service_name);
			return -EINVAL;
		}

		rc = of_property_read_string_index(dev->of_node,
				"qcom,protection-domain", 1,
				&pgdev->pdr_path_name);
		if (rc) {
			pr_err("Failed to get PDR path name rc=%d\n", rc);
			return rc;
		} else if (strlen(pgdev->pdr_path_name) >
				SERVREG_NAME_LENGTH) {
			pr_err("PDR path name %s is too long\n",
				pgdev->pdr_path_name);
			return -EINVAL;
		}
	}

	pgdev->dev = dev;
	rc = init_srcu_struct(&pgdev->client_srcu);
	if (rc)
		return rc;

	pgdev->rx_wq = create_singlethread_workqueue("pmic_glink_rx");
	if (!pgdev->rx_wq) {
		pr_err("Failed to create pmic_glink_rx wq\n");
		cleanup_srcu_struct(&pgdev->client_srcu);
		return -ENOMEM;
	}

	init_rwsem(&pgdev->rpdev_sem);
	INIT_WORK(&pgdev->rx_work, pmic_glink_rx_work);
	INIT_WORK(&pgdev->init_work, pmic_glink_init_work);
	INIT_LIST_HEAD(&pgdev->client_dev_list);
	INIT_LIST_HEAD(&pgdev->reply_fences);
	INIT_LIST_HEAD(&pgdev->rx_list);
	INIT_LIST_HEAD(&pgdev->dev_list);
	spin_lock_init(&pgdev->rx_lock);
	mutex_init(&pgdev->client_lock);
	idr_init(&pgdev->client_idr);
	atomic_set(&pgdev->prev_state, QCOM_SSR_BEFORE_POWERUP);
	atomic_set(&pgdev->pdr_state, SERVREG_SERVICE_STATE_UNINIT);

	pgdev->ipc_log = ipc_log_context_create(NUM_LOG_PAGES,
						pgdev->channel_name, 0);
	if (!pgdev->ipc_log)
		pr_warn("Error in creating ipc_log\n");

	if (pgdev->subsys_name) {
		pgdev->ssr_nb.notifier_call = pmic_glink_ssr_notifier_cb;
		pgdev->subsys_handle = qcom_register_ssr_notifier(
							pgdev->subsys_name,
							&pgdev->ssr_nb);
		if (IS_ERR(pgdev->subsys_handle)) {
			rc = PTR_ERR(pgdev->subsys_handle);
			pr_err("Failed in qcom_register_ssr_notifier rc=%d\n",
				rc);
			goto error_subsys;
		}
	}

	if (pgdev->pdr_service_name) {
		pgdev->pdr_handle = pdr_handle_alloc(pmic_glink_pdr_notifier_cb,
						pgdev);
		if (IS_ERR(pgdev->pdr_handle)) {
			rc = PTR_ERR(pgdev->pdr_handle);
			if (rc != -EPROBE_DEFER)
				pr_err("Failed in pdr_handle_alloc rc=%d\n",
					rc);
			goto error_service;
		}

		service = pdr_add_lookup(pgdev->pdr_handle,
				pgdev->pdr_service_name, pgdev->pdr_path_name);
		if (IS_ERR(service) && PTR_ERR(service) != -EALREADY) {
			rc = PTR_ERR(service);
			pr_err("Failed in pdr_add_lookup rc=%d\n", rc);
			goto error_pdr;
		}

		pmic_glink_dbg(pgdev, "Registering PDR for path_name: %s service_name: %s\n",
			pgdev->pdr_path_name, pgdev->pdr_service_name);
	}

	dev_set_drvdata(dev, pgdev);
	pgdev->dev = dev;

	pmic_glink_dev_add(pgdev);
	pmic_glink_add_debugfs(pgdev);
	device_init_wakeup(pgdev->dev, true);

	pmic_glink_dbg(pgdev, "%s probed successfully\n", pgdev->channel_name);
	return 0;

error_pdr:
	pdr_handle_release(pgdev->pdr_handle);
error_service:
	if (pgdev->subsys_name)
		qcom_unregister_ssr_notifier(pgdev->subsys_handle, &pgdev->ssr_nb);
error_subsys:
	ipc_log_context_destroy(pgdev->ipc_log);
	idr_destroy(&pgdev->client_idr);
	destroy_workqueue(pgdev->rx_wq);
	cleanup_srcu_struct(&pgdev->client_srcu);
	return rc;
}

static void pmic_glink_remove(struct platform_device *pdev)
{
	struct pmic_glink_dev *pgdev = dev_get_drvdata(&pdev->dev);

	pmic_glink_dev_remove(pgdev);
	down_write(&pgdev->rpdev_sem);
	pgdev->removing = true;
	atomic_set(&pgdev->state, 0);
	up_write(&pgdev->rpdev_sem);
	if (pgdev->pdr_service_name)
		pdr_handle_release(pgdev->pdr_handle);
	if (pgdev->subsys_name)
		qcom_unregister_ssr_notifier(pgdev->subsys_handle, &pgdev->ssr_nb);
	cancel_work_sync(&pgdev->init_work);
	of_platform_depopulate(&pdev->dev);
	flush_workqueue(pgdev->rx_wq);
	destroy_workqueue(pgdev->rx_wq);
	pmic_glink_clear_requests(pgdev);
	debugfs_remove_recursive(pgdev->debugfs_dir);
	device_init_wakeup(pgdev->dev, false);
	idr_destroy(&pgdev->client_idr);
	cleanup_srcu_struct(&pgdev->client_srcu);
	ipc_log_context_destroy(pgdev->ipc_log);
}


static const struct of_device_id pmic_glink_of_match[] = {
	{ .compatible = "qcom,qti-pmic-glink" },
	{}
};
MODULE_DEVICE_TABLE(of, pmic_glink_of_match);

static struct platform_driver pmic_glink_driver = {
	.probe = pmic_glink_probe,
	.remove = pmic_glink_remove,
	.driver = {
		.name = "pmic_glink",
		.of_match_table = pmic_glink_of_match,
	},
};

static int __init pmic_glink_init(void)
{
	int rc;

	rc = platform_driver_register(&pmic_glink_driver);
	if (rc < 0)
		return rc;

	rc = register_rpmsg_driver(&pmic_glink_rpmsg_driver);
	if (rc)
		platform_driver_unregister(&pmic_glink_driver);
	return rc;
}
module_init(pmic_glink_init);

static void __exit pmic_glink_exit(void)
{
	unregister_rpmsg_driver(&pmic_glink_rpmsg_driver);
	platform_driver_unregister(&pmic_glink_driver);
}
module_exit(pmic_glink_exit);

MODULE_DESCRIPTION("QTI PMIC Glink driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("qcom,qti-pmic-glink");
