// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2019, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_address.h>

struct boot_stats {
	uint32_t bootloader_start;
	uint32_t bootloader_end;
	uint32_t bootloader_load_boot_start;
	uint32_t bootloader_load_boot_end;
	uint32_t bootloader_load_vendor_boot_start;
	uint32_t bootloader_load_vendor_boot_end;
	uint32_t bootloader_load_init_boot_start;
	uint32_t bootloader_load_init_boot_end;
} __packed;

static void __iomem *mpm_counter_base;
static uint32_t mpm_counter_freq;
static struct boot_stats __iomem *boot_stats;

static int mpm_parse_dt(void)
{
	struct device_node *np_imem = NULL, *np_mpm2 = NULL;
	struct resource res;
	int ret = -ENODEV;

	np_imem = of_find_compatible_node(NULL, NULL, "qcom,msm-imem-boot_stats");
	if (!np_imem || !of_device_is_available(np_imem))
		goto out;
	ret = of_address_to_resource(np_imem, 0, &res);
	if (ret)
		goto out;
	if (resource_size(&res) < sizeof(*boot_stats)) {
		ret = -EINVAL;
		goto out;
	}
	boot_stats = of_iomap(np_imem, 0);
	if (!boot_stats) {
		ret = -ENOMEM;
		goto out;
	}

	np_mpm2 = of_find_compatible_node(NULL, NULL, "qcom,mpm2-sleep-counter");
	if (!np_mpm2 || !of_device_is_available(np_mpm2)) {
		ret = -ENODEV;
		goto out;
	}
	ret = of_property_read_u32(np_mpm2, "clock-frequency", &mpm_counter_freq);
	if (ret)
		goto out;
	if (!mpm_counter_freq) {
		ret = -EINVAL;
		goto out;
	}
	ret = of_address_to_resource(np_mpm2, 0, &res);
	if (ret)
		goto out;
	if (resource_size(&res) < sizeof(u32)) {
		ret = -EINVAL;
		goto out;
	}
	mpm_counter_base = of_iomap(np_mpm2, 0);
	if (!mpm_counter_base) {
		ret = -ENOMEM;
		goto out;
	}
	ret = 0;
out:
	of_node_put(np_mpm2);
	of_node_put(np_imem);
	if (ret && boot_stats) {
		iounmap(boot_stats);
		boot_stats = NULL;
	}
	return ret;
}

static void print_boot_stats(void)
{
	pr_info("KPI: Bootloader start count = %u\n",
		readl_relaxed(&boot_stats->bootloader_start));
	pr_info("KPI: Bootloader end count = %u\n",
		readl_relaxed(&boot_stats->bootloader_end));
	pr_info("KPI: Bootloader load kernel count = %u\n",
		readl_relaxed(&boot_stats->bootloader_load_boot_end) -
		readl_relaxed(&boot_stats->bootloader_load_boot_start));
	pr_info("KPI: Kernel MPM timestamp = %u\n",
		readl_relaxed(mpm_counter_base));
	pr_info("KPI: Kernel MPM Clock frequency = %u\n",
		mpm_counter_freq);
}

static int __init boot_stats_init(void)
{
	int ret;

	ret = mpm_parse_dt();
	if (ret < 0)
		return ret;

	print_boot_stats();
	iounmap(boot_stats);
	iounmap(mpm_counter_base);
	boot_stats = NULL;
	mpm_counter_base = NULL;

	return 0;
}
module_init(boot_stats_init);

static void __exit boot_stats_exit(void)
{
}
module_exit(boot_stats_exit);

MODULE_DESCRIPTION("MSM boot stats info driver");
MODULE_LICENSE("GPL");
