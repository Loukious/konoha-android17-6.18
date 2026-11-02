// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <bindings/qcom,audio-ext-clk.h>
#include <linux/ratelimit.h>
#include "audio-ext-clk-up.h"
#ifdef CONFIG_AUDIO_PRM
#include <dsp/audio_prm.h>
#endif
enum {
	AUDIO_EXT_CLK_PMI,
	AUDIO_EXT_CLK_LNBB2,
	AUDIO_EXT_CLK_LPASS,
	AUDIO_EXT_CLK_LPASS2,
	AUDIO_EXT_CLK_LPASS3,
	AUDIO_EXT_CLK_LPASS4,
	AUDIO_EXT_CLK_LPASS5,
	AUDIO_EXT_CLK_LPASS6,
	AUDIO_EXT_CLK_LPASS7,
	AUDIO_EXT_CLK_LPASS_CORE_HW_VOTE,
	AUDIO_EXT_CLK_LPASS8,
	AUDIO_EXT_CLK_LPASS_AUDIO_HW_VOTE,
	AUDIO_EXT_CLK_LPASS9,
	AUDIO_EXT_CLK_LPASS10,
	AUDIO_EXT_CLK_LPASS11,
	AUDIO_EXT_CLK_LPASS12,
	AUDIO_EXT_CLK_LPASS13,
	AUDIO_EXT_CLK_LPASS14,
	AUDIO_EXT_CLK_LPASS15,
	AUDIO_EXT_CLK_LPASS16,
	AUDIO_EXT_CLK_LPASS_MAX,
	AUDIO_EXT_CLK_EXTERNAL_PLL = AUDIO_EXT_CLK_LPASS_MAX,
	AUDIO_EXT_CLK_MAX,
};

struct pinctrl_info {
	struct pinctrl *pinctrl;
	struct pinctrl_state *sleep;
	struct pinctrl_state *active;
	char __iomem *base;
};

struct audio_ext_clk {
	struct pinctrl_info pnctrl_info;
	struct clk_fixed_factor fact;
};

struct audio_ext_clk_priv {
	struct device *dev;
	int clk_src;
	uint32_t enable;
	bool pins_active;
	bool gate_enabled;
	const struct clk_init_data *clk_init;
#ifdef CONFIG_AUDIO_PRM
	struct clk_cfg prm_clk_cfg;
#endif
	struct audio_ext_clk audio_clk;
	const char *clk_name;
	uint32_t lpass_core_hwvote_client_handle;
	uint32_t lpass_audio_hwvote_client_handle;
};

static inline struct audio_ext_clk_priv *to_audio_clk(struct clk_hw *hw)
{
	return container_of(hw, struct audio_ext_clk_priv, audio_clk.fact.hw);
}

static int audio_ext_clk_set_vote(struct audio_ext_clk_priv *clk_priv,
				  bool enable)
{
#ifdef CONFIG_AUDIO_PRM
	return audio_prm_set_lpass_clk_cfg(&clk_priv->prm_clk_cfg, enable);
#else
	return -EPERM;
#endif
}

static int audio_ext_clk_prepare(struct clk_hw *hw)
{
	struct audio_ext_clk_priv *clk_priv = to_audio_clk(hw);
	struct pinctrl_info *pnctrl_info = &clk_priv->audio_clk.pnctrl_info;
	bool acquired = false;
	int ret;

	if (clk_priv->clk_src >= AUDIO_EXT_CLK_LPASS &&
	    clk_priv->clk_src < AUDIO_EXT_CLK_LPASS_MAX && !clk_priv->enable) {
		ret = audio_ext_clk_set_vote(clk_priv, true);
		if (ret < 0) {
			pr_err_ratelimited("%s: clock request failed: %d\n",
					   __func__, ret);
			return ret;
		}
		clk_priv->enable = 1;
		acquired = true;
	}

	if (pnctrl_info->pinctrl) {
		ret = pinctrl_select_state(pnctrl_info->pinctrl,
					   pnctrl_info->active);
		if (ret) {
			if (acquired) {
				int rollback = audio_ext_clk_set_vote(clk_priv, false);

				if (rollback >= 0)
					clk_priv->enable = 0;
				else
					pr_err("%s: clock rollback failed: %d\n",
					       __func__, rollback);
			}
			return ret;
		}
		clk_priv->pins_active = true;
	}

	if (pnctrl_info->base) {
		iowrite32(1, pnctrl_info->base);
		clk_priv->gate_enabled = true;
	}
	return 0;
}

static void audio_ext_clk_unprepare(struct clk_hw *hw)
{
	struct audio_ext_clk_priv *clk_priv = to_audio_clk(hw);
	struct pinctrl_info *pnctrl_info = &clk_priv->audio_clk.pnctrl_info;
	int ret;

	if (clk_priv->pins_active) {
		ret = pinctrl_select_state(pnctrl_info->pinctrl,
					   pnctrl_info->sleep);
		if (ret) {
			pr_err("%s: sleep state select failed: %d\n", __func__, ret);
			return;
		}
		clk_priv->pins_active = false;
	}

	if (clk_priv->enable) {
		ret = audio_ext_clk_set_vote(clk_priv, false);
		if (ret < 0)
			pr_err_ratelimited("%s: clock release failed: %d\n",
					   __func__, ret);
		else
			clk_priv->enable = 0;
	}

	if (clk_priv->gate_enabled) {
		iowrite32(0, pnctrl_info->base);
		clk_priv->gate_enabled = false;
	}
}

static u8 audio_ext_clk_get_parent(struct clk_hw *hw)
{
	struct audio_ext_clk_priv *clk_priv = to_audio_clk(hw);
	int num_parents = clk_hw_get_num_parents(hw);
	const char * const *parent_names = clk_priv->clk_init->parent_names;
	u8 i = 0, ret = clk_priv->clk_init->num_parents + 1;

	if ((clk_priv->clk_src == AUDIO_EXT_CLK_PMI) && clk_priv->clk_name) {
		for (i = 0; i < num_parents; i++) {
			if (!strcmp(parent_names[i], clk_priv->clk_name))
				ret = i;
		}
		pr_debug("%s: parent index = %u\n", __func__, ret);
		return ret;
	} else
		return 0;
}

static int lpass_hw_set_vote(struct audio_ext_clk_priv *clk_priv, bool enable)
{
#ifdef CONFIG_AUDIO_PRM
	int core;

	if (clk_priv->clk_src == AUDIO_EXT_CLK_LPASS_CORE_HW_VOTE)
		core = HW_CORE_ID_LPASS;
	else if (clk_priv->clk_src == AUDIO_EXT_CLK_LPASS_AUDIO_HW_VOTE)
		core = HW_CORE_ID_DCODEC;
	else
		return -EINVAL;
	return audio_prm_set_lpass_hw_core_req(&clk_priv->prm_clk_cfg, core, enable);
#else
	return -EPERM;
#endif
}

static int lpass_hw_vote_prepare(struct clk_hw *hw)
{
	struct audio_ext_clk_priv *clk_priv = to_audio_clk(hw);
	int ret;

	if (clk_priv->enable)
		return 0;
	ret = lpass_hw_set_vote(clk_priv, true);
	if (ret < 0) {
		pr_err_ratelimited("%s: core request failed: %d\n", __func__, ret);
		return ret;
	}
	clk_priv->enable = 1;
	return 0;
}

static void lpass_hw_vote_unprepare(struct clk_hw *hw)
{
	struct audio_ext_clk_priv *clk_priv = to_audio_clk(hw);
	int ret;

	if (!clk_priv->enable)
		return;
	ret = lpass_hw_set_vote(clk_priv, false);
	if (ret < 0)
		pr_err("%s: core release failed: %d\n", __func__, ret);
	else
		clk_priv->enable = 0;
}

static const struct clk_ops audio_ext_clk_ops = {
	.prepare = audio_ext_clk_prepare,
	.unprepare = audio_ext_clk_unprepare,
	.get_parent = audio_ext_clk_get_parent,
};

static const struct clk_ops lpass_hw_vote_ops = {
	.prepare = lpass_hw_vote_prepare,
	.unprepare = lpass_hw_vote_unprepare,
};

static const char * const audio_ext_pmi_div_clk[] = {
	"qpnp_clkdiv_1",
	"pms405_div_clk1",
	"pm6150_div_clk1",
	"pm6125_div_clk1",
};

static int audio_ext_clk_dummy_prepare(struct clk_hw *hw)
{
	return 0;
}

static void audio_ext_clk_dummy_unprepare(struct clk_hw *hw)
{

}

static const struct clk_ops audio_ext_clk_dummy_ops = {
	.prepare = audio_ext_clk_dummy_prepare,
	.unprepare = audio_ext_clk_dummy_unprepare,
};

static struct audio_ext_clk audio_clk_array[] = {
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_ext_pmi_clk",
				.parent_names = audio_ext_pmi_div_clk,
				.num_parents =
					 ARRAY_SIZE(audio_ext_pmi_div_clk),
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_ext_pmi_lnbb_clk",
				.parent_names = (const char *[])
							{ "ln_bb_clk2" },
				.num_parents = 1,
				.ops = &audio_ext_clk_dummy_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk2",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk3",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk4",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk5",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk6",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk7",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.hw.init = &(struct clk_init_data){
				.name = "lpass_hw_vote_clk",
				.ops = &lpass_hw_vote_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk8",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.hw.init = &(struct clk_init_data){
				.name = "lpass_audio_hw_vote_clk",
				.ops = &lpass_hw_vote_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk9",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk10",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk11",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk12",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk13",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk14",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk15",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_lpass_mclk16",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
	{
		.pnctrl_info = {NULL},
		.fact = {
			.mult = 1,
			.div = 1,
			.hw.init = &(struct clk_init_data){
				.name = "audio_external_pll_clk",
				.ops = &audio_ext_clk_ops,
			},
		},
	},
};

static int audio_get_pinctrl(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct audio_ext_clk_priv *clk_priv = platform_get_drvdata(pdev);
	struct pinctrl_info *pnctrl_info = &clk_priv->audio_clk.pnctrl_info;
	int ret;
	u32 reg;

	pnctrl_info->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pnctrl_info->pinctrl))
		return PTR_ERR(pnctrl_info->pinctrl);
	pnctrl_info->sleep = pinctrl_lookup_state(pnctrl_info->pinctrl, "sleep");
	if (IS_ERR(pnctrl_info->sleep))
		return PTR_ERR(pnctrl_info->sleep);
	pnctrl_info->active = pinctrl_lookup_state(pnctrl_info->pinctrl, "active");
	if (IS_ERR(pnctrl_info->active))
		return PTR_ERR(pnctrl_info->active);
	ret = pinctrl_select_state(pnctrl_info->pinctrl, pnctrl_info->sleep);
	if (ret)
		return ret;

	ret = of_property_read_u32(dev->of_node, "qcom,mclk-clk-reg", &reg);
	if (!ret) {
		pnctrl_info->base = devm_ioremap(dev, reg, sizeof(u32));
		if (!pnctrl_info->base)
			return -ENOMEM;
	}
	return 0;
}

static void audio_ref_clk_cleanup(void *data)
{
	struct audio_ext_clk_priv *clk_priv = data;
	struct clk_hw *hw = &clk_priv->audio_clk.fact.hw;

	/* Devres unregisters the provider and clock before releasing votes/pins. */
	if (clk_priv->clk_src == AUDIO_EXT_CLK_LPASS_CORE_HW_VOTE ||
	    clk_priv->clk_src == AUDIO_EXT_CLK_LPASS_AUDIO_HW_VOTE)
		lpass_hw_vote_unprepare(hw);
	else
		audio_ext_clk_unprepare(hw);
	if (clk_priv->enable || clk_priv->pins_active || clk_priv->gate_enabled)
		dev_err(clk_priv->dev, "external clock cleanup requires hardware recovery\n");
}

static int audio_get_clk_data(struct platform_device *pdev)
{
	struct audio_ext_clk_priv *clk_priv = platform_get_drvdata(pdev);
	struct clk_hw *hw = &clk_priv->audio_clk.fact.hw;
	struct clk_hw_onecell_data *clk_data;
	int ret;

	clk_data = devm_kzalloc(&pdev->dev, struct_size(clk_data, hws, 1), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = 1;
	clk_data->hws[0] = hw;

	ret = devm_clk_hw_register(&pdev->dev, hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get, clk_data);
}

static int audio_ref_clk_probe(struct platform_device *pdev)
{
	int ret;
	struct audio_ext_clk_priv *clk_priv;
	u32 clk_freq = 0, clk_id = 0, clk_src = 0, use_pinctrl = 0;

	clk_priv = devm_kzalloc(&pdev->dev, sizeof(*clk_priv), GFP_KERNEL);
	if (!clk_priv)
		return -ENOMEM;

	ret = of_property_read_u32(pdev->dev.of_node,
			"qcom,codec-ext-clk-src",
			&clk_src);
	if (ret) {
		dev_err(&pdev->dev, "%s: could not get clk source, ret = %d\n",
				__func__, ret);
		return ret;
	}

	if (clk_src >= AUDIO_EXT_CLK_MAX) {
		dev_err(&pdev->dev, "%s: Invalid clk source = %d\n",
				__func__, clk_src);
		return -EINVAL;
	}
	clk_priv->dev = &pdev->dev;
	clk_priv->clk_name = NULL;
	clk_priv->clk_src = clk_src;
	memcpy(&clk_priv->audio_clk, &audio_clk_array[clk_src],
		   sizeof(struct audio_ext_clk));
	/* The clock core clears hw.init after registration. */
	clk_priv->clk_init = clk_priv->audio_clk.fact.hw.init;

#ifdef CONFIG_AUDIO_PRM
	/* Init prm clk cfg default values */
	clk_priv->prm_clk_cfg.clk_id = CLOCK_ID_QUI_MI2S_OSR;
	clk_priv->prm_clk_cfg.clk_freq_in_hz = OSR_CLOCK_9_P600_MHZ;
	clk_priv->prm_clk_cfg.clk_attri = CLOCK_ATTRIBUTE_COUPLE_NO;
	clk_priv->prm_clk_cfg.clk_root = 0;
#endif

	ret = of_property_read_u32(pdev->dev.of_node,
			"qcom,codec-lpass-ext-clk-freq",
			&clk_freq);
	if (!ret) {
#ifdef CONFIG_AUDIO_PRM
		clk_priv->prm_clk_cfg.clk_freq_in_hz = clk_freq;
#endif
	}

	ret = of_property_read_u32(pdev->dev.of_node,
			"qcom,codec-lpass-clk-id",
			&clk_id);
	if (!ret) {
#ifdef CONFIG_AUDIO_PRM
		clk_priv->prm_clk_cfg.clk_id = clk_id;
		dev_dbg(&pdev->dev, "%s: PRM ext-clk freq: %d, lpass clk_id: %d, clk_src: %d\n",
			__func__, clk_priv->prm_clk_cfg.clk_freq_in_hz,
			clk_priv->prm_clk_cfg.clk_id, clk_priv->clk_src);
#endif
	}

        dev_dbg(&pdev->dev, "%s: PRM2 ext-clk freq: %d, lpass clk_id: %d, clk_src: %d\n",
                        __func__, clk_priv->prm_clk_cfg.clk_freq_in_hz,
                        clk_priv->prm_clk_cfg.clk_id, clk_priv->clk_src);

	platform_set_drvdata(pdev, clk_priv);

	ret = of_property_read_string(pdev->dev.of_node, "pmic-clock-names",
				      &clk_priv->clk_name);
	if (ret)
		dev_dbg(&pdev->dev, "%s: could not find pmic clock names\n",
			__func__);
	/*
	 * property qcom,use-pinctrl to be defined in DTSI to val 1
	 * for clock nodes using pinctrl
	 */
	of_property_read_u32(pdev->dev.of_node, "qcom,use-pinctrl",
			     &use_pinctrl);
	dev_dbg(&pdev->dev, "%s: use-pinctrl : %d\n",
		__func__, use_pinctrl);

	if (use_pinctrl) {
		ret = audio_get_pinctrl(pdev);
		if (ret) {
			dev_err(&pdev->dev, "%s: Parsing PMI pinctrl failed\n",
				__func__);
			return ret;
		}
	}

	ret = devm_add_action_or_reset(&pdev->dev, audio_ref_clk_cleanup, clk_priv);
	if (ret)
		return ret;

	ret = audio_get_clk_data(pdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: clk_init is failed\n",
			__func__);
		return ret;
	}
	return 0;
}

static const struct of_device_id audio_ref_clk_match[] = {
	{.compatible = "qcom,audio-ref-clk"},
	{}
};
MODULE_DEVICE_TABLE(of, audio_ref_clk_match);

static struct platform_driver audio_ref_clk_driver = {
	.driver = {
		.name = "audio-ref-clk",
		.owner = THIS_MODULE,
		.of_match_table = audio_ref_clk_match,
		.suppress_bind_attrs = true,
	},
	.probe = audio_ref_clk_probe,
};

int audio_ref_clk_platform_init(void)
{
	return platform_driver_register(&audio_ref_clk_driver);
}

void audio_ref_clk_platform_exit(void)
{
	platform_driver_unregister(&audio_ref_clk_driver);
}

MODULE_DESCRIPTION("Audio Ref Up Clock module platform driver");
MODULE_LICENSE("GPL v2");
