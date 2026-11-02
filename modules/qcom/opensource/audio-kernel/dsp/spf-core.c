/* Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
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

#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/kref.h>
#include <linux/atomic.h>
#include <ipc/gpr-lite.h>
#include <dsp/spf-core.h>
#include <dsp/digital-cdc-rsc-mgr.h>

#define Q6_READY_TIMEOUT_MS 1000
#define Q6_CLOSE_ALL_TIMEOUT_MS 5000
#define APM_CMD_GET_SPF_STATE 0x01001021
#define APM_CMD_CLOSE_ALL 0x01001013
#define APM_CMD_RSP_GET_SPF_STATE 0x02001007
#define APM_MODULE_INSTANCE_ID 0x00000001
#define GPR_SVC_ADSP_CORE 0x3
#define ADD_CHILD_DEVICES_APM_TIMEOUT_MS 300000

struct spf_core_private;

struct spf_core {
	struct gpr_device *adev;
	struct spf_core_private *priv;
	wait_queue_head_t wait;
	/* Protect request identity and response publication in the GPR callback. */
	spinlock_t response_lock;
	bool resp_received;
	bool stopping;
	s32 status;
	u32 pending_opcode;
	u32 token;
};

struct spf_core_private {
	struct device *dev;
	struct kref ref;
	/* Serialize commands and protect the bound core's lifetime. */
	struct mutex lock;
	struct spf_core *spf_core_drv;
	bool is_initial_boot;
	bool stopping;
	struct work_struct add_chld_dev_work;
};

static DEFINE_MUTEX(spf_provider_mutex);
static struct spf_core_private *spf_core_priv;
static bool spf_provider_transition;
/* Keep late replies from a withdrawn service out of replacement commands. */
static atomic_t spf_command_token = ATOMIC_INIT(0);

struct spf_cmd_basic_rsp {
	u32 opcode;
	s32 status;
};

struct apm_cmd_rsp_get_spf_status_t {
	u32 status;
};

static void spf_private_release(struct kref *ref)
{
	struct spf_core_private *priv =
		container_of(ref, struct spf_core_private, ref);

	put_device(priv->dev);
	kfree(priv);
}

static struct spf_core_private *spf_private_get(void)
{
	struct spf_core_private *priv;

	mutex_lock(&spf_provider_mutex);
	priv = spf_core_priv;
	if (priv)
		kref_get(&priv->ref);
	mutex_unlock(&spf_provider_mutex);
	return priv;
}

static void spf_private_put(struct spf_core_private *priv)
{
	kref_put(&priv->ref, spf_private_release);
}

static int spf_core_callback(struct gpr_device *adev, void *data)
{
	struct spf_core *core = dev_get_drvdata(&adev->dev);
	struct spf_cmd_basic_rsp basic;
	struct apm_cmd_rsp_get_spf_status_t state;
	struct gpr_hdr *hdr = data;
	unsigned long flags;
	u32 packet_size, header_size, command;
	s32 status;

	if (!core || !hdr)
		return -EINVAL;
	packet_size = GPR_PKT_GET_PACKET_BYTE_SIZE(hdr->header);
	header_size = GPR_PKT_GET_HEADER_BYTE_SIZE(hdr->header);
	if (header_size < GPR_HDR_SIZE || header_size > packet_size)
		return -EINVAL;
	switch (hdr->opcode) {
	case GPR_IBASIC_RSP_RESULT:
		if (packet_size - header_size < sizeof(basic))
			return -EINVAL;
		memcpy(&basic, (u8 *)data + header_size, sizeof(basic));
		command = basic.opcode;
		status = basic.status ? -EREMOTEIO : 0;
		break;
	case APM_CMD_RSP_GET_SPF_STATE:
		if (packet_size - header_size < sizeof(state))
			return -EINVAL;
		memcpy(&state, (u8 *)data + header_size, sizeof(state));
		command = APM_CMD_GET_SPF_STATE;
		status = state.status == 1;
		break;
	default:
		return 0;
	}
	spin_lock_irqsave(&core->response_lock, flags);
	if (READ_ONCE(core->stopping) || !core->pending_opcode ||
	    core->pending_opcode != command || core->token != hdr->token) {
		spin_unlock_irqrestore(&core->response_lock, flags);
		return 0;
	}
	core->status = status;
	core->pending_opcode = 0;
	/* Publish status before the command waiter observes completion. */
	smp_store_release(&core->resp_received, true);
	spin_unlock_irqrestore(&core->response_lock, flags);
	wake_up(&core->wait);
	return 0;
}

static bool spf_response_ready(struct spf_core *core)
{
	/* Pair with callback publication before the waiter consumes status. */
	return smp_load_acquire(&core->resp_received);
}

/* Called with priv->lock held, which also serializes command identities. */
static int spf_run_command(struct spf_core *core, u32 command, int timeout_ms)
{
	struct gpr_pkt pkt = { 0 };
	unsigned long flags;
	long waited;
	int ret;

	if (READ_ONCE(core->stopping) || READ_ONCE(core->priv->stopping))
		return -ENODEV;
	spin_lock_irqsave(&core->response_lock, flags);
	core->token = (u32)atomic_inc_return(&spf_command_token);
	if (!core->token)
		core->token = (u32)atomic_inc_return(&spf_command_token);
	core->pending_opcode = command;
	core->resp_received = false;
	core->status = -EINPROGRESS;
	pkt.hdr.token = core->token;
	spin_unlock_irqrestore(&core->response_lock, flags);
	pkt.hdr.header = GPR_SET_FIELD(GPR_PKT_VERSION, GPR_PKT_VER) |
		GPR_SET_FIELD(GPR_PKT_HEADER_SIZE, GPR_PKT_HEADER_WORD_SIZE_V) |
		GPR_SET_FIELD(GPR_PKT_PACKET_SIZE, GPR_PKT_HEADER_BYTE_SIZE_V);
	pkt.hdr.dst_port = APM_MODULE_INSTANCE_ID;
	pkt.hdr.src_port = GPR_SVC_ADSP_CORE;
	pkt.hdr.dst_domain_id = GPR_IDS_DOMAIN_ID_ADSP_V;
	pkt.hdr.src_domain_id = GPR_IDS_DOMAIN_ID_APPS_V;
	pkt.hdr.opcode = command;
	ret = gpr_send_pkt(core->adev, &pkt);
	if (ret < 0)
		goto finish;
	waited = wait_event_timeout(core->wait,
				    spf_response_ready(core) ||
				    READ_ONCE(core->stopping) ||
				    READ_ONCE(core->priv->stopping),
				    msecs_to_jiffies(timeout_ms));
	if (READ_ONCE(core->stopping) || READ_ONCE(core->priv->stopping))
		ret = -ENODEV;
	else if (waited && spf_response_ready(core))
		ret = READ_ONCE(core->status);
	else
		ret = -ETIMEDOUT;
finish:
	spin_lock_irqsave(&core->response_lock, flags);
	core->pending_opcode = 0;
	core->resp_received = false;
	spin_unlock_irqrestore(&core->response_lock, flags);
	return ret;
}

bool spf_core_is_apm_ready(int timeout_ms)
{
	struct spf_core_private *priv;
	struct spf_core *core;
	unsigned long timeout;
	bool ready = false;

	if (timeout_ms < 0)
		return false;
	priv = spf_private_get();
	if (!priv)
		return false;
	mutex_lock(&priv->lock);
	core = priv->spf_core_drv;
	if (!core || READ_ONCE(priv->stopping))
		goto unlock;
	timeout = jiffies + msecs_to_jiffies(timeout_ms);
	for (;;) {
		if (spf_run_command(core, APM_CMD_GET_SPF_STATE,
				    Q6_READY_TIMEOUT_MS) == 1) {
			ready = true;
			break;
		}
		if (!timeout_ms || READ_ONCE(core->stopping) ||
		    READ_ONCE(priv->stopping) || time_after_eq(jiffies, timeout))
			break;
		usleep_range(50000, 50050);
	}
unlock:
	mutex_unlock(&priv->lock);
	spf_private_put(priv);
	return ready;
}
EXPORT_SYMBOL_GPL(spf_core_is_apm_ready);

void spf_core_apm_close_all(void)
{
	struct spf_core_private *priv = spf_private_get();
	int ret;

	if (!priv)
		return;
	mutex_lock(&priv->lock);
	if (priv->spf_core_drv && !READ_ONCE(priv->stopping)) {
		ret = spf_run_command(priv->spf_core_drv, APM_CMD_CLOSE_ALL,
				      Q6_CLOSE_ALL_TIMEOUT_MS);
		if (ret)
			dev_err_ratelimited(priv->dev, "close-all failed: %d\n", ret);
	}
	mutex_unlock(&priv->lock);
	spf_private_put(priv);
}
EXPORT_SYMBOL_GPL(spf_core_apm_close_all);

static int spf_core_probe(struct gpr_device *adev)
{
	struct spf_core_private *priv = spf_private_get();
	struct spf_core *core;
	int ret = 0;

	if (!priv)
		return -EPROBE_DEFER;
	mutex_lock(&priv->lock);
	if (priv->spf_core_drv || READ_ONCE(priv->stopping)) {
		ret = -EBUSY;
		goto unlock;
	}
	core = kzalloc(sizeof(*core), GFP_KERNEL);
	if (!core) {
		ret = -ENOMEM;
		goto unlock;
	}
	core->adev = adev;
	core->priv = priv;
	spin_lock_init(&core->response_lock);
	init_waitqueue_head(&core->wait);
	dev_set_drvdata(&adev->dev, core);
	priv->spf_core_drv = core;
	if (priv->is_initial_boot)
		schedule_work(&priv->add_chld_dev_work);
unlock:
	mutex_unlock(&priv->lock);
	/* Successful service binding owns this private reference until remove. */
	if (ret)
		spf_private_put(priv);
	return ret;
}

static int spf_core_exit(struct gpr_device *adev)
{
	struct spf_core *core = dev_get_drvdata(&adev->dev);
	struct spf_core_private *priv;

	if (!core)
		return 0;
	priv = core->priv;
	/* Stop and wake the waiter before acquiring its command/lifetime mutex. */
	WRITE_ONCE(core->stopping, true);
	wake_up(&core->wait);
	mutex_lock(&priv->lock);
	if (priv->spf_core_drv == core)
		priv->spf_core_drv = NULL;
	dev_set_drvdata(&adev->dev, NULL);
	mutex_unlock(&priv->lock);
	kfree(core);
	spf_private_put(priv);
	return 0;
}

static const struct of_device_id spf_core_device_id[] = {
	{ .compatible = "qcom,spf_core" },
	{},
};
MODULE_DEVICE_TABLE(of, spf_core_device_id);

static struct gpr_driver qcom_spf_core_driver = {
	.probe = spf_core_probe,
	.remove = spf_core_exit,
	.callback = spf_core_callback,
	.driver = {
		.name = "qcom-spf_core",
		.of_match_table = of_match_ptr(spf_core_device_id),
	},
};

static void spf_core_add_child_devices(struct work_struct *work)
{
	struct spf_core_private *priv =
		container_of(work, struct spf_core_private, add_chld_dev_work);
	int ret;

	if (!spf_core_is_apm_ready(ADD_CHILD_DEVICES_APM_TIMEOUT_MS) ||
	    READ_ONCE(priv->stopping))
		return;
	ret = of_platform_populate(priv->dev->of_node, NULL, NULL, priv->dev);
	if (ret) {
		of_platform_depopulate(priv->dev);
		dev_err(priv->dev, "failed to populate children: %d\n", ret);
		return;
	}
	mutex_lock(&priv->lock);
	priv->is_initial_boot = false;
	mutex_unlock(&priv->lock);
}

static int spf_core_platform_driver_probe(struct platform_device *pdev)
{
	struct spf_core_private *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = get_device(&pdev->dev);
	kref_init(&priv->ref);
	mutex_init(&priv->lock);
	INIT_WORK(&priv->add_chld_dev_work, spf_core_add_child_devices);
	priv->is_initial_boot = true;
	mutex_lock(&spf_provider_mutex);
	if (spf_core_priv || spf_provider_transition) {
		mutex_unlock(&spf_provider_mutex);
		spf_private_put(priv);
		return -EBUSY;
	}
	digital_cdc_rsc_mgr_init();
	spf_core_priv = priv;
	platform_set_drvdata(pdev, priv);
	mutex_unlock(&spf_provider_mutex);
	ret = gpr_driver_register(&qcom_spf_core_driver);
	if (!ret)
		return 0;
	mutex_lock(&spf_provider_mutex);
	spf_core_priv = NULL;
	spf_provider_transition = true;
	WRITE_ONCE(priv->stopping, true);
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&spf_provider_mutex);
	cancel_work_sync(&priv->add_chld_dev_work);
	digital_cdc_rsc_mgr_exit();
	mutex_lock(&spf_provider_mutex);
	spf_provider_transition = false;
	mutex_unlock(&spf_provider_mutex);
	spf_private_put(priv);
	return ret;
}

static void spf_core_platform_driver_remove(struct platform_device *pdev)
{
	struct spf_core_private *priv = platform_get_drvdata(pdev);

	mutex_lock(&spf_provider_mutex);
	spf_core_priv = NULL;
	spf_provider_transition = true;
	WRITE_ONCE(priv->stopping, true);
	mutex_unlock(&spf_provider_mutex);
	cancel_work_sync(&priv->add_chld_dev_work);
	of_platform_depopulate(priv->dev);
	gpr_driver_unregister(&qcom_spf_core_driver);
	digital_cdc_rsc_mgr_exit();
	platform_set_drvdata(pdev, NULL);
	mutex_lock(&spf_provider_mutex);
	spf_provider_transition = false;
	mutex_unlock(&spf_provider_mutex);
	spf_private_put(priv);
}

static const struct of_device_id spf_core_of_match[] = {
	{ .compatible = "qcom,spf-core-platform" },
	{},
};

static struct platform_driver spf_core_driver = {
	.probe = spf_core_platform_driver_probe,
	.remove = spf_core_platform_driver_remove,
	.driver = {
		.name = "spf-core-platform",
		.owner = THIS_MODULE,
		.of_match_table = spf_core_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(spf_core_driver);
MODULE_DESCRIPTION("q6 core");
MODULE_LICENSE("GPL v2");
