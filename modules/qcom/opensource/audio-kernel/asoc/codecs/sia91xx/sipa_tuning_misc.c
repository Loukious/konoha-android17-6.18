/*
 * Copyright (C) 2022, SI-IN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/current.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "sipa_tuning_if.h"

/*
 up:     pc -> kernel -> hal -> dsp
    vdd/cmd -> kernel -> hal -> dsp

 down:   dsp -> hal -> kernel -> pc
*/
#define DEVICE_NAME_CMD "sipa_cmd"
#define DEVICE_NAME_TOOL "sipa_tool"

sipa_turning_t *g_sipa_turning = NULL;

struct miscdevice sipa_cmd_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME_CMD,
    .fops = &sipa_turning_cmd_fops,
};

struct miscdevice sipa_tool_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME_TOOL,
    .fops = &sipa_turning_tool_fops,
};

static void sipa_queue_init(sipa_sync_t *queue)
{
	init_waitqueue_head(&queue->wq);
	mutex_init(&queue->lock);
	queue->k = MAX_SIZE;
}

static int __init sipa_tuning_if_init(void)
{
	int ret = 0;
	sipa_turning_t *priv = NULL;

	pr_info("[ info] %s: run\n", __func__);
	priv = kzalloc(sizeof(sipa_turning_t), GFP_KERNEL);
	if (priv == NULL) {
		pr_err("[  err] %s: kmalloc failed \n", __func__);
		return -ENOMEM;
	}

	sipa_queue_init(&priv->toolup);
	sipa_queue_init(&priv->tooldown);
	sipa_queue_init(&priv->cmdup);
	sipa_queue_init(&priv->cmddown);

	ret = misc_register(&sipa_cmd_dev);
	if (ret) {
		pr_err("[  err] %s: err\n", __func__);
		goto err1;
	}

	ret = misc_register(&sipa_tool_dev);
	if (ret) {
		pr_err("[  err] %s: err\n", __func__);
		goto err2;
	}
	/* Both devices reject opens until all registration can no longer fail. */
	WRITE_ONCE(g_sipa_turning, priv);

	pr_info("[ info] %s: success\n", __func__);

	return 0;
err2:
	misc_deregister(&sipa_cmd_dev);
err1:
	if (priv) {
		kfree(priv);
		priv = NULL;
	}
	return ret;
}

static void __exit sipa_tuning_if_exit(void)
{
	pr_info("[ info] %s: run\n", __func__);

	/* The fops owner retains this module while a tuning descriptor is open. */
	misc_deregister(&sipa_tool_dev);
	misc_deregister(&sipa_cmd_dev);
	if (g_sipa_turning) {
		mutex_destroy(&(g_sipa_turning->toolup.lock));
		mutex_destroy(&(g_sipa_turning->tooldown.lock));
		mutex_destroy(&(g_sipa_turning->cmdup.lock));
		mutex_destroy(&(g_sipa_turning->cmddown.lock));

		kfree(g_sipa_turning);
		g_sipa_turning = NULL;
	}
}

module_init(sipa_tuning_if_init);
module_exit(sipa_tuning_if_exit);
MODULE_DESCRIPTION("SI-IN native amplifier tuning transport");
MODULE_LICENSE("GPL");
