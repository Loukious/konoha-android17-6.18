// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/topology.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct qcom_cpufreq_thermal_domain {
	struct mbox_client cl;
	struct mbox_chan *ch;
	struct cpufreq_policy *policy;
	struct device *cpu_dev;
	bool sysfs_created;

	struct device_attribute freq_limit_attr;
	unsigned long freq_limit;
};

struct qcom_cpufreq_thermal {
	struct qcom_cpufreq_thermal_domain *domains;
	int num_domains;
};

static inline
struct qcom_cpufreq_thermal_domain *to_qcom_cpufreq_thermal_domain(struct mbox_client *cl)
{
	return container_of(cl, struct qcom_cpufreq_thermal_domain, cl);
}

static void qcom_cpufreq_thermal_rx(struct mbox_client *cl, void *msg)
{
	struct qcom_cpufreq_thermal_domain *domain = to_qcom_cpufreq_thermal_domain(cl);
	unsigned int cpu = cpumask_first(domain->policy->related_cpus);
	unsigned long throttled_freq = *((unsigned long *)msg);

	dev_dbg(cl->dev, "cpu%u thermal limit: %lu\n", cpu, throttled_freq);

	WRITE_ONCE(domain->freq_limit, throttled_freq);

	arch_update_hw_pressure(domain->policy->related_cpus, throttled_freq);
}

static ssize_t dcvsh_freq_limit_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct qcom_cpufreq_thermal_domain *domain;

	domain = container_of(attr, struct qcom_cpufreq_thermal_domain, freq_limit_attr);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(domain->freq_limit));
}

static void qcom_cpufreq_thermal_release(void *arg)
{
	struct qcom_cpufreq_thermal_domain *domain = arg;

	if (domain->sysfs_created)
		device_remove_file(domain->cpu_dev, &domain->freq_limit_attr);
	/* The receiver must stop before its policy reference is released. */
	if (!IS_ERR_OR_NULL(domain->ch))
		mbox_free_channel(domain->ch);
	arch_update_hw_pressure(domain->policy->related_cpus, U32_MAX);
	cpufreq_cpu_put(domain->policy);
}

static int qcom_cpufreq_thermal_driver_probe(struct platform_device *pdev)
{
	struct qcom_cpufreq_thermal *data;
	struct qcom_cpufreq_thermal_domain *domain;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret, i, j;
	u32 cpu;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num_domains = of_property_count_u32_elems(np, "qcom,policy-cpus");
	if (data->num_domains <= 0)
		return data->num_domains ?: -EINVAL;
	if (data->num_domains > nr_cpu_ids)
		return -EINVAL;

	data->domains = devm_kcalloc(dev, data->num_domains, sizeof(*domain), GFP_KERNEL);
	if (!data->domains)
		return -ENOMEM;

	for (i = 0; i < data->num_domains; i++) {
		domain = &data->domains[i];
		ret = of_property_read_u32_index(np, "qcom,policy-cpus", i, &cpu);
		if (ret)
			return ret;
		if (cpu >= nr_cpu_ids || !cpu_possible(cpu))
			return -EINVAL;
		domain->cpu_dev = get_cpu_device(cpu);
		if (!domain->cpu_dev)
			return -EPROBE_DEFER;
		domain->policy = cpufreq_cpu_get(cpu);
		if (!domain->policy)
			return -EPROBE_DEFER;
		ret = devm_add_action_or_reset(dev, qcom_cpufreq_thermal_release, domain);
		if (ret)
			return ret;
		for (j = 0; j < i; j++)
			if (data->domains[j].policy == domain->policy)
				return -EINVAL;

		domain->freq_limit = U32_MAX;
		domain->cl.dev = dev;
		domain->cl.rx_callback = qcom_cpufreq_thermal_rx;
		domain->ch = mbox_request_channel(&domain->cl, i);
		if (IS_ERR(domain->ch))
			return dev_err_probe(dev, PTR_ERR(domain->ch),
					     "Error getting mailbox %d\n", i);

		sysfs_attr_init(&domain->freq_limit_attr.attr);
		domain->freq_limit_attr.attr.name = "dcvsh_freq_limit";
		domain->freq_limit_attr.show = dcvsh_freq_limit_show;
		domain->freq_limit_attr.attr.mode = 0444;
		ret = device_create_file(domain->cpu_dev, &domain->freq_limit_attr);
		if (ret)
			return ret;
		domain->sysfs_created = true;
	}
	return 0;
}

static const struct of_device_id qcom_cpufreq_thermal_match[] = {
	{ .compatible = "qcom,cpufreq-thermal" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_cpufreq_thermal_match);

static struct platform_driver qcom_cpufreq_thermal_driver = {
	.probe = qcom_cpufreq_thermal_driver_probe,
	.driver = {
		.name = "qcom-cpufreq-thermal",
		.of_match_table = qcom_cpufreq_thermal_match,
	},
};

module_platform_driver(qcom_cpufreq_thermal_driver);

MODULE_DESCRIPTION("QCOM CPUFREQ Thermal Driver");
MODULE_LICENSE("GPL");
