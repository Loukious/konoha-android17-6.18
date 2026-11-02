// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <soc/qcom/pcie-pdc.h>

#define IRQ_i_CFG		0x110
#define IRQ_i_CFG_IRQ_ENABLE	BIT(3)
#define IRQ_i_CFG_TYPE_MASK	0x7
#define IRQ_i_CFG_DISTANCE	0x4

/* IRQ_i_GP_IRQ_SELECT Register */
#define IRQ_i_GP_IRQ_DISTANCE	0x14

struct irq_map {
	u32 gpio;
	u32 mux;
	u32 irq;
};

struct pdc_match_data {
	const struct irq_map *map;
	u32 size;
	u32 irq_select_offset;
};

static const struct pdc_match_data *d;
static DEFINE_RAW_SPINLOCK(pdc_lock);
static void __iomem *pcie_pdc_base;

enum pdc_irq_config_bits {
	PDC_LEVEL_LOW		= 0b000,
	PDC_EDGE_FALLING	= 0b010,
	PDC_LEVEL_HIGH		= 0b100,
	PDC_EDGE_RISING		= 0b110,
	PDC_EDGE_DUAL		= 0b111,
};

static int pdc_cfg_irq(u32 irq, u32 mux, int mux_select, unsigned int type,
		u32 irq_select_offset, bool enable)
{
	u32 value;
	enum pdc_irq_config_bits pdc_type;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		pdc_type = PDC_EDGE_RISING;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		pdc_type = PDC_EDGE_FALLING;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		pdc_type = PDC_EDGE_DUAL;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		pdc_type = PDC_LEVEL_HIGH;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		pdc_type = PDC_LEVEL_LOW;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	if (enable) {
		value = mux;
		pdc_type |= IRQ_i_CFG_IRQ_ENABLE;
	} else {
		value = 0;
		pdc_type &= ~IRQ_i_CFG_IRQ_ENABLE;
	}

	writel_relaxed(value, pcie_pdc_base + irq_select_offset +
		       mux_select * IRQ_i_GP_IRQ_DISTANCE);
	writel_relaxed(pdc_type, pcie_pdc_base + IRQ_i_CFG + irq * IRQ_i_CFG_DISTANCE);

	return 0;
}

/**
 * pcie_pdc_cfg_irq() - Configure the GPIO interrupt at PCIe PDC
 * @gpio:      The wake capable GPIO number
 * @type:      The interrupt type for GPIO
 * @enable:    Enable or disable GPIO interrupt
 *
 * Configures the GPIO interrupt at PCIe PDC.
 *
 * Return:
 * * 0			- Success
 * * -Error		- Error code
 */
int pcie_pdc_cfg_irq(u32 gpio, unsigned int type, bool enable)
{
	unsigned long flags;
	int i, ret = -EINVAL;

	raw_spin_lock_irqsave(&pdc_lock, flags);
	if (!d || !pcie_pdc_base) {
		ret = -ENODEV;
		goto out;
	}

	for (i = 0; i < d->size; i++) {
		if (gpio == d->map[i].gpio) {
			ret = pdc_cfg_irq(d->map[i].irq, d->map[i].mux, i, type,
					  d->irq_select_offset, enable);
			break;
		}
	}

out:
	raw_spin_unlock_irqrestore(&pdc_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(pcie_pdc_cfg_irq);

static int qcom_pcie_pdc_probe(struct platform_device *pdev)
{
	const struct pdc_match_data *match;
	struct resource *res;
	void __iomem *base;
	unsigned long flags;
	resource_size_t size;
	int i, ret = 0;

	match = of_device_get_match_data(&pdev->dev);
	if (!match)
		return -EINVAL;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	size = resource_size(res);
	for (i = 0; i < match->size; i++) {
		u64 cfg = IRQ_i_CFG + (u64)match->map[i].irq * IRQ_i_CFG_DISTANCE;
		u64 select = match->irq_select_offset + (u64)i * IRQ_i_GP_IRQ_DISTANCE;

		if (size < sizeof(u32) || cfg > size - sizeof(u32) ||
		    select > size - sizeof(u32))
			return -EINVAL;
	}
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	raw_spin_lock_irqsave(&pdc_lock, flags);
	if (d) {
		ret = -EBUSY;
	} else {
		pcie_pdc_base = base;
		d = match;
		platform_set_drvdata(pdev, base);
	}
	raw_spin_unlock_irqrestore(&pdc_lock, flags);
	return ret;
}

static void qcom_pcie_pdc_remove(struct platform_device *pdev)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&pdc_lock, flags);
	if (pcie_pdc_base == platform_get_drvdata(pdev)) {
		d = NULL;
		pcie_pdc_base = NULL;
	}
	raw_spin_unlock_irqrestore(&pdc_lock, flags);
}

static const struct irq_map sun_irq_map[] = {
	{ 103, 64, 10 },
};

static const struct pdc_match_data sun_pdc_match_data = {
	.map = sun_irq_map,
	.size = ARRAY_SIZE(sun_irq_map),
	.irq_select_offset = 0x4800,
};

static const struct irq_map pineapple_irq_map[] = {
	{ 95, 67, 10 },
	{ 98, 50, 11 },
};

static const struct pdc_match_data pineapple_pdc_match_data = {
	.map = pineapple_irq_map,
	.size = ARRAY_SIZE(pineapple_irq_map),
	.irq_select_offset = 0x4900,
};

static const struct irq_map cliffs_irq_map[] = {
	{ 118, 95, 10 },
};

static const struct pdc_match_data cliffs_pdc_match_data = {
	.map = cliffs_irq_map,
	.size = ARRAY_SIZE(cliffs_irq_map),
	.irq_select_offset = 0x4900,
};

static const struct irq_map tuna_irq_map[] = {
	{ 118, 69, 10 },
};

static const struct pdc_match_data tuna_pdc_match_data = {
	.map = tuna_irq_map,
	.size = ARRAY_SIZE(tuna_irq_map),
	.irq_select_offset = 0x4800,
};

static const struct irq_map kera_irq_map[] = {
	{ 52, 81, 10 },
	{ 80, 90, 11 },
};

static const struct pdc_match_data kera_pdc_match_data = {
	.map = kera_irq_map,
	.size = ARRAY_SIZE(kera_irq_map),
	.irq_select_offset = 0x4800,
};

static const struct irq_map sdxkova_irq_map[] = {
	{ 43, 9, 10 },
	{ 124, 33, 11 },
	{ 121, 31, 12 },
};

static const struct pdc_match_data sdxkova_pdc_match_data = {
	.map = sdxkova_irq_map,
	.size = ARRAY_SIZE(sdxkova_irq_map),
	.irq_select_offset = 0x4900,
};

static const struct of_device_id qcom_pcie_pdc_match_table[] = {
	{ .compatible = "qcom,sun-pcie-pdc", .data = &sun_pdc_match_data },
	{ .compatible = "qcom,pineapple-pcie-pdc", .data = &pineapple_pdc_match_data },
	{ .compatible = "qcom,cliffs-pcie-pdc", .data = &cliffs_pdc_match_data },
	{ .compatible = "qcom,tuna-pcie-pdc", .data = &tuna_pdc_match_data },
	{ .compatible = "qcom,kera-pcie-pdc", .data = &kera_pdc_match_data },
	{ .compatible = "qcom,sdxkova-pcie-pdc", .data = &sdxkova_pdc_match_data },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_pcie_pdc_match_table);

static struct platform_driver qcom_pcie_pdc_driver = {
	.probe = qcom_pcie_pdc_probe,
	.remove = qcom_pcie_pdc_remove,
	.driver = {
		.name = "qcom-pcie-pdc",
		.of_match_table = qcom_pcie_pdc_match_table,
	},
};

module_platform_driver(qcom_pcie_pdc_driver);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Power Domain Controller for PCIe");
MODULE_LICENSE("GPL");
