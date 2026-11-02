// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/amba/bus.h>
#include <linux/bitfield.h>
#include <linux/coresight.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "coresight-priv.h"
#include "coresight-tpda.h"
#include "coresight-trace-id.h"
#include "coresight-tpdm.h"

DEFINE_CORESIGHT_DEVLIST(tpda_devs, "tpda");

static bool coresight_device_is_tpdm(struct coresight_device *csdev)
{
	return (coresight_is_device_source(csdev)) &&
	       (csdev->subtype.source_subtype ==
			CORESIGHT_DEV_SUBTYPE_SOURCE_TPDM);
}

static void tpda_clear_element_size(struct coresight_device *csdev)
{
	struct tpda_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	drvdata->dsb_esize = 0;
	drvdata->cmb_esize = 0;
}

static void tpda_set_element_size(struct tpda_drvdata *drvdata, u32 *val)
{
	/* Clear all relevant fields */
	*val &= ~(TPDA_Pn_CR_DSBSIZE | TPDA_Pn_CR_CMBSIZE);

	if (drvdata->dsb_esize == 64)
		*val |= TPDA_Pn_CR_DSBSIZE;
	else if (drvdata->dsb_esize == 32)
		*val &= ~TPDA_Pn_CR_DSBSIZE;

	if (drvdata->cmb_esize == 64)
		*val |= FIELD_PREP(TPDA_Pn_CR_CMBSIZE, 0x2);
	else if (drvdata->cmb_esize == 32)
		*val |= FIELD_PREP(TPDA_Pn_CR_CMBSIZE, 0x1);
	else if (drvdata->cmb_esize == 8)
		*val &= ~TPDA_Pn_CR_CMBSIZE;
}

/*
 * Read the element size from the TPDM device. One TPDM must have at least one of the
 * element size property.
 * Returns
 *    0 - The element size property is read
 *    Others - Cannot read the property of the element size
 */
static int tpdm_read_element_size(struct tpda_drvdata *drvdata,
				  struct coresight_device *csdev)
{
	int rc = -EINVAL;
	struct tpdm_drvdata *tpdm_data = dev_get_drvdata(csdev->dev.parent);

	if (tpdm_data->dsb) {
		rc = fwnode_property_read_u32(dev_fwnode(csdev->dev.parent),
				"qcom,dsb-element-bits", &drvdata->dsb_esize);
		if (rc)
			goto out;
	}

	if (tpdm_data->cmb) {
		rc = fwnode_property_read_u32(dev_fwnode(csdev->dev.parent),
				"qcom,cmb-element-bits", &drvdata->cmb_esize);
	}

out:
	if (rc)
		dev_warn_once(&csdev->dev,
			"Failed to read TPDM Element size: %d\n", rc);

	return rc;
}

/*
 * Search and read element data size from the TPDM node in
 * the devicetree. Each input port of TPDA is connected to
 * a TPDM. Different TPDM supports different types of dataset,
 * and some may support more than one type of dataset.
 * Parameter "inport" is used to pass in the input port number
 * of TPDA, and it is set to -1 in the recursize call.
 */
static int tpda_get_element_size(struct tpda_drvdata *drvdata,
				 struct coresight_device *csdev,
				 int inport)
{
	int rc = 0;
	int i;
	struct coresight_device *in;

	for (i = 0; i < csdev->pdata->nr_inconns; i++) {
		in = csdev->pdata->in_conns[i]->src_dev;
		if (!in)
			continue;

		/* Ignore the paths that do not match port */
		if (inport >= 0 &&
		    csdev->pdata->in_conns[i]->dest_port != inport)
			continue;

		/*
		 * If this port has a hardcoded filter, use the source
		 * device directly.
		 */
		if (csdev->pdata->in_conns[i]->filter_src_fwnode) {
			in = csdev->pdata->in_conns[i]->filter_src_dev;
			if (!in)
				continue;
		}

		if (coresight_device_is_tpdm(in)) {
			if (drvdata->dsb_esize || drvdata->cmb_esize)
				return -EEXIST;
			rc = tpdm_read_element_size(drvdata, in);
			if (rc)
				return rc;
		} else {
			/* Recurse down the path */
			rc = tpda_get_element_size(drvdata, in, -1);
			if (rc)
				return rc;
		}
	}

	return rc;
}


/* Native v1 supplies (port, width) pairs on TPDA, rather than TPDM. */
static int tpda_parse_widths(const u32 *cells, unsigned int count, bool cmb,
                             u32 *widths)
{
	unsigned int i;

	if (!count || count > 2 * TPDA_MAX_INPORTS || count % 2)
		return -EINVAL;
	for (i = 0; i < count; i += 2) {
		u32 port = cells[i], width = cells[i + 1];

		if (port >= TPDA_MAX_INPORTS || widths[port] ||
		    (width != 32 && width != 64 && !(cmb && width == 8)))
			return -EINVAL;
		widths[port] = width;
	}
	return 0;
}

static int tpda_read_legacy_widths(struct device *dev, const char *name,
                                 bool cmb, u32 *widths)
{
	u32 cells[2 * TPDA_MAX_INPORTS];
	int count, ret;

	if (!device_property_present(dev, name))
		return 0;
	count = device_property_count_u32(dev, name);
	if (count <= 0 || count > ARRAY_SIZE(cells))
		return -EINVAL;
	ret = device_property_read_u32_array(dev, name, cells, count);
	return ret ? ret : tpda_parse_widths(cells, count, cmb, widths);
}

static bool tpda_legacy_authorized(u32 status)
{
	unsigned int shift;

	/* Honor the native AUTHSTATUS check; do not bypass debug authorization. */
	for (shift = 0; shift < 8; shift += 2)
		if (((status >> shift) & 3) == 2)
			return false;
	return true;
}

/* Settings pre enabling port control register */
static void tpda_enable_pre_port(struct tpda_drvdata *drvdata)
{
	u32 val;

	val = readl_relaxed(drvdata->base + TPDA_CR);
	val &= ~TPDA_CR_ATID;
	if (drvdata->legacy) {
		/* Native default: frequency timestamp on, trigger fields off. */
		val &= ~(GENMASK(19, 13) | BIT(20) | GENMASK(5, 2));
		val |= BIT(2);
	}
	val |= FIELD_PREP(TPDA_CR_ATID, drvdata->atid);
	writel_relaxed(val, drvdata->base + TPDA_CR);
}

static int tpda_enable_port(struct tpda_drvdata *drvdata, int port)
{
	u32 val;
	int rc;

	val = readl_relaxed(drvdata->base + TPDA_Pn_CR(port));
	if (port < 0 || port >= TPDA_MAX_INPORTS)
		return -EINVAL;
	tpda_clear_element_size(drvdata->csdev);
	if (drvdata->legacy) {
		drvdata->dsb_esize = drvdata->legacy_dsb[port];
		drvdata->cmb_esize = drvdata->legacy_cmb[port];
		rc = (drvdata->dsb_esize || drvdata->cmb_esize) ? 0 : -ENODATA;
		val &= ~GENMASK(3, 1);
		val |= FIELD_PREP(GENMASK(3, 1), 5);
	} else {
		rc = tpda_get_element_size(drvdata, drvdata->csdev, port);
	}
	if (!rc && (drvdata->dsb_esize || drvdata->cmb_esize)) {
		tpda_set_element_size(drvdata, &val);
		/* Enable the port */
		val |= TPDA_Pn_CR_ENA;
		writel_relaxed(val, drvdata->base + TPDA_Pn_CR(port));
	} else if (rc == -EEXIST)
		dev_warn_once(&drvdata->csdev->dev,
			      "Detected multiple TPDMs on port %d", port);
	else
		dev_warn_once(&drvdata->csdev->dev,
			      "Didn't find TPDM element size");

	return rc;
}

static int __tpda_enable(struct tpda_drvdata *drvdata, int port)
{
	int ret;

	CS_UNLOCK(drvdata->base);

	/*
	 * Only do pre-port enable for first port that calls enable when the
	 * device's main refcount is still 0
	 */
	lockdep_assert_held(&drvdata->spinlock);
	if (!drvdata->csdev->refcnt)
		tpda_enable_pre_port(drvdata);

	ret = tpda_enable_port(drvdata, port);
	CS_LOCK(drvdata->base);

	return ret;
}

static int tpda_enable(struct coresight_device *csdev,
		       struct coresight_connection *in,
		       struct coresight_connection *out)
{
	struct tpda_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	int ret = 0;

	spin_lock(&drvdata->spinlock);
	if (in->dest_refcnt == 0) {
		ret = __tpda_enable(drvdata, in->dest_port);
		if (!ret) {
			in->dest_refcnt++;
			csdev->refcnt++;
			dev_dbg(drvdata->dev, "TPDA inport %d enabled.\n", in->dest_port);
		}
	}

	spin_unlock(&drvdata->spinlock);
	return ret;
}

static void __tpda_disable(struct tpda_drvdata *drvdata, int port)
{
	u32 val;

	CS_UNLOCK(drvdata->base);

	val = readl_relaxed(drvdata->base + TPDA_Pn_CR(port));
	val &= ~TPDA_Pn_CR_ENA;
	writel_relaxed(val, drvdata->base + TPDA_Pn_CR(port));

	CS_LOCK(drvdata->base);
}

static void tpda_disable(struct coresight_device *csdev,
			 struct coresight_connection *in,
			 struct coresight_connection *out)
{
	struct tpda_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock(&drvdata->spinlock);
	if (--in->dest_refcnt == 0) {
		__tpda_disable(drvdata, in->dest_port);
		csdev->refcnt--;
	}
	spin_unlock(&drvdata->spinlock);

	dev_dbg(drvdata->dev, "TPDA inport %d disabled\n", in->dest_port);
}

static int tpda_trace_id(struct coresight_device *csdev, __maybe_unused enum cs_mode mode,
			 __maybe_unused struct coresight_device *sink)
{
	struct tpda_drvdata *drvdata;

	drvdata = dev_get_drvdata(csdev->dev.parent);

	return drvdata->atid;
}

static const struct coresight_ops_link tpda_link_ops = {
	.enable		= tpda_enable,
	.disable	= tpda_disable,
};

static const struct coresight_ops tpda_cs_ops = {
	.trace_id	= tpda_trace_id,
	.link_ops	= &tpda_link_ops,
};

static int tpda_init_default_data(struct tpda_drvdata *drvdata)
{
	int atid;
	/*
	 * TPDA must has a unique atid. This atid can uniquely
	 * identify the TPDM trace source connected to the TPDA.
	 * The TPDMs which are connected to same TPDA share the
	 * same trace-id. When TPDA does packetization, different
	 * port will have unique channel number for decoding.
	 */
	atid = coresight_trace_id_get_system_id();
	if (atid < 0)
		return atid;

	drvdata->atid = atid;
	return 0;
}

static int tpda_probe(struct amba_device *adev, const struct amba_id *id)
{
	int ret;
	struct device *dev = &adev->dev;
	struct coresight_platform_data *pdata;
	struct tpda_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	void __iomem *base;

	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	adev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->legacy = id->data != NULL;
	drvdata->dev = &adev->dev;
	dev_set_drvdata(dev, drvdata);

	base = devm_ioremap_resource(dev, &adev->res);
	if (IS_ERR(base))
		return PTR_ERR(base);
	drvdata->base = base;

	spin_lock_init(&drvdata->spinlock);
	if (resource_size(&adev->res) < 0x1000)
		return -EINVAL;
	if (drvdata->legacy) {
		if (!tpda_legacy_authorized(readl_relaxed(base + CORESIGHT_AUTHSTATUS)))
			return -EACCES;
		ret = tpda_read_legacy_widths(dev, "qcom,dsb-elem-size", false,
					    drvdata->legacy_dsb);
		if (ret)
			return ret;
		ret = tpda_read_legacy_widths(dev, "qcom,cmb-elem-size", true,
					    drvdata->legacy_cmb);
		if (ret)
			return ret;
	}

	ret = tpda_init_default_data(drvdata);
	if (ret)
		return ret;

	desc.name = coresight_alloc_device_name(&tpda_devs, dev);
	if (!desc.name) {
		coresight_trace_id_put_system_id(drvdata->atid);
		return -ENOMEM;
	}
	desc.type = CORESIGHT_DEV_TYPE_LINK;
	desc.subtype.link_subtype = CORESIGHT_DEV_SUBTYPE_LINK_MERG;
	desc.ops = &tpda_cs_ops;
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.access = CSDEV_ACCESS_IOMEM(base);
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		coresight_trace_id_put_system_id(drvdata->atid);
		return PTR_ERR(drvdata->csdev);
	}

	pm_runtime_put(&adev->dev);

	dev_dbg(drvdata->dev, "TPDA initialized\n");
	return 0;
}

static void tpda_remove(struct amba_device *adev)
{
	struct tpda_drvdata *drvdata = dev_get_drvdata(&adev->dev);

	coresight_unregister(drvdata->csdev);
	coresight_trace_id_put_system_id(drvdata->atid);
}

/*
 * Different TPDA has different periph id.
 * The difference is 0-7 bits' value. So ignore 0-7 bits.
 */
static const struct amba_id tpda_ids[] = {
	{
		.id     = 0x000f0f00,
		.mask   = 0x000fff00,
	},
	{
		.id = 0x0003b969,
		.mask = 0x0003ffff,
		.data = (void *)1,
	},
	{ 0, 0, NULL },
};

static struct amba_driver tpda_driver = {
	.drv = {
		.name   = "coresight-tpda",
		.suppress_bind_attrs = true,
	},
	.probe          = tpda_probe,
	.remove		= tpda_remove,
	.id_table	= tpda_ids,
};

MODULE_DEVICE_TABLE(amba, tpda_ids);

module_amba_driver(tpda_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Trace, Profiling & Diagnostic Aggregator driver");
