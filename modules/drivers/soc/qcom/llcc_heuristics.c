// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/llcc-qcom.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/scmi_protocol.h>
#include <linux/qcom_scmi_vendor.h>
#include <soc/qcom/llcc_heuristics.h>
#include <linux/of.h>

struct heuristics_context {
	struct scmi_protocol_handle *ph;
	const struct qcom_scmi_vendor_ops *ops;
	bool v2;
	bool enabled;
	union {
		struct scid_heuristics_params v1;
		struct scid_heuristics_params_v2 v2;
	} params;
};

static int heuristics_send(struct heuristics_context *ctx, u32 command)
{
	return ctx->ops->set_param(ctx->ph, &ctx->params, SCID_HEURISTICS_SCMI_STR,
		command, ctx->v2 ? sizeof(ctx->params.v2) : sizeof(ctx->params.v1));
}

static int heuristics_scid_probe(struct platform_device *pdev)
{
	struct heuristics_context *ctx;
	struct scmi_device *scmi;
	struct device_node *node = pdev->dev.of_node;
	u32 *freq, *residency, scid;
	int count, max, ret;
	bool enabled;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	scmi = get_qcom_scmi_device();
	if (IS_ERR(scmi))
		return PTR_ERR(scmi);
	ctx->ops = scmi->handle->devm_protocol_get(scmi, QCOM_SCMI_VENDOR_PROTOCOL, &ctx->ph);
	if (IS_ERR(ctx->ops))
		return PTR_ERR(ctx->ops);
	ctx->v2 = !pdev->dev.parent ||
		of_property_match_string(pdev->dev.parent->of_node, "compatible", "qcom,sun-llcc") < 0;
	ret = of_property_read_u32(node, "qcom,heuristics-scid", &scid);
	if (ret)
		return ret;
	count = of_property_count_u32_elems(node, "qcom,freq-threshold-idx");
	enabled = of_property_read_bool(node, "qcom,scid-heuristics-enabled");
	if (ctx->v2) {
		max = ARRAY_SIZE(ctx->params.v2.freq_idx);
		freq = ctx->params.v2.freq_idx;
		residency = ctx->params.v2.freq_idx_residency;
		ctx->params.v2.heuristics_scid = scid;
		ctx->params.v2.scid_heuristics_enabled = enabled;
	} else {
		max = ARRAY_SIZE(ctx->params.v1.freq_idx);
		freq = ctx->params.v1.freq_idx;
		residency = ctx->params.v1.freq_idx_residency;
		ctx->params.v1.heuristics_scid = scid;
		ctx->params.v1.scid_heuristics_enabled = enabled;
	}
	if (enabled) {
		if (count <= 0 || count > max ||
		    of_property_count_u32_elems(node, "qcom,frequency-threshold-residency") != count)
			return -EINVAL;
		ret = of_property_read_u32_array(node, "qcom,freq-threshold-idx", freq, count);
		if (ret)
			return ret;
		ret = of_property_read_u32_array(node, "qcom,frequency-threshold-residency", residency, count);
		if (ret)
			return ret;
	}
	ret = heuristics_send(ctx, enabled ? HEURISTICS_INIT : SCID_ACTIVATION_CONTROL);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "LLCC heuristics firmware request failed\n");
	ctx->enabled = enabled;
	platform_set_drvdata(pdev, ctx);
	return 0;
}

static void heuristics_scid_remove(struct platform_device *pdev)
{
	struct heuristics_context *ctx = platform_get_drvdata(pdev);
	if (!ctx->enabled)
		return;
	if (ctx->v2)
		ctx->params.v2.scid_heuristics_enabled = 0;
	else
		ctx->params.v1.scid_heuristics_enabled = 0;
	if (heuristics_send(ctx, SCID_ACTIVATION_CONTROL))
		dev_err(&pdev->dev, "Failed to stop LLCC heuristics firmware thread\n");
}

static const struct of_device_id heuristics_scid_table[] = {
	{ .compatible = "qcom,scid-heuristics" },
	{}
};

MODULE_DEVICE_TABLE(of, heuristics_scid_table);

static struct platform_driver heuristics_scid_driver = {
	.driver = {
		.name = "scid-heuristics",
		.of_match_table = heuristics_scid_table,
	},
	.probe = heuristics_scid_probe,
	.remove = heuristics_scid_remove,
};

module_platform_driver(heuristics_scid_driver);

MODULE_SOFTDEP("pre: llcc_qcom");
MODULE_SOFTDEP("pre: qcom_scmi_client");
MODULE_DESCRIPTION("QCOM HEURISTICS SCID driver");
MODULE_LICENSE("GPL");
