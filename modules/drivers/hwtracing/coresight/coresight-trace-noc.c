// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/amba/bus.h>
#include <linux/coresight.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "coresight-priv.h"

#include "coresight-trace-id.h"
#include "coresight-trace-noc.h"

static ssize_t flush_req_store(struct device *dev, struct device_attribute *attr, const char *buf,
			       size_t size)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;
	u32 reg;

	if (kstrtoul(buf, 16, &val) || val > 1)
		return -EINVAL;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (!drvdata->enable) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EPERM;
	}

	if (val) {
		reg = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
		reg = reg | TRACE_NOC_CTRL_FLUSHREQ;
		writel_relaxed(reg, drvdata->base + TRACE_NOC_CTRL);
	}
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	return size;
}
static DEVICE_ATTR_WO(flush_req);

static ssize_t flush_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	u32 val;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (!drvdata->enable) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EPERM;
	}

	val = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
	spin_unlock_irqrestore(&drvdata->spinlock, flags);
	return scnprintf(buf, PAGE_SIZE, "%lx\n", BMVAL(val, 2, 2));
}
static DEVICE_ATTR_RO(flush_status);

static ssize_t flag_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%x\n", drvdata->flagType);
}

static ssize_t flag_type_store(struct device *dev, struct device_attribute *attr, const char *buf,
			       size_t size)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val) || val > 1)
		return -EINVAL;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (drvdata->enable) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EBUSY;
	}
	if (val)
		drvdata->flagType = FLAG_TS;
	else
		drvdata->flagType = FLAG;
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	return size;
}
static DEVICE_ATTR_RW(flag_type);

static ssize_t freq_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%x\n", drvdata->freqType);
}

static ssize_t freq_type_store(struct device *dev, struct device_attribute *attr, const char *buf,
			       size_t size)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 16, &val) || val > 1)
		return -EINVAL;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (drvdata->enable) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EBUSY;
	}
	if (val)
		drvdata->freqType = FREQ_TS;
	else
		drvdata->freqType = FREQ;
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	return size;
}
static DEVICE_ATTR_RW(freq_type);

static ssize_t freq_req_val_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return scnprintf(buf, PAGE_SIZE, "%x\n", drvdata->freq_req_val);
}

static ssize_t freq_req_val_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t size)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 0, &val) || val > U32_MAX)
		return -EINVAL;

	if (val) {
		spin_lock_irqsave(&drvdata->spinlock, flags);
		if (drvdata->enable) {
			spin_unlock_irqrestore(&drvdata->spinlock, flags);
			return -EBUSY;
		}
		drvdata->freq_req_val = val;
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
	}

	return size;
}
static DEVICE_ATTR_RW(freq_req_val);

static ssize_t freq_ts_req_store(struct device *dev, struct device_attribute *attr, const char *buf,
				 size_t size)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;
	u32 reg;

	if (kstrtoul(buf, 16, &val) || val > 1)
		return -EINVAL;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (!drvdata->enable) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EPERM;
	}

	if (val) {
		reg = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
		if (drvdata->version == TRACE_NOC_VERSION_V2)
			reg = reg | TRACE_NOC_CTRL_FREQTSREQ_V2;
		else
			reg = reg | TRACE_NOC_CTRL_FREQTSREQ;
		writel_relaxed(reg, drvdata->base + TRACE_NOC_CTRL);
	}
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	return size;
}
static DEVICE_ATTR_WO(freq_ts_req);

static struct attribute *trace_noc_attrs[] = {
    &dev_attr_flush_req.attr,
    &dev_attr_flush_status.attr,
    &dev_attr_flag_type.attr,
    &dev_attr_freq_type.attr,
    &dev_attr_freq_req_val.attr,
    &dev_attr_freq_ts_req.attr,
    NULL,
};

static struct attribute_group trace_noc_attr_grp = {
    .attrs = trace_noc_attrs,
};

static const struct attribute_group *trace_noc_attr_grps[] = {
    &trace_noc_attr_grp,
    NULL,
};

static int trace_noc_trace_id(struct coresight_device *csdev, enum cs_mode mode,
			      struct coresight_device *sink)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	return drvdata->atid;
}

static int trace_noc_enable(struct coresight_device *csdev, struct coresight_connection *inport,
			    struct coresight_connection *outport)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	int ret;
	u32 val;
	int i, nr_conns;

	spin_lock_irqsave(&drvdata->spinlock, flags);

	nr_conns = csdev->pdata->nr_inconns;
	for (i = 0; i < nr_conns; i++) {
		if (csdev->pdata->in_conns[i]->dest_refcnt != 0) {
			inport->dest_refcnt++;
			spin_unlock_irqrestore(&drvdata->spinlock, flags);
			return 0;
		}
	}
	ret = coresight_claim_device(csdev);
	if (ret < 0) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return ret;
	}
	/* Set ATID */
	writel_relaxed(drvdata->atid, drvdata->base + TRACE_NOC_XLD);

	/* Config sync CR */
	writel_relaxed(0xffff, drvdata->base + TRACE_NOC_SYNCR);

	/* Set frequency value */
	if (drvdata->freq_req_val)
		writel_relaxed(drvdata->freq_req_val, drvdata->base + TRACE_NOC_FREQVAL);

	/* Set Ctrl register */
	val = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
	if (drvdata->version == TRACE_NOC_VERSION_V2) {
		if (drvdata->flagType == FLAG_TS)
			val = val | TRACE_NOC_CTRL_FLAGTYPE_V2;
		else
			val = val & ~TRACE_NOC_CTRL_FLAGTYPE_V2;
		if (drvdata->freqType == FREQ_TS)
			val = val | TRACE_NOC_CTRL_FREQTYPE_V2;
		else
			val = val & ~TRACE_NOC_CTRL_FREQTYPE_V2;
	} else {
		if (drvdata->flagType == FLAG_TS)
			val = val | TRACE_NOC_CTRL_FLAGTYPE;
		else
			val = val & ~TRACE_NOC_CTRL_FLAGTYPE;
		if (drvdata->freqType == FREQ_TS)
			val = val | TRACE_NOC_CTRL_FREQTYPE;
		else
			val = val & ~TRACE_NOC_CTRL_FREQTYPE;
	}

	val = val | TRACE_NOC_CTRL_PORTEN;
	writel_relaxed(val, drvdata->base + TRACE_NOC_CTRL);
	inport->dest_refcnt++;
	drvdata->enable = true;
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	dev_dbg(drvdata->dev, "Trace NOC is enabled\n");
	return 0;
}

static void trace_noc_disable(struct coresight_device *csdev, struct coresight_connection *inport,
			      struct coresight_connection *outport)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	int i, nr_conns;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	inport->dest_refcnt--;

	nr_conns = csdev->pdata->nr_inconns;
	for (i = 0; i < nr_conns; i++) {
		if (csdev->pdata->in_conns[i]->dest_refcnt != 0) {
			spin_unlock_irqrestore(&drvdata->spinlock, flags);
			return;
		}
	}

	writel_relaxed(0x0, drvdata->base + TRACE_NOC_CTRL);
	drvdata->enable = false;
	coresight_disclaim_device(csdev);
	spin_unlock_irqrestore(&drvdata->spinlock, flags);
	dev_dbg(drvdata->dev, "Trace NOC is disabled\n");
}

static const struct coresight_ops_link trace_noc_link_ops = {
    .enable = trace_noc_enable,
    .disable = trace_noc_disable,
};

static const struct coresight_ops trace_noc_cs_ops = {
    .trace_id = trace_noc_trace_id,
    .link_ops = &trace_noc_link_ops,
};

static int interconnect_trace_noc_enable(struct coresight_device *csdev,
					 struct coresight_connection *inport,
					 struct coresight_connection *outport)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	u32 val;
	int i, nr_conns;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	nr_conns = csdev->pdata->nr_inconns;
	for (i = 0; i < nr_conns; i++) {
		if (csdev->pdata->in_conns[i]->dest_refcnt != 0) {
			inport->dest_refcnt++;
			spin_unlock_irqrestore(&drvdata->spinlock, flags);
			return 0;
		}
	}
	/* Set Ctrl register */
	val = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
	val = val | TRACE_NOC_CTRL_PORTEN;
	writel_relaxed(val, drvdata->base + TRACE_NOC_CTRL);

	drvdata->enable = true;
	inport->dest_refcnt++;
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	dev_dbg(drvdata->dev, "Trace NOC is enabled\n");
	return 0;
}

static void interconnect_trace_noc_disable(struct coresight_device *csdev,
					   struct coresight_connection *inport,
					   struct coresight_connection *outport)
{
	unsigned long flags;
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	int i, nr_conns;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	inport->dest_refcnt--;

	nr_conns = csdev->pdata->nr_inconns;
	for (i = 0; i < nr_conns; i++) {
		if (csdev->pdata->in_conns[i]->dest_refcnt != 0) {
			spin_unlock_irqrestore(&drvdata->spinlock, flags);
			return;
		}
	}
	writel_relaxed(0x0, drvdata->base + TRACE_NOC_CTRL);
	drvdata->enable = false;
	spin_unlock_irqrestore(&drvdata->spinlock, flags);
	dev_dbg(drvdata->dev, "Trace NOC is disabled\n");
}

static const struct coresight_ops_link interconnect_trace_noc_link_ops = {
    .enable = interconnect_trace_noc_enable,
    .disable = interconnect_trace_noc_disable,
};

static const struct coresight_ops interconnect_trace_noc_cs_ops = {
    .link_ops = &interconnect_trace_noc_link_ops,
};

static void trace_noc_init_default_data(struct trace_noc_drvdata *drvdata)
{
	drvdata->freqType = FREQ_TS;
	drvdata->freqTsReq = true;
}

static int trace_noc_probe(struct amba_device *adev, const struct amba_id *id)
{
	struct device *dev = &adev->dev;
	struct coresight_platform_data *pdata;
	struct trace_noc_drvdata *drvdata;
	struct coresight_desc desc = {0};

	desc.name = coresight_alloc_device_name(&trace_noc_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	adev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &adev->dev;
	dev_set_drvdata(dev, drvdata);

	drvdata->base = devm_ioremap_resource(dev, &adev->res);
	if (IS_ERR(drvdata->base))
		return PTR_ERR(drvdata->base);
	if (resource_size(&adev->res) < 0x1000)
		return -EINVAL;
	spin_lock_init(&drvdata->spinlock);

	if (of_property_read_bool(dev->of_node, "qcom,trace-noc-v2"))
		drvdata->version = TRACE_NOC_VERSION_V2;

	if (of_property_read_bool(dev->of_node, "qcom,interconnect-trace-noc")) {
		drvdata->atid = 0;
		desc.ops = &interconnect_trace_noc_cs_ops;
	} else {
		trace_noc_init_default_data(drvdata);
		drvdata->atid = coresight_trace_id_get_system_id();
		if ((int)drvdata->atid < 0)
			return (int)drvdata->atid;
		desc.ops = &trace_noc_cs_ops;
		desc.groups = trace_noc_attr_grps;
	}

	desc.type = CORESIGHT_DEV_TYPE_LINK;
	desc.subtype.link_subtype = CORESIGHT_DEV_SUBTYPE_LINK_MERG;
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.access = CSDEV_ACCESS_IOMEM(drvdata->base);
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		if (drvdata->atid)
			coresight_trace_id_put_system_id(drvdata->atid);
		return PTR_ERR(drvdata->csdev);
	}

	pm_runtime_put_sync(&adev->dev);

	dev_dbg(drvdata->dev, "Trace Noc initialized\n");
	return 0;
}

static void trace_noc_remove(struct amba_device *adev)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(&adev->dev);

	coresight_unregister(drvdata->csdev);
	if (drvdata->atid)
		coresight_trace_id_put_system_id(drvdata->atid);
}

static struct amba_id trace_noc_ids[] = {
    {
	.id = 0x000f0c00,
	.mask = 0x000fff00,
    },
    {0, 0},
};
MODULE_DEVICE_TABLE(amba, trace_noc_ids);

static struct amba_driver trace_noc_driver = {
    .drv =
	{
	    .name = "coresight-trace-noc",
	    .owner = THIS_MODULE,
	    .suppress_bind_attrs = true,
	},
    .probe = trace_noc_probe,
    .remove = trace_noc_remove,
    .id_table = trace_noc_ids,
};

module_amba_driver(trace_noc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Trace NOC driver");
