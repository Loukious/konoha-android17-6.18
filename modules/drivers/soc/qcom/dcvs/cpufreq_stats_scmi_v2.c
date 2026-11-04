// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/scmi_protocol.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include "cpufreq_stats_common.h"

/* Preserve the pinned native numeric identifier and response layout. */
#define SCMI_CPUFREQ_STATS_STR (5045524653544154)
#define PARAM_PROTOCOL_ATTRIBUTE 2
struct cpufreq_stats_prot_attr {
	__le32 attributes;
	__le32 statistics_address_low;
	__le32 statistics_address_high;
	__le32 statistics_len;
};

static void cpufreq_stats_protocol_put(void *arg)
{
	struct scmi_device *sdev = arg;
	sdev->handle->devm_protocol_put(sdev, QCOM_SCMI_VENDOR_PROTOCOL);
	put_device(&sdev->dev);
}

static int scmi_cpufreq_stats_probe(struct platform_device *pdev)
{
	struct cpufreq_stats_prot_attr attributes = { };
	const struct qcom_scmi_vendor_ops *ops;
	struct scmi_protocol_handle *ph;
	struct scmi_device *sdev;
	int ret;

	sdev = get_qcom_scmi_device();
	if (IS_ERR(sdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(sdev), "SCMI device unavailable\n");
	if (!sdev || !sdev->handle)
		return -ENODEV;
	get_device(&sdev->dev);
	ops = sdev->handle->devm_protocol_get(sdev, QCOM_SCMI_VENDOR_PROTOCOL, &ph);
	if (IS_ERR(ops)) {
		put_device(&sdev->dev);
		return PTR_ERR(ops);
	}
	ret = devm_add_action_or_reset(&pdev->dev, cpufreq_stats_protocol_put, sdev);
	if (ret)
		return ret;
	ret = ops->get_param(ph, &attributes, SCMI_CPUFREQ_STATS_STR,
			    PARAM_PROTOCOL_ATTRIBUTE, 0, sizeof(attributes));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "PERF statistics metadata unavailable\n");
	return cpufreq_stats_map(&pdev->dev,
		le32_to_cpu(attributes.statistics_address_low) |
		(u64)le32_to_cpu(attributes.statistics_address_high) << 32,
		le32_to_cpu(attributes.statistics_len));
}

static const struct of_device_id cpufreq_stats_v2[] = {
	{ .compatible = "qcom,cpufreq-stats-v2" },
	{ }
};
MODULE_DEVICE_TABLE(of, cpufreq_stats_v2);
static struct platform_driver cpufreq_stats_v2_driver = {
	.driver = {
		.name = "cpufreq-stats-v2",
		.of_match_table = cpufreq_stats_v2,
		.suppress_bind_attrs = true,
	},
	.probe = scmi_cpufreq_stats_probe,
};
module_platform_driver(cpufreq_stats_v2_driver);
MODULE_SOFTDEP("pre: qcom_scmi_client");
MODULE_DESCRIPTION("Qcom SCMI CPUFREQ STATS V2 driver");
MODULE_LICENSE("GPL");
