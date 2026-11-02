// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018, 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>
#include <linux/limits.h>
#include "lpass-cdc.h"
#include "internal.h"

#define REG_BYTES 2
#define VAL_BYTES 1

const u16 macro_id_base_offset[MAX_MACRO] = {
	TX_START_OFFSET,
	RX_START_OFFSET,
	WSA_START_OFFSET,
	VA_START_OFFSET,
	WSA2_START_OFFSET,
};

int lpass_cdc_get_macro_id(bool va_no_dec_flag, u16 reg)
{
	if (reg >= TX_START_OFFSET
		&& reg <= TX_MAX_OFFSET)
		return TX_MACRO;
	if (reg >= RX_START_OFFSET
		&& reg <= RX_MAX_OFFSET)
		return RX_MACRO;
	if (reg >= WSA_START_OFFSET
		&& reg <= WSA_MAX_OFFSET)
		return WSA_MACRO;
	if (reg >= WSA2_START_OFFSET
		&& reg <= WSA2_MAX_OFFSET)
		return WSA2_MACRO;
	if (reg >= VA_START_OFFSET &&
		reg <= VA_MAX_OFFSET)
		return VA_MACRO;

	return -EINVAL;
}

static int regmap_bus_read(void *context, const void *reg, size_t reg_size,
			   void *val, size_t val_size)
{
	struct device *dev = context;
	struct lpass_cdc_priv *priv;
	u16 first, last, offset;
	int macro_id, ret = 0;
	size_t i;

	if (!dev || !reg || !val || reg_size != REG_BYTES || !val_size)
		return -EINVAL;
	priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODEV;
	first = get_unaligned((const u16 *)reg);
	if ((first & 3) || val_size - 1 > (U16_MAX - first) / 4)
		return -EINVAL;
	last = first + (val_size - 1) * 4;
	macro_id = lpass_cdc_get_macro_id(priv->va_without_decimation, first);
	if (macro_id < 0 || macro_id !=
	    lpass_cdc_get_macro_id(priv->va_without_decimation, last))
		return -EINVAL;

	if (!lpass_cdc_op_get(priv, false))
		return -ENODEV;
	mutex_lock(&priv->io_lock);
	if (!priv->macros_supported[macro_id] || !priv->macro_params[macro_id].io_base) {
		ret = -ENODEV;
		goto unlock;
	}
	for (i = 0; i < val_size; i++) {
		offset = first + i * 4 - macro_id_base_offset[macro_id];
		ret = priv->read_dev(priv, macro_id, offset, &((u8 *)val)[i]);
		if (ret)
			break;
	}
unlock:
	mutex_unlock(&priv->io_lock);
	lpass_cdc_op_put(priv, false);
	return ret;
}

static int regmap_bus_gather_write(void *context, const void *reg, size_t reg_size,
				   const void *val, size_t val_size)
{
	struct device *dev = context;
	struct lpass_cdc_priv *priv;
	u16 first, last, offset;
	int macro_id, ret = 0;
	size_t i;

	if (!dev || !reg || !val || reg_size != REG_BYTES || !val_size)
		return -EINVAL;
	priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODEV;
	first = get_unaligned((const u16 *)reg);
	if ((first & 3) || val_size - 1 > (U16_MAX - first) / 4)
		return -EINVAL;
	last = first + (val_size - 1) * 4;
	macro_id = lpass_cdc_get_macro_id(priv->va_without_decimation, first);
	if (macro_id < 0 || macro_id !=
	    lpass_cdc_get_macro_id(priv->va_without_decimation, last))
		return -EINVAL;

	if (!lpass_cdc_op_get(priv, false))
		return -ENODEV;
	mutex_lock(&priv->io_lock);
	if (!priv->macros_supported[macro_id] || !priv->macro_params[macro_id].io_base) {
		ret = -ENODEV;
		goto unlock;
	}
	for (i = 0; i < val_size; i++) {
		offset = first + i * 4 - macro_id_base_offset[macro_id];
		ret = priv->write_dev(priv, macro_id, offset, ((const u8 *)val)[i]);
		if (ret)
			break;
	}
unlock:
	mutex_unlock(&priv->io_lock);
	lpass_cdc_op_put(priv, false);
	return ret;
}

static int regmap_bus_write(void *context, const void *data, size_t count)
{
	struct device *dev = context;
	struct lpass_cdc_priv *priv = dev_get_drvdata(dev);

	if (!priv)
		return -EINVAL;

	if (!data || count <= REG_BYTES) {
		dev_err_ratelimited(dev, "%s: count %zd bytes < %d, not supported\n",
			__func__, count, REG_BYTES);
		return -EINVAL;
	}

	return regmap_bus_gather_write(context, data, REG_BYTES,
				       data + REG_BYTES,
				       count - REG_BYTES);
}

static struct regmap_bus regmap_bus_config = {
	.write = regmap_bus_write,
	.gather_write = regmap_bus_gather_write,
	.read = regmap_bus_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

struct regmap *lpass_cdc_regmap_init(struct device *dev,
				      const struct regmap_config *config)
{
	return devm_regmap_init(dev, &regmap_bus_config, dev, config);
}
