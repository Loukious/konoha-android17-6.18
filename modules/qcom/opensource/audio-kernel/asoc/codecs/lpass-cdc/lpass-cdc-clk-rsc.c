// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/limits.h>
#include "lpass-cdc.h"
#include "internal.h"
#include "lpass-cdc-clk-rsc.h"

#define DRV_NAME "lpass-cdc-clk-rsc"
#define LPASS_CDC_CLK_NAME_LENGTH 30

static char clk_src_name[MAX_CLK][LPASS_CDC_CLK_NAME_LENGTH] = {
	"tx_core_clk",
	"rx_core_clk",
	"wsa_core_clk",
	"va_core_clk",
	"wsa2_core_clk",
	"rx_tx_core_clk",
	"wsa_tx_core_clk",
	"wsa2_tx_core_clk",
};

struct lpass_cdc_clk_rsc {
	struct device *dev;
	struct mutex rsc_clk_lock;
	struct mutex fs_gen_lock;
	struct clk *clk[MAX_CLK];
	int clk_cnt[MAX_CLK];
	int reg_seq_en_cnt;
	int request_count[MAX_CLK];
	int owned[MAX_CLK];
	int all_count;
	bool dev_up;
	bool dev_up_gfmux;
	u32 num_fs_reg;
	u32 *fs_gen_seq;
	int default_clk_id[MAX_CLK];
	struct regmap *regmap;
	char __iomem *rx_clk_muxsel;
	char __iomem *wsa_clk_muxsel;
	char __iomem *va_clk_muxsel;
};

static int lpass_cdc_clk_rsc_cb(struct device *dev, u16 event)
{
	struct lpass_cdc_clk_rsc *priv;

	if (!dev) {
		pr_err("%s: Invalid device pointer\n",
				__func__);
		return -EINVAL;
	}

	priv = dev_get_drvdata(dev);
	if (!priv) {
		pr_err("%s: Invalid clk rsc priviate data\n",
				__func__);
		return -EINVAL;
	}

	mutex_lock(&priv->rsc_clk_lock);
	if (event == LPASS_CDC_MACRO_EVT_SSR_UP) {
		priv->dev_up = true;
	} else if (event == LPASS_CDC_MACRO_EVT_SSR_DOWN) {
		priv->dev_up = false;
		priv->dev_up_gfmux = false;
	} else if (event == LPASS_CDC_MACRO_EVT_SSR_GFMUX_UP) {
		priv->dev_up_gfmux = true;
	}
	mutex_unlock(&priv->rsc_clk_lock);

	return 0;
}

static char __iomem *lpass_cdc_clk_rsc_get_clk_muxsel(struct lpass_cdc_clk_rsc *priv,
						 int clk_id)
{
	switch (clk_id) {
	case RX_CORE_CLK:
		return priv->rx_clk_muxsel;
	case WSA_CORE_CLK:
	case WSA2_CORE_CLK:
		return priv->wsa_clk_muxsel;
	case VA_CORE_CLK:
		return priv->va_clk_muxsel;
	case TX_CORE_CLK:
	case RX_TX_CORE_CLK:
	case WSA_TX_CORE_CLK:
	case WSA2_TX_CORE_CLK:
	default:
		dev_err_ratelimited(priv->dev, "%s: Invalid case\n", __func__);
		break;
	}

	return NULL;
}

/* Parent removal drains these operations before releasing provider devres. */
static struct lpass_cdc_clk_rsc *lpass_cdc_clk_get(struct device *dev,
						   struct lpass_cdc_priv **cdc)
{
	struct lpass_cdc_priv *parent;
	struct lpass_cdc_clk_rsc *priv;

	if (!dev || !dev->parent ||
	    !of_device_is_compatible(dev->parent->of_node, "qcom,lpass-cdc"))
		return NULL;
	parent = dev_get_drvdata(dev->parent);
	if (!lpass_cdc_op_get(parent, false))
		return NULL;
	priv = parent->clk_dev ? dev_get_drvdata(parent->clk_dev) : NULL;
	if (!priv) {
		lpass_cdc_op_put(parent, false);
		return NULL;
	}
	*cdc = parent;
	return priv;
}

static int lpass_cdc_clock_enable(struct lpass_cdc_clk_rsc *priv, int id)
{
	int ret;

	if (!priv->clk[id])
		return -ENODEV;
	ret = clk_prepare_enable(priv->clk[id]);
	if (!ret)
		priv->owned[id]++;
	return ret;
}

static void lpass_cdc_clock_disable(struct lpass_cdc_clk_rsc *priv, int id)
{
	if (WARN_ON(!priv->owned[id]))
		return;
	clk_disable_unprepare(priv->clk[id]);
	priv->owned[id]--;
}

int lpass_cdc_rsc_clk_reset(struct device *dev, int clk_id)
{
	struct lpass_cdc_priv *cdc;
	struct lpass_cdc_clk_rsc *priv;
	int wanted, ret = 0;

	if (clk_id < 0 || clk_id >= MAX_CLK)
		return -EINVAL;
	priv = lpass_cdc_clk_get(dev, &cdc);
	if (!priv)
		return -ENODEV;
	mutex_lock(&priv->rsc_clk_lock);
	if (!priv->clk[clk_id]) {
		ret = -ENODEV;
		goto out;
	}
	wanted = !!priv->clk_cnt[clk_id] + !!priv->all_count;
	while (priv->owned[clk_id])
		lpass_cdc_clock_disable(priv, clk_id);
	while (priv->owned[clk_id] < wanted) {
		ret = lpass_cdc_clock_enable(priv, clk_id);
		if (ret) {
			priv->dev_up = false;
			break;
		}
	}
out:
	mutex_unlock(&priv->rsc_clk_lock);
	lpass_cdc_op_put(cdc, false);
	return ret;
}
EXPORT_SYMBOL(lpass_cdc_rsc_clk_reset);

int lpass_cdc_clk_rsc_all_clocks(struct device *dev, bool enable)
{
	struct lpass_cdc_priv *cdc;
	struct lpass_cdc_clk_rsc *priv;
	bool added[MAX_CLK] = {};
	int i, ret = 0;

	priv = lpass_cdc_clk_get(dev, &cdc);
	if (!priv)
		return -ENODEV;
	mutex_lock(&priv->rsc_clk_lock);
	if (enable) {
		if (priv->all_count == INT_MAX) {
			ret = -EOVERFLOW;
			goto out;
		}
		if (!priv->all_count) {
			for (i = 0; i < MAX_CLK; i++) {
				if (!priv->clk[i])
					continue;
				ret = lpass_cdc_clock_enable(priv, i);
				if (ret)
					goto rollback;
				added[i] = true;
			}
		}
		priv->all_count++;
	} else {
		if (!priv->all_count) {
			ret = -EINVAL;
			goto out;
		}
		if (--priv->all_count == 0)
			for (i = MAX_CLK - 1; i >= 0; i--)
				if (priv->owned[i] > !!priv->clk_cnt[i])
					lpass_cdc_clock_disable(priv, i);
	}
	goto out;
rollback:
	for (i = MAX_CLK - 1; i >= 0; i--)
		if (added[i])
			lpass_cdc_clock_disable(priv, i);
out:
	mutex_unlock(&priv->rsc_clk_lock);
	lpass_cdc_op_put(cdc, false);
	return ret;
}

void lpass_cdc_clk_rsc_enable_all_clocks(struct device *dev, bool enable)
{
	int ret = lpass_cdc_clk_rsc_all_clocks(dev, enable);

	if (ret && dev)
		dev_err_ratelimited(dev, "all-clocks request failed: %d\n", ret);
}
EXPORT_SYMBOL(lpass_cdc_clk_rsc_enable_all_clocks);

/* Preflight the complete VA/TX transition. Acquire every needed source before
 * changing a mux or dropping the previous source; failure only drops our new
 * acquisitions. Logical references are committed after the hardware sequence.
 */
static int lpass_cdc_clock_request(struct lpass_cdc_clk_rsc *priv,
				   int default_id, int id, bool enable)
{
	int requested[MAX_CLK], next[MAX_CLK], wanted[MAX_CLK];
	int before[MAX_CLK];
	char __iomem *mux = NULL;
	int i, ret, active;
	bool switch_mux, first, last;

	if (id < 0 || id >= MAX_CLK || default_id < 0 || default_id >= MAX_CLK)
		return -EINVAL;
	if (enable && !priv->dev_up)
		return -ENOTSYNC;
	if (!priv->clk[id])
		return -ENODEV;
	active = priv->request_count[id];
	if (active && priv->default_clk_id[id] != default_id)
		return -EBUSY;
	if (enable && active == INT_MAX)
		return -EOVERFLOW;
	if (!enable && !active)
		return -EINVAL;
	switch_mux = default_id != id && id != VA_CORE_CLK;
	if (switch_mux) {
		mux = lpass_cdc_clk_rsc_get_clk_muxsel(priv, id);
		if (!mux || !priv->clk[default_id])
			return -EINVAL;
	}
	memcpy(requested, priv->request_count, sizeof(requested));
	requested[id] += enable ? 1 : -1;
	memcpy(next, requested, sizeof(next));
	if (requested[VA_CORE_CLK] && requested[TX_CORE_CLK] &&
	    (id == VA_CORE_CLK ? default_id : priv->default_clk_id[VA_CORE_CLK])
	    != VA_CORE_CLK) {
		if (requested[VA_CORE_CLK] > INT_MAX - requested[TX_CORE_CLK])
			return -EOVERFLOW;
		next[TX_CORE_CLK] += requested[VA_CORE_CLK];
		next[VA_CORE_CLK] = 0;
	}
	first = enable && !active;
	last = !enable && active == 1;
	memcpy(before, priv->owned, sizeof(before));
	for (i = 0; i < MAX_CLK; i++) {
		wanted[i] = !!next[i] + (priv->clk[i] && priv->all_count ? 1 : 0);
		if (wanted[i] && !priv->clk[i])
			return -ENODEV;
	}
	/* A mux edge needs its default source, including on disable. */
	if (switch_mux && (first || last) && !priv->owned[default_id]) {
		ret = lpass_cdc_clock_enable(priv, default_id);
		if (ret)
			return ret;
	}
	for (i = 0; i < MAX_CLK; i++) {
		while (priv->owned[i] < wanted[i]) {
			ret = lpass_cdc_clock_enable(priv, i);
			if (ret)
				goto rollback;
		}
	}
	/* Native VA mux switching is performed by the DSP. */
	if (switch_mux && (first || last) && priv->dev_up_gfmux) {
		iowrite32(enable ? 1 : 0, mux);
		ioread32(mux);
	}
	/* Keep the fallback source enabled until the old mux source is dropped. */
	if (switch_mux && last)
		while (priv->owned[id] > wanted[id])
			lpass_cdc_clock_disable(priv, id);
	for (i = MAX_CLK - 1; i >= 0; i--)
		while (priv->owned[i] > wanted[i])
			lpass_cdc_clock_disable(priv, i);
	memcpy(priv->request_count, requested, sizeof(requested));
	memcpy(priv->clk_cnt, next, sizeof(next));
	priv->default_clk_id[id] = default_id;
	return 0;
rollback:
	for (i = MAX_CLK - 1; i >= 0; i--)
		while (priv->owned[i] > before[i])
			lpass_cdc_clock_disable(priv, i);
	return ret;
}

int lpass_cdc_clk_rsc_request_clock(struct device *dev, int default_clk_id,
				    int clk_id_req, bool enable)
{
	struct lpass_cdc_priv *cdc;
	struct lpass_cdc_clk_rsc *priv;
	int ret;

	if (clk_id_req < 0 || clk_id_req >= MAX_CLK ||
	    default_clk_id < 0 || default_clk_id >= MAX_CLK)
		return -EINVAL;
	priv = lpass_cdc_clk_get(dev, &cdc);
	if (!priv)
		return -ENODEV;
	mutex_lock(&priv->rsc_clk_lock);
	ret = lpass_cdc_clock_request(priv, default_clk_id, clk_id_req, enable);
	mutex_unlock(&priv->rsc_clk_lock);
	lpass_cdc_op_put(cdc, false);
	return ret;
}
EXPORT_SYMBOL(lpass_cdc_clk_rsc_request_clock);

static int lpass_cdc_fs_gen_request(struct lpass_cdc_clk_rsc *priv, bool enable)
{
	unsigned int *old;
	int n = priv->num_fs_reg, step, idx, ret = 0, err;

	if (enable && priv->reg_seq_en_cnt == INT_MAX)
		return -EOVERFLOW;
	if (!enable && !priv->reg_seq_en_cnt)
		return -EINVAL;
	if ((enable && priv->reg_seq_en_cnt) ||
	    (!enable && priv->reg_seq_en_cnt > 1)) {
		priv->reg_seq_en_cnt += enable ? 1 : -1;
		return 0;
	}
	old = kcalloc(n, sizeof(*old), GFP_KERNEL);
	if (!old)
		return -ENOMEM;
	for (step = 0; step < n; step++) {
		idx = (enable ? step : n - step - 1) * 3;
		ret = regmap_read(priv->regmap, priv->fs_gen_seq[idx], &old[step]);
		if (ret)
			goto rollback;
		ret = regmap_update_bits(priv->regmap, priv->fs_gen_seq[idx],
					 priv->fs_gen_seq[idx + 1],
					 enable ? priv->fs_gen_seq[idx + 2] : 0);
		if (ret) {
			/* Include the failed write: a transport can fail after writing. */
			step++;
			goto rollback;
		}
	}
	priv->reg_seq_en_cnt += enable ? 1 : -1;
	goto out;
rollback:
	while (step--) {
		idx = (enable ? step : n - step - 1) * 3;
		err = regmap_update_bits(priv->regmap, priv->fs_gen_seq[idx],
					 priv->fs_gen_seq[idx + 1], old[step]);
		if (err)
			dev_err_ratelimited(priv->dev, "FS rollback failed: %d\n", err);
	}
out:
	kfree(old);
	return ret;
}

void lpass_cdc_clk_rsc_fs_gen_request(struct device *dev, bool enable)
{
	struct lpass_cdc_priv *cdc;
	struct lpass_cdc_clk_rsc *priv;
	int ret;

	priv = lpass_cdc_clk_get(dev, &cdc);
	if (!priv)
		return;
	mutex_lock(&priv->fs_gen_lock);
	ret = lpass_cdc_fs_gen_request(priv, enable);
	mutex_unlock(&priv->fs_gen_lock);
	if (ret)
		dev_err_ratelimited(dev, "FS request failed: %d\n", ret);
	lpass_cdc_op_put(cdc, false);
}
EXPORT_SYMBOL(lpass_cdc_clk_rsc_fs_gen_request);

static int lpass_cdc_clk_rsc_probe(struct platform_device *pdev)
{
	int ret = 0, fs_gen_size, i, j;
	const char **clk_name_array;
	int clk_cnt;
	struct clk *clk;
	struct lpass_cdc_clk_rsc *priv = NULL;
	u32 muxsel = 0;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct lpass_cdc_clk_rsc),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Get clk fs gen sequence from device tree */
	if (!of_find_property(pdev->dev.of_node, "qcom,fs-gen-sequence",
						  &fs_gen_size)) {
		dev_err(&pdev->dev, "%s: unable to find qcom,fs-gen-sequence property\n",
			__func__);
		ret = -EINVAL;
		goto err;
	}
	if (fs_gen_size <= 0 || fs_gen_size % (3 * sizeof(u32)))
		return -EINVAL;
	priv->num_fs_reg = fs_gen_size / (3 * sizeof(u32));
	priv->fs_gen_seq = devm_kzalloc(&pdev->dev, fs_gen_size, GFP_KERNEL);
	if (!priv->fs_gen_seq) {
		ret = -ENOMEM;
		goto err;
	}
	dev_dbg(&pdev->dev, "%s: num_fs_reg %d\n", __func__, priv->num_fs_reg);
	/* Parse fs-gen-sequence */
	ret = of_property_read_u32_array(pdev->dev.of_node,
					 "qcom,fs-gen-sequence",
					 priv->fs_gen_seq,
					 priv->num_fs_reg * 3);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: unable to parse fs-gen-sequence, ret = %d\n",
			__func__, ret);
		goto err;
	}

	for (i = 0; i < priv->num_fs_reg * 3; i += 3) {
		if (priv->fs_gen_seq[i] > U16_MAX || (priv->fs_gen_seq[i] & 3) ||
		    !priv->fs_gen_seq[i + 1] || priv->fs_gen_seq[i + 1] > U8_MAX ||
		    priv->fs_gen_seq[i + 2] & ~priv->fs_gen_seq[i + 1])
			return -EINVAL;
	}
	priv->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!priv->regmap)
		return -EPROBE_DEFER;

	/* Get clk details from device tree */
	clk_cnt = of_property_count_strings(pdev->dev.of_node, "clock-names");
	if (clk_cnt <= 0 || clk_cnt > MAX_CLK) {
		dev_err(&pdev->dev, "%s: Invalid number of clocks %d",
				__func__, clk_cnt);
		ret = -EINVAL;
		goto err;
	}
	clk_name_array = devm_kzalloc(&pdev->dev, clk_cnt * sizeof(char *),
					  GFP_KERNEL);
	if (!clk_name_array) {
		ret = -ENOMEM;
		goto err;
	}

	ret = of_property_read_string_array(pdev->dev.of_node, "clock-names",
					clk_name_array, clk_cnt);
	if (ret != clk_cnt)
		return ret < 0 ? ret : -EINVAL;

	for (i = 0; i < MAX_CLK; i++) {
		priv->clk[i] = NULL;
		for (j = 0; j < clk_cnt; j++) {
			if (!strcmp(clk_src_name[i], clk_name_array[j])) {
				clk = devm_clk_get(&pdev->dev, clk_src_name[i]);
				if (IS_ERR(clk)) {
					ret = PTR_ERR(clk);
					dev_err(&pdev->dev, "%s: clk get failed for %s with ret %d\n",
						__func__, clk_src_name[i], ret);
					goto err;
				}
				priv->clk[i] = clk;
				dev_dbg(&pdev->dev, "%s: clk get success for clk name %s\n",
						__func__, clk_src_name[i]);
				break;
			}
		}
	}
	ret = of_property_read_u32(pdev->dev.of_node,
				 "qcom,rx_mclk_mode_muxsel", &muxsel);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: could not find qcom,rx_mclk_mode_muxsel entry in dt\n",
			__func__);
	} else {
		priv->rx_clk_muxsel = devm_ioremap(&pdev->dev, muxsel, 0x4);
		if (!priv->rx_clk_muxsel) {
			dev_err(&pdev->dev, "%s: ioremap failed for rx muxsel\n",
				__func__);
			return -ENOMEM;
		}
	}
	ret = of_property_read_u32(pdev->dev.of_node,
				"qcom,wsa_mclk_mode_muxsel", &muxsel);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: could not find qcom,wsa_mclk_mode_muxsel entry in dt\n",
			__func__);
	} else {
		priv->wsa_clk_muxsel = devm_ioremap(&pdev->dev, muxsel, 0x4);
		if (!priv->wsa_clk_muxsel) {
			dev_err(&pdev->dev, "%s: ioremap failed for wsa muxsel\n",
				__func__);
			return -ENOMEM;
		}
	}
	ret = of_property_read_u32(pdev->dev.of_node,
				 "qcom,va_mclk_mode_muxsel", &muxsel);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: could not find qcom,va_mclk_mode_muxsel entry in dt\n",
			__func__);
	} else {
		priv->va_clk_muxsel = devm_ioremap(&pdev->dev, muxsel, 0x4);
		if (!priv->va_clk_muxsel) {
			dev_err(&pdev->dev, "%s: ioremap failed for va muxsel\n",
				__func__);
			return -ENOMEM;
		}
	}

	priv->dev = &pdev->dev;
	priv->dev_up = true;
	priv->dev_up_gfmux = true;
	mutex_init(&priv->rsc_clk_lock);
	mutex_init(&priv->fs_gen_lock);
	dev_set_drvdata(&pdev->dev, priv);
	ret = lpass_cdc_register_res_clk(&pdev->dev, lpass_cdc_clk_rsc_cb);
	if (ret)
		dev_set_drvdata(&pdev->dev, NULL);

err:
	return ret;
}

static void lpass_cdc_clk_rsc_remove(struct platform_device *pdev)
{
	struct lpass_cdc_clk_rsc *priv = dev_get_drvdata(&pdev->dev);
	int i;

	lpass_cdc_unregister_res_clk(&pdev->dev);
	for (i = MAX_CLK - 1; i >= 0; i--)
		while (priv->owned[i])
			lpass_cdc_clock_disable(priv, i);
}

static const struct of_device_id lpass_cdc_clk_rsc_dt_match[] = {
	{.compatible = "qcom,lpass-cdc-clk-rsc-mngr"},
	{}
};
MODULE_DEVICE_TABLE(of, lpass_cdc_clk_rsc_dt_match);

static struct platform_driver lpass_cdc_clk_rsc_mgr = {
	.driver = {
		.name = "lpass-cdc-clk-rsc-mngr",
		.owner = THIS_MODULE,
		.of_match_table = lpass_cdc_clk_rsc_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = lpass_cdc_clk_rsc_probe,
	.remove = lpass_cdc_clk_rsc_remove,
};

int lpass_cdc_clk_rsc_mgr_init(void)
{
	return platform_driver_register(&lpass_cdc_clk_rsc_mgr);
}

void lpass_cdc_clk_rsc_mgr_exit(void)
{
	platform_driver_unregister(&lpass_cdc_clk_rsc_mgr);
}
MODULE_DESCRIPTION("LPASS codec clock resource manager driver");
MODULE_LICENSE("GPL v2");
