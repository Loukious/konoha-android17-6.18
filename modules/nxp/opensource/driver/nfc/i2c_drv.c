/******************************************************************************
 * Copyright (C) 2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013-2022 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ******************************************************************************/
/*
 * Copyright (C) 2010 Trusted Logic S.A.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 ****************************************************************************/
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 ****************************************************************************/

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif
#include "common.h"

/**
 * i2c_disable_irq()
 *
 * Check if interrupt is disabled or not
 * and disable interrupt
 *
 * Return: int
 */
int i2c_disable_irq(struct nfc_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->i2c_dev.irq_enabled_lock, flags);
	if (dev->i2c_dev.irq_enabled) {
		disable_irq_nosync(dev->i2c_dev.client->irq);
		dev->i2c_dev.irq_enabled = false;
	}
	spin_unlock_irqrestore(&dev->i2c_dev.irq_enabled_lock, flags);

	return 0;
}

/**
 * i2c_enable_irq()
 *
 * Check if interrupt is enabled or not
 * and enable interrupt
 *
 * Return: int
 */
int i2c_enable_irq(struct nfc_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->i2c_dev.irq_enabled_lock, flags);
	if (!dev->irq_requested || READ_ONCE(dev->removing)) {
		spin_unlock_irqrestore(&dev->i2c_dev.irq_enabled_lock, flags);
		return -ENODEV;
	}
	if (!dev->i2c_dev.irq_enabled) {
		dev->i2c_dev.irq_enabled = true;
		enable_irq(dev->i2c_dev.client->irq);
	}
	spin_unlock_irqrestore(&dev->i2c_dev.irq_enabled_lock, flags);

	return 0;
}

static irqreturn_t i2c_irq_handler(int irq, void *dev_id)
{
	struct nfc_dev *nfc_dev = dev_id;
	struct i2c_dev *i2c_dev = &nfc_dev->i2c_dev;

	if (device_may_wakeup(&i2c_dev->client->dev))
		pm_wakeup_event(&i2c_dev->client->dev, WAKEUP_SRC_TIMEOUT);

	i2c_disable_irq(nfc_dev);
	wake_up(&nfc_dev->read_wq);

	return IRQ_HANDLED;
}

int i2c_read(struct nfc_dev *nfc_dev, char *buf, size_t count, int timeout)
{
	int ret;
	struct i2c_dev *i2c_dev = &nfc_dev->i2c_dev;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	uint16_t i = 0;
	uint16_t disp_len = GET_IPCLOG_MAX_PKT_LEN(count);

	pr_debug("NxpDrv: %s: reading %zu bytes.\n", __func__, count);

	if (READ_ONCE(nfc_dev->removing))
		return -ENODEV;
	if (!count)
		return 0;
	if (timeout < 0)
		return -EINVAL;
	if (timeout > NCI_CMD_RSP_TIMEOUT_MS)
		timeout = NCI_CMD_RSP_TIMEOUT_MS;

	if (count > MAX_NCI_BUFFER_SIZE)
		count = MAX_NCI_BUFFER_SIZE;

	if (!gpio_get_value(nfc_gpio->irq)) {
		while (1) {
			ret = 0;
			ret = i2c_enable_irq(nfc_dev);
			if (ret)
				return ret;
			if (!gpio_get_value(nfc_gpio->irq)) {
				if (timeout) {
					ret = wait_event_interruptible_timeout(
						nfc_dev->read_wq,
						!READ_ONCE(i2c_dev->irq_enabled) ||
						READ_ONCE(nfc_dev->release_read) ||
						READ_ONCE(nfc_dev->removing),
						msecs_to_jiffies(timeout));
					if (ret <= 0) {
						ret = ret ? ret : -ETIMEDOUT;
						pr_err("NxpDrv: %s: timeout error\n",
						       __func__);
						goto err;
					}
				} else {
					ret = wait_event_interruptible(
						nfc_dev->read_wq,
						!READ_ONCE(i2c_dev->irq_enabled) ||
						READ_ONCE(nfc_dev->release_read) ||
						READ_ONCE(nfc_dev->removing));
					if (ret) {
						pr_err("NxpDrv: %s: err wakeup of wq\n",
						       __func__);
						goto err;
					}
				}
			}
			i2c_disable_irq(nfc_dev);
			if (READ_ONCE(nfc_dev->removing))
				return -ENODEV;
			if (READ_ONCE(nfc_dev->release_read))
				return 0;

			if (gpio_get_value(nfc_gpio->irq))
				break;
			if(!nfc_dev->secure_zone) {
				if (!gpio_get_value(nfc_gpio->ven)) {
					pr_info("NxpDrv: %s: releasing read\n", __func__);
					ret = -EIO;
					goto err;
				}
			}
			/*
			 * NFC service wanted to close the driver so,
			 * release the calling reader thread asap.
			 *
			 * This can happen in case of nfc node close call from
			 * eSE HAL in that case the NFC HAL reader thread
			 * will again call read system call
			 */
			if (nfc_dev->release_read) {
				pr_debug("NxpDrv: %s: releasing read\n", __func__);
				return 0;
			}
			pr_warn("NxpDrv: %s: spurious interrupt detected\n", __func__);
		}
	}

	memset(buf, 0x00, count);
	/* Read data */
	ret = i2c_master_recv(nfc_dev->i2c_dev.client, buf, count);
	NFCLOG_IPC(nfc_dev, false, "%s of %zu bytes, ret %d", __func__, count,
								ret);
	if (ret <= 0) {
		pr_err("NxpDrv: %s: returned %d\n", __func__, ret);
		goto err;
	}

	for (i = 0; i < min_t(int, disp_len, ret); i++)
		NFCLOG_IPC(nfc_dev, false, " %02x", buf[i]);

	if (nfc_cold_reset_response(nfc_dev, buf, ret))
		return 0;

err:
	return ret;
}

int i2c_write(struct nfc_dev *nfc_dev, const char *buf, size_t count,
	      int max_retry_cnt)
{
	int ret = -EINVAL;
	int retry_cnt;
	uint16_t i = 0;
	uint16_t disp_len = GET_IPCLOG_MAX_PKT_LEN(count);
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;

	if (count > MAX_DL_BUFFER_SIZE)
		count = MAX_DL_BUFFER_SIZE;

	pr_debug("NxpDrv: %s: writing %zu bytes.\n", __func__, count);
	NFCLOG_IPC(nfc_dev, false, "%s sending %zu B", __func__, count);

	for (i = 0; i < disp_len; i++)
	    NFCLOG_IPC(nfc_dev, false, " %02x", buf[i]);
	/*
	 * Wait for any pending read for max 15ms before write
	 * This is to avoid any packet corruption during read, when
	 * the host cmds resets NFCC during any parallel read operation
	 */
	for (retry_cnt = 1; retry_cnt <= MAX_WRITE_IRQ_COUNT; retry_cnt++) {
		if (gpio_get_value(nfc_gpio->irq)) {
			pr_warn("NxpDrv: %s: irq high during write, wait\n", __func__);
			usleep_range(WRITE_RETRY_WAIT_TIME_US,
				     WRITE_RETRY_WAIT_TIME_US + 100);
		} else {
			break;
		}
		if (retry_cnt == MAX_WRITE_IRQ_COUNT &&
		    gpio_get_value(nfc_gpio->irq)) {
			pr_warn("NxpDrv: %s: allow after maximum wait\n", __func__);
		}
	}

	for (retry_cnt = 1; retry_cnt <= max_retry_cnt; retry_cnt++) {
		ret = i2c_master_send(nfc_dev->i2c_dev.client, buf, count);
		NFCLOG_IPC(nfc_dev, false, "%s ret %d", __func__, ret);
		if (ret <= 0) {
			pr_warn("NxpDrv: %s: write failed ret(%d), maybe in standby\n",
				__func__, ret);
			usleep_range(WRITE_RETRY_WAIT_TIME_US,
				     WRITE_RETRY_WAIT_TIME_US + 100);
		} else if (ret != count) {
			pr_err("NxpDrv: %s: failed to write %d\n", __func__, ret);
			ret = -EIO;
		} else if (ret == count)
			break;
	}
	return ret;
}

static ssize_t nfc_i2c_dev_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *offset)
{
	guard(srcu)(&nfc_io_srcu);
	int ret;
	struct nfc_dev *nfc_dev = (struct nfc_dev *)filp->private_data;

	if (!nfc_dev || READ_ONCE(nfc_dev->removing)) {
		pr_err("NxpDrv: %s: device doesn't exist anymore\n", __func__);
		return -ENODEV;
	}
	if (!count)
		return 0;
	mutex_lock(&nfc_dev->read_mutex);
	if (count > MAX_NCI_BUFFER_SIZE)
		count = MAX_NCI_BUFFER_SIZE;

	if (filp->f_flags & O_NONBLOCK) {
		ret = i2c_master_recv(nfc_dev->i2c_dev.client, nfc_dev->read_kbuf, count);
		if (ret > 0 && nfc_cold_reset_response(nfc_dev, nfc_dev->read_kbuf, ret))
			ret = 0;
		pr_debug("NxpDrv: %s: NONBLOCK read ret = %d\n", __func__, ret);
	} else {
		ret = i2c_read(nfc_dev, nfc_dev->read_kbuf, count, 0);
	}
	if (ret > 0) {
		if (copy_to_user(buf, nfc_dev->read_kbuf, ret)) {
			pr_warn("NxpDrv: %s: failed to copy to user space\n", __func__);
			ret = -EFAULT;
		}
	}
	mutex_unlock(&nfc_dev->read_mutex);
	return ret;
}

static ssize_t nfc_i2c_dev_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *offset)
{
	guard(srcu)(&nfc_io_srcu);
	int ret;
	struct nfc_dev *nfc_dev = (struct nfc_dev *)filp->private_data;

	if (count > MAX_DL_BUFFER_SIZE)
		count = MAX_DL_BUFFER_SIZE;

	if (!nfc_dev || READ_ONCE(nfc_dev->removing)) {
		pr_err("NxpDrv: %s: device doesn't exist anymore\n", __func__);
		return -ENODEV;
	}
	if (!count)
		return 0;

	mutex_lock(&nfc_dev->write_mutex);
	if (copy_from_user(nfc_dev->write_kbuf, buf, count)) {
		pr_err("NxpDrv: %s: failed to copy from user space\n", __func__);
		mutex_unlock(&nfc_dev->write_mutex);
		return -EFAULT;
	}
	ret = i2c_write(nfc_dev, nfc_dev->write_kbuf, count, NO_RETRY);
	mutex_unlock(&nfc_dev->write_mutex);
	return ret;
}

static const struct file_operations nfc_i2c_dev_fops = {
	.owner = THIS_MODULE,
	.read = nfc_i2c_dev_read,
	.write = nfc_i2c_dev_write,
	.open = nfc_dev_open,
	.flush = nfc_dev_flush,
	.release = nfc_dev_close,
	.unlocked_ioctl = nfc_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = nfc_dev_compat_ioctl,
#endif
};


static void nfc_i2c_cleanup(struct nfc_dev *dev)
{
	struct i2c_client *client = dev->i2c_dev.client;

	nfc_dev_unpublish(dev);
	nfc_misc_unregister(dev, DEV_COUNT);
	if (dev->irq_requested) {
		if (dev->i2c_dev.irq_wake_up)
			disable_irq_wake(client->irq);
		free_irq(client->irq, dev);
		dev->irq_requested = false;
	}
	device_init_wakeup(&client->dev, false);
	if (dev->gpio_owned & BIT(1))
		set_valid_gpio(dev->configs.gpio.ven, 0);
	if (dev->reg) {
		nfc_ldo_unvote(dev);
		regulator_put(dev->reg);
		dev->reg = NULL;
	}
	gpio_free_all(dev);
	if (dev->ipcl) {
		ipc_log_context_destroy(dev->ipcl);
		dev->ipcl = NULL;
	}
	i2c_set_clientdata(client, NULL);
}

int nfc_i2c_dev_probe(struct i2c_client *client)
{
	struct nfc_dev *dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->ref);
	dev->i2c_dev.client = client;
	dev->interface = PLATFORM_IF_I2C;
	dev->nfc_state = NFC_STATE_UNKNOWN;
	dev->nfc_read = i2c_read;
	dev->nfc_write = i2c_write;
	dev->nfc_enable_intr = i2c_enable_irq;
	dev->nfc_disable_intr = i2c_disable_irq;
	mutex_init(&dev->read_mutex);
	mutex_init(&dev->write_mutex);
	mutex_init(&dev->dev_ref_mutex);
	mutex_init(&dev->cold_reset.lock);
	init_waitqueue_head(&dev->read_wq);
	init_waitqueue_head(&dev->cold_reset.read_wq);
	spin_lock_init(&dev->i2c_dev.irq_enabled_lock);
	dev->cold_reset.last_src_ese_prot = ESE_COLD_RESET_ORIGIN_NONE;
	ret = nfc_parse_dt(&client->dev, &dev->configs, PLATFORM_IF_I2C);
	if (ret)
		goto fail;
	dev->read_kbuf = kzalloc(MAX_NCI_BUFFER_SIZE, GFP_DMA | GFP_KERNEL);
	dev->write_kbuf = kzalloc(MAX_DL_BUFFER_SIZE, GFP_DMA | GFP_KERNEL);
	if (!dev->read_kbuf || !dev->write_kbuf) {
		ret = -ENOMEM;
		goto fail;
	}
	dev->ipcl = ipc_log_context_create(NUM_OF_IPC_LOG_PAGES, NFC_CHAR_DEV_NAME, 0);
	ret = configure_gpio(dev->configs.gpio.irq, GPIO_IRQ);
	if (ret < 0)
		goto fail;
	dev->gpio_owned |= BIT(0);
	client->irq = ret;
	ret = request_irq(client->irq, i2c_irq_handler,
			  IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN, client->name, dev);
	if (ret)
		goto fail;
	dev->irq_requested = true;
	ret = nfc_ldo_config(&client->dev, dev);
	if (ret)
		goto fail;
#ifdef NFC_SECURE_PERIPHERAL_ENABLED
	if (dev->configs.CNSS_NFC_HW_SECURE_ENABLE)
		dev->secure_zone = nfc_hw_secure_check();
#endif
	if (!dev->secure_zone) {
		ret = nfc_post_init(dev);
		if (ret)
			goto fail;
	}
	ret = device_init_wakeup(&client->dev, true);
	if (ret)
		goto fail;
	i2c_set_clientdata(client, dev);
	ret = nfc_misc_register(dev, &nfc_i2c_dev_fops, DEV_COUNT,
				NFC_CHAR_DEV_NAME, CLASS_NAME);
	if (ret)
		goto fail;
	ret = nfc_dev_publish(dev);
	if (ret)
		goto fail;
	return 0;
fail:
	nfc_i2c_cleanup(dev);
	nfc_dev_put(dev);
	return ret;
}

void nfc_i2c_dev_remove(struct i2c_client *client)
{
	struct nfc_dev *dev = i2c_get_clientdata(client);

	if (!dev)
		return;
	nfc_i2c_cleanup(dev);
	nfc_dev_put(dev);
}

int nfc_i2c_dev_suspend(struct device *device)
{
	struct i2c_client *client = to_i2c_client(device);
	struct nfc_dev *nfc_dev = i2c_get_clientdata(client);
	struct i2c_dev *i2c_dev = NULL;
	if (!nfc_dev) {
		pr_err("NxpDrv: %s: device doesn't exist anymore\n", __func__);
		return -ENODEV;
	}
	i2c_dev = &nfc_dev->i2c_dev;

	NFCLOG_IPC(nfc_dev, false, "%s: irq_enabled = %d", __func__,
							i2c_dev->irq_enabled);

	if (device_may_wakeup(&client->dev) && READ_ONCE(i2c_dev->irq_enabled) &&
	    !i2c_dev->irq_wake_up) {
		int ret = enable_irq_wake(client->irq);

		if (ret)
			return ret;
		i2c_dev->irq_wake_up = true;
	}
	pr_debug("NxpDrv: %s: irq_wake_up = %d", __func__, i2c_dev->irq_wake_up);
	return 0;
}

int nfc_i2c_dev_resume(struct device *device)
{
	struct i2c_client *client = to_i2c_client(device);
	struct nfc_dev *nfc_dev = i2c_get_clientdata(client);
	struct i2c_dev *i2c_dev = NULL;
	if (!nfc_dev) {
		pr_err("NxpDrv: %s: device doesn't exist anymore\n", __func__);
		return -ENODEV;
	}
	i2c_dev = &nfc_dev->i2c_dev;

	NFCLOG_IPC(nfc_dev, false, "%s: irq_wake_up = %d", __func__,
							i2c_dev->irq_wake_up);

	if (i2c_dev->irq_wake_up) {
		int ret = disable_irq_wake(client->irq);

		if (ret)
			return ret;
		i2c_dev->irq_wake_up = false;
	}
	pr_debug("NxpDrv: %s: irq_wake_up = %d", __func__, i2c_dev->irq_wake_up);
	return 0;
}

static const struct i2c_device_id nfc_i2c_dev_id[] = { { NFC_I2C_DEV_ID, 0 },
						       {} };

static const struct of_device_id nfc_i2c_dev_match_table[] = {
	{
		.compatible = NFC_I2C_DRV_STR,
	},
	{}
};

static const struct dev_pm_ops nfc_i2c_dev_pm_ops = { SET_SYSTEM_SLEEP_PM_OPS(
	nfc_i2c_dev_suspend, nfc_i2c_dev_resume) };

static struct i2c_driver nfc_i2c_dev_driver = {
	.id_table = nfc_i2c_dev_id,
	.probe = nfc_i2c_dev_probe,
	.remove = nfc_i2c_dev_remove,
	.driver = {
		.name = NFC_I2C_DRV_STR,
		.pm = &nfc_i2c_dev_pm_ops,
		.of_match_table = nfc_i2c_dev_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

MODULE_DEVICE_TABLE(of, nfc_i2c_dev_match_table);

static int __init nfc_i2c_dev_init(void)
{
	int ret = 0;

	pr_info("NxpDrv: %s: Loading NXP NFC I2C driver\n", __func__);
	ret = i2c_add_driver(&nfc_i2c_dev_driver);
	if (ret != 0)
		pr_err("NxpDrv: %s: NFC I2C add driver error ret %d\n", __func__, ret);
	return ret;
}

module_init(nfc_i2c_dev_init);

static void __exit nfc_i2c_dev_exit(void)
{
	pr_info("NxpDrv: %s: Unloading NXP NFC I2C driver\n", __func__);
	i2c_del_driver(&nfc_i2c_dev_driver);
}

module_exit(nfc_i2c_dev_exit);

MODULE_DESCRIPTION("NXP NFC I2C driver");
MODULE_LICENSE("GPL");
