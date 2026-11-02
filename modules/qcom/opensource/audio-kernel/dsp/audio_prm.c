// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <ipc/gpr-lite.h>
#include <dsp/audio_prm.h>
#include <dsp/spf-core.h>
#include <dsp/audio_notifier.h>

#define TIMEOUT_MS 450
#define MAX_RETRY_COUNT 3
#define APM_READY_WAIT_DURATION 2
#define GPR_SEND_PKT_APM_TIMEOUT_MS 0

struct audio_prm {
	struct gpr_device *adev;
	bool available;
	bool accept_notifier;
	bool is_adsp_up;
	bool apm_ready;
	bool waiting;
	bool received;
	u32 epoch;
	u32 token;
	u32 opcode;
	u32 param;
	int result;
	int lpi_pcm_logging_enable;
	u32 prm_sleep_api_supported;
};

/* These objects must exist before synchronous GPR/notifier registration. */
static struct audio_prm g_prm;
static DEFINE_MUTEX(prm_binding_lock);
static DEFINE_MUTEX(prm_command_lock);
static DEFINE_SPINLOCK(prm_response_lock);
static DECLARE_WAIT_QUEUE_HEAD(prm_wait);
static atomic_t prm_command_token = ATOMIC_INIT(0);

static bool prm_response_ready(u32 epoch)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&prm_response_lock, flags);
	ready = g_prm.received || !g_prm.available || !g_prm.is_adsp_up ||
		g_prm.epoch != epoch;
	spin_unlock_irqrestore(&prm_response_lock, flags);
	return ready;
}

static int audio_prm_callback(struct gpr_device *adev, void *data)
{
	struct gpr_hdr *hdr = data;
	u32 words[2], header, bytes, response;
	unsigned long flags;

	if (!hdr || dev_get_drvdata(&adev->dev) != &g_prm)
		return -ENODEV;
	header = GPR_PKT_GET_HEADER_BYTE_SIZE(hdr->header);
	bytes = GPR_PKT_GET_PACKET_BYTE_SIZE(hdr->header);
	if (header < GPR_HDR_SIZE || header > bytes || bytes - header < sizeof(words))
		return -EINVAL;
	memcpy(words, (u8 *)data + header, sizeof(words));
	spin_lock_irqsave(&prm_response_lock, flags);
	if (!g_prm.available || !g_prm.is_adsp_up || !g_prm.waiting ||
	    g_prm.received || hdr->token != g_prm.token)
		goto unlock;
	response = g_prm.opcode == PRM_CMD_REQUEST_HW_RSC ?
		PRM_CMD_RSP_REQUEST_HW_RSC : PRM_CMD_RSP_RELEASE_HW_RSC;
	if (hdr->opcode == GPR_IBASIC_RSP_RESULT) {
		if (words[0] != g_prm.opcode)
			goto unlock;
	} else if (hdr->opcode != response || words[0] != g_prm.param) {
		goto unlock;
	}
	g_prm.result = words[1] ? -EINVAL : 0;
	g_prm.received = true;
	spin_unlock_irqrestore(&prm_response_lock, flags);
	wake_up(&prm_wait);
	return 0;
unlock:
	spin_unlock_irqrestore(&prm_response_lock, flags);
	return 0;
}

static int prm_check_ready(u32 epoch)
{
	unsigned long flags;
	bool ready, online;
	unsigned int retry;

	for (retry = 0; retry < MAX_RETRY_COUNT; retry++) {
		spin_lock_irqsave(&prm_response_lock, flags);
		online = g_prm.available && g_prm.is_adsp_up && g_prm.epoch == epoch;
		ready = g_prm.apm_ready;
		spin_unlock_irqrestore(&prm_response_lock, flags);
		if (!online || gpr_get_q6_state() != GPR_SUBSYS_LOADED)
			return -ENETRESET;
		if (ready)
			return 0;
		if (spf_core_is_apm_ready(GPR_SEND_PKT_APM_TIMEOUT_MS)) {
			spin_lock_irqsave(&prm_response_lock, flags);
			online = g_prm.available && g_prm.is_adsp_up && g_prm.epoch == epoch;
			if (online)
				g_prm.apm_ready = true;
			spin_unlock_irqrestore(&prm_response_lock, flags);
			return online ? 0 : -ENETRESET;
		}
		if (retry + 1 < MAX_RETRY_COUNT)
			msleep(APM_READY_WAIT_DURATION);
	}
	return -ETIMEDOUT;
}

static int prm_send(u32 param, const void *payload, size_t payload_size, bool enable)
{
	apm_module_param_data_t module = { 0 };
	apm_cmd_header_t command = { 0 };
	struct gpr_pkt *pkt;
	unsigned long flags;
	u32 epoch, value;
	size_t size;
	long waited;
	int ret;

	mutex_lock(&prm_command_lock);
	if (!g_prm.adev) {
		ret = -ENODEV;
		goto unlock;
	}
	spin_lock_irqsave(&prm_response_lock, flags);
	epoch = g_prm.epoch;
	spin_unlock_irqrestore(&prm_response_lock, flags);
	ret = prm_check_ready(epoch);
	if (ret)
		goto unlock;
	/* Both native core and CPU-LPR requests carry one u32 resource value. */
	if (param == PARAM_ID_RSC_HW_CORE && payload_size == sizeof(value)) {
		memcpy(&value, payload, sizeof(value));
		if (value == HW_CORE_ID_LPASS && g_prm.prm_sleep_api_supported) {
			param = PARAM_ID_RSC_CPU_LPR;
			value = LPR_CPU_SS_SLEEP_DISABLED;
			payload = &value;
		}
	}
	size = GPR_HDR_SIZE + sizeof(command) + sizeof(module) + payload_size;
	pkt = kzalloc(size, GFP_KERNEL);
	if (!pkt) {
		ret = -ENOMEM;
		goto unlock;
	}
	pkt->hdr.header = GPR_SET_FIELD(GPR_PKT_VERSION, GPR_PKT_VER) |
		GPR_SET_FIELD(GPR_PKT_HEADER_SIZE, GPR_PKT_HEADER_WORD_SIZE_V) |
		GPR_SET_FIELD(GPR_PKT_PACKET_SIZE, size);
	pkt->hdr.src_port = GPR_SVC_ASM;
	pkt->hdr.dst_port = PRM_MODULE_INSTANCE_ID;
	pkt->hdr.dst_domain_id = GPR_IDS_DOMAIN_ID_ADSP_V;
	pkt->hdr.src_domain_id = GPR_IDS_DOMAIN_ID_APPS_V;
	pkt->hdr.opcode = enable ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC;
	pkt->hdr.token = (u32)atomic_inc_return(&prm_command_token);
	if (!pkt->hdr.token)
		pkt->hdr.token = (u32)atomic_inc_return(&prm_command_token);
	command.payload_size = sizeof(module) + payload_size;
	module.module_instance_id = PRM_MODULE_INSTANCE_ID;
	module.param_id = param;
	module.param_size = payload_size;
	memcpy(pkt->payload, &command, sizeof(command));
	memcpy(pkt->payload + sizeof(command), &module, sizeof(module));
	memcpy(pkt->payload + sizeof(command) + sizeof(module), payload, payload_size);
	spin_lock_irqsave(&prm_response_lock, flags);
	if (!g_prm.available || !g_prm.is_adsp_up || g_prm.epoch != epoch) {
		spin_unlock_irqrestore(&prm_response_lock, flags);
		ret = -ENETRESET;
		goto free;
	}
	g_prm.token = pkt->hdr.token;
	g_prm.opcode = pkt->hdr.opcode;
	g_prm.param = param;
	g_prm.result = -ETIMEDOUT;
	g_prm.received = false;
	g_prm.waiting = true;
	spin_unlock_irqrestore(&prm_response_lock, flags);
	ret = gpr_send_pkt(g_prm.adev, pkt);
	if (ret >= 0) {
		waited = wait_event_timeout(prm_wait, prm_response_ready(epoch),
					    msecs_to_jiffies(TIMEOUT_MS));
		spin_lock_irqsave(&prm_response_lock, flags);
		if (!g_prm.available || !g_prm.is_adsp_up || g_prm.epoch != epoch)
			ret = -ENETRESET;
		else if (!waited && !g_prm.received)
			ret = -ETIMEDOUT;
		else
			ret = g_prm.result;
		spin_unlock_irqrestore(&prm_response_lock, flags);
	}
	spin_lock_irqsave(&prm_response_lock, flags);
	g_prm.waiting = false;
	spin_unlock_irqrestore(&prm_response_lock, flags);
free:
	kfree(pkt);
unlock:
	mutex_unlock(&prm_command_lock);
	return ret;
}

void audio_prm_set_lpi_logging_status(int enabled)
{
	WRITE_ONCE(g_prm.lpi_pcm_logging_enable, enabled);
}
EXPORT_SYMBOL(audio_prm_set_lpi_logging_status);

static int _audio_prm_set_lpass_hw_core_req(u32 hw_core_id, u8 enable)
{
	return prm_send(PARAM_ID_RSC_HW_CORE, &hw_core_id, sizeof(hw_core_id), enable);
}

int audio_prm_set_lpass_hw_core_req(struct clk_cfg *cfg, u32 hw_core_id, u8 enable)
{
	return _audio_prm_set_lpass_hw_core_req(hw_core_id, enable);
}
EXPORT_SYMBOL(audio_prm_set_lpass_hw_core_req);

int audio_prm_set_vote_against_sleep(u8 enable)
{
	return _audio_prm_set_lpass_hw_core_req(HW_CORE_ID_LPASS, enable);
}
EXPORT_SYMBOL(audio_prm_set_vote_against_sleep);

int audio_prm_set_lpass_clk_cfg(struct clk_cfg *cfg, u8 enable)
{
	prm_cmd_request_rsc_t request = { 0 };
	prm_cmd_release_rsc_t release = { 0 };

	if (!cfg)
		return -EINVAL;
	if (!enable) {
		release.num_clk_id_t.num_clock_id = MAX_AUD_HW_CLK_NUM_REQ;
		release.clock_ids_t[0].clock_id = cfg->clk_id;
		return prm_send(PARAM_ID_RSC_AUDIO_HW_CLK, (u8 *)&release +
				 offsetof(prm_cmd_release_rsc_t, num_clk_id_t),
				 sizeof(release) - sizeof(apm_cmd_header_t) -
				 sizeof(apm_module_param_data_t), false);
	}
	request.num_clk_id_t.num_clock_id = MAX_AUD_HW_CLK_NUM_REQ;
	request.clock_ids_t[0].clock_id = cfg->clk_id;
	request.clock_ids_t[0].clock_freq = cfg->clk_freq_in_hz;
	request.clock_ids_t[0].clock_attri = cfg->clk_attri;
	if (READ_ONCE(g_prm.lpi_pcm_logging_enable) &&
	    (cfg->clk_id == CLOCK_ID_TX_CORE_MCLK || cfg->clk_id == CLOCK_ID_WSA_CORE_TX_MCLK ||
	     cfg->clk_id == CLOCK_ID_WSA2_CORE_TX_MCLK || cfg->clk_id == CLOCK_ID_RX_CORE_TX_MCLK))
		request.clock_ids_t[0].clock_root = CLOCK_ROOT_SRC_RCO;
	else
		request.clock_ids_t[0].clock_root = cfg->clk_root;
	return prm_send(PARAM_ID_RSC_AUDIO_HW_CLK, (u8 *)&request +
			 offsetof(prm_cmd_request_rsc_t, num_clk_id_t),
			 sizeof(request) - sizeof(apm_cmd_header_t) -
			 sizeof(apm_module_param_data_t), true);
}
EXPORT_SYMBOL(audio_prm_set_lpass_clk_cfg);

int audio_prm_set_cdc_earpa_duty_cycling_req(struct prm_earpa_hw_intf_config *cfg,
					     u32 enable)
{
	audio_hw_codec_reg_info_t info = { 0 };
	audio_hw_codec_reg_op_info_t *digital = &info.hw_codec_reg[0];
	audio_hw_codec_reg_op_info_t *analog = &info.hw_codec_reg[1];

	if (!cfg)
		return -EINVAL;
	info.num_reg_info_t = MAX_EARPA_REG;
	digital->hw_codec_reg_id = HW_CODEC_DIG_REG_ID_MUTE_CTRL;
	digital->hw_codec_reg_addr_lsw = cfg->ear_pa_hw_reg_cfg.lpass_cdc_rx0_rx_path_ctl_phy_addr;
	digital->num_ops = MAX_EARPA_CDC_DUTY_CYC_OPERATION;
	digital->hw_codec_op[0].hw_codec_op_id = HW_CODEC_OP_DIG_MUTE_ENABLE;
	digital->hw_codec_op[0].hw_codec_op_value = DIG_MUTE_ENABLE;
	digital->hw_codec_op[1].hw_codec_op_id = HW_CODEC_OP_DIG_MUTE_DISABLE;
	digital->hw_codec_op[1].hw_codec_op_value = DIG_MUTE_DISABLE;
	analog->hw_codec_reg_id = HW_CODEC_ANALOG_REG_ID_CMD_FIFO_WRITE;
	analog->hw_codec_reg_addr_lsw = cfg->ear_pa_hw_reg_cfg.lpass_wr_fifo_reg_phy_addr;
	analog->num_ops = MAX_EARPA_CDC_DUTY_CYC_OPERATION;
	analog->hw_codec_op[0].hw_codec_op_id = HW_CODEC_OP_ANA_PGA_ENABLE;
	analog->hw_codec_op[0].hw_codec_op_value = cfg->ear_pa_pkd_cfg.ear_pa_enable_pkd_reg_addr;
	analog->hw_codec_op[1].hw_codec_op_id = HW_CODEC_OP_ANA_PGA_DISABLE;
	analog->hw_codec_op[1].hw_codec_op_value = cfg->ear_pa_pkd_cfg.ear_pa_disable_pkd_reg_addr;
	return prm_send(PARAM_ID_RSC_HW_CODEC_REG_INFO, &info, sizeof(info), enable);
}
EXPORT_SYMBOL(audio_prm_set_cdc_earpa_duty_cycling_req);

static int audio_prm_service_cb(struct notifier_block *nb, unsigned long opcode, void *data)
{
	struct audio_notifier_cb_data *event = data;
	unsigned long flags;

	if (!event || event->domain != AUDIO_NOTIFIER_ADSP_DOMAIN ||
	    (opcode != AUDIO_NOTIFIER_SERVICE_DOWN && opcode != AUDIO_NOTIFIER_SERVICE_UP))
		return NOTIFY_OK;
	spin_lock_irqsave(&prm_response_lock, flags);
	if (g_prm.accept_notifier) {
		g_prm.is_adsp_up = opcode == AUDIO_NOTIFIER_SERVICE_UP;
		g_prm.apm_ready = false;
		g_prm.epoch++;
		if (g_prm.waiting) {
			g_prm.result = -ENETRESET;
			g_prm.received = true;
		}
	}
	spin_unlock_irqrestore(&prm_response_lock, flags);
	wake_up(&prm_wait);
	return NOTIFY_OK;
}

static struct notifier_block service_nb = {
	.notifier_call = audio_prm_service_cb,
	.priority = -INT_MAX,
};

static int audio_prm_probe(struct gpr_device *adev)
{
	unsigned long flags;
	u32 supported = 0;
	int ret;

	mutex_lock(&prm_binding_lock);
	if (g_prm.adev) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!audio_notifier_probe_status()) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	of_property_read_u32(adev->dev.of_node, "qcom,sleep-api-supported", &supported);
	spin_lock_irqsave(&prm_response_lock, flags);
	g_prm.accept_notifier = true;
	g_prm.is_adsp_up = true;
	g_prm.apm_ready = false;
	g_prm.epoch++;
	spin_unlock_irqrestore(&prm_response_lock, flags);
	ret = audio_notifier_register("audio_prm", AUDIO_NOTIFIER_ADSP_DOMAIN, &service_nb);
	if (ret) {
		spin_lock_irqsave(&prm_response_lock, flags);
		g_prm.accept_notifier = false;
		g_prm.is_adsp_up = false;
		spin_unlock_irqrestore(&prm_response_lock, flags);
		goto unlock;
	}
	mutex_lock(&prm_command_lock);
	get_device(&adev->dev);
	g_prm.adev = adev;
	g_prm.prm_sleep_api_supported = supported;
	dev_set_drvdata(&adev->dev, &g_prm);
	spin_lock_irqsave(&prm_response_lock, flags);
	g_prm.available = true;
	spin_unlock_irqrestore(&prm_response_lock, flags);
	mutex_unlock(&prm_command_lock);
	ret = 0;
unlock:
	mutex_unlock(&prm_binding_lock);
	return ret;
}

static int audio_prm_remove(struct gpr_device *adev)
{
	unsigned long flags;

	mutex_lock(&prm_binding_lock);
	if (dev_get_drvdata(&adev->dev) != &g_prm) {
		mutex_unlock(&prm_binding_lock);
		return 0;
	}
	spin_lock_irqsave(&prm_response_lock, flags);
	g_prm.available = false;
	g_prm.accept_notifier = false;
	g_prm.is_adsp_up = false;
	g_prm.apm_ready = false;
	g_prm.epoch++;
	spin_unlock_irqrestore(&prm_response_lock, flags);
	wake_up(&prm_wait);
	audio_notifier_deregister("audio_prm");
	mutex_lock(&prm_command_lock);
	dev_set_drvdata(&adev->dev, NULL);
	g_prm.adev = NULL;
	g_prm.prm_sleep_api_supported = 0;
	mutex_unlock(&prm_command_lock);
	put_device(&adev->dev);
	mutex_unlock(&prm_binding_lock);
	return 0;
}

static const struct of_device_id audio_prm_device_id[] = {
	{ .compatible = "qcom,audio_prm" },
	{},
};
MODULE_DEVICE_TABLE(of, audio_prm_device_id);

static struct gpr_driver qcom_audio_prm_driver = {
	.probe = audio_prm_probe,
	.remove = audio_prm_remove,
	.callback = audio_prm_callback,
	.driver = {
		.name = "qcom-audio_prm",
		.of_match_table = of_match_ptr(audio_prm_device_id),
	},
};

static int __init audio_prm_module_init(void)
{
	return gpr_driver_register(&qcom_audio_prm_driver);
}

static void __exit audio_prm_module_exit(void)
{
	gpr_driver_unregister(&qcom_audio_prm_driver);
}

module_init(audio_prm_module_init);
module_exit(audio_prm_module_exit);
MODULE_DESCRIPTION("audio prm");
MODULE_LICENSE("GPL v2");
