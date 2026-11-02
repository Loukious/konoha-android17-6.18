// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <asoc/sdca-registers-api.h>
#include <linux/component.h>

#define RW_BUF_LEN (20)
#define SDCA_REG_BASE (0x40000000)
#define BYTES_PER_LINE (17)

static ssize_t codec_debug_address_read(struct file *file, char __user *ubuf,
					size_t count, loff_t *ppos)
{
	char buf[RW_BUF_LEN];
	int len;
	struct sdca_regdump_info *regdump_info = NULL;
	struct snd_soc_component *component = NULL;

	if (!file || !ppos || !ubuf)
		return -EINVAL;

	regdump_info = file->private_data;
	if (!regdump_info)
		return -EINVAL;

	component = regdump_info->component;
	if (!component)
		return -EINVAL;

	if (*ppos < 0)
		return -EINVAL;

	len = scnprintf(buf, sizeof(buf), "0x%x\n",
			READ_ONCE(regdump_info->reg_address));
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t codec_debug_address_write(struct file *file,
					 const char __user *ubuf, size_t cnt,
					 loff_t *ppos)
{
	char buf[RW_BUF_LEN];
	int rc = 0, ret = 0;
	u32 address;
	struct sdca_regdump_info *regdump_info = NULL;
	struct snd_soc_component *component = NULL;

	if (!file || !ppos || !ubuf)
		return -EINVAL;

	regdump_info = file->private_data;
	if (!regdump_info)
		return -EINVAL;

	component = regdump_info->component;
	if (!component)
		return -EINVAL;

	if (cnt > sizeof(buf) - 1)
		return -EINVAL;

	rc = copy_from_user(buf, ubuf, cnt);
	if (rc)
		return -EFAULT;

	buf[cnt] = '\0';
	ret = kstrtouint(buf, 0, &address);
	if (ret) {
		dev_dbg(component->dev, "%s: convert data string failed\n",
			__func__);
		return -EINVAL;
	}
	WRITE_ONCE(regdump_info->reg_address, address);
	return cnt;
}

static ssize_t codec_debug_data_read(struct file *file, char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char buf[RW_BUF_LEN];
	struct sdca_regdump_info *regdump_info = NULL;
	struct snd_soc_component *component = NULL;
	int reg_val = 0;
	int len;
	u32 address;

	if (!file || !ppos || !ubuf)
		return -EINVAL;

	regdump_info = file->private_data;
	if (!regdump_info)
		return -EINVAL;

	component = regdump_info->component;
	if (!component)
		return -EINVAL;

	if (*ppos < 0)
		return -EINVAL;

	if (!count)
		return 0;
	address = READ_ONCE(regdump_info->reg_address);
	if (address > SDCA_REG_BASE) {
		reg_val = snd_soc_component_read(component,
						 address);
	} else {
		pr_err("%s: invalid reg_address\n", __func__);
		return -EINVAL;
	}

	len = scnprintf(buf, sizeof(buf), "0x%.8x: 0x%.2x\n",
			address, reg_val & 0xFF);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t codec_debug_data_write(struct file *file,
				      const char __user *ubuf, size_t cnt,
				      loff_t *ppos)
{
	char buf[RW_BUF_LEN];
	struct sdca_regdump_info *regdump_info = NULL;
	struct snd_soc_component *component = NULL;
	u32 data, address;
	int rc = 0, ret = 0;

	if (!file || !ppos || !ubuf)
		return -EINVAL;

	regdump_info = file->private_data;
	if (!regdump_info)
		return -EINVAL;

	component = regdump_info->component;
	if (!component)
		return -EINVAL;

	if (cnt > sizeof(buf) - 1)
		return -EINVAL;

	rc = copy_from_user(buf, ubuf, cnt);
	if (rc)
		return -EFAULT;

	buf[cnt] = '\0';
	ret = kstrtouint(buf, 0, &data);
	if (ret) {
		dev_dbg(component->dev, "%ss: convert data string failed\n",
			__func__);
		return -EINVAL;
	}

	address = READ_ONCE(regdump_info->reg_address);
	if (address > SDCA_REG_BASE) {
		ret = snd_soc_component_update_bits(component, address, 0xFF, data);
		if (ret < 0)
			return ret;
	} else {
		pr_err("%s: the reg isn't in the range\n", __func__);
		return -EINVAL;
	}

	return cnt;
}

static ssize_t codec_debug_registers_dump(struct file *file, char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct sdca_regdump_info *regdump_info = NULL;
	struct snd_soc_component *component = NULL;
	char tmp_buf[RW_BUF_LEN];
	int i, reg_val, len;
	loff_t offset;
	ssize_t copied;
	ssize_t total = 0;

	if (!file || !ppos || !ubuf || *ppos < 0)
		return -EINVAL;

	regdump_info = file->private_data;
	if (!regdump_info)
		return -EINVAL;

	component = regdump_info->component;
	if (!component)
		return -EINVAL;

	if (regdump_info->reg_num < 0 ||
	    (regdump_info->reg_num && !regdump_info->reg_array))
		return -EINVAL;

	while (count && *ppos < (loff_t)regdump_info->reg_num * BYTES_PER_LINE) {
		i = *ppos / BYTES_PER_LINE;
		offset = *ppos % BYTES_PER_LINE;
		reg_val = snd_soc_component_read(component,
						 regdump_info->reg_array[i]);
		len = scnprintf(tmp_buf, sizeof(tmp_buf), "0x%.8x: 0x%.2x\n",
				regdump_info->reg_array[i], (reg_val & 0xFF));
		copied = simple_read_from_buffer(ubuf + total, count,
						 &offset, tmp_buf, len);
		if (copied < 0)
			return total ? total : copied;
		if (!copied)
			break;
		total += copied;
		*ppos += copied;
		count -= copied;
	}

	return total;
}

static const struct file_operations codec_debug_address_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = codec_debug_address_read,
	.write = codec_debug_address_write,
};

static const struct file_operations codec_debug_data_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = codec_debug_data_read,
	.write = codec_debug_data_write,
};

static const struct file_operations codec_debug_registers_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = codec_debug_registers_dump,
};

void sdca_devices_debugfs_dentry_create(struct sdca_debugfs_info *debugfs_info,
					struct sdca_regdump_info *regdump_info)
{
	struct snd_soc_component *component;

	if (!debugfs_info || !regdump_info || !regdump_info->component)
		return;
	component = regdump_info->component;

	debugfs_info->sdca_dentry =
		debugfs_create_dir(dev_name(component->dev), 0);
	if (!IS_ERR_OR_NULL(debugfs_info->sdca_dentry)) {
		debugfs_info->address = debugfs_create_file(
			"address", S_IFREG | 0444, debugfs_info->sdca_dentry,
			(void *)regdump_info, &codec_debug_address_ops);
		if (IS_ERR_OR_NULL(debugfs_info->address))
			pr_err("%s: debugfs_create_file address failed\n",
			       __func__);

		debugfs_info->data = debugfs_create_file(
			"data", S_IFREG | 0444, debugfs_info->sdca_dentry,
			(void *)regdump_info, &codec_debug_data_ops);
		if (IS_ERR_OR_NULL(debugfs_info->data))
			pr_err("%s: debugfs_create_file data failed\n",
			       __func__);

		debugfs_info->registers = debugfs_create_file(
			"registers", S_IFREG | 0444, debugfs_info->sdca_dentry,
			(void *)regdump_info, &codec_debug_registers_ops);
		if (IS_ERR_OR_NULL(debugfs_info->registers))
			pr_err("%s: debugfs_create_file registers failed\n",
			       __func__);
	}
	if (IS_ERR_OR_NULL(debugfs_info->sdca_dentry) ||
	    IS_ERR_OR_NULL(debugfs_info->address) ||
	    IS_ERR_OR_NULL(debugfs_info->data) ||
	    IS_ERR_OR_NULL(debugfs_info->registers)) {
		sdca_devices_debugfs_dentry_remove(debugfs_info);
		return;
	}
	dev_info(component->dev, "%s: create files success\n", __func__);
}
EXPORT_SYMBOL_GPL(sdca_devices_debugfs_dentry_create);

void sdca_devices_debugfs_dentry_remove(struct sdca_debugfs_info *debugfs_info)
{
	if (debugfs_info) {
		debugfs_remove_recursive(debugfs_info->sdca_dentry);
		debugfs_info->sdca_dentry = NULL;
		debugfs_info->address = NULL;
		debugfs_info->data = NULL;
		debugfs_info->registers = NULL;
	}
}
EXPORT_SYMBOL_GPL(sdca_devices_debugfs_dentry_remove);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SDCA codec register debugfs access");
