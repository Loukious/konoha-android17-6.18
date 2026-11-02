// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"altmode-glink: %s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/ipc_logging.h>
#include <linux/ktime.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/altmode-glink.h>
#include <linux/soc/qcom/qti_pmic_glink.h>

#define MSG_OWNER_USBC_PAN	32780
#define MSG_TYPE_REQ_RESP	1
#define MSG_TYPE_NOTIFY		2

#define NOTIFY_PAYLOAD_SIZE	16
#define USBC_WRITE_BUFFER_SIZE	8

#define USBC_CMD_WRITE_REQ	0x15
#define USBC_NOTIFY_IND		0x16

#define MAX_NUM_PORTS		4

#define NUM_LOG_PAGES		10

#define IDR_KEY_GEN(svid, ind)	(((svid) << 8) | (ind))
#define IDR_KEY(client)		\
	IDR_KEY_GEN((client)->data.svid, (client)->port_index)

#define altmode_dbg(fmt, ...) \
	do { \
		ipc_log_string(amdev->ipc_log, fmt, ##__VA_ARGS__); \
		pr_debug(fmt, ##__VA_ARGS__); \
	} while (0)

struct usbc_notify_ind_msg {
	struct pmic_glink_hdr	hdr;
	u8			payload[NOTIFY_PAYLOAD_SIZE];
	u32			reserved;
};

struct usbc_write_buffer_req_msg {
	struct pmic_glink_hdr	hdr;
	u8			buf[USBC_WRITE_BUFFER_SIZE];
	u32			reserved;
};

/**
 * struct altmode_dev
 *	Definition of an altmode device.
 *
 * @dev:		Altmode parent device for all client devices
 * @pgclient:		PMIC GLINK client to talk to remote subsystem
 * @client_idr:		idr list for altmode clients
 * @client_lock:	mutex protecting changes to client_idr
 * @d_node:		Linked list node to string together multiple amdev's
 * @client_list:	Linked list head keeping track of this device's clients
 * @pan_en_sent:	Flag to ensure PAN Enable msg is sent only once
 * @send_pan_en_work:	To schedule the sending of the PAN Enable message
 * @send_pan_ack_work:	To schedule the sending of the PAN Ack message
 * @debugfs_dir:	Dentry for debugfs directory "altmode"
 * @response_received:	To detect remote subsystem response failures
 * @ack_port_index:	Port index to ack for a nonexistent client
 */
struct altmode_dev {
	struct kref ref;
	struct mutex write_lock;
	bool removing;
	bool response_pending;
	bool resync_required;
	atomic_t link_generation;
	unsigned long ack_ports;
	void *ipc_log;
	struct device			*dev;
	struct pmic_glink_client	*pgclient;
	struct idr			client_idr;
	struct mutex			client_lock;
	struct list_head		d_node;
	struct list_head		client_list;
	atomic_t			pan_en_sent;
	struct delayed_work		send_pan_en_work;
	struct delayed_work		send_pan_ack_work;
	struct dentry			*debugfs_dir;
	struct completion		response_received;
	u8				ack_port_index;
};

/**
 * struct altmode_client
 *	Definition of a client of an altmode device.
 *
 * @amdev:		Parent altmode device of this client
 * @data:		Supplied by client driver during registration
 * @c_node:		Linked list node for parent altmode device's client list
 * @port_index:		Type-C port index assigned by remote subystem
 *
 * Note: The following members are for internal use only, clients should not
 *	 use them.
 *
 * @client_cb_work:	Work to run client callback
 * @msg:		Latest notify msg stashed to be retrieved in cb_work
 */
struct altmode_message {
	struct list_head node;
	u8 payload[NOTIFY_PAYLOAD_SIZE];
};

struct altmode_client {
	struct kref ref;
	struct list_head messages;
	bool closing;
	struct altmode_dev		*amdev;
	struct altmode_client_data	data;
	struct list_head		c_node;
	u8				port_index;
	struct work_struct		client_cb_work;
	u8				msg[NOTIFY_PAYLOAD_SIZE];
};

/**
 * struct probe_notify_node
 *	Linked list node to keep track of altmode clients who, by design,
 *	register with the altmode framework before altmode probes.
 *
 * @node:		Linked list node for probe_notify_list
 * @amdev_node:		device_node of the altmode device of the client
 * @cb:			Client's probe completion callback function
 * @priv:		Pointer to client's driver data struct
 */
struct probe_notify_node {
	struct list_head		node;
	struct device_node		*amdev_node;
	void				(*cb)(void *priv);
	void				*priv;
};

/**
 * struct list_head probe_notify_list
 *	List of altmode clients that register to get notified upon probe
 *	completion.
 */
static LIST_HEAD(probe_notify_list);
static DEFINE_MUTEX(notify_lock);

static void altmode_send_pan_ack(struct work_struct *work);
static LIST_HEAD(altmode_devices);
static DEFINE_MUTEX(altmode_registry_lock);

static void altmode_release(struct kref *ref)
{
	struct altmode_dev *amdev = container_of(ref, struct altmode_dev, ref);

	idr_destroy(&amdev->client_idr);
	put_device(amdev->dev);
	kfree(amdev);
}

static void altmode_client_release(struct kref *ref)
{
	struct altmode_client *client = container_of(ref, struct altmode_client, ref);
	struct altmode_message *msg, *tmp;

	list_for_each_entry_safe(msg, tmp, &client->messages, node) {
		list_del(&msg->node);
		kfree(msg);
	}
	kfree(client->data.name);
	kref_put(&client->amdev->ref, altmode_release);
	kfree(client);
}

/* Returns an owned reference; device drvdata is never read without a guard. */
static struct altmode_dev *to_altmode_device(struct device_node *amdev_node)
{
	struct altmode_dev *amdev, *found = NULL;

	mutex_lock(&altmode_registry_lock);
	list_for_each_entry(amdev, &altmode_devices, d_node) {
		if (amdev->dev->of_node == amdev_node && !amdev->removing) {
			kref_get(&amdev->ref);
			found = amdev;
			break;
		}
	}
	mutex_unlock(&altmode_registry_lock);
	return found;
}

#define ALTMODE_WAIT_MS	1000
static int altmode_write(struct altmode_dev *amdev, void *data, size_t len)
{
	int rc, generation;

	mutex_lock(&amdev->write_lock);
	if (READ_ONCE(amdev->removing)) {
		rc = -ENODEV;
		goto unlock;
	}
	if (READ_ONCE(amdev->resync_required)) {
		rc = -ENOTCONN;
		goto unlock;
	}
	reinit_completion(&amdev->response_received);
	generation = atomic_read(&amdev->link_generation);
	WRITE_ONCE(amdev->response_pending, true);
	rc = pmic_glink_write(amdev->pgclient, data, len);
	if (!rc) {
		if (!wait_for_completion_timeout(&amdev->response_received,
				msecs_to_jiffies(ALTMODE_WAIT_MS))) {
			/* The wire format has no transaction ID: a late response cannot
			 * be reused by another command before a new GLINK session.
			 */
			WRITE_ONCE(amdev->resync_required, true);
			rc = -ETIMEDOUT;
		}
	}
	WRITE_ONCE(amdev->response_pending, false);
	if (!rc && generation != atomic_read(&amdev->link_generation))
		rc = -ENOTCONN;
	if (READ_ONCE(amdev->removing))
		rc = -ENODEV;
unlock:
	mutex_unlock(&amdev->write_lock);
	return rc;
}

static int __altmode_send_data(struct altmode_dev *amdev, void *data,
			       size_t len)
{
	struct usbc_write_buffer_req_msg msg = { };

	if ((!data && len) || len > sizeof(msg.buf)) {
		pr_err("len %zu exceeds msg buf's size: %d\n",
				len, USBC_WRITE_BUFFER_SIZE);
		return -EINVAL;
	}

	msg.hdr.owner = MSG_OWNER_USBC_PAN;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = USBC_CMD_WRITE_REQ;

	if (len)
		memcpy(msg.buf, data, len);

	return altmode_write(amdev, &msg, sizeof(msg));
}

/**
 * altmode_register_notifier()
 *	Register to be notified when altmode device probes.
 *
 * @client_dev:		Client device pointer
 * @cb:			Callback function to execute when altmode device of
 *			interest probes successfully
 * @priv:		Client's private data which is passed back when cb() is
 *			called
 *
 * Returns:		0 upon success, negative upon errors.
 */
int altmode_register_notifier(struct device *client_dev, void (*cb)(void *),
			      void *priv)
{
	int rc;
	struct probe_notify_node *notify_node;
	struct probe_notify_node *pos;
	struct device_node *amdev_node;
	struct of_phandle_args pargs;
	struct altmode_dev *amdev;

	if (!client_dev || !cb || !priv)
		return -EINVAL;

	rc = of_parse_phandle_with_args(client_dev->of_node,
			"qcom,altmode-dev", "#altmode-cells", 0, &pargs);
	if (rc) {
		dev_err(client_dev, "Error parsing qcom,altmode-dev property: %d\n",
				rc);
		return rc;
	}

	amdev_node = pargs.np;
	if (pargs.args_count != 1 || pargs.args[0] >= MAX_NUM_PORTS) {
		of_node_put(amdev_node);
		return -EINVAL;
	}

	mutex_lock(&notify_lock);
	list_for_each_entry(pos, &probe_notify_list, node) {
		if (pos->amdev_node == amdev_node && pos->priv == priv) {
			mutex_unlock(&notify_lock);
			of_node_put(amdev_node);
			return -EEXIST;
		}
	}
	amdev = to_altmode_device(amdev_node);
	if (amdev) {
		/*
		 * If altmode device has probed already, notify
		 * immediately.
		 */
		of_node_put(amdev_node);
		cb(priv);
		kref_put(&amdev->ref, altmode_release);
	} else {
		notify_node = kzalloc(sizeof(*notify_node), GFP_KERNEL);
		if (!notify_node) {
			mutex_unlock(&notify_lock);
			of_node_put(amdev_node);
			return -ENOMEM;
		}

		notify_node->cb = cb;
		notify_node->priv = priv;
		notify_node->amdev_node = amdev_node;

		list_add(&notify_node->node, &probe_notify_list);
	}
	mutex_unlock(&notify_lock);

	return 0;
}
EXPORT_SYMBOL(altmode_register_notifier);

/**
 * altmode_deregister_notifier()
 *	Deregister probe completion notifier.
 *
 * @client_dev:		Client device pointer
 * @priv:		Client's private data
 *
 * Returns:		0 upon success, negative upon errors.
 */
int altmode_deregister_notifier(struct device *client_dev, void *priv)
{
	int rc;
	struct device_node *amdev_node;
	struct probe_notify_node *pos, *tmp;
	struct of_phandle_args pargs;

	if (!client_dev)
		return -EINVAL;

	rc = of_parse_phandle_with_args(client_dev->of_node,
			"qcom,altmode-dev", "#altmode-cells", 0, &pargs);
	if (rc) {
		dev_err(client_dev, "Error parsing qcom,altmode-dev property: %d\n",
				rc);
		return rc;
	}

	amdev_node = pargs.np;

	mutex_lock(&notify_lock);
	list_for_each_entry_safe(pos, tmp, &probe_notify_list, node) {
		if (pos->amdev_node == amdev_node && pos->priv == priv) {
			of_node_put(pos->amdev_node);
			list_del(&pos->node);
			kfree(pos);
			break;
		}
	}
	mutex_unlock(&notify_lock);

	of_node_put(amdev_node);

	return 0;
}
EXPORT_SYMBOL(altmode_deregister_notifier);

/**
 * altmode_send_data()
 *	Send data from altmode client to remote subsystem.
 *
 * @client:		Parent altmode device of this client
 * @data:		Data to be sent
 * @len:		Length in bytes of the data to be sent
 *
 * Returns:		0 upon success, -EINVAL if len exceeds message buffer's
 *			capacity, and other negative error codes as appropriate.
 */
int altmode_send_data(struct altmode_client *client, void *data, size_t len)
{
	if (IS_ERR_OR_NULL(client) || READ_ONCE(client->closing))
		return -ENODEV;
	return __altmode_send_data(client->amdev, data, len);
}
EXPORT_SYMBOL(altmode_send_data);

static void client_cb_work(struct work_struct *work)
{
	struct altmode_client *client = container_of(work,
			struct altmode_client, client_cb_work);
	struct altmode_dev *amdev = client->amdev;
	struct altmode_message *msg;

	for (;;) {
		mutex_lock(&amdev->client_lock);
		if (client->closing || READ_ONCE(amdev->removing) ||
		    list_empty(&client->messages)) {
			mutex_unlock(&amdev->client_lock);
			return;
		}
		msg = list_first_entry(&client->messages, struct altmode_message, node);
		list_del(&msg->node);
		mutex_unlock(&amdev->client_lock);
		client->data.callback(client->data.priv, msg->payload,
				     sizeof(msg->payload));
		kfree(msg);
	}
}

/**
 * altmode_register_client()
 *	Register with altmode to receive PMIC GLINK messages from remote
 *	subsystem.
 *
 * @client_dev:		Client device pointer
 * @client_data:	Details identifying altmode client uniquely
 *
 * Returns:		Valid altmode client pointer upon success, ERR_PTRs
 *			upon errors.
 *
 * Notes:		client_data should contain a unique SVID.
 */
struct altmode_client *altmode_register_client(struct device *client_dev,
		const struct altmode_client_data *client_data)
{
	int rc, key;
	struct altmode_dev *amdev;
	struct of_phandle_args pargs;
	struct device_node *amdev_node;
	struct altmode_client *amclient;

	if (!client_dev || !client_data)
		return ERR_PTR(-EINVAL);

	if (!client_data->name || !client_data->priv || !client_data->callback
			|| !client_data->svid)
		return ERR_PTR(-EINVAL);

	rc = of_parse_phandle_with_args(client_dev->of_node,
			"qcom,altmode-dev", "#altmode-cells", 0, &pargs);
	if (rc) {
		dev_err(client_dev, "Error parsing qcom,altmode-dev property: %d\n",
				rc);
		return ERR_PTR(rc);
	}

	if (pargs.args_count != 1) {
		dev_err(client_dev, "Error in port_index specification\n");
		of_node_put(pargs.np);
		return ERR_PTR(-EINVAL);
	}

	if (pargs.args[0] >= MAX_NUM_PORTS) {
		dev_err(client_dev, "Invalid port_index: %d, max is %d\n",
				pargs.args[0], MAX_NUM_PORTS - 1);
		of_node_put(pargs.np);
		return ERR_PTR(-EINVAL);
	}

	amdev_node = pargs.np;

	amdev = to_altmode_device(amdev_node);
	of_node_put(amdev_node);
	if (!amdev)
		return ERR_PTR(-EPROBE_DEFER);

	amclient = kzalloc(sizeof(*amclient), GFP_KERNEL);
	if (!amclient) {
		kref_put(&amdev->ref, altmode_release);
		return ERR_PTR(-ENOMEM);
	}
	kref_init(&amclient->ref);
	INIT_LIST_HEAD(&amclient->messages);

	amclient->amdev = amdev;
	amclient->port_index = pargs.args[0];
	amclient->data.svid = client_data->svid;
	amclient->data.priv = client_data->priv;
	amclient->data.callback = client_data->callback;
	INIT_WORK(&amclient->client_cb_work, client_cb_work);
	amclient->data.name = kstrdup(client_data->name, GFP_KERNEL);
	if (!amclient->data.name) {
		kref_put(&amclient->ref, altmode_client_release);
		return ERR_PTR(-ENOMEM);
	}

	mutex_lock(&amdev->client_lock);
	if (READ_ONCE(amdev->removing)) {
		rc = -ENODEV;
		goto free_client;
	}
	key = IDR_KEY(amclient);
	rc = idr_alloc(&amdev->client_idr, amclient, key, key + 1, GFP_KERNEL);
	if (rc < 0) {
		pr_err("Error in allocating idr for client %s: %d\n",
				client_data->name, rc);
		goto free_client;
	}

	list_add(&amclient->c_node, &amdev->client_list);
	if (!atomic_read(&amdev->pan_en_sent))
		schedule_delayed_work(&amdev->send_pan_en_work,
				msecs_to_jiffies(20));

	mutex_unlock(&amdev->client_lock);
	return amclient;
free_client:
	mutex_unlock(&amdev->client_lock);
	kref_put(&amclient->ref, altmode_client_release);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL(altmode_register_client);

/**
 * altmode_deregister_client()
 *	Deregister altmode client to stop receiving PMIC GLINK messages
 *	specific to its SVID from remote subsystem.
 *
 * @client:		Client returned by altmode_register_client()
 *
 * Returns:		0 upon success, negative upon errors.
 *
 * Notes:		This does not stop the transmission of the messages by
 *			the remote subsystem - only the reception of them by
 *			the client.
 */
int altmode_deregister_client(struct altmode_client *client)
{
	struct altmode_dev *amdev;

	if (IS_ERR_OR_NULL(client))
		return -ENODEV;
	amdev = client->amdev;
	mutex_lock(&amdev->client_lock);
	client->closing = true;
	if (idr_find(&amdev->client_idr, IDR_KEY(client)) == client)
		idr_remove(&amdev->client_idr, IDR_KEY(client));
	list_del_init(&client->c_node);
	mutex_unlock(&amdev->client_lock);
	/* Withdraw from lookup before draining so transport cannot requeue it. */
	cancel_work_sync(&client->client_cb_work);
	kref_put(&client->ref, altmode_client_release);
	return 0;
}
EXPORT_SYMBOL(altmode_deregister_client);

static void altmode_send_pan_en(struct work_struct *work)
{
	int rc, generation;
	u32 enable_msg = ALTMODE_PAN_EN;
	struct altmode_dev *amdev = container_of(work, struct altmode_dev,
			send_pan_en_work.work);

	generation = atomic_read(&amdev->link_generation);
	rc = __altmode_send_data(amdev, &enable_msg, sizeof(enable_msg));
	if (rc < 0) {
		pr_err("Failed to send PAN EN: %d\n", rc);
		return;
	}

	mutex_lock(&amdev->client_lock);
	if (!READ_ONCE(amdev->removing) && generation == atomic_read(&amdev->link_generation))
		atomic_set(&amdev->pan_en_sent, 1);
	mutex_unlock(&amdev->client_lock);
	altmode_dbg("Sent PAN EN\n");
}

static int altmode_send_ack(struct altmode_dev *amdev, u8 port_index)
{
	int rc;
	struct altmode_pan_ack_msg ack = { };

	if (port_index >= MAX_NUM_PORTS)
		return -EINVAL;
	ack.cmd_type = ALTMODE_PAN_ACK;
	ack.port_index = port_index;

	rc = __altmode_send_data(amdev, &ack, sizeof(ack));
	if (rc < 0) {
		pr_err("port %u: Failed to send PAN ACK: %d\n", port_index, rc);
		return rc;
	}

	altmode_dbg("port %u: Sent PAN ACK\n", port_index);

	return rc;
}

static void altmode_state_cb(void *priv, enum pmic_glink_state state)
{
	struct altmode_dev *amdev = priv;

	mutex_lock(&amdev->client_lock);
	if (READ_ONCE(amdev->removing))
		goto unlock;
	if (state == PMIC_GLINK_STATE_DOWN) {
		atomic_inc(&amdev->link_generation);
		atomic_set(&amdev->pan_en_sent, 0);
		WRITE_ONCE(amdev->resync_required, true);
		complete_all(&amdev->response_received);
	} else if (state == PMIC_GLINK_STATE_UP) {
		WRITE_ONCE(amdev->resync_required, false);
		if (!list_empty(&amdev->client_list))
			schedule_delayed_work(&amdev->send_pan_en_work, msecs_to_jiffies(20));
	}
unlock:
	mutex_unlock(&amdev->client_lock);
}

static void altmode_send_pan_ack(struct work_struct *work)
{
	struct altmode_dev *amdev = container_of(work, struct altmode_dev,
			send_pan_ack_work.work);
	unsigned long ports;
	unsigned int port;

	mutex_lock(&amdev->client_lock);
	ports = amdev->ack_ports;
	amdev->ack_ports = 0;
	mutex_unlock(&amdev->client_lock);
	for_each_set_bit(port, &ports, MAX_NUM_PORTS)
		altmode_send_ack(amdev, port);
}

#define USBC_NOTIFY_IND_MASK	GENMASK(7, 0)
#define GET_OP(opcode)		(opcode & USBC_NOTIFY_IND_MASK)
#define GET_SVID(opcode)	(opcode >> 16)

static int altmode_callback(void *priv, void *data, size_t len)
{
	struct altmode_dev *amdev = priv;
	struct pmic_glink_hdr *hdr = data;
	struct usbc_notify_ind_msg *notify_msg = data;
	struct altmode_client *client;
	struct altmode_message *msg;
	u16 op, svid;
	u8 port;
	int ret = 0;

	if (!data || len < sizeof(*hdr) || hdr->owner != MSG_OWNER_USBC_PAN)
		return -EINVAL;
	op = GET_OP(hdr->opcode);
	svid = GET_SVID(hdr->opcode);
	if (op == USBC_CMD_WRITE_REQ) {
		if (hdr->opcode != USBC_CMD_WRITE_REQ || hdr->type != MSG_TYPE_REQ_RESP)
			return -EINVAL;
		if (READ_ONCE(amdev->response_pending))
			complete(&amdev->response_received);
		return 0;
	}
	if (op != USBC_NOTIFY_IND)
		return -EOPNOTSUPP;
	if (hdr->type != MSG_TYPE_REQ_RESP && hdr->type != MSG_TYPE_NOTIFY)
		return -EINVAL;
	if (len != sizeof(*notify_msg))
		return -EINVAL;
	port = notify_msg->payload[0];
	if (port >= MAX_NUM_PORTS || !svid)
		return -EINVAL;
	mutex_lock(&amdev->client_lock);
	if (READ_ONCE(amdev->removing)) {
		ret = -ENODEV;
		goto unlock;
	}
	client = idr_find(&amdev->client_idr, IDR_KEY_GEN(svid, port));
	if (!client) {
		set_bit(port, &amdev->ack_ports);
		schedule_delayed_work(&amdev->send_pan_ack_work, msecs_to_jiffies(20));
		goto unlock;
	}
	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto unlock;
	}
	memcpy(msg->payload, notify_msg->payload, sizeof(msg->payload));
	list_add_tail(&msg->node, &client->messages);
	schedule_work(&client->client_cb_work);
	/* Never wait here for a callback that may itself wait for a GLINK reply. */
unlock:
	mutex_unlock(&amdev->client_lock);
	return ret;
}

static void altmode_notify_clients(struct altmode_dev *amdev)
{
	struct probe_notify_node *pos, *tmp;

	/* Also drains against deregistration. Callbacks may register a client,
	 * but must not recursively change this probe notifier list.
	 */
	mutex_lock(&notify_lock);
	list_for_each_entry_safe(pos, tmp, &probe_notify_list, node) {
		if (pos->amdev_node != amdev->dev->of_node)
			continue;
		pos->cb(pos->priv);
		of_node_put(pos->amdev_node);
		list_del(&pos->node);
		kfree(pos);
	}
	mutex_unlock(&notify_lock);
}

#ifdef CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG
static int pan_en_write(void *data, u64 val)
{
	struct altmode_dev *amdev = data;

	schedule_delayed_work(&amdev->send_pan_en_work,
				msecs_to_jiffies(20));
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(pan_en_fops, NULL, pan_en_write, "%llu\n");

static int send_ack_write(void *data, u64 val)
{
	int rc;
	struct altmode_dev *amdev = data;
	struct altmode_pan_ack_msg ack = { };

	if (val >= MAX_NUM_PORTS)
		return -EINVAL;

	ack.cmd_type = ALTMODE_PAN_ACK;
	ack.port_index = val;

	rc = __altmode_send_data(amdev, &ack, sizeof(ack));
	if (rc < 0) {
		dev_err(amdev->dev, "port %llu: Failed sending PAN ACK: %d\n",
				val, rc);
		return rc;
	}

	dev_dbg(amdev->dev, "port %llu: Sent PAN ACK via debugfs\n", val);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(send_ack_fops, NULL, send_ack_write, "%llu\n");

static int altmode_setup_debugfs(struct altmode_dev *amdev)
{
	int rc;
	struct dentry *am_dir, *file;

	am_dir = debugfs_create_dir("altmode", NULL);
	if (IS_ERR(am_dir)) {
		rc = PTR_ERR(am_dir);
		dev_err(amdev->dev, "Failed to create altmode directory: %d\n",
				rc);
		return rc;
	}

	file = debugfs_create_file("send_pan_en", 0200, am_dir, amdev,
					  &pan_en_fops);
	if (IS_ERR(file)) {
		rc = PTR_ERR(file);
		dev_err(amdev->dev, "Failed to create send_pan_en: %d\n", rc);
		goto error;
	}

	file = debugfs_create_file("send_pan_ack", 0200, am_dir,
					  amdev, &send_ack_fops);
	if (IS_ERR(file)) {
		rc = PTR_ERR(file);
		dev_err(amdev->dev, "Failed to create send_pan_ack: %d\n", rc);
		goto error;
	}

	amdev->debugfs_dir = am_dir;

	return 0;

error:
	debugfs_remove_recursive(am_dir);
	return rc;
}
#else
static int altmode_setup_debugfs(struct altmode_dev *amdev)
{
	return 0;
}
#endif

static int altmode_probe(struct platform_device *pdev)
{
	struct altmode_dev *amdev;
	struct pmic_glink_client_data pgclient_data = { };
	struct device *dev = &pdev->dev;
	int rc;

	amdev = kzalloc(sizeof(*amdev), GFP_KERNEL);
	if (!amdev)
		return -ENOMEM;
	kref_init(&amdev->ref);
	amdev->dev = get_device(dev);
	mutex_init(&amdev->client_lock);
	mutex_init(&amdev->write_lock);
	idr_init(&amdev->client_idr);
	init_completion(&amdev->response_received);
	INIT_DELAYED_WORK(&amdev->send_pan_en_work, altmode_send_pan_en);
	INIT_DELAYED_WORK(&amdev->send_pan_ack_work, altmode_send_pan_ack);
	INIT_LIST_HEAD(&amdev->d_node);
	INIT_LIST_HEAD(&amdev->client_list);
	amdev->ipc_log = ipc_log_context_create(NUM_LOG_PAGES, "altmode", 0);
	if (IS_ERR(amdev->ipc_log))
		amdev->ipc_log = NULL;
	pgclient_data.id = MSG_OWNER_USBC_PAN;
	pgclient_data.name = "altmode";
	pgclient_data.msg_cb = altmode_callback;
	pgclient_data.priv = amdev;
	pgclient_data.state_cb = altmode_state_cb;
	amdev->pgclient = pmic_glink_register_client(dev, &pgclient_data);
	if (IS_ERR(amdev->pgclient)) {
		rc = PTR_ERR(amdev->pgclient);
		goto release;
	}
	rc = altmode_setup_debugfs(amdev);
	if (rc)
		goto unregister;
	platform_set_drvdata(pdev, amdev);
	mutex_lock(&altmode_registry_lock);
	list_add_tail(&amdev->d_node, &altmode_devices);
	mutex_unlock(&altmode_registry_lock);
	altmode_notify_clients(amdev);
	return 0;
unregister:
	WRITE_ONCE(amdev->removing, true);
	pmic_glink_unregister_client(amdev->pgclient);
	cancel_delayed_work_sync(&amdev->send_pan_en_work);
	cancel_delayed_work_sync(&amdev->send_pan_ack_work);
release:
	ipc_log_context_destroy(amdev->ipc_log);
	kref_put(&amdev->ref, altmode_release);
	return rc;
}

static void altmode_remove(struct platform_device *pdev)
{
	struct altmode_dev *amdev = platform_get_drvdata(pdev);
	struct altmode_client *client;
	int key = 0;

	mutex_lock(&altmode_registry_lock);
	WRITE_ONCE(amdev->removing, true);
	list_del_init(&amdev->d_node);
	mutex_unlock(&altmode_registry_lock);
	complete_all(&amdev->response_received);
	debugfs_remove_recursive(amdev->debugfs_dir);
	/* Drain transport users before freeing pgclient; unregister drains RX. */
	mutex_lock(&amdev->write_lock);
	pmic_glink_unregister_client(amdev->pgclient);
	mutex_unlock(&amdev->write_lock);
	cancel_delayed_work_sync(&amdev->send_pan_en_work);
	cancel_delayed_work_sync(&amdev->send_pan_ack_work);
	for (;;) {
		mutex_lock(&amdev->client_lock);
		client = idr_get_next(&amdev->client_idr, &key);
		if (client) {
			kref_get(&client->ref);
			idr_remove(&amdev->client_idr, key);
		}
		mutex_unlock(&amdev->client_lock);
		if (!client)
			break;
		cancel_work_sync(&client->client_cb_work);
		kref_put(&client->ref, altmode_client_release);
		key++;
	}
	/* Callers retain offline client handles until explicit deregistration. */
	ipc_log_context_destroy(amdev->ipc_log);
	amdev->ipc_log = NULL;
	platform_set_drvdata(pdev, NULL);
	kref_put(&amdev->ref, altmode_release);
}

static const struct of_device_id altmode_match_table[] = {
	{ .compatible = "qcom,altmode-glink" },
	{},
};

static struct platform_driver altmode_driver = {
	.driver	= {
		.name = "altmode-glink",
		.of_match_table = altmode_match_table,
	},
	.probe	= altmode_probe,
	.remove	= altmode_remove,
};
module_platform_driver(altmode_driver);

MODULE_DESCRIPTION("QTI Type-C Alt Mode over GLINK");
MODULE_LICENSE("GPL");
