// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#include <linux/amba/bus.h>
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/coresight.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/spinlock.h>
#include <linux/pinctrl/consumer.h>
#include <linux/suspend.h>

#include "coresight-priv.h"
#include "coresight-cti.h"

/*
 * CTI devices can be associated with a PE, or be connected to CoreSight
 * hardware. We have a list of all CTIs irrespective of CPU bound or
 * otherwise.
 *
 * We assume that the non-CPU CTIs are always powered as we do with sinks etc.
 *
 * We leave the client to figure out if all the CTIs are interconnected with
 * the same CTM, in general this is the case but does not always have to be.
 */

/* net of CTI devices connected via CTM */
static LIST_HEAD(ect_net);

/* protect the list */
static DEFINE_MUTEX(ect_mutex);

#define csdev_to_cti_drvdata(csdev)	\
	dev_get_drvdata(csdev->dev.parent)

/* power management handling */
static int nr_cti_cpu;
static DEFINE_MUTEX(cti_pm_mutex);
static DEFINE_RAW_SPINLOCK(cti_cpu_lock);

/* quick lookup list for CPU bound CTIs when power handling */
static struct cti_drvdata *cti_cpu_drvdata[NR_CPUS];

/*
 * CTI naming. CTI bound to cores will have the name cti_cpu<N> where
 * N is the CPU ID. System CTIs will have the name cti_sys<I> where I
 * is an index allocated by order of discovery.
 *
 * CTI device name list - for CTI not bound to cores.
 */
DEFINE_CORESIGHT_DEVLIST(cti_sys_devs, "cti_sys");

/* write set of regs to hardware - call with spinlock claimed */
void cti_write_all_hw_regs(struct cti_drvdata *drvdata)
{
	struct cti_config *config = &drvdata->config;
	int i;

	CS_UNLOCK(drvdata->base);

	/* disable CTI before writing registers */
	writel_relaxed(0, drvdata->base + CTICONTROL);

	/* write the CTI trigger registers */
	if (drvdata->extended_cti) {
		for (i = 0; i < config->nr_trig_max; i++) {
			writel_relaxed(config->ctiinen[i], drvdata->base + CTIINEN_EXTENDED(i));
			writel_relaxed(config->ctiouten[i],
				      drvdata->base + CTIOUTEN_EXTENDED(i));
		}

		/* other regs */
		writel_relaxed(config->ctigate, drvdata->base + CTIGATE_EXTENDED);
		writel_relaxed(config->asicctl, drvdata->base + ASICCTL_EXTENDED);
		writel_relaxed(config->ctiappset, drvdata->base + CTIAPPSET_EXTENDED);
	} else {
		for (i = 0; i < config->nr_trig_max; i++) {
			writel_relaxed(config->ctiinen[i], drvdata->base + CTIINEN(i));
			writel_relaxed(config->ctiouten[i],
					      drvdata->base + CTIOUTEN(i));
		}

		/* other regs */
		writel_relaxed(config->ctigate, drvdata->base + CTIGATE);
		writel_relaxed(config->asicctl, drvdata->base + ASICCTL);
		writel_relaxed(config->ctiappset, drvdata->base + CTIAPPSET);
	}

	/* re-enable CTI */
	writel_relaxed(1, drvdata->base + CTICONTROL);

	CS_LOCK(drvdata->base);
}

/* write regs to hardware and enable */
static int cti_enable_hw(struct cti_drvdata *drvdata)
{
	struct cti_config *config = &drvdata->config;
	unsigned long flags;
	int rc = 0;

	raw_spin_lock_irqsave(&drvdata->spinlock, flags);

	if (drvdata->removing) {
		rc = -ENODEV;
		goto cti_err_not_enabled;
	}
	if (config->enable_req_count == INT_MAX) {
		rc = -EOVERFLOW;
		goto cti_err_not_enabled;
	}
	/* no need to do anything if enabled or unpowered*/
	if (config->hw_enabled || !config->hw_powered)
		goto cti_state_unchanged;

	if (!drvdata->extended_cti) {
		/* claim the device */
		rc = coresight_claim_device(drvdata->csdev);
		if (rc)
			goto cti_err_not_enabled;
	}

	cti_write_all_hw_regs(drvdata);

	config->hw_enabled = true;
	drvdata->config.enable_req_count++;
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
	return rc;

cti_state_unchanged:
	drvdata->config.enable_req_count++;

	/* cannot enable due to error */
cti_err_not_enabled:
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
	return rc;
}

/* re-enable CTI on CPU when using CPU hotplug */
static void cti_cpuhp_enable_hw(struct cti_drvdata *drvdata)
{
	unsigned long cti_irqflags;
	struct cti_config *config = &drvdata->config;

	raw_spin_lock_irqsave(&drvdata->spinlock, cti_irqflags);
	config->hw_powered = true;

	/* no need to do anything if no enable request */
	if (!drvdata->config.enable_req_count)
		goto cti_hp_not_enabled;

	/* try to claim the device */
	if (!drvdata->extended_cti && coresight_claim_device(drvdata->csdev))
		goto cti_hp_not_enabled;

	cti_write_all_hw_regs(drvdata);
	config->hw_enabled = true;
	raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);
	return;

	/* did not re-enable due to no claim / no request */
cti_hp_not_enabled:
	raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);
}

/* disable hardware */
static int cti_disable_hw(struct cti_drvdata *drvdata)
{
	unsigned long cti_irqflags;
	struct cti_config *config = &drvdata->config;
	struct coresight_device *csdev = drvdata->csdev;
	int ret = 0;

	raw_spin_lock_irqsave(&drvdata->spinlock, cti_irqflags);

	/* don't allow negative refcounts, return an error */
	if (!drvdata->config.enable_req_count) {
		ret = -EINVAL;
		goto cti_not_disabled;
	}

	/* check refcount - disable on 0 */
	if (--drvdata->config.enable_req_count > 0)
		goto cti_not_disabled;

	/* no need to do anything if disabled or cpu unpowered */
	if (!config->hw_enabled || !config->hw_powered)
		goto cti_not_disabled;

	CS_UNLOCK(drvdata->base);

	/* disable CTI */
	writel_relaxed(0, drvdata->base + CTICONTROL);
	config->hw_enabled = false;

	if (!drvdata->extended_cti)
		coresight_disclaim_device_unlocked(csdev);
	CS_LOCK(drvdata->base);
	raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);
	return ret;

	/* not disabled this call */
cti_not_disabled:
	raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);
	return ret;
}

void cti_write_single_reg(struct cti_drvdata *drvdata, int offset, u32 value)
{
	CS_UNLOCK(drvdata->base);
	writel_relaxed(value, drvdata->base + offset);
	CS_LOCK(drvdata->base);
}

int cti_write_intack(struct device *dev, u32 ackval)
{
	struct cti_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long flags;
	int ret = 0;

	/* The native scalar ABI acknowledges only trigger bank zero. */
	if (!drvdata->extended_cti && drvdata->config.nr_trig_max < 32 &&
	    (ackval & ~GENMASK(drvdata->config.nr_trig_max - 1, 0)))
		return -EINVAL;
	raw_spin_lock_irqsave(&drvdata->spinlock, flags);
	if (!cti_active(&drvdata->config) || drvdata->removing)
		ret = -EHOSTDOWN;
	else
		cti_write_single_reg(drvdata, drvdata->extended_cti ?
			CTIINTACK_EXTENDED(0) : CTIINTACK, ackval);
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
	return ret;
}

/*
 * Look at the HW DEVID register for some of the HW settings.
 * DEVID[15:8] - max number of in / out triggers.
 */
#define CTI_DEVID_MAXTRIGS(devid_val) ((int) BMVAL(devid_val, 8, 15))

/* DEVID[19:16] - number of CTM channels */
#define CTI_DEVID_CTMCHANNELS(devid_val) ((int) BMVAL(devid_val, 16, 19))

static int cti_set_default_config(struct cti_drvdata *drvdata)
{
	struct cti_config *config = &drvdata->config;
	u32 devid;
	int max_trigs = drvdata->extended_cti ? CTIINOUTEN_MAX : 32;

	if (drvdata->regs_size < 0x1000)
		return -EINVAL;
	devid = readl_relaxed(drvdata->base + CORESIGHT_DEVID);
	config->nr_trig_max = CTI_DEVID_MAXTRIGS(devid);
	config->nr_ctm_channels = CTI_DEVID_CTMCHANNELS(devid);
	if (!config->nr_trig_max || config->nr_trig_max > max_trigs ||
	    !config->nr_ctm_channels)
		return -EINVAL;
	config->trig_filter_enable = true;
	config->ctigate = cti_channel_mask(config);
	config->enable_req_count = 0;
	return 0;
}

/*
 * Add a connection entry to the list of connections for this
 * CTI device.
 */
int cti_add_connection_entry(struct device *dev, struct cti_drvdata *drvdata,
			     struct cti_trig_con *tc,
			     struct coresight_device *csdev,
			     const char *assoc_dev_name)
{
	struct cti_device *cti_dev = &drvdata->ctidev;

	tc->con_dev = csdev;
	/*
	 * Prefer actual associated CS device dev name to supplied value -
	 * which is likely to be node name / other conn name.
	 */
	if (csdev) {
		tc->con_dev_name = devm_kstrdup(dev, dev_name(&csdev->dev), GFP_KERNEL);
		tc->con_node_name = devm_kstrdup(dev,
			cti_plat_get_node_name(dev_fwnode(csdev->dev.parent)), GFP_KERNEL);
		if (!tc->con_dev_name || !tc->con_node_name)
			return -ENOMEM;
	} else if (assoc_dev_name != NULL) {
		tc->con_dev_name = devm_kstrdup(dev,
						assoc_dev_name, GFP_KERNEL);
		if (!tc->con_dev_name)
			return -ENOMEM;
		tc->con_node_name = tc->con_dev_name;
	}
	list_add_tail(&tc->node, &cti_dev->trig_cons);
	cti_dev->nr_trig_con++;

	/* add connection usage bit info to overall info */
	bitmap_or(drvdata->config.trig_in_use, drvdata->config.trig_in_use,
			tc->con_in->used_mask, drvdata->config.nr_trig_max);
	bitmap_or(drvdata->config.trig_out_use, drvdata->config.trig_out_use,
			tc->con_out->used_mask, drvdata->config.nr_trig_max);

	return 0;
}

/* create a trigger connection with appropriately sized signal groups */
struct cti_trig_con *cti_allocate_trig_con(struct device *dev, int in_sigs,
					   int out_sigs)
{
	struct cti_trig_con *tc = NULL;
	struct cti_trig_grp *in = NULL, *out = NULL;

	if (in_sigs < 0 || out_sigs < 0 || in_sigs > CTIINOUTEN_MAX ||
	    out_sigs > CTIINOUTEN_MAX)
		return NULL;
	tc = devm_kzalloc(dev, sizeof(struct cti_trig_con), GFP_KERNEL);
	if (!tc)
		return tc;

	in = devm_kzalloc(dev,
			  struct_size(in, sig_types, in_sigs),
			  GFP_KERNEL);
	if (!in)
		return NULL;

	out = devm_kzalloc(dev,
			   struct_size(out, sig_types, out_sigs),
			   GFP_KERNEL);
	if (!out)
		return NULL;

	tc->con_in = in;
	tc->con_out = out;
	tc->con_in->nr_sigs = in_sigs;
	tc->con_out->nr_sigs = out_sigs;
	return tc;
}

/*
 * Add a default connection if nothing else is specified.
 * single connection based on max in/out info, no assoc device
 */
int cti_add_default_connection(struct device *dev, struct cti_drvdata *drvdata)
{
	int ret = 0;
	int n_trigs = drvdata->config.nr_trig_max;
	struct cti_trig_con *tc = NULL;

	/*
	 * Assume max trigs for in and out,
	 * all used, default sig types allocated
	 */
	if (n_trigs <= 0 || n_trigs > CTIINOUTEN_MAX)
		return -EINVAL;
	tc = cti_allocate_trig_con(dev, n_trigs, n_trigs);
	if (!tc)
		return -ENOMEM;

	if (n_trigs > CTIINOUTEN_MAX)
		return -EINVAL;

	bitmap_fill(tc->con_in->used_mask, n_trigs);
	bitmap_fill(tc->con_out->used_mask, n_trigs);
	ret = cti_add_connection_entry(dev, drvdata, tc, NULL, "default");
	return ret;
}

static int cti_trigin_gpio_enable(struct cti_drvdata *drvdata)
{
	int ret;
	struct pinctrl *pctrl;
	struct pinctrl_state *pctrl_state;

	if (drvdata->gpio_trigin->pctrl)
		return 0;

	pctrl = devm_pinctrl_get(drvdata->csdev->dev.parent);
	if (IS_ERR(pctrl)) {
		dev_err(&drvdata->csdev->dev, "pinctrl get failed\n");
		return PTR_ERR(pctrl);
	}

	pctrl_state = pinctrl_lookup_state(pctrl, "cti-trigin-pctrl");
	if (IS_ERR(pctrl_state)) {
		dev_err(&drvdata->csdev->dev,
			"pinctrl get state failed\n");
		ret = PTR_ERR(pctrl_state);
		goto err;
	}

	ret = pinctrl_select_state(pctrl, pctrl_state);
	if (ret) {
		dev_err(&drvdata->csdev->dev,
			"pinctrl enable state failed\n");
		goto err;
	}

	drvdata->gpio_trigin->pctrl = pctrl;
	return 0;
err:
	devm_pinctrl_put(pctrl);
	return ret;
}

static int cti_trigout_gpio_enable(struct cti_drvdata *drvdata)
{
	int ret;
	struct pinctrl *pctrl;
	struct pinctrl_state *pctrl_state;

	if (drvdata->gpio_trigout->pctrl)
		return 0;

	pctrl = devm_pinctrl_get(drvdata->csdev->dev.parent);
	if (IS_ERR(pctrl)) {
		dev_err(&drvdata->csdev->dev, "pinctrl get failed\n");
		return PTR_ERR(pctrl);
	}

	pctrl_state = pinctrl_lookup_state(pctrl, "cti-trigout-pctrl");
	if (IS_ERR(pctrl_state)) {
		dev_err(&drvdata->csdev->dev,
			"pinctrl get state failed\n");
		ret = PTR_ERR(pctrl_state);
		goto err;
	}

	ret = pinctrl_select_state(pctrl, pctrl_state);
	if (ret) {
		dev_err(&drvdata->csdev->dev,
			"pinctrl enable state failed\n");
		goto err;
	}

	drvdata->gpio_trigout->pctrl = pctrl;
	return 0;
err:
	devm_pinctrl_put(pctrl);
	return ret;
}

void cti_trigin_gpio_disable(struct cti_drvdata *drvdata)
{
	if (!drvdata->gpio_trigin->pctrl)
		return;

	devm_pinctrl_put(drvdata->gpio_trigin->pctrl);
	drvdata->gpio_trigin->pctrl = NULL;
}

void cti_trigout_gpio_disable(struct cti_drvdata *drvdata)
{
	if (!drvdata->gpio_trigout->pctrl)
		return;

	devm_pinctrl_put(drvdata->gpio_trigout->pctrl);
	drvdata->gpio_trigout->pctrl = NULL;
}

/** cti channel api **/
/* attach/detach channel from trigger - write through if enabled. */
int cti_channel_trig_op(struct device *dev, enum cti_chan_op op,
			enum cti_trig_dir direction, u32 channel_idx,
			u32 trigger_idx)
{
	struct cti_drvdata *drvdata = dev_get_drvdata(dev->parent);
	struct cti_config *config = &drvdata->config;
	struct cti_pctrl *gpio;
	unsigned long flags;
	u32 value, offset;
	int ret = 0;

	if ((op != CTI_CHAN_ATTACH && op != CTI_CHAN_DETACH) ||
	    (direction != CTI_TRIG_IN && direction != CTI_TRIG_OUT) ||
	    channel_idx >= (u32)config->nr_ctm_channels ||
	    trigger_idx >= (u32)config->nr_trig_max)
		return -EINVAL;
	mutex_lock(&drvdata->gpio_mutex);
	raw_spin_lock_irqsave(&drvdata->spinlock, flags);
	if (drvdata->removing) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!test_bit(trigger_idx, direction == CTI_TRIG_IN ?
		config->trig_in_use : config->trig_out_use) ||
	    (direction == CTI_TRIG_OUT && config->trig_filter_enable &&
	     test_bit(trigger_idx, config->trig_out_filter))) {
		ret = -EINVAL;
		goto unlock;
	}
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
	gpio = direction == CTI_TRIG_IN ? drvdata->gpio_trigin : drvdata->gpio_trigout;
	if (op == CTI_CHAN_ATTACH && gpio->trig >= 0 && (u32)gpio->trig == trigger_idx) {
		ret = direction == CTI_TRIG_IN ? cti_trigin_gpio_enable(drvdata) :
			cti_trigout_gpio_enable(drvdata);
		if (ret)
			goto mutex_unlock;
	}
	raw_spin_lock_irqsave(&drvdata->spinlock, flags);
	value = direction == CTI_TRIG_IN ? config->ctiinen[trigger_idx] :
		config->ctiouten[trigger_idx];
	if (op == CTI_CHAN_ATTACH)
		value |= BIT(channel_idx);
	else
		value &= ~BIT(channel_idx);
	if (direction == CTI_TRIG_IN) {
		config->ctiinen[trigger_idx] = value;
		offset = drvdata->extended_cti ? CTIINEN_EXTENDED(trigger_idx) : CTIINEN(trigger_idx);
	} else {
		config->ctiouten[trigger_idx] = value;
		offset = drvdata->extended_cti ? CTIOUTEN_EXTENDED(trigger_idx) : CTIOUTEN(trigger_idx);
	}
	if (cti_active(config))
		cti_write_single_reg(drvdata, offset, value);
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
	if (op == CTI_CHAN_DETACH && !value && gpio->trig >= 0 && (u32)gpio->trig == trigger_idx) {
		if (direction == CTI_TRIG_IN)
			cti_trigin_gpio_disable(drvdata);
		else
			cti_trigout_gpio_disable(drvdata);
	}
	goto mutex_unlock;
unlock:
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
mutex_unlock:
	mutex_unlock(&drvdata->gpio_mutex);
	return ret;
}

int cti_channel_gate_op(struct device *dev, enum cti_chan_gate_op op,
			u32 channel_idx)
{
	unsigned long cti_irqflags;
	struct cti_drvdata *drvdata = dev_get_drvdata(dev->parent);
	struct cti_config *config = &drvdata->config;
	u32 chan_bitmask;
	u32 reg_value;
	int err = 0;

	if (channel_idx >= (u32)config->nr_ctm_channels)
		return -EINVAL;

	chan_bitmask = BIT(channel_idx);

	raw_spin_lock_irqsave(&drvdata->spinlock, cti_irqflags);
	reg_value = config->ctigate;
	switch (op) {
	case CTI_GATE_CHAN_ENABLE:
		reg_value |= chan_bitmask;
		break;

	case CTI_GATE_CHAN_DISABLE:
		reg_value &= ~chan_bitmask;
		break;

	default:
		err = -EINVAL;
		break;
	}
	if (err == 0) {
		config->ctigate = reg_value;
		if (cti_active(config)) {
			if (drvdata->extended_cti)
				cti_write_single_reg(drvdata, CTIGATE_EXTENDED, reg_value);
			else
				cti_write_single_reg(drvdata, CTIGATE, reg_value);
		}
	}
	raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);
	return err;
}

int cti_channel_setop(struct device *dev, enum cti_chan_set_op op,
		      u32 channel_idx)
{
	unsigned long cti_irqflags;
	struct cti_drvdata *drvdata = dev_get_drvdata(dev->parent);
	struct cti_config *config = &drvdata->config;
	u32 chan_bitmask;
	u32 reg_value;
	u32 reg_offset;
	int err = 0;

	if (channel_idx >= (u32)config->nr_ctm_channels)
		return -EINVAL;

	chan_bitmask = BIT(channel_idx);

	raw_spin_lock_irqsave(&drvdata->spinlock, cti_irqflags);
	reg_value = config->ctiappset;
	switch (op) {
	case CTI_CHAN_SET:
		config->ctiappset |= chan_bitmask;
		reg_value  = config->ctiappset;
		if (drvdata->extended_cti)
			reg_offset = CTIAPPSET_EXTENDED;
		else
			reg_offset = CTIAPPSET;
		break;

	case CTI_CHAN_CLR:
		config->ctiappset &= ~chan_bitmask;
		reg_value = chan_bitmask;
		if (drvdata->extended_cti)
			reg_offset = CTIAPPCLEAR_EXTENDED;
		else
			reg_offset = CTIAPPCLEAR;
		break;

	case CTI_CHAN_PULSE:
		config->ctiappset &= ~chan_bitmask;
		reg_value = chan_bitmask;
		if (drvdata->extended_cti)
			reg_offset = CTIAPPPULSE_EXTENDED;
		else
			reg_offset = CTIAPPPULSE;
		break;

	default:
		err = -EINVAL;
		break;
	}

	if ((err == 0) && cti_active(config))
		cti_write_single_reg(drvdata, reg_offset, reg_value);
	raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);

	return err;
}

static bool cti_add_sysfs_link(struct cti_drvdata *drvdata,
			       struct cti_trig_con *tc)
{
	struct coresight_sysfs_link link_info;
	int link_err = 0;

	link_info.orig = drvdata->csdev;
	link_info.orig_name = tc->con_dev_name;
	link_info.target = tc->con_dev;
	link_info.target_name = dev_name(&drvdata->csdev->dev);

	link_err = coresight_add_sysfs_link(&link_info);
	if (link_err)
		dev_warn(&drvdata->csdev->dev,
			 "Failed to set CTI sysfs link %s<=>%s\n",
			 link_info.orig_name, link_info.target_name);
	return !link_err;
}

static void cti_remove_sysfs_link(struct cti_drvdata *drvdata,
				  struct cti_trig_con *tc)
{
	struct coresight_sysfs_link link_info;

	link_info.orig = drvdata->csdev;
	link_info.orig_name = tc->con_dev_name;
	link_info.target = tc->con_dev;
	link_info.target_name = dev_name(&drvdata->csdev->dev);
	coresight_remove_sysfs_link(&link_info);
}

/*
 * Look for a matching connection device name in the list of connections.
 * If found then swap in the csdev name, set trig con association pointer
 * and return found.
 */
static bool
cti_match_fixup_csdev(struct cti_device *ctidev, const char *node_name,
		      struct coresight_device *csdev)
{
	struct cti_trig_con *tc;
	struct cti_drvdata *drvdata = container_of(ctidev, struct cti_drvdata,
						   ctidev);

	list_for_each_entry(tc, &ctidev->trig_cons, node) {
		if (tc->con_dev_name) {
			if (!tc->con_dev && !strcmp(node_name, tc->con_node_name)) {
				/* match: so swap in csdev name & dev */
				tc->con_dev_name = devm_kstrdup(drvdata->csdev->dev.parent,
					dev_name(&csdev->dev), GFP_KERNEL);
				if (!tc->con_dev_name) {
					tc->con_dev_name = tc->con_node_name;
					return false;
				}
				tc->con_dev = csdev;
				/* try to set sysfs link */
				if (cti_add_sysfs_link(drvdata, tc))
					return true;
				/* link failed - remove CTI reference */
				tc->con_dev = NULL;
				tc->con_dev_name = tc->con_node_name;
				break;
			}
		}
	}
	return false;
}

/*
 * Search the cti list to add an associated CTI into the supplied CS device
 * This will set the association if CTI declared before the CS device.
 * (called from coresight_register() without coresight_mutex locked).
 */
static void cti_add_assoc_to_csdev(struct coresight_device *csdev)
{
	struct cti_drvdata *ect_item;
	struct cti_device *ctidev;
	const char *node_name = NULL;

	/* protect the list */
	mutex_lock(&ect_mutex);

	/* exit if current is an ECT device.*/
	if ((csdev->type == CORESIGHT_DEV_TYPE_HELPER &&
	     csdev->subtype.helper_subtype ==
		     CORESIGHT_DEV_SUBTYPE_HELPER_ECT_CTI) ||
	    list_empty(&ect_net))
		goto cti_add_done;

	/* if we didn't find the csdev previously we used the fwnode name */
	node_name = cti_plat_get_node_name(dev_fwnode(csdev->dev.parent));
	if (!node_name)
		goto cti_add_done;

	/* for each CTI in list... */
	list_for_each_entry(ect_item, &ect_net, node) {
		ctidev = &ect_item->ctidev;
		if (cti_match_fixup_csdev(ctidev, node_name, csdev)) {
			/*
			 * if we found a matching csdev then update the ECT
			 * association pointer for the device with this CTI.
			 */
			coresight_add_helper(csdev, ect_item->csdev);
			break;
		}
	}
cti_add_done:
	mutex_unlock(&ect_mutex);
}

/*
 * Removing the associated devices is easier.
 */
static void cti_remove_assoc_from_csdev(struct coresight_device *csdev)
{
	struct cti_drvdata *drvdata;
	struct cti_trig_con *tc;

	mutex_lock(&ect_mutex);
	list_for_each_entry(drvdata, &ect_net, node) {
		list_for_each_entry(tc, &drvdata->ctidev.trig_cons, node) {
			if (tc->con_dev == csdev) {
				cti_remove_sysfs_link(drvdata, tc);
				tc->con_dev = NULL;
				tc->con_dev_name = tc->con_node_name;
			}
		}
	}
	mutex_unlock(&ect_mutex);
}

/*
 * Operations to add and remove associated CTI.
 * Register to coresight core driver as call back function.
 */
static struct cti_assoc_op cti_assoc_ops = {
	.add = cti_add_assoc_to_csdev,
	.remove = cti_remove_assoc_from_csdev
};

/*
 * Update the cross references where the associated device was found
 * while we were building the connection info. This will occur if the
 * assoc device was registered before the CTI.
 */
static void cti_update_conn_xrefs(struct cti_drvdata *drvdata)
{
	struct cti_trig_con *tc;
	struct cti_device *ctidev = &drvdata->ctidev;

	list_for_each_entry(tc, &ctidev->trig_cons, node) {
		if (tc->con_dev) {
			/* if we can set the sysfs link */
			if (cti_add_sysfs_link(drvdata, tc))
				/* set the CTI/csdev association */
				coresight_add_helper(tc->con_dev,
						     drvdata->csdev);
			else {
				/* otherwise remove reference from CTI */
				tc->con_dev = NULL;
				tc->con_dev_name = tc->con_node_name;
			}
		}
	}
}

static void cti_remove_conn_xrefs(struct cti_drvdata *drvdata)
{
	struct cti_trig_con *tc;
	struct cti_device *ctidev = &drvdata->ctidev;

	list_for_each_entry(tc, &ctidev->trig_cons, node) {
		if (tc->con_dev) {
			cti_remove_sysfs_link(drvdata, tc);
			tc->con_dev = NULL;
		}
	}
}

/** cti PM callbacks **/
static int cti_cpu_pm_notify(struct notifier_block *nb, unsigned long cmd,
			     void *v)
{
	unsigned long cti_irqflags;
	unsigned long cti_cpuflags;
	struct cti_drvdata *drvdata;
	struct coresight_device *csdev;
	unsigned int cpu = smp_processor_id();
	int notify_res = NOTIFY_OK;

	raw_spin_lock_irqsave(&cti_cpu_lock, cti_cpuflags);
	drvdata = cti_cpu_drvdata[cpu];
	if (!drvdata) {
		raw_spin_unlock_irqrestore(&cti_cpu_lock, cti_cpuflags);
		return NOTIFY_OK;
	}
	csdev = drvdata->csdev;

	if (WARN_ON_ONCE(drvdata->ctidev.cpu != cpu)) {
		raw_spin_unlock_irqrestore(&cti_cpu_lock, cti_cpuflags);
		return NOTIFY_BAD;
	}

	raw_spin_lock_irqsave(&drvdata->spinlock, cti_irqflags);

	switch (cmd) {
	case CPU_PM_ENTER:
		/* CTI regs all static - we have a copy & nothing to save */
		drvdata->config.hw_powered = false;
		if (drvdata->config.hw_enabled && !drvdata->extended_cti)
			coresight_disclaim_device(csdev);
		break;

	case CPU_PM_ENTER_FAILED:
		drvdata->config.hw_powered = true;
		if (drvdata->config.hw_enabled) {
			if (!drvdata->extended_cti && coresight_claim_device(csdev))
				drvdata->config.hw_enabled = false;
		}
		break;

	case CPU_PM_EXIT:
		/* write hardware registers to re-enable. */
		drvdata->config.hw_powered = true;
		drvdata->config.hw_enabled = false;

		/* check enable reference count to enable HW */
		if (drvdata->config.enable_req_count) {
			/* check we can claim the device as we re-power */
			if (!drvdata->extended_cti && coresight_claim_device(csdev))
				goto cti_notify_exit;

			drvdata->config.hw_enabled = true;
			cti_write_all_hw_regs(drvdata);
		}
		break;

	default:
		notify_res = NOTIFY_DONE;
		break;
	}

cti_notify_exit:
	raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);
	raw_spin_unlock_irqrestore(&cti_cpu_lock, cti_cpuflags);
	return notify_res;
}

static struct notifier_block cti_cpu_pm_nb = {
	.notifier_call = cti_cpu_pm_notify,
};

/* CPU HP handlers */
static int cti_starting_cpu(unsigned int cpu)
{
	unsigned long cti_cpuflags;
	struct cti_drvdata *drvdata;

	raw_spin_lock_irqsave(&cti_cpu_lock, cti_cpuflags);
	drvdata = cti_cpu_drvdata[cpu];
	if (drvdata)
		cti_cpuhp_enable_hw(drvdata);
	raw_spin_unlock_irqrestore(&cti_cpu_lock, cti_cpuflags);
	return 0;
}

static int cti_dying_cpu(unsigned int cpu)
{
	unsigned long cti_irqflags;
	unsigned long cti_cpuflags;
	struct cti_drvdata *drvdata;

	raw_spin_lock_irqsave(&cti_cpu_lock, cti_cpuflags);
	drvdata = cti_cpu_drvdata[cpu];
	if (drvdata) {
		raw_spin_lock_irqsave(&drvdata->spinlock, cti_irqflags);
		drvdata->config.hw_powered = false;
		if (drvdata->config.hw_enabled && !drvdata->extended_cti)
			coresight_disclaim_device(drvdata->csdev);
		raw_spin_unlock_irqrestore(&drvdata->spinlock, cti_irqflags);
	}
	raw_spin_unlock_irqrestore(&cti_cpu_lock, cti_cpuflags);
	return 0;
}

static int cti_pm_setup(struct cti_drvdata *drvdata)
{
	int cpu = drvdata->ctidev.cpu, ret = 0;
	unsigned long flags;

	if (cpu == -1)
		return 0;
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return -EINVAL;
	mutex_lock(&cti_pm_mutex);
	cpus_read_lock();
	if (cti_cpu_drvdata[cpu]) {
		ret = -EBUSY;
		goto out;
	}
	if (!nr_cti_cpu) {
		ret = cpuhp_setup_state_nocalls_cpuslocked(
			CPUHP_AP_ARM_CORESIGHT_CTI_STARTING,
			"arm/coresight_cti:starting", cti_starting_cpu, cti_dying_cpu);
		if (ret)
			goto out;
		ret = cpu_pm_register_notifier(&cti_cpu_pm_nb);
		if (ret) {
			cpuhp_remove_state_nocalls_cpuslocked(CPUHP_AP_ARM_CORESIGHT_CTI_STARTING);
			goto out;
		}
	}
	raw_spin_lock_irqsave(&cti_cpu_lock, flags);
	cti_cpu_drvdata[cpu] = drvdata;
	if (cpu_online(cpu))
		cti_cpuhp_enable_hw(drvdata);
	drvdata->pm_registered = true;
	nr_cti_cpu++;
	raw_spin_unlock_irqrestore(&cti_cpu_lock, flags);
out:
	cpus_read_unlock();
	mutex_unlock(&cti_pm_mutex);
	return ret;
}

/* release PM registrations */
static void cti_pm_release(struct cti_drvdata *drvdata)
{
	unsigned long flags;

	mutex_lock(&cti_pm_mutex);
	if (!drvdata->pm_registered)
		goto out;
	cpus_read_lock();
	/* Wait for any CPU callback before detaching the devm allocation. */
	raw_spin_lock_irqsave(&cti_cpu_lock, flags);
	cti_cpu_drvdata[drvdata->ctidev.cpu] = NULL;
	drvdata->pm_registered = false;
	nr_cti_cpu--;
	raw_spin_unlock_irqrestore(&cti_cpu_lock, flags);
	if (!nr_cti_cpu) {
		cpu_pm_unregister_notifier(&cti_cpu_pm_nb);
		cpuhp_remove_state_nocalls_cpuslocked(CPUHP_AP_ARM_CORESIGHT_CTI_STARTING);
	}
	cpus_read_unlock();
out:
	mutex_unlock(&cti_pm_mutex);
}

/** cti ect operations **/
int cti_enable(struct coresight_device *csdev, enum cs_mode mode, void *data)
{
	struct cti_drvdata *drvdata = csdev_to_cti_drvdata(csdev);

	return cti_enable_hw(drvdata);
}

int cti_disable(struct coresight_device *csdev, void *data)
{
	struct cti_drvdata *drvdata = csdev_to_cti_drvdata(csdev);

	return cti_disable_hw(drvdata);
}

static const struct coresight_ops_helper cti_ops_ect = {
	.enable = cti_enable,
	.disable = cti_disable,
};

static const struct coresight_ops cti_ops = {
	.helper_ops = &cti_ops_ect,
};

static void cti_remove(struct amba_device *adev)
{
	struct cti_drvdata *drvdata = dev_get_drvdata(&adev->dev);
	unsigned long flags;
	int ret = pm_runtime_resume_and_get(&adev->dev);

	/* Drain sysfs before devm data and module text can disappear. */
	sysfs_remove_groups(&drvdata->csdev->dev.kobj, drvdata->ctidev.con_groups);
	cti_pm_release(drvdata);
	mutex_lock(&drvdata->gpio_mutex);
	raw_spin_lock_irqsave(&drvdata->spinlock, flags);
	drvdata->removing = true;
	if (ret >= 0 && cti_active(&drvdata->config)) {
		CS_UNLOCK(drvdata->base);
		writel_relaxed(0, drvdata->base + CTICONTROL);
		if (!drvdata->extended_cti)
			coresight_disclaim_device_unlocked(drvdata->csdev);
		CS_LOCK(drvdata->base);
	}
	drvdata->config.hw_enabled = false;
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
	cti_trigin_gpio_disable(drvdata);
	cti_trigout_gpio_disable(drvdata);
	mutex_unlock(&drvdata->gpio_mutex);
	mutex_lock(&ect_mutex);
	list_del_init(&drvdata->node);
	cti_remove_conn_xrefs(drvdata);
	mutex_unlock(&ect_mutex);
	coresight_unregister(drvdata->csdev);
	if (ret >= 0)
		pm_runtime_put(&adev->dev);
}

static bool is_extended_cti(struct device *dev)
{
	return fwnode_property_present(dev->fwnode, "qcom,extended_cti");
}

static int cti_parse_gpio(struct cti_drvdata *drvdata, struct amba_device *adev)
{
	int ret;
	int trig;

	drvdata->gpio_trigin = devm_kzalloc(&adev->dev,
			sizeof(struct cti_pctrl), GFP_KERNEL);
	if (!drvdata->gpio_trigin)
		return -ENOMEM;

	drvdata->gpio_trigin->trig = -1;
	ret = of_property_read_u32(adev->dev.of_node,
			"qcom,cti-gpio-trigin", &trig);
	if (!ret) {
		if (trig < 0 || trig >= drvdata->config.nr_trig_max)
			return -EINVAL;
		drvdata->gpio_trigin->trig = trig;
	} else if (ret != -EINVAL)
		return ret;

	drvdata->gpio_trigout = devm_kzalloc(&adev->dev,
			sizeof(struct cti_pctrl), GFP_KERNEL);
	if (!drvdata->gpio_trigout)
		return -ENOMEM;

	drvdata->gpio_trigout->trig = -1;
	ret = of_property_read_u32(adev->dev.of_node,
			"qcom,cti-gpio-trigout", &trig);

	if (!ret) {
		if (trig < 0 || trig >= drvdata->config.nr_trig_max)
			return -EINVAL;
		drvdata->gpio_trigout->trig = trig;
	} else if (ret != -EINVAL)
		return ret;

	return 0;
}

static void cti_disable_atclk(void *data)
{
	struct cti_drvdata *drvdata = data;

	if (drvdata->atclk_enabled) {
		clk_disable_unprepare(drvdata->atclk);
		drvdata->atclk_enabled = false;
	}
}

static int cti_probe(struct amba_device *adev, const struct amba_id *id)
{
	int ret = 0;
	void __iomem *base;
	struct device *dev = &adev->dev;
	struct cti_drvdata *drvdata = NULL;
	struct coresight_desc cti_desc = {};
	struct coresight_platform_data *pdata = NULL;
	struct resource *res = &adev->res;

	/* driver data*/
	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->atclk = devm_clk_get_optional(dev, "atclk"); /* optional */
	if (IS_ERR(drvdata->atclk)) {
		ret = PTR_ERR(drvdata->atclk);
		dev_err(dev, "enable/get atclk fail, ret = %d\n", ret);
		return ret == -ETIMEDOUT ? -EPROBE_DEFER : ret;
	}

	ret = clk_prepare_enable(drvdata->atclk);
	if (ret)
		return ret;
	drvdata->atclk_enabled = true;
	ret = devm_add_action_or_reset(dev, cti_disable_atclk, drvdata);
	if (ret)
		return ret;

	/* Validity for the resource is already checked by the AMBA core */
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	drvdata->base = base;
	drvdata->regs_size = resource_size(res);
	drvdata->extended_cti = is_extended_cti(dev);
	INIT_LIST_HEAD(&drvdata->node);
	mutex_init(&drvdata->gpio_mutex);
	cti_desc.access = CSDEV_ACCESS_IOMEM(base);

	dev_set_drvdata(dev, drvdata);

	/* default CTI device info  */
	drvdata->ctidev.cpu = -1;
	drvdata->ctidev.nr_trig_con = 0;
	drvdata->ctidev.ctm_id = 0;
	INIT_LIST_HEAD(&drvdata->ctidev.trig_cons);

	raw_spin_lock_init(&drvdata->spinlock);

	/* initialise CTI driver config values */
	ret = cti_set_default_config(drvdata);
	if (ret)
		return ret;

	pdata = coresight_cti_get_platform_data(dev);
	if (IS_ERR(pdata)) {
		dev_err(dev, "coresight_cti_get_platform_data err\n");
		return PTR_ERR(pdata);
	}
	/* CPU CTIs remain unpowered until synchronized PM publication. */
	drvdata->config.hw_powered = drvdata->ctidev.cpu < 0;

	/* set up device name - will depend if cpu bound or otherwise */
	if (device_property_present(dev, "coresight-name")) {
		ret = device_property_read_string(dev, "coresight-name", &cti_desc.name);
		if (ret || !cti_desc.name[0])
			return -EINVAL;
	} else if (drvdata->ctidev.cpu >= 0)
		cti_desc.name = devm_kasprintf(dev, GFP_KERNEL, "cti_cpu%d",
					       drvdata->ctidev.cpu);
	else
		cti_desc.name = coresight_alloc_device_name(&cti_sys_devs, dev);
	if (!cti_desc.name)
		return -ENOMEM;

	ret = cti_parse_gpio(drvdata, adev);
	if (ret)
		return ret;

	/* create dynamic attributes for connections */
	ret = cti_create_cons_sysfs(dev, drvdata);
	if (ret) {
		dev_err(dev, "%s: create dynamic sysfs entries failed\n",
			cti_desc.name);
		goto pm_release;
	}

	/* set up coresight component description */
	cti_desc.pdata = pdata;
	cti_desc.type = CORESIGHT_DEV_TYPE_HELPER;
	cti_desc.subtype.helper_subtype = CORESIGHT_DEV_SUBTYPE_HELPER_ECT_CTI;
	cti_desc.ops = &cti_ops;
	cti_desc.groups = drvdata->ctidev.con_groups;
	cti_desc.dev = dev;
	drvdata->csdev = coresight_register(&cti_desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		goto pm_release;
	}

	ret = cti_pm_setup(drvdata);
	if (ret) {
		coresight_unregister(drvdata->csdev);
		return ret;
	}

	/* add to list of CTI devices */
	mutex_lock(&ect_mutex);
	list_add(&drvdata->node, &ect_net);
	/* set any cross references */
	cti_update_conn_xrefs(drvdata);
	mutex_unlock(&ect_mutex);

	/* all done - dec pm refcount */
	pm_runtime_put(&adev->dev);
	dev_info(&drvdata->csdev->dev, "CTI initialized\n");
	return 0;

pm_release:
	cti_pm_release(drvdata);
	return ret;
}

static int cti_runtime_suspend(struct device *dev)
{
	struct cti_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata)
		cti_disable_atclk(drvdata);
	return 0;
}

static int cti_runtime_resume(struct device *dev)
{
	struct cti_drvdata *drvdata = dev_get_drvdata(dev);
	int ret;

	if (!drvdata || drvdata->atclk_enabled)
		return 0;
	ret = clk_prepare_enable(drvdata->atclk);
	if (ret)
		return ret;
	drvdata->atclk_enabled = true;
	return 0;
}

static void cti_restore_hw(struct cti_drvdata *drvdata)
{
	if (drvdata->ctidev.cpu < 0) {
		cti_cpuhp_enable_hw(drvdata);
		return;
	}
	cpus_read_lock();
	if (cpu_online(drvdata->ctidev.cpu))
		cti_cpuhp_enable_hw(drvdata);
	cpus_read_unlock();
}

/* Preserve client enable votes across system sleep. */
static int cti_suspend(struct device *dev)
{
	struct cti_drvdata *drvdata = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&drvdata->spinlock, flags);
	if (cti_active(&drvdata->config)) {
		CS_UNLOCK(drvdata->base);
		writel_relaxed(0, drvdata->base + CTICONTROL);
		if (!drvdata->extended_cti)
			coresight_disclaim_device_unlocked(drvdata->csdev);
		CS_LOCK(drvdata->base);
	}
	drvdata->config.hw_enabled = false;
	drvdata->config.hw_powered = false;
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);
	ret = pm_runtime_force_suspend(dev);

	if (ret)
		cti_restore_hw(drvdata);
	return ret;
}

static int cti_resume(struct device *dev)
{
	struct cti_drvdata *drvdata = dev_get_drvdata(dev);
	int ret = pm_runtime_force_resume(dev);

	if (ret)
		return ret;
	cti_restore_hw(drvdata);
	return 0;
}

static const struct dev_pm_ops cti_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cti_suspend, cti_resume)
	SET_RUNTIME_PM_OPS(cti_runtime_suspend, cti_runtime_resume, NULL)
};

static struct amba_cs_uci_id uci_id_cti[] = {
	{
		/*  CTI UCI data */
		.devarch	= 0x47701a14, /* CTI v2 */
		.devarch_mask	= 0xfff0ffff,
		.devtype	= 0x00000014, /* maj(0x4-debug) min(0x1-ECT) */
	}
};

static const struct amba_id cti_ids[] = {
	CS_AMBA_ID(0x000bb906), /* Coresight CTI (SoC 400), C-A72, C-A57 */
	CS_AMBA_ID(0x000bb922), /* CTI - C-A8 */
	CS_AMBA_ID(0x000bb9a8), /* CTI - C-A53 */
	CS_AMBA_ID(0x000bb9aa), /* CTI - C-A73 */
	CS_AMBA_UCI_ID(0x000bb9da, uci_id_cti), /* CTI - C-A35 */
	CS_AMBA_UCI_ID(0x000bb9ed, uci_id_cti), /* Coresight CTI (SoC 600) */
	{ 0, 0},
};

MODULE_DEVICE_TABLE(amba, cti_ids);

static struct amba_driver cti_driver = {
	.drv = {
		.name	= "coresight-cti",
		.owner = THIS_MODULE,
		.pm     = pm_ptr(&cti_dev_pm_ops),
		.suppress_bind_attrs = true,
	},
	.probe		= cti_probe,
	.remove		= cti_remove,
	.id_table	= cti_ids,
};

static int __init cti_init(void)
{
	int ret;

	coresight_set_cti_ops(&cti_assoc_ops);
	ret = amba_driver_register(&cti_driver);
	if (ret)
		coresight_remove_cti_ops();
	return ret;
}

static void __exit cti_exit(void)
{
	coresight_remove_cti_ops();
	amba_driver_unregister(&cti_driver);
}

module_init(cti_init);
module_exit(cti_exit);

MODULE_AUTHOR("Mike Leach <mike.leach@linaro.org>");
MODULE_DESCRIPTION("Arm CoreSight CTI Driver");
MODULE_LICENSE("GPL v2");
