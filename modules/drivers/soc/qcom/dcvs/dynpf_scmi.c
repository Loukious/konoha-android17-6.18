// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/scmi_protocol.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/completion.h>

#define DYNPF_ALGO_STR	0x53434d495f444750 /* "SCMI_DGP" */

static struct kobject dynpf_kobj;
static struct scmi_protocol_handle *ph;
static const struct qcom_scmi_vendor_ops *ops;
static struct scmi_device *sdev;
static DEFINE_MUTEX(dynpf_lock);
static DECLARE_COMPLETION(dynpf_released);
static bool dynpf_ready;

struct qcom_dynpf_attr {
	struct attribute		attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
			const char *buf, size_t count);
};

enum dynpf_param_ids {
	PARAM_ENABLE_DYNPF = 1,
};

#define to_dynpf_attr(_attr) \
	container_of(_attr, struct qcom_dynpf_attr, attr)
#define DYNPF_ATTR_RW(_name)						\
static struct qcom_dynpf_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)			\

static ssize_t store_enable_dynpf(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	u32 val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ret = ops->set_param(ph, &val, DYNPF_ALGO_STR, PARAM_ENABLE_DYNPF,
				sizeof(u32));

	return ((ret < 0) ? ret : count);
}

static ssize_t show_enable_dynpf(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	u32 val;
	int ret;

	ret = ops->get_param(ph, &val, DYNPF_ALGO_STR, PARAM_ENABLE_DYNPF, 0,
				sizeof(u32));
	if (ret < 0)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%u\n", le32_to_cpu(val));
}

DYNPF_ATTR_RW(enable_dynpf);

static struct attribute *dynpf_settings_attrs[] = {
	&enable_dynpf.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dynpf_settings);

static ssize_t attr_show(struct kobject *kobj, struct attribute *attr,
			 char *buf)
{
	struct qcom_dynpf_attr *entry = to_dynpf_attr(attr);
	ssize_t ret = -EIO;
	mutex_lock(&dynpf_lock);
	if (!dynpf_ready || !ops)
		ret = -ENODEV;
	else if (entry->show)
		ret = entry->show(kobj, attr, buf);
	mutex_unlock(&dynpf_lock);
	return ret;
}


static ssize_t attr_store(struct kobject *kobj, struct attribute *attr,
			 const char *buf, size_t count)
{
	struct qcom_dynpf_attr *entry = to_dynpf_attr(attr);
	ssize_t ret = -EIO;
	mutex_lock(&dynpf_lock);
	if (!dynpf_ready || !ops)
		ret = -ENODEV;
	else if (entry->store)
		ret = entry->store(kobj, attr, buf, count);
	mutex_unlock(&dynpf_lock);
	return ret;
}


static const struct sysfs_ops dynpf_sysfs_ops = {
	.show	= attr_show,
	.store	= attr_store,
};

static void dynpf_kobject_release(struct kobject *kobj)
{
	complete(&dynpf_released);
}

static const struct kobj_type dynpf_settings_ktype = {
	.release = dynpf_kobject_release,
	.sysfs_ops		= &dynpf_sysfs_ops,
	.default_groups		= dynpf_settings_groups,
};

static void dynpf_kobject_cleanup(void *arg)
{
	mutex_lock(&dynpf_lock);
	dynpf_ready = false;
	mutex_unlock(&dynpf_lock);
	kobject_del(&dynpf_kobj);
	kobject_put(&dynpf_kobj);
	wait_for_completion(&dynpf_released);
}

static void dynpf_protocol_cleanup(void *arg)
{
	mutex_lock(&dynpf_lock);
	sdev->handle->devm_protocol_put(sdev, QCOM_SCMI_VENDOR_PROTOCOL);
	put_device(&sdev->dev);
	sdev = NULL;
	ops = NULL;
	ph = NULL;
	mutex_unlock(&dynpf_lock);
}

static int qcom_dynpf_probe(struct platform_device *pdev)
{
	struct device *dev_root;
	int ret;

	mutex_lock(&dynpf_lock);
	if (ops) {
		ret = -EBUSY;
		goto unlock;
	}
	sdev = get_qcom_scmi_device();
	if (IS_ERR(sdev)) {
		ret = PTR_ERR(sdev);
		sdev = NULL;
		goto unlock;
	}
	if (!sdev || !sdev->handle) {
		ret = -ENODEV;
		goto unlock;
	}
	get_device(&sdev->dev);
	ops = sdev->handle->devm_protocol_get(sdev, QCOM_SCMI_VENDOR_PROTOCOL, &ph);
	if (IS_ERR(ops)) {
		ret = PTR_ERR(ops);
		ops = NULL;
		put_device(&sdev->dev);
		sdev = NULL;
		goto unlock;
	}
	ret = devm_add_action(&pdev->dev, dynpf_protocol_cleanup, NULL);
	if (ret) {
		sdev->handle->devm_protocol_put(sdev, QCOM_SCMI_VENDOR_PROTOCOL);
		put_device(&sdev->dev);
		sdev = NULL;
		ops = NULL;
		goto unlock;
	}
	dev_root = bus_get_dev_root(&cpu_subsys);
	if (!dev_root) {
		ret = -ENODEV;
		goto unlock;
	}
	reinit_completion(&dynpf_released);
	/* No callback can access protocol state before the owner is ready. */
	ret = kobject_init_and_add(&dynpf_kobj, &dynpf_settings_ktype,
				   &dev_root->kobj, "dynpf");
	put_device(dev_root);
	mutex_unlock(&dynpf_lock);
	if (ret) {
		kobject_put(&dynpf_kobj);
		wait_for_completion(&dynpf_released);
		return ret;
	}
	ret = devm_add_action_or_reset(&pdev->dev, dynpf_kobject_cleanup, NULL);
	if (ret)
		return ret;
	mutex_lock(&dynpf_lock);
	dynpf_ready = true;
	mutex_unlock(&dynpf_lock);
	return 0;
unlock:
	mutex_unlock(&dynpf_lock);
	return ret;
}


static const struct of_device_id dynpf_table[] = {
	{.compatible = "qcom,dynpf"},
	{},
};

static struct platform_driver dynpf_driver = {
	.driver = {
		.name = "qcom-dynpf",
		.of_match_table = dynpf_table,
	},
	.probe = qcom_dynpf_probe,
};

MODULE_DEVICE_TABLE(of, dynpf_table);
module_platform_driver(dynpf_driver);
MODULE_SOFTDEP("pre: qcom_scmi_client");
MODULE_DESCRIPTION("QCOM DYNPF SCMI driver");
MODULE_LICENSE("GPL");
