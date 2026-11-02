// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "qti_bcl_common.h"

static struct dentry	*bcl_stats_parent;
static DEFINE_MUTEX(bcl_debugfs_lock);
static unsigned int bcl_debugfs_users;

void bcl_update_clear_stats(struct bcl_lvl_stats *bcl_stat)
{
	uint32_t iter = 0;
	unsigned long long last_duration;
	struct bcl_data_history *hist_data;

	if (!bcl_stat->trigger_state)
		return;

	iter = (bcl_stat->counter % BCL_HISTORY_COUNT);
	hist_data = &bcl_stat->bcl_history[iter];
	hist_data->clear_ts = sched_clock();
	last_duration = DIV_ROUND_UP(
			hist_data->clear_ts - hist_data->trigger_ts,
			NSEC_PER_USEC);
	bcl_stat->total_duration += last_duration;
	if (last_duration > bcl_stat->max_duration)
		bcl_stat->max_duration = last_duration;

	bcl_stat->counter++;
	bcl_stat->trigger_state = false;
}

void bcl_update_trigger_stats(struct bcl_lvl_stats *bcl_stat, int ibat, int vbat,
			unsigned long long trigger_ts)
{
	uint32_t iter = 0;

	iter = (bcl_stat->counter % BCL_HISTORY_COUNT);
	bcl_stat->bcl_history[iter].clear_ts = 0x0;
	bcl_stat->bcl_history[iter].trigger_ts = trigger_ts;
	bcl_stat->bcl_history[iter].ibat = ibat;
	bcl_stat->bcl_history[iter].vbat = vbat;
	bcl_stat->trigger_state = true;
}

static int bcl_lvl_show(struct seq_file *s, void *data)
{
	struct bcl_lvl_stats *bcl_stat = s->private;
	unsigned long long last_duration = 0;
	int idx = 0, cur_counter = 0, loop_till = 0;

	mutex_lock(&bcl_stat->bcl_dev->stats_lock);
	seq_printf(s, "%-30s: %d\n",
					"SW Counter", bcl_stat->counter);
	seq_printf(s, "%-30s: %d\n",
					"Irq self cleared counter",
					bcl_stat->self_cleared_counter);
	seq_printf(s, "%-30s: %llu\n",
					"Max Mitigation at", bcl_stat->max_mitig_ts);
	seq_printf(s, "%-30s: %llu usec\n",
					"Max Mitigation latency",
					DIV_ROUND_UP(bcl_stat->max_mitig_latency,
						NSEC_PER_USEC));
	seq_printf(s, "%-30s: %llu usec\n",
					"BCL mitigation residency", bcl_stat->max_duration);
	seq_printf(s, "%-30s: %llu usec\n",
					"Total residency",	bcl_stat->total_duration);
	seq_printf(s, "Last %d iterations	:\n", BCL_HISTORY_COUNT);
	seq_printf(s, "%s%10s%10s%15s%15s%16s\n", "idx", "ibat", "vbat",
				"trigger_ts", "clear_ts", "duration(usec)");

	cur_counter = (bcl_stat->counter % BCL_HISTORY_COUNT);
	idx = cur_counter - 1;
	loop_till = -2;
	/* print history data as stack. latest entry first */
	do {
		last_duration = 0;
		if (idx < 0) {
			idx = BCL_HISTORY_COUNT - 1;
			loop_till = cur_counter - 1;
			continue;
		}
		if (bcl_stat->bcl_history[idx].clear_ts)
			last_duration = DIV_ROUND_UP(
					bcl_stat->bcl_history[idx].clear_ts -
					bcl_stat->bcl_history[idx].trigger_ts,
					NSEC_PER_USEC);
		seq_printf(s, "[%d]%10d%10d%15llu%15llu%16llu\n", idx,
				bcl_stat->bcl_history[idx].ibat,
				bcl_stat->bcl_history[idx].vbat,
				bcl_stat->bcl_history[idx].trigger_ts,
				bcl_stat->bcl_history[idx].clear_ts,
				last_duration);
		--idx;
	} while (idx > loop_till);

	mutex_unlock(&bcl_stat->bcl_dev->stats_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bcl_lvl);

static int bpm_stats_show(struct seq_file *s, void *data)
{
	struct bcl_device *bcl_perph = s->private;
	struct bcl_bpm_stats sample;
	int ret;

	if (bcl_perph == NULL) {
		pr_err("NULL\n");
		return 0;
	}

	if (!bcl_perph->enable_bpm)
		return 0;

	ret = get_bpm_stats(bcl_perph, &sample);
	if (ret)
		return ret;

	seq_printf(s, "Max Ibat\t: %d \tSynchronus Vbat\t: %d\n",
		sample.max_ibat, sample.sync_vbat);
	seq_printf(s, "Min Vbat\t: %d \tSynchronus Ibat\t: %d\n",
		sample.min_vbat, sample.sync_ibat);
	seq_printf(s, "lvl0 count\t: %d\n", sample.lvl0_cnt);
	seq_printf(s, "lvl1 count\t: %d\n", sample.lvl1_cnt);
	seq_printf(s, "lvl2 count\t: %d\n", sample.lvl2_cnt);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bpm_stats);

static int bpm_reset(void *data, u64 val)
{
	struct bcl_device *bcl_perph = data;
	unsigned int reset = 1;
	uint16_t base;
	int ret = 0;

	if (bcl_perph == NULL) {
		pr_err("bcl_perph is NULL\n");
		return 0;
	}

	mutex_lock(&bcl_perph->stats_lock);
	base = bcl_perph->fg_bcl_addr;
	ret = regmap_write(bcl_perph->regmap, (base + BPM_CLR_OFFSET), reset);
	if (ret < 0) {
		pr_err("Error reading register:0x%04x val:0x%02x err:%d\n",
			(base + BPM_CLR_OFFSET), reset, ret);
		mutex_unlock(&bcl_perph->stats_lock);
		return ret;
	}
	bcl_perph->last_bpm_reset_ts = sched_clock();
	mutex_unlock(&bcl_perph->stats_lock);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(bpm_reset_fops, NULL, bpm_reset, "%llu\n");

static void bcl_stats_remove(void *data)
{
	struct bcl_device *bcl = data;

	mutex_lock(&bcl_debugfs_lock);
	debugfs_remove_recursive(bcl->debugfs);
	if (!--bcl_debugfs_users) {
		debugfs_remove_recursive(bcl_stats_parent);
		bcl_stats_parent = NULL;
	}
	mutex_unlock(&bcl_debugfs_lock);
}

int bcl_stats_init(char *name, struct bcl_device *bcl, uint32_t stats_len)
{
	char stats_name[BCL_STATS_NAME_LENGTH];
	struct dentry *parent;
	int idx;

	if (stats_len > MAX_BCL_LVL_COUNT)
		return -EINVAL;
	mutex_lock(&bcl_debugfs_lock);
	if (!bcl_debugfs_users)
		bcl_stats_parent = debugfs_create_dir("bcl_stats", NULL);
	/* Never create files in the root if the parent failed. */
	if (IS_ERR_OR_NULL(bcl_stats_parent)) {
		mutex_unlock(&bcl_debugfs_lock);
		return 0;
	}
	parent = debugfs_create_dir(name, bcl_stats_parent);
	if (IS_ERR_OR_NULL(parent)) {
		if (!bcl_debugfs_users) {
			debugfs_remove_recursive(bcl_stats_parent);
			bcl_stats_parent = NULL;
		}
		mutex_unlock(&bcl_debugfs_lock);
		return 0;
	}
	bcl->debugfs = parent;
	bcl_debugfs_users++;
	for (idx = 0; idx < stats_len; idx++) {
		snprintf(stats_name, sizeof(stats_name), "lvl%d_stats", idx);
		debugfs_create_file(stats_name, 0444, parent, &bcl->stats[idx], &bcl_lvl_fops);
	}
	if (bcl->enable_bpm) {
		debugfs_create_file("bpm_stats", 0444, parent, bcl, &bpm_stats_fops);
		debugfs_create_file("reset", 0200, parent, bcl, &bpm_reset_fops);
	}
	mutex_unlock(&bcl_debugfs_lock);
	return devm_add_action_or_reset(bcl->dev, bcl_stats_remove, bcl);
}
