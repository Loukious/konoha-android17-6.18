// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <asm/cputype.h>
#include <asm/smp.h>
#include <linux/uaccess.h>
#include <linux/limits.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/slab.h>

#define MAX_POSSIBLE_CPUS	8
#define MIN_POSSIBLE_CPUS	1
#define SEQ_LPM_STR_SZ		24
#define CPUNAME_SZ		6
#define STATS_NAME_SZ		25
#define OFFSET_8BYTES		0x08
#define OFFSET_4BYTES		0x04

#define MAX_CPU_LPM_BITS	4
#define MAX_CL_LPM_BITS		4
#define MAX_CPU_RESIDENCY_BITS	5
#define MAX_CL_RESIDENCY_BITS	3

enum {
	APSS_LPM_COUNTER_CPUx_C1_LO_VAL,
	APSS_LPM_COUNTER_CPUx_C2D_LO_VAL,
	APSS_LPM_COUNTER_CPUx_C3_LO_VAL,
	APSS_LPM_COUNTER_CPUx_C4_LO_VAL,
	APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n,
	APSS_CL_LPM_RESIDENCY_CNTR_CFG,
	APSS_LPM_RESIDENCY_C2_D2_CNTR_n,
	APSS_LPM_RESIDENCY_C3_CNTR_n,
	APSS_LPM_RESIDENCY_C4_D4_CNTR_n,
};

static const u32 qcom_cpuss_cntr_v1_offsets[] = {
	[APSS_LPM_COUNTER_CPUx_C1_LO_VAL]	=	0x8000,
	[APSS_LPM_COUNTER_CPUx_C2D_LO_VAL]	=	0x8048,
	[APSS_LPM_COUNTER_CPUx_C3_LO_VAL]	=	0x8090,
	[APSS_LPM_COUNTER_CPUx_C4_LO_VAL]	=	0x80D8,
	[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n]	=	0xC004,
	[APSS_CL_LPM_RESIDENCY_CNTR_CFG]	=	0xC030,
	[APSS_LPM_RESIDENCY_C2_D2_CNTR_n]	=	0xC040,
	[APSS_LPM_RESIDENCY_C3_CNTR_n]		=	0xC090,
	[APSS_LPM_RESIDENCY_C4_D4_CNTR_n]	=	0xC0D0,
};

static const u32 qcom_cpuss_cntr_v2_offsets[] = {
	[APSS_LPM_COUNTER_CPUx_C1_LO_VAL]	=	0x8000,
	[APSS_LPM_COUNTER_CPUx_C2D_LO_VAL]	=	0x8028,
	[APSS_LPM_COUNTER_CPUx_C3_LO_VAL]	=	0x8050,
	[APSS_LPM_COUNTER_CPUx_C4_LO_VAL]	=	0x8078,
	[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n]	=	0xC004,
	[APSS_CL_LPM_RESIDENCY_CNTR_CFG]	=	0xC030,
	[APSS_LPM_RESIDENCY_C2_D2_CNTR_n]	=	0xC040,
	[APSS_LPM_RESIDENCY_C3_CNTR_n]		=	0xC090,
	[APSS_LPM_RESIDENCY_C4_D4_CNTR_n]	=	0xC0D0,
};


static const u32 qcom_cpuss_cntr_v3_offsets[] = {
	[APSS_LPM_COUNTER_CPUx_C1_LO_VAL]	=	0x8000,
	[APSS_LPM_COUNTER_CPUx_C2D_LO_VAL]	=	0x8038,
	[APSS_LPM_COUNTER_CPUx_C3_LO_VAL]	=	0x8070,
	[APSS_LPM_COUNTER_CPUx_C4_LO_VAL]	=	0x80A8,
	[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n]	=	0xC004,
	[APSS_CL_LPM_RESIDENCY_CNTR_CFG]	=	0xC030,
	[APSS_LPM_RESIDENCY_C2_D2_CNTR_n]	=	0xC040,
	[APSS_LPM_RESIDENCY_C3_CNTR_n]		=	0xC090,
	[APSS_LPM_RESIDENCY_C4_D4_CNTR_n]	=	0xC0D0,
};

/* bits are per CPU LPM_CFG register */
#define LPM_COUNTER_EN_C1	BIT(0)
#define LPM_COUNTER_EN_C2D	BIT(1)
#define LPM_COUNTER_EN_C3	BIT(2)
#define LPM_COUNTER_EN_C4	BIT(3)
#define LPM_COUNTER_RESET_C1	BIT(4)
#define LPM_COUNTER_RESET_C2D	BIT(5)
#define LPM_COUNTER_RESET_C3	BIT(6)
#define LPM_COUNTER_RESET_C4	BIT(7)

#define RESET_ALL_CPU_LPM	(LPM_COUNTER_RESET_C1 | \
				LPM_COUNTER_RESET_C2D | LPM_COUNTER_RESET_C3 | \
				LPM_COUNTER_RESET_C4)

/* bits are per CL LPM_CFG register */
#define LPM_COUNTER_EN_D1	BIT(0)
#define LPM_COUNTER_EN_D2D	BIT(1)
#define LPM_COUNTER_EN_D3	BIT(2)
#define LPM_COUNTER_EN_D4	BIT(3)
#define LPM_COUNTER_RESET_D1	BIT(4)
#define LPM_COUNTER_RESET_D2D	BIT(5)
#define LPM_COUNTER_RESET_D3	BIT(6)
#define LPM_COUNTER_RESET_D4	BIT(7)


#define RESET_ALL_CL_LPM	(LPM_COUNTER_RESET_D1 | \
				LPM_COUNTER_RESET_D2D | LPM_COUNTER_RESET_D3 | \
				LPM_COUNTER_RESET_D4)

/* Core residency cfg as per register */
#define RESIDENCY_CNTR_C2_EN	BIT(0)
#define RESIDENCY_CNTR_C2_CLR	BIT(1)
#define RESIDENCY_CNTR_C3_EN	BIT(2)
#define RESIDENCY_CNTR_C3_CLR	BIT(3)
#define RESIDENCY_CNTR_C4_EN	BIT(4)
#define	RESIDENCY_CNTR_C4_CLR	BIT(5)

#define CLR_ALL_CPU_RESIDENCY	(RESIDENCY_CNTR_C2_CLR | \
				RESIDENCY_CNTR_C3_CLR | RESIDENCY_CNTR_C4_CLR)

/* Cluster residency bits as per cfg register*/
#define RESIDENCY_CNTR_D2_EN	BIT(0)
#define RESIDENCY_CNTR_D2_CLR	BIT(1)
#define RESIDENCY_CNTR_D4_EN	BIT(2)
#define RESIDENCY_CNTR_D4_CLR	BIT(3)

#define CLR_CL_RESIDENCY	(RESIDENCY_CNTR_D2_CLR | \
				RESIDENCY_CNTR_D4_CLR)

struct qcom_cpuss_stats {
	char mode_name[20];
	void __iomem *reg;	/* iomapped reg */
	struct list_head node;
};

struct qcom_target_info {
	struct platform_device *pdev;
	int ncpu;
	phys_addr_t per_cpu_lpm_cfg[MAX_POSSIBLE_CPUS];
	u32 per_cpu_lpm_cfg_size[MAX_POSSIBLE_CPUS];
	void __iomem *per_cpu_cfg_base[MAX_POSSIBLE_CPUS];
	phys_addr_t apss_seq_mem_base;
	resource_size_t apss_seq_mem_size;
	void __iomem *apss_base;
	phys_addr_t l3_seq_lpm_cfg;
	u32 l3_seq_lpm_size;
	void __iomem *l3_cfg_base;
	bool config_mmio;
	const u32 *offsets;
	u8 cpu_pcpu_map[MAX_POSSIBLE_CPUS];
	struct qcom_cpuss_stats complete_stats;

	struct dentry *stats_rootdir;
	struct dentry *cpu_dir[MAX_POSSIBLE_CPUS];
	struct dentry *cl_rootdir;
	struct mutex stats_reset_lock;
};

static int qcom_cpuss_create_file(const char *name, umode_t mode,
                                  struct dentry *parent, void *data,
                                  const struct file_operations *fops)
{
	struct dentry *file = debugfs_create_file(name, mode, parent, data, fops);

	if (IS_ERR_OR_NULL(file))
		return file ? PTR_ERR(file) : -ENOMEM;
	return 0;
}

static bool qcom_cpuss_offset_valid(struct qcom_target_info *info,
                                  u64 offset, size_t width)
{
	return width <= info->apss_seq_mem_size &&
		offset <= info->apss_seq_mem_size - width;
}

static int qcom_cpuss_validate_offsets(struct qcom_target_info *info)
{
	int cpu, bit;
	u64 offset;

	for (cpu = 0; cpu < info->ncpu; cpu++) {
		if (!qcom_cpuss_offset_valid(info,
		    (u64)info->offsets[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n] + 4 * cpu, 4))
			return -EINVAL;
		for (bit = 0; bit < MAX_CPU_LPM_BITS; bit++) {
			offset = (u64)info->offsets[APSS_LPM_COUNTER_CPUx_C1_LO_VAL + bit] + 8 * cpu;
			if (!qcom_cpuss_offset_valid(info, offset, 8))
				return -EINVAL;
		}
		for (bit = 0; bit < 3; bit++) {
			offset = (u64)info->offsets[APSS_LPM_RESIDENCY_C2_D2_CNTR_n + bit] + 8 * cpu;
			if (!qcom_cpuss_offset_valid(info, offset, 8))
				return -EINVAL;
		}
	}
	if (!qcom_cpuss_offset_valid(info, info->offsets[APSS_CL_LPM_RESIDENCY_CNTR_CFG], 4))
		return -EINVAL;
	for (bit = 0; bit < MAX_CL_LPM_BITS; bit++) {
		offset = (u64)info->offsets[APSS_LPM_COUNTER_CPUx_C1_LO_VAL + bit] + 8 * info->ncpu;
		if (!qcom_cpuss_offset_valid(info, offset, 8))
			return -EINVAL;
	}
	for (bit = 0; bit < 3; bit += 2) {
		offset = (u64)info->offsets[APSS_LPM_RESIDENCY_C2_D2_CNTR_n + bit] + 8 * info->ncpu;
		if (!qcom_cpuss_offset_valid(info, offset, 8))
			return -EINVAL;
	}
	return 0;
}

static bool qcom_cpuss_cfg_resource_valid(const struct resource *res)
{
	return res->end >= res->start && !(res->start & 3) &&
		resource_size(res) >= sizeof(u32);
}

static void qcom_cpuss_cleanup(void *data)
{
	struct qcom_target_info *info = data;

	debugfs_remove_recursive(info->stats_rootdir);
	info->stats_rootdir = NULL;
}

static char *get_str_cpu_lpm_state(u8 state)
{
	switch (state) {
	case LPM_COUNTER_EN_C1:
		return "C1_count";
	case LPM_COUNTER_EN_C2D:
		return "C2D_count";
	case LPM_COUNTER_EN_C3:
		return "C3_count";
	case LPM_COUNTER_EN_C4:
		return "C4_count";
	default:
		return NULL;
	}
}

static char *get_str_cl_lpm_state(u8 state)
{
	switch (state) {
	case LPM_COUNTER_EN_D1:
		return "D1_count";
	case LPM_COUNTER_EN_D2D:
		return "D2D_count";
	case LPM_COUNTER_EN_D3:
		return "D3_count";
	case LPM_COUNTER_EN_D4:
		return "D4_count";
	default:
		return NULL;
	}
}

static char *get_str_cpu_res(u32 cfg)
{
	switch (cfg) {
	case RESIDENCY_CNTR_C2_EN:
		return "C2_residency";
	case RESIDENCY_CNTR_C3_EN:
		return "C3_residency";
	case RESIDENCY_CNTR_C4_EN:
		return "C4_residency";
	default:
		return NULL;
	}
}

static char *get_str_cl_res(u8 cfg)
{
	switch (cfg) {
	case RESIDENCY_CNTR_D2_EN:
		return "D2_residency";
	case RESIDENCY_CNTR_D4_EN:
		return "D4_residency";
	default:
		return NULL;
	}

}

static int get_cpu_lpm_read_offset(u8 cpu, u8 cpu_mode, const u32 *regs)
{
	int offset;

	switch (cpu_mode) {
	case LPM_COUNTER_EN_C1:
		offset = regs[APSS_LPM_COUNTER_CPUx_C1_LO_VAL] + (cpu * OFFSET_8BYTES);
		break;
	case LPM_COUNTER_EN_C2D:
		offset = regs[APSS_LPM_COUNTER_CPUx_C2D_LO_VAL] + (cpu * OFFSET_8BYTES);
		break;
	case LPM_COUNTER_EN_C3:
		offset = regs[APSS_LPM_COUNTER_CPUx_C3_LO_VAL] + (cpu * OFFSET_8BYTES);
		break;
	case LPM_COUNTER_EN_C4:
		offset = regs[APSS_LPM_COUNTER_CPUx_C4_LO_VAL] + (cpu * OFFSET_8BYTES);
		break;
	default:
		pr_err("Unknown mode\n");
		offset = -EINVAL;
	}

	return offset;
}

static int get_cpu_residency_read_offset(u8 cpu, u8 cpu_res_cfg, const u32 *regs)
{
	int offset;

	switch (cpu_res_cfg) {
	case RESIDENCY_CNTR_C2_EN:
		offset = regs[APSS_LPM_RESIDENCY_C2_D2_CNTR_n] + (cpu * OFFSET_8BYTES);
		break;
	case RESIDENCY_CNTR_C3_EN:
		offset = regs[APSS_LPM_RESIDENCY_C3_CNTR_n] + (cpu * OFFSET_8BYTES);
		break;
	case RESIDENCY_CNTR_C4_EN:
		offset = regs[APSS_LPM_RESIDENCY_C4_D4_CNTR_n] + (cpu * OFFSET_8BYTES);
		break;
	default:
		pr_err("Unknown mode\n");
		offset = -EINVAL;
	}

	return offset;
}

static int get_cl_residency_read_offset(int ncpu, u8 cl_res_cfg, const u32 *regs)
{
	int offset;

	switch (cl_res_cfg) {
	case RESIDENCY_CNTR_D2_EN:
		offset = regs[APSS_LPM_RESIDENCY_C2_D2_CNTR_n] + OFFSET_8BYTES * ncpu;
		break;
	case RESIDENCY_CNTR_D4_EN:
		offset = regs[APSS_LPM_RESIDENCY_C4_D4_CNTR_n] + OFFSET_8BYTES * ncpu;
		break;
	default:
		pr_err("Unknown mode\n");
		offset = -EINVAL;
	}

	return offset;
}

static int get_cl_lpm_read_offset(int ncpu, u8 cl_mode, const u32 *regs)
{
	int offset;

	switch (cl_mode) {
	case LPM_COUNTER_EN_D1:
		offset = regs[APSS_LPM_COUNTER_CPUx_C1_LO_VAL] + ncpu * OFFSET_8BYTES;
		break;
	case LPM_COUNTER_EN_D2D:
		offset = regs[APSS_LPM_COUNTER_CPUx_C2D_LO_VAL] + ncpu * OFFSET_8BYTES;
		break;
	case LPM_COUNTER_EN_D3:
		offset = regs[APSS_LPM_COUNTER_CPUx_C3_LO_VAL] + ncpu * OFFSET_8BYTES;
		break;
	case LPM_COUNTER_EN_D4:
		offset = regs[APSS_LPM_COUNTER_CPUx_C4_LO_VAL] + ncpu * OFFSET_8BYTES;
		break;
	default:
		pr_err("Unknown mode\n");
		offset = -EINVAL;
	}

	return offset;
}

static int qcom_cpuss_sleep_stats_show(struct seq_file *s, void *d)
{
	void __iomem *reg = (void __iomem *)s->private;
	u64 val;

	val = readq_relaxed(reg);
	seq_printf(s, "%llu\n", val);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(qcom_cpuss_sleep_stats);

static int qcom_cpuss_all_stats_show(struct seq_file *s, void *d)
{
	struct list_head *node1 = (struct list_head *)s->private;
	struct qcom_cpuss_stats *data;
	u64 count;

	list_for_each_entry(data, node1, node) {
		count = readq_relaxed(data->reg);
		seq_printf(s, "%s: %llu\n", data->mode_name, count);
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(qcom_cpuss_all_stats);

static ssize_t qcom_cpuss_reset_clear_lpm_residency(phys_addr_t addr, u32 new_val)
{
	u32 val;
	int ret = 0;

	ret = qcom_scm_io_readl(addr, &val);
	if (ret)
		goto error;

	val |= new_val;
	ret = qcom_scm_io_writel(addr, val);
	if (ret)
		goto error;

	/* clear reset */
	val &= ~new_val;
	ret = qcom_scm_io_writel(addr, val);
	if (ret)
		goto error;

	return 0;
error:
	pr_err("Cannot reset lpm hw count and residency\n");
	return ret;
}

static int check_val(const char __user *in, size_t count)
{
	char buffer[2];

	if (!count || count > sizeof(buffer))
		return -EINVAL;
	if (copy_from_user(buffer, in, count))
		return -EFAULT;
	if (buffer[0] != '1' || (count == 2 && buffer[1] != '\n'))
		return -EINVAL;
	return 0;
}

static ssize_t qcom_cpuss_stats_reset_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *off)
{
	struct inode *in = file->f_inode;
	struct qcom_target_info *t_info;
	int ncpu;
	u8 i, j;
	ssize_t ret;

	t_info = (struct qcom_target_info *)in->i_private;
	ncpu = t_info->ncpu;

	ret = check_val(buffer, count);
	if (ret)
		return ret;

	/* Reset cpu LPM/Residencies */
	mutex_lock(&t_info->stats_reset_lock);
	for (j = 0; j < ncpu; j++) {
		i = t_info->cpu_pcpu_map[j];
		if (i == U8_MAX)
			continue;

		ret = qcom_cpuss_reset_clear_lpm_residency(t_info->per_cpu_lpm_cfg[j],
							   RESET_ALL_CPU_LPM);
		if (ret)
			goto error;

		ret = qcom_cpuss_reset_clear_lpm_residency(t_info->apss_seq_mem_base +
				t_info->offsets[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n] + 0x4 * i,
				CLR_ALL_CPU_RESIDENCY);
		if (ret)
			goto error;
	}

	/* Reset cluster LPM/Residencies */
	ret = qcom_cpuss_reset_clear_lpm_residency(t_info->l3_seq_lpm_cfg,
						   RESET_ALL_CL_LPM);
	if (ret)
		goto error;

	ret = qcom_cpuss_reset_clear_lpm_residency(t_info->apss_seq_mem_base +
			t_info->offsets[APSS_CL_LPM_RESIDENCY_CNTR_CFG], CLR_CL_RESIDENCY);
	if (ret)
		goto error;
	mutex_unlock(&t_info->stats_reset_lock);

	return count;

error:
	mutex_unlock(&t_info->stats_reset_lock);
	return ret;
}

static const struct file_operations qcom_cpuss_stats_reset_fops = {
	.owner		= THIS_MODULE,
	.write		= qcom_cpuss_stats_reset_write,
};

static int store_stats_data(struct qcom_target_info *t_info, char *str,
			     void __iomem *reg)
{
	struct qcom_cpuss_stats *store_stats_data;

	store_stats_data = devm_kzalloc(&t_info->pdev->dev, sizeof(*store_stats_data),
					GFP_KERNEL);
	if (!store_stats_data)
		return -ENOMEM;

	store_stats_data->reg = reg;
	strscpy(store_stats_data->mode_name, str,
		sizeof(store_stats_data->mode_name));

	list_add_tail(&store_stats_data->node, &t_info->complete_stats.node);

	return 0;
}

static int qcom_cpuss_sleep_stats_create_cpu_debugfs(struct qcom_target_info *t_info,
					     u8 cpu, u32 lpm_cfg)
{
	void __iomem *reg, *base;
	u32 offset;
	int bit, ret;
	char cpu_name[CPUNAME_SZ] = {0}, stats_name[STATS_NAME_SZ] = {0}, *state;

	if (cpu >= MAX_POSSIBLE_CPUS)
		return -EINVAL;
	snprintf(cpu_name, sizeof(cpu_name), "pcpu%u", cpu);
	t_info->cpu_dir[cpu] = debugfs_create_dir(cpu_name,
						  t_info->stats_rootdir);

	if (IS_ERR_OR_NULL(t_info->cpu_dir[cpu]))
		return t_info->cpu_dir[cpu] ? PTR_ERR(t_info->cpu_dir[cpu]) : -ENOMEM;

	base = t_info->apss_base;

	for (bit = 0; bit < MAX_CPU_LPM_BITS; bit++) {
		if (lpm_cfg & BIT(bit)) {
			offset = get_cpu_lpm_read_offset(cpu, BIT(bit), t_info->offsets);
			if (offset == -EINVAL)
				return offset;

			reg = base + offset;
			state = get_str_cpu_lpm_state(1 << bit);
			if (state) {
				ret = qcom_cpuss_create_file(state, 0444,
						    t_info->cpu_dir[cpu],
						    (void *) reg,
						    &qcom_cpuss_sleep_stats_fops);
				if (ret)
					return ret;
				snprintf(stats_name, sizeof(stats_name),
					 "pcpu%u: %s", cpu, state);
				ret = store_stats_data(t_info, stats_name, reg);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int qcom_cpuss_sleep_stats_create_cluster_debugfs(struct qcom_target_info *t_info,
						u32 cl_cfg)
{
	void __iomem *reg, *base;
	u32 offset;
	int bit, ret;
	char *state;

	t_info->cl_rootdir = debugfs_create_dir("L3", t_info->stats_rootdir);
	if (IS_ERR_OR_NULL(t_info->cl_rootdir))
		return t_info->cl_rootdir ? PTR_ERR(t_info->cl_rootdir) : -ENOMEM;

	base = t_info->apss_base;

	for (bit = 0; bit < MAX_CL_LPM_BITS; bit++) {
		if (cl_cfg & BIT(bit)) {
			offset = get_cl_lpm_read_offset(t_info->ncpu, BIT(bit), t_info->offsets);
			if (offset == -EINVAL)
				return offset;

			reg = base + offset;
			state = get_str_cl_lpm_state(1 << bit);
			if (state) {
				ret = qcom_cpuss_create_file(state, 0444,
						    t_info->cl_rootdir,
						    (void *) reg,
						    &qcom_cpuss_sleep_stats_fops);
				if (ret)
					return ret;
				ret = store_stats_data(t_info, state, reg);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int qcom_cpuss_sleep_stats_create_cpu_residency_debugfs(struct qcom_target_info *t_info,
						u8 cpu, u32 residency_cfg)
{
	void __iomem *reg, *base;
	u32 offset;
	int bit, ret;
	char stats_name[STATS_NAME_SZ] = {0}, *state;

	base = t_info->apss_base;

	for (bit = 0; bit < MAX_CPU_RESIDENCY_BITS; bit += 2) {
		if (residency_cfg & BIT(bit)) {
			offset = get_cpu_residency_read_offset(cpu, BIT(bit), t_info->offsets);
			if (offset == -EINVAL)
				return offset;

			reg = base + offset;
			state = get_str_cpu_res(1 << bit);
			if (state) {
				ret = qcom_cpuss_create_file(state, 0444,
						    t_info->cpu_dir[cpu],
						    (void *) reg,
						    &qcom_cpuss_sleep_stats_fops);
				if (ret)
					return ret;
				snprintf(stats_name, sizeof(stats_name),
					 "pcpu%u: %s", cpu, state);
				ret = store_stats_data(t_info, stats_name, reg);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int qcom_cpuss_sleep_stats_create_cl_residency_debugfs(struct qcom_target_info *t_info,
						u32 cl_residency_cfg)
{
	void __iomem *reg;
	u32 offset;
	int bit, ret;
	char *state;

	for (bit = 0; bit < MAX_CL_RESIDENCY_BITS; bit += 2) {
		if (cl_residency_cfg & BIT(bit)) {
			offset = get_cl_residency_read_offset(t_info->ncpu,
							      BIT(bit), t_info->offsets);
			if (offset == -EINVAL)
				return offset;

		reg = t_info->apss_base + offset;
		state = get_str_cl_res(1 << bit);
		if (state) {
			ret = qcom_cpuss_create_file(state, 0444, t_info->cl_rootdir,
					    (void *) reg,
					    &qcom_cpuss_sleep_stats_fops);
			if (ret)
				return ret;
			ret = store_stats_data(t_info, state, reg);
			if (ret)
				return ret;
		}
		}
	}

	return 0;
}

/* Only DT-selected HLOS-readable configuration uses MMIO; reset stays secure. */
static int qcom_cpuss_read_cfg(struct qcom_target_info *info, phys_addr_t addr,
			       void __iomem *reg, u32 *val)
{
	if (!info->config_mmio)
		return qcom_scm_io_readl(addr, val);
	*val = readl_relaxed(reg);
	return 0;
}

static int qcom_cpuss_map_cfg(struct qcom_target_info *info)
{
	int cpu;

	if (!info->config_mmio)
		return 0;
	for (cpu = 0; cpu < MAX_POSSIBLE_CPUS; cpu++) {
		if (info->cpu_pcpu_map[cpu] == U8_MAX)
			continue;
		info->per_cpu_cfg_base[cpu] =
			devm_ioremap(&info->pdev->dev, info->per_cpu_lpm_cfg[cpu], sizeof(u32));
		if (!info->per_cpu_cfg_base[cpu])
			return -ENOMEM;
	}
	info->l3_cfg_base = devm_ioremap(&info->pdev->dev, info->l3_seq_lpm_cfg, sizeof(u32));
	return info->l3_cfg_base ? 0 : -ENOMEM;
}

static int qcom_cpuss_read_lpm_and_residency_cfg_informaion(struct qcom_target_info *t_info)
{
	u32 val;
	u8 i, j;
	int ret;
	phys_addr_t addr;

	/* per cpu lpm and residency */
	for (j = 0; j < t_info->ncpu; j++) {
		i = t_info->cpu_pcpu_map[j];
		if (i == U8_MAX)
			continue;

		addr = t_info->per_cpu_lpm_cfg[j];
		ret = qcom_cpuss_read_cfg(t_info, addr, t_info->per_cpu_cfg_base[j], &val);
		if (ret)
			return dev_err_probe(&t_info->pdev->dev, ret, "Cannot read CPU config at %pa\n", &addr);

		ret = qcom_cpuss_sleep_stats_create_cpu_debugfs(t_info, i, val);
		if (ret)
			return ret;

		addr = t_info->apss_seq_mem_base;
		addr += (t_info->offsets[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n] + OFFSET_4BYTES * i);
		ret = qcom_cpuss_read_cfg(t_info, addr, t_info->apss_base +
			t_info->offsets[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n] + OFFSET_4BYTES * i,
			&val);
		if (ret)
			return dev_err_probe(&t_info->pdev->dev, ret, "Cannot read CPU config at %pa\n", &addr);

		ret = qcom_cpuss_sleep_stats_create_cpu_residency_debugfs(t_info, i, val);
		if (ret)
			return ret;
	}

	/* cluster lpm */
	addr = t_info->l3_seq_lpm_cfg;
	ret = qcom_cpuss_read_cfg(t_info, addr, t_info->l3_cfg_base, &val);
	if (ret)
		return dev_err_probe(&t_info->pdev->dev, ret, "Cannot read cluster config at %pa\n", &addr);

	ret = qcom_cpuss_sleep_stats_create_cluster_debugfs(t_info, val);
	if (ret)
		return ret;

	/* cluster residency */
	addr = t_info->apss_seq_mem_base + t_info->offsets[APSS_CL_LPM_RESIDENCY_CNTR_CFG];
	ret = qcom_cpuss_read_cfg(t_info, addr, t_info->apss_base +
				t_info->offsets[APSS_CL_LPM_RESIDENCY_CNTR_CFG], &val);
	if (ret)
		return dev_err_probe(&t_info->pdev->dev, ret, "Cannot read cluster config at %pa\n", &addr);

	ret = qcom_cpuss_sleep_stats_create_cl_residency_debugfs(t_info, val);

	return ret;
}

static int qcom_cpuss_sleep_stats_probe(struct platform_device *pdev)
{
	struct qcom_target_info *info;
	struct resource *res;
	u32 ncpu;
	u64 mpidr;
	unsigned int seen = 0;
	int cpu, pcpu, ret;

	ret = of_property_read_u32(pdev->dev.of_node, "num-cpus", &ncpu);
	if (ret)
		return ret;
	if (ncpu < MIN_POSSIBLE_CPUS || ncpu > MAX_POSSIBLE_CPUS)
		return -EINVAL;
	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->pdev = pdev;
	info->ncpu = ncpu;
	info->config_mmio = of_property_read_bool(pdev->dev.of_node, "qcom,config-mmio");
	info->offsets = device_get_match_data(&pdev->dev);
	if (!info->offsets)
		return -ENODEV;
	INIT_LIST_HEAD(&info->complete_stats.node);
	mutex_init(&info->stats_reset_lock);
	memset(info->cpu_pcpu_map, U8_MAX, sizeof(info->cpu_pcpu_map));
	for_each_possible_cpu(cpu) {
		char reg_name[SEQ_LPM_STR_SZ];

		if (cpu >= MAX_POSSIBLE_CPUS)
			return dev_err_probe(&pdev->dev, -EINVAL, "Logical CPU %d exceeds native capacity\n", cpu);
		/* The boot-time MPIDR map is valid for offline CPUs too. */
		mpidr = cpu_logical_map(cpu);
		if (mpidr == U64_MAX)
			return dev_err_probe(&pdev->dev, -EINVAL, "Invalid MPIDR map for CPU %d\n", cpu);
		pcpu = MPIDR_AFFINITY_LEVEL(mpidr, 1);
		if (pcpu >= MAX_POSSIBLE_CPUS || pcpu >= info->ncpu)
			return dev_err_probe(&pdev->dev, -EINVAL, "Physical CPU %d exceeds native count\n", pcpu);
		snprintf(reg_name, sizeof(reg_name), "seq_lpm_cntr_cfg_cpu%u", pcpu);
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, reg_name);
		if (!res)
			continue;
		if (pcpu >= info->ncpu || (seen & BIT(pcpu)) ||
		    !qcom_cpuss_cfg_resource_valid(res))
			return dev_err_probe(&pdev->dev, -EINVAL, "Duplicate CPU %d or invalid config resource %pr\n", pcpu, res);
		seen |= BIT(pcpu);
		info->cpu_pcpu_map[cpu] = pcpu;
		info->per_cpu_lpm_cfg[cpu] = res->start;
		info->per_cpu_lpm_cfg_size[cpu] = sizeof(u32);
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "apss_seq_mem_base");
	if (!res)
		return -ENODEV;
	if (!qcom_cpuss_cfg_resource_valid(res))
		return dev_err_probe(&pdev->dev, -EINVAL, "Invalid native stats resource %pr\n", res);
	info->apss_seq_mem_base = res->start;
	info->apss_seq_mem_size = resource_size(res);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "l3_seq_lpm_cntr_cfg");
	if (!res)
		return -ENODEV;
	if (!qcom_cpuss_cfg_resource_valid(res))
		return dev_err_probe(&pdev->dev, -EINVAL, "Invalid native stats resource %pr\n", res);
	info->l3_seq_lpm_cfg = res->start;
	info->l3_seq_lpm_size = sizeof(u32);
	ret = qcom_cpuss_validate_offsets(info);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Native counter offsets exceed APSS window\n");
	info->apss_base = devm_ioremap(&pdev->dev, info->apss_seq_mem_base,
				      info->apss_seq_mem_size);
	if (!info->apss_base)
		return -ENOMEM;
	ret = qcom_cpuss_map_cfg(info);
	if (ret)
		return ret;
	info->stats_rootdir = debugfs_create_dir("qcom_cpuss_sleep_stats", NULL);
	if (IS_ERR_OR_NULL(info->stats_rootdir))
		return info->stats_rootdir ? PTR_ERR(info->stats_rootdir) : -ENOMEM;
	ret = qcom_cpuss_read_lpm_and_residency_cfg_informaion(info);
	if (ret)
		goto cleanup;
	ret = qcom_cpuss_create_file("stats", 0444, info->stats_rootdir,
				    &info->complete_stats.node, &qcom_cpuss_all_stats_fops);
	if (ret)
		goto cleanup;
	ret = qcom_cpuss_create_file("reset", 0220, info->stats_rootdir,
				    info, &qcom_cpuss_stats_reset_fops);
	if (ret)
		goto cleanup;
	/* Registered last: debugfs drains before any devres-backed data is freed. */
	ret = devm_add_action_or_reset(&pdev->dev, qcom_cpuss_cleanup, info);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, info);
	return 0;
cleanup:
	qcom_cpuss_cleanup(info);
	return ret;
}

static void qcom_cpuss_sleep_stats_remove(struct platform_device *pdev)
{
	qcom_cpuss_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id qcom_cpuss_stats_table[] = {
		{ .compatible = "qcom,cpuss-sleep-stats", .data = &qcom_cpuss_cntr_v1_offsets },
		{ .compatible = "qcom,cpuss-sleep-stats-v2", .data = &qcom_cpuss_cntr_v2_offsets },
		{ .compatible = "qcom,cpuss-sleep-stats-v3", .data = &qcom_cpuss_cntr_v3_offsets },
		{ },
};

static struct platform_driver qcom_cpuss_sleep_stats = {
	.probe = qcom_cpuss_sleep_stats_probe,
	.remove = qcom_cpuss_sleep_stats_remove,
	.driver	= {
		.name = "qcom_cpuss_sleep_stats",
		.of_match_table	= qcom_cpuss_stats_table,
	},
};

MODULE_DEVICE_TABLE(of, qcom_cpuss_stats_table);
module_platform_driver(qcom_cpuss_sleep_stats);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. (QTI) CPUSS sleep stats driver");
MODULE_LICENSE("GPL");
