// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2014, 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/reset-controller.h>
#include <linux/of.h>
#include <linux/clk/qcom.h>
#include <linux/clk.h>
#include <linux/interconnect.h>
#include <linux/pm_clock.h>
#include <linux/pm_runtime.h>
#include <linux/mfd/syscon.h>

#include "common.h"
#include "clk-opp.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "reset.h"
#include "gdsc.h"
#include "vdd-level.h"
#include "clk-debug.h"

struct qcom_cc {
	struct qcom_cc_lifetime *life;
	struct qcom_reset_controller reset;
	struct clk_regmap **rclks;
	size_t num_rclks;
	struct clk_hw **clk_hws;
	size_t num_clk_hws;
};

/*
 * This private devres owner deliberately leaves drvdata as qcom_cc_desc.
 * clk-pm and clk_restore_critical_clocks share that established contract.
 */
struct qcom_cc_lifetime {
	struct list_head node;
	struct device *dev;
	const struct qcom_cc_desc *desc;
	struct qcom_cc_desc *runtime_desc;
	struct clk *iface;
	struct icc_path *path;
	bool *runtime_votes;
	bool *proxy_votes;
	unsigned long *flags;
	bool runtime_enabled;
	bool probe_ref;
	bool iface_enabled;
	bool bw_voted;
	bool exposed;
	struct qcom_cc_quarantine *quarantine;
};

/* No descriptor/code pointers or callbacks survive a hardware-release fault. */
struct qcom_cc_quarantine {
	struct list_head node;
	struct device *dev;
	struct icc_path *path;
	int error;
	unsigned int num_regulators;
	struct regulator *regulators[];
};

static LIST_HEAD(qcom_cc_quarantines);

static LIST_HEAD(qcom_cc_lifetimes);
static DEFINE_MUTEX(qcom_cc_lifetime_lock);

static int qcom_cc_unvote(struct qcom_cc_lifetime *life, bool proxy)
{
	const struct qcom_cc_desc *desc = life->desc;
	bool *votes = proxy ? life->proxy_votes : life->runtime_votes;
	struct clk_vdd_class_data data = { };
	int i, ret, first = 0;

	for (i = desc->num_clk_regulators; i-- > 0;) {
		if (!votes[i])
			continue;
		data.vdd_class = desc->clk_regulators[i];
		ret = clk_unvote_vdd_level(&data,
			proxy ? data.vdd_class->num_levels - 1 : 1);
		if (!ret)
			votes[i] = false;
		else if (!first)
			first = ret;
	}
	return first;
}

static int qcom_cc_vote(struct qcom_cc_lifetime *life, bool proxy)
{
	const struct qcom_cc_desc *desc = life->desc;
	bool *votes = proxy ? life->proxy_votes : life->runtime_votes;
	struct clk_vdd_class_data data = { };
	int i, ret;

	for (i = 0; i < desc->num_clk_regulators; i++) {
		data.vdd_class = desc->clk_regulators[i];
		if (!data.vdd_class || votes[i])
			continue;
		ret = clk_vote_vdd_level(&data,
			proxy ? data.vdd_class->num_levels - 1 : 1);
		if (ret)
			return ret;
		votes[i] = true;
	}
	return 0;
}

static int qcom_cc_runtime_resources_put(struct qcom_cc_lifetime *life)
{
	int ret, first = 0;

	if (life->iface_enabled) {
		clk_disable_unprepare(life->iface);
		life->iface_enabled = false;
	}
	if (life->bw_voted) {
		ret = icc_set_bw(life->path, 0, 0);
		if (!ret)
			life->bw_voted = false;
		else
			first = ret;
	}
	ret = qcom_cc_unvote(life, false);
	return first ? first : ret;
}

static int qcom_cc_runtime_resources_get(struct qcom_cc_lifetime *life)
{
	int ret;

	ret = qcom_cc_vote(life, false);
	if (ret)
		return ret;
	if (life->path && !life->bw_voted) {
		ret = icc_set_bw(life->path, 0, 1);
		if (ret)
			return ret;
		life->bw_voted = true;
	}
	if (!life->iface_enabled) {
		ret = clk_prepare_enable(life->iface);
		if (ret)
			return ret;
		life->iface_enabled = true;
	}
	return 0;
}

static void qcom_cc_lifetime_release(struct device *dev, void *res);

static struct qcom_cc_lifetime *qcom_cc_lifetime_find(struct device *dev)
{
	struct qcom_cc_lifetime *life, *found = NULL;

	mutex_lock(&qcom_cc_lifetime_lock);
	list_for_each_entry(life, &qcom_cc_lifetimes, node) {
		if (life->dev == dev) {
			found = life;
			break;
		}
	}
	mutex_unlock(&qcom_cc_lifetime_lock);
	return found;
}

/*
 * An OF handout may outlive forced device teardown. A normal module unload
 * has already excluded module consumer references; other exposed instances
 * keep their consumed hw metadata and are refused on a later probe.
 */
bool qcom_cc_can_restore(struct device *dev)
{
	struct qcom_cc_lifetime *life = qcom_cc_lifetime_find(dev);

	if (!life)
		return false;
	if (!READ_ONCE(life->exposed))
		return true;
	return dev->driver && dev->driver->owner &&
		!module_is_live(dev->driver->owner);
}

static struct qcom_cc_lifetime *
qcom_cc_lifetime_get(struct device *dev, const struct qcom_cc_desc *desc)
{
	struct qcom_cc_lifetime *life, *other;
	struct qcom_cc_quarantine *quarantine;
	size_t num_regulators = 0;
	int i, j, k, ret;

	life = qcom_cc_lifetime_find(dev);
	if (life)
		return life->desc == desc ? life : ERR_PTR(-EBUSY);

	life = devres_alloc(qcom_cc_lifetime_release, sizeof(*life), GFP_KERNEL);
	if (!life)
		return ERR_PTR(-ENOMEM);
	life->dev = dev;
	life->desc = desc;
	life->runtime_votes = kcalloc(desc->num_clk_regulators, sizeof(bool), GFP_KERNEL);
	life->proxy_votes = kcalloc(desc->num_clk_regulators, sizeof(bool), GFP_KERNEL);
	life->flags = kcalloc(desc->num_clks, sizeof(*life->flags), GFP_KERNEL);
	for (i = 0; i < desc->num_clk_regulators; i++)
		num_regulators += desc->clk_regulators[i]->num_regulators;
	life->quarantine = kzalloc(struct_size(life->quarantine, regulators,
					      num_regulators), GFP_KERNEL);
	if (!life->runtime_votes || !life->proxy_votes || !life->flags ||
	    !life->quarantine) {
		ret = -ENOMEM;
		goto free;
	}

	mutex_lock(&qcom_cc_lifetime_lock);
	list_for_each_entry(quarantine, &qcom_cc_quarantines, node) {
		if (quarantine->dev == dev) {
			ret = quarantine->error;
			mutex_unlock(&qcom_cc_lifetime_lock);
			goto free;
		}
	}
	/* A quarantined static template must not be reused on another device. */
	for (i = 0; i < desc->num_clks; i++) {
		if (desc->clks[i] && !desc->clks[i]->hw.init) {
			mutex_unlock(&qcom_cc_lifetime_lock);
			ret = -EBUSY;
			goto free;
		}
	}
	for (i = 0; i < desc->num_clk_hws; i++) {
		if (desc->clk_hws[i] && !desc->clk_hws[i]->init) {
			mutex_unlock(&qcom_cc_lifetime_lock);
			ret = -EBUSY;
			goto free;
		}
	}
	list_for_each_entry(other, &qcom_cc_lifetimes, node) {
		if (other->desc == desc ||
		    (desc->num_clks && other->desc->clks == desc->clks)) {
			mutex_unlock(&qcom_cc_lifetime_lock);
			ret = -EBUSY;
			goto free;
		}
		for (j = 0; j < desc->num_clk_regulators; j++) {
			for (k = 0; k < other->desc->num_clk_regulators; k++) {
				if (desc->clk_regulators[j] != other->desc->clk_regulators[k])
					continue;
				mutex_unlock(&qcom_cc_lifetime_lock);
				ret = -EBUSY;
				goto free;
			}
		}
	}
	list_add(&life->node, &qcom_cc_lifetimes);
	mutex_unlock(&qcom_cc_lifetime_lock);

	for (i = 0; i < desc->num_clks; i++)
		if (desc->clks[i])
			life->flags[i] = desc->clks[i]->flags;

	ret = clk_regulator_init(dev, desc);
	if (ret) {
		mutex_lock(&qcom_cc_lifetime_lock);
		list_del(&life->node);
		mutex_unlock(&qcom_cc_lifetime_lock);
		goto free;
	}
	/* The owner releases explicit regulator handles after clock/PM users. */
	devres_add(dev, life);
	return life;
free:
	kfree(life->quarantine);
	kfree(life->flags);
	kfree(life->proxy_votes);
	kfree(life->runtime_votes);
	devres_free(life);
	return ERR_PTR(ret);
}

int qcom_cc_prepare(struct device *dev, const struct qcom_cc_desc *desc)
{
	struct qcom_cc_lifetime *life = qcom_cc_lifetime_get(dev, desc);

	return IS_ERR(life) ? PTR_ERR(life) : 0;
}

static void qcom_cc_lifetime_release(struct device *dev, void *res)
{
	struct qcom_cc_lifetime *life = res;
	const struct qcom_cc_desc *desc = life->desc;
	struct qcom_cc_quarantine *quarantine = life->quarantine;
	int i, j, ret, error;

	if (life->probe_ref) {
		life->probe_ref = false;
		pm_runtime_put_noidle(dev);
	}
	if (life->runtime_enabled)
		pm_runtime_disable(dev);
	error = qcom_cc_runtime_resources_put(life);
	ret = qcom_cc_unvote(life, true);
	if (!error)
		error = ret;
	if (!qcom_cc_can_restore(dev) && !error)
		error = -EBUSY;
	/* CCF boot holds are drained before unregister; do not lose other votes. */
	for (i = 0; i < desc->num_clk_regulators; i++) {
		if (desc->clk_regulators[i]->cur_level < 0 && !error)
			error = -EIO;
		for (j = 0; j < desc->clk_regulators[i]->num_levels; j++)
			if (desc->clk_regulators[i]->level_votes[j] && !error)
				error = -EBUSY;
	}

	if (error) {
		/*
		 * A void teardown cannot reject a final hardware-release fault.
		 * Retain only framework handles and a device reference. Never keep
		 * leaf metadata/code accessible, or permit a second owner here.
		 */
		/* Keep the refusal registry alive if all leaf modules are unloaded. */
		__module_get(THIS_MODULE);
		quarantine->dev = get_device(dev);
		quarantine->path = life->path;
		quarantine->error = error;
		for (i = 0; i < desc->num_clk_regulators; i++)
			for (j = 0; j < desc->clk_regulators[i]->num_regulators; j++)
				quarantine->regulators[quarantine->num_regulators++] =
					desc->clk_regulators[i]->regulator[j];
		mutex_lock(&qcom_cc_lifetime_lock);
		list_add(&quarantine->node, &qcom_cc_quarantines);
		mutex_unlock(&qcom_cc_lifetime_lock);
		dev_err(dev, "clock release failed (%d); resources retained, reprobe refused\n",
			error);
	} else {
		icc_put(life->path);
		kfree(quarantine);
	}
	clk_put(life->iface);
	if (life->runtime_desc)
		life->runtime_desc->path = NULL;
	if (dev_get_drvdata(dev) == desc)
		dev_set_drvdata(dev, NULL);
	clk_regulator_deinit(desc, !error);
	if (qcom_cc_can_restore(dev))
		for (i = 0; i < desc->num_clks; i++)
			if (desc->clks[i])
				desc->clks[i]->flags = life->flags[i];
	mutex_lock(&qcom_cc_lifetime_lock);
	list_del(&life->node);
	mutex_unlock(&qcom_cc_lifetime_lock);
	kfree(life->flags);
	kfree(life->proxy_votes);
	kfree(life->runtime_votes);
}

static void qcom_cc_release_boot_holds(void *data)
{
	/* CCF owns registration-time boot holds; release them before unregister. */
	clk_sync_state(data);
}

struct qcom_cc_hw_saved {
	struct clk_hw *hw;
	struct device *dev;
	const struct clk_init_data *init;
};

static void qcom_cc_restore_hw(void *data)
{
	struct qcom_cc_hw_saved *saved = data;

	if (!qcom_cc_can_restore(saved->dev))
		return;
	saved->hw->init = saved->init;
	saved->hw->core = NULL;
	saved->hw->clk = NULL;
}

static int qcom_cc_register_hw(struct device *dev, struct clk_hw *hw)
{
	struct qcom_cc_hw_saved *saved;
	int ret;

	if (!hw->init)
		return -EBUSY;
	saved = devm_kzalloc(dev, sizeof(*saved), GFP_KERNEL);
	if (!saved)
		return -ENOMEM;
	saved->hw = hw;
	saved->dev = dev;
	saved->init = hw->init;
	ret = devm_add_action_or_reset(dev, qcom_cc_restore_hw, saved);
	if (ret)
		return ret;
	return devm_clk_hw_register(dev, hw);
}

int qcom_clk_crm_init(struct device *dev, struct clk_crm *crm)
{
	char prop_name[32];

	if (!crm)
		return -EINVAL;

	if (!crm->initialized) {
		snprintf(prop_name, sizeof(prop_name), "qcom,%s-crmc", crm->name);

		if (of_find_property(dev->of_node, prop_name, NULL)) {
			crm->regmap_crmc =
				syscon_regmap_lookup_by_phandle(dev->of_node,
								prop_name);
			if (IS_ERR(crm->regmap_crmc)) {
				dev_err(dev, "%s regmap error\n", prop_name);
				return PTR_ERR(crm->regmap_crmc);
			}
		}

		if (crm->name) {
			crm->dev = crm_get_device(crm->name);
			if (IS_ERR(crm->dev)) {
				pr_err("%s Failed to get crm dev=%s, ret=%ld\n",
				       __func__, crm->name, PTR_ERR(crm->dev));
				return PTR_ERR(crm->dev);
			}
		}

		/*
		 * Until all targets and instances have updated to explicitly
		 * specify this, use the most common default value by default.
		 */
		if (!crm->num_perf_ol)
			crm->num_perf_ol = 8;

		crm->initialized = true;
	}

	return 0;
}
EXPORT_SYMBOL(qcom_clk_crm_init);

static int qcom_find_freq_index(const struct freq_tbl *f, unsigned long rate)
{
	int index;

	for (index = 0; f->freq; f++, index++) {
		if (rate <= f->freq)
			return index;
	}

	return index - 1;
}

int qcom_find_crm_freq_index(const struct freq_tbl *f, unsigned long rate)
{
	if (!f || !f->freq)
		return -EINVAL;

	/*
	 * If rate is 0 return PERF_OL 0 index
	 */
	if (!rate)
		return 0;

	/*
	 * Return PERF_OL index + 1 as PERF_OL 0 is
	 * treated as CLK OFF as per LUT population
	 */
	return qcom_find_freq_index(f, rate) + 1;
}
EXPORT_SYMBOL(qcom_find_crm_freq_index);

const
struct freq_tbl *qcom_find_freq(const struct freq_tbl *f, unsigned long rate)
{
	if (!f)
		return NULL;

	if (!f->freq)
		return f;

	for (; f->freq; f++)
		if (rate <= f->freq)
			return f;

	/* Default to our fastest rate */
	return f - 1;
}
EXPORT_SYMBOL_GPL(qcom_find_freq);

const struct freq_tbl *qcom_find_freq_floor(const struct freq_tbl *f,
					    unsigned long rate)
{
	const struct freq_tbl *best = NULL;

	for ( ; f->freq; f++) {
		if (rate >= f->freq)
			best = f;
		else
			break;
	}

	return best;
}
EXPORT_SYMBOL_GPL(qcom_find_freq_floor);

int qcom_find_src_index(struct clk_hw *hw, const struct parent_map *map, u8 src)
{
	int i, num_parents = clk_hw_get_num_parents(hw);

	for (i = 0; i < num_parents; i++)
		if (src == map[i].src)
			return i;

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(qcom_find_src_index);

int qcom_find_cfg_index(struct clk_hw *hw, const struct parent_map *map, u8 cfg)
{
	int i, num_parents = clk_hw_get_num_parents(hw);

	for (i = 0; i < num_parents; i++)
		if (cfg == map[i].cfg)
			return i;

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(qcom_find_cfg_index);

struct regmap *
qcom_cc_map(struct platform_device *pdev, const struct qcom_cc_desc *desc)
{
	void __iomem *base;
	struct device *dev = &pdev->dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return ERR_CAST(base);

	return devm_regmap_init_mmio(dev, base, desc->config);
}
EXPORT_SYMBOL_GPL(qcom_cc_map);

void
qcom_pll_set_fsm_mode(struct regmap *map, u32 reg, u8 bias_count, u8 lock_count)
{
	u32 val;
	u32 mask;

	/* De-assert reset to FSM */
	regmap_update_bits(map, reg, PLL_VOTE_FSM_RESET, 0);

	/* Program bias count and lock count */
	val = bias_count << PLL_BIAS_COUNT_SHIFT |
		lock_count << PLL_LOCK_COUNT_SHIFT;
	mask = PLL_BIAS_COUNT_MASK << PLL_BIAS_COUNT_SHIFT;
	mask |= PLL_LOCK_COUNT_MASK << PLL_LOCK_COUNT_SHIFT;
	regmap_update_bits(map, reg, mask, val);

	/* Enable PLL FSM voting */
	regmap_update_bits(map, reg, PLL_VOTE_FSM_ENA, PLL_VOTE_FSM_ENA);
}
EXPORT_SYMBOL_GPL(qcom_pll_set_fsm_mode);

static void qcom_cc_gdsc_unregister(void *data)
{
	gdsc_unregister(data);
}

/*
 * Backwards compatibility with old DTs. Register a pass-through factor 1/1
 * clock to translate 'path' clk into 'name' clk and register the 'path'
 * clk as a fixed rate clock if it isn't present.
 */
static int _qcom_cc_register_board_clk(struct device *dev, const char *path,
				       const char *name, unsigned long rate,
				       bool add_factor)
{
	struct device_node *node = NULL;
	struct device_node *clocks_node;
	struct clk_fixed_factor *factor;
	struct clk_fixed_rate *fixed;
	struct clk_init_data init_data = { };
	int ret;

	clocks_node = of_find_node_by_path("/clocks");
	if (clocks_node) {
		node = of_get_child_by_name(clocks_node, path);
		of_node_put(clocks_node);
	}

	if (!node) {
		fixed = devm_kzalloc(dev, sizeof(*fixed), GFP_KERNEL);
		if (!fixed)
			return -EINVAL;

		fixed->fixed_rate = rate;
		fixed->hw.init = &init_data;

		init_data.name = path;
		init_data.ops = &clk_fixed_rate_ops;

		ret = devm_clk_hw_register(dev, &fixed->hw);
		if (ret)
			return ret;
	}
	of_node_put(node);

	if (add_factor) {
		factor = devm_kzalloc(dev, sizeof(*factor), GFP_KERNEL);
		if (!factor)
			return -EINVAL;

		factor->mult = factor->div = 1;
		factor->hw.init = &init_data;

		init_data.name = name;
		init_data.parent_names = &path;
		init_data.num_parents = 1;
		init_data.flags = 0;
		init_data.ops = &clk_fixed_factor_ops;

		ret = devm_clk_hw_register(dev, &factor->hw);
		if (ret)
			return ret;
	}

	return 0;
}

int qcom_cc_register_board_clk(struct device *dev, const char *path,
			       const char *name, unsigned long rate)
{
	bool add_factor = true;

	/*
	 * TODO: The RPM clock driver currently does not support the xo clock.
	 * When xo is added to the RPM clock driver, we should change this
	 * function to skip registration of xo factor clocks.
	 */

	return _qcom_cc_register_board_clk(dev, path, name, rate, add_factor);
}
EXPORT_SYMBOL_GPL(qcom_cc_register_board_clk);

int qcom_cc_register_sleep_clk(struct device *dev)
{
	return _qcom_cc_register_board_clk(dev, "sleep_clk", "sleep_clk_src",
					   32768, true);
}
EXPORT_SYMBOL_GPL(qcom_cc_register_sleep_clk);

/* Drop 'protected-clocks' from the list of clocks to register */
static void qcom_cc_drop_protected(struct device *dev, struct qcom_cc *cc)
{
	struct device_node *np = dev->of_node;
	u32 i;

	of_property_for_each_u32(np, "protected-clocks", i) {
		if (i >= cc->num_rclks)
			continue;

		cc->rclks[i] = NULL;
	}
}

/* Set QCOM_CLK_IS_CRITICAL on clocks specified in dt */
static void qcom_cc_set_critical(struct device *dev, struct qcom_cc *cc)
{
	struct of_phandle_args args;
	struct device_node *np;
	u32 clock_idx;
	u32 phandle;
	int i, cnt;

	of_property_for_each_u32(dev->of_node, "qcom,critical-clocks", clock_idx) {
		if (clock_idx >= cc->num_rclks)
			continue;

		if (cc->rclks[clock_idx])
			cc->rclks[clock_idx]->flags |= QCOM_CLK_IS_CRITICAL;
	}

	of_property_for_each_u32(dev->of_node, "qcom,critical-devices", phandle) {
		for (np = of_find_node_by_phandle(phandle); np;
		     np = of_get_next_parent(np)) {
			cnt = of_count_phandle_with_args(np, "clocks", "#clock-cells");
			for (i = 0; i < cnt; i++) {
				if (of_parse_phandle_with_args(np, "clocks", "#clock-cells",
							      i, &args))
					continue;

				if (args.np == dev->of_node && args.args_count) {
					clock_idx = args.args[0];
					if (clock_idx < cc->num_rclks && cc->rclks[clock_idx])
						cc->rclks[clock_idx]->flags |= QCOM_CLK_IS_CRITICAL;
				}
				of_node_put(args.np);
			}
		}
	}
}

static struct clk_hw *qcom_cc_clk_hw_get(struct of_phandle_args *clkspec,
					 void *data)
{
	struct qcom_cc *cc = data;
	unsigned int idx = clkspec->args[0];

	if (idx < cc->num_clk_hws && cc->clk_hws[idx]) {
		WRITE_ONCE(cc->life->exposed, true);
		return cc->clk_hws[idx];
	}

	if (idx >= cc->num_rclks) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	if (!cc->rclks[idx])
		return NULL;
	WRITE_ONCE(cc->life->exposed, true);
	return &cc->rclks[idx]->hw;
}

int qcom_cc_really_probe(struct platform_device *pdev,
			 const struct qcom_cc_desc *desc, struct regmap *regmap)
{
	int i, ret;
	struct device *dev = &pdev->dev;
	struct qcom_reset_controller *reset;
	struct qcom_cc *cc;
	struct gdsc_desc *scd;
	struct qcom_cc_lifetime *life;
	void *group;
	size_t num_clks = desc->num_clks;
	struct clk_regmap **rclks = desc->clks;
	size_t num_clk_hws = desc->num_clk_hws;
	struct clk_hw **clk_hws = desc->clk_hws;

	life = qcom_cc_lifetime_get(dev, desc);
	if (IS_ERR(life))
		return PTR_ERR(life);
	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc) {
		ret = -ENOMEM;
		goto release_group;
	}

	cc->life = life;
	reset = &cc->reset;
	reset->dev = dev;
	reset->rcdev.of_node = dev->of_node;
	reset->rcdev.ops = &qcom_reset_ops;
	reset->rcdev.owner = dev->driver->owner;
	reset->rcdev.nr_resets = desc->num_resets;
	reset->regmap = regmap;
	reset->reset_map = desc->resets;

	ret = qcom_cc_vote(life, true);
	if (ret)
		goto release_group;

	if (desc->num_resets) {
		ret = devm_reset_controller_register(dev, &reset->rcdev);
		if (ret)
			goto release_group;
	}

	if (desc->gdscs && desc->num_gdscs) {
		scd = devm_kzalloc(dev, sizeof(*scd), GFP_KERNEL);
		if (!scd) {
			ret = -ENOMEM;
			goto release_group;
		}
		scd->dev = dev;
		scd->scs = desc->gdscs;
		scd->num = desc->num_gdscs;
		ret = gdsc_register(scd, &reset->rcdev, regmap);
		if (ret)
			goto release_group;
		ret = devm_add_action_or_reset(dev, qcom_cc_gdsc_unregister,
					       scd);
		if (ret)
			goto release_group;
	}

	cc->rclks = devm_kmemdup(dev, rclks, sizeof(*rclks) * num_clks, GFP_KERNEL);
	if (!cc->rclks && num_clks) {
		ret = -ENOMEM;
		goto release_group;
	}
	rclks = cc->rclks;
	cc->num_rclks = num_clks;
	cc->clk_hws = clk_hws;
	cc->num_clk_hws = num_clk_hws;

	qcom_cc_drop_protected(dev, cc);
	qcom_cc_set_critical(dev, cc);

	for (i = 0; i < num_clk_hws; i++) {
		if (!clk_hws[i])
			continue;

		ret = qcom_cc_register_hw(dev, clk_hws[i]);
		if (ret)
			goto release_group;
	}

	for (i = 0; i < num_clks; i++) {
		if (!rclks[i])
			continue;

		ret = devm_clk_register_regmap(dev, rclks[i]);
		if (ret)
			goto release_group;

		clk_hw_populate_clock_opp_table(dev->of_node, &rclks[i]->hw);

		/*
		 * Critical clocks are enabled by devm_clk_register_regmap()
		 * and registration skipped. So remove from rclks so that the
		 * get() callback returns NULL and client requests are stubbed.
		 */
		if (rclks[i]->flags & QCOM_CLK_IS_CRITICAL)
			rclks[i] = NULL;
	}

	ret = devm_add_action_or_reset(dev, qcom_cc_release_boot_holds, dev);
	if (ret)
		goto release_group;
	ret = devm_of_clk_add_hw_provider(dev, qcom_cc_clk_hw_get, cc);
	if (ret)
		goto release_group;

	devres_remove_group(dev, group);
	return 0;

release_group:
	clk_sync_state(dev);
	devres_release_group(dev, group);
	if (qcom_cc_unvote(life, true))
		dev_warn(dev, "clock proxy rollback requires retry\n");
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_cc_really_probe);

int qcom_cc_probe(struct platform_device *pdev, const struct qcom_cc_desc *desc)
{
	struct regmap *regmap;

	regmap = qcom_cc_map(pdev, desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return qcom_cc_really_probe(pdev, desc, regmap);
}
EXPORT_SYMBOL_GPL(qcom_cc_probe);

int qcom_cc_probe_by_index(struct platform_device *pdev, int index,
			   const struct qcom_cc_desc *desc)
{
	struct regmap *regmap;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(&pdev->dev, base, desc->config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return qcom_cc_really_probe(pdev, desc, regmap);
}
EXPORT_SYMBOL_GPL(qcom_cc_probe_by_index);

void qcom_cc_sync_state(struct device *dev, const struct qcom_cc_desc *desc)
{
	struct qcom_cc_lifetime *life = qcom_cc_lifetime_find(dev);

	clk_sync_state(dev);
	if (life && life->desc == desc && qcom_cc_unvote(life, true))
		dev_warn(dev, "clock proxy release requires retry\n");
}
EXPORT_SYMBOL(qcom_cc_sync_state);

int qcom_clk_crm_set_rate(struct clk *clk,
			  enum crm_drv_type client_type, u32 client_idx,
			  u32 pwr_st, unsigned long rate)
{
	struct clk_hw *hw;
	int ret;

	if (!clk)
		return -EINVAL;

	do {
		hw = __clk_get_hw(clk);

		if (clk_is_regmap_clk(hw)) {
			struct clk_regmap *rclk = to_clk_regmap(hw);

			if (rclk->ops && rclk->ops->set_crm_rate) {
				ret = rclk->ops->set_crm_rate(hw, client_type,
							      client_idx, pwr_st, rate);
				return ret;
			}
		}

	} while ((clk = clk_get_parent(hw->clk)));

	return -EINVAL;
}
EXPORT_SYMBOL(qcom_clk_crm_set_rate);

int qcom_clk_crmb_set_rate(struct clk *clk,
			   enum crm_drv_type client_type, u32 client_idx,
			   u32 resource_idx, u32 pwr_st, u32 ab_rate, u32 ib_rate)
{
	struct clk_hw *hw;
	int ret;

	if (!clk)
		return -EINVAL;

	do {
		hw = __clk_get_hw(clk);

		if (clk_is_regmap_clk(hw)) {
			struct clk_regmap *rclk = to_clk_regmap(hw);

			if (rclk->ops && rclk->ops->set_crmb_rate) {
				ret = rclk->ops->set_crmb_rate(hw, client_type,
							      client_idx, resource_idx, pwr_st,
							      ab_rate, ib_rate);
				return ret;
			}
		}

	} while ((clk = clk_get_parent(hw->clk)));

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(qcom_clk_crmb_set_rate);

int qcom_clk_get_voltage(struct clk *clk, unsigned long rate)
{
	struct clk_regmap *rclk;
	struct clk_hw *hw = __clk_get_hw(clk);
	int vdd_level;

	if (!clk_is_regmap_clk(hw))
		return -EINVAL;

	rclk = to_clk_regmap(hw);
	vdd_level = clk_find_vdd_level(hw, &rclk->vdd_data, rate);
	if (vdd_level < 0)
		return vdd_level;

	return clk_get_vdd_voltage(&rclk->vdd_data, vdd_level);
}
EXPORT_SYMBOL(qcom_clk_get_voltage);

int qcom_clk_set_flags(struct clk *clk, unsigned long flags)
{
	struct clk_regmap *rclk;
	struct clk_hw *hw;

	if (IS_ERR_OR_NULL(clk))
		return 0;

	hw = __clk_get_hw(clk);
	if (IS_ERR_OR_NULL(hw))
		return -EINVAL;

	if (!clk_is_regmap_clk(hw))
		return -EINVAL;

	rclk = to_clk_regmap(hw);
	if (rclk->ops && rclk->ops->set_flags)
		return rclk->ops->set_flags(hw, flags);

	return 0;
}
EXPORT_SYMBOL(qcom_clk_set_flags);

int qcom_cc_runtime_init(struct platform_device *pdev,
			 struct qcom_cc_desc *desc)
{
	struct device *dev = &pdev->dev;
	struct qcom_cc_lifetime *life;
	struct clk *iface;
	struct icc_path *path;

	life = qcom_cc_lifetime_get(dev, desc);
	if (IS_ERR(life))
		return PTR_ERR(life);
	if (life->runtime_enabled)
		return -EBUSY;
	iface = clk_get_optional(dev, "iface");
	if (IS_ERR(iface))
		return PTR_ERR(iface);
	path = of_icc_get(dev, NULL);
	if (IS_ERR(path)) {
		clk_put(iface);
		return PTR_ERR(path);
	}
	life->iface = iface;
	life->path = path;
	life->runtime_desc = desc;
	desc->path = path;
	platform_set_drvdata(pdev, desc);
	pm_runtime_enable(dev);
	life->runtime_enabled = true;
	return 0;
}
EXPORT_SYMBOL(qcom_cc_runtime_init);

int qcom_cc_runtime_acquire(struct platform_device *pdev,
			    struct qcom_cc_desc *desc)
{
	struct qcom_cc_lifetime *life;
	int ret;

	ret = qcom_cc_runtime_init(pdev, desc);
	if (ret)
		return ret;
	life = qcom_cc_lifetime_find(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret)
		return ret;
	life->probe_ref = true;
	return 0;
}
EXPORT_SYMBOL_GPL(qcom_cc_runtime_acquire);

int qcom_cc_runtime_release(struct platform_device *pdev)
{
	struct qcom_cc_lifetime *life = qcom_cc_lifetime_find(&pdev->dev);

	if (!life)
		return -EINVAL;
	if (!life->probe_ref)
		return 0;
	/* put drops the usage count even if the suspend callback returns an error. */
	life->probe_ref = false;
	return pm_runtime_put_sync(&pdev->dev);
}
EXPORT_SYMBOL_GPL(qcom_cc_runtime_release);

int qcom_cc_runtime_resume(struct device *dev)
{
	struct qcom_cc_lifetime *life = qcom_cc_lifetime_find(dev);
	int ret;

	if (!life || !life->runtime_enabled)
		return -EINVAL;
	ret = qcom_cc_runtime_resources_get(life);
	if (ret && qcom_cc_runtime_resources_put(life))
		dev_warn(dev, "clock runtime resume rollback requires retry\n");
	return ret;
}
EXPORT_SYMBOL(qcom_cc_runtime_resume);

int qcom_cc_runtime_suspend(struct device *dev)
{
	struct qcom_cc_lifetime *life = qcom_cc_lifetime_find(dev);
	int ret;

	if (!life || !life->runtime_enabled)
		return -EINVAL;
	ret = qcom_cc_runtime_resources_put(life);
	/* PM retains the active state after failure: restore released resources. */
	if (ret && qcom_cc_runtime_resources_get(life))
		dev_warn(dev, "clock runtime suspend rollback failed\n");
	return ret;
}
EXPORT_SYMBOL(qcom_cc_runtime_suspend);

static void __exit qcom_clk_exit(void)
{
	clk_debug_exit();
}
module_exit(qcom_clk_exit);

MODULE_DESCRIPTION("Common QCOM clock control library");
MODULE_LICENSE("GPL v2");
