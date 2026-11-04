// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved. */
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched/walt.h>

#define FAST_MBOX_CPUMASK 0xFF
struct qcom_cpucp_fast {
	u8 last_cpu;
	cpumask_t fast_cpus;
	struct mbox_client cl;
	struct mbox_chan *ch;
	bool ready;
};

static void qcom_cpucp_fast_rx(struct mbox_client *client, void *message)
{
	struct qcom_cpucp_fast *data = container_of(client, struct qcom_cpucp_fast, cl);
	u64 payload;
	u32 cpu;

	if (!READ_ONCE(data->ready) || !message)
		return;
	memcpy(&payload, message, sizeof(payload));
	cpu = payload & FAST_MBOX_CPUMASK;
	/* Native 0xff is a stop hint; never use it as a cpumask index. */
	if (cpu == FAST_MBOX_CPUMASK) {
		if (data->last_cpu < nr_cpu_ids) {
			data->last_cpu = FAST_MBOX_CPUMASK;
			sched_walt_oscillate(FAST_MBOX_CPUMASK);
		}
		return;
	}
	if (cpu >= nr_cpu_ids || !cpu_possible(cpu) ||
	    !cpumask_test_cpu(cpu, &data->fast_cpus))
		return;
	data->last_cpu = cpu;
	sched_walt_oscillate(cpu);
}

static void qcom_cpucp_fast_stop(void *arg)
{
	struct qcom_cpucp_fast *data = arg;
	WRITE_ONCE(data->ready, false);
	if (!IS_ERR_OR_NULL(data->ch))
		mbox_free_channel(data->ch);
	data->ch = NULL;
}

static int qcom_cpucp_fast_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_cpucp_fast *data;
	struct cpufreq_policy *policy;
	u32 cpu;
	int ret, related;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	ret = of_property_read_u32(dev->of_node, "qcom,policy-cpus", &cpu);
	if (ret)
		return ret;
	if (cpu >= nr_cpu_ids || !cpu_present(cpu) || cpu >= FAST_MBOX_CPUMASK)
		return -EINVAL;
	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -EPROBE_DEFER;
	cpumask_copy(&data->fast_cpus, policy->related_cpus);
	cpufreq_cpu_put(policy);
	if (cpumask_empty(&data->fast_cpus) || !cpumask_test_cpu(cpu, &data->fast_cpus) ||
	    !cpumask_subset(&data->fast_cpus, cpu_possible_mask))
		return -EINVAL;
	for_each_cpu(related, &data->fast_cpus)
		if (related >= FAST_MBOX_CPUMASK)
			return -ERANGE;
	data->last_cpu = FAST_MBOX_CPUMASK;
	data->cl.dev = dev;
	data->cl.rx_callback = qcom_cpucp_fast_rx;
	dev_set_drvdata(dev, data);
	/* The mailbox controller can deliver RX during channel request. */
	WRITE_ONCE(data->ready, true);
	data->ch = mbox_request_channel(&data->cl, 0);
	if (IS_ERR(data->ch)) {
		WRITE_ONCE(data->ready, false);
		return dev_err_probe(dev, PTR_ERR(data->ch), "FAST mailbox unavailable\n");
	}
	return devm_add_action_or_reset(dev, qcom_cpucp_fast_stop, data);
}

static void qcom_cpucp_fast_shutdown(struct platform_device *pdev)
{
	qcom_cpucp_fast_stop(dev_get_drvdata(&pdev->dev));
}
static const struct of_device_id qcom_cpucp_fast_match[] = {
	{ .compatible = "qcom,cpucp_fast" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_cpucp_fast_match);
static struct platform_driver qcom_cpucp_fast_driver = {
	.probe = qcom_cpucp_fast_probe,
	.shutdown = qcom_cpucp_fast_shutdown,
	.driver = {
		.name = "qcom-cpucp-fast",
		.of_match_table = qcom_cpucp_fast_match,
	},
};
module_platform_driver(qcom_cpucp_fast_driver);
MODULE_SOFTDEP("pre: qcom_cpucp sched-walt");
MODULE_DESCRIPTION("QCOM CPUCP FAST Driver");
MODULE_LICENSE("GPL");
