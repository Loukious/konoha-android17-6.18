// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/amba/bus.h>
#include <linux/topology.h>
#include <linux/of.h>
#include <linux/coresight.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>

#include "coresight-priv.h"

#define tgu_writel(drvdata, val, off)	__raw_writel((val), drvdata->base + off)
#define tgu_readl(drvdata, off)		__raw_readl(drvdata->base + off)

#define TGU_LOCK(drvdata)						\
do {									\
	mb(); /* ensure configuration take effect before we lock it */	\
	tgu_writel(drvdata, 0x0, CORESIGHT_LAR);			\
} while (0)
#define TGU_UNLOCK(drvdata)						\
do {									\
	tgu_writel(drvdata, CORESIGHT_UNLOCK, CORESIGHT_LAR);		\
	mb(); /* ensure unlock take effect before we configure */	\
} while (0)

#define TGU_CONTROL			0x0000
#define TIMER0_STATUS			0x0004
#define COUNTER0_STATUS			0x000C
#define TGU_STATUS			0x0014
#define TIMER0_COMPARE_STEP(n)		(0x0040 + 0x1D8 * (n))
#define COUNTER0_COMPARE_STEP(n)	(0x0048 + 0x1D8 * (n))
#define GROUP_REG_STEP(grp, reg, step)	(0x0074 + 0x60 * (grp) + 0x4 * (reg) + \
								 0x1D8 * (step))
#define CONDITION_DECODE_STEP(m, n)	(0x0050 + 0x4 * (m) + 0x1D8 * (n))
#define CONDITION_SELECT_STEP(m, n)	(0x0060 + 0x4 * (m) + 0x1D8 * (n))
#define GROUP0				0x0074
#define GROUP1				0x00D4
#define GROUP2				0x0134
#define GROUP3				0x0194
#define TGU_LAR				0x0FB0

#define MAX_GROUP_SETS			256
#define MAX_GROUPS			4
#define MAX_CONDITION_SETS		64
#define MAX_TIMER_COUNTER_SETS		8

#define to_tgu_drvdata(c)		container_of(c, struct tgu_drvdata, tgu)

struct Trigger_group_data {
	unsigned long grpaddr;
	unsigned long value;
};

struct Trigger_condition_data {
	unsigned long condaddr;
	unsigned long value;
};

struct Trigger_select_data {
	unsigned long selectaddr;
	unsigned long value;
};

struct Trigger_timer_data {
	unsigned long timeraddr;
	unsigned long value;
};

struct Trigger_counter_data {
	unsigned long counteraddr;
	unsigned long value;
};
struct tgu_drvdata {
	void __iomem			*base;
	struct device			*dev;
	struct coresight_device		*csdev;
	struct clk			*clk;
	struct mutex			mutex;
	resource_size_t			size;
	bool				removing;
	int				max_steps;
	int				max_conditions;
	int				max_regs;
	int				max_timer_counter;
	struct Trigger_group_data	*grp_data;
	struct Trigger_condition_data	*condition_data;
	struct Trigger_select_data	*select_data;
	struct Trigger_timer_data	*timer_data;
	struct Trigger_counter_data	*counter_data;
	int				grp_refcnt;
	int				cond_refcnt;
	int				select_refcnt;
	int				timer_refcnt;
	int				counter_refcnt;
	bool				enable;
};

DEFINE_CORESIGHT_DEVLIST(tgu_devs, "tgu");

static ssize_t enable_tgu_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret = 0, i, j;

	if (kstrtoul(buf, 16, &value) || value > 1)
		return -EINVAL;
	mutex_lock(&drvdata->mutex);
	if (drvdata->removing) {
		ret = -ENODEV;
		goto out;
	}
	if (!value && !drvdata->enable)
		goto out;
	if (value) {
		if (!drvdata->enable) {
			ret = pm_runtime_resume_and_get(drvdata->dev);
			if (ret < 0)
				goto out;
			ret = coresight_claim_device(drvdata->csdev);
			if (ret) {
				pm_runtime_put(drvdata->dev);
				goto out;
			}
		}
		TGU_UNLOCK(drvdata);
		tgu_writel(drvdata, 0, TGU_CONTROL);
		for (i = 0; i < drvdata->grp_refcnt; i++)
			tgu_writel(drvdata, drvdata->grp_data[i].value, drvdata->grp_data[i].grpaddr);
		for (i = 0; i < drvdata->max_conditions; i++)
			for (j = 0; j < drvdata->max_steps; j++)
				tgu_writel(drvdata, 0x1000000, CONDITION_DECODE_STEP(i, j));
		for (i = 0; i < drvdata->cond_refcnt; i++)
			tgu_writel(drvdata, drvdata->condition_data[i].value, drvdata->condition_data[i].condaddr);
		for (i = 0; i < drvdata->select_refcnt; i++)
			tgu_writel(drvdata, drvdata->select_data[i].value, drvdata->select_data[i].selectaddr);
		for (i = 0; i < drvdata->timer_refcnt; i++)
			tgu_writel(drvdata, drvdata->timer_data[i].value, drvdata->timer_data[i].timeraddr);
		for (i = 0; i < drvdata->counter_refcnt; i++)
			tgu_writel(drvdata, drvdata->counter_data[i].value, drvdata->counter_data[i].counteraddr);
		tgu_writel(drvdata, 1, TGU_CONTROL);
		TGU_LOCK(drvdata);
		drvdata->enable = true;
	} else {
		TGU_UNLOCK(drvdata);
		tgu_writel(drvdata, 0, TGU_CONTROL);
		coresight_disclaim_device_unlocked(drvdata->csdev);
		TGU_LOCK(drvdata);
		drvdata->enable = false;
		pm_runtime_put(drvdata->dev);
	}
out:
	mutex_unlock(&drvdata->mutex);
	return ret < 0 ? ret : (ssize_t)size;
}
static DEVICE_ATTR_WO(enable_tgu);

static ssize_t reset_tgu_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct tgu_drvdata *d = dev_get_drvdata(dev->parent);
	unsigned long value;
	int ret;

	if (kstrtoul(buf, 16, &value) || value != 1)
		return -EINVAL;
	mutex_lock(&d->mutex);
	if (d->removing) {
		ret = -ENODEV;
		goto out;
	}
	if (!d->enable) {
		ret = pm_runtime_resume_and_get(d->dev);
		if (ret < 0)
			goto out;
		ret = coresight_claim_device(d->csdev);
		if (ret) {
			pm_runtime_put(d->dev);
			goto out;
		}
	}
	TGU_UNLOCK(d);
	tgu_writel(d, 0, TGU_CONTROL);
	coresight_disclaim_device_unlocked(d->csdev);
	TGU_LOCK(d);
	d->enable = false;
	d->grp_refcnt = d->cond_refcnt = d->select_refcnt = 0;
	d->timer_refcnt = d->counter_refcnt = 0;
	memset(d->grp_data, 0, sizeof(*d->grp_data) * MAX_GROUP_SETS);
	memset(d->condition_data, 0, sizeof(*d->condition_data) * MAX_CONDITION_SETS);
	memset(d->select_data, 0, sizeof(*d->select_data) * MAX_CONDITION_SETS);
	memset(d->timer_data, 0, sizeof(*d->timer_data) * MAX_TIMER_COUNTER_SETS);
	memset(d->counter_data, 0, sizeof(*d->counter_data) * MAX_TIMER_COUNTER_SETS);
	pm_runtime_put(d->dev);
	ret = 0;
out:
	mutex_unlock(&d->mutex);
	return ret < 0 ? ret : (ssize_t)size;
}
static DEVICE_ATTR_WO(reset_tgu);

static ssize_t set_group_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	int grp, reg, step;
	unsigned long value;
	int ret = 0;

	if (sscanf(buf, "%d %d %d %lx", &grp, &reg, &step, &value) != 4 || value > U32_MAX ||
	    grp < 0 || grp >= MAX_GROUPS || reg < 0 || reg >= drvdata->max_regs || step < 0 || step >= drvdata->max_steps)
		return -EINVAL;
	mutex_lock(&drvdata->mutex);
	if (drvdata->removing)
		ret = -ENODEV;
	else if (drvdata->grp_refcnt >= MAX_GROUP_SETS)
		ret = -ENOSPC;
	else {
		drvdata->grp_data[drvdata->grp_refcnt].grpaddr = GROUP_REG_STEP(grp, reg, step);
		drvdata->grp_data[drvdata->grp_refcnt].value = value;
		drvdata->grp_refcnt++;
	}
	mutex_unlock(&drvdata->mutex);
	return ret ? ret : (ssize_t)size;
}
static DEVICE_ATTR_WO(set_group);

static ssize_t set_condition_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	int cond, step;
	unsigned long value;
	int ret = 0;

	if (sscanf(buf, "%d %d %lx", &cond, &step, &value) != 3 || value > U32_MAX ||
	    cond < 0 || cond >= drvdata->max_conditions || step < 0 || step >= drvdata->max_steps)
		return -EINVAL;
	mutex_lock(&drvdata->mutex);
	if (drvdata->removing)
		ret = -ENODEV;
	else if (drvdata->cond_refcnt >= MAX_CONDITION_SETS)
		ret = -ENOSPC;
	else {
		drvdata->condition_data[drvdata->cond_refcnt].condaddr = CONDITION_DECODE_STEP(cond, step);
		drvdata->condition_data[drvdata->cond_refcnt].value = value;
		drvdata->cond_refcnt++;
	}
	mutex_unlock(&drvdata->mutex);
	return ret ? ret : (ssize_t)size;
}
static DEVICE_ATTR_WO(set_condition);

static ssize_t set_select_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	int select, step;
	unsigned long value;
	int ret = 0;

	if (sscanf(buf, "%d %d %lx", &select, &step, &value) != 3 || value > U32_MAX ||
	    select < 0 || select >= drvdata->max_conditions || step < 0 || step >= drvdata->max_steps)
		return -EINVAL;
	mutex_lock(&drvdata->mutex);
	if (drvdata->removing)
		ret = -ENODEV;
	else if (drvdata->select_refcnt >= MAX_CONDITION_SETS)
		ret = -ENOSPC;
	else {
		drvdata->select_data[drvdata->select_refcnt].selectaddr = CONDITION_SELECT_STEP(select, step);
		drvdata->select_data[drvdata->select_refcnt].value = value;
		drvdata->select_refcnt++;
	}
	mutex_unlock(&drvdata->mutex);
	return ret ? ret : (ssize_t)size;
}
static DEVICE_ATTR_WO(set_select);

static ssize_t set_timer_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	int step;
	unsigned long value;
	int ret = 0;

	if (sscanf(buf, "%d %lx", &step, &value) != 2 || value > U32_MAX ||
	    step < 0 || step >= drvdata->max_timer_counter)
		return -EINVAL;
	mutex_lock(&drvdata->mutex);
	if (drvdata->removing)
		ret = -ENODEV;
	else if (drvdata->timer_refcnt >= MAX_TIMER_COUNTER_SETS)
		ret = -ENOSPC;
	else {
		drvdata->timer_data[drvdata->timer_refcnt].timeraddr = TIMER0_COMPARE_STEP(step);
		drvdata->timer_data[drvdata->timer_refcnt].value = value;
		drvdata->timer_refcnt++;
	}
	mutex_unlock(&drvdata->mutex);
	return ret ? ret : (ssize_t)size;
}
static DEVICE_ATTR_WO(set_timer);

static ssize_t set_counter_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct tgu_drvdata *drvdata = dev_get_drvdata(dev->parent);
	int step;
	unsigned long value;
	int ret = 0;

	if (sscanf(buf, "%d %lx", &step, &value) != 2 || value > U32_MAX ||
	    step < 0 || step >= drvdata->max_timer_counter)
		return -EINVAL;
	mutex_lock(&drvdata->mutex);
	if (drvdata->removing)
		ret = -ENODEV;
	else if (drvdata->counter_refcnt >= MAX_TIMER_COUNTER_SETS)
		ret = -ENOSPC;
	else {
		drvdata->counter_data[drvdata->counter_refcnt].counteraddr = COUNTER0_COMPARE_STEP(step);
		drvdata->counter_data[drvdata->counter_refcnt].value = value;
		drvdata->counter_refcnt++;
	}
	mutex_unlock(&drvdata->mutex);
	return ret ? ret : (ssize_t)size;
}
static DEVICE_ATTR_WO(set_counter);

static struct attribute *tgu_attrs[] = {
	&dev_attr_enable_tgu.attr,
	&dev_attr_reset_tgu.attr,
	&dev_attr_set_group.attr,
	&dev_attr_set_condition.attr,
	&dev_attr_set_select.attr,
	&dev_attr_set_timer.attr,
	&dev_attr_set_counter.attr,
	NULL,
};

static struct attribute_group tgu_attr_grp = {
	.attrs = tgu_attrs,
};

static const struct attribute_group *tgu_attr_grps[] = {
	&tgu_attr_grp,
	NULL,
};

static int tgu_validate_layout(struct tgu_drvdata *d)
{
	if (d->size < 0x1000 || d->max_steps <= 0 || d->max_steps > 8 ||
	    d->max_conditions <= 0 || d->max_conditions > 4 ||
	    d->max_regs <= 0 || d->max_regs > 24 ||
	    d->max_timer_counter <= 0 || d->max_timer_counter > MAX_TIMER_COUNTER_SETS)
		return -EINVAL;
	/* The last programming address must stay below integration/management. */
	if (GROUP_REG_STEP(MAX_GROUPS - 1, d->max_regs - 1, d->max_steps - 1) >= 0xF00)
		return -EINVAL;
	return 0;
}

/* TGU is armed through its control attributes, not through a trace path. */
static int tgu_path_enable(struct coresight_device *csdev, enum cs_mode mode, void *data)
{
	return -EOPNOTSUPP;
}

static int tgu_path_disable(struct coresight_device *csdev, void *data)
{
	return -EOPNOTSUPP;
}

static const struct coresight_ops_helper tgu_helper_ops = {
	.enable = tgu_path_enable,
	.disable = tgu_path_disable,
};
static const struct coresight_ops tgu_ops = { .helper_ops = &tgu_helper_ops };

static int tgu_probe(struct amba_device *adev, const struct amba_id *id)
{
	int ret = 0;
	struct device *dev = &adev->dev;
	struct coresight_platform_data *pdata;
	struct tgu_drvdata *drvdata;
	struct coresight_desc desc = { 0 };

	if (device_property_read_string(dev, "coresight-name", &desc.name))
		desc.name = coresight_alloc_device_name(&tgu_devs, dev);
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
	drvdata->size = resource_size(&adev->res);

	mutex_init(&drvdata->mutex);

	ret = of_property_read_u32(adev->dev.of_node, "tgu-steps",
						&drvdata->max_steps);
	if (ret)
		return -EINVAL;

	ret = of_property_read_u32(adev->dev.of_node, "tgu-conditions",
						&drvdata->max_conditions);
	if (ret)
		return -EINVAL;

	ret = of_property_read_u32(adev->dev.of_node, "tgu-regs",
							&drvdata->max_regs);
	if (ret)
		return -EINVAL;

	ret = of_property_read_u32(adev->dev.of_node, "tgu-timer-counters",
						&drvdata->max_timer_counter);
	if (ret)
		return -EINVAL;

	ret = tgu_validate_layout(drvdata);
	if (ret)
		return ret;

	/* Alloc memory for Grps, Conditions and Steps */
	drvdata->grp_data = devm_kzalloc(dev, MAX_GROUP_SETS *
				       sizeof(*drvdata->grp_data),
				       GFP_KERNEL);
	if (!drvdata->grp_data)
		return -ENOMEM;

	drvdata->condition_data = devm_kzalloc(dev, MAX_CONDITION_SETS *
				       sizeof(*drvdata->condition_data),
				       GFP_KERNEL);

	if (!drvdata->condition_data)
		return -ENOMEM;

	drvdata->select_data = devm_kzalloc(dev, MAX_CONDITION_SETS *
				       sizeof(*drvdata->select_data),
				       GFP_KERNEL);
	if (!drvdata->select_data)
		return -ENOMEM;

	drvdata->timer_data = devm_kzalloc(dev, MAX_TIMER_COUNTER_SETS *
				       sizeof(*drvdata->timer_data),
				       GFP_KERNEL);
	if (!drvdata->timer_data)
		return -ENOMEM;

	drvdata->counter_data = devm_kzalloc(dev, MAX_TIMER_COUNTER_SETS *
				       sizeof(*drvdata->counter_data),
				       GFP_KERNEL);
	if (!drvdata->counter_data)
		return -ENOMEM;

	drvdata->enable = false;

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.subtype.helper_subtype = CORESIGHT_DEV_SUBTYPE_HELPER_QCOM_TGU;
	desc.ops = &tgu_ops;
	desc.access = CSDEV_ACCESS_IOMEM(drvdata->base);
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.groups = tgu_attr_grps;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		goto err;
	}

	pm_runtime_put(&adev->dev);
	dev_dbg(dev, "TGU initialized\n");
	return 0;
err:
	return ret;
}

static void tgu_remove(struct amba_device *adev)
{
	struct tgu_drvdata *d = dev_get_drvdata(&adev->dev);

	sysfs_remove_groups(&d->csdev->dev.kobj, tgu_attr_grps);
	mutex_lock(&d->mutex);
	d->removing = true;
	if (d->enable) {
		TGU_UNLOCK(d);
		tgu_writel(d, 0, TGU_CONTROL);
		coresight_disclaim_device_unlocked(d->csdev);
		TGU_LOCK(d);
		d->enable = false;
		pm_runtime_put(d->dev);
	}
	mutex_unlock(&d->mutex);
	coresight_unregister(d->csdev);
}

static struct amba_id tgu_ids[] = {
	{
		.id	=	0x0003b999,
		.mask	=	0x0003ffff,
		.data	=	"TGU",
	},
	{ 0, 0},
};

MODULE_DEVICE_TABLE(amba, tgu_ids);

static struct amba_driver tgu_driver = {
	.drv = {
		.name			=	"coresight-tgu",
		.owner			=	THIS_MODULE,
		.suppress_bind_attrs	=	true,
	},
	.probe		=	tgu_probe,
	.id_table	=	tgu_ids,
	.remove		=	tgu_remove,
};

module_amba_driver(tgu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight TGU driver");
