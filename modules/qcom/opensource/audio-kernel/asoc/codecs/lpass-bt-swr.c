// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/limits.h>
#include <soc/swr-common.h>
#include <asoc/msm-cdc-pinctrl.h>
#include <dsp/digital-cdc-rsc-mgr.h>
#include <soc/swr-wcd.h>
#include <soc/snd_event.h>

#define DRV_NAME "lpass-bt-swr"

/* pm runtime auto suspend timer in msecs */
#define LPASS_BT_SWR_AUTO_SUSPEND_DELAY          100 /* delay in msec */

#define LPASS_BT_SWR_STRING_LEN 80

#define LPASS_BT_SWR_CHILD_DEVICES_MAX 1

struct lpass_bt_swr_ctrl_platform_data {
	void *handle; /* holds parent private data */
	int (*read)(void *handle, int reg);
	int (*write)(void *handle, int reg, int val);
	int (*bulk_write)(void *handle, u32 *reg, u32 *val, size_t len);
	int (*clk)(void *handle, bool enable);
	int (*core_vote)(void *handle, bool enable);
	int (*handle_irq)(void *handle,
			  irqreturn_t (*swrm_irq_handler)(int irq,
							  void *data),
			  void *swrm_handle,
			  int action);
};

struct lpass_bt_swr_priv {
	struct device *dev;
	struct mutex vote_lock;
	struct mutex swr_clk_lock;
	struct mutex ssr_lock;
	bool dev_up;
	bool initial_boot;

	struct clk *lpass_core_hw_vote;
	struct clk *lpass_audio_hw_vote;
	int core_hw_vote_count;
	int core_audio_vote_count;
	int swr_clk_users;
	struct clk *clk_handle;
	struct clk *clk_handle_2x;

	struct platform_device *swr_pdev;
	bool mclk_enabled;
	bool snd_registered;
	struct lpass_bt_swr_ctrl_platform_data swr_plat_data;

	struct device_node *bt_swr_gpio_p;

	/* Entry for version info */
	struct snd_info_entry *entry;
	struct snd_info_entry *version_entry;

	struct blocking_notifier_head notifier;
	struct device *clk_dev;
};

static int lpass_bt_swr_add_child(struct lpass_bt_swr_priv *priv)
{
	struct device_node *node, *selected = NULL;
	struct platform_device *pdev;
	int ret;

	for_each_available_child_of_node(priv->dev->of_node, node) {
		if (!strnstr(node->name, "bt_swr_mstr", strlen("bt_swr_mstr")))
			continue;
		if (selected) {
			of_node_put(node);
			of_node_put(selected);
			return -EINVAL;
		}
		selected = of_node_get(node);
	}
	if (!selected)
		return -ENODEV;
	pdev = platform_device_alloc("bt_swr_mstr", -1);
	if (!pdev) {
		of_node_put(selected);
		return -ENOMEM;
	}
	pdev->dev.parent = priv->dev;
	pdev->dev.of_node = selected;
	ret = platform_device_add_data(pdev, &priv->swr_plat_data,
				       sizeof(priv->swr_plat_data));
	if (ret)
		goto put_device;
	ret = platform_device_add(pdev);
	if (ret)
		goto put_device;
	priv->swr_pdev = pdev;
	return 0;

put_device:
	platform_device_put(pdev);
	return ret;
}

static bool lpass_bt_swr_check_core_votes(struct lpass_bt_swr_priv *priv)
{
	bool ret = true;

	mutex_lock(&priv->vote_lock);
	if (!READ_ONCE(priv->dev_up) ||
	    (priv->lpass_core_hw_vote && !priv->core_hw_vote_count) ||
	    (priv->lpass_audio_hw_vote && !priv->core_audio_vote_count))
		ret = false;
	mutex_unlock(&priv->vote_lock);

	return ret;
}

static int lpass_bt_swr_core_vote(void *handle, bool enable)
{
	struct lpass_bt_swr_priv *priv = handle;
	int ret;

	if (!priv)
		return -EINVAL;
	if (enable) {
		if (!READ_ONCE(priv->dev_up))
			return -ENOTSYNC;
		ret = pm_runtime_resume_and_get(priv->dev);
		if (ret < 0)
			return ret;
		if (lpass_bt_swr_check_core_votes(priv))
			return 0;
		pm_runtime_mark_last_busy(priv->dev);
		pm_runtime_put_autosuspend(priv->dev);
		return -ENOTSYNC;
	}
	pm_runtime_mark_last_busy(priv->dev);
	pm_runtime_put_autosuspend(priv->dev);
	return 0;
}

static int lpass_bt_swr_mclk_enable(struct lpass_bt_swr_priv *priv, bool enable)
{
	int ret;

	if (priv->mclk_enabled == enable)
		return 0;
	if (enable) {
		ret = clk_prepare_enable(priv->clk_handle);
		if (ret)
			return ret;
		ret = clk_prepare_enable(priv->clk_handle_2x);
		if (ret) {
			clk_disable_unprepare(priv->clk_handle);
			return ret;
		}
	} else {
		clk_disable_unprepare(priv->clk_handle_2x);
		clk_disable_unprepare(priv->clk_handle);
	}
	priv->mclk_enabled = enable;
	return 0;
}

static int lpass_bt_swrm_clock(void *handle, bool enable)
{
	struct lpass_bt_swr_priv *priv = handle;
	int ret = 0;

	if (!priv)
		return -EINVAL;
	mutex_lock(&priv->swr_clk_lock);
	if (enable) {
		if (!READ_ONCE(priv->dev_up)) {
			ret = -ENOTSYNC;
			goto unlock;
		}
		if (priv->swr_clk_users == INT_MAX) {
			ret = -EOVERFLOW;
			goto unlock;
		}
		ret = pm_runtime_resume_and_get(priv->dev);
		if (ret < 0)
			goto unlock;
		if (!priv->swr_clk_users) {
			ret = msm_cdc_pinctrl_select_active_state(priv->bt_swr_gpio_p);
			if (ret)
				goto put_pm;
			ret = lpass_bt_swr_mclk_enable(priv, true);
			if (ret) {
				msm_cdc_pinctrl_select_sleep_state(priv->bt_swr_gpio_p);
				goto put_pm;
			}
		}
		priv->swr_clk_users++;
put_pm:
		pm_runtime_mark_last_busy(priv->dev);
		pm_runtime_put_autosuspend(priv->dev);
	} else {
		if (!priv->swr_clk_users) {
			ret = -EINVAL;
			goto unlock;
		}
		if (!--priv->swr_clk_users) {
			lpass_bt_swr_mclk_enable(priv, false);
			ret = msm_cdc_pinctrl_select_sleep_state(priv->bt_swr_gpio_p);
		}
	}
unlock:
	mutex_unlock(&priv->swr_clk_lock);
	return ret;
}

static void lpass_bt_swr_ssr_disable(struct device *dev, void *data)
{
	struct lpass_bt_swr_priv *priv = data;

	mutex_lock(&priv->ssr_lock);
	if (READ_ONCE(priv->dev_up)) {
		WRITE_ONCE(priv->dev_up, false);
		priv->initial_boot = false;
		if (priv->swr_pdev)
			swrm_wcd_notify(priv->swr_pdev, SWR_DEVICE_SSR_DOWN, NULL);
	}
	mutex_unlock(&priv->ssr_lock);
}

static int lpass_bt_swr_ssr_enable(struct device *dev, void *data)
{
	struct lpass_bt_swr_priv *priv = data;
	int ret = 0;

	mutex_lock(&priv->ssr_lock);
	if (priv->initial_boot) {
		priv->initial_boot = false;
		goto unlock;
	}
	mutex_lock(&priv->swr_clk_lock);
	if (priv->swr_clk_users) {
		lpass_bt_swr_mclk_enable(priv, false);
		ret = msm_cdc_pinctrl_select_sleep_state(priv->bt_swr_gpio_p);
		if (!ret)
			ret = msm_cdc_pinctrl_select_active_state(priv->bt_swr_gpio_p);
		if (!ret)
			ret = lpass_bt_swr_mclk_enable(priv, true);
		if (ret)
			msm_cdc_pinctrl_select_sleep_state(priv->bt_swr_gpio_p);
	}
	mutex_unlock(&priv->swr_clk_lock);
	if (ret)
		goto unlock;
	WRITE_ONCE(priv->dev_up, true);
	if (priv->swr_pdev)
		ret = swrm_wcd_notify(priv->swr_pdev, SWR_DEVICE_SSR_UP, NULL);
	if (ret)
		WRITE_ONCE(priv->dev_up, false);
unlock:
	mutex_unlock(&priv->ssr_lock);
	return ret;
}

static const struct snd_event_ops lpass_bt_swr_ssr_ops = {
	.enable = lpass_bt_swr_ssr_enable,
	.disable = lpass_bt_swr_ssr_disable,
};

static void lpass_bt_swr_put_gpio_node(void *data)
{
	of_node_put(data);
}

static void lpass_bt_swr_release_votes(struct lpass_bt_swr_priv *priv)
{
	if (priv->core_audio_vote_count)
		digital_cdc_rsc_mgr_hw_vote_disable(priv->lpass_audio_hw_vote, priv->dev);
	if (priv->core_hw_vote_count)
		digital_cdc_rsc_mgr_hw_vote_disable(priv->lpass_core_hw_vote, priv->dev);
	priv->core_audio_vote_count = 0;
	priv->core_hw_vote_count = 0;
}

static void lpass_bt_swr_cleanup(struct lpass_bt_swr_priv *priv)
{
	/* Deregistration synchronously withdraws and drains SSR callbacks. */
	if (priv->snd_registered) {
		snd_event_client_deregister(priv->dev);
		priv->snd_registered = false;
	}
	/* The controller drains callbacks while parent clocks/PM remain valid. */
	if (priv->swr_pdev) {
		platform_device_unregister(priv->swr_pdev);
		priv->swr_pdev = NULL;
	}
	pm_runtime_disable(priv->dev);
	mutex_lock(&priv->swr_clk_lock);
	lpass_bt_swr_mclk_enable(priv, false);
	if (priv->swr_clk_users)
		msm_cdc_pinctrl_select_sleep_state(priv->bt_swr_gpio_p);
	priv->swr_clk_users = 0;
	mutex_unlock(&priv->swr_clk_lock);
	mutex_lock(&priv->vote_lock);
	lpass_bt_swr_release_votes(priv);
	mutex_unlock(&priv->vote_lock);
	pm_runtime_set_suspended(priv->dev);
	dev_set_drvdata(priv->dev, NULL);
}

static int lpass_bt_swr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lpass_bt_swr_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->dev_up = true;
	priv->initial_boot = true;
	mutex_init(&priv->vote_lock);
	mutex_init(&priv->swr_clk_lock);
	mutex_init(&priv->ssr_lock);
	priv->bt_swr_gpio_p = of_parse_phandle(dev->of_node, "qcom,bt-swr-gpios", 0);
	if (!priv->bt_swr_gpio_p)
		return -EINVAL;
	ret = devm_add_action_or_reset(dev, lpass_bt_swr_put_gpio_node,
				       priv->bt_swr_gpio_p);
	if (ret)
		return ret;
	if (msm_cdc_pinctrl_get_state(priv->bt_swr_gpio_p) < 0)
		return -EPROBE_DEFER;

	priv->lpass_core_hw_vote = devm_clk_get_optional(dev, "lpass_core_hw_vote");
	if (IS_ERR(priv->lpass_core_hw_vote))
		return PTR_ERR(priv->lpass_core_hw_vote);
	priv->lpass_audio_hw_vote = devm_clk_get_optional(dev, "lpass_audio_hw_vote");
	if (IS_ERR(priv->lpass_audio_hw_vote))
		return PTR_ERR(priv->lpass_audio_hw_vote);
	priv->clk_handle = devm_clk_get(dev, "bt_swr_mclk_clk");
	if (IS_ERR(priv->clk_handle))
		return PTR_ERR(priv->clk_handle);
	priv->clk_handle_2x = devm_clk_get_optional(dev, "bt_swr_mclk_clk_2x");
	if (IS_ERR(priv->clk_handle_2x))
		return PTR_ERR(priv->clk_handle_2x);

	priv->swr_plat_data.handle = priv;
	priv->swr_plat_data.clk = lpass_bt_swrm_clock;
	priv->swr_plat_data.core_vote = lpass_bt_swr_core_vote;
	dev_set_drvdata(dev, priv);
	pm_runtime_set_autosuspend_delay(dev, LPASS_BT_SWR_AUTO_SUSPEND_DELAY);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_suspended(dev);
	pm_suspend_ignore_children(dev, true);
	pm_runtime_enable(dev);
	ret = lpass_bt_swr_add_child(priv);
	if (ret)
		goto cleanup;
	ret = snd_event_client_register(dev, &lpass_bt_swr_ssr_ops, priv);
	if (ret)
		goto cleanup;
	priv->snd_registered = true;
	ret = snd_event_notify(dev, SND_EVENT_UP);
	if (ret)
		goto cleanup;
	return 0;

cleanup:
	lpass_bt_swr_cleanup(priv);
	return ret;
}

static void lpass_bt_swr_remove(struct platform_device *pdev)
{
	struct lpass_bt_swr_priv *priv = platform_get_drvdata(pdev);

	if (priv)
		lpass_bt_swr_cleanup(priv);
}

#ifdef CONFIG_PM
static int lpass_bt_swr_runtime_resume(struct device *dev)
{
	struct lpass_bt_swr_priv *priv = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&priv->vote_lock);
	if (!READ_ONCE(priv->dev_up)) {
		ret = -ENOTSYNC;
		goto unlock;
	}
	if (priv->lpass_core_hw_vote && !priv->core_hw_vote_count) {
		ret = digital_cdc_rsc_mgr_hw_vote_enable(priv->lpass_core_hw_vote, dev);
		if (ret)
			goto unlock;
		priv->core_hw_vote_count = 1;
	}
	if (priv->lpass_audio_hw_vote && !priv->core_audio_vote_count) {
		ret = digital_cdc_rsc_mgr_hw_vote_enable(priv->lpass_audio_hw_vote, dev);
		if (ret) {
			lpass_bt_swr_release_votes(priv);
			goto unlock;
		}
		priv->core_audio_vote_count = 1;
	}
unlock:
	mutex_unlock(&priv->vote_lock);
	return ret;
}

static int lpass_bt_swr_runtime_suspend(struct device *dev)
{
	struct lpass_bt_swr_priv *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->vote_lock);
	lpass_bt_swr_release_votes(priv);
	mutex_unlock(&priv->vote_lock);
	return 0;
}
#endif /* CONFIG_PM */

static const struct of_device_id lpass_bt_swr_dt_match[] = {
	{.compatible = "qcom,lpass-bt-swr"},
	{}
};
MODULE_DEVICE_TABLE(of, lpass_bt_swr_dt_match);

static const struct dev_pm_ops lpass_bt_swr_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(
		pm_runtime_force_suspend,
		pm_runtime_force_resume
	)
	SET_RUNTIME_PM_OPS(
		lpass_bt_swr_runtime_suspend,
		lpass_bt_swr_runtime_resume,
		NULL
	)
};

static struct platform_driver lpass_bt_swr_drv = {
	.driver = {
		.name = "lpass-bt-swr",
		.pm = &lpass_bt_swr_pm_ops,
		.of_match_table = lpass_bt_swr_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = lpass_bt_swr_probe,
	.remove = lpass_bt_swr_remove,
};

static int lpass_bt_swr_drv_init(void)
{
	return platform_driver_register(&lpass_bt_swr_drv);
}

static void lpass_bt_swr_drv_exit(void)
{
	platform_driver_unregister(&lpass_bt_swr_drv);
}

static int __init lpass_bt_swr_init(void)
{
	return lpass_bt_swr_drv_init();
}
module_init(lpass_bt_swr_init);

static void __exit lpass_bt_swr_exit(void)
{
	lpass_bt_swr_drv_exit();
}
module_exit(lpass_bt_swr_exit);

MODULE_SOFTDEP("pre: bt_fm_swr");
MODULE_DESCRIPTION("LPASS BT SWR driver");
MODULE_LICENSE("GPL");
