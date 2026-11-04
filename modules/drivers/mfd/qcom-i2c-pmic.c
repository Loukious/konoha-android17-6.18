// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2018, 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/qti-regmap-debugfs.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define I2C_INTR_STATUS_BASE	0x0550
#define INT_RT_STS_OFFSET	0x10
#define INT_SET_TYPE_OFFSET	0x11
#define INT_POL_HIGH_OFFSET	0x12
#define INT_POL_LOW_OFFSET	0x13
#define INT_LATCHED_CLR_OFFSET	0x14
#define INT_EN_SET_OFFSET	0x15
#define INT_EN_CLR_OFFSET	0x16
#define INT_LATCHED_STS_OFFSET	0x18
#define INT_PENDING_STS_OFFSET	0x19
#define INT_MID_SEL_OFFSET	0x1A
#define INT_MID_SEL_MASK	GENMASK(1, 0)
#define INT_PRIORITY_OFFSET	0x1B
#define INT_PRIORITY_BIT	BIT(0)

enum {
	IRQ_SET_TYPE = 0,
	IRQ_POL_HIGH,
	IRQ_POL_LOW,
	IRQ_LATCHED_CLR, /* not needed but makes life easy */
	IRQ_EN_SET,
	IRQ_MAX_REGS,
};

struct i2c_pmic_periph {
	void		*data;
	u16		addr;
	u8		cached[IRQ_MAX_REGS];
	u8		synced[IRQ_MAX_REGS];
	u8		wake;
	struct mutex	lock;
};

struct i2c_pmic {
	struct device		*dev;
	struct regmap		*regmap;
	struct irq_domain	*domain;
	struct i2c_pmic_periph	*periph;
	struct pinctrl		*pinctrl;
	struct mutex		irq_complete;
	const char		*pinctrl_name;
	int			num_periphs;
	int			summary_irq;
	bool			resume_completed;
	bool			irq_waiting;
	bool			toggle_stat;
	bool			wake_enabled;
	bool			irq_disabled;
	bool			stopping;
};

static void i2c_pmic_irq_bus_lock(struct irq_data *d)
{
	struct i2c_pmic_periph *periph = irq_data_get_irq_chip_data(d);

	mutex_lock(&periph->lock);
}

static int i2c_pmic_sync_type_polarity(struct i2c_pmic *chip,
			       struct i2c_pmic_periph *periph)
{
	u8 changed = 0;
	int i, rc;

	for (i = IRQ_SET_TYPE; i <= IRQ_POL_LOW; i++)
		changed |= periph->cached[i] ^ periph->synced[i];
	changed &= periph->synced[IRQ_EN_SET];
	/* Do not enable a partially configured interrupt after an I2C error. */
	if (changed) {
		rc = regmap_write(chip->regmap, periph->addr | INT_EN_CLR_OFFSET, changed);
		if (rc)
			return rc;
		periph->synced[IRQ_EN_SET] &= ~changed;
	}
	for (i = IRQ_SET_TYPE; i <= IRQ_POL_LOW; i++) {
		if (periph->cached[i] == periph->synced[i])
			continue;
		rc = regmap_write(chip->regmap, periph->addr | (INT_SET_TYPE_OFFSET + i), periph->cached[i]);
		if (rc)
			return rc;
		periph->synced[i] = periph->cached[i];
	}
	return 0;
}

static void i2c_pmic_sync_enable(struct i2c_pmic *chip,
				 struct i2c_pmic_periph *periph)
{
	u8 en_set, en_clr;
	int rc;

	/* determine which irqs were enabled and which were disabled */
	en_clr = periph->synced[IRQ_EN_SET] & ~periph->cached[IRQ_EN_SET];
	en_set = ~periph->synced[IRQ_EN_SET] & periph->cached[IRQ_EN_SET];

	/* were any irqs disabled? */
	if (en_clr) {
		rc = regmap_write(chip->regmap,
				  periph->addr | INT_EN_CLR_OFFSET, en_clr);
		if (rc < 0) {
			dev_err(chip->dev, "Couldn't disable periph 0x%04x irqs 0x%02x rc=%d\n",
				periph->addr, en_clr, rc);
			return;
		}
	}

	periph->synced[IRQ_EN_SET] &= ~en_clr;

	/* were any irqs enabled? */
	if (en_set) {
		rc = regmap_write(chip->regmap,
				  periph->addr | INT_EN_SET_OFFSET, en_set);
		if (rc < 0) {
			dev_err(chip->dev, "Couldn't enable periph 0x%04x irqs 0x%02x rc=%d\n",
				periph->addr, en_set, rc);
			return;
		}
	}

	/* irq enabled status was written to hardware */
	periph->synced[IRQ_EN_SET] = periph->cached[IRQ_EN_SET];
}

static void i2c_pmic_irq_bus_sync_unlock(struct irq_data *d)
{
	struct i2c_pmic_periph *periph = irq_data_get_irq_chip_data(d);
	struct i2c_pmic *chip = periph->data;

	int rc;

	rc = i2c_pmic_sync_type_polarity(chip, periph);
	if (rc)
		dev_err(chip->dev, "IRQ type/polarity sync failed: %d\n", rc);
	else
		i2c_pmic_sync_enable(chip, periph);
	mutex_unlock(&periph->lock);
}

static void i2c_pmic_irq_disable(struct irq_data *d)
{
	struct i2c_pmic_periph *periph = irq_data_get_irq_chip_data(d);

	periph->cached[IRQ_EN_SET] &= ~d->hwirq & 0xFF;
}

static void i2c_pmic_irq_enable(struct irq_data *d)
{
	struct i2c_pmic_periph *periph = irq_data_get_irq_chip_data(d);

	periph->cached[IRQ_EN_SET] |= d->hwirq & 0xFF;
}

static int i2c_pmic_irq_set_type(struct irq_data *d, unsigned int irq_type)
{
	struct i2c_pmic_periph *periph = irq_data_get_irq_chip_data(d);

	switch (irq_type) {
	case IRQ_TYPE_EDGE_RISING:
		periph->cached[IRQ_SET_TYPE] |= d->hwirq & 0xFF;
		periph->cached[IRQ_POL_HIGH] |= d->hwirq & 0xFF;
		periph->cached[IRQ_POL_LOW] &= ~d->hwirq & 0xFF;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		periph->cached[IRQ_SET_TYPE] |= d->hwirq & 0xFF;
		periph->cached[IRQ_POL_HIGH] &= ~d->hwirq & 0xFF;
		periph->cached[IRQ_POL_LOW] |= d->hwirq & 0xFF;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		periph->cached[IRQ_SET_TYPE] |= d->hwirq & 0xFF;
		periph->cached[IRQ_POL_HIGH] |= d->hwirq & 0xFF;
		periph->cached[IRQ_POL_LOW] |= d->hwirq & 0xFF;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		periph->cached[IRQ_SET_TYPE] &= ~d->hwirq & 0xFF;
		periph->cached[IRQ_POL_HIGH] |= d->hwirq & 0xFF;
		periph->cached[IRQ_POL_LOW] &= ~d->hwirq & 0xFF;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		periph->cached[IRQ_SET_TYPE] &= ~d->hwirq & 0xFF;
		periph->cached[IRQ_POL_HIGH] &= ~d->hwirq & 0xFF;
		periph->cached[IRQ_POL_LOW] |= d->hwirq & 0xFF;
		break;
	default:
		pr_err("irq type 0x%04x is not supported\n", irq_type);
		return -EINVAL;
	}

	return 0;
}

static int __maybe_unused i2c_pmic_irq_set_wake(struct irq_data *d,
						unsigned int on)
{
	struct i2c_pmic_periph *periph = irq_data_get_irq_chip_data(d);

	/* IRQ core already holds irq_bus_lock for irq_set_wake. */
	if (on)
		periph->wake |= d->hwirq & 0xFF;
	else
		periph->wake &= ~d->hwirq & 0xFF;

	return 0;
}

static struct irq_chip i2c_pmic_irq_chip = {
	.name			= "i2c_pmic_irq_chip",
	.irq_bus_lock		= i2c_pmic_irq_bus_lock,
	.irq_bus_sync_unlock	= i2c_pmic_irq_bus_sync_unlock,
	.irq_disable		= i2c_pmic_irq_disable,
	.irq_enable		= i2c_pmic_irq_enable,
	.irq_set_type		= i2c_pmic_irq_set_type,
	.irq_set_wake		= pm_sleep_ptr(i2c_pmic_irq_set_wake),
};

static struct i2c_pmic_periph *i2c_pmic_find_periph(struct i2c_pmic *chip,
						    irq_hw_number_t hwirq)
{
	int i;

	for (i = 0; i < chip->num_periphs; i++)
		if (chip->periph[i].addr == (hwirq & 0xFF00))
			return &chip->periph[i];

	dev_err_ratelimited(chip->dev, "Couldn't find periph struct for hwirq 0x%04lx\n",
			   hwirq);
	return NULL;
}

static int i2c_pmic_domain_map(struct irq_domain *d, unsigned int virq,
			irq_hw_number_t hwirq)
{
	struct i2c_pmic *chip = d->host_data;
	struct i2c_pmic_periph *periph = i2c_pmic_find_periph(chip, hwirq);

	if (!periph || hwirq > 0xffff || !is_power_of_2(hwirq & 0xff))
		return -ENODEV;

	irq_set_chip_data(virq, periph);
	irq_set_chip_and_handler(virq, &i2c_pmic_irq_chip, handle_level_irq);
	irq_set_nested_thread(virq, 1);
	irq_set_noprobe(virq);
	return 0;
}

static int i2c_pmic_domain_xlate(struct irq_domain *d,
				 struct device_node *ctrlr, const u32 *intspec,
				 unsigned int intsize, unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (intsize != 3)
		return -EINVAL;

	if (intspec[0] > 0xFF || intspec[1] > 0x7 ||
					intspec[2] > IRQ_TYPE_SENSE_MASK)
		return -EINVAL;

	/*
	 * Interrupt specifiers are triplets
	 * <peripheral-address, irq-number, IRQ_TYPE_*>
	 *
	 * peripheral-address - The base address of the peripheral
	 * irq-number	      - The zero based bit position of the peripheral's
	 *			interrupt registers corresponding to the irq
	 *			where the LSB is 0 and the MSB is 7
	 * IRQ_TYPE_*	      - Please refer to linux/irq.h
	 */
	*out_hwirq = intspec[0] << 8 | BIT(intspec[1]);
	*out_type = intspec[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static const struct irq_domain_ops i2c_pmic_domain_ops = {
	.map	= i2c_pmic_domain_map,
	.xlate	= i2c_pmic_domain_xlate,
};

static void i2c_pmic_irq_ack_now(struct i2c_pmic *chip, u16 hwirq)
{
	int rc;

	rc = regmap_write(chip->regmap,
			  (hwirq & 0xFF00) | INT_LATCHED_CLR_OFFSET,
			  hwirq & 0xFF);
	if (rc < 0)
		dev_err_ratelimited(chip->dev, "Couldn't ack 0x%04x rc=%d\n",
				    hwirq, rc);
}

static void i2c_pmic_irq_disable_now(struct i2c_pmic *chip, u16 hwirq)
{
	struct i2c_pmic_periph *periph = i2c_pmic_find_periph(chip, hwirq);
	int rc;

	if (!periph)
		return;

	mutex_lock(&periph->lock);
	periph->cached[IRQ_EN_SET] &= ~hwirq & 0xFF;

	rc = regmap_write(chip->regmap,
			  (hwirq & 0xFF00) | INT_EN_CLR_OFFSET,
			  hwirq & 0xFF);
	if (rc < 0) {
		dev_err_ratelimited(chip->dev, "Couldn't disable irq 0x%04x rc=%d\n",
				   hwirq, rc);
		goto unlock;
	}

	periph->synced[IRQ_EN_SET] &= ~(hwirq & 0xff);

unlock:
	mutex_unlock(&periph->lock);
}

static void i2c_pmic_periph_status_handler(struct i2c_pmic *chip,
					   u16 periph_address, u8 periph_status)
{
	unsigned int hwirq, virq;
	int i;

	while (periph_status) {
		i = ffs(periph_status) - 1;
		periph_status &= ~BIT(i);
		hwirq = periph_address | BIT(i);
		virq = irq_find_mapping(chip->domain, hwirq);
		if (virq == 0) {
			dev_err_ratelimited(chip->dev, "Couldn't find mapping; disabling 0x%04x\n",
					   hwirq);
			i2c_pmic_irq_disable_now(chip, hwirq);
			continue;
		}

		handle_nested_irq(virq);
		i2c_pmic_irq_ack_now(chip, hwirq);
	}
}

static void i2c_pmic_summary_status_handler(struct i2c_pmic *chip,
					    unsigned int first_periph,
					    u8 summary_status)
{
	unsigned int periph_status;
	int rc, i;

	while (summary_status) {
		i = ffs(summary_status) - 1;
		summary_status &= ~BIT(i);
		if (first_periph + i >= chip->num_periphs)
			continue;

		rc = regmap_read(chip->regmap,
				 chip->periph[first_periph + i].addr | INT_LATCHED_STS_OFFSET,
				 &periph_status);
		if (rc < 0) {
			dev_err_ratelimited(chip->dev, "Couldn't read 0x%04x | INT_LATCHED_STS rc=%d\n",
					   chip->periph[first_periph + i].addr, rc);
			continue;
		}

		i2c_pmic_periph_status_handler(chip, chip->periph[first_periph + i].addr,
					       periph_status);
	}
}

static irqreturn_t i2c_pmic_irq_handler(int irq, void *dev_id)
{
	struct i2c_pmic *chip = dev_id;
	unsigned int summary_status;
	int rc, i;

	mutex_lock(&chip->irq_complete);
	if (chip->stopping) {
		mutex_unlock(&chip->irq_complete);
		return IRQ_HANDLED;
	}
	chip->irq_waiting = true;
	if (!chip->resume_completed) {
		dev_dbg(chip->dev, "IRQ triggered before device-resume\n");
		if (!chip->irq_disabled) {
			disable_irq_nosync(irq);
			chip->irq_disabled = true;
		}
		mutex_unlock(&chip->irq_complete);
		return IRQ_HANDLED;
	}
	chip->irq_waiting = false;

	for (i = 0; i < DIV_ROUND_UP(chip->num_periphs, BITS_PER_BYTE); i++) {
		rc = regmap_read(chip->regmap, I2C_INTR_STATUS_BASE + i,
				&summary_status);
		if (rc < 0) {
			dev_err_ratelimited(chip->dev, "Couldn't read I2C_INTR_STATUS%d rc=%d\n",
					   i, rc);
			continue;
		}

		if (summary_status == 0)
			continue;

		i2c_pmic_summary_status_handler(chip, i * 8, summary_status);
	}

	mutex_unlock(&chip->irq_complete);

	return IRQ_HANDLED;
}

static int i2c_pmic_parse_dt(struct i2c_pmic *chip)
{
	struct device_node *node = chip->dev->of_node;
	int rc, i, j;
	u32 temp;

	if (!node) {
		dev_err(chip->dev, "missing device tree\n");
		return -EINVAL;
	}

	chip->num_periphs = of_property_count_u32_elems(node,
							"qcom,periph-map");
	if (chip->num_periphs < 0) {
		dev_err(chip->dev, "missing qcom,periph-map property rc=%d\n",
			chip->num_periphs);
		return chip->num_periphs;
	}

	if (chip->num_periphs == 0 || chip->num_periphs > 256) {
		dev_err(chip->dev, "qcom,periph-map must contain at least one address\n");
		return -EINVAL;
	}

	chip->periph = devm_kcalloc(chip->dev, chip->num_periphs,
				     sizeof(*chip->periph), GFP_KERNEL);
	if (!chip->periph)
		return -ENOMEM;

	for (i = 0; i < chip->num_periphs; i++) {
		rc = of_property_read_u32_index(node, "qcom,periph-map",
						i, &temp);
		if (rc < 0) {
			dev_err(chip->dev, "Couldn't read qcom,periph-map[%d] rc=%d\n",
			       i, rc);
			return rc;
		}

		if (temp > 0xff)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (chip->periph[j].addr == (temp << 8))
				return -EINVAL;
		chip->periph[i].addr = temp << 8;
		chip->periph[i].data = chip;
		mutex_init(&chip->periph[i].lock);
	}

	of_property_read_string(node, "pinctrl-names", &chip->pinctrl_name);

	chip->toggle_stat = of_property_read_bool(node,
				"qcom,enable-toggle-stat");

	return rc;
}

#define MAX_I2C_RETRIES	3
static int i2c_pmic_read(struct regmap *map, unsigned int reg, void *val,
			size_t val_count)
{
	int rc, retries = 0;

	do {
		rc = regmap_bulk_read(map, reg, val, val_count);
	} while (rc == -ENOTCONN && retries++ < MAX_I2C_RETRIES);

	if (retries > 1)
		pr_err("i2c_pmic_read failed for %d retries, rc = %d\n",
			retries - 1, rc);

	return rc;
}

static int i2c_pmic_determine_initial_status(struct i2c_pmic *chip)
{
	int rc, i;

	for (i = 0; i < chip->num_periphs; i++) {
		rc = i2c_pmic_read(chip->regmap,
				chip->periph[i].addr | INT_SET_TYPE_OFFSET,
				chip->periph[i].cached, IRQ_MAX_REGS);
		if (rc < 0) {
			dev_err(chip->dev, "Couldn't read irq data rc=%d\n",
				rc);
			return rc;
		}

		memcpy(chip->periph[i].synced, chip->periph[i].cached,
		       IRQ_MAX_REGS * sizeof(*chip->periph[i].synced));
	}

	return 0;
}

#define INT_TEST_OFFSET			0xE0
#define INT_TEST_MODE_EN_BIT		BIT(7)
#define INT_TEST_VAL_OFFSET		0xE1
#define INT_0_BIT			BIT(0)
static int i2c_pmic_toggle_stat(struct i2c_pmic *chip)
{
	u16 addr;
	unsigned int saved_mode, saved_value;
	u8 saved_enable;
	int rc, restore, i;

	if (!chip->toggle_stat || !chip->num_periphs)
		return 0;
	addr = chip->periph[0].addr;
	saved_enable = chip->periph[0].synced[IRQ_EN_SET];
	rc = regmap_read(chip->regmap, addr | INT_TEST_OFFSET, &saved_mode);
	if (rc)
		return rc;
	rc = regmap_read(chip->regmap, addr | INT_TEST_VAL_OFFSET, &saved_value);
	if (rc)
		return rc;
	rc = regmap_write(chip->regmap, addr | INT_EN_SET_OFFSET, INT_0_BIT);
	if (rc)
		goto restore;
	rc = regmap_write(chip->regmap, addr | INT_TEST_OFFSET, INT_TEST_MODE_EN_BIT);
	if (rc)
		goto restore;
	for (i = 0; i < 5; i++) {
		rc = regmap_write(chip->regmap, addr | INT_TEST_VAL_OFFSET, INT_0_BIT);
		if (rc)
			break;
		usleep_range(5000, 5500);
		rc = regmap_write(chip->regmap, addr | INT_TEST_VAL_OFFSET, 0);
		if (rc)
			break;
		rc = regmap_write(chip->regmap, addr | INT_LATCHED_CLR_OFFSET, INT_0_BIT);
		if (rc)
			break;
		usleep_range(5000, 5500);
	}
restore:
	restore = regmap_write(chip->regmap, addr | INT_TEST_VAL_OFFSET, saved_value);
	if (!rc && restore)
		rc = restore;
	restore = regmap_write(chip->regmap, addr | INT_TEST_OFFSET, saved_mode);
	if (!rc && restore)
		rc = restore;
	if (!(saved_enable & INT_0_BIT)) {
		restore = regmap_write(chip->regmap, addr | INT_EN_CLR_OFFSET, INT_0_BIT);
		if (!rc && restore)
			rc = restore;
	}
	return rc;
}

static struct regmap_config i2c_pmic_regmap_config = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= 0xFFFF,
};

static void i2c_pmic_cleanup(struct i2c_client *client)
{
	struct i2c_pmic *chip = i2c_get_clientdata(client);
	bool reenable;

	mutex_lock(&chip->irq_complete);
	chip->stopping = true;
	reenable = chip->irq_disabled;
	chip->irq_disabled = false;
	mutex_unlock(&chip->irq_complete);
	if (chip->summary_irq >= 0) {
		if (chip->wake_enabled)
			disable_irq_wake(chip->summary_irq);
		if (reenable)
			enable_irq(chip->summary_irq);
		devm_free_irq(chip->dev, chip->summary_irq, chip);
		chip->summary_irq = -EINVAL;
	}
	/* Parent IRQ is drained before child mappings and their domain vanish. */
	of_platform_depopulate(chip->dev);
	if (chip->domain)
		irq_domain_remove(chip->domain);
	i2c_set_clientdata(client, NULL);
}

static int i2c_pmic_probe(struct i2c_client *client)
{
	struct i2c_pmic *chip;
	int rc;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &client->dev;
	chip->summary_irq = -EINVAL;
	chip->resume_completed = true;
	mutex_init(&chip->irq_complete);
	chip->regmap = devm_regmap_init_i2c(client, &i2c_pmic_regmap_config);
	if (IS_ERR(chip->regmap))
		return PTR_ERR(chip->regmap);
	rc = devm_regmap_qti_debugfs_register(chip->dev, chip->regmap);
	if (rc)
		return rc;
	i2c_set_clientdata(client, chip);
	if (!of_property_read_bool(chip->dev->of_node, "interrupt-controller"))
		goto probe_children;
	rc = i2c_pmic_parse_dt(chip);
	if (rc)
		goto cleanup;
	chip->domain = irq_domain_add_tree(client->dev.of_node, &i2c_pmic_domain_ops, chip);
	if (!chip->domain) {
		rc = -ENOMEM;
		goto cleanup;
	}
	rc = i2c_pmic_determine_initial_status(chip);
	if (rc)
		goto cleanup;
	if (chip->pinctrl_name) {
		chip->pinctrl = devm_pinctrl_get_select(chip->dev, chip->pinctrl_name);
		if (IS_ERR(chip->pinctrl)) {
			rc = PTR_ERR(chip->pinctrl);
			goto cleanup;
		}
	}
	rc = i2c_pmic_toggle_stat(chip);
	if (rc)
		goto cleanup;
	rc = devm_request_threaded_irq(chip->dev, client->irq, NULL, i2c_pmic_irq_handler,
			IRQF_ONESHOT | IRQF_SHARED, "i2c_pmic_stat_irq", chip);
	if (rc)
		goto cleanup;
	chip->summary_irq = client->irq;
	rc = enable_irq_wake(client->irq);
	if (!rc)
		chip->wake_enabled = true;
	else
		dev_warn(chip->dev, "Summary IRQ cannot wake the system: %d\n", rc);
probe_children:
	rc = of_platform_populate(chip->dev->of_node, NULL, NULL, chip->dev);
	if (rc)
		goto cleanup;
	dev_dbg(chip->dev, "I2C PMIC probe successful\n");
	return 0;
cleanup:
	i2c_pmic_cleanup(client);
	return rc;
}

static void i2c_pmic_remove(struct i2c_client *client)
{
	i2c_pmic_cleanup(client);
}

static int __maybe_unused i2c_pmic_suspend_noirq(struct device *dev)
{
	struct i2c_pmic *chip = dev_get_drvdata(dev);

	return READ_ONCE(chip->irq_waiting) ? -EBUSY : 0;
}

/* irq_complete serializes mask programming against the summary handler. */
static int i2c_pmic_set_masks(struct i2c_pmic *chip, bool wake_only)
{
	struct i2c_pmic_periph *periph;
	int i, rc, first_error = 0;

	for (i = 0; i < chip->num_periphs; i++) {
		periph = &chip->periph[i];
		mutex_lock(&periph->lock);
		rc = regmap_write(chip->regmap, periph->addr | INT_EN_CLR_OFFSET, 0xff);
		if (!rc)
			rc = regmap_write(chip->regmap, periph->addr | INT_EN_SET_OFFSET,
				wake_only ? periph->wake : periph->synced[IRQ_EN_SET]);
		if (rc && !first_error)
			first_error = rc;
		mutex_unlock(&periph->lock);
	}
	return first_error;
}

static int __maybe_unused i2c_pmic_suspend(struct device *dev)
{
	struct i2c_pmic *chip = dev_get_drvdata(dev);
	int rc, restore;

	if (chip->summary_irq < 0)
		return 0;
	mutex_lock(&chip->irq_complete);
	rc = i2c_pmic_set_masks(chip, true);
	if (rc) {
		restore = i2c_pmic_set_masks(chip, false);
		if (restore)
			dev_err(dev, "Failed to restore IRQ masks after failed suspend: %d\n", restore);
	} else {
		chip->resume_completed = false;
	}
	mutex_unlock(&chip->irq_complete);
	return rc;
}

static int __maybe_unused i2c_pmic_resume(struct device *dev)
{
	struct i2c_pmic *chip = dev_get_drvdata(dev);
	bool pending, reenable;
	int rc;

	if (chip->summary_irq < 0)
		return 0;
	mutex_lock(&chip->irq_complete);
	rc = i2c_pmic_set_masks(chip, false);
	chip->resume_completed = true;
	pending = chip->irq_waiting;
	reenable = chip->irq_disabled;
	chip->irq_disabled = false;
	mutex_unlock(&chip->irq_complete);
	if (pending)
		i2c_pmic_irq_handler(chip->summary_irq, chip);
	if (reenable)
		enable_irq(chip->summary_irq);
	return rc;
}

static const struct dev_pm_ops __maybe_unused i2c_pmic_pm_ops = {
	.suspend	= pm_sleep_ptr(i2c_pmic_suspend),
	.suspend_noirq	= pm_sleep_ptr(i2c_pmic_suspend_noirq),
	.resume		= pm_sleep_ptr(i2c_pmic_resume),
};

static const struct of_device_id i2c_pmic_match_table[] = {
	{ .compatible = "qcom,i2c-pmic", },
	{ }
};

static const struct i2c_device_id i2c_pmic_id[] = {
	{ "i2c-pmic", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, i2c_pmic_id);

static struct i2c_driver i2c_pmic_driver = {
	.driver		= {
		.name		= "i2c_pmic",
		.pm		= pm_sleep_ptr(&i2c_pmic_pm_ops),
		.of_match_table	= i2c_pmic_match_table,
	},
	.probe		= i2c_pmic_probe,
	.remove		= i2c_pmic_remove,
	.id_table	= i2c_pmic_id,
};
module_i2c_driver(i2c_pmic_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm I2C PMIC register and interrupt driver");
MODULE_ALIAS("i2c:i2c_pmic");
