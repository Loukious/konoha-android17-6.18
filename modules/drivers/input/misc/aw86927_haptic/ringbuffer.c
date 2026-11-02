// SPDX-License-Identifier: GPL-2.0
/* A single owned haptic stream; readers never wait for producer data. */
#include <linux/errno.h>
#include <linux/srcu.h>
#include "ringbuffer.h"

#define BUFFER_SIZE (1024 * 8)

struct rb {
	/* Serializes buffer data and producer admission separately. */
	struct mutex data_lock;
	struct mutex writer_lock;
	wait_queue_head_t wait_q;
	char data[BUFFER_SIZE];
	unsigned int read_pos;
	unsigned int write_pos;
	unsigned int used;
	unsigned int generation;
	bool eof;
	bool stopping;
	bool removing;
};

static DEFINE_MUTEX(rb_instance_lock);
DEFINE_STATIC_SRCU(rb_instance_srcu);
static struct rb __rcu *grb;

static bool rb_can_write(struct rb *buffer, int size, unsigned int generation)
{
	bool ready;

	mutex_lock(&buffer->data_lock);
	ready = buffer->removing || buffer->stopping || buffer->eof ||
		buffer->generation != generation ||
		BUFFER_SIZE - buffer->used >= size;
	mutex_unlock(&buffer->data_lock);
	return ready;
}

int write_rb(const char *data, int32_t size)
{
	struct rb *buffer;
	unsigned int generation, part;
	int index, ret;

	if (!data || size <= 0 || size > BUFFER_SIZE)
		return -EINVAL;
	index = srcu_read_lock(&rb_instance_srcu);
	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (!buffer) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&buffer->data_lock);
	generation = buffer->generation;
	mutex_unlock(&buffer->data_lock);
	ret = mutex_lock_interruptible(&buffer->writer_lock);
	if (ret)
		goto out;
	ret = wait_event_interruptible(buffer->wait_q, rb_can_write(buffer, size, generation));
	if (ret)
		goto writer_out;
	mutex_lock(&buffer->data_lock);
	if (buffer->removing || buffer->stopping || buffer->eof) {
		ret = -ESHUTDOWN;
	} else if (buffer->generation != generation) {
		ret = -ECANCELED;
	} else {
		part = min_t(unsigned int, size, BUFFER_SIZE - buffer->write_pos);
		memcpy(buffer->data + buffer->write_pos, data, part);
		memcpy(buffer->data, data + part, size - part);
		buffer->write_pos = (buffer->write_pos + size) % BUFFER_SIZE;
		buffer->used += size;
		ret = size;
	}
	mutex_unlock(&buffer->data_lock);
writer_out:
	mutex_unlock(&buffer->writer_lock);
out:
	srcu_read_unlock(&rb_instance_srcu, index);
	return ret;
}

int read_rb(char *data, int32_t size)
{
	struct rb *buffer;
	unsigned int part, bytes;
	int index, ret = 0;

	if (!data || size <= 0 || size > BUFFER_SIZE)
		return -EINVAL;
	index = srcu_read_lock(&rb_instance_srcu);
	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (!buffer)
		goto out;
	mutex_lock(&buffer->data_lock);
	if (!buffer->removing && !buffer->stopping) {
		bytes = min_t(unsigned int, size, buffer->used);
		part = min(bytes, BUFFER_SIZE - buffer->read_pos);
		memcpy(data, buffer->data + buffer->read_pos, part);
		memcpy(data + part, buffer->data, bytes - part);
		buffer->read_pos = (buffer->read_pos + bytes) % BUFFER_SIZE;
		buffer->used -= bytes;
		ret = bytes;
	}
	mutex_unlock(&buffer->data_lock);
	wake_up_interruptible_all(&buffer->wait_q);
out:
	srcu_read_unlock(&rb_instance_srcu, index);
	return ret;
}

int get_rb_free_size(void)
{
	struct rb *buffer;
	int index, bytes = 0;

	index = srcu_read_lock(&rb_instance_srcu);
	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (buffer) {
		mutex_lock(&buffer->data_lock);
		bytes = BUFFER_SIZE - buffer->used;
		mutex_unlock(&buffer->data_lock);
	}
	srcu_read_unlock(&rb_instance_srcu, index);
	return bytes;
}

int get_rb_avalible_size(void)
{
	struct rb *buffer;
	int index, bytes = 0;

	index = srcu_read_lock(&rb_instance_srcu);
	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (buffer) {
		mutex_lock(&buffer->data_lock);
		bytes = buffer->used;
		mutex_unlock(&buffer->data_lock);
	}
	srcu_read_unlock(&rb_instance_srcu, index);
	return bytes;
}

int get_rb_max_size(void)
{
	return BUFFER_SIZE;
}

void rb_force_exit(void)
{
	struct rb *buffer;
	int index = srcu_read_lock(&rb_instance_srcu);

	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (buffer) {
		mutex_lock(&buffer->data_lock);
		buffer->stopping = true;
		mutex_unlock(&buffer->data_lock);
		wake_up_interruptible_all(&buffer->wait_q);
	}
	srcu_read_unlock(&rb_instance_srcu, index);
}

void rb_end(void)
{
	struct rb *buffer;
	int index = srcu_read_lock(&rb_instance_srcu);

	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (buffer) {
		mutex_lock(&buffer->data_lock);
		buffer->eof = true;
		mutex_unlock(&buffer->data_lock);
		wake_up_interruptible_all(&buffer->wait_q);
	}
	srcu_read_unlock(&rb_instance_srcu, index);
}

int rb_shoule_exit(void)
{
	struct rb *buffer;
	int index, done = 1;

	index = srcu_read_lock(&rb_instance_srcu);
	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (buffer) {
		mutex_lock(&buffer->data_lock);
		done = buffer->eof || buffer->stopping;
		mutex_unlock(&buffer->data_lock);
	}
	srcu_read_unlock(&rb_instance_srcu, index);
	return done;
}

int create_rb(void)
{
	struct rb *buffer;
	int ret = 0;

	mutex_lock(&rb_instance_lock);
	if (rcu_access_pointer(grb)) {
		ret = -EBUSY;
		goto out;
	}
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}
	mutex_init(&buffer->data_lock);
	mutex_init(&buffer->writer_lock);
	init_waitqueue_head(&buffer->wait_q);
	rcu_assign_pointer(grb, buffer);
out:
	mutex_unlock(&rb_instance_lock);
	return ret;
}

void rb_init(void)
{
	struct rb *buffer;
	int index = srcu_read_lock(&rb_instance_srcu);

	buffer = srcu_dereference(grb, &rb_instance_srcu);
	if (buffer) {
		mutex_lock(&buffer->data_lock);
		if (!buffer->removing) {
			buffer->generation++;
			buffer->used = buffer->read_pos = buffer->write_pos = 0;
			buffer->eof = buffer->stopping = false;
		}
		mutex_unlock(&buffer->data_lock);
		wake_up_interruptible_all(&buffer->wait_q);
	}
	srcu_read_unlock(&rb_instance_srcu, index);
}

int release_rb(void)
{
	struct rb *buffer;

	mutex_lock(&rb_instance_lock);
	buffer = rcu_dereference_protected(grb, lockdep_is_held(&rb_instance_lock));
	if (buffer) {
		mutex_lock(&buffer->data_lock);
		buffer->removing = buffer->stopping = true;
		mutex_unlock(&buffer->data_lock);
		rcu_assign_pointer(grb, NULL);
		wake_up_interruptible_all(&buffer->wait_q);
		synchronize_srcu(&rb_instance_srcu);
		kfree(buffer);
	}
	mutex_unlock(&rb_instance_lock);
	return 0;
}
