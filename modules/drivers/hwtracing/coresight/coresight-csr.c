// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2013, 2015-2017, 2019-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Qualcomm CSR register layouts from the pinned native provider.
 * Integrate ATID routing as a common CoreSight helper: one vote per path.
 */
#include "coresight-csr.h"
#include "coresight-priv.h"
#include "coresight-trace-id.h"
#include <linux/of.h>
#include <linux/property.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define CSR_ATIDS 128
#define CSR_BANKS 2
#define CSR_BYTECNT0 0x6c
#define CSR_BYTECNT1 0x70
#define CSR_ATID0 0xf8
#define CSR_ATID1 0x108
#define CSR_TIMESTAMPCTRL 0x38
#define CSR_QDSSTIMEVAL0 0x44
#define CSR_QDSSTIMEVAL1 0x48

struct csr_drvdata {
	void __iomem *base;
	size_t size;
	struct coresight_device *csdev;
	struct clk *pclk, *atclk;
	raw_spinlock_t lock;
	unsigned int refs[CSR_BANKS][CSR_ATIDS];
	bool original[CSR_BANKS][CSR_ATIDS];
	unsigned int users;
	bool byte_counter, timestamp;
};
DEFINE_CORESIGHT_DEVLIST(csr_devs, "csr");

/* Only the two verified native ETR banks are supported. */
static int csr_bank(u32 offset, size_t size)
{
	if ((offset != CSR_ATID0 && offset != CSR_ATID1) || offset > size || 16 > size - offset)
		return -EINVAL;
	return offset == CSR_ATID0 ? 0 : 1;
}

static int csr_route(struct csr_drvdata *d, u32 offset, unsigned int id, bool enable)
{
	unsigned long flags;
	u32 reg, mask, val;
	int bank, ret = 0;

	bank = csr_bank(offset, d->size);
	if (bank < 0 || !IS_VALID_CS_TRACE_ID(id))
		return -EINVAL;
	reg = offset + (id / 32) * 4;
	mask = BIT(id % 32);
	raw_spin_lock_irqsave(&d->lock, flags);
	if (enable) {
		if (d->refs[bank][id] == UINT_MAX || d->users == UINT_MAX) {
			ret = -EOVERFLOW;
			goto out;
		}
		if (!d->users) {
			ret = coresight_claim_device(d->csdev);
			if (ret)
				goto out;
		}
		if (!d->refs[bank][id]) {
			CS_UNLOCK(d->base);
			val = readl_relaxed(d->base + reg);
			d->original[bank][id] = !!(val & mask);
			writel_relaxed(val | mask, d->base + reg);
			CS_LOCK(d->base);
		}
		d->refs[bank][id]++;
		d->users++;
	} else if (d->refs[bank][id]) {
		if (!--d->refs[bank][id]) {
			CS_UNLOCK(d->base);
			val = readl_relaxed(d->base + reg);
			val = d->original[bank][id] ? val | mask : val & ~mask;
			writel_relaxed(val, d->base + reg);
			CS_LOCK(d->base);
		}
		if (!--d->users)
			coresight_disclaim_device(d->csdev);
	}
out:
	raw_spin_unlock_irqrestore(&d->lock, flags);
	return ret;
}

static int csr_path_route(struct coresight_device *csdev, void *data, bool enable)
{
	struct csr_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	struct coresight_path *path = data;
	struct coresight_device *sink;
	u32 offset;

	if (!path)
		return -EINVAL;
	sink = coresight_get_sink(path);
	if (!sink || device_property_read_u32(sink->dev.parent, "csr-atid-offset", &offset))
		return -EINVAL;
	return csr_route(d, offset, path->trace_id, enable);
}

static int csr_enable(struct coresight_device *csdev, enum cs_mode mode, void *data)
{
	return csr_path_route(csdev, data, true);
}
static int csr_disable(struct coresight_device *csdev, void *data)
{
	return csr_path_route(csdev, data, false);
}
static const struct coresight_ops_helper csr_helper_ops = {
    .enable = csr_enable,
    .disable = csr_disable,
};
static const struct coresight_ops csr_ops = {.helper_ops = &csr_helper_ops};

int qcom_csr_set_byte_counter(struct coresight_device *csdev, u32 offset, u32 count)
{
	struct csr_drvdata *d;
	unsigned long flags;

	if (!csdev)
		return -ENODEV;
	d = dev_get_drvdata(csdev->dev.parent);
	if (!d->byte_counter)
		return -EOPNOTSUPP;
	if ((offset != CSR_BYTECNT0 && offset != CSR_BYTECNT1) || offset > d->size ||
	    4 > d->size - offset)
		return -EINVAL;
	raw_spin_lock_irqsave(&d->lock, flags);
	CS_UNLOCK(d->base);
	writel(count, d->base + offset);
	CS_LOCK(d->base);
	raw_spin_unlock_irqrestore(&d->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(qcom_csr_set_byte_counter);

/* Resolve the supplier before publishing a sink that depends on its helper. */
int qcom_csr_attach(struct device *dev, struct coresight_platform_data *pdata,
		    struct coresight_device **helper)
{
	struct device_node *node;
	struct platform_device *supplier;
	struct csr_drvdata *d;
	struct coresight_connection conn = {.src_port = -1, .dest_port = -1};
	struct coresight_connection *added;
	u32 offset;
	int ret = 0;

	if (!device_property_present(dev, "coresight-csr"))
		return 0;
	node = of_parse_phandle(dev->of_node, "coresight-csr", 0);
	if (!node)
		return -EINVAL;
	supplier = of_find_device_by_node(node);
	if (!supplier) {
		ret = -EPROBE_DEFER;
		goto out_node;
	}
	device_lock(&supplier->dev);
	d = platform_get_drvdata(supplier);
	if (!supplier->dev.driver || !d || !d->csdev) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}
	if (device_property_read_u32(dev, "csr-atid-offset", &offset) ||
	    csr_bank(offset, d->size) < 0) {
		ret = -EINVAL;
		goto out_unlock;
	}
	conn.dest_fwnode = of_fwnode_handle(node);
	added = coresight_add_out_conn(dev, pdata, &conn);
	if (IS_ERR(added)) {
		ret = PTR_ERR(added);
		goto out_unlock;
	}
	if (!device_link_add(dev, &supplier->dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER | DL_FLAG_PM_RUNTIME)) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	*helper = d->csdev;
out_unlock:
	device_unlock(&supplier->dev);
	put_device(&supplier->dev);
out_node:
	of_node_put(node);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_csr_attach);

static ssize_t timestamp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct csr_drvdata *d = dev_get_drvdata(dev->parent);
	unsigned long flags;
	u32 val, lo, hi;
	int ret;

	if (!d->timestamp)
		return -EOPNOTSUPP;
	ret = pm_runtime_resume_and_get(dev->parent);
	if (ret < 0)
		return ret;
	raw_spin_lock_irqsave(&d->lock, flags);
	CS_UNLOCK(d->base);
	val = readl_relaxed(d->base + CSR_TIMESTAMPCTRL);
	writel_relaxed(val & ~BIT(0), d->base + CSR_TIMESTAMPCTRL);
	writel(val | BIT(0), d->base + CSR_TIMESTAMPCTRL);
	lo = readl_relaxed(d->base + CSR_QDSSTIMEVAL0);
	hi = readl_relaxed(d->base + CSR_QDSSTIMEVAL1);
	CS_LOCK(d->base);
	raw_spin_unlock_irqrestore(&d->lock, flags);
	pm_runtime_put(dev->parent);
	return sysfs_emit(buf, "%llu\n", ((u64)hi << 32) | lo);
}
static DEVICE_ATTR_RO(timestamp);
static struct attribute *csr_attrs[] = {&dev_attr_timestamp.attr, NULL};
static const struct attribute_group csr_group = {.attrs = csr_attrs};
static const struct attribute_group *csr_groups[] = {&csr_group, NULL};

static int csr_runtime_suspend(struct device *dev)
{
	struct csr_drvdata *d = dev_get_drvdata(dev);
	clk_disable_unprepare(d->atclk);
	clk_disable_unprepare(d->pclk);
	return 0;
}
static int csr_runtime_resume(struct device *dev)
{
	struct csr_drvdata *d = dev_get_drvdata(dev);
	int ret = clk_prepare_enable(d->pclk);
	if (ret)
		return ret;
	ret = clk_prepare_enable(d->atclk);
	if (ret)
		clk_disable_unprepare(d->pclk);
	return ret;
}
static const struct dev_pm_ops csr_pm_ops = {
    SET_RUNTIME_PM_OPS(csr_runtime_suspend, csr_runtime_resume, NULL)};

static int csr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct csr_drvdata *d;
	struct resource *res;
	struct coresight_desc desc = {};
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	platform_set_drvdata(pdev, d);
	raw_spin_lock_init(&d->lock);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "csr-base");
	if (!res || resource_size(res) < 0x1000)
		return -EINVAL;
	d->size = resource_size(res);
	d->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(d->base))
		return PTR_ERR(d->base);
	d->byte_counter = device_property_present(dev, "qcom,set-byte-cntr-support");
	d->timestamp = device_property_present(dev, "qcom,timestamp-support");
	desc.pdata = coresight_get_platform_data(dev);
	if (IS_ERR(desc.pdata))
		return PTR_ERR(desc.pdata);
	dev->platform_data = desc.pdata;
	desc.name = coresight_alloc_device_name(&csr_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.subtype.helper_subtype = CORESIGHT_DEV_SUBTYPE_HELPER_QCOM_CSR;
	desc.ops = &csr_ops;
	desc.dev = dev;
	desc.access = CSDEV_ACCESS_IOMEM(d->base);
	desc.groups = csr_groups;
	ret = coresight_get_enable_clocks(dev, &d->pclk, &d->atclk);
	if (ret)
		return ret;
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	coresight_clear_self_claim_tag(&desc.access);
	d->csdev = coresight_register(&desc);
	ret = IS_ERR(d->csdev) ? PTR_ERR(d->csdev) : 0;
	if (ret)
		d->csdev = NULL;
	pm_runtime_put(dev);
	if (ret)
		pm_runtime_disable(dev);
	return ret;
}

static void csr_remove(struct platform_device *pdev)
{
	struct csr_drvdata *d = platform_get_drvdata(pdev);
	coresight_unregister(d->csdev);
	pm_runtime_disable(&pdev->dev);
}
static const struct of_device_id csr_match[] = {{.compatible = "qcom,coresight-csr"}, {}};
MODULE_DEVICE_TABLE(of, csr_match);
static struct platform_driver csr_driver = {
    .probe = csr_probe,
    .remove = csr_remove,
    .driver = {.name = "coresight-csr",
	       .of_match_table = csr_match,
	       .pm = &csr_pm_ops,
	       .suppress_bind_attrs = true},
};
module_platform_driver(csr_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm CoreSight CSR ATID routing and byte counter");
