// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "qcom-lpm.h"

static struct kobject *qcom_lpm_kobj;

static ssize_t cluster_idle_set(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t len)
{
	struct qcom_cluster_node *d = container_of(attr, struct qcom_cluster_node, disable_attr);
	bool disable;
	int ret;

	ret = kstrtobool(buf, &disable);
	if (ret)
		return -EINVAL;

	WRITE_ONCE(d->cluster->state_allowed[d->state_idx], !disable);

	return len;
}

static ssize_t cluster_idle_get(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	struct qcom_cluster_node *d = container_of(attr, struct qcom_cluster_node, disable_attr);

	return scnprintf(buf, PAGE_SIZE, "%d\n", !READ_ONCE(d->cluster->state_allowed[d->state_idx]));
}

static int create_cluster_state_node(struct device *dev, struct qcom_cluster_node *d)
{
	struct kobj_attribute *attr = &d->disable_attr;
	int ret;

	d->attr_group = devm_kzalloc(dev, sizeof(struct attribute_group), GFP_KERNEL);
	if (!d->attr_group)
		return -ENOMEM;

	d->attrs = devm_kcalloc(dev, 2, sizeof(struct attribute *), GFP_KERNEL);
	if (!d->attrs)
		return -ENOMEM;

	sysfs_attr_init(&attr->attr);
	attr->attr.name = "disable";
	attr->attr.mode = 0644;
	attr->show = cluster_idle_get;
	attr->store = cluster_idle_set;

	d->attrs[0] = &attr->attr;
	d->attrs[1] = NULL;
	d->attr_group->attrs = d->attrs;

	ret = sysfs_create_group(d->kobj, d->attr_group);
	return ret;
}

void remove_cluster_sysfs_nodes(struct lpm_cluster *cluster)
{
	int i;

	for (i = 0; i < cluster->genpd->state_count; i++) {
		struct qcom_cluster_node *d = cluster->dev_node[i];

		if (!d)
			continue;
		sysfs_remove_group(d->kobj, d->attr_group);
		kobject_put(d->kobj);
		cluster->dev_node[i] = NULL;
	}
	kobject_put(cluster->dev_kobj);
	cluster->dev_kobj = NULL;
}

int create_cluster_sysfs_nodes(struct lpm_cluster *cluster)
{
	char name[10];
	int i, ret = -ENOMEM;
	struct generic_pm_domain *genpd = cluster->genpd;

	if (!qcom_lpm_kobj)
		return -EPROBE_DEFER;
	cluster->dev_kobj = kobject_create_and_add(genpd->name, qcom_lpm_kobj);
	if (!cluster->dev_kobj)
		return -ENOMEM;

	for (i = 0; i < genpd->state_count; i++) {
		struct qcom_cluster_node *d;

		d = devm_kzalloc(cluster->dev, sizeof(*d), GFP_KERNEL);
		if (!d)
			goto fail;
		d->state_idx = i;
		d->cluster = cluster;
		scnprintf(name, sizeof(name), "D%u", i);
		d->kobj = kobject_create_and_add(name, cluster->dev_kobj);
		if (!d->kobj)
			goto fail;
		ret = create_cluster_state_node(cluster->dev, d);
		if (ret) {
			kobject_put(d->kobj);
			goto fail;
		}
		cluster->dev_node[i] = d;
		ret = -ENOMEM;
	}
	return 0;
fail:
	remove_cluster_sysfs_nodes(cluster);
	return ret;
}

static ssize_t optimized_resi_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", optimized_resi);
}

static ssize_t optimized_resi_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	optimized_resi = val;

	return count;
}

static ssize_t premature_resi_div_cpu_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", premature_resi_div_cpu);
}

static ssize_t premature_resi_div_cpu_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	if (val < num_possible_cpus())
		premature_resi_div_cpu = val;
	else
		premature_resi_div_cpu = U32_MAX;

	return count;
}

static ssize_t pred_active_time_show(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", pred_active_time);
}

static ssize_t pred_active_time_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	pred_active_time = val;

	return count;
}

static ssize_t resi_fact_show(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", resi_fact);
}

static ssize_t resi_fact_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	if (val == 0)
		resi_fact = 1;
	else
		resi_fact = val;

	return count;
}

static ssize_t pred_timer_add_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", pred_timer_add);
}

static ssize_t pred_timer_add_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	if (val > MAX_PRED_TIMER_ADD) {
		pr_err("pred_timer_add must be less than %dusec\n", MAX_PRED_TIMER_ADD);
		return -EINVAL;
	}

	pred_timer_add = val;

	return count;
}

static ssize_t pred_premature_cnt_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", pred_premature_cnt);
}

static ssize_t pred_premature_cnt_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	if (val == 0 || val > MAXSAMPLES) {
		pr_err("pred_premature_cnt must be between 1 and %d\n", MAXSAMPLES);
		return -EINVAL;
	}

	pred_premature_cnt = val;

	return count;
}

static ssize_t ipi_pred_ref_stddev_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", ipi_pred_ref_stddev);
}

static ssize_t ipi_pred_ref_stddev_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	ipi_pred_ref_stddev = val;

	return count;
}
static ssize_t pred_ref_stddev_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", pred_ref_stddev);
}

static ssize_t pred_ref_stddev_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	pred_ref_stddev = val;

	return count;
}

static ssize_t premature_ext_disabled_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", premature_ext_disabled);
}

static ssize_t premature_ext_disabled_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	premature_ext_disabled = val;

	return count;
}

static ssize_t cluster_bias_disabled_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", cluster_bias_disabled);
}

static ssize_t cluster_bias_disabled_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	cluster_bias_disabled = val;

	return count;
}

static ssize_t bias_disabled_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", bias_disabled);
}

static ssize_t bias_disabled_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret < 0) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	bias_disabled = val;

	return count;
}

static ssize_t sleep_disabled_show(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", sleep_disabled);
}

static ssize_t sleep_disabled_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	sleep_disabled = val;

	return count;
}

static ssize_t prediction_disabled_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", prediction_disabled);
}

static ssize_t prediction_disabled_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret) {
		pr_err("Invalid argument passed\n");
		return ret;
	}

	prediction_disabled = val;

	return count;
}

static struct kobj_attribute attr_optimized_resi = __ATTR_RW(optimized_resi);
static struct kobj_attribute attr_premature_resi_div_cpu = __ATTR_RW(premature_resi_div_cpu);
static struct kobj_attribute attr_resi_fact = __ATTR_RW(resi_fact);
static struct kobj_attribute attr_pred_active_time = __ATTR_RW(pred_active_time);
static struct kobj_attribute attr_pred_timer_add = __ATTR_RW(pred_timer_add);
static struct kobj_attribute attr_pred_premature_cnt = __ATTR_RW(pred_premature_cnt);
static struct kobj_attribute attr_ipi_pred_ref_stddev = __ATTR_RW(ipi_pred_ref_stddev);
static struct kobj_attribute attr_pred_ref_stddev = __ATTR_RW(pred_ref_stddev);
static struct kobj_attribute attr_premature_ext_disabled = __ATTR_RW(premature_ext_disabled);
static struct kobj_attribute attr_cluster_bias_disabled = __ATTR_RW(cluster_bias_disabled);
static struct kobj_attribute attr_bias_disabled = __ATTR_RW(bias_disabled);
static struct kobj_attribute attr_sleep_disabled = __ATTR_RW(sleep_disabled);
static struct kobj_attribute attr_prediction_disabled = __ATTR_RW(prediction_disabled);

static struct attribute *lpm_gov_attrs[] = {
	&attr_optimized_resi.attr,
	&attr_premature_resi_div_cpu.attr,
	&attr_resi_fact.attr,
	&attr_pred_active_time.attr,
	&attr_pred_timer_add.attr,
	&attr_pred_premature_cnt.attr,
	&attr_ipi_pred_ref_stddev.attr,
	&attr_pred_ref_stddev.attr,
	&attr_premature_ext_disabled.attr,
	&attr_cluster_bias_disabled.attr,
	&attr_bias_disabled.attr,
	&attr_sleep_disabled.attr,
	&attr_prediction_disabled.attr,
	NULL
};

static struct attribute_group lpm_gov_attr_group = {
	.attrs = lpm_gov_attrs,
	.name = "parameters",
};

void remove_global_sysfs_nodes(void)
{
	if (qcom_lpm_kobj)
		sysfs_remove_group(qcom_lpm_kobj, &lpm_gov_attr_group);
	kobject_put(qcom_lpm_kobj);
	qcom_lpm_kobj = NULL;
}

int create_global_sysfs_nodes(void)
{
	int ret;
	struct kobject *cpuidle_kobj;
	struct device *dev_root = bus_get_dev_root(&cpu_subsys);

	if (!dev_root)
		return -EINVAL;

	cpuidle_kobj = &dev_root->kobj;

	qcom_lpm_kobj = kobject_create_and_add(KBUILD_MODNAME, cpuidle_kobj);
	put_device(dev_root);
	if (!qcom_lpm_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(qcom_lpm_kobj, &lpm_gov_attr_group);
	if (ret) {
		kobject_put(qcom_lpm_kobj);
		qcom_lpm_kobj = NULL;
	}
	return ret;
}
