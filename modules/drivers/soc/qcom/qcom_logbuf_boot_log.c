// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */


#include <linux/module.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <soc/qcom/minidump.h>

#define BOOT_LOG_SIZE    SZ_512K

static char *boot_log_buf;
static char *boot_log_pos;
static unsigned int boot_log_buf_size;
static unsigned int boot_log_buf_left;
static struct kmsg_dump_iter iter;
static struct task_struct *dump_thread;

static int dump_thread_func(void *arg)
{
	size_t text_len;

	while (!kthread_should_stop() && boot_log_buf_left) {
		while (boot_log_buf_left &&
		       kmsg_dump_get_line(&iter, true, boot_log_pos,
					  boot_log_buf_left, &text_len)) {
			boot_log_pos += text_len;
			boot_log_buf_left -= text_len;
			if (text_len == 0)
				goto out;
		}
		schedule_timeout_interruptible(HZ);
	}
out:
	return 0;
}

static int __init boot_log_dump_init(void)
{
	struct md_region md_entry;
	u64 dumped_line;
	size_t text_len = 0;
	int ret;

	boot_log_buf_size = BOOT_LOG_SIZE;
	boot_log_buf = kzalloc(boot_log_buf_size, GFP_KERNEL);
	if (!boot_log_buf)
		return -ENOMEM;
	boot_log_pos = boot_log_buf;
	boot_log_buf_left = boot_log_buf_size;
	kmsg_dump_rewind(&iter);
	dumped_line = iter.next_seq;
	kmsg_dump_get_buffer(&iter, true, boot_log_buf, boot_log_buf_size, &text_len);
	boot_log_pos += text_len;
	boot_log_buf_left -= text_len;
	iter.cur_seq = dumped_line;

	dump_thread = kthread_create(dump_thread_func, NULL, "dump_thread");
	if (IS_ERR(dump_thread)) {
		ret = PTR_ERR(dump_thread);
		goto free_buffer;
	}
	strscpy(md_entry.name, "KBOOT_LOG", sizeof(md_entry.name));
	md_entry.virt_addr = (uintptr_t)boot_log_buf;
	md_entry.phys_addr = virt_to_phys(boot_log_buf);
	md_entry.size = boot_log_buf_size;
	ret = msm_minidump_add_region(&md_entry);
	if (ret < 0) {
		kthread_stop(dump_thread);
		goto free_buffer;
	}

	/* Keep the service resident while the crash table retains this buffer. */
	wake_up_process(dump_thread);
	return 0;
free_buffer:
	kfree(boot_log_buf);
	return ret;
}
late_initcall(boot_log_dump_init);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Boot Log Dump Driver");
MODULE_LICENSE("GPL");
