// SPDX-License-Identifier: GPL-2.0
/*
 * mca_platform_base.c
 *
 * mca platform base driver
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/atomic.h>
#include <linux/of.h>
#include <linux/gpio.h>
#include <mca/common/mca_log.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_platform_base"
#endif

struct mca_platform_base_device {
	struct device *dev;
	struct i2c_client **plat_i2c;
	struct regmap *rmap;
	struct mca_platform_class *platform_core;
	u8 *slave_addrs;
	int slave_addr_num;
};

static inline struct i2c_client *
bank_to_i2c(struct mca_platform_base_device *dev, u8 bank)
{
	if (bank >= dev->slave_addr_num)
		return NULL;
	return dev->plat_i2c[bank];
}

static int mca_platform_base_regmap_write(void *context, const void *data,
					  size_t count)
{
	struct mca_platform_base_device *dev = context;
	struct i2c_client *i2c;
	const u8 *_data = data;

	if (!data || count < 3 || count > I2C_SMBUS_BLOCK_MAX + 2)
		return -EINVAL;
	i2c = bank_to_i2c(dev, _data[0]);
	if (!i2c)
		return -EINVAL;

	return i2c_smbus_write_i2c_block_data(i2c, _data[1], count - 2,
					      _data + 2);
}

static int mca_platform_base_regmap_read(void *context, const void *reg_buf,
					 size_t reg_size, void *val_buf,
					 size_t val_size)
{
	int ret;
	struct mca_platform_base_device *dev = context;
	struct i2c_client *i2c;
	const u8 *_reg_buf = reg_buf;

	if (!reg_buf || reg_size != 2 || !val_buf || !val_size ||
	    val_size > I2C_SMBUS_BLOCK_MAX)
		return -EINVAL;
	i2c = bank_to_i2c(dev, _reg_buf[0]);
	if (!i2c)
		return -EINVAL;

	ret = i2c_smbus_read_i2c_block_data(i2c, _reg_buf[1], val_size,
					    val_buf);
	if (ret < 0)
		return ret;

	return ret != val_size ? -EIO : 0;
}

static const struct regmap_bus mca_platform_base_regmap_bus = {
	.write = mca_platform_base_regmap_write,
	.read = mca_platform_base_regmap_read,
};

static const struct regmap_config mca_platform_base_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
};

static int mca_platform_base_parse_dt(struct mca_platform_base_device *dev)
{
	struct device_node *node = dev->dev->of_node;
	int i, j, count, ret;

	count = of_property_count_u8_elems(node, "platform_slave_addrs");
	if (count <= 0 || count > 128)
		return -EINVAL;
	dev->slave_addrs = devm_kmalloc(dev->dev, count, GFP_KERNEL);
	dev->plat_i2c = devm_kcalloc(dev->dev, count, sizeof(*dev->plat_i2c), GFP_KERNEL);
	if (!dev->slave_addrs || !dev->plat_i2c)
		return -ENOMEM;
	ret = of_property_read_u8_array(node, "platform_slave_addrs", dev->slave_addrs, count);
	if (ret)
		return ret;
	for (i = 0; i < count; i++) {
		if (dev->slave_addrs[i] < 0x08 || dev->slave_addrs[i] > 0x77)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (dev->slave_addrs[i] == dev->slave_addrs[j])
				return -EINVAL;
	}
	dev->slave_addr_num = count;
	return 0;
}

static int mca_platform_base_probe(struct i2c_client *client)
{
	int i = 0, ret = 0;
	struct mca_platform_base_device *dev;

	mca_log_info("probe start\n");

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->dev = &client->dev;
	i2c_set_clientdata(client, dev);

	ret = mca_platform_base_parse_dt(dev);
	if (ret)
		return ret;

	for (i = 0; i < dev->slave_addr_num; i++) {
		if (dev->slave_addrs[i] == client->addr) {
			dev->plat_i2c[i] = client;
			continue;
		}
		dev->plat_i2c[i] = devm_i2c_new_dummy_device(
			dev->dev, client->adapter, dev->slave_addrs[i]);
		if (IS_ERR(dev->plat_i2c[i])) {
			mca_log_err("failed to create new i2c[0x%02x] dev\n",
				    dev->slave_addrs[i]);
			ret = PTR_ERR(dev->plat_i2c[i]);
			goto err;
		}
	}

	dev->rmap = devm_regmap_init(dev->dev, &mca_platform_base_regmap_bus,
				     dev, &mca_platform_base_regmap_config);
	if (IS_ERR(dev->rmap)) {
		mca_log_info("failed to init regmap\n");
		ret = PTR_ERR(dev->rmap);
		goto err;
	}

	mca_log_err("probe end\n");

	return devm_of_platform_populate(dev->dev);

err:
	return ret;
}

static const struct of_device_id mca_platform_base_of_match[] = {
	{
		.compatible = "mca,platform_base",
	},
	{},
};

MODULE_DEVICE_TABLE(of, mca_platform_base_of_match);

static struct i2c_driver mca_platform_base_driver = {
	.probe = mca_platform_base_probe,
	.driver = {
		.name = "mca_platform_base",
		.of_match_table = of_match_ptr(mca_platform_base_of_match),
	},
};
module_i2c_driver(mca_platform_base_driver);

MODULE_DESCRIPTION("mca platform base");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
