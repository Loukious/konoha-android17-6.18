// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "cpufreq_hw_debug: %s: " fmt, __func__

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

#define MAX_FREQ_DOMAINS 16

enum debug_hw_regs_data {
	REG_PERF_STATE,
	REG_CYCLE_CNTR,
	REG_PSTATE_STATUS,
	REG_EPSS_DEBUG_STATUS,
	REG_EPSS_DEBUG_SRB,
	REG_EPSS_DEBUG_LUT,
	REG_ARRAY_SIZE,
};

struct cpufreq_debug_config {
	const u16 *offsets;
	unsigned int reg_count;
};

struct cpufreq_hwregs {
	struct kobject kobj;
	struct notifier_block panic_nb;
	const struct cpufreq_debug_config *config;
	unsigned int domain_cnt;
	void __iomem *base[MAX_FREQ_DOMAINS];
};

static const u16 cpufreq_qcom_std_data[] = {
	[REG_PERF_STATE] = 0x920,
	[REG_CYCLE_CNTR] = 0x9c0,
	[REG_PSTATE_STATUS] = 0x700,
};

static const u16 cpufreq_qcom_std_epss_data[] = {
	[REG_PERF_STATE] = 0x320,
	[REG_CYCLE_CNTR] = 0x3c4,
	[REG_PSTATE_STATUS] = 0x020,
	[REG_EPSS_DEBUG_STATUS] = 0x01c,
	[REG_EPSS_DEBUG_SRB] = 0x0bc,
	[REG_EPSS_DEBUG_LUT] = 0x100,
};

static const struct cpufreq_debug_config std_config = {
	.offsets = cpufreq_qcom_std_data,
	.reg_count = ARRAY_SIZE(cpufreq_qcom_std_data),
};

static const struct cpufreq_debug_config epss_config = {
	.offsets = cpufreq_qcom_std_epss_data,
	.reg_count = ARRAY_SIZE(cpufreq_qcom_std_epss_data),
};

static const char * const register_names[] = {
	"PERF_STATE_DESIRED", "CYCLE_CNTR_VAL", "PSTATE_STATUS",
	"EPSS_DEBUG_STATUS", "EPSS_DEBUG_SRB", "EPSS_DEBUG_LUT",
};

static ssize_t cpufreq_hwregs_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct cpufreq_hwregs *regs = container_of(kobj, struct cpufreq_hwregs, kobj);
	unsigned int i, j;
	ssize_t count = 0;
	u32 value;

	for (i = 0; i < regs->domain_cnt && count < PAGE_SIZE - 1; i++) {
		count += sysfs_emit_at(buf, count, "FREQUENCY DOMAIN %u\n", i);
		for (j = 0; j < regs->config->reg_count && count < PAGE_SIZE - 1; j++) {
			value = readl_relaxed(regs->base[i] + regs->config->offsets[j]);
			count += sysfs_emit_at(buf, count, "%25s: 0x%.8x\n",
					      register_names[j], value);
		}
	}
	return count;
}

static int cpufreq_panic_callback(struct notifier_block *nfb,
				  unsigned long event, void *unused)
{
	struct cpufreq_hwregs *regs = container_of(nfb, struct cpufreq_hwregs, panic_nb);
	unsigned int i, j;
	u32 value;

	for (i = 0; i < regs->domain_cnt; i++) {
		pr_err("FREQUENCY DOMAIN %u\n", i);
		for (j = 0; j < regs->config->reg_count; j++) {
			value = readl_relaxed(regs->base[i] + regs->config->offsets[j]);
			pr_err("%25s: 0x%.8x\n", register_names[j], value);
		}
	}
	return NOTIFY_OK;
}

static struct kobj_attribute cpufreq_hwregs_attr =
	__ATTR(print_cpufreq_debug_regs, 0444, cpufreq_hwregs_show, NULL);

static void cpufreq_hwregs_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct cpufreq_hwregs, kobj));
}

static const struct kobj_type cpufreq_hwregs_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.release = cpufreq_hwregs_release,
};

static int cpufreq_get_hwregs(struct platform_device *pdev, struct cpufreq_hwregs *regs)
{
	struct of_phandle_args args;
	struct resource res;
	const __be32 *prop;
	unsigned int i, j;
	int length, ret;

	prop = of_get_property(pdev->dev.of_node, "qcom,freq-hw-domain", &length);
	if (!prop || length <= 0 || length % (2 * sizeof(*prop)))
		return -EINVAL;
	regs->domain_cnt = length / (2 * sizeof(*prop));
	if (regs->domain_cnt > MAX_FREQ_DOMAINS)
		return -E2BIG;
	for (i = 0; i < regs->domain_cnt; i++) {
		ret = of_parse_phandle_with_fixed_args(pdev->dev.of_node,
					"qcom,freq-hw-domain", 1, i, &args);
		if (ret)
			return ret;
		ret = of_address_to_resource(args.np, args.args[0], &res);
		of_node_put(args.np);
		if (ret)
			return ret;
		if (resource_type(&res) != IORESOURCE_MEM || res.end < res.start)
			return -EINVAL;
		for (j = 0; j < regs->config->reg_count; j++)
			if (resource_size(&res) < regs->config->offsets[j] + sizeof(u32))
				return -EINVAL;
		/* The cpufreq provider owns these registers; diagnostics only reads. */
		regs->base[i] = devm_ioremap(&pdev->dev, res.start, resource_size(&res));
		if (!regs->base[i])
			return -ENOMEM;
	}
	return 0;
}

static int qcom_cpufreq_hw_debug_probe(struct platform_device *pdev)
{
	struct cpufreq_hwregs *regs;
	const struct cpufreq_debug_config *config;
	int ret;

	config = of_device_get_match_data(&pdev->dev);
	if (!config)
		return -EINVAL;
	regs = kzalloc(sizeof(*regs), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	regs->config = config;
	ret = cpufreq_get_hwregs(pdev, regs);
	if (ret) {
		kfree(regs);
		return ret;
	}
	/* kobject's release owns the allocation from this point onward. */
	ret = kobject_init_and_add(&regs->kobj, &cpufreq_hwregs_ktype,
				  kernel_kobj, "qcom-cpufreq-hw");
	if (ret)
		goto put_kobj;
	regs->panic_nb.notifier_call = cpufreq_panic_callback;
	regs->panic_nb.priority = 1;
	ret = atomic_notifier_chain_register(&panic_notifier_list, &regs->panic_nb);
	if (ret)
		goto del_kobj;
	ret = sysfs_create_file(&regs->kobj, &cpufreq_hwregs_attr.attr);
	if (ret) {
		atomic_notifier_chain_unregister(&panic_notifier_list, &regs->panic_nb);
		goto del_kobj;
	}
	platform_set_drvdata(pdev, regs);
	return 0;
del_kobj:
	kobject_del(&regs->kobj);
put_kobj:
	kobject_put(&regs->kobj);
	return ret;
}

static void qcom_cpufreq_hw_debug_remove(struct platform_device *pdev)
{
	struct cpufreq_hwregs *regs = platform_get_drvdata(pdev);

	/* Drain both readers before devres releases their MMIO mappings. */
	atomic_notifier_chain_unregister(&panic_notifier_list, &regs->panic_nb);
	sysfs_remove_file(&regs->kobj, &cpufreq_hwregs_attr.attr);
	kobject_del(&regs->kobj);
	platform_set_drvdata(pdev, NULL);
	kobject_put(&regs->kobj);
}

static const struct of_device_id qcom_cpufreq_hw_debug_match[] = {
	{ .compatible = "qcom,cpufreq-hw-debug", .data = &std_config },
	{ .compatible = "qcom,cpufreq-hw-epss-debug", .data = &epss_config },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_cpufreq_hw_debug_match);

static struct platform_driver qcom_cpufreq_hw_debug = {
	.probe = qcom_cpufreq_hw_debug_probe,
	.remove = qcom_cpufreq_hw_debug_remove,
	.driver = {
		.name = "qcom-cpufreq-hw-debug",
		.of_match_table = qcom_cpufreq_hw_debug_match,
	},
};

module_platform_driver(qcom_cpufreq_hw_debug);
MODULE_DESCRIPTION("QTI clock driver for CPUFREQ HW debug");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: qcom_cpufreq_hw");
