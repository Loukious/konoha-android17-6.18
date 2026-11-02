/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
// MIUI ADD: Camera_CameraSkyNet
#define DEBUG
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/cred.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/poll.h>
#include <linux/kfifo.h>
#include "cam_log.h"
/*device name after register in charater*/
#define CAMLOG_DEV_NAME "camlog"
#define CAMLOG_DEV_VERSION "2.0"
static struct camlog_dev s_read_buf;
static struct camlog_dev s_write_buf;

static DEFINE_MUTEX(camlog_message_lock);
static DECLARE_WAIT_QUEUE_HEAD(camlog_is_not_empty);
static bool camlog_ready;
static dev_t devt;
static int debug_level;
static struct kfifo message_kfifo;

#define MY_IOCTL_TYPE 'M'

#define IOCTL_CMD_LOG_SWITCH _IOW(MY_IOCTL_TYPE, 1, int)
#define IOCTL_CMD_DUMP _IOW(MY_IOCTL_TYPE, 2, int)
#define IOCTL_CMD_SET_DEBUG_LEVEL _IOW(MY_IOCTL_TYPE, 3, int)
#define IOCTL_CMD_READ_KFIFO_CNT _IOR(MY_IOCTL_TYPE, 4, int)
#define IOCTL_CMD_READ_LAST_MSG _IOR(MY_IOCTL_TYPE, 5, char *)
#define IOCTL_CMD_SET_DEBUG_ON _IOW(MY_IOCTL_TYPE, 6, int)
#define IOCTL_CMD_SET_DEBUG_OFF _IOW(MY_IOCTL_TYPE, 7, int)

static uint8_t s_last_message[MESSAGE_MAX]; //debug used only
static uid_t s_last_caller_uid = 0;
static pid_t s_last_caller_pid = 0;

static uid_t s_last_reader_uid = 0;
static pid_t s_last_reader_pid = 0;

static void dump(void)
{
	struct camlog_dev element;
	int message_cnt = 0;
	int i = 0;
	int ret = 0;
	pr_debug(
		"----------------- DUMP CAMERALOG START------------------------");
	pr_debug("cameralog version %s", CAMLOG_DEV_VERSION);

	pr_debug(" last writer uid:%u  pid:%u", s_last_caller_uid,
		 s_last_caller_pid);
	pr_debug(" last reader uid:%u  pid:%u", s_last_reader_uid,
		 s_last_reader_pid);
	pr_debug(" last message:%s ", s_last_message);

	message_cnt = kfifo_len(&message_kfifo);
	pr_debug(" kfifo len:%d ", message_cnt);
	if (message_cnt > 0) {
		pr_debug("-------fifo messages-------");
		pr_debug("UID\tPID\tMESSAGE");
		for (i = 0; i < message_cnt; i++) {
			ret = kfifo_out(&message_kfifo, &element,
					sizeof(struct camlog_dev));
			if (ret != sizeof(struct camlog_dev)) {
				break;
			}
			pr_debug("  %u\t%u\t %s\n", element.writer_uid,
				 element.writer_pid, element.m_camlog_message);
		}
		pr_debug("-------------------------------");
	}
	pr_debug(
		"----------------- DUMP CAMERALOG END   ------------------------");
}

static long cameralog_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *data = (void __user *)arg;
	int value;
	long ret = 0;

	mutex_lock(&camlog_message_lock);
	if (!camlog_ready) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case IOCTL_CMD_LOG_SWITCH:
		debug_level = debug_level <= 0;
		break;
	case IOCTL_CMD_SET_DEBUG_ON:
		debug_level = 1;
		break;
	case IOCTL_CMD_SET_DEBUG_OFF:
		debug_level = 0;
		break;
	case IOCTL_CMD_DUMP:
		dump();
		break;
	case IOCTL_CMD_SET_DEBUG_LEVEL:
		if (copy_from_user(&value, data, sizeof(value)))
			ret = -EFAULT;
		else
			debug_level = value;
		break;
	case IOCTL_CMD_READ_KFIFO_CNT:
		/* Preserve the native byte-count ioctl contract. */
		value = kfifo_len(&message_kfifo);
		if (copy_to_user(data, &value, sizeof(value)))
			ret = -EFAULT;
		break;
	case IOCTL_CMD_READ_LAST_MSG:
		if (copy_to_user(data, s_last_message,
				 strnlen(s_last_message, MESSAGE_MAX - 1) + 1))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&camlog_message_lock);
	return ret;
}

static int cameralog_open(struct inode *inode, struct file *filp)
{
	int ret;

	mutex_lock(&camlog_message_lock);
	ret = camlog_ready ? 0 : -ENODEV;
	mutex_unlock(&camlog_message_lock);
	return ret;
}

static ssize_t cameralog_read(struct file *fp, char __user *buff, size_t length,
			      loff_t *ppos)
{
	ssize_t ret = 0;
	size_t message_len;

	if (!length)
		return 0;
	mutex_lock(&camlog_message_lock);
	if (!camlog_ready) {
		ret = -ENODEV;
		goto out;
	}
	if (kfifo_out_peek(&message_kfifo, &s_read_buf, sizeof(s_read_buf)) !=
	    sizeof(s_read_buf))
		goto out;
	message_len = strnlen(s_read_buf.m_camlog_message, MESSAGE_MAX);
	if (length < message_len) {
		ret = -EMSGSIZE;
		goto out;
	}
	/* The native record protocol replaces its final byte with a newline. */
	if (message_len)
		s_read_buf.m_camlog_message[message_len - 1] = '\n';
	if (copy_to_user(buff, s_read_buf.m_camlog_message, message_len)) {
		ret = -EFAULT;
		goto out;
	}
	if (kfifo_out(&message_kfifo, &s_read_buf, sizeof(s_read_buf)) != sizeof(s_read_buf)) {
		ret = -EIO;
		goto out;
	}
	s_last_reader_uid = from_kuid(&init_user_ns, current_uid());
	s_last_reader_pid = task_pid_nr(current);
	ret = message_len;
out:
	mutex_unlock(&camlog_message_lock);
	return ret;
}

void camlog_send_message(void)
{
	pr_debug(" camlog_send_message ss  \n");
}
EXPORT_SYMBOL(camlog_send_message);
static ssize_t cameralog_write(struct file *fp, const char __user *buff, size_t length,
			       loff_t *ppos)
{
	ssize_t ret;
	size_t valid_len = min_t(size_t, length, MESSAGE_MAX - 1);

	mutex_lock(&camlog_message_lock);
	if (!camlog_ready) {
		ret = -ENODEV;
		goto out;
	}
	memset(&s_write_buf, 0, sizeof(s_write_buf));
	if (copy_from_user(s_write_buf.m_camlog_message, buff, valid_len)) {
		ret = -EFAULT;
		goto out;
	}
	ret = strnlen(s_write_buf.m_camlog_message, valid_len);
	if (!ret)
		goto out;
	s_write_buf.writer_uid = from_kuid(&init_user_ns, current_uid());
	s_write_buf.writer_pid = task_pid_nr(current);
	/* Drop complete oldest records, retaining the native overwrite policy. */
	while (kfifo_avail(&message_kfifo) < sizeof(s_write_buf)) {
		if (kfifo_out(&message_kfifo, &s_read_buf, sizeof(s_read_buf)) !=
		    sizeof(s_read_buf)) {
			ret = -EIO;
			goto out;
		}
	}
	if (kfifo_in(&message_kfifo, &s_write_buf, sizeof(s_write_buf)) != sizeof(s_write_buf)) {
		ret = -EIO;
		goto out;
	}
	s_last_caller_uid = s_write_buf.writer_uid;
	s_last_caller_pid = s_write_buf.writer_pid;
	strscpy(s_last_message, s_write_buf.m_camlog_message, sizeof(s_last_message));
	wake_up_interruptible(&camlog_is_not_empty);
out:
	mutex_unlock(&camlog_message_lock);
	return ret;
}

static int cameralog_release(struct inode *inode, struct file *filp)
{
	pr_debug(" cameralog_release!\n");
	wake_up_interruptible(&camlog_is_not_empty);
	return 0;
}

static __poll_t cameralog_poll(struct file *file, struct poll_table_struct *poll_table)
{
	__poll_t mask = 0;

	poll_wait(file, &camlog_is_not_empty, poll_table);
	mutex_lock(&camlog_message_lock);
	if (!camlog_ready)
		mask = EPOLLHUP | EPOLLERR;
	else if (kfifo_len(&message_kfifo) >= sizeof(struct camlog_dev))
		mask = EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&camlog_message_lock);
	return mask;
}

static const struct file_operations cameralog_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = cameralog_ioctl,
	.open = cameralog_open,
	.release = cameralog_release,
	.poll = cameralog_poll,
	.read = cameralog_read,
	.write = cameralog_write,
};
static struct class *camlog_class;
static int camlog_major;
static int __init cameralog_init(void)
{
	struct device *device;
	int ret;

	ret = kfifo_alloc(&message_kfifo, KFIFO_COUNT * sizeof(struct camlog_dev), GFP_KERNEL);
	if (ret)
		return ret;
	camlog_major = register_chrdev(0, CAMLOG_DEV_NAME, &cameralog_fops);
	if (camlog_major < 0) {
		ret = camlog_major;
		goto free_fifo;
	}
	camlog_class = class_create(CAMLOG_DEV_NAME);
	if (IS_ERR(camlog_class)) {
		ret = PTR_ERR(camlog_class);
		goto unregister;
	}
	devt = MKDEV(camlog_major, 0);
	device = device_create(camlog_class, NULL, devt, NULL, CAMLOG_DEV_NAME);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto destroy_class;
	}
	mutex_lock(&camlog_message_lock);
	camlog_ready = true;
	mutex_unlock(&camlog_message_lock);
	return 0;
destroy_class:
	class_destroy(camlog_class);
unregister:
	unregister_chrdev(camlog_major, CAMLOG_DEV_NAME);
free_fifo:
	kfifo_free(&message_kfifo);
	return ret;
}
module_init(cameralog_init);
static void __exit cameralog_exit(void)
{
	mutex_lock(&camlog_message_lock);
	camlog_ready = false;
	mutex_unlock(&camlog_message_lock);
	wake_up_interruptible(&camlog_is_not_empty);
	device_destroy(camlog_class, devt);
	class_destroy(camlog_class);
	unregister_chrdev(camlog_major, CAMLOG_DEV_NAME);
	kfifo_free(&message_kfifo);
}
module_exit(cameralog_exit);
MODULE_DESCRIPTION("camera log device driver");
MODULE_LICENSE("GPL v2");
// END Camera_CameraSkyNet