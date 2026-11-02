// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019-2021, The Linux Foundation. All rights reserved. */

#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/regulator/consumer.h>
#include <linux/device.h>

#include "vdd-class.h"

static DEFINE_MUTEX(vdd_lock);

/*
 * Aggregate the vdd_class level votes and call regulator framework functions
 * to enforce the highest vote.
 */
static int clk_aggregate_vdd(struct clk_vdd_class *vdd_class)
{
	struct regulator **r = vdd_class->regulator;
	int level, ret, i;
	bool rollback_failed = false;
	int *uv = vdd_class->vdd_uv;
	int n_reg = vdd_class->num_regulators;
	int cur_lvl = vdd_class->cur_level;
	int max_lvl = vdd_class->num_levels - 1;
	int cur_base = (cur_lvl < vdd_class->num_levels ? cur_lvl : 0) * n_reg;
	bool was_enabled = cur_lvl > 0 && cur_lvl < vdd_class->num_levels;
	int new_base;

	if (cur_lvl < 0)
		return -EIO;

	/* aggregate votes */
	for (level = max_lvl; level > 0; level--)
		if (vdd_class->level_votes[level])
			break;

	if (level == cur_lvl)
		return 0;

	new_base = level * n_reg;

	for (i = 0; i < vdd_class->num_regulators; i++) {
		pr_debug("Set voltage level %d\n", uv[new_base + i]);
		ret = regulator_set_voltage(r[i], uv[new_base + i], INT_MAX);
		if (ret)
			goto set_voltage_fail;

		if (!was_enabled && level > 0)
			ret = regulator_enable(r[i]);
		else if (was_enabled && level == 0)
			ret = regulator_disable(r[i]);
		if (ret)
			goto enable_disable_fail;
	}

	vdd_class->cur_level = level;

	return 0;

enable_disable_fail:
	if (regulator_set_voltage(r[i], uv[cur_base + i], INT_MAX))
		rollback_failed = true;

set_voltage_fail:
	for (i--; i >= 0; i--) {
		if (regulator_set_voltage(r[i], uv[cur_base + i], INT_MAX))
			rollback_failed = true;
		if (!was_enabled && level > 0) {
			if (regulator_disable(r[i]))
				rollback_failed = true;
		} else if (was_enabled && level == 0) {
			if (regulator_enable(r[i]))
				rollback_failed = true;
		}
	}
	if (rollback_failed) {
		/* Unknown hardware state: refuse new votes; owner retains handles. */
		vdd_class->cur_level = -1;
		pr_err("%s: regulator rollback failed\n", vdd_class->class_name);
	}

	return ret;
}

static int clk_vote_vdd_class_level(struct clk_vdd_class *vdd_class, int level)
{
	int ret;

	if (level < 0 || level >= vdd_class->num_levels)
		return -EINVAL;

	mutex_lock(&vdd_lock);
	vdd_class->level_votes[level]++;

	ret = clk_aggregate_vdd(vdd_class);
	if (ret)
		vdd_class->level_votes[level]--;

	mutex_unlock(&vdd_lock);

	return ret;
}

static int clk_unvote_vdd_class_level(struct clk_vdd_class *vdd_class, int level)
{
	int ret;

	if (level < 0 || level >= vdd_class->num_levels)
		return -EINVAL;

	mutex_lock(&vdd_lock);
	if (WARN(!vdd_class->level_votes[level],
		 "Reference counts are incorrect for %s level %d\n",
		 vdd_class->class_name, level)) {
		mutex_unlock(&vdd_lock);
		return -EINVAL;
	}
	vdd_class->level_votes[level]--;

	ret = clk_aggregate_vdd(vdd_class);
	if (ret)
		vdd_class->level_votes[level]++;

	mutex_unlock(&vdd_lock);

	return ret;
}

/**
 * clk_get_vdd_voltage - Return corner voltage for a vdd class
 * @vdd_data:	vdd_class data for the clock
 * @level:	voltage level
 *
 * Returns corner voltage on success, -EERROR otherwise.
 */
int clk_get_vdd_voltage(struct clk_vdd_class_data *vdd_data, int vdd_level)
{
	int i, corner = -EINVAL;

	for (i = 0; i < vdd_data->num_vdd_classes; i++)
		corner = max(corner, vdd_data->vdd_classes[i]->vdd_uv[vdd_level]);

	if (vdd_data->vdd_class)
		corner = max(corner, vdd_data->vdd_class->vdd_uv[vdd_level]);

	return corner;

}
EXPORT_SYMBOL(clk_get_vdd_voltage);

/**
 * clk_vote_vdd_level - Add a vote for a given voltage level
 * @vdd_data:	vdd_class data for the clock
 * @level:	voltage level to add a vote for
 *
 * Add a regulator framework voltage request for the vdd_class regulator(s)
 * by decrementing the reference count for a given voltage level. This ensures
 * that the supply regulator is allowed to potentially lower its voltage after
 * re-aggregating overall voltage requests.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_vote_vdd_level(struct clk_vdd_class_data *vdd_data, int level)
{
	int ret, i;

	for (i = 0; i < vdd_data->num_vdd_classes; i++) {
		ret = clk_vote_vdd_class_level(vdd_data->vdd_classes[i], level);
		if (ret)
			goto vote_fail;
	}

	if (vdd_data->vdd_class) {
		ret = clk_vote_vdd_class_level(vdd_data->vdd_class, level);
		if (ret)
			goto vote_fail;
	}

	return 0;

vote_fail:
	for (i--; i >= 0; i--)
		clk_unvote_vdd_class_level(vdd_data->vdd_classes[i], level);

	return ret;

}
EXPORT_SYMBOL(clk_vote_vdd_level);

/**
 * clk_unvote_vdd_level - Remove a vote for a given voltage level
 * @vdd_data:	vdd_class data for the clock
 * @level:	voltage level to remove a vote for
 *
 * Removes a regulator framework voltage request for the vdd_class regulator(s)
 * by incrementing the reference count for a given voltage level. This ensures
 * that the supply regulator is outputting at least at the requested level.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_unvote_vdd_level(struct clk_vdd_class_data *vdd_data, int level)
{
	int ret, i;

	for (i = 0; i < vdd_data->num_vdd_classes; i++) {
		ret = clk_unvote_vdd_class_level(vdd_data->vdd_classes[i], level);
		if (ret)
			goto unvote_fail;
	}

	if (vdd_data->vdd_class) {
		ret = clk_unvote_vdd_class_level(vdd_data->vdd_class, level);
		if (ret)
			goto unvote_fail;
	}

	return 0;

unvote_fail:
	for (i--; i >= 0; i--)
		clk_vote_vdd_class_level(vdd_data->vdd_classes[i], level);

	return ret;
}
EXPORT_SYMBOL(clk_unvote_vdd_level);

/**
 * clk_find_vdd_level - Find the voltage level required for a given clock rate.
 * @hw:		clk_hw pointer of clock being voted on
 * @vdd_data:	vdd_class data for the clock
 * @rate:	Clock rate in Hz to vote for
 *
 * Finds the matching required voltage level for a rate from a list
 * of well characterized frequency to voltage points for a given clock.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_find_vdd_level(struct clk_hw *hw,
				struct clk_vdd_class_data *vdd_data,
				unsigned long rate)
{
	int level;

	if (!vdd_data->num_rate_max)
		return -ENODATA;

	/*
	 * For certain PLLs, due to the limitation in the bits allocated for
	 * programming the fractional divider, the actual rate of the PLL will
	 * be slightly higher than the requested rate (in the order of several
	 * Hz). To accommodate this difference, convert the FMAX rate and the
	 * clock frequency to KHz and use that for deriving the voltage level.
	 */
	for (level = 0; level < vdd_data->num_rate_max; level++)
		if (DIV_ROUND_CLOSEST(rate, 1000) <=
		    DIV_ROUND_CLOSEST(vdd_data->rate_max[level], 1000) &&
		     vdd_data->rate_max[level] > 0)
			break;

	if (level == vdd_data->num_rate_max) {
		pr_err("Rate %lu for %s is greater than highest Fmax\n",
			rate, clk_hw_get_name(hw));
		return -EINVAL;
	}

	return level;
}
EXPORT_SYMBOL(clk_find_vdd_level);

int clk_regulator_init(struct device *dev, const struct qcom_cc_desc *desc)
{
	struct clk_vdd_class *vdd_class;
	struct regulator *regulator;
	const char *name;
	int i, cnt, ret;
	size_t array_size;

	for (i = 0; i < desc->num_clk_regulators; i++) {
		vdd_class = desc->clk_regulators[i];

		for (cnt = 0; cnt < vdd_class->num_regulators; cnt++) {
			if (vdd_class->regulator[cnt])
				continue;

			name = vdd_class->regulator_names[cnt];
			regulator = regulator_get(dev, name);
			if (IS_ERR(regulator)) {
				if (PTR_ERR(regulator) != -EPROBE_DEFER)
					dev_err(dev, "%s error %s regulator\n",
						__func__, name);
				ret = PTR_ERR(regulator);
				goto err_regulator_get;
			}
			vdd_class->regulator[cnt] = regulator;
		}
	}

	return 0;

err_regulator_get:
	while (i >= 0) {
		vdd_class = desc->clk_regulators[i];
		for (cnt = 0; cnt < vdd_class->num_regulators; cnt++)
			if (vdd_class->regulator[cnt])
				regulator_put(vdd_class->regulator[cnt]);
		array_size = vdd_class->num_regulators * sizeof(*vdd_class->regulator);
		memset(vdd_class->regulator, 0, array_size);
		i--;
	}

	return ret;
}

void clk_regulator_deinit(const struct qcom_cc_desc *desc, bool release)
{
	struct clk_vdd_class *vdd_class;
	int i, j;

	/* Provider callbacks have been drained by the shared lifetime owner. */
	for (i = 0; i < desc->num_clk_regulators; i++) {
		vdd_class = desc->clk_regulators[i];
		for (j = 0; j < vdd_class->num_regulators; j++) {
			if (release && vdd_class->regulator[j])
				regulator_put(vdd_class->regulator[j]);
			vdd_class->regulator[j] = NULL;
		}
		memset(vdd_class->level_votes, 0,
		       sizeof(*vdd_class->level_votes) * vdd_class->num_levels);
		vdd_class->cur_level = vdd_class->num_levels;
	}
}

int clk_vdd_proxy_vote(struct device *dev, const struct qcom_cc_desc *desc)
{
	struct clk_vdd_class *vdd_class;
	int i, ret;

	for (i = 0; i < desc->num_clk_regulators; i++) {
		vdd_class = desc->clk_regulators[i];
		ret = clk_vote_vdd_class_level(vdd_class, vdd_class->num_levels - 1);
		if (ret)
			goto rollback;
	}
	return 0;
rollback:
	while (i-- > 0) {
		vdd_class = desc->clk_regulators[i];
		if (clk_unvote_vdd_class_level(vdd_class, vdd_class->num_levels - 1))
			dev_warn(dev, "clock legacy proxy rollback failed\n");
	}
	return ret;
}

int clk_vdd_proxy_unvote(struct device *dev, const struct qcom_cc_desc *desc)
{
	struct clk_vdd_class *vdd_class;
	u32 i;
	int ret = 0;

	for (i = 0; i < desc->num_clk_regulators; i++) {
		vdd_class = desc->clk_regulators[i];

		ret = clk_unvote_vdd_class_level(vdd_class,
						 vdd_class->num_levels - 1);
		if (ret)
			WARN(ret, "clk_unvote_vdd_level failed ret=%d\n", ret);
	}

	return ret;
}

