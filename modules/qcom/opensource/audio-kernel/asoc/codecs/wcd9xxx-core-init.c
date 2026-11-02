// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <asoc/msm-cdc-pinctrl.h>
#include <asoc/wcd9xxx-irq.h>
#include <asoc/core.h>

static int __init wcd9xxx_core_init(void)
{
	int ret;

	ret = msm_cdc_pinctrl_drv_init();
	if (ret)
		return ret;
	ret = wcd9xxx_irq_drv_init();
	if (ret)
		goto unregister_pinctrl;
	ret = wcd9xxx_init();
	if (!ret)
		return 0;

	wcd9xxx_irq_drv_exit();
unregister_pinctrl:
	msm_cdc_pinctrl_drv_exit();
	return ret;
}
module_init(wcd9xxx_core_init);

static void __exit wcd9xxx_core_exit(void)
{
	wcd9xxx_exit();
	wcd9xxx_irq_drv_exit();
	msm_cdc_pinctrl_drv_exit();
}
module_exit(wcd9xxx_core_exit);

MODULE_DESCRIPTION("WCD9XXX CODEC core init driver");
MODULE_LICENSE("GPL v2");
