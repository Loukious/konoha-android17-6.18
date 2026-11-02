// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/init.h>
#include <soc/soundwire.h>

#define ADDR_BYTES                (2)
#define ADDR_BYTES_4              (4)
#define VAL_BYTES                 (1)
#define PAD_BYTES                 (0)
#define SCP1_ADDRESS_VAL_MASK     (0x7f800000)
#define SCP2_ADDRESS_VAL_MASK     (0x007f8000)
#define BIT_WIDTH_CHECK_MASK      (0xffff0000)
#define SCP1_ADDRESS_VAL_SHIFT    (23)
#define SCP2_ADDRESS_VAL_SHIFT    (15)
#define SCP1_ADDRESS              (0X48)
#define SCP2_ADDRESS              (0X49)
#define SDCA_READ_WRITE_BIT       (0x8000)
static DEFINE_MUTEX(swr_rw_lock);

/* swr_rw_lock serializes page selection and the associated data transfer. */
static int regmap_swr_reg_address_get(struct swr_device *swr,
				     u16 *reg_addr, const void *reg,
				     size_t reg_size)
{
	u32 address = 0;
	u8 scp1, scp2;
	int generation, ret;

	if (!swr || !reg || !reg_addr ||
	    (reg_size != ADDR_BYTES && reg_size != ADDR_BYTES_4))
		return -EINVAL;
	if (reg_size == ADDR_BYTES) {
		memcpy(reg_addr, reg, sizeof(*reg_addr));
		return 0;
	}
	memcpy(&address, reg, sizeof(address));
	scp1 = (address & SCP1_ADDRESS_VAL_MASK) >> SCP1_ADDRESS_VAL_SHIFT;
	scp2 = (address & SCP2_ADDRESS_VAL_MASK) >> SCP2_ADDRESS_VAL_SHIFT;
	*reg_addr = address;
	if (!scp1 && !scp2)
		return 0;
	generation = atomic_read(&swr->scp_generation);
	if (generation != swr->scp_cached_generation) {
		swr->scp_valid = 0;
		swr->scp_cached_generation = generation;
	}
	if (!(swr->scp_valid & 1) || scp1 != swr->g_scp1_val) {
		ret = swr_write(swr, swr->dev_num, SCP1_ADDRESS, &scp1);
		if (ret)
			return ret;
		swr->g_scp1_val = scp1;
		swr->scp_valid |= 1;
	}
	if (!(swr->scp_valid & 2) || scp2 != swr->g_scp2_val) {
		ret = swr_write(swr, swr->dev_num, SCP2_ADDRESS, &scp2);
		if (ret)
			return ret;
		swr->g_scp2_val = scp2;
		swr->scp_valid |= 2;
	}
	if (generation != atomic_read(&swr->scp_generation)) {
		swr->scp_valid = 0;
		return -EAGAIN;
	}
	*reg_addr |= SDCA_READ_WRITE_BIT;
	return 0;
}

static int regmap_swr_span(const void *reg, size_t reg_size,
			   size_t count, u32 *address)
{
	u16 short_address;

	if (!reg || !count ||
	    (reg_size != ADDR_BYTES && reg_size != ADDR_BYTES_4))
		return -EINVAL;
	if (reg_size == ADDR_BYTES) {
		memcpy(&short_address, reg, sizeof(short_address));
		*address = short_address;
		if (count > 0x10000U - *address)
			return -EINVAL;
	} else {
		memcpy(address, reg, sizeof(*address));
		if (count - 1 > U32_MAX - *address)
			return -EINVAL;
	}
	return 0;
}

static int regmap_swr_gather_write(void *context,
				  const void *reg, size_t reg_size,
				  const void *val, size_t val_len)
{
	struct device *dev = context;
	struct swr_device *swr = to_swr_device(dev);
	const u8 *value = val;
	u32 address;
	u16 reg_addr, short_address;
	size_t i;
	int ret;

	if (!swr || !val || !dev_get_regmap(dev, NULL))
		return -EINVAL;
	ret = regmap_swr_span(reg, reg_size, val_len, &address);
	if (ret)
		return ret;
	mutex_lock(&swr_rw_lock);
	for (i = 0; i < val_len; i++, address++) {
		short_address = address;
		ret = regmap_swr_reg_address_get(swr, &reg_addr,
			reg_size == ADDR_BYTES ? (void *)&short_address : &address,
			reg_size);
		if (ret)
			break;
		ret = swr_write(swr, swr->dev_num, reg_addr, value + i);
		if (ret)
			break;
	}
	mutex_unlock(&swr_rw_lock);
	return ret;
}

static int regmap_swr_raw_multi_reg_write(void *context,
					 const void *data, size_t count)
{
	struct device *dev = context;
	struct swr_device *swr = to_swr_device(dev);
	const u8 *buf = data;
	u16 *reg, reg_addr;
	u8 *val;
	size_t addr_bytes, stride, num_regs, i;
	int ret = 0;

	if (!swr || !data || !dev_get_regmap(dev, NULL))
		return -EINVAL;
	addr_bytes = swr->paging_support ? ADDR_BYTES_4 : ADDR_BYTES;
	stride = addr_bytes + VAL_BYTES;
	if (!count || count % stride)
		return -EINVAL;
	num_regs = count / stride;
	if (swr->paging_support) {
		mutex_lock(&swr_rw_lock);
		for (i = 0; i < num_regs; i++, buf += stride) {
			ret = regmap_swr_reg_address_get(swr, &reg_addr,
							buf, addr_bytes);
			if (ret)
				break;
			ret = swr_write(swr, swr->dev_num, reg_addr, buf + addr_bytes);
			if (ret)
				break;
		}
		mutex_unlock(&swr_rw_lock);
		return ret;
	}
	reg = kcalloc(num_regs, sizeof(*reg), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;
	val = kcalloc(num_regs, sizeof(*val), GFP_KERNEL);
	if (!val) {
		kfree(reg);
		return -ENOMEM;
	}
	for (i = 0; i < num_regs; i++, buf += stride) {
		memcpy(&reg[i], buf, sizeof(*reg));
		val[i] = buf[addr_bytes];
	}
	mutex_lock(&swr_rw_lock);
	ret = swr_bulk_write(swr, swr->dev_num, reg, val, num_regs);
	mutex_unlock(&swr_rw_lock);
	kfree(val);
	kfree(reg);
	return ret;
}

static int regmap_swr_write(void *context, const void *data, size_t count)
{
	struct swr_device *swr = to_swr_device(context);
	size_t addr_bytes;

	if (!swr || !data)
		return -EINVAL;
	addr_bytes = swr->paging_support ? ADDR_BYTES_4 : ADDR_BYTES;
	if (count < addr_bytes + VAL_BYTES)
		return -EINVAL;
	if (count > addr_bytes + VAL_BYTES)
		return regmap_swr_raw_multi_reg_write(context, data, count);
	return regmap_swr_gather_write(context, data, addr_bytes,
				       (const u8 *)data + addr_bytes,
				       count - addr_bytes);
}

static int regmap_swr_read(void *context, const void *reg, size_t reg_size,
			   void *val, size_t val_size)
{
	struct device *dev = context;
	struct swr_device *swr = to_swr_device(dev);
	u8 *value = val;
	u32 address;
	u16 reg_addr, short_address;
	size_t chunk;
	int ret;

	if (!swr || !val || !dev_get_regmap(dev, NULL))
		return -EINVAL;
	ret = regmap_swr_span(reg, reg_size, val_size, &address);
	if (ret)
		return ret;
	mutex_lock(&swr_rw_lock);
	while (val_size) {
		short_address = address;
		ret = regmap_swr_reg_address_get(swr, &reg_addr,
			reg_size == ADDR_BYTES ? (void *)&short_address : &address,
			reg_size);
		if (ret)
			break;
		/* A paged transfer must not cross the next SCP2 page. */
		chunk = min_t(size_t, val_size,
			      reg_size == ADDR_BYTES ? 0x10000U - reg_addr :
			      0x8000U - (address & 0x7fff));
		ret = swr_read(swr, swr->dev_num, reg_addr, value, chunk);
		if (ret)
			break;
		address += chunk;
		value += chunk;
		val_size -= chunk;
	}
	mutex_unlock(&swr_rw_lock);
	return ret;
}

static struct regmap_bus regmap_swr = {
	.write = regmap_swr_write,
	.gather_write = regmap_swr_gather_write,
	.read = regmap_swr_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

struct regmap *__regmap_init_swr(struct swr_device *swr,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name)
{
	if (!swr || !config || config->val_bits != 8 || config->pad_bits ||
	    config->reg_bits != (swr->paging_support ? 32 : 16))
		return ERR_PTR(-EINVAL);
	return __regmap_init(&swr->dev, &regmap_swr, &swr->dev, config,
			   lock_key, lock_name);
}
EXPORT_SYMBOL(__regmap_init_swr);

struct regmap *__devm_regmap_init_swr(struct swr_device *swr,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name)
{
	if (!swr || !config || config->val_bits != 8 || config->pad_bits ||
	    config->reg_bits != (swr->paging_support ? 32 : 16))
		return ERR_PTR(-EINVAL);
	return __devm_regmap_init(&swr->dev, &regmap_swr, &swr->dev, config,
				lock_key, lock_name);
}
EXPORT_SYMBOL(__devm_regmap_init_swr);

MODULE_LICENSE("GPL v2");
