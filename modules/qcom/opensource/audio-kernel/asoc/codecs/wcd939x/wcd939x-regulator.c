// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/regulator/driver.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/component.h>
#include <sound/soc.h>
#include <asoc/wcd-mbhc-v2.h>
#include "wcd939x.h"

#define WCD939X_MAX_MICBIAS 4

struct wcd_reg_priv {
	struct device *dev;
	struct regulator_dev *wcd_reg_rdev[WCD939X_MAX_MICBIAS];
	bool mb_enabled_count[WCD939X_MAX_MICBIAS];
};

static inline bool is_valid_micb(int micb)
{
	if ((micb == MIC_BIAS_1) || (micb == MIC_BIAS_2) ||
			(micb == MIC_BIAS_3) || (micb == MIC_BIAS_4))
		return true;

	return false;
}

static int get_micb_num(struct regulator_dev *rdev)
{
	struct wcd_reg_priv *priv = rdev_get_drvdata(rdev);
	int i = 0;

	for (i = 0; i < WCD939X_MAX_MICBIAS; ++i) {
		if (rdev == priv->wcd_reg_rdev[i])
			return (i + 1);
	}
	return -ENODEV;
}

static int wcd_reg_enable_mb(struct regulator_dev *rdev)
{
	struct wcd_reg_priv *priv = rdev_get_drvdata(rdev);
	int micb = 0, ret;

	micb = get_micb_num(rdev);
	if (is_valid_micb(micb)) {
		ret = wcd939x_micb_external_event(priv->dev, micb, true);
		if (!ret)
			priv->mb_enabled_count[micb - 1] = true;

		return ret;
	}

	return -ENODEV;
}

static int wcd_reg_disable_mb(struct regulator_dev *rdev)
{
	struct wcd_reg_priv *priv = rdev_get_drvdata(rdev);
	int micb = get_micb_num(rdev);
	int ret;

	if (!is_valid_micb(micb))
		return -ENODEV;
	ret = wcd939x_micb_external_event(priv->dev, micb, false);
	if (!ret)
		priv->mb_enabled_count[micb - 1] = false;
	return ret;
}

static int wcd_reg_is_enabled_mb(struct regulator_dev *rdev)
{
	struct wcd_reg_priv *priv = rdev_get_drvdata(rdev);
	int micb = 0;

	micb = get_micb_num(rdev);
	if (is_valid_micb(micb))
		return priv->mb_enabled_count[micb - 1];

	return false;
}


static const struct regulator_ops wcd_reg_mb_ops = {
	.enable		= wcd_reg_enable_mb,
	.disable	= wcd_reg_disable_mb,
	.is_enabled	= wcd_reg_is_enabled_mb,
};

static struct regulator_desc wcd_reg_mb_rdesc[] = {
	{
		.owner		= THIS_MODULE,
		.of_match	= "qcom,wcd-mb1-reg",
		.name		= "wcd-mb1-reg",
		.type		= REGULATOR_VOLTAGE,
		.ops		= &wcd_reg_mb_ops,
	},
	{
		.owner		= THIS_MODULE,
		.of_match	= "qcom,wcd-mb2-reg",
		.name		= "wcd-mb2-reg",
		.type		= REGULATOR_VOLTAGE,
		.ops		= &wcd_reg_mb_ops,
	},
	{
		.owner		= THIS_MODULE,
		.of_match	= "qcom,wcd-mb3-reg",
		.name		= "wcd-mb3-reg",
		.type		= REGULATOR_VOLTAGE,
		.ops		= &wcd_reg_mb_ops,
	},
	{
		.owner		= THIS_MODULE,
		.of_match	= "qcom,wcd-mb4-reg",
		.name		= "wcd-mb4-reg",
		.type		= REGULATOR_VOLTAGE,
		.ops		= &wcd_reg_mb_ops,
	},
};


static void wcd_reg_release_votes(void *data)
{
	struct wcd_reg_priv *priv = data;
	int i, ret;

	/* Regulator interfaces have been withdrawn; the codec is still alive. */
	for (i = 0; i < WCD939X_MAX_MICBIAS; i++) {
		if (!priv->mb_enabled_count[i])
			continue;
		ret = wcd939x_micb_external_event(priv->dev, i + 1, false);
		if (ret)
			dev_err(priv->dev, "Failed to release micbias %d: %d\n", i + 1, ret);
		else
			priv->mb_enabled_count[i] = false;
	}
}

int wcd_init_mb_regulator(struct device *dev)
{
	struct regulator_config cfg = {};
	struct wcd_reg_priv *priv;
	int ret, i;

	if (!devres_open_group(dev, wcd_init_mb_regulator, GFP_KERNEL))
		return -ENOMEM;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err;
	}
	priv->dev = dev;
	ret = devm_add_action_or_reset(dev, wcd_reg_release_votes, priv);
	if (ret)
		goto err;
	cfg.dev = dev;
	cfg.driver_data = priv;
	for (i = 0; i < WCD939X_MAX_MICBIAS; i++) {
		priv->wcd_reg_rdev[i] = devm_regulator_register(dev, &wcd_reg_mb_rdesc[i], &cfg);
		if (IS_ERR(priv->wcd_reg_rdev[i])) {
			ret = PTR_ERR(priv->wcd_reg_rdev[i]);
			goto err;
		}
	}
	devres_close_group(dev, wcd_init_mb_regulator);
	return 0;
err:
	devres_release_group(dev, wcd_init_mb_regulator);
	return ret;
}

void wcd_deinit_mb_regulator(struct device *dev)
{
	devres_release_group(dev, wcd_init_mb_regulator);
}
