// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define NAME_LENGTH 0x12
#define REGS_MAX 3

struct regs_config {
	void __iomem *base;
	u32 offset;
	char name[NAME_LENGTH];
	u32 residency_lo_hi_distance;
};

struct stats_config {
	struct regs_config count_regs[REGS_MAX];
	struct regs_config residency_regs[REGS_MAX];
};

struct qcom_stats_prvdata {
	struct platform_device *pdev;
	void __iomem *base;
	struct stats_config *offset;
	struct dentry *rootdir;
};

static int qcom_stats_count_show(struct seq_file *s, void *d)
{
	struct regs_config *reg = s->private;
	u32 val;

	val = readl_relaxed(reg->base + reg->offset);
	seq_printf(s, "%u\n", val);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qcom_stats_count);

static int qcom_stats_residency_show(struct seq_file *s, void *d)
{
	struct regs_config *reg = s->private;
	u32 high, low, verify;
	unsigned int attempt;

	for (attempt = 0; attempt < 128; attempt++) {
		high = readl(reg->base + reg->offset + reg->residency_lo_hi_distance);
		low = readl(reg->base + reg->offset);
		verify = readl(reg->base + reg->offset + reg->residency_lo_hi_distance);
		if (high == verify) {
			seq_printf(s, "%llu\n", (u64)high << 32 | low);
			return 0;
		}
	}
	return -EAGAIN;
}

DEFINE_SHOW_ATTRIBUTE(qcom_stats_residency);

static int qcom_create_count_file(struct qcom_stats_prvdata *pdata)
{
	struct dentry *file;
	unsigned int i;
	for (i = 0; i < REGS_MAX; i++) {
		pdata->offset->count_regs[i].base = pdata->base;
		file = debugfs_create_file(pdata->offset->count_regs[i].name, 0400, pdata->rootdir,
					  &pdata->offset->count_regs[i], &qcom_stats_count_fops);
		if (IS_ERR_OR_NULL(file))
			return file ? PTR_ERR(file) : -ENOMEM;
	}
	return 0;
}


static int qcom_create_resindency_file(struct qcom_stats_prvdata *pdata)
{
	struct dentry *file;
	unsigned int i;
	for (i = 0; i < REGS_MAX; i++) {
		pdata->offset->residency_regs[i].base = pdata->base;
		file = debugfs_create_file(pdata->offset->residency_regs[i].name, 0400, pdata->rootdir,
					  &pdata->offset->residency_regs[i], &qcom_stats_residency_fops);
		if (IS_ERR_OR_NULL(file))
			return file ? PTR_ERR(file) : -ENOMEM;
	}
	return 0;
}


static int qcom_cpuss_v4_validate_config(const struct stats_config *config, u64 size)
{
	unsigned int i;
	for (i = 0; i < REGS_MAX; i++) {
		if ((u64)config->count_regs[i].offset + sizeof(u32) > size ||
		    (u64)config->residency_regs[i].offset + sizeof(u32) > size ||
		    (u64)config->residency_regs[i].offset +
			config->residency_regs[i].residency_lo_hi_distance + sizeof(u32) > size)
			return -EINVAL;
	}
	return 0;
}

static void qcom_cpuss_v4_cleanup(void *arg)
{
	struct qcom_stats_prvdata *pdata = arg;
	debugfs_remove_recursive(pdata->rootdir);
	pdata->rootdir = NULL;
}

static int qcom_cpuss_sleep_stats_v4_probe(struct platform_device *pdev)
{
	struct qcom_stats_prvdata *pdata;
	const struct stats_config *configuration;
	struct resource *resource;
	int ret;

	configuration = of_device_get_match_data(&pdev->dev);
	if (!configuration)
		return -ENODEV;
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource)
		return -ENODEV;
	ret = qcom_cpuss_v4_validate_config(configuration, resource_size(resource));
	if (ret)
		return ret;
	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;
	pdata->offset = devm_kmemdup(&pdev->dev, configuration, sizeof(*configuration), GFP_KERNEL);
	if (!pdata->offset)
		return -ENOMEM;
	pdata->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(pdata->base))
		return PTR_ERR(pdata->base);
	pdata->pdev = pdev;
	pdata->rootdir = debugfs_create_dir("qcom_cpuss_sleep_stats", NULL);
	if (IS_ERR_OR_NULL(pdata->rootdir))
		return pdata->rootdir ? PTR_ERR(pdata->rootdir) : -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, qcom_cpuss_v4_cleanup, pdata);
	if (ret)
		return ret;
	ret = qcom_create_count_file(pdata);
	if (ret)
		return ret;
	ret = qcom_create_resindency_file(pdata);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, pdata);
	return 0;
}


static void qcom_cpuss_sleep_stats_remove(struct platform_device *pdev)
{
	qcom_cpuss_v4_cleanup(platform_get_drvdata(pdev));
}


static const struct stats_config qcom_cpuss_cntr_offsets = {
	.count_regs = {
		{
			.offset = 0x1220,
			.name = "CL0_CL5_Count",
		},
		{
			.offset = 0x1224,
			.name = "CL1_CL5_Count",
		},
		{
			.offset = 0x1018,
			.name = "SS3_Count",
		},
	},
	.residency_regs = {
		{
			.offset = 0x1320,
			.name = "CL0_CL5_Residency",
			.residency_lo_hi_distance = 0x10,
		},
		{
			.offset = 0x1324,
			.name = "CL1_CL5_Residency",
			.residency_lo_hi_distance = 0x10,
		},
		{
			.offset = 0x1118,
			.name = "SS3_Residency",
			.residency_lo_hi_distance = 0x4,
		},
	},
};

static const struct of_device_id qcom_cpuss_stats_v4_table[] = {
	{ .compatible = "qcom,cpuss-sleep-stats-v4", .data = &qcom_cpuss_cntr_offsets },
	{ },
};

static struct platform_driver qcom_cpuss_sleep_stats_v4 = {
	.probe = qcom_cpuss_sleep_stats_v4_probe,
	.remove = qcom_cpuss_sleep_stats_remove,
	.driver	= {
		.name = "qcom_cpuss_sleep_stats_v4",
		.of_match_table	= qcom_cpuss_stats_v4_table,
	},
};

MODULE_DEVICE_TABLE(of, qcom_cpuss_stats_v4_table);
module_platform_driver(qcom_cpuss_sleep_stats_v4);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. (QTI) CPUSS sleep stats v4 driver");
MODULE_LICENSE("GPL");
