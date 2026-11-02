#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/srcu.h>
#include <linux/spinlock.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/soc/qcom/qti_pmic_glink.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_adsp_glink.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_adsp_glink"
#endif

#define MCA_ADSP_GLINK_OWNER 0x800A
#define MCA_ADSP_GLINK_QBG_OWNER 0x8009
#define MCA_ADSP_GLINK_MSG_REQ 1 /* hdr.type: request/response */
#define MCA_ADSP_GLINK_MSG_NOTIFY 2 /* hdr.type: async notification */
#define MCA_ADSP_GLINK_OPCODE_READ 1
#define MCA_ADSP_GLINK_OPCODE_WRITE 2
#define MCA_ADSP_PROP_DATA_LEN 256
#define MCA_ADSP_GLINK_TIMEOUT_MS 250

struct mca_adsp_glink_req_msg {
	struct pmic_glink_hdr hdr;
	u32 property_id;
	u32 seq;
	u8 data[MCA_ADSP_PROP_DATA_LEN];
} __packed;

struct mca_adsp_glink_resp_msg {
	struct pmic_glink_hdr hdr;
	u32 property_id;
	u32 retcode;
	u32 seq;
	u8 data[MCA_ADSP_PROP_DATA_LEN];
} __packed;

struct mca_adsp_glink_notify_msg {
	struct pmic_glink_hdr hdr;
	u32 property_id;
	u8 data[MCA_ADSP_PROP_DATA_LEN];
} __packed;

struct mca_adsp_glink_dev {
	struct device *dev;
	struct pmic_glink_client *client;
	struct pmic_glink_client *qbg_client;
	struct mutex rw_lock;
	spinlock_t response_lock;
	struct completion ack;
	bool online;
	bool pending;
	u32 owner;
	u32 property_id;
	u32 opcode;
	u32 seq;
	int retcode;
	u8 read_buf[MCA_ADSP_PROP_DATA_LEN];
	struct workqueue_struct *wq;
	struct work_struct notify_work;
	struct mutex notify_lock;
	struct list_head notifications;
};

struct mca_adsp_ops_node {
	struct list_head node;
	void *priv;
	struct mca_adsp_glink_ops *ops;
};

struct mca_adsp_event {
	struct list_head node;
	bool state_event;
	enum pmic_glink_state state;
	struct mca_adsp_glink_notify_msg msg;
};

static struct mca_adsp_glink_dev __rcu *g_mca_adsp_glink;
static DEFINE_MUTEX(mca_provider_lock);
static DEFINE_MUTEX(mca_ops_lock);
DEFINE_STATIC_SRCU(mca_provider_srcu);
DEFINE_STATIC_SRCU(mca_ops_srcu);
static LIST_HEAD(g_mca_adsp_glink_ops_head);
static LIST_HEAD(g_mca_adsp_glink_qbg_ops_head);

static int mca_adsp_glink_xfer(u32 owner, int property_id, void *value,
			       size_t size, bool write)
{
	struct mca_adsp_glink_req_msg msg = {};
	struct mca_adsp_glink_dev *mca;
	struct pmic_glink_client *client;
	unsigned long flags;
	int idx, ret;

	if (!value || !size || size > MCA_ADSP_PROP_DATA_LEN || property_id < 0)
		return -EINVAL;
	idx = srcu_read_lock(&mca_provider_srcu);
	mca = srcu_dereference(g_mca_adsp_glink, &mca_provider_srcu);
	if (!mca) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&mca->rw_lock);
	spin_lock_irqsave(&mca->response_lock, flags);
	if (!mca->online) {
		spin_unlock_irqrestore(&mca->response_lock, flags);
		ret = -ENOTCONN;
		goto unlock;
	}
	msg.hdr.owner = owner;
	msg.hdr.type = MCA_ADSP_GLINK_MSG_REQ;
	msg.hdr.opcode = write ? MCA_ADSP_GLINK_OPCODE_WRITE : MCA_ADSP_GLINK_OPCODE_READ;
	msg.property_id = property_id;
	msg.seq = ++mca->seq;
	if (write)
		memcpy(msg.data, value, size);
	mca->owner = owner;
	mca->property_id = property_id;
	mca->opcode = msg.hdr.opcode;
	mca->retcode = -ETIMEDOUT;
	mca->pending = true;
	reinit_completion(&mca->ack);
	spin_unlock_irqrestore(&mca->response_lock, flags);

	client = owner == MCA_ADSP_GLINK_QBG_OWNER ? mca->qbg_client : mca->client;
	ret = pmic_glink_write(client, &msg, sizeof(msg));
	if (!ret) {
		wait_for_completion_timeout(&mca->ack,
					    msecs_to_jiffies(MCA_ADSP_GLINK_TIMEOUT_MS));
		spin_lock_irqsave(&mca->response_lock, flags);
		ret = mca->retcode;
		if (!ret && !write)
			memcpy(value, mca->read_buf, size);
		spin_unlock_irqrestore(&mca->response_lock, flags);
	}
	spin_lock_irqsave(&mca->response_lock, flags);
	mca->pending = false;
	spin_unlock_irqrestore(&mca->response_lock, flags);
unlock:
	mutex_unlock(&mca->rw_lock);
out:
	srcu_read_unlock(&mca_provider_srcu, idx);
	return ret;
}

int mca_adsp_glink_write_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_xfer(MCA_ADSP_GLINK_OWNER, prop_id, value, size, true);
}
EXPORT_SYMBOL(mca_adsp_glink_write_prop);

int mca_adsp_glink_read_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_xfer(MCA_ADSP_GLINK_OWNER, prop_id, value, size, false);
}
EXPORT_SYMBOL(mca_adsp_glink_read_prop);

int mca_adsp_glink_qbg_write_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_xfer(MCA_ADSP_GLINK_QBG_OWNER, prop_id, value, size, true);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_write_prop);

int mca_adsp_glink_qbg_read_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_xfer(MCA_ADSP_GLINK_QBG_OWNER, prop_id, value, size, false);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_read_prop);

static int mca_adsp_register_ops(struct list_head *head,
			       struct mca_adsp_glink_ops *ops, void *priv)
{
	struct mca_adsp_ops_node *n, *existing;

	if (!ops)
		return -EINVAL;
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;
	n->ops = ops;
	n->priv = priv;
	mutex_lock(&mca_ops_lock);
	list_for_each_entry(existing, head, node) {
		if (existing->ops == ops && existing->priv == priv) {
			mutex_unlock(&mca_ops_lock);
			kfree(n);
			return -EEXIST;
		}
	}
	list_add_tail_rcu(&n->node, head);
	mutex_unlock(&mca_ops_lock);
	return 0;
}

static void mca_adsp_unregister_ops(struct list_head *head,
				    struct mca_adsp_glink_ops *ops, void *priv)
{
	struct mca_adsp_ops_node *n, *found = NULL;

	mutex_lock(&mca_ops_lock);
	list_for_each_entry(n, head, node) {
		if (n->ops == ops && n->priv == priv) {
			list_del_rcu(&n->node);
			found = n;
			break;
		}
	}
	mutex_unlock(&mca_ops_lock);
	if (found) {
		synchronize_srcu(&mca_ops_srcu);
		kfree(found);
	}
}

int mca_adsp_glink_resister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	return mca_adsp_register_ops(&g_mca_adsp_glink_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_resister_ops);

int mca_adsp_glink_qbg_resister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	return mca_adsp_register_ops(&g_mca_adsp_glink_qbg_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_resister_ops);

void mca_adsp_glink_unregister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	mca_adsp_unregister_ops(&g_mca_adsp_glink_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_unregister_ops);

void mca_adsp_glink_qbg_unregister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	mca_adsp_unregister_ops(&g_mca_adsp_glink_qbg_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_unregister_ops);

static void mca_adsp_dispatch(struct list_head *head, struct mca_adsp_event *event)
{
	struct mca_adsp_ops_node *n;
	int idx;

	idx = srcu_read_lock(&mca_ops_srcu);
	list_for_each_entry_rcu(n, head, node, srcu_read_lock_held(&mca_ops_srcu)) {
		if (!event->state_event) {
			if (n->ops->notification)
				n->ops->notification(event->msg.property_id, event->msg.data,
						     MCA_ADSP_PROP_DATA_LEN, n->priv);
		} else if (event->state == PMIC_GLINK_STATE_UP) {
			if (n->ops->glink_state_up)
				n->ops->glink_state_up(n->priv);
		} else if (n->ops->glink_state_down) {
			n->ops->glink_state_down(n->priv);
		}
	}
	srcu_read_unlock(&mca_ops_srcu, idx);
}

static void mca_adsp_notify_work(struct work_struct *work)
{
	struct mca_adsp_glink_dev *mca = container_of(work, struct mca_adsp_glink_dev,
						    notify_work);
	struct mca_adsp_event *event;

	mutex_lock(&mca->notify_lock);
	while (!list_empty(&mca->notifications)) {
		event = list_first_entry(&mca->notifications, struct mca_adsp_event, node);
		list_del(&event->node);
		mutex_unlock(&mca->notify_lock);
		if (event->state_event || event->msg.hdr.owner == MCA_ADSP_GLINK_OWNER)
			mca_adsp_dispatch(&g_mca_adsp_glink_ops_head, event);
		if (event->state_event || event->msg.hdr.owner == MCA_ADSP_GLINK_QBG_OWNER)
			mca_adsp_dispatch(&g_mca_adsp_glink_qbg_ops_head, event);
		kfree(event);
		mutex_lock(&mca->notify_lock);
	}
	mutex_unlock(&mca->notify_lock);
}

static void mca_adsp_queue_event(struct mca_adsp_glink_dev *mca,
				 struct mca_adsp_event *event)
{
	mutex_lock(&mca->notify_lock);
	list_add_tail(&event->node, &mca->notifications);
	queue_work(mca->wq, &mca->notify_work);
	mutex_unlock(&mca->notify_lock);
}

static int mca_adsp_glink_callback(void *priv, void *data, size_t len)
{
	struct mca_adsp_glink_dev *mca = priv;
	const struct pmic_glink_hdr *hdr = data;
	const struct mca_adsp_glink_resp_msg *msg = data;
	struct mca_adsp_event *event;
	unsigned long flags;

	if (!data || len < sizeof(*hdr) ||
	    (hdr->owner != MCA_ADSP_GLINK_OWNER && hdr->owner != MCA_ADSP_GLINK_QBG_OWNER))
		return -EINVAL;
	if (hdr->type == MCA_ADSP_GLINK_MSG_NOTIFY) {
		if (len != sizeof(event->msg))
			return -EMSGSIZE;
		event = kzalloc(sizeof(*event), GFP_KERNEL);
		if (!event)
			return -ENOMEM;
		memcpy(&event->msg, data, sizeof(event->msg));
		mca_adsp_queue_event(mca, event);
		return 0;
	}
	if (hdr->type != MCA_ADSP_GLINK_MSG_REQ || len != sizeof(*msg))
		return -EMSGSIZE;
	spin_lock_irqsave(&mca->response_lock, flags);
	if (mca->pending && msg->seq == mca->seq && msg->property_id == mca->property_id &&
	    hdr->owner == mca->owner && hdr->opcode == mca->opcode) {
		mca->retcode = msg->retcode ? -EREMOTEIO : 0;
		if (!mca->retcode && hdr->opcode == MCA_ADSP_GLINK_OPCODE_READ)
			memcpy(mca->read_buf, msg->data, sizeof(mca->read_buf));
		mca->pending = false;
		complete(&mca->ack);
	}
	spin_unlock_irqrestore(&mca->response_lock, flags);
	return 0;
}

static void mca_adsp_glink_state_cb(void *priv, enum pmic_glink_state state)
{
	struct mca_adsp_glink_dev *mca = priv;
	struct mca_adsp_event *event;
	unsigned long flags;

	spin_lock_irqsave(&mca->response_lock, flags);
	mca->online = state == PMIC_GLINK_STATE_UP;
	if (!mca->online && mca->pending) {
		mca->retcode = -ENOTCONN;
		mca->pending = false;
		complete(&mca->ack);
	}
	spin_unlock_irqrestore(&mca->response_lock, flags);
	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (event) {
		event->state_event = true;
		event->state = state;
		mca_adsp_queue_event(mca, event);
	}
}

static void mca_adsp_stop(struct mca_adsp_glink_dev *mca)
{
	unsigned long flags;

	mutex_lock(&mca_provider_lock);
	if (rcu_access_pointer(g_mca_adsp_glink) == mca)
		rcu_assign_pointer(g_mca_adsp_glink, NULL);
	mutex_unlock(&mca_provider_lock);
	spin_lock_irqsave(&mca->response_lock, flags);
	mca->online = false;
	mca->pending = false;
	mca->retcode = -ENODEV;
	complete(&mca->ack);
	spin_unlock_irqrestore(&mca->response_lock, flags);
	/* Exported property requests may have captured the old provider. */
	synchronize_srcu(&mca_provider_srcu);
	if (!IS_ERR_OR_NULL(mca->client))
		pmic_glink_unregister_client(mca->client);
	if (!IS_ERR_OR_NULL(mca->qbg_client))
		pmic_glink_unregister_client(mca->qbg_client);
	/* Events are dispatched outside PMIC RX so callbacks can read properties. */
	destroy_workqueue(mca->wq);
}

static int mca_adsp_glink_probe(struct platform_device *pdev)
{
	struct pmic_glink_client_data client_data = {};
	struct mca_adsp_glink_dev *mca;
	int ret;

	mca = devm_kzalloc(&pdev->dev, sizeof(*mca), GFP_KERNEL);
	if (!mca)
		return -ENOMEM;
	mca->dev = &pdev->dev;
	mutex_init(&mca->rw_lock);
	spin_lock_init(&mca->response_lock);
	init_completion(&mca->ack);
	mutex_init(&mca->notify_lock);
	INIT_LIST_HEAD(&mca->notifications);
	INIT_WORK(&mca->notify_work, mca_adsp_notify_work);
	mca->online = true;
	mca->wq = alloc_ordered_workqueue("mca_adsp", WQ_MEM_RECLAIM);
	if (!mca->wq)
		return -ENOMEM;
	client_data.name = "mca_adsp_glink";
	client_data.id = MCA_ADSP_GLINK_OWNER;
	client_data.priv = mca;
	client_data.msg_cb = mca_adsp_glink_callback;
	client_data.state_cb = mca_adsp_glink_state_cb;
	mca->client = pmic_glink_register_client(&pdev->dev, &client_data);
	if (IS_ERR(mca->client)) {
		ret = PTR_ERR(mca->client);
		goto stop;
	}
	client_data.name = "mca_adsp_qbg_glink";
	client_data.id = MCA_ADSP_GLINK_QBG_OWNER;
	client_data.state_cb = NULL;
	mca->qbg_client = pmic_glink_register_client(&pdev->dev, &client_data);
	if (IS_ERR(mca->qbg_client)) {
		ret = PTR_ERR(mca->qbg_client);
		goto stop;
	}
	mutex_lock(&mca_provider_lock);
	if (rcu_access_pointer(g_mca_adsp_glink)) {
		mutex_unlock(&mca_provider_lock);
		ret = -EBUSY;
		goto stop;
	}
	rcu_assign_pointer(g_mca_adsp_glink, mca);
	mutex_unlock(&mca_provider_lock);
	platform_set_drvdata(pdev, mca);
	return 0;
stop:
	mca_adsp_stop(mca);
	return ret;
}

static void mca_adsp_glink_remove(struct platform_device *pdev)
{
	mca_adsp_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id mca_adsp_match_table[] = {
	{ .compatible = "mca,adsp_glink" },
	{},
};
MODULE_DEVICE_TABLE(of, mca_adsp_match_table);

static struct platform_driver mca_adsp_driver = {
	.driver = {
		.name = "mca_adsp_glink",
		.of_match_table = mca_adsp_match_table,
	},
	.probe = mca_adsp_glink_probe,
	.remove = mca_adsp_glink_remove,
};
module_platform_driver(mca_adsp_driver);

MODULE_DESCRIPTION("mca glink driver");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
