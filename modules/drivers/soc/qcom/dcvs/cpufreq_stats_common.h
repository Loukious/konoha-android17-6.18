/* SPDX-License-Identifier: GPL-2.0-only */
/* Native PERF statistics memory layout shared by the V2 and V3 consumers. */
#ifndef _QCOM_CPUFREQ_STATS_COMMON_H
#define _QCOM_CPUFREQ_STATS_COMMON_H
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#define PERF_STATS_SIGNATURE 0x50455246
#define PERF_STATS_ATTEMPTS 128

enum entry_type { ENTRY_USAGE, ENTRY_RESIDENCY, ENTRY_MAX };
struct scmi_stats {
	u32 signature;
	u16 revision;
	u16 attributes;
	u16 num_domains;
	u16 reserved;
	u32 match_sequence;
	u32 perf_dom_entry_off_arr[];
} __packed;
struct perf_lvl_entry {
	u32 perf_lvl;
	u32 reserved;
	u64 usage;
	u64 residency;
} __packed;
struct perf_dom_entry {
	u16 num_perf_levels;
	u16 curr_perf_idx;
	u32 ext_tbl_off;
	u64 ts_last_change;
	struct perf_lvl_entry perf_lvl_arr[];
} __packed;
struct stats_info;
struct clkdom_entry {
	enum entry_type entry;
	u16 clkdom;
	struct stats_info *info;
};
struct stats_domain {
	u32 offset;
	u16 num_levels;
	u32 *frequencies;
};
struct stats_info {
	u32 stats_size;
	void __iomem *stats_iomem;
	u16 num_clkdom;
	struct stats_domain *domains;
	struct clkdom_entry *entries;
	struct dentry *dir;
};

static bool stats_range_valid(struct stats_info *info, u64 offset, u64 bytes)
{
	return offset <= info->stats_size && bytes <= info->stats_size - offset;
}

static int stats_domain_geometry(struct stats_info *info, u32 clkdom,
				 u32 *offset, u16 *levels)
{
	u64 start, header = offsetof(struct scmi_stats, perf_dom_entry_off_arr);

	if (clkdom >= info->num_clkdom ||
	    !stats_range_valid(info, header + sizeof(u32) * (u64)clkdom, sizeof(u32)))
		return -EINVAL;
	start = (u64)readl(info->stats_iomem + header + sizeof(u32) * clkdom) * 4;
	if (start < header + sizeof(u32) * (u64)info->num_clkdom ||
	    !stats_range_valid(info, start, sizeof(struct perf_dom_entry)))
		return -EPROTO;
	*levels = readl(info->stats_iomem + start) & U16_MAX;
	if (!stats_range_valid(info, start + sizeof(struct perf_dom_entry),
			      (u64)*levels * sizeof(struct perf_lvl_entry)))
		return -EPROTO;
	*offset = start;
	return 0;
}

static int stats_read_values(struct stats_info *info, struct clkdom_entry *entry,
			    u64 *values)
{
	struct stats_domain *domain;
	u32 before, after, offset;
	u16 levels;
	unsigned int attempt, i;
	size_t field;
	int ret;

	if (entry->clkdom >= info->num_clkdom || entry->entry >= ENTRY_MAX)
		return -EINVAL;
	domain = &info->domains[entry->clkdom];
	field = entry->entry == ENTRY_USAGE ? offsetof(struct perf_lvl_entry, usage) :
					     offsetof(struct perf_lvl_entry, residency);
	for (attempt = 0; attempt < PERF_STATS_ATTEMPTS; attempt++) {
		before = readl(info->stats_iomem + offsetof(struct scmi_stats, match_sequence));
		if (before & 1)
			continue;
		ret = stats_domain_geometry(info, entry->clkdom, &offset, &levels);
		if (!ret && (offset != domain->offset || levels != domain->num_levels))
			ret = -ESTALE;
		if (!ret) {
			for (i = 0; i < levels; i++) {
				void __iomem *row = info->stats_iomem + offset +
					offsetof(struct perf_dom_entry, perf_lvl_arr) +
					i * sizeof(struct perf_lvl_entry);
				if (readl(row + offsetof(struct perf_lvl_entry, perf_lvl)) !=
				    domain->frequencies[i]) {
					ret = -ESTALE;
					break;
				}
				values[i] = readl(row + field) | (u64)readl(row + field + 4) << 32;
			}
		}
		after = readl(info->stats_iomem + offsetof(struct scmi_stats, match_sequence));
		if (before == after)
			return ret;
	}
	return -EAGAIN;
}

static ssize_t cpufreq_stats_get(struct file *file, char __user *user_buf,
				 size_t count, loff_t *ppos)
{
	struct clkdom_entry *entry;
	struct stats_domain *domain;
	struct dentry *dentry = file->f_path.dentry;
	u64 *values = NULL;
	char *buffer = NULL;
	size_t length, bytes = 0;
	unsigned int i;
	ssize_t ret;

	ret = debugfs_file_get(dentry);
	if (ret)
		return ret;
	entry = file->private_data;
	if (!entry || !entry->info || entry->clkdom >= entry->info->num_clkdom) {
		ret = -ENOENT;
		goto out;
	}
	domain = &entry->info->domains[entry->clkdom];
	if (!domain->num_levels) {
		ret = 0;
		goto out;
	}
	length = (size_t)domain->num_levels * 40 + 1;
	values = kcalloc(domain->num_levels, sizeof(*values), GFP_KERNEL);
	buffer = kzalloc(length, GFP_KERNEL);
	if (!values || !buffer) {
		ret = -ENOMEM;
		goto out;
	}
	ret = stats_read_values(entry->info, entry, values);
	if (ret)
		goto out;
	for (i = 0; i < domain->num_levels; i++)
		bytes += scnprintf(buffer + bytes, length - bytes, "%u %llu\n",
				   domain->frequencies[i], values[i]);
	ret = simple_read_from_buffer(user_buf, count, ppos, buffer, bytes);
out:
	kfree(buffer);
	kfree(values);
	debugfs_file_put(dentry);
	return ret;
}

static const struct file_operations stats_ops = {
	.owner = THIS_MODULE,
	.read = cpufreq_stats_get,
	.open = simple_open,
	.llseek = default_llseek,
};

static void cpufreq_stats_debugfs_cleanup(void *arg)
{
	struct stats_info *info = arg;
	debugfs_remove_recursive(info->dir);
}

static int cpufreq_stats_create_fs(struct device *dev, struct stats_info *info)
{
	struct dentry *directory, *file;
	char name[24];
	int i, ret;

	info->dir = debugfs_create_dir("cpufreq_stats", NULL);
	if (IS_ERR_OR_NULL(info->dir))
		return info->dir ? PTR_ERR(info->dir) : -ENOMEM;
	ret = devm_add_action_or_reset(dev, cpufreq_stats_debugfs_cleanup, info);
	if (ret)
		return ret;
	for (i = 0; i < info->num_clkdom; i++) {
		snprintf(name, sizeof(name), "clkdom%d", i);
		directory = debugfs_create_dir(name, info->dir);
		if (IS_ERR_OR_NULL(directory))
			return directory ? PTR_ERR(directory) : -ENOMEM;
		file = debugfs_create_file("usage", 0400, directory,
					  &info->entries[i * ENTRY_MAX + ENTRY_USAGE], &stats_ops);
		if (IS_ERR_OR_NULL(file))
			return file ? PTR_ERR(file) : -ENOMEM;
		file = debugfs_create_file("time_in_state", 0400, directory,
					  &info->entries[i * ENTRY_MAX + ENTRY_RESIDENCY], &stats_ops);
		if (IS_ERR_OR_NULL(file))
			return file ? PTR_ERR(file) : -ENOMEM;
	}
	return 0;
}

static int cpufreq_stats_map(struct device *dev, u64 address, u32 size)
{
	struct stats_info *info;
	struct stats_domain *domain;
	u64 last;
	u32 sequence;
	int ret, i, j;

	if (!size)
		return -ENODATA;
	if (size < sizeof(struct scmi_stats))
		return -EPROTO;
	if ((u64)(phys_addr_t)address != address ||
	    check_add_overflow(address, (u64)size - 1, &last))
		return -ERANGE;
	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->stats_size = size;
	info->stats_iomem = devm_ioremap(dev, address, size);
	if (!info->stats_iomem)
		return -ENOMEM;
	sequence = readl(info->stats_iomem + offsetof(struct scmi_stats, match_sequence));
	if (sequence & 1)
		return -EAGAIN;
	if (readl(info->stats_iomem + offsetof(struct scmi_stats, signature)) != PERF_STATS_SIGNATURE)
		return -EPROTO;
	if ((readl(info->stats_iomem + offsetof(struct scmi_stats, revision)) & U16_MAX) != 1)
		return -EOPNOTSUPP;
	info->num_clkdom = readl(info->stats_iomem + offsetof(struct scmi_stats, num_domains)) & U16_MAX;
	if (!info->num_clkdom || !stats_range_valid(info, sizeof(struct scmi_stats),
						  (u64)info->num_clkdom * sizeof(u32)))
		return -EPROTO;
	info->domains = devm_kcalloc(dev, info->num_clkdom, sizeof(*info->domains), GFP_KERNEL);
	info->entries = devm_kcalloc(dev, info->num_clkdom * ENTRY_MAX, sizeof(*info->entries), GFP_KERNEL);
	if (!info->domains || !info->entries)
		return -ENOMEM;
	for (i = 0; i < info->num_clkdom; i++) {
		domain = &info->domains[i];
		ret = stats_domain_geometry(info, i, &domain->offset, &domain->num_levels);
		if (ret)
			return ret;
		domain->frequencies = devm_kcalloc(dev, domain->num_levels, sizeof(u32), GFP_KERNEL);
		if (!domain->frequencies && domain->num_levels)
			return -ENOMEM;
		for (j = 0; j < domain->num_levels; j++)
			domain->frequencies[j] = readl(info->stats_iomem + domain->offset +
				offsetof(struct perf_dom_entry, perf_lvl_arr) + j * sizeof(struct perf_lvl_entry));
		for (j = 0; j < ENTRY_MAX; j++) {
			info->entries[i * ENTRY_MAX + j].info = info;
			info->entries[i * ENTRY_MAX + j].clkdom = i;
			info->entries[i * ENTRY_MAX + j].entry = j;
		}
	}
	if (sequence != readl(info->stats_iomem + offsetof(struct scmi_stats, match_sequence)))
		return -EAGAIN;
	return cpufreq_stats_create_fs(dev, info);
}
#endif
