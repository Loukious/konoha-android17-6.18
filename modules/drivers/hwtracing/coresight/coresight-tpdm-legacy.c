// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
/* Pinned Qualcomm v1 BC/TC/GPR sequences and bounded configuration. */
#define tpdm_writel(d, val, off) writel_relaxed(val, (d)->base + (off))
#define tpdm_readl(d, off) readl_relaxed((d)->base + (off))
static void __tpdm_config_bc_msr(struct tpdm_drvdata *drvdata)
{
	int i;

	if (!drvdata->msr_support)
		return;

	for (i = 0; i < TPDM_BC_MAX_MSR; i++)
		tpdm_writel(drvdata, drvdata->bc->msr[i], TPDM_BC_MSR(i));
}
static void __tpdm_config_tc_msr(struct tpdm_drvdata *drvdata)
{
	int i;

	if (!drvdata->msr_support)
		return;

	for (i = 0; i < TPDM_TC_MAX_MSR; i++)
		tpdm_writel(drvdata, drvdata->tc->msr[i], TPDM_TC_MSR(i));
}
static void __tpdm_enable_bc(struct tpdm_drvdata *drvdata)
{
	int i;
	uint32_t val;

	if (drvdata->bc->sat_mode)
		tpdm_writel(drvdata, drvdata->bc->sat_mode, TPDM_BC_SATROLL);
	else
		tpdm_writel(drvdata, 0x0, TPDM_BC_SATROLL);

	{
		tpdm_writel(drvdata, 0xFFFFFFFF, TPDM_BC_CNTENCLR);
		tpdm_writel(drvdata, drvdata->bc->enable_counters, TPDM_BC_CNTENSET);
	}
	if (drvdata->bc->clear_counters)
		tpdm_writel(drvdata, drvdata->bc->clear_counters, TPDM_BC_CNTENCLR);

	{
		tpdm_writel(drvdata, 0xFFFFFFFF, TPDM_BC_INTENCLR);
		tpdm_writel(drvdata, drvdata->bc->enable_irq, TPDM_BC_INTENSET);
	}
	if (drvdata->bc->clear_irq)
		tpdm_writel(drvdata, drvdata->bc->clear_irq, TPDM_BC_INTENCLR);

	if (drvdata->bc_trig_type == TPDM_SUPPORT_TYPE_FULL) {
		for (i = 0; i < drvdata->bc_counters_avail; i++) {
			tpdm_writel(drvdata, drvdata->bc->trig_val_lo[i], TPDM_BC_TRIG_LO(i));
			tpdm_writel(drvdata, drvdata->bc->trig_val_hi[i], TPDM_BC_TRIG_HI(i));
		}
	} else if (drvdata->bc_trig_type == TPDM_SUPPORT_TYPE_PARTIAL) {
		tpdm_writel(drvdata, drvdata->bc->trig_val_lo[0], TPDM_BC_TRIG_LO(0));
		tpdm_writel(drvdata, drvdata->bc->trig_val_hi[0], TPDM_BC_TRIG_HI(0));
	}

	if (drvdata->bc_gang_type != TPDM_SUPPORT_TYPE_NO)
		tpdm_writel(drvdata, drvdata->bc->enable_ganging, TPDM_BC_GANG);

	for (i = 0; i < TPDM_BC_MAX_OVERFLOW; i++)
		tpdm_writel(drvdata, drvdata->bc->overflow_val[i], TPDM_BC_OVERFLOW(i));

	__tpdm_config_bc_msr(drvdata);

	val = tpdm_readl(drvdata, TPDM_BC_CR);
	if (drvdata->bc->retrieval_mode == TPDM_MODE_APB)
		val = val | BIT(2);
	else
		val = val & ~BIT(2);
	tpdm_writel(drvdata, val, TPDM_BC_CR);

	val = tpdm_readl(drvdata, TPDM_BC_CR);
	/* Set the enable bit */
	val = val | BIT(0);
	tpdm_writel(drvdata, val, TPDM_BC_CR);
}
static void __tpdm_enable_tc(struct tpdm_drvdata *drvdata)
{
	int i;
	uint32_t val;

	{
		tpdm_writel(drvdata, 0xF, TPDM_TC_CNTENCLR);
		tpdm_writel(drvdata, drvdata->tc->enable_counters, TPDM_TC_CNTENSET);
	}
	if (drvdata->tc->clear_counters)
		tpdm_writel(drvdata, drvdata->tc->clear_counters, TPDM_TC_CNTENCLR);

	{
		tpdm_writel(drvdata, 0xF, TPDM_TC_INTENCLR);
		tpdm_writel(drvdata, drvdata->tc->enable_irq, TPDM_TC_INTENSET);
	}
	if (drvdata->tc->clear_irq)
		tpdm_writel(drvdata, drvdata->tc->clear_irq, TPDM_TC_INTENCLR);

	if (drvdata->tc_trig_type == TPDM_SUPPORT_TYPE_FULL) {
		for (i = 0; i < TPDM_TC_MAX_TRIG; i++) {
			tpdm_writel(drvdata, drvdata->tc->trig_sel[i], TPDM_TC_TRIG_SEL(i));
			tpdm_writel(drvdata, drvdata->tc->trig_val_lo[i], TPDM_TC_TRIG_LO(i));
			tpdm_writel(drvdata, drvdata->tc->trig_val_hi[i], TPDM_TC_TRIG_HI(i));
		}
	} else if (drvdata->tc_trig_type == TPDM_SUPPORT_TYPE_PARTIAL) {
		tpdm_writel(drvdata, drvdata->tc->trig_sel[0], TPDM_TC_TRIG_SEL(0));
		tpdm_writel(drvdata, drvdata->tc->trig_val_lo[0], TPDM_TC_TRIG_LO(0));
		tpdm_writel(drvdata, drvdata->tc->trig_val_hi[0], TPDM_TC_TRIG_HI(0));
	}

	__tpdm_config_tc_msr(drvdata);

	val = tpdm_readl(drvdata, TPDM_TC_CR);
	if (drvdata->tc->sat_mode)
		val = val | BIT(4);
	else
		val = val & ~BIT(4);
	if (drvdata->tc->retrieval_mode == TPDM_MODE_APB)
		val = val | BIT(2);
	else
		val = val & ~BIT(2);
	tpdm_writel(drvdata, val, TPDM_TC_CR);

	val = tpdm_readl(drvdata, TPDM_TC_CR);
	/* Set the enable bit */
	val = val | BIT(0);
	tpdm_writel(drvdata, val, TPDM_TC_CR);
}

static void tpdm_enable_legacy(struct tpdm_drvdata *d)
{
	unsigned int i;
	if (d->bc)
		__tpdm_enable_bc(d);
	if (d->tc)
		__tpdm_enable_tc(d);
	if (d->gpr)
		for (i = 0; i < TPDM_GPR_REGS_MAX; i++)
			if (d->gpr->dirty[i])
				tpdm_writel(d, d->gpr->gp_regs[i], TPDM_GPR_CR(i));
}
static void tpdm_disable_legacy(struct tpdm_drvdata *d)
{
	if (d->bc)
		tpdm_writel(d, tpdm_readl(d, TPDM_BC_CR) & ~BIT(0), TPDM_BC_CR);
	if (d->tc)
		tpdm_writel(d, tpdm_readl(d, TPDM_TC_CR) & ~BIT(0), TPDM_TC_CR);
}

enum legacy_setting {
	BC_ENABLE,
	BC_SAT,
	BC_RETRIEVAL,
	BC_GANG,
	BC_TRIGGER_LO,
	BC_TRIGGER_HI,
	BC_MSR,
	TC_ENABLE,
	TC_SAT,
	TC_RETRIEVAL,
	TC_TRIGGER_SEL,
	TC_TRIGGER_LO,
	TC_TRIGGER_HI,
	TC_MSR,
	GPR_REG,
};
struct tpdm_legacy_attribute {
	struct device_attribute attr;
	enum legacy_setting setting;
	bool array;
};

static u32 *tpdm_legacy_slot(struct tpdm_drvdata *d, enum legacy_setting key, unsigned int index,
			     u32 *mask)
{
	u32 *slot = NULL;
	unsigned int count = 1;
	*mask = U32_MAX;
	if (!d->legacy)
		return NULL;
	switch (key) {
	case BC_ENABLE:
		if (d->bc) {
			slot = &d->bc->enable_counters;
			*mask = U32_MAX >> (32 - d->bc_counters_avail);
		}
		break;
	case BC_SAT:
		if (d->bc) {
			slot = &d->bc->sat_mode;
			*mask = U32_MAX >> (32 - d->bc_counters_avail);
		}
		break;
	case BC_RETRIEVAL:
		if (d->bc) {
			slot = &d->bc->retrieval_mode;
			*mask = 1;
		}
		break;
	case BC_GANG:
		if (d->bc && d->bc_gang_type != TPDM_SUPPORT_TYPE_NO) {
			slot = &d->bc->enable_ganging;
			*mask = U32_MAX >> (32 - d->bc_counters_avail);
		}
		break;
	case BC_TRIGGER_LO:
	case BC_TRIGGER_HI:
		if (d->bc && d->bc_trig_type != TPDM_SUPPORT_TYPE_NO) {
			slot = key == BC_TRIGGER_LO ? d->bc->trig_val_lo : d->bc->trig_val_hi;
			count =
			    d->bc_trig_type == TPDM_SUPPORT_TYPE_FULL ? d->bc_counters_avail : 1;
		}
		break;
	case BC_MSR:
		if (d->bc && d->msr_support) {
			slot = d->bc->msr;
			count = TPDM_BC_MAX_MSR;
		}
		break;
	case TC_ENABLE:
		if (d->tc) {
			slot = &d->tc->enable_counters;
			*mask = (1U << d->tc_counters_avail) - 1;
		}
		break;
	case TC_SAT:
		if (d->tc) {
			slot = &d->tc->sat_mode;
			*mask = 1;
		}
		break;
	case TC_RETRIEVAL:
		if (d->tc) {
			slot = &d->tc->retrieval_mode;
			*mask = 1;
		}
		break;
	case TC_TRIGGER_SEL:
	case TC_TRIGGER_LO:
	case TC_TRIGGER_HI:
		if (d->tc && d->tc_trig_type != TPDM_SUPPORT_TYPE_NO) {
			slot = key == TC_TRIGGER_SEL  ? d->tc->trig_sel
			       : key == TC_TRIGGER_LO ? d->tc->trig_val_lo
						      : d->tc->trig_val_hi;
			count = d->tc_trig_type == TPDM_SUPPORT_TYPE_FULL ? TPDM_TC_MAX_TRIG : 1;
			/* Native selector is a raw u32 register, not a counter-enable mask. */
		}
		break;
	case TC_MSR:
		if (d->tc && d->msr_support) {
			slot = d->tc->msr;
			count = TPDM_TC_MAX_MSR;
		}
		break;
	case GPR_REG:
		if (d->gpr) {
			slot = d->gpr->gp_regs;
			count = TPDM_GPR_REGS_MAX;
		}
		break;
	}
	return slot && index < count ? slot + index : NULL;
}

static ssize_t tpdm_legacy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpdm_drvdata *d = dev_get_drvdata(dev->parent);
	struct tpdm_legacy_attribute *a = container_of(attr, struct tpdm_legacy_attribute, attr);
	u32 *slot, mask;
	unsigned int i;
	ssize_t len = 0;
	guard(spinlock)(&d->spinlock);
	for (i = 0; (slot = tpdm_legacy_slot(d, a->setting, i, &mask)); i++) {
		if (a->array)
			len += sysfs_emit_at(buf, len, "%u 0x%08x\n", i, *slot);
		else {
			len = sysfs_emit(buf, "0x%x\n", *slot);
			break;
		}
	}
	return len ? len : -EOPNOTSUPP;
}
static ssize_t tpdm_legacy_store(struct device *dev, struct device_attribute *attr, const char *buf,
				 size_t size)
{
	struct tpdm_drvdata *d = dev_get_drvdata(dev->parent);
	struct tpdm_legacy_attribute *a = container_of(attr, struct tpdm_legacy_attribute, attr);
	unsigned int index = 0;
	u32 value, mask, *slot;
	char extra, index_text[20], value_text[20];
	if (a->array) {
		if (sscanf(buf, "%19s %19s %c", index_text, value_text, &extra) != 2 ||
		    kstrtou32(index_text, 10, &index) || kstrtou32(value_text, 16, &value))
			return -EINVAL;
	} else if (kstrtou32(buf, 0, &value))
		return -EINVAL;
	guard(spinlock)(&d->spinlock);
	if (d->enable)
		return -EBUSY;
	slot = tpdm_legacy_slot(d, a->setting, index, &mask);
	if (!slot)
		return -EOPNOTSUPP;
	if (value & ~mask)
		return -ERANGE;
	*slot = value;
	if (a->setting == GPR_REG)
		d->gpr->dirty[index] = true;
	return size;
}
#define LEGACY_ATTR(name, key, indexed)                                                            \
	static struct tpdm_legacy_attribute legacy_##name = {                                      \
	    .attr = __ATTR(name, 0644, tpdm_legacy_show, tpdm_legacy_store),                       \
	    .setting = key,                                                                        \
	    .array = indexed}
LEGACY_ATTR(bc_enable_counters, BC_ENABLE, false);
LEGACY_ATTR(bc_sat_mode, BC_SAT, false);
LEGACY_ATTR(bc_retrieval_mode, BC_RETRIEVAL, false);
LEGACY_ATTR(bc_enable_ganging, BC_GANG, false);
LEGACY_ATTR(bc_trig_val_lo, BC_TRIGGER_LO, true);
LEGACY_ATTR(bc_trig_val_hi, BC_TRIGGER_HI, true);
LEGACY_ATTR(bc_msr, BC_MSR, true);
LEGACY_ATTR(tc_enable_counters, TC_ENABLE, false);
LEGACY_ATTR(tc_sat_mode, TC_SAT, false);
LEGACY_ATTR(tc_retrieval_mode, TC_RETRIEVAL, false);
LEGACY_ATTR(tc_trig_sel, TC_TRIGGER_SEL, true);
LEGACY_ATTR(tc_trig_val_lo, TC_TRIGGER_LO, true);
LEGACY_ATTR(tc_trig_val_hi, TC_TRIGGER_HI, true);
LEGACY_ATTR(tc_msr, TC_MSR, true);
LEGACY_ATTR(gp_regs, GPR_REG, true);
static struct attribute *tpdm_legacy_attrs[] = {
    &legacy_bc_enable_counters.attr.attr,
    &legacy_bc_sat_mode.attr.attr,
    &legacy_bc_retrieval_mode.attr.attr,
    &legacy_bc_enable_ganging.attr.attr,
    &legacy_bc_trig_val_lo.attr.attr,
    &legacy_bc_trig_val_hi.attr.attr,
    &legacy_bc_msr.attr.attr,
    &legacy_tc_enable_counters.attr.attr,
    &legacy_tc_sat_mode.attr.attr,
    &legacy_tc_retrieval_mode.attr.attr,
    &legacy_tc_trig_sel.attr.attr,
    &legacy_tc_trig_val_lo.attr.attr,
    &legacy_tc_trig_val_hi.attr.attr,
    &legacy_tc_msr.attr.attr,
    &legacy_gp_regs.attr.attr,
    NULL,
};
static umode_t tpdm_legacy_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct tpdm_drvdata *d = dev_get_drvdata(dev->parent);
	struct tpdm_legacy_attribute *a =
	    container_of(attr, struct tpdm_legacy_attribute, attr.attr);
	u32 mask;
	return tpdm_legacy_slot(d, a->setting, 0, &mask) ? attr->mode : 0;
}
static const struct attribute_group tpdm_legacy_grp = {
    .attrs = tpdm_legacy_attrs,
    .is_visible = tpdm_legacy_visible,
};
