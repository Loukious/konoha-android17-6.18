// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
/* Native CSR eight-byte counter, adapted to common ETR buffer ownership.
 * Included by coresight-tmc-etr.c to use its private buffer operations.
 */
#include "coresight-csr.h"
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/uaccess.h>

#define TMC_STREAM_BLOCK PAGE_SIZE
struct tmc_stream {
	struct miscdevice misc;
	struct tmc_drvdata *drvdata;
	struct mutex read_lock;
	wait_queue_head_t wait;
	struct etr_buf *buf;
	atomic64_t produced;
	u64 consumed, final_bytes;
	unsigned long start_offset;
	u32 irq_offset;
	int irq;
	bool opened, active, stopped;
};

static bool tmc_stream_is_open(struct tmc_drvdata *d)
{
	return d->stream && READ_ONCE(d->stream->opened);
}

static bool tmc_stream_overrun(u64 produced, u64 consumed, size_t size)
{
	/* Leave one block for the device's current, incomplete DMA transaction. */
	return size < 4 * TMC_STREAM_BLOCK || produced < consumed ||
	       produced - consumed > size - TMC_STREAM_BLOCK;
}

static irqreturn_t tmc_stream_irq(int irq, void *data)
{
	struct tmc_stream *s = data;
	unsigned long flags;
	raw_spin_lock_irqsave(&s->drvdata->spinlock, flags);
	if (s->active)
		atomic64_add(TMC_STREAM_BLOCK, &s->produced);
	raw_spin_unlock_irqrestore(&s->drvdata->spinlock, flags);
	wake_up_interruptible(&s->wait);
	return IRQ_HANDLED;
}

/* Called with the TMC lock held after the formatter has been flushed. */
static void tmc_stream_stop(struct tmc_drvdata *d)
{
	struct tmc_stream *s = d->stream;
	u64 rwp, position, complete, expected, partial;
	if (!s || !s->active)
		return;
	qcom_csr_set_byte_counter(d->csr, s->irq_offset, 0);
	rwp = tmc_read_rwp(d);
	complete = atomic64_read(&s->produced);
	if (rwp >= s->buf->hwaddr && rwp <= s->buf->hwaddr + s->buf->size) {
		position = (rwp - s->buf->hwaddr) % s->buf->size;
		expected = (s->start_offset + complete) % s->buf->size;
		partial = (position + s->buf->size - expected) % s->buf->size;
		/* Unaccounted complete blocks mean the IRQ/reader was overrun. */
		s->final_bytes = partial < TMC_STREAM_BLOCK ? complete + partial : U64_MAX;
	} else {
		s->final_bytes = U64_MAX;
	}
	s->active = false;
	smp_store_release(&s->stopped, true);
	wake_up_interruptible(&s->wait);
}

static int tmc_stream_open(struct inode *inode, struct file *file)
{
	struct tmc_stream *s = container_of(file->private_data, struct tmc_stream, misc);
	struct tmc_drvdata *d = s->drvdata;
	struct etr_flat_buf *flat;
	unsigned long flags;
	u64 rwp;
	int ret;

	mutex_lock(&s->read_lock);
	if (s->opened) {
		ret = -EBUSY;
		goto out;
	}
	ret = pm_runtime_resume_and_get(d->csdev->dev.parent);
	if (ret < 0)
		goto out;
	ret = pm_runtime_resume_and_get(d->csr->dev.parent);
	if (ret < 0)
		goto out_tmc;
	raw_spin_lock_irqsave(&d->spinlock, flags);
	if (coresight_get_mode(d->csdev) != CS_MODE_SYSFS || !d->etr_buf || d->reading) {
		ret = -EBUSY;
		goto out_spin;
	}
	if (d->etr_buf->mode != ETR_MODE_FLAT || d->etr_buf->size < 4 * TMC_STREAM_BLOCK) {
		ret = -EOPNOTSUPP;
		goto out_spin;
	}
	flat = d->etr_buf->private;
	if (!flat->coherent) {
		ret = -EOPNOTSUPP;
		goto out_spin;
	}
	rwp = tmc_read_rwp(d);
	if (rwp < d->etr_buf->hwaddr || rwp > d->etr_buf->hwaddr + d->etr_buf->size) {
		ret = -EIO;
		goto out_spin;
	}
	s->buf = d->etr_buf;
	s->start_offset = (rwp - s->buf->hwaddr) % s->buf->size;
	s->consumed = s->final_bytes = 0;
	atomic64_set(&s->produced, 0);
	s->stopped = false;
	s->active = true;
	ret = qcom_csr_set_byte_counter(d->csr, s->irq_offset, TMC_STREAM_BLOCK / 8);
	if (ret) {
		s->active = false;
		s->buf = NULL;
		goto out_spin;
	}
	refcount_inc(&s->buf->refcount);
	s->opened = true;
	file->private_data = s;
	ret = nonseekable_open(inode, file);
out_spin:
	raw_spin_unlock_irqrestore(&d->spinlock, flags);
	if (!ret)
		goto out;
	pm_runtime_put(d->csr->dev.parent);
out_tmc:
	pm_runtime_put(d->csdev->dev.parent);
out:
	mutex_unlock(&s->read_lock);
	return ret;
}

static ssize_t tmc_stream_read(struct file *file, char __user *user, size_t len, loff_t *pos)
{
	struct tmc_stream *s = file->private_data;
	u64 produced, available;
	size_t offset, bytes;
	char *src, *copy;
	ssize_t ret;

	if (!len)
		return 0;
	ret = mutex_lock_interruptible(&s->read_lock);
	if (ret)
		return ret;
	for (;;) {
		produced = smp_load_acquire(&s->stopped) ? READ_ONCE(s->final_bytes)
							 : atomic64_read(&s->produced);
		if (tmc_stream_overrun(produced, s->consumed, s->buf->size)) {
			ret = -EOVERFLOW;
			goto out;
		}
		available = produced - s->consumed;
		if (available || smp_load_acquire(&s->stopped))
			break;
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		ret = wait_event_interruptible(s->wait,
					       smp_load_acquire(&s->stopped) ||
						   atomic64_read(&s->produced) > s->consumed);
		if (ret)
			goto out;
	}
	if (!available) {
		ret = 0;
		goto out;
	}
	offset = (s->start_offset + s->consumed) % s->buf->size;
	bytes = min_t(u64, min_t(size_t, len, PAGE_SIZE), available);
	bytes = min_t(size_t, bytes, s->buf->size - offset);
	copy = kmalloc(bytes, GFP_KERNEL);
	if (!copy) {
		ret = -ENOMEM;
		goto out;
	}
	ret = tmc_etr_buf_get_data(s->buf, offset, bytes, &src);
	if (ret <= 0)
		goto out_copy;
	dma_rmb();
	memcpy(copy, src, ret);
	produced =
	    smp_load_acquire(&s->stopped) ? READ_ONCE(s->final_bytes) : atomic64_read(&s->produced);
	if (tmc_stream_overrun(produced, s->consumed, s->buf->size))
		ret = -EOVERFLOW;
	else if (copy_to_user(user, copy, ret))
		ret = -EFAULT;
	else
		s->consumed += ret;
out_copy:
	kfree(copy);
out:
	mutex_unlock(&s->read_lock);
	return ret;
}

static __poll_t tmc_stream_poll(struct file *file, poll_table *wait)
{
	struct tmc_stream *s = file->private_data;
	u64 produced, consumed = READ_ONCE(s->consumed);
	__poll_t mask = 0;
	poll_wait(file, &s->wait, wait);
	produced =
	    smp_load_acquire(&s->stopped) ? READ_ONCE(s->final_bytes) : atomic64_read(&s->produced);
	if (tmc_stream_overrun(produced, consumed, s->buf->size))
		mask |= EPOLLERR;
	else if (produced > consumed)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (smp_load_acquire(&s->stopped))
		mask |= EPOLLHUP;
	return mask;
}

static int tmc_stream_release(struct inode *inode, struct file *file)
{
	struct tmc_stream *s = file->private_data;
	struct tmc_drvdata *d = s->drvdata;
	struct etr_buf *buf;
	unsigned long flags;
	mutex_lock(&s->read_lock);
	raw_spin_lock_irqsave(&d->spinlock, flags);
	if (s->active)
		qcom_csr_set_byte_counter(d->csr, s->irq_offset, 0);
	s->active = false;
	s->opened = false;
	buf = s->buf;
	s->buf = NULL;
	raw_spin_unlock_irqrestore(&d->spinlock, flags);
	synchronize_irq(s->irq);
	tmc_etr_free_sysfs_buf(buf);
	pm_runtime_put(d->csr->dev.parent);
	pm_runtime_put(d->csdev->dev.parent);
	mutex_unlock(&s->read_lock);
	return 0;
}
static const struct file_operations tmc_stream_fops = {
    .owner = THIS_MODULE,
    .open = tmc_stream_open,
    .read = tmc_stream_read,
    .poll = tmc_stream_poll,
    .release = tmc_stream_release,
};

int tmc_stream_init(struct tmc_drvdata *d)
{
	struct device *dev = d->csdev->dev.parent;
	struct tmc_stream *s;
	const char *name;
	int irq, ret;
	if (!d->csr)
		return 0;
	irq = of_irq_get_byname(dev->of_node, "byte-cntr-irq");
	if (irq < 0)
		return irq;
	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->drvdata = d;
	s->irq = irq;
	mutex_init(&s->read_lock);
	init_waitqueue_head(&s->wait);
	atomic64_set(&s->produced, 0);
	ret = device_property_read_u32(dev, "csr-irqctrl-offset", &s->irq_offset);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(d->csr->dev.parent);
	if (ret < 0)
		return ret;
	ret = qcom_csr_set_byte_counter(d->csr, s->irq_offset, 0);
	pm_runtime_put(d->csr->dev.parent);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, irq, tmc_stream_irq, 0, dev_name(dev), s);
	if (ret)
		return ret;
	ret = device_property_read_string(dev, "byte-cntr-name", &name);
	if (ret)
		return ret;
	s->misc.name = name;
	s->misc.minor = MISC_DYNAMIC_MINOR;
	s->misc.fops = &tmc_stream_fops;
	s->misc.parent = dev;
	ret = misc_register(&s->misc);
	if (ret)
		return ret;
	d->stream = s;
	return 0;
}
void tmc_stream_remove(struct tmc_drvdata *d)
{
	if (d->stream)
		misc_deregister(&d->stream->misc);
}
