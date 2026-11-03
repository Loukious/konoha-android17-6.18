// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/coresight.h>
#include <linux/suspend.h>

#include "coresight-qmi.h"
#include "coresight-trace-id.h"
#include "coresight-csr.h"
#include <linux/property.h>

#ifdef CONFIG_CORESIGHT_REMOTE_ETM_DEFAULT_ENABLE
static int boot_enable = CONFIG_CORESIGHT_REMOTE_ETM_DEFAULT_ENABLE;
#else
static int boot_enable;
#endif

DEFINE_CORESIGHT_DEVLIST(remote_etm_devs, "remote-etm");

struct remote_etm_drvdata {
	struct device			*dev;
	struct coresight_device		*csdev;
	struct mutex			mutex;
	bool				enable;
	u8				*traceids;
	u32				num_trcid;
	bool				static_atid;
	bool				removing;
	bool				stop_failed;
	bool				suspended;
	unsigned int			reserved;
	struct coresight_path		*active_path;
};

/*
 * Remote ETM could be connected to a QMI device, which can send commmand
 * to subsystem via QMI. This is represented by the Output port of the remote
 * ETM connected to the input port of the QMI.
 *
 * Returns	: coresight_device ptr for the QMI device if a QMI is found.
 *		: NULL otherwise.
 */
static struct coresight_device *remote_etm_get_qmi_device(struct remote_etm_drvdata *drvdata)
{
	int i;
	struct coresight_device *tmp, *etm = drvdata->csdev;

	if (!IS_ENABLED(CONFIG_CORESIGHT_QMI))
		return NULL;

	for (i = 0; i < etm->pdata->nr_outconns; i++) {
		tmp = etm->pdata->out_conns[i]->dest_dev;
		if (!tmp && fwnode_property_match_string(etm->pdata->out_conns[i]->dest_fwnode,
				"compatible", "qcom,coresight-qmi") >= 0)
			return ERR_PTR(-EPROBE_DEFER);
		if (tmp && coresight_is_qmi_device(tmp))
			return tmp;
	}

	return NULL;
}

static int qmi_assign_remote_etm_atid(struct remote_etm_drvdata *d)
{
	struct coresight_device *qmi = remote_etm_get_qmi_device(d);
	struct coresight_atid_assign_req_msg_v01 req = {};
	const char *name;
	int ret;

	if (IS_ERR(qmi))
		return PTR_ERR(qmi);
	if (!qmi)
		return -ENODEV;
	ret = device_property_read_string(d->dev, "trace-name", &name);
	if (ret)
		return ret;
	ret = strscpy(req.name, name, sizeof(req.name));
	if (ret < 0)
		return ret;
	memcpy(req.atids, d->traceids, d->num_trcid);
	req.num_atids = d->num_trcid;
	return coresight_qmi_assign_atid(qmi, &req);
}

static int qmi_enable_remote_etm(struct remote_etm_drvdata *d)
{
	struct coresight_device *qmi = remote_etm_get_qmi_device(d);

	return IS_ERR(qmi) ? PTR_ERR(qmi) : qmi ? coresight_qmi_remote_etm_enable(qmi) : -ENODEV;
}

static int qmi_disable_remote_etm(struct remote_etm_drvdata *d)
{
	struct coresight_device *qmi = remote_etm_get_qmi_device(d);

	return IS_ERR(qmi) ? PTR_ERR(qmi) : qmi ? coresight_qmi_remote_etm_disable(qmi) : -ENODEV;
}

static void remote_etm_release_ids(struct remote_etm_drvdata *d)
{
	while (d->reserved)
		coresight_trace_id_put_system_id(d->traceids[--d->reserved]);
}

static int remote_etm_enable(struct coresight_device *csdev,
		struct perf_event *event, enum cs_mode mode, struct coresight_path *path)
{
	struct remote_etm_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	unsigned int i;
	int ret;

	/* Native subsystem sources are controlled by the sysfs trace path. */
	if (mode != CS_MODE_SYSFS)
		return -EINVAL;

	mutex_lock(&d->mutex);
	if (d->removing) {
		ret = -ENODEV;
		goto out;
	}
	/* Never recycle an ID while a previous stop was unconfirmed. */
	if (d->stop_failed) {
		ret = qmi_disable_remote_etm(d);
		if (ret)
			goto out;
		d->stop_failed = false;
		d->enable = false;
		if (d->static_atid)
			remote_etm_release_ids(d);
	}
	if (d->enable || !coresight_take_mode(csdev, mode)) {
		ret = -EBUSY;
		goto out;
	}
	if (d->static_atid) {
		for (i = 0; i < d->num_trcid; i++) {
			ret = coresight_trace_id_get_static_system_id(d->traceids[i]);
			if (ret < 0)
				goto release_ids;
			d->reserved++;
		}
	} else {
		ret = qmi_assign_remote_etm_atid(d);
		if (ret)
			goto release_ids;
	}
	ret = qcom_csr_route_ids(path, d->traceids, d->num_trcid, true);
	if (ret)
		goto release_ids;
	ret = qmi_enable_remote_etm(d);
	if (ret) {
		qcom_csr_route_ids(path, d->traceids, d->num_trcid, false);
		if (qmi_disable_remote_etm(d)) {
			d->stop_failed = true;
			d->enable = true;
		}
		goto release_ids;
	}
	d->active_path = path;
	d->enable = true;
	d->suspended = false;
	ret = 0;
	goto out;
release_ids:
	if (d->static_atid && !d->stop_failed)
		remote_etm_release_ids(d);
	coresight_set_mode(csdev, CS_MODE_DISABLED);
out:
	mutex_unlock(&d->mutex);
	return ret;
}

static void remote_etm_disable(struct coresight_device *csdev, struct perf_event *event)
{
	struct remote_etm_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	int ret = 0;

	mutex_lock(&d->mutex);
	if (d->enable && !d->suspended)
		ret = qmi_disable_remote_etm(d);
	if (d->active_path)
		qcom_csr_route_ids(d->active_path, d->traceids, d->num_trcid, false);
	d->active_path = NULL;
	d->stop_failed = ret != 0;
	d->enable = d->stop_failed;
	d->suspended = false;
	if (!ret && d->static_atid)
		remote_etm_release_ids(d);
	if (ret)
		dev_err(d->dev, "Remote ETM stop unconfirmed (%d); retaining trace IDs\n", ret);
	coresight_set_mode(csdev, CS_MODE_DISABLED);
	mutex_unlock(&d->mutex);
}


static const struct coresight_ops_source remote_etm_source_ops = {
	.enable		= remote_etm_enable,
	.disable	= remote_etm_disable,
};

static int remote_etm_trace_id(struct coresight_device *csdev, enum cs_mode mode,
		struct coresight_device *sink)
{
	struct remote_etm_drvdata *d = dev_get_drvdata(csdev->dev.parent);

	return d->traceids[0];
}

static const struct coresight_ops remote_cs_ops = {
	.source_ops	= &remote_etm_source_ops,
	.trace_id = remote_etm_trace_id,
};

static ssize_t traceid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct remote_etm_drvdata *d = dev_get_drvdata(dev->parent);
	unsigned int i;
	int len = 0;

	for (i = 0; i < d->num_trcid; i++)
		len += sysfs_emit_at(buf, len, "%s%#x", i ? " " : "", d->traceids[i]);
	return len + sysfs_emit_at(buf, len, "\n");
}
static DEVICE_ATTR_RO(traceid);

static struct attribute *remote_etm_attrs[] = {
	&dev_attr_traceid.attr,
	NULL,
};

static struct attribute_group remote_etm_attr_grp = {
	.attrs = remote_etm_attrs,
};

static const struct attribute_group *remote_etm_attr_grps[] = {
	&remote_etm_attr_grp,
	NULL,
};

static int remote_etm_get_traceid(struct remote_etm_drvdata *d)
{
	u32 atids[8];
	unsigned int i, j;
	int count, ret;

	d->static_atid = device_property_present(d->dev, "atid");
	if (d->static_atid) {
		count = of_property_count_u32_elems(d->dev->of_node, "atid");
		if (count <= 0 || count > (int)ARRAY_SIZE(atids))
			return -EINVAL;
		d->num_trcid = count;
		ret = of_property_read_u32_array(d->dev->of_node, "atid", atids, count);
		if (ret)
			return ret;
		for (i = 0; i < d->num_trcid; i++) {
			if (!IS_VALID_CS_TRACE_ID(atids[i]))
				return -EINVAL;
			for (j = 0; j < i; j++)
				if (atids[i] == atids[j])
					return -EINVAL;
		}
	} else {
		ret = device_property_read_u32(d->dev, "qcom,atid-num", &d->num_trcid);
		if (ret || !d->num_trcid || d->num_trcid > ARRAY_SIZE(atids))
			return -EINVAL;
	}
	d->traceids = devm_kcalloc(d->dev, d->num_trcid, sizeof(*d->traceids), GFP_KERNEL);
	if (!d->traceids)
		return -ENOMEM;
	for (i = 0; i < d->num_trcid; i++) {
		if (d->static_atid)
			d->traceids[i] = atids[i];
		else {
			ret = coresight_trace_id_get_system_id();
			if (ret < 0) {
				remote_etm_release_ids(d);
				return ret;
			}
			d->traceids[i] = ret;
			d->reserved++;
		}
	}
	return 0;
}

static int remote_etm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct remote_etm_drvdata *drvdata;
	struct coresight_desc desc = {0 };
	int ret;

	if (device_property_read_string(dev, "coresight-name", &desc.name))
		desc.name = coresight_alloc_device_name(&remote_etm_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	pdev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, drvdata);

	mutex_init(&drvdata->mutex);

	desc.type = CORESIGHT_DEV_TYPE_SOURCE;
	desc.subtype.source_subtype = CORESIGHT_DEV_SUBTYPE_SOURCE_SOFTWARE;
	desc.ops = &remote_cs_ops;
	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;
	desc.groups = remote_etm_attr_grps;
	ret = remote_etm_get_traceid(drvdata);
	if (ret)
		return ret;
	pm_runtime_enable(dev);
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		pm_runtime_disable(dev);
		goto err;
	}
	dev_info(dev, "Remote ETM initialized\n");

	if (boot_enable) {
		ret = coresight_enable_sysfs(drvdata->csdev);
		if (ret)
			dev_warn(dev, "Default remote ETM enable failed: %d\n", ret);
	}

	return 0;

err:
	remote_etm_release_ids(drvdata);
	return ret;
}

static void remote_etm_remove(struct platform_device *pdev)
{
	struct remote_etm_drvdata *d = platform_get_drvdata(pdev);
	int ret = 0;

	mutex_lock(&d->mutex);
	d->removing = true;
	mutex_unlock(&d->mutex);
	coresight_disable_sysfs(d->csdev);
	mutex_lock(&d->mutex);
	if (d->enable)
		ret = qmi_disable_remote_etm(d);
	if (!ret)
		remote_etm_release_ids(d);
	else
		dev_err(d->dev, "Retaining IDs after unconfirmed remote stop: %d\n", ret);
	mutex_unlock(&d->mutex);
	coresight_unregister(d->csdev);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id remote_etm_match[] = {
	{.compatible = "qcom,coresight-remote-etm"},
	{}
};

static int remote_etm_suspend(struct device *dev)
{
	struct remote_etm_drvdata *d = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&d->mutex);
	if (d->enable && !d->suspended) {
		ret = qmi_disable_remote_etm(d);
		if (!ret)
			d->suspended = true;
	}
	mutex_unlock(&d->mutex);
	return ret;
}

static int remote_etm_resume(struct device *dev)
{
	struct remote_etm_drvdata *d = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&d->mutex);
	if (d->enable && d->suspended) {
		ret = qmi_enable_remote_etm(d);
		if (!ret)
			d->suspended = false;
	}
	mutex_unlock(&d->mutex);
	return ret;
}

static const struct dev_pm_ops remote_etm_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(remote_etm_suspend, remote_etm_resume)
};
MODULE_DEVICE_TABLE(of, remote_etm_match);

static struct platform_driver remote_etm_driver = {
	.probe          = remote_etm_probe,
	.remove         = remote_etm_remove,
	.driver         = {
		.name   = "coresight-remote-etm",
		.of_match_table = remote_etm_match,
		.suppress_bind_attrs = true,
		.pm = pm_ptr(&remote_etm_dev_pm_ops),
	},
};

static int __init remote_etm_init(void)
{
	return platform_driver_register(&remote_etm_driver);
}
module_init(remote_etm_init);

static void __exit remote_etm_exit(void)
{
	platform_driver_unregister(&remote_etm_driver);
}
module_exit(remote_etm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight Remote ETM driver");
