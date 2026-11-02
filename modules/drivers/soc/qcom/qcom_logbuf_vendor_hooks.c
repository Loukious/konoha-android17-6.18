// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "logbuf_vh: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <soc/qcom/minidump.h>
#include <linux/kallsyms.h>

#include "../../../kernel/printk/printk_ringbuffer.h"
#include "debug_symbol.h"

static void register_log_minidump(struct printk_ringbuffer *prb)
{
	struct prb_desc_ring descring;
	struct prb_data_ring textdata_ring;
	struct prb_desc *descaddr;
	struct printk_info *p_infos;
	struct md_region md_entry;
	int ret;
	static bool log_buf_registered;

	if (log_buf_registered || !prb)
		return;

	log_buf_registered = true;
	descring = prb->desc_ring;
	textdata_ring = prb->text_data_ring;
	descaddr = descring.descs;
	p_infos = descring.infos;

	strscpy(md_entry.name, "KLOGBUF", sizeof(md_entry.name));
	md_entry.virt_addr = (uintptr_t)textdata_ring.data;
	md_entry.phys_addr = virt_to_phys(textdata_ring.data);
	md_entry.size = _DATA_SIZE(textdata_ring.size_bits);
	ret = msm_minidump_add_region(&md_entry);
	if (ret < 0)
		pr_err("Failed to add log_text entry in minidump table\n");

	strscpy(md_entry.name, "LOG_DESC", sizeof(md_entry.name));
	md_entry.virt_addr = (uintptr_t)descaddr;
	md_entry.phys_addr = virt_to_phys(descaddr);
	md_entry.size = sizeof(struct prb_desc) * _DESCS_COUNT(descring.count_bits);
	ret = msm_minidump_add_region(&md_entry);
	if (ret < 0)
		pr_err("Failed to add log_desc entry in minidump table\n");

	strscpy(md_entry.name, "LOG_INFO", sizeof(md_entry.name));
	md_entry.virt_addr = (uintptr_t)p_infos;
	md_entry.phys_addr = virt_to_phys(p_infos);
	md_entry.size = sizeof(struct printk_info) * _DESCS_COUNT(descring.count_bits);
	ret = msm_minidump_add_region(&md_entry);
	if (ret < 0)
		pr_err("Failed to add log_info entry in minidump table\n");
}

static int logbuf_vh_probe(struct platform_device *pdev)
{
	struct printk_ringbuffer *prb = NULL;
	struct printk_ringbuffer **prb_ptr;

	if (!debug_symbol_available())
		return -EPROBE_DEFER;

	prb_ptr = DEBUG_SYMBOL_LOOKUP(prb);

	if (!prb_ptr)
		return -ENOENT;
	prb = READ_ONCE(*prb_ptr);
	register_log_minidump(prb);

	return 0;
}

static struct platform_driver logbuf_vh_driver = {
	.probe = logbuf_vh_probe,
	.driver = {
		.name = "qcom-logbuf-vh",
		.suppress_bind_attrs = true,
	},
};

static int __init logbuf_vh_driver_init(void)
{
	struct platform_device *pdev;
	int ret;

	ret = platform_driver_register(&logbuf_vh_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("qcom-logbuf-vh", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&logbuf_vh_driver);
		return PTR_ERR(pdev);
	}
	return 0;
}
module_init(logbuf_vh_driver_init);

MODULE_DESCRIPTION("QCOM Logbuf Vendor Hooks Driver");
MODULE_LICENSE("GPL");
