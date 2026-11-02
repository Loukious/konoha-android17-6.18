#define DEBUG 1
#define pr_fmt(fmt) "miev: " fmt

#include "midevice.h"
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/poll.h>

static long miev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static ssize_t miev_read(struct file *filp, char __user *ubuf, size_t size,
			 loff_t *offset);
static ssize_t miev_write(struct file *filp, const char __user *ubuf,
			  size_t size, loff_t *offset);

/* Kfifo buffer size */
#define FIFO_SIZE (64 * 1024)
/* write and read buffer max size */
#define BUF_MAX_SIZE 4096
static struct miev_device *miev_dev;

int write_kbuf(char __kernel *kbuf, int size)
{
	int ret;
	char discarded;

	if (!kbuf || size < 0)
		return -EINVAL;
	if (!size)
		return 0;
	size = min(size, BUF_MAX_SIZE);
	mutex_lock(&miev_dev->lock);
	while (kfifo_size(&miev_dev->fifo) - kfifo_len(&miev_dev->fifo) < size + sizeof(u16)) {
		/* A short record read discards the remainder of the oldest event. */
		ret = kfifo_out(&miev_dev->fifo, &discarded, 1);
		if (!ret) {
			mutex_unlock(&miev_dev->lock);
			return -EIO;
		}
	}
	ret = kfifo_in(&miev_dev->fifo, kbuf, size);
	mutex_unlock(&miev_dev->lock);
	wake_up_interruptible(&miev_dev->wait_queue);
	return ret;
}

static long miev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	if (_IOC_TYPE(cmd) != MIEV_IOC_MAGIC)
		return -EINVAL;
	if (_IOC_NR(cmd) > MIEV_IOC_MAXNR)
		return -EINVAL;

	if (!access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	switch (cmd) {
	case MIEV_IOC_WRITE:
		pr_devel("MIEV_IOC_WRITE");
		break;
	case MIEV_IOC_READ:
		pr_devel("MIEV_IOC_READ");
		break;
	case MIEV_IOC_NONE:
		pr_devel("miev ioctl call success");
		break;
	default:
		return -ENOTTY;
	}
	return ret;
}

static __poll_t miev_poll(struct file *filp, poll_table *wait)
{
	__poll_t mask = 0;
	poll_wait(filp, &miev_dev->wait_queue, wait);
	mutex_lock(&miev_dev->lock);
	if (!kfifo_is_empty(&miev_dev->fifo)) {
		mask |= POLLIN | POLLRDNORM;
	}
	mutex_unlock(&miev_dev->lock);
	return mask;
}

static ssize_t miev_read(struct file *filp, char __user *ubuf, size_t size,
			 loff_t *offset)
{
	unsigned int copied = 0;
	int ret = 0;

	if (!size)
		return 0;
	mutex_lock(&miev_dev->lock);
	if (!kfifo_is_empty(&miev_dev->fifo)) {
		if (size < kfifo_peek_len(&miev_dev->fifo))
			ret = -EMSGSIZE;
		else
			ret = kfifo_to_user(&miev_dev->fifo, ubuf, BUF_MAX_SIZE, &copied);
	}
	mutex_unlock(&miev_dev->lock);
	return ret ? ret : (ssize_t)copied;
}

static ssize_t miev_write(struct file *filp, const char __user *ubuf,
			  size_t size, loff_t *offset)
{
	char *buffer;
	int ret;

	if (!size)
		return 0;
	size = min_t(size_t, size, BUF_MAX_SIZE);
	/* A failed user copy must leave existing queued records intact. */
	buffer = memdup_user(ubuf, size);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	ret = write_kbuf(buffer, size);
	kfree(buffer);
	return ret;
}

static const struct file_operations miev_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.read = miev_read,
	.write = miev_write,
	.unlocked_ioctl = miev_ioctl,
	.poll = miev_poll,
};

static int __init miev_init_module(void)
{
	struct device *device;
	int ret;

	miev_dev = kzalloc(sizeof(*miev_dev), GFP_KERNEL);
	if (!miev_dev)
		return -ENOMEM;
	mutex_init(&miev_dev->lock);
	init_waitqueue_head(&miev_dev->wait_queue);
	ret = kfifo_alloc(&miev_dev->fifo, FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto free_device;

	miev_dev->dev_no = MKDEV(DEV_MAJOR, DEV_MINOR);
	ret = register_chrdev_region(miev_dev->dev_no, 1, DEV_NAME);
	if (ret) {
		ret = alloc_chrdev_region(&miev_dev->dev_no, 0, 1, DEV_NAME);
		if (ret)
			goto free_fifo;
	}
	miev_dev->my_class = class_create(DEV_NAME);
	if (IS_ERR(miev_dev->my_class)) {
		ret = PTR_ERR(miev_dev->my_class);
		goto unregister;
	}
	device = device_create(miev_dev->my_class, NULL, miev_dev->dev_no, NULL, DEV_NAME);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto destroy_class;
	}
	/* Publish file operations only after all resources are initialized. */
	cdev_init(&miev_dev->chrdev, &miev_fops);
	miev_dev->chrdev.owner = THIS_MODULE;
	ret = cdev_add(&miev_dev->chrdev, miev_dev->dev_no, 1);
	if (!ret)
		return 0;
	device_destroy(miev_dev->my_class, miev_dev->dev_no);
destroy_class:
	class_destroy(miev_dev->my_class);
unregister:
	unregister_chrdev_region(miev_dev->dev_no, 1);
free_fifo:
	kfifo_free(&miev_dev->fifo);
free_device:
	kfree(miev_dev);
	return ret;
}

static void __exit miev_exit_module(void)
{
	/* The file-operations owner and dependent modules pin this storage. */
	cdev_del(&miev_dev->chrdev);
	device_destroy(miev_dev->my_class, miev_dev->dev_no);
	class_destroy(miev_dev->my_class);
	unregister_chrdev_region(miev_dev->dev_no, 1);
	kfifo_free(&miev_dev->fifo);
	kfree(miev_dev);
}

module_init(miev_init_module);
module_exit(miev_exit_module);

MODULE_AUTHOR("huangqibo <huangqibo@xiaomi.com>");
MODULE_DESCRIPTION("exception log transfer.");
MODULE_LICENSE("GPL");
