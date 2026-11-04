/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_HVC_BACKEND_H
#define _LINUX_HVC_BACKEND_H

#include <linux/types.h>

struct hvc_backend;
struct module;

/* get/put, flush(false), and dtr_rts run in atomic context; others may sleep. */
struct hvc_backend_ops {
	ssize_t (*get_chars)(void *data, u8 *buf, size_t count);
	ssize_t (*put_chars)(void *data, const u8 *buf, size_t count);
	int (*flush)(void *data, bool wait);
	int (*open)(void *data);
	void (*close)(void *data);
	void (*hangup)(void *data);
	int (*tiocmget)(void *data);
	int (*tiocmset)(void *data, unsigned int set, unsigned int clear);
	void (*dtr_rts)(void *data, bool active);
};

/* Allocation does not expose a console or TTY. All fallible setup goes first. */
struct hvc_backend *hvc_backend_alloc(const struct hvc_backend_ops *ops,
				     void *data, int outbuf_size,
				     struct module *owner);
/* Publish the complete group atomically. No adapter is published on error. */
int hvc_backend_publish(struct hvc_backend **backends, unsigned int count);
/*
 * Detach, hang up, and drain callbacks. After return ops/data may be freed;
 * retained TTY descriptors only reference native-core operations. The caller
 * must release its hardware state explicitly: module exit may already prevent
 * callback owner acquisition, including the hangup callback.
 */
void hvc_backend_remove(struct hvc_backend *backend);
/* Caller serializes polling against hvc_backend_remove(). */
void hvc_backend_poll(struct hvc_backend *backend);

#endif
