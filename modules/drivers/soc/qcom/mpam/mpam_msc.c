// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "mpam_msc: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/scmi_protocol.h>
#include <soc/qcom/mpam_msc.h>
#include <soc/qcom/mpam_slc.h>

#include <linux/mutex.h>

static LIST_HEAD(qcom_mpam_list);
static DEFINE_MUTEX(qcom_mpam_lock);

/* The registry lock remains held until the provider operation completes. */
static struct qcom_mpam_msc *qcom_msc_lookup(u32 msc_id)
{
	struct qcom_mpam_msc *msc;
	list_for_each_entry(msc, &qcom_mpam_list, node)
		if (msc->msc_id == msc_id)
			return msc;
	return NULL;
}

int qcom_msc_get_query(u32 msc_id, struct msc_query *query)
{
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;
	if (!query)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (msc && (msc->mpam_available & MPAM_AVAILABLE)) {
		query->qcom_msc_id = msc->qcom_msc_id;
		ret = 0;
	}
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_msc_get_query);

int msc_system_get_partition(u32 msc_id, void *arg1, void *arg2)
{
	struct msc_query *query = arg1;
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;

	if (!arg1 || !arg2)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (!msc || !(msc->mpam_available & MPAM_AVAILABLE))
		goto out;
	if (memcmp(&msc->qcom_msc_id, &query->qcom_msc_id, sizeof(query->qcom_msc_id))) {
		ret = -EINVAL;
		goto out;
	}
	ret = -EOPNOTSUPP;
	if (msc->qcom_msc_id.qcom_msc_type == SLC && msc->ops && msc->ops->get_cache_partition) {
		ret = msc->ops->get_cache_partition(msc->dev, arg1, arg2);
	}
out:
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(msc_system_get_partition);

int msc_system_get_device_capability(u32 msc_id, void *arg1, void *arg2)
{
	struct msc_query *query = arg1;
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;

	if (!arg1 || !arg2)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (!msc || !(msc->mpam_available & MPAM_AVAILABLE))
		goto out;
	if (memcmp(&msc->qcom_msc_id, &query->qcom_msc_id, sizeof(query->qcom_msc_id))) {
		ret = -EINVAL;
		goto out;
	}
	ret = -EOPNOTSUPP;
	if (msc->qcom_msc_id.qcom_msc_type == SLC && msc->ops && msc->ops->get_cache_partition_capability) {
		ret = msc->ops->get_cache_partition_capability(msc->dev, arg1, arg2);
	}
out:
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(msc_system_get_device_capability);

int msc_system_set_partition(u32 msc_id, void *arg1, void *arg2)
{
	struct msc_query *query = arg1;
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;

	if (!arg1 || !arg2)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (!msc || !(msc->mpam_available & MPAM_AVAILABLE))
		goto out;
	if (memcmp(&msc->qcom_msc_id, &query->qcom_msc_id, sizeof(query->qcom_msc_id))) {
		ret = -EINVAL;
		goto out;
	}
	ret = -EOPNOTSUPP;
	if (msc->qcom_msc_id.qcom_msc_type == SLC && msc->ops && msc->ops->set_cache_partition) {
		ret = msc->ops->set_cache_partition(msc->dev, arg1, arg2);
	}
out:
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(msc_system_set_partition);

int msc_system_mon_config(u32 msc_id, void *arg1, void *arg2)
{
	struct msc_query *query = arg1;
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;

	if (!arg1 || !arg2)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (!msc || !(msc->mpam_available & MPAM_AVAILABLE))
		goto out;
	if (memcmp(&msc->qcom_msc_id, &query->qcom_msc_id, sizeof(query->qcom_msc_id))) {
		ret = -EINVAL;
		goto out;
	}
	ret = -EOPNOTSUPP;
	if (msc->qcom_msc_id.qcom_msc_type == SLC && msc->ops && msc->ops->mon_config) {
		ret = msc->ops->mon_config(msc->dev, arg1, arg2);
	}
out:
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(msc_system_mon_config);

int msc_system_reset_partition(u32 msc_id, void *arg1, void *arg2)
{
	struct msc_query *query = arg1;
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;

	if (!arg1 || !arg2)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (!msc || !(msc->mpam_available & MPAM_AVAILABLE))
		goto out;
	if (memcmp(&msc->qcom_msc_id, &query->qcom_msc_id, sizeof(query->qcom_msc_id))) {
		ret = -EINVAL;
		goto out;
	}
	ret = -EOPNOTSUPP;
	if (msc->qcom_msc_id.qcom_msc_type == SLC && msc->ops && msc->ops->reset_cache_partition) {
		ret = msc->ops->reset_cache_partition(msc->dev, arg1, arg2);
	}
out:
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(msc_system_reset_partition);

int msc_system_mon_alloc_info(u32 msc_id, void *arg1, void *arg2)
{
	struct msc_query *query = arg1;
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;

	if (!arg1 || !arg2)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (!msc || !(msc->mpam_available & MPAM_AVAILABLE))
		goto out;
	if (memcmp(&msc->qcom_msc_id, &query->qcom_msc_id, sizeof(query->qcom_msc_id))) {
		ret = -EINVAL;
		goto out;
	}
	ret = -EOPNOTSUPP;
	if (msc->qcom_msc_id.qcom_msc_type == SLC && msc->ops && msc->ops->mon_stats_read) {
		((union mon_values *)arg2)->ref.slc_mon_function = CACHE_CAPACITY_CONFIG;
		ret = msc->ops->mon_stats_read(msc->dev, arg1, arg2);
	}
out:
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(msc_system_mon_alloc_info);

int msc_system_mon_read_miss_info(u32 msc_id, void *arg1, void *arg2)
{
	struct msc_query *query = arg1;
	struct qcom_mpam_msc *msc;
	int ret = -ENODEV;

	if (!arg1 || !arg2)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	msc = qcom_msc_lookup(msc_id);
	if (!msc || !(msc->mpam_available & MPAM_AVAILABLE))
		goto out;
	if (memcmp(&msc->qcom_msc_id, &query->qcom_msc_id, sizeof(query->qcom_msc_id))) {
		ret = -EINVAL;
		goto out;
	}
	ret = -EOPNOTSUPP;
	if (msc->qcom_msc_id.qcom_msc_type == SLC && msc->ops && msc->ops->mon_stats_read) {
		((union mon_values *)arg2)->ref.slc_mon_function = CACHE_READ_MISS_CONFIG;
		ret = msc->ops->mon_stats_read(msc->dev, arg1, arg2);
	}
out:
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(msc_system_mon_read_miss_info);

int attach_mpam_msc(struct device *dev, struct qcom_mpam_msc *msc, u32 type)
{
	int ret = 0;
	if (!msc || type != SLC || !msc->ops)
		return -EINVAL;
	mutex_lock(&qcom_mpam_lock);
	if (qcom_msc_lookup(msc->msc_id))
		ret = -EEXIST;
	else
		list_add_tail(&msc->node, &qcom_mpam_list);
	mutex_unlock(&qcom_mpam_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(attach_mpam_msc);

void detach_mpam_msc(struct device *dev, struct qcom_mpam_msc *msc, u32 type)
{
	if (!msc || type != SLC)
		return;
	mutex_lock(&qcom_mpam_lock);
	list_del_init(&msc->node);
	mutex_unlock(&qcom_mpam_lock);
}
EXPORT_SYMBOL_GPL(detach_mpam_msc);

static int mpam_msc_probe(struct platform_device *pdev)
{
	return devm_of_platform_populate(&pdev->dev);
}


static const struct of_device_id mpam_msc_table[] = {
	{ .compatible = "qcom,mpam-msc" },
	{}
};

MODULE_DEVICE_TABLE(of, mpam_msc_table);

static struct platform_driver mpam_msc_driver = {
	.driver = {
		.name = "mpam-msc",
		.of_match_table = mpam_msc_table,
		.suppress_bind_attrs = true,
	},
	.probe = mpam_msc_probe,
};

module_platform_driver(mpam_msc_driver);

MODULE_DESCRIPTION("QCOM MPAM MSC driver");
MODULE_LICENSE("GPL");
