/* SPDX-License-Identifier: GPL-2.0 */
/* One owned stream; read_rb returns the bytes copied and never waits for data. */
#ifndef _RINGBUFFER_H_
#define _RINGBUFFER_H_

#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

int write_rb(const char *data, int32_t size);
int read_rb(char *data, int32_t size);
int get_rb_free_size(void);
int get_rb_max_size(void);
void rb_force_exit(void);
void rb_end(void);
int rb_shoule_exit(void);
int create_rb(void);
void rb_init(void);
int release_rb(void);
int get_rb_avalible_size(void);
#endif
