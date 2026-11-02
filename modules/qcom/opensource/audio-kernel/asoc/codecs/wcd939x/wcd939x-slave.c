// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/component.h>
#include <soc/soundwire.h>
#include "wcd939x.h"

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define SWR_SLV_MAX_REG_ADDR    0x2009
#define SWR_SLV_START_REG_ADDR  0x40
#define SWR_SLV_MAX_BUF_LEN     20
#define BYTES_PER_LINE          12
#define SWR_SLV_RD_BUF_LEN      8
#define SWR_SLV_WR_BUF_LEN      32
#define SWR_SLV_MAX_DEVICES     2
#endif /* CONFIG_DEBUG_FS */

#define SWR_MAX_RETRY    5

struct wcd939x_slave_priv {
	struct swr_device *swr_slave;
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_wcd939x_dent;
	struct dentry *debugfs_peek;
	struct dentry *debugfs_poke;
	struct dentry *debugfs_reg_dump;
	unsigned int read_data;
#endif
	int (*rst_notify)(void *);
	void *slv_handle;
};

/* Serializes callback publication, reset calls and drvdata withdrawal. */
static DEFINE_MUTEX(wcd939x_slave_lock);

int wcd939x_slave_register_notify(struct swr_device *pdev,
				  int (*notify)(void *), void *handle)
{
	struct wcd939x_slave_priv *priv;
	int ret = 0;

	if (!pdev)
		return -EINVAL;
	mutex_lock(&wcd939x_slave_lock);
	priv = swr_get_dev_data(pdev);
	if (priv) {
		priv->slv_handle = handle;
		priv->rst_notify = notify;
		ret = 1;
	}
	mutex_unlock(&wcd939x_slave_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd939x_slave_register_notify);

#ifdef CONFIG_DEBUG_FS

static int get_parameters(char *buf, u32 *params, int count)
{
	char *token;
	int i, base;

	for (i = 0; i < count; i++) {
		do {
			token = strsep(&buf, " \t\n");
		} while (token && !*token);
		if (!token)
			return -EINVAL;
		base = token[0] == '0' && (token[1] == 'x' || token[1] == 'X') ? 16 : 10;
		if (kstrtou32(token, base, &params[i]))
			return -EINVAL;
	}
	while ((token = strsep(&buf, " \t\n")))
		if (*token)
			return -EINVAL;
	return 0;
}

static bool is_swr_slv_reg_readable(int reg)
{
	int ret = true;

	if (((reg > 0x46) && (reg < 0x4A)) ||
	    ((reg > 0x4A) && (reg < 0x50)) ||
	    ((reg > 0x55) && (reg < 0xD0)) ||
	    ((reg > 0xD0) && (reg < 0xE0)) ||
	    ((reg > 0xE0) && (reg < 0xF0)) ||
	    ((reg > 0xF0) && (reg < 0x100)) ||
	    ((reg > 0x105) && (reg < 0x120)) ||
	    ((reg > 0x205) && (reg < 0x220)) ||
	    ((reg > 0x305) && (reg < 0x320)) ||
	    ((reg > 0x405) && (reg < 0x420)) ||
	    ((reg > 0x128) && (reg < 0x130)) ||
	    ((reg > 0x228) && (reg < 0x230)) ||
	    ((reg > 0x328) && (reg < 0x330)) ||
	    ((reg > 0x428) && (reg < 0x430)) ||
	    ((reg > 0x138) && (reg < 0x205)) ||
	    ((reg > 0x238) && (reg < 0x305)) ||
	    ((reg > 0x338) && (reg < 0x405)) ||
	    ((reg > 0x438) && (reg < 0x2000)))
		ret = false;

	return ret;
}

static int wcd939x_swrslave_reg_show(struct seq_file *seq, void *unused)
{
	struct swr_device *pdev = seq->private;
	u8 value;
	int reg, ret;

	for (reg = SWR_SLV_START_REG_ADDR; reg <= SWR_SLV_MAX_REG_ADDR; reg++) {
		if (!is_swr_slv_reg_readable(reg))
			continue;
		ret = swr_read(pdev, pdev->dev_num, reg, &value, 1);
		if (ret)
			return ret;
		seq_printf(seq, "0x%.3x: 0x%.2x\n", reg, value);
	}
	return 0;
}

static int codec_debug_dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, wcd939x_swrslave_reg_show, inode->i_private);
}

static ssize_t codec_debug_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char lbuf[SWR_SLV_RD_BUF_LEN];
	struct swr_device *pdev = NULL;
	struct wcd939x_slave_priv *wcd939x_slave = NULL;

	if (!count || !file || !ppos || !ubuf)
		return -EINVAL;

	pdev = file->private_data;
	if (!pdev)
		return -EINVAL;

	wcd939x_slave = swr_get_dev_data(pdev);
	if (!wcd939x_slave)
		return -EINVAL;

	if (*ppos < 0)
		return -EINVAL;

	snprintf(lbuf, sizeof(lbuf), "0x%x\n",
			(READ_ONCE(wcd939x_slave->read_data) & 0xFF));

	return simple_read_from_buffer(ubuf, count, ppos, lbuf,
					       strnlen(lbuf, 7));
}

static ssize_t codec_debug_peek_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct swr_device *pdev = file->private_data;
	struct wcd939x_slave_priv *priv = swr_get_dev_data(pdev);
	char input[SWR_SLV_WR_BUF_LEN];
	u32 reg;
	u8 value;
	int ret;

	if (!count)
		return 0;
	if (!priv || *ppos < 0 || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, ubuf, count))
		return -EFAULT;
	input[count] = '\0';
	ret = get_parameters(input, &reg, 1);
	if (ret || reg > SWR_SLV_MAX_REG_ADDR)
		return -EINVAL;
	ret = swr_read(pdev, pdev->dev_num, reg, &value, 1);
	if (ret)
		return ret;
	WRITE_ONCE(priv->read_data, value);
	return count;
}

static ssize_t codec_debug_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct swr_device *pdev = file->private_data;
	char input[SWR_SLV_WR_BUF_LEN];
	u32 params[2];
	u8 value;
	int ret;

	if (!count)
		return 0;
	if (!pdev || *ppos < 0 || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, ubuf, count))
		return -EFAULT;
	input[count] = '\0';
	ret = get_parameters(input, params, ARRAY_SIZE(params));
	if (ret || params[0] > SWR_SLV_MAX_REG_ADDR || params[1] > U8_MAX)
		return -EINVAL;
	value = params[1];
	ret = swr_write(pdev, pdev->dev_num, params[0], &value);
	return ret ? ret : count;
}

static const struct file_operations codec_debug_write_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = codec_debug_write,
};

static const struct file_operations codec_debug_read_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = codec_debug_read,
	.write = codec_debug_peek_write,
};

static const struct file_operations codec_debug_dump_ops = {
	.owner = THIS_MODULE,
	.open = codec_debug_dump_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int wcd939x_slave_bind(struct device *dev,
				struct device *master, void *data)
{
	int ret = 0;
	uint8_t devnum = 0;
	struct swr_device *pdev = to_swr_device(dev);
	int retry = SWR_MAX_RETRY;

	if (!pdev) {
		pr_err_ratelimited("%s: invalid swr device handle\n", __func__);
		return -EINVAL;
	}

	do {
		/* Add delay for soundwire enumeration */
		usleep_range(100, 110);
		ret = swr_get_logical_dev_num(pdev, pdev->addr, &devnum);
	} while (ret && --retry);

	if (ret) {
		dev_dbg(&pdev->dev,
			"%s get devnum %d for dev addr %llx failed\n",
			__func__, devnum, pdev->addr);
		ret = -EPROBE_DEFER;
		return ret;
	}
	pdev->dev_num = devnum;

	return ret;
}

static void wcd939x_slave_unbind(struct device *dev,
				struct device *master, void *data)
{
	struct wcd939x_slave_priv *wcd939x_slave = NULL;
	struct swr_device *pdev = to_swr_device(dev);

	wcd939x_slave = swr_get_dev_data(pdev);
	if (!wcd939x_slave) {
		dev_err_ratelimited(&pdev->dev, "%s: wcd939x_slave is NULL\n", __func__);
		return;
	}
}

static const struct swr_device_id wcd939x_swr_id[] = {
	{"wcd939x-slave", 0},
	{}
};

static const struct of_device_id wcd939x_swr_dt_match[] = {
	{
		.compatible = "qcom,wcd939x-slave",
	},
	{}
};

static const struct component_ops wcd939x_slave_comp_ops = {
	.bind   = wcd939x_slave_bind,
	.unbind = wcd939x_slave_unbind,
};

static int wcd939x_swr_probe(struct swr_device *pdev)
{
	struct wcd939x_slave_priv *wcd939x_slave = NULL;
	int ret;

	wcd939x_slave = devm_kzalloc(&pdev->dev,
				sizeof(struct wcd939x_slave_priv), GFP_KERNEL);
	if (!wcd939x_slave)
		return -ENOMEM;

	mutex_lock(&wcd939x_slave_lock);
	swr_set_dev_data(pdev, wcd939x_slave);
	mutex_unlock(&wcd939x_slave_lock);

	wcd939x_slave->swr_slave = pdev;
	ret = component_add(&pdev->dev, &wcd939x_slave_comp_ops);
	if (ret) {
		mutex_lock(&wcd939x_slave_lock);
		swr_set_dev_data(pdev, NULL);
		mutex_unlock(&wcd939x_slave_lock);
		return ret;
	}

#ifdef CONFIG_DEBUG_FS
	if (!wcd939x_slave->debugfs_wcd939x_dent) {
		wcd939x_slave->debugfs_wcd939x_dent = debugfs_create_dir(
						dev_name(&pdev->dev), 0);
		if (!IS_ERR_OR_NULL(wcd939x_slave->debugfs_wcd939x_dent)) {
			wcd939x_slave->debugfs_peek =
					debugfs_create_file("swrslave_peek",
					S_IFREG | 0444,
					wcd939x_slave->debugfs_wcd939x_dent,
					(void *) pdev,
					&codec_debug_read_ops);

			wcd939x_slave->debugfs_poke =
					debugfs_create_file("swrslave_poke",
					S_IFREG | 0444,
					wcd939x_slave->debugfs_wcd939x_dent,
					(void *) pdev,
					&codec_debug_write_ops);

			wcd939x_slave->debugfs_reg_dump =
					debugfs_create_file(
					"swrslave_reg_dump",
					S_IFREG | 0444,
					wcd939x_slave->debugfs_wcd939x_dent,
					(void *) pdev,
					&codec_debug_dump_ops);
                }
        }
#endif

	return 0;
}


static int wcd939x_swr_reset(struct swr_device *pdev)
{
	struct wcd939x_slave_priv *priv;
	int ret = 0;

	mutex_lock(&wcd939x_slave_lock);
	priv = swr_get_dev_data(pdev);
	if (priv && priv->rst_notify)
		ret = priv->rst_notify(priv->slv_handle);
	mutex_unlock(&wcd939x_slave_lock);
	return ret;
}

static int wcd939x_swr_remove(struct swr_device *pdev)
{
#ifdef CONFIG_DEBUG_FS
	struct wcd939x_slave_priv *wcd939x_slave = swr_get_dev_data(pdev);

	if (wcd939x_slave) {
		debugfs_remove_recursive(wcd939x_slave->debugfs_wcd939x_dent);
		wcd939x_slave->debugfs_wcd939x_dent = NULL;
	}
#endif
	component_del(&pdev->dev, &wcd939x_slave_comp_ops);
	mutex_lock(&wcd939x_slave_lock);
	swr_set_dev_data(pdev, NULL);
	mutex_unlock(&wcd939x_slave_lock);

	return 0;
}

static struct swr_driver wcd939x_slave_driver = {
	.driver = {
		.name = "wcd939x-slave",
		.owner = THIS_MODULE,
		.of_match_table = wcd939x_swr_dt_match,
	},
	.probe = wcd939x_swr_probe,
	.remove = wcd939x_swr_remove,
	.reset_device = wcd939x_swr_reset,
	.id_table = wcd939x_swr_id,
};

static int __init wcd939x_slave_init(void)
{
	return swr_driver_register(&wcd939x_slave_driver);
}

static void __exit wcd939x_slave_exit(void)
{
	swr_driver_unregister(&wcd939x_slave_driver);
}

module_init(wcd939x_slave_init);
module_exit(wcd939x_slave_exit);

MODULE_DESCRIPTION("WCD939X Swr Slave driver");
MODULE_LICENSE("GPL v2");
