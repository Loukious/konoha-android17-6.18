// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/qcom-pinctrl.h>
#include <linux/pinctrl/consumer.h>
#include <asoc/msm-cdc-pinctrl.h>

#define MAX_GPIOS 16

struct msm_cdc_pinctrl_info {
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_active;
	struct pinctrl_state *pinctrl_sleep;
	struct pinctrl_state *pinctrl_alt_active;
	int gpio;
	bool state;
	u32 tlmm_gpio[MAX_GPIOS];
	char __iomem *chip_wakeup_register[MAX_GPIOS];
	u32 chip_wakeup_maskbit[MAX_GPIOS];
	u32 count;
	u32 wakeup_reg_count;
	bool wakeup_capable;
	bool chip_wakeup_reg;
};

/* Protect exported operations against platform removal and state changes. */
static DEFINE_MUTEX(msm_cdc_pinctrl_lock);

/* A successful lookup returns with msm_cdc_pinctrl_lock held. */
static struct msm_cdc_pinctrl_info *msm_cdc_pinctrl_get_gpiodata(
						struct device_node *np)
{
	struct platform_device *pdev;
	struct msm_cdc_pinctrl_info *gpio_data;

	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	if (!pdev)
		return NULL;

	mutex_lock(&msm_cdc_pinctrl_lock);
	gpio_data = dev_get_drvdata(&pdev->dev);
	put_device(&pdev->dev);
	if (!gpio_data)
		mutex_unlock(&msm_cdc_pinctrl_lock);
	return gpio_data;
}

/*
 * msm_cdc_get_gpio_state: select pinctrl sleep state
 * @np: pointer to struct device_node
 *
 * Returns error code for failure and GPIO value on success
 */
int msm_cdc_get_gpio_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;
	int value = -EINVAL;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return value;

	if (gpio_is_valid(gpio_data->gpio))
		value = gpio_get_value_cansleep(gpio_data->gpio);

	mutex_unlock(&msm_cdc_pinctrl_lock);
	return value;
}
EXPORT_SYMBOL(msm_cdc_get_gpio_state);

/*
 * msm_cdc_pinctrl_select_sleep_state: select pinctrl sleep state
 * @np: pointer to struct device_node
 *
 * Returns error code for failure
 */
int msm_cdc_pinctrl_select_sleep_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;
	int ret;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	if (!gpio_data->pinctrl_sleep) {
		ret = -EINVAL;
		goto unlock;
	}
	ret = pinctrl_select_state(gpio_data->pinctrl,
				   gpio_data->pinctrl_sleep);
	if (!ret)
		gpio_data->state = false;
unlock:
	mutex_unlock(&msm_cdc_pinctrl_lock);
	return ret;
}
EXPORT_SYMBOL(msm_cdc_pinctrl_select_sleep_state);

/*
 * msm_cdc_pinctrl_select_alt_active_state: select pinctrl alt_active state
 * @np: pointer to struct device_node
 *
 * Returns error code for failure
 */
int msm_cdc_pinctrl_select_alt_active_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;
	int ret;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	if (!gpio_data->pinctrl_alt_active) {
		ret = -EINVAL;
		goto unlock;
	}
	ret = pinctrl_select_state(gpio_data->pinctrl,
				   gpio_data->pinctrl_alt_active);
	if (!ret)
		gpio_data->state = true;
unlock:
	mutex_unlock(&msm_cdc_pinctrl_lock);
	return ret;
}
EXPORT_SYMBOL(msm_cdc_pinctrl_select_alt_active_state);

/*
 * msm_cdc_pinctrl_select_active_state: select pinctrl active state
 * @np: pointer to struct device_node
 *
 * Returns error code for failure
 */
int msm_cdc_pinctrl_select_active_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;
	int ret;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	if (!gpio_data->pinctrl_active) {
		ret = -EINVAL;
		goto unlock;
	}
	ret = pinctrl_select_state(gpio_data->pinctrl,
				   gpio_data->pinctrl_active);
	if (!ret)
		gpio_data->state = true;
unlock:
	mutex_unlock(&msm_cdc_pinctrl_lock);
	return ret;
}
EXPORT_SYMBOL(msm_cdc_pinctrl_select_active_state);

/*
 * msm_cdc_pinctrl_get_state: get curren pinctrl state
 * @np: pointer to struct device_node
 *
 * Returns 0 for sleep state, 1 for active state,
 * error code for failure
 */
int msm_cdc_pinctrl_get_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;
	int state;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;
	state = gpio_data->state;
	mutex_unlock(&msm_cdc_pinctrl_lock);
	return state;
}
EXPORT_SYMBOL(msm_cdc_pinctrl_get_state);

/*
 * msm_cdc_pinctrl_set_wakeup_capable: Set a pinctrl to wakeup capable
 * @np: pointer to struct device_node
 * @enable: wakeup capable when set to true
 *
 * Returns 0 for success and error code for failure
 */
int msm_cdc_pinctrl_set_wakeup_capable(struct device_node *np, bool enable)
{
	struct msm_cdc_pinctrl_info *gpio_data;
	int ret = 0;
	u32 i = 0, temp = 0;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	if (gpio_data->wakeup_capable) {
		for (i = 0; i < gpio_data->count; i++) {
			ret = msm_gpio_mpm_wake_set(gpio_data->tlmm_gpio[i],
						    enable);
			if (ret < 0)
				goto exit;
		}
	}
	if (gpio_data->chip_wakeup_reg) {
		for (i = 0; i < gpio_data->wakeup_reg_count; i++) {
			temp = ioread32(gpio_data->chip_wakeup_register[i]);
			if (enable)
				temp |= BIT(gpio_data->chip_wakeup_maskbit[i]);
			else
				temp &= ~BIT(gpio_data->chip_wakeup_maskbit[i]);
			iowrite32(temp, gpio_data->chip_wakeup_register[i]);
		}
	}
exit:
	mutex_unlock(&msm_cdc_pinctrl_lock);
	return ret;
}
EXPORT_SYMBOL(msm_cdc_pinctrl_set_wakeup_capable);

static void msm_cdc_pinctrl_free_gpio(void *data)
{
	struct msm_cdc_pinctrl_info *gpio_data = data;

	gpio_free(gpio_data->gpio);
}

static int msm_cdc_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct msm_cdc_pinctrl_info *gpio_data;
	u32 chip_wakeup_reg[MAX_GPIOS];
	u32 defaults[MAX_GPIOS];
	bool has_defaults = false;
	int count, ret;
	u32 i, value;

	gpio_data = devm_kzalloc(dev, sizeof(*gpio_data), GFP_KERNEL);
	if (!gpio_data)
		return -ENOMEM;
	gpio_data->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(gpio_data->pinctrl))
		return PTR_ERR(gpio_data->pinctrl);
	gpio_data->pinctrl_active = pinctrl_lookup_state(gpio_data->pinctrl,
							 "aud_active");
	if (IS_ERR(gpio_data->pinctrl_active))
		return PTR_ERR(gpio_data->pinctrl_active);
	gpio_data->pinctrl_sleep = pinctrl_lookup_state(gpio_data->pinctrl,
							"aud_sleep");
	if (IS_ERR(gpio_data->pinctrl_sleep))
		return PTR_ERR(gpio_data->pinctrl_sleep);
	gpio_data->pinctrl_alt_active = pinctrl_lookup_state(gpio_data->pinctrl,
							     "aud_alt_active");
	if (IS_ERR(gpio_data->pinctrl_alt_active)) {
		ret = PTR_ERR(gpio_data->pinctrl_alt_active);
		if (ret == -EPROBE_DEFER)
			return ret;
		gpio_data->pinctrl_alt_active = NULL;
	}

	if (of_property_present(np, "qcom,chip-wakeup-reg")) {
		count = of_property_count_u32_elems(np, "qcom,chip-wakeup-reg");
		if (count <= 0 || count > MAX_GPIOS)
			return -EINVAL;
		ret = of_property_read_u32_array(np, "qcom,chip-wakeup-reg",
						 chip_wakeup_reg, count);
		if (ret)
			return ret;
		ret = of_property_read_u32_array(np, "qcom,chip-wakeup-maskbit",
						 gpio_data->chip_wakeup_maskbit, count);
		if (ret)
			return ret;
		for (i = 0; i < count; i++) {
			if (gpio_data->chip_wakeup_maskbit[i] >= 32)
				return -EINVAL;
			gpio_data->chip_wakeup_register[i] =
				devm_ioremap(dev, chip_wakeup_reg[i], sizeof(u32));
			if (!gpio_data->chip_wakeup_register[i])
				return -ENOMEM;
		}
		if (of_property_present(np, "qcom,chip-wakeup-default-val")) {
			ret = of_property_read_u32_array(np, "qcom,chip-wakeup-default-val",
							 defaults, count);
			if (ret)
				return ret;
			has_defaults = true;
		}
		gpio_data->wakeup_reg_count = count;
		gpio_data->chip_wakeup_reg = true;
	}
	if (of_property_present(np, "qcom,tlmm-pins")) {
		count = of_property_count_u32_elems(np, "qcom,tlmm-pins");
		if (count <= 0 || count > MAX_GPIOS)
			return -EINVAL;
		ret = of_property_read_u32_array(np, "qcom,tlmm-pins",
						 gpio_data->tlmm_gpio, count);
		if (ret)
			return ret;
		gpio_data->count = count;
		gpio_data->wakeup_capable = true;
	}
	gpio_data->gpio = of_get_named_gpio(np, "qcom,cdc-rst-n-gpio", 0);
	if (gpio_data->gpio == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (gpio_is_valid(gpio_data->gpio)) {
		ret = gpio_request(gpio_data->gpio, "MSM_CDC_RESET");
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(dev, msm_cdc_pinctrl_free_gpio, gpio_data);
		if (ret)
			return ret;
	}
	/* Apply defaults only after validating and acquiring all resources. */
	if (!of_property_read_bool(np, "qcom,lpi-gpios")) {
		ret = pinctrl_select_state(gpio_data->pinctrl,
					   gpio_data->pinctrl_sleep);
		if (ret)
			return ret;
	}
	if (has_defaults) {
		for (i = 0; i < gpio_data->wakeup_reg_count; i++) {
			value = ioread32(gpio_data->chip_wakeup_register[i]);
			if (defaults[i])
				value |= BIT(gpio_data->chip_wakeup_maskbit[i]);
			else
				value &= ~BIT(gpio_data->chip_wakeup_maskbit[i]);
			iowrite32(value, gpio_data->chip_wakeup_register[i]);
		}
	}
	mutex_lock(&msm_cdc_pinctrl_lock);
	dev_set_drvdata(dev, gpio_data);
	mutex_unlock(&msm_cdc_pinctrl_lock);
	return 0;
}

static void msm_cdc_pinctrl_remove(struct platform_device *pdev)
{
	/* Drain API users before devres releases GPIOs, pinctrl and MMIO. */
	mutex_lock(&msm_cdc_pinctrl_lock);
	dev_set_drvdata(&pdev->dev, NULL);
	mutex_unlock(&msm_cdc_pinctrl_lock);
}

static const struct of_device_id msm_cdc_pinctrl_match[] = {
	{.compatible = "qcom,msm-cdc-pinctrl"},
	{}
};

static struct platform_driver msm_cdc_pinctrl_driver = {
	.driver = {
		.name = "msm-cdc-pinctrl",
		.owner = THIS_MODULE,
		.of_match_table = msm_cdc_pinctrl_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_cdc_pinctrl_probe,
	.remove = msm_cdc_pinctrl_remove,
};

int msm_cdc_pinctrl_drv_init(void)
{
	return platform_driver_register(&msm_cdc_pinctrl_driver);
}

void msm_cdc_pinctrl_drv_exit(void)
{
	platform_driver_unregister(&msm_cdc_pinctrl_driver);
}
MODULE_DESCRIPTION("MSM CODEC pin control platform driver");
MODULE_LICENSE("GPL v2");
