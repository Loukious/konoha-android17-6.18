// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved. */
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/srcu.h>
#include <asm/arch_timer.h>
#include "smem-mailbox.h"

#define SMEM_VERSION 0x00000001
#define START_COOKIE 0xbeefcafe
#define END_COOKIE 0x0fed0fed
#define DEVICE_NAME "smem_mailbox"
#define SMEM_DESCRIPTOR 655
#define QCOM_SMEM_HOST_MODEM 1
#define IRQ_NUM_NAMES 4
#define BUF_IO_PTR_SIZE 8
#define TLV_TYPE_TIME 0
#define TLV_TYPE_DATA 1

static const char * const IRQ_NAMES[] = {
	"smem-mailbox-smp2p-1-in", "smem-mailbox-smp2p-2-in",
	"smem-mailbox-smp2p-3-in", "smem-mailbox-smp2p-4-in",
};

/* Keep descriptor and TLV layouts identical to the modem protocol. */
struct client_info {
	u32 id;
	u32 start_loc;
	u32 read_size;
	u32 write_size;
};

struct smem_info {
	u32 start_cookie;
	u32 version;
	u32 num_of_clients;
	struct client_info clients[IRQ_NUM_NAMES];
	u32 end_cookie;
};

struct tlv {
	u16 type;
	u16 length;
};

struct smem_mailbox_provider;
struct smem_client_channel {
	struct smem_mailbox_provider *provider;
	struct client_info info;
	smem_mailbox_urgent_cb urgent_cb;
	struct task_struct *callback_task;
	int irq;
	bool connected;
	bool stopping;
};

struct smem_mailbox_provider {
	struct kref ref;
	struct device *dev;
	struct qcom_smem_state *state;
	u32 state_bit;
	u32 signal_value;
	struct smem_info info;
	struct smem_client_channel channels[IRQ_NUM_NAMES];
	bool mapped;
	bool removing;
};

static DEFINE_MUTEX(mailbox_lock);
DEFINE_STATIC_SRCU(mailbox_callbacks);
static struct smem_mailbox_provider *mailbox_provider;
static bool mailbox_retiring;

static void mailbox_release(struct kref *ref)
{
	struct smem_mailbox_provider *p;

	p = container_of(ref, struct smem_mailbox_provider, ref);
	kfree(p);
}

static u32 ring_used(u32 head, u32 tail, u32 size)
{
	return head >= tail ? head - tail : size - tail + head;
}

static int mailbox_validate_info(const struct smem_info *info, size_t size,
				 u32 state_bit)
{
	u64 end, other_end;
	u32 i, j;

	if (size < sizeof(*info) || info->start_cookie != START_COOKIE ||
	    info->end_cookie != END_COOKIE || info->version != SMEM_VERSION ||
	    !info->num_of_clients || info->num_of_clients > IRQ_NUM_NAMES ||
	    state_bit >= 32)
		return -EPROTO;
	for (i = 0; i < info->num_of_clients; i++) {
		const struct client_info *c = &info->clients[i];

		if (c->id >= 32 - state_bit || c->start_loc < sizeof(*info) ||
		    (c->start_loc & 3) || (c->read_size & 3) || (c->write_size & 3) ||
		    c->read_size < 4 || c->read_size > U16_MAX ||
		    c->write_size < 4 || c->write_size > INT_MAX)
			return -EPROTO;
		end = (u64)c->start_loc + 2 * BUF_IO_PTR_SIZE +
			c->read_size + c->write_size;
		if (end > size)
			return -EPROTO;
		for (j = 0; j < i; j++) {
			const struct client_info *other = &info->clients[j];

			other_end = (u64)other->start_loc + 2 * BUF_IO_PTR_SIZE +
				other->read_size + other->write_size;
			if (c->id == other->id ||
			    (c->start_loc < other_end && other->start_loc < end))
				return -EPROTO;
		}
	}
	return 0;
}

/* Caller holds mailbox_lock. Publish only a complete checked descriptor. */
static int mailbox_memory(struct smem_mailbox_provider *p, void __iomem **memory)
{
	struct smem_info info;
	void *smem;
	size_t size;
	u32 i;
	int ret;

	smem = qcom_smem_get(QCOM_SMEM_HOST_MODEM, SMEM_DESCRIPTOR, &size);
	if (IS_ERR(smem))
		return PTR_ERR(smem);
	if (!smem || size < sizeof(info))
		return -EPROTO;
	memcpy_fromio(&info, (void __iomem *)smem, sizeof(info));
	ret = mailbox_validate_info(&info, size, p->state_bit);
	if (ret)
		return ret;
	if (p->mapped) {
		if (memcmp(&info, &p->info, sizeof(info)))
			return -ESTALE;
	} else {
		p->info = info;
		for (i = 0; i < info.num_of_clients; i++)
			p->channels[i].info = info.clients[i];
		p->mapped = true;
	}
	*memory = (void __iomem *)smem;
	return 0;
}

static struct smem_client_channel *mailbox_channel(struct smem_mailbox_provider *p,
						   int id)
{
	u32 i;

	if (!p || p->removing || !p->mapped || id < 0)
		return NULL;
	for (i = 0; i < p->info.num_of_clients; i++)
		if (p->channels[i].info.id == (u32)id)
			return &p->channels[i];
	return NULL;
}

static int mailbox_indices(void __iomem *ring, u32 size, u32 *head, u32 *tail)
{
	*head = readl(ring);
	*tail = readl(ring + sizeof(u32));
	if (*head >= size || *tail >= size || (*head & 3) || (*tail & 3))
		return -EPROTO;
	/* The peer publishes its head after writing the payload. */
	rmb();
	return 0;
}

static void mailbox_copy_from(void *dest, void __iomem *ring, u32 size,
			      u32 index, u32 length)
{
	u32 first = min(length, size - index);

	if (first)
		memcpy_fromio(dest, ring + BUF_IO_PTR_SIZE + index, first);
	if (length > first)
		memcpy_fromio((u8 *)dest + first, ring + BUF_IO_PTR_SIZE, length - first);
}

static void mailbox_copy_to(void __iomem *ring, u32 size, u32 index,
			    const void *source, u32 length)
{
	u32 first = min(length, size - index);

	memcpy_toio(ring + BUF_IO_PTR_SIZE + index, source, first);
	if (length > first)
		memcpy_toio(ring + BUF_IO_PTR_SIZE, (const u8 *)source + first,
			    length - first);
}

static irqreturn_t smem_read_ready(int irq, void *channel)
{
	struct smem_client_channel *c = channel;
	struct smem_mailbox_provider *p = c->provider;
	smem_mailbox_urgent_cb callback = NULL;
	void __iomem *memory;
	u32 head, tail;
	u16 pending = 0;
	int idx;

	idx = srcu_read_lock(&mailbox_callbacks);
	mutex_lock(&mailbox_lock);
	if (!p->removing && c->connected && c->urgent_cb &&
	    !mailbox_memory(p, &memory) &&
	    !mailbox_indices(memory + c->info.start_loc, c->info.read_size, &head, &tail)) {
		pending = ring_used(head, tail, c->info.read_size);
		callback = c->urgent_cb;
		c->callback_task = current;
	}
	mutex_unlock(&mailbox_lock);
	if (callback) {
		callback(pending);
		mutex_lock(&mailbox_lock);
		c->callback_task = NULL;
		mutex_unlock(&mailbox_lock);
	}
	srcu_read_unlock(&mailbox_callbacks, idx);
	return IRQ_HANDLED;
}

int smem_mailbox_start(int id, smem_mailbox_urgent_cb urgent_cb)
{
	struct smem_mailbox_provider *p;
	struct smem_client_channel *c;
	void __iomem *memory;
	int ret;

	mutex_lock(&mailbox_lock);
	p = mailbox_provider;
	if (!p) {
		ret = -ENODEV;
		goto out;
	}
	ret = mailbox_memory(p, &memory);
	if (ret)
		goto out;
	c = mailbox_channel(p, id);
	if (!c) {
		ret = -EINVAL;
		goto out;
	}
	if (c->connected || c->stopping) {
		ret = -EBUSY;
		goto out;
	}
	c->urgent_cb = urgent_cb;
	c->connected = true;
	ret = 1;
out:
	mutex_unlock(&mailbox_lock);
	return ret;
}
EXPORT_SYMBOL(smem_mailbox_start);

int smem_mailbox_stop(int id)
{
	struct smem_mailbox_provider *p;
	struct smem_client_channel *c;
	void __iomem *memory, *ring;
	u32 head, tail, i;
	int ret = 1, error;

	mutex_lock(&mailbox_lock);
	p = mailbox_provider;
	c = mailbox_channel(p, id);
	if (!c) {
		ret = p ? -EINVAL : -ENODEV;
		goto out;
	}
	/* Withdrawal must run outside callbacks in this SRCU domain. */
	for (i = 0; i < IRQ_NUM_NAMES; i++)
		if (p->channels[i].callback_task == current) {
			ret = -EDEADLK;
			goto out;
		}
	if (c->stopping || !c->connected) {
		ret = c->stopping ? -EBUSY : -EPERM;
		goto out;
	}
	c->connected = false;
	c->stopping = true;
	c->urgent_cb = NULL;
	kref_get(&p->ref);
	error = mailbox_memory(p, &memory);
	if (!error) {
		ring = memory + c->info.start_loc;
		error = mailbox_indices(ring, c->info.read_size, &head, &tail);
		if (!error) {
			/* Release consumed data before the peer reuses it. */
			wmb();
			writel(head, ring + sizeof(u32));
			ring += BUF_IO_PTR_SIZE + c->info.read_size;
			error = mailbox_indices(ring, c->info.write_size, &head, &tail);
			if (!error)
				writel(tail, ring);
		}
	}
	if (error)
		ret = error;
	mutex_unlock(&mailbox_lock);
	synchronize_srcu(&mailbox_callbacks);
	mutex_lock(&mailbox_lock);
	c->stopping = false;
	mutex_unlock(&mailbox_lock);
	kref_put(&p->ref, mailbox_release);
	return ret;
out:
	mutex_unlock(&mailbox_lock);
	return ret;
}
EXPORT_SYMBOL(smem_mailbox_stop);

int smem_mailbox_read(int id, u8 **data, u16 *data_length,
		      unsigned long long *xo_time)
{
	struct smem_mailbox_provider *p;
	struct smem_client_channel *c;
	struct tlv item;
	void __iomem *memory, *ring;
	unsigned long long timestamp = 0;
	u8 *buffer = NULL;
	u32 head, tail, cursor, remaining, length, payload;
	int ret;

	if (!data || !data_length || !xo_time)
		return -EINVAL;
	*data = NULL;
	*data_length = 0;
	*xo_time = 0;
	mutex_lock(&mailbox_lock);
	p = mailbox_provider;
	c = mailbox_channel(p, id);
	if (!c || !c->connected) {
		ret = !p ? -ENODEV : !p->mapped ? -EAGAIN : !c ? -EINVAL : -EPERM;
		goto out;
	}
	ret = mailbox_memory(p, &memory);
	if (ret)
		goto out;
	ring = memory + c->info.start_loc;
	ret = mailbox_indices(ring, c->info.read_size, &head, &tail);
	if (ret)
		goto out;
	remaining = ring_used(head, tail, c->info.read_size);
	if (!remaining) {
		ret = -ENOMSG;
		goto out;
	}
	cursor = tail;
	while (remaining >= sizeof(item)) {
		mailbox_copy_from(&item, ring, c->info.read_size, cursor, sizeof(item));
		length = ALIGN((u32)item.length, 4);
		if (item.length < sizeof(item) || length > remaining)
			break;
		payload = item.length - sizeof(item);
		if (item.type == TLV_TYPE_TIME && payload == sizeof(timestamp)) {
			mailbox_copy_from(&timestamp, ring, c->info.read_size,
					  (cursor + sizeof(item)) % c->info.read_size,
					  sizeof(timestamp));
			cursor = (cursor + length) % c->info.read_size;
			remaining -= length;
			continue;
		}
		if (item.type != TLV_TYPE_DATA)
			break;
		if (payload) {
			buffer = kmalloc(payload, GFP_KERNEL);
			if (!buffer) {
				ret = -ENOMEM;
				goto out;
			}
			mailbox_copy_from(buffer, ring, c->info.read_size,
					  (cursor + sizeof(item)) % c->info.read_size, payload);
		}
		cursor = (cursor + length) % c->info.read_size;
		/* Release consumed data before the peer reuses it. */
		wmb();
		writel(cursor, ring + sizeof(u32));
		*data = buffer;
		*data_length = payload;
		*xo_time = timestamp;
		ret = remaining - length;
		goto out;
	}
	/* Partial or malformed messages never advance the consumer pointer. */
	ret = -EPROTO;
out:
	mutex_unlock(&mailbox_lock);
	return ret;
}
EXPORT_SYMBOL(smem_mailbox_read);

int smem_mailbox_write(int id, int flags, __u8 *data, u16 data_length)
{
	struct smem_mailbox_provider *p;
	struct smem_client_channel *c;
	struct tlv time_tlv = { TLV_TYPE_TIME, sizeof(struct tlv) + sizeof(u64) };
	struct tlv data_tlv;
	void __iomem *memory, *ring;
	u64 timestamp;
	u8 *buffer;
	u32 head, tail, length, available, signal;
	int ret;

	if ((flags & ~FLAG_URGENT) || (data_length && !data) ||
	    data_length > U16_MAX - sizeof(struct tlv))
		return -EINVAL;
	length = sizeof(time_tlv) + sizeof(timestamp) +
		ALIGN(sizeof(data_tlv) + (u32)data_length, 4);
	mutex_lock(&mailbox_lock);
	p = mailbox_provider;
	c = mailbox_channel(p, id);
	if (!c || !c->connected) {
		ret = !p ? -ENODEV : !p->mapped ? -EAGAIN : !c ? -EINVAL : -EPERM;
		goto out;
	}
	ret = mailbox_memory(p, &memory);
	if (ret)
		goto out;
	ring = memory + c->info.start_loc + BUF_IO_PTR_SIZE + c->info.read_size;
	ret = mailbox_indices(ring, c->info.write_size, &head, &tail);
	if (ret)
		goto out;
	available = c->info.write_size - ring_used(head, tail, c->info.write_size) - 4;
	if (length > available) {
		ret = -ENOSPC;
		goto out;
	}
	buffer = kzalloc(length, GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}
	timestamp = arch_timer_read_cntvct_el0();
	data_tlv.type = TLV_TYPE_DATA;
	data_tlv.length = sizeof(data_tlv) + data_length;
	memcpy(buffer, &time_tlv, sizeof(time_tlv));
	memcpy(buffer + sizeof(time_tlv), &timestamp, sizeof(timestamp));
	memcpy(buffer + sizeof(time_tlv) + sizeof(timestamp), &data_tlv, sizeof(data_tlv));
	if (data_length)
		memcpy(buffer + sizeof(time_tlv) + sizeof(timestamp) + sizeof(data_tlv),
		       data, data_length);
	mailbox_copy_to(ring, c->info.write_size, head, buffer, length);
	kfree(buffer);
	head = (head + length) % c->info.write_size;
	/* Publish the new head only after every payload and padding write. */
	wmb();
	writel(head, ring);
	ret = available - length + 4;
	if (flags & FLAG_URGENT) {
		signal = p->signal_value ^ BIT(p->state_bit + c->info.id);
		ret = qcom_smem_state_update_bits(p->state,
						  BIT(p->state_bit + c->info.id), signal);
		if (ret)
			goto out;
		p->signal_value = signal;
		ret = available - length + 4;
	}
out:
	mutex_unlock(&mailbox_lock);
	return ret;
}
EXPORT_SYMBOL(smem_mailbox_write);

static int smem_probe(struct platform_device *pdev)
{
	struct smem_mailbox_provider *p;
	u32 i;
	int ret;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	kref_init(&p->ref);
	p->dev = &pdev->dev;
	p->state = qcom_smem_state_get(p->dev, "smem-mailbox-smp2p-out", &p->state_bit);
	if (IS_ERR(p->state)) {
		ret = PTR_ERR(p->state);
		goto free;
	}
	if (p->state_bit >= 32) {
		ret = -EINVAL;
		goto put_state;
	}
	for (i = 0; i < IRQ_NUM_NAMES; i++) {
		p->channels[i].provider = p;
		ret = of_irq_get_byname(p->dev->of_node, IRQ_NAMES[i]);
		if (ret <= 0) {
			if (!ret)
				ret = -EINVAL;
			goto free_irqs;
		}
		p->channels[i].irq = ret;
		ret = devm_request_threaded_irq(p->dev, ret, NULL, smem_read_ready,
						IRQF_ONESHOT | IRQF_TRIGGER_RISING |
						IRQF_TRIGGER_FALLING, IRQ_NAMES[i],
						&p->channels[i]);
		if (ret)
			goto free_irqs;
	}
	mutex_lock(&mailbox_lock);
	if (mailbox_provider || mailbox_retiring) {
		mutex_unlock(&mailbox_lock);
		ret = -EBUSY;
		goto free_irqs;
	}
	/* The modem may publish its descriptor only after this driver probes. */
	mailbox_provider = p;
	platform_set_drvdata(pdev, p);
	mutex_unlock(&mailbox_lock);
	return 0;
free_irqs:
	while (i--)
		devm_free_irq(p->dev, p->channels[i].irq, &p->channels[i]);
put_state:
	qcom_smem_state_put(p->state);
free:
	kref_put(&p->ref, mailbox_release);
	return ret;
}

static void smem_remove(struct platform_device *pdev)
{
	struct smem_mailbox_provider *p = platform_get_drvdata(pdev);
	u32 i;

	if (!p)
		return;
	mutex_lock(&mailbox_lock);
	p->removing = true;
	mailbox_retiring = true;
	if (mailbox_provider == p)
		mailbox_provider = NULL;
	for (i = 0; i < IRQ_NUM_NAMES; i++) {
		p->channels[i].connected = false;
		p->channels[i].urgent_cb = NULL;
	}
	mutex_unlock(&mailbox_lock);
	for (i = 0; i < IRQ_NUM_NAMES; i++)
		devm_free_irq(p->dev, p->channels[i].irq, &p->channels[i]);
	synchronize_srcu(&mailbox_callbacks);
	qcom_smem_state_put(p->state);
	platform_set_drvdata(pdev, NULL);
	mutex_lock(&mailbox_lock);
	mailbox_retiring = false;
	mutex_unlock(&mailbox_lock);
	kref_put(&p->ref, mailbox_release);
}

static const struct of_device_id qcm_smem_match[] = {
	{ .compatible = "qcom,smem_mailbox", },
	{},
};
MODULE_DEVICE_TABLE(of, qcm_smem_match);
static struct platform_driver qcom_smem_driver = {
	.probe = smem_probe,
	.remove = smem_remove,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = qcm_smem_match,
	},
};
module_platform_driver(qcom_smem_driver);
MODULE_DESCRIPTION("QCOM SMEM Mailbox Driver");
MODULE_LICENSE("GPL v2");
