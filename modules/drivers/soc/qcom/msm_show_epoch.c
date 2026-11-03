// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <trace/hooks/epoch.h>

static void msm_show_suspend_epoch_val(void *data, u64 ns, u64 cycles)
{
	pr_info("suspend ns:%17llu      suspend cycles:%17llu\n",
						ns, cycles);
}

static void msm_show_resume_epoch_val(void *data, u64 cycles)
{
	pr_info("resume cycles:%17llu\n", cycles);
}

static int __init msm_show_epoch_init(void)
{
	int ret;

	ret = register_trace_android_vh_show_suspend_epoch_val(
					msm_show_suspend_epoch_val, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_show_resume_epoch_val(
					msm_show_resume_epoch_val, NULL);
	if (ret) {
		unregister_trace_android_vh_show_suspend_epoch_val(
					msm_show_suspend_epoch_val, NULL);
		tracepoint_synchronize_unregister();
		return ret;
	}

	return 0;
}

#if IS_MODULE(CONFIG_SHOW_SUSPEND_EPOCH)
module_init(msm_show_epoch_init);
#else
pure_initcall(msm_show_epoch_init);
#endif

static void __exit msm_show_epoch_exit(void)
{
	unregister_trace_android_vh_show_suspend_epoch_val(
					msm_show_suspend_epoch_val, NULL);
	unregister_trace_android_vh_show_resume_epoch_val(
					msm_show_resume_epoch_val, NULL);
	tracepoint_synchronize_unregister();
}
module_exit(msm_show_epoch_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. show epoch values driver");
MODULE_LICENSE("GPL");
