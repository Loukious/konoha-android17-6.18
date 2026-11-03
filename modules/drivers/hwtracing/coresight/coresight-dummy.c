// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/coresight.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "coresight-priv.h"
#include "coresight-trace-id.h"
#include <linux/mutex.h>
#include <linux/property.h>
#include "coresight-qmi.h"
struct dummy_drvdata {
	struct device			*dev;
	struct coresight_device		*csdev;
	u8				traceid;
	bool				static_atid;
	bool				enabled;
	bool				removing;
	bool				id_owned;
	unsigned int			sink_users;
	struct mutex			mutex;
};

DEFINE_CORESIGHT_DEVLIST(source_devs, "dummy_source");
DEFINE_CORESIGHT_DEVLIST(sink_devs, "dummy_sink");

/*
 * Dummy source could be connected to a QMI device, which can send commmand
 * to subsystem via QMI. This is represented by the Output port of the dummy
 * source connected to the input port of the QMI.
 *
 * Returns	: coresight_device ptr for the QMI device if a QMI is found.
 *		: NULL otherwise.
 */
static struct coresight_device *dummy_source_get_qmi_device(struct dummy_drvdata *drvdata)
{
	int i;
	struct coresight_device *tmp, *dummy = drvdata->csdev;

	if (!IS_ENABLED(CONFIG_CORESIGHT_QMI))
		return NULL;

	for (i = 0; i < dummy->pdata->nr_outconns; i++) {
		tmp = dummy->pdata->out_conns[i]->dest_dev;
		if (!tmp && fwnode_property_match_string(dummy->pdata->out_conns[i]->dest_fwnode,
				"compatible", "qcom,coresight-qmi") >= 0)
			return ERR_PTR(-EPROBE_DEFER);
		if (tmp && coresight_is_qmi_device(tmp))
			return tmp;
	}

	return NULL;
}

/* qmi_assign_dummy_source_atid: assign atid to subsystem via qmi
 * device. if there is no qmi helper device connection, retunr 0
 * and exit.
 *
 * Returns : 0 on success
 */

static int qmi_assign_dummy_source_atid(struct dummy_drvdata *drvdata)
{
	struct coresight_device *qmi = dummy_source_get_qmi_device(drvdata);
	struct coresight_atid_assign_req_msg_v01 req = {};
	const char *name;
	int ret;

	if (IS_ERR(qmi))
		return PTR_ERR(qmi);
	/* Native dummy endpoints may have no firmware-control helper. */
	if (!qmi)
		return 0;
	ret = device_property_read_string(drvdata->dev, "trace-name", &name);
	if (ret)
		return ret;
	ret = strscpy(req.name, name, sizeof(req.name));
	if (ret < 0)
		return ret;
	req.atids[0] = drvdata->traceid;
	req.num_atids = 1;
	return coresight_qmi_assign_atid(qmi, &req);
}

static int dummy_source_enable(struct coresight_device *csdev,
		struct perf_event *event, enum cs_mode mode, struct coresight_path *path)
{
	struct dummy_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	int ret = 0;

	/* Native subsystem sources are controlled by the sysfs trace path. */
	if (mode != CS_MODE_SYSFS)
		return -EINVAL;

	mutex_lock(&d->mutex);
	if (d->removing) {
		ret = -ENODEV;
		goto out;
	}
	if (d->enabled || !coresight_take_mode(csdev, mode)) {
		ret = -EBUSY;
		goto out;
	}
	if (!d->static_atid)
		ret = qmi_assign_dummy_source_atid(d);
	if (ret)
		coresight_set_mode(csdev, CS_MODE_DISABLED);
	else
		d->enabled = true;
out:
	mutex_unlock(&d->mutex);
	return ret;
}

static void dummy_source_disable(struct coresight_device *csdev, struct perf_event *event)
{
	struct dummy_drvdata *d = dev_get_drvdata(csdev->dev.parent);

	mutex_lock(&d->mutex);
	d->enabled = false;
	coresight_set_mode(csdev, CS_MODE_DISABLED);
	mutex_unlock(&d->mutex);
}

static int dummy_sink_enable(struct coresight_device *csdev, enum cs_mode mode, void *data)
{
	struct dummy_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	int ret = 0;

	mutex_lock(&d->mutex);
	if (d->removing)
		ret = -ENODEV;
	else if (d->sink_users == UINT_MAX)
		ret = -EOVERFLOW;
	else if (d->sink_users ? coresight_get_mode(csdev) != mode : !coresight_take_mode(csdev, mode))
		ret = -EBUSY;
	else
		d->sink_users++;
	mutex_unlock(&d->mutex);
	return ret;
}

static int dummy_sink_disable(struct coresight_device *csdev)
{
	struct dummy_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	int ret = 0;

	mutex_lock(&d->mutex);
	if (!d->sink_users)
		ret = -EINVAL;
	else if (!--d->sink_users)
		coresight_set_mode(csdev, CS_MODE_DISABLED);
	mutex_unlock(&d->mutex);
	return ret;
}

static const struct coresight_ops_source dummy_source_ops = {
	.enable	= dummy_source_enable,
	.disable = dummy_source_disable,
};

static int dummy_trace_id(struct coresight_device *csdev, enum cs_mode mode,
		struct coresight_device *sink)
{
	struct dummy_drvdata *d = dev_get_drvdata(csdev->dev.parent);

	return d->traceid;
}

static const struct coresight_ops dummy_source_cs_ops = {
	.source_ops = &dummy_source_ops,
	.trace_id = dummy_trace_id,
};

static const struct coresight_ops_sink dummy_sink_ops = {
	.enable	= dummy_sink_enable,
	.disable = dummy_sink_disable,
};

static const struct coresight_ops dummy_sink_cs_ops = {
	.sink_ops = &dummy_sink_ops,
};

static ssize_t traceid_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	unsigned long val;
	struct dummy_drvdata *drvdata = dev_get_drvdata(dev->parent);

	val = drvdata->traceid;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}
static DEVICE_ATTR_RO(traceid);

static struct attribute *dummy_source_attrs[] = {
	&dev_attr_traceid.attr,
	NULL,
};

static struct attribute_group dummy_source_attr_grp = {
	.attrs = dummy_source_attrs,
};

static const struct attribute_group *dummy_source_attr_grps[] = {
	&dummy_source_attr_grp,
	NULL,
};

static int dummy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct coresight_platform_data *pdata;
	struct dummy_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	u32 trace_id;
	int ret;

	if (of_device_is_compatible(node, "arm,coresight-dummy-source")) {

		desc.name = coresight_alloc_device_name(&source_devs, dev);
		if (!desc.name)
			return -ENOMEM;

		desc.type = CORESIGHT_DEV_TYPE_SOURCE;
		desc.subtype.source_subtype =
					CORESIGHT_DEV_SUBTYPE_SOURCE_OTHERS;
		desc.ops = &dummy_source_cs_ops;
		desc.groups = dummy_source_attr_grps;
	} else if (of_device_is_compatible(node, "arm,coresight-dummy-sink")) {
		desc.name = coresight_alloc_device_name(&sink_devs, dev);
		if (!desc.name)
			return -ENOMEM;

		desc.type = CORESIGHT_DEV_TYPE_SINK;
		desc.subtype.sink_subtype = CORESIGHT_DEV_SUBTYPE_SINK_DUMMY;
		desc.ops = &dummy_sink_cs_ops;
	} else {
		dev_err(dev, "Device type not set\n");
		return -EINVAL;
	}

	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	pdev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &pdev->dev;
	mutex_init(&drvdata->mutex);
	platform_set_drvdata(pdev, drvdata);

	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;
	/* Allocate/reserve the native ATID before publishing any source attributes. */
	if (desc.type == CORESIGHT_DEV_TYPE_SOURCE) {
		if (device_property_present(dev, "atid")) {
			ret = device_property_read_u32(dev, "atid", &trace_id);
			if (ret || !IS_VALID_CS_TRACE_ID(trace_id))
				return -EINVAL;
			drvdata->static_atid = true;
			ret = coresight_trace_id_get_static_system_id(trace_id);
		} else {
			ret = coresight_trace_id_get_system_id();
		}
		if (ret < 0)
			return ret;
		drvdata->traceid = ret;
		drvdata->id_owned = true;
	}
	if (device_property_read_string(dev, "coresight-name", &desc.name)) {
		/* Keep the allocated fallback name. */
		desc.name = desc.type == CORESIGHT_DEV_TYPE_SOURCE ?
			coresight_alloc_device_name(&source_devs, dev) :
			coresight_alloc_device_name(&sink_devs, dev);
	}
	if (!desc.name) {
		ret = -ENOMEM;
		goto release_id;
	}
	pm_runtime_enable(dev);
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		pm_runtime_disable(dev);
		goto release_id;
	}
	dev_dbg(dev, "Dummy device initialized\n");

	return 0;
release_id:
	if (drvdata->id_owned)
		coresight_trace_id_put_system_id(drvdata->traceid);
	return ret;
}

static void dummy_remove(struct platform_device *pdev)
{
	struct dummy_drvdata *d = platform_get_drvdata(pdev);

	mutex_lock(&d->mutex);
	d->removing = true;
	mutex_unlock(&d->mutex);
	if (d->csdev->type == CORESIGHT_DEV_TYPE_SOURCE)
		coresight_disable_sysfs(d->csdev);
	coresight_unregister(d->csdev);
	pm_runtime_disable(&pdev->dev);
	if (d->id_owned) {
		coresight_trace_id_put_system_id(d->traceid);
		d->id_owned = false;
	}
}

static const struct of_device_id dummy_match[] = {
	{.compatible = "arm,coresight-dummy-source"},
	{.compatible = "arm,coresight-dummy-sink"},
	{},
};

MODULE_DEVICE_TABLE(of, dummy_match);

static struct platform_driver dummy_driver = {
	.probe	= dummy_probe,
	.remove	= dummy_remove,
	.driver	= {
		.name   = "coresight-dummy",
		.of_match_table = dummy_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(dummy_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight dummy driver");
