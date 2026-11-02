// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/soc/qcom/wcd939x-i2c.h>
#include "wcd-usbss-priv.h"

#define REG_BYTES 2
#define PAGE_REG_ADDR 0x00
#define WCD_USBSS_PAGE_SIZE 256

static int wcd_usbss_i2c_transfer(struct wcd_usbss_ctxt *ctxt,
				struct i2c_msg *messages, int count)
{
	int ret, retry;

	for (retry = 0; retry < 2; retry++) {
		ret = i2c_transfer(ctxt->client->adapter, messages, count);
		if (ret == count)
			return 0;
	}
	ctxt->prev_pg_valid = false;
	return ret < 0 ? ret : -EIO;
}

static int wcd_usbss_i2c_write_device(struct wcd_usbss_ctxt *ctxt, u16 reg,
				     const u8 *value, size_t bytes)
{
	struct i2c_client *client;
	struct i2c_msg msg;
	u8 *data;
	int ret;

	if (!ctxt || !ctxt->client)
		return -ENODEV;
	if (!value || !bytes || reg >= WCD_USBSS_PAGE_SIZE ||
	    bytes > WCD_USBSS_PAGE_SIZE - reg)
		return -EINVAL;
	client = ctxt->client;
	data = kmalloc(bytes + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data[0] = reg;
	memcpy(data + 1, value, bytes);
	msg.addr = client->addr;
	msg.flags = client->flags & I2C_CLIENT_TEN ? I2C_M_TEN : 0;
	msg.len = bytes + 1;
	msg.buf = data;
	ret = wcd_usbss_i2c_transfer(ctxt, &msg, 1);
	if (!reg)
		ctxt->prev_pg_valid = false;
	kfree(data);
	return ret;
}

static int wcd_usbss_i2c_read_device(struct wcd_usbss_ctxt *ctxt, u16 reg,
				    size_t bytes, u8 *dest)
{
	struct i2c_client *client;
	struct i2c_msg messages[2];
	size_t i;
	u8 address;
	int ret;

	if (!ctxt || !ctxt->client)
		return -ENODEV;
	if (!dest || !bytes || reg >= WCD_USBSS_PAGE_SIZE ||
	    bytes > WCD_USBSS_PAGE_SIZE - reg)
		return -EINVAL;
	client = ctxt->client;
	messages[0].addr = client->addr;
	messages[0].flags = client->flags & I2C_CLIENT_TEN ? I2C_M_TEN : 0;
	messages[0].len = 1;
	messages[0].buf = &address;
	messages[1] = messages[0];
	messages[1].flags |= I2C_M_RD;
	for (i = 0; i < bytes; i++) {
		address = reg + i;
		messages[1].buf = dest + i;
		ret = wcd_usbss_i2c_transfer(ctxt, messages, ARRAY_SIZE(messages));
		if (ret)
			return ret;
	}
	return 0;
}

/* The caller holds io_lock across page selection and the data transfer. */
static int wcd_usbss_page_write(struct wcd_usbss_ctxt *ctxt, u16 *reg)
{
	u8 page = *reg >> 8;
	int ret;

	if (!ctxt->prev_pg_valid || ctxt->prev_pg != page) {
		ret = wcd_usbss_i2c_write_device(ctxt, PAGE_REG_ADDR, &page, 1);
		if (ret)
			return ret;
		ctxt->prev_pg = page;
		ctxt->prev_pg_valid = true;
	}
	*reg &= 0xff;
	return 0;
}

static int wcd_usbss_regmap_transfer(void *context, const void *reg, size_t reg_size,
				     void *value, size_t value_size, bool write)
{
	struct device *dev = context;
	struct wcd_usbss_ctxt *ctxt = dev_get_drvdata(dev);
	u16 address, offset;
	size_t chunk;
	u8 *cursor = value;
	int ret = 0;

	if (!ctxt)
		return -ENODEV;
	if (!reg || !value || reg_size != REG_BYTES || !value_size)
		return -EINVAL;
	/* Regmap uses native-endian addresses; its buffer need not be aligned. */
	memcpy(&address, reg, sizeof(address));
	if (address > WCD_USBSS_MAX_REGISTER ||
	    value_size > WCD_USBSS_MAX_REGISTER - address + 1)
		return -EINVAL;

	mutex_lock(&ctxt->io_lock);
	while (value_size) {
		offset = address;
		ret = wcd_usbss_page_write(ctxt, &offset);
		if (ret)
			break;
		chunk = min_t(size_t, value_size, WCD_USBSS_PAGE_SIZE - offset);
		if (write && !offset)
			chunk = 1;
		if (write)
			ret = wcd_usbss_i2c_write_device(ctxt, offset, cursor, chunk);
		else
			ret = wcd_usbss_i2c_read_device(ctxt, offset, chunk, cursor);
		if (ret)
			break;
		address += chunk;
		cursor += chunk;
		value_size -= chunk;
	}
	mutex_unlock(&ctxt->io_lock);
	return ret;
}

static int regmap_bus_read(void *context, const void *reg, size_t reg_size,
			   void *val, size_t val_size)
{
	return wcd_usbss_regmap_transfer(context, reg, reg_size, val, val_size, false);
}

static int regmap_bus_gather_write(void *context, const void *reg, size_t reg_size,
				   const void *val, size_t val_size)
{
	return wcd_usbss_regmap_transfer(context, reg, reg_size, (void *)val, val_size, true);
}

static int regmap_bus_write(void *context, const void *data, size_t count)
{
	if (!data || count <= REG_BYTES)
		return -EINVAL;
	return regmap_bus_gather_write(context, data, REG_BYTES,
				       (const u8 *)data + REG_BYTES, count - REG_BYTES);
}

static const struct regmap_bus regmap_bus_config = {
	.write = regmap_bus_write,
	.gather_write = regmap_bus_gather_write,
	.read = regmap_bus_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

struct regmap *wcd_usbss_regmap_init(struct device *dev,
				   const struct regmap_config *config)
{
	return devm_regmap_init(dev, &regmap_bus_config, dev, config);
}
