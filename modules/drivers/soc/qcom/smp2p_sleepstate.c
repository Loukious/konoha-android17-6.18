// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2022,2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/pm_wakeup.h>

/* The remote sleep-state protocol uses bit 12, independently of DT bit 0. */
#define AWAKE_BIT BIT(12)

struct smp2p_sleepstate {
	struct qcom_smem_state *state;
	struct wakeup_source *notify_ws;
	struct notifier_block pm_nb;
};

static int sleepstate_pm_notifier(struct notifier_block *nb,
				 unsigned long event, void *unused)
{
	struct smp2p_sleepstate *sleep = container_of(nb, struct smp2p_sleepstate, pm_nb);
	int ret;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		ret = qcom_smem_state_update_bits(sleep->state, AWAKE_BIT, 0);
		return notifier_from_errno(ret);
	case PM_POST_SUSPEND:
		qcom_smem_state_update_bits(sleep->state, AWAKE_BIT, AWAKE_BIT);
		break;
	}
	return NOTIFY_DONE;
}

static irqreturn_t smp2p_sleepstate_handler(int irq, void *data)
{
	struct smp2p_sleepstate *sleep = data;

	__pm_wakeup_event(sleep->notify_ws, 200);
	return IRQ_HANDLED;
}

static void smp2p_sleepstate_unregister_notifier(void *data)
{
	struct smp2p_sleepstate *sleep = data;

	unregister_pm_notifier(&sleep->pm_nb);
}

static void smp2p_sleepstate_unregister_wakeup(void *data)
{
	struct smp2p_sleepstate *sleep = data;

	wakeup_source_unregister(sleep->notify_ws);
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct smp2p_sleepstate *sleep;
	unsigned int bit;
	int irq, ret;

	sleep = devm_kzalloc(dev, sizeof(*sleep), GFP_KERNEL);
	if (!sleep)
		return -ENOMEM;
	sleep->state = devm_qcom_smem_state_get(dev, NULL, &bit);
	if (IS_ERR(sleep->state))
		return PTR_ERR(sleep->state);

	irq = platform_get_irq_byname(pdev, "smp2p-sleepstate-in");
	if (irq < 0)
		return irq;
	sleep->notify_ws = wakeup_source_register(dev, "smp2p-sleepstate");
	if (!sleep->notify_ws)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, smp2p_sleepstate_unregister_wakeup, sleep);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, irq, NULL, smp2p_sleepstate_handler,
					IRQF_ONESHOT | IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
					"smp2p_sleepstate", sleep);
	if (ret)
		return ret;
	ret = qcom_smem_state_update_bits(sleep->state, AWAKE_BIT, AWAKE_BIT);
	if (ret)
		return ret;
	sleep->pm_nb.notifier_call = sleepstate_pm_notifier;
	sleep->pm_nb.priority = INT_MAX;
	ret = register_pm_notifier(&sleep->pm_nb);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, smp2p_sleepstate_unregister_notifier, sleep);
}

static const struct of_device_id smp2p_slst_match_table[] = {
	{ .compatible = "qcom,smp2p-sleepstate" },
	{},
};
MODULE_DEVICE_TABLE(of, smp2p_slst_match_table);

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.driver = {
		.name = "smp2p_sleepstate",
		.of_match_table = smp2p_slst_match_table,
	},
};
module_platform_driver(smp2p_sleepstate_driver);

MODULE_DESCRIPTION("SMP2P SLEEP STATE");
MODULE_LICENSE("GPL");
