// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/irqdomain.h>
#include <linux/regmap.h>
#include <asoc/wcd-irq.h>

static int wcd_map_irq(struct wcd_irq_info *irq_info, int irq)
{
	if (!irq_info || !irq_info->irq_chip) {
		pr_err("%s: Null IRQ handle\n", __func__);
		return -EINVAL;
	}
	return regmap_irq_get_virq(irq_info->irq_chip, irq);
}

/**
 * wcd_request_irq: Request a thread handler for the given IRQ
 * @irq_info: pointer to IRQ info structure
 * @irq: irq number
 * @name: name for the IRQ thread
 * @handler: irq handler
 * @data: data pointer
 *
 * Returns 0 on success or error on failure
 */
int wcd_request_irq(struct wcd_irq_info *irq_info, int irq, const char *name,
			irq_handler_t handler, void *data)
{
	if (!irq_info) {
		pr_err("%s: Null IRQ handle\n", __func__);
		return -EINVAL;
	}
	irq = wcd_map_irq(irq_info, irq);
	if (irq <= 0)
		return irq ? irq : -EINVAL;

	return devm_request_threaded_irq(irq_info->dev, irq, NULL, handler,
				    IRQF_ONESHOT | IRQF_TRIGGER_RISING,
				    name, data);
}
EXPORT_SYMBOL(wcd_request_irq);

/**
 * wcd_free_irq: Free the IRQ resources allocated during request_irq
 * @irq_info: pointer to IRQ info structure
 * @irq: irq number
 * @data: data pointer
 */
void wcd_free_irq(struct wcd_irq_info *irq_info, int irq, void *data)
{
	if (!irq_info) {
		pr_err("%s: Null IRQ handle\n", __func__);
		return;
	}

	irq = wcd_map_irq(irq_info, irq);
	if (irq <= 0)
		return;

	devm_free_irq(irq_info->dev, irq, data);
}
EXPORT_SYMBOL(wcd_free_irq);

/**
 * wcd_enable_irq: Enable the given IRQ
 * @irq_info: pointer to IRQ info structure
 * @irq: irq number
 */
void wcd_enable_irq(struct wcd_irq_info *irq_info, int irq)
{
	irq = wcd_map_irq(irq_info, irq);
	if (irq > 0)
		enable_irq(irq);
}
EXPORT_SYMBOL(wcd_enable_irq);

/**
 * wcd_disable_irq: Disable the given IRQ
 * @irq_info: pointer to IRQ info structure
 * @irq: irq number
 */
void wcd_disable_irq(struct wcd_irq_info *irq_info, int irq)
{
	irq = wcd_map_irq(irq_info, irq);
	if (irq > 0)
		disable_irq_nosync(irq);
}
EXPORT_SYMBOL(wcd_disable_irq);

static void wcd_irq_chip_disable(struct irq_data *data)
{
}

static void wcd_irq_chip_enable(struct irq_data *data)
{
}

static const struct irq_chip wcd_irq_chip = {
	.name = NULL,
	.irq_disable = wcd_irq_chip_disable,
	.irq_enable = wcd_irq_chip_enable,
};

struct wcd_irq_domain {
	struct irq_chip chip;
	struct irq_domain *domain;
	unsigned int parent_irq;
};

static struct lock_class_key wcd_irq_lock_class;
static struct lock_class_key wcd_irq_lock_requested_class;

static int wcd_irq_chip_map(struct irq_domain *irqd, unsigned int virq,
			irq_hw_number_t hw)
{
	struct wcd_irq_domain *ctx = irqd->host_data;

	irq_set_chip_and_handler(virq, &ctx->chip, handle_simple_irq);
	irq_set_lockdep_class(virq, &wcd_irq_lock_class,
			&wcd_irq_lock_requested_class);
	irq_set_nested_thread(virq, 1);
	irq_set_noprobe(virq);

	return 0;
}

static const struct irq_domain_ops wcd_domain_ops = {
	.map = wcd_irq_chip_map,
};

static void wcd_irq_domain_release(void *data)
{
	struct wcd_irq_domain *ctx = data;

	irq_dispose_mapping(ctx->parent_irq);
	irq_domain_remove(ctx->domain);
	kfree(ctx);
}

/**
 * wcd_irq_init: Initializes IRQ module
 * @irq_info: pointer to IRQ info structure
 * @virq: receives the parent IRQ domain
 *
 * Returns 0 on success or error on failure
 */
int wcd_irq_init(struct wcd_irq_info *irq_info, struct irq_domain **virq)
{
	struct wcd_irq_domain *ctx;
	int ret;

	if (!irq_info || !virq || !irq_info->dev || !irq_info->regmap ||
	    !irq_info->wcd_regmap_irq_chip)
		return -EINVAL;
	if (irq_info->irq_chip)
		return -EBUSY;
	*virq = NULL;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->chip = wcd_irq_chip;
	ctx->chip.name = irq_info->codec_name;
	ctx->domain = irq_domain_add_linear(NULL, 1, &wcd_domain_ops, ctx);
	if (!ctx->domain) {
		kfree(ctx);
		return -ENOMEM;
	}
	ctx->parent_irq = irq_create_mapping(ctx->domain, 0);
	if (!ctx->parent_irq) {
		irq_domain_remove(ctx->domain);
		kfree(ctx);
		return -ENOMEM;
	}
	/* Devres unwinds the regmap chip before its parent IRQ/domain. */
	ret = devm_add_action_or_reset(irq_info->dev, wcd_irq_domain_release, ctx);
	if (ret)
		return ret;
	ret = devm_regmap_add_irq_chip(irq_info->dev, irq_info->regmap,
				       ctx->parent_irq, IRQF_ONESHOT, 0,
				       irq_info->wcd_regmap_irq_chip,
				       &irq_info->irq_chip);
	if (ret) {
		irq_info->irq_chip = NULL;
		devm_remove_action(irq_info->dev, wcd_irq_domain_release, ctx);
		wcd_irq_domain_release(ctx);
		return ret;
	}
	*virq = ctx->domain;
	return 0;
}
EXPORT_SYMBOL(wcd_irq_init);

/**
 * wcd_irq_exit: Uninitialize regmap IRQ and free IRQ resources
 * @irq_info: pointer to IRQ info structure
 * @virq: parent IRQ domain returned by wcd_irq_init
 *
 * Returns 0 on success or error on failure
 */
int wcd_irq_exit(struct wcd_irq_info *irq_info, struct irq_domain *virq)
{
	struct wcd_irq_domain *ctx;

	if (!irq_info || !irq_info->dev || !irq_info->irq_chip || !virq)
		return -EINVAL;
	ctx = virq->host_data;
	devm_regmap_del_irq_chip(irq_info->dev, ctx->parent_irq,
				 irq_info->irq_chip);
	irq_info->irq_chip = NULL;
	devm_remove_action(irq_info->dev, wcd_irq_domain_release, ctx);
	wcd_irq_domain_release(ctx);
	return 0;
}
EXPORT_SYMBOL(wcd_irq_exit);
