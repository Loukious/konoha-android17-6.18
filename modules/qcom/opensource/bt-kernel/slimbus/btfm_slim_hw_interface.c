// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/debugfs.h>
#include <linux/slimbus.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include "btfm_slim.h"
#include "btfm_slim_hw_interface.h"
#include "btfm_codec_hw_interface.h"

static int bt_soc_enable_status;
int btfm_feedback_ch_setting;
static uint8_t usecase_codec;

static int btfm_slim_hwep_write(struct snd_soc_component *codec,
			unsigned int reg, unsigned int value)
{
	BTFMSLIM_DBG("");
	return 0;
}

static unsigned int btfm_slim_hwep_read(struct snd_soc_component *codec,
				unsigned int reg)
{
	BTFMSLIM_DBG("");
	return 0;
}

static int btfm_soc_status_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	BTFMSLIM_DBG("");
	ucontrol->value.integer.value[0] = READ_ONCE(bt_soc_enable_status);
	return 1;
}

static int btfm_soc_status_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	BTFMSLIM_DBG("");
	return 1;
}

static int btfm_get_feedback_ch_setting(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	BTFMSLIM_DBG("");
	ucontrol->value.integer.value[0] = READ_ONCE(btfm_feedback_ch_setting);
	return 1;
}

static int btfm_put_feedback_ch_setting(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	BTFMSLIM_DBG("");
	if (ucontrol->value.integer.value[0] < 0 ||
	    ucontrol->value.integer.value[0] > 1)
		return -EINVAL;
	WRITE_ONCE(btfm_feedback_ch_setting, ucontrol->value.integer.value[0]);
	return 1;
}

static int btfm_get_codec_type(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	BTFMSLIM_DBG("current codec type:%s", codec_text[usecase_codec]);
	ucontrol->value.integer.value[0] = usecase_codec;
	return 1;
}

static int btfm_put_codec_type(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	if (ucontrol->value.integer.value[0] < 0 ||
	    ucontrol->value.integer.value[0] >= ARRAY_SIZE(codec_text))
		return -EINVAL;
	WRITE_ONCE(usecase_codec, ucontrol->value.integer.value[0]);
	BTFMSLIM_DBG("codec type set to:%s", codec_text[usecase_codec]);
	return 1;
}

static struct snd_kcontrol_new status_controls[] = {
	SOC_SINGLE_EXT("BT SOC status", 0, 0, 1, 0,
	btfm_soc_status_get, btfm_soc_status_put),
	SOC_SINGLE_EXT("BT set feedback channel", 0, 0, 1, 0,
	btfm_get_feedback_ch_setting,
	btfm_put_feedback_ch_setting),
	SOC_ENUM_EXT("BT codec type", codec_display,
	btfm_get_codec_type, btfm_put_codec_type),
};

static int btfm_slim_hwep_probe(struct snd_soc_component *codec)
{
	BTFMSLIM_DBG("");
	return 0;
}

static void btfm_slim_hwep_remove(struct snd_soc_component *codec)
{
	BTFMSLIM_DBG("");
}

static int btfm_slim_dai_startup(void *dai)
{
	struct hwep_data *hwep_info = (struct hwep_data *)dai;
	struct btfmslim *btfmslim = dev_get_drvdata(hwep_info->dev);
	int ret = -1;

	BTFMSLIM_DBG("");
	ret = btfm_slim_hw_init(btfmslim);
	return ret;
}

static struct btfmslim_ch *btfm_slim_find_channel(struct btfmslim *priv, int id,
						  u8 *rxport, u8 *count)
{
	struct btfmslim_ch *table;
	unsigned int size, i;

	if (id < 0 || id >= BTFM_SLIM_NUM_CODEC_DAIS)
		return NULL;
	*rxport = id == BTFM_BT_SCO_A2DP_SLIM_RX || id == BTFM_BT_SPLIT_A2DP_SLIM_RX;
	*count = id == BTFM_FM_SLIM_TX ? 2 : 1;
	table = *rxport ? priv->rx_chs : priv->tx_chs;
	size = *rxport ? priv->num_rx_port : priv->num_tx_port;
	for (i = 0; i < size; i++)
		if (table[i].id == id && *count <= size - i)
			return &table[i];
	return NULL;
}

static void btfm_slim_dai_shutdown(void *dai, int id)
{
	struct hwep_data *ep = dai;
	struct btfmslim *priv = dev_get_drvdata(ep->dev);
	struct btfmslim_ch *ch;
	u8 rxport, count;
	int ret;

	mutex_lock(&priv->io_lock);
	ch = btfm_slim_find_channel(priv, id, &rxport, &count);
	if (ch) {
		ret = btfm_slim_disable_ch(priv, ch, rxport, count);
		if (ret)
			BTFMSLIM_ERR("shutdown cleanup retained for retry: %d", ret);
	}
	mutex_unlock(&priv->io_lock);
	btfm_slim_hw_deinit(priv);
}

static int btfm_slim_dai_hw_params(void *dai, u32 bps, u32 direction, u8 channels)
{
	struct hwep_data *ep = dai;
	struct btfmslim *priv = dev_get_drvdata(ep->dev);
	int ret = 0;

	if (bps != 16 || direction > SNDRV_PCM_STREAM_CAPTURE ||
	    !channels || channels > 2)
		return -EINVAL;
	mutex_lock(&priv->io_lock);
	if (priv->stopping) {
		ret = -ENODEV;
	} else {
		priv->bps = bps;
		priv->direction = direction;
	}
	mutex_unlock(&priv->io_lock);
	return ret;
}

static void btfm_get_sampling_rate(uint32_t *sampling_rate)
{
	uint8_t codec_types_avb = ARRAY_SIZE(codec_text);
	u8 codec = READ_ONCE(usecase_codec);

	if (codec > (codec_types_avb - 1)) {
		BTFMSLIM_ERR("falling back to use default sampling_rate: %u",
				*sampling_rate);
		return;
	}

	if (*sampling_rate == 44100 || *sampling_rate == 48000) {
		if (codec == LDAC ||
		    codec == APTX_AD ||
		    codec == LHDC)
			*sampling_rate = (*sampling_rate) *2;
	}

	if (codec == LC3_VOICE ||
	    codec == APTX_AD_SPEECH ||
	    codec == LC3 || codec == APTX_AD_R4 ||
	    codec == RVP) {
		*sampling_rate = 96000;
	}

	if (codec == APTX_AD_QLEA)
		*sampling_rate = 192000;

	BTFMSLIM_INFO("current usecase codec type %s and sampling rate:%u khz",
			codec_text[codec], *sampling_rate);
}

static int btfm_slim_dai_prepare(void *dai, u32 rate, u32 direction, int id)
{
	struct hwep_data *ep = dai;
	struct btfmslim *priv = dev_get_drvdata(ep->dev);
	struct btfmslim_ch *ch;
	u8 rxport, count;
	int ret = -EINVAL;

	if (direction > SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;
	mutex_lock(&priv->io_lock);
	if (priv->stopping) {
		ret = -ENODEV;
		goto out;
	}
	ch = btfm_slim_find_channel(priv, id, &rxport, &count);
	if (!ch || direction != (rxport ? SNDRV_PCM_STREAM_PLAYBACK : SNDRV_PCM_STREAM_CAPTURE))
		goto out;
	btfm_get_sampling_rate(&rate);
	priv->direction = direction;
	priv->sample_rate = rate;
	ret = btfm_slim_enable_ch(priv, ch, rxport, rate, count);
	if (ret == -EISCONN)
		ret = 0;
	WRITE_ONCE(bt_soc_enable_status, !ret);
out:
	mutex_unlock(&priv->io_lock);
	return ret;
}

static int btfm_slim_dai_set_channel_map(void *dai,
			unsigned int tx_num, unsigned int *tx_slot,
			unsigned int rx_num, unsigned int *rx_slot)
{
	struct hwep_data *ep = dai;
	struct btfmslim *priv = dev_get_drvdata(ep->dev);
	unsigned int i;
	int ret = 0;

	if (tx_num > priv->num_tx_port || rx_num > priv->num_rx_port ||
	    (tx_num && !tx_slot) || (rx_num && !rx_slot))
		return -EINVAL;
	for (i = 0; i < tx_num; i++)
		if (tx_slot[i] > U8_MAX)
			return -EINVAL;
	for (i = 0; i < rx_num; i++)
		if (rx_slot[i] > U8_MAX)
			return -EINVAL;
	mutex_lock(&priv->io_lock);
	if (priv->stopping) {
		ret = -ENODEV;
		goto out;
	}
	for (i = 0; i < priv->num_tx_port; i++)
		if (priv->tx_chs[i].dai.sruntime) {
			ret = -EBUSY;
			goto out;
		}
	for (i = 0; i < priv->num_rx_port; i++)
		if (priv->rx_chs[i].dai.sruntime) {
			ret = -EBUSY;
			goto out;
		}
	for (i = 0; i < tx_num; i++)
		priv->tx_chs[i].ch = tx_slot[i];
	for (i = 0; i < rx_num; i++)
		priv->rx_chs[i].ch = rx_slot[i];
out:
	mutex_unlock(&priv->io_lock);
	return ret;
}

static int btfm_slim_dai_get_channel_map(void *dai,
			unsigned int *tx_num, unsigned int *tx_slot,
			unsigned int *rx_num, unsigned int *rx_slot, int id)
{
	struct hwep_data *ep = dai;
	struct btfmslim *priv = dev_get_drvdata(ep->dev);
	struct btfmslim_ch *ch;
	unsigned int *slot, i;
	u8 rxport, count;
	int ret = -EINVAL;

	if (!tx_num || !rx_num)
		return -EINVAL;
	mutex_lock(&priv->io_lock);
	if (priv->stopping) {
		ret = -ENODEV;
		goto out;
	}
	ch = btfm_slim_find_channel(priv, id, &rxport, &count);
	if (!ch || (rxport ? !rx_slot : !tx_slot))
		goto out;
	*tx_num = rxport ? 0 : count;
	*rx_num = rxport ? count : 0;
	slot = rxport ? rx_slot : tx_slot;
	for (i = 0; i < count; i++)
		slot[i] = ch[i].ch;
	ret = 0;
out:
	mutex_unlock(&priv->io_lock);
	return ret;
}

static int btfm_slim_dai_get_configs(void *dai, void *config, u8 id)
{
	struct hwep_data *ep = dai;
	struct btfmslim *priv = dev_get_drvdata(ep->dev);
	struct master_hwep_configurations *cfg = config;
	struct btfmslim_ch *ch;
	u8 rxport, count;
	int ret = -EINVAL;

	if (!cfg)
		return -EINVAL;
	mutex_lock(&priv->io_lock);
	if (priv->stopping) {
		ret = -ENODEV;
		goto out;
	}
	ch = btfm_slim_find_channel(priv, id, &rxport, &count);
	if (!ch)
		goto out;
	cfg->stream_id = id;
	cfg->device_id = priv->device_id;
	cfg->sample_rate = ch->dai.sruntime ? ch->dai.sconfig.rate : priv->sample_rate;
	cfg->bit_width = ch->dai.sruntime ? ch->dai.sconfig.bps : priv->bps;
	cfg->codectype = READ_ONCE(usecase_codec);
	cfg->direction = rxport ? SNDRV_PCM_STREAM_PLAYBACK : SNDRV_PCM_STREAM_CAPTURE;
	cfg->num_channels = count;
	cfg->chan_num = ch->ch;
	ret = 1;
out:
	mutex_unlock(&priv->io_lock);
	return ret;
}

static struct hwep_dai_ops  btfmslim_hw_dai_ops = {
	.hwep_startup = btfm_slim_dai_startup,
	.hwep_shutdown = btfm_slim_dai_shutdown,
	.hwep_hw_params = btfm_slim_dai_hw_params,
	.hwep_prepare = btfm_slim_dai_prepare,
	.hwep_set_channel_map = btfm_slim_dai_set_channel_map,
	.hwep_get_channel_map = btfm_slim_dai_get_channel_map,
	.hwep_get_configs = btfm_slim_dai_get_configs,
	.hwep_codectype = &usecase_codec,
};

static struct hwep_dai_driver btfmslim_dai_driver[] = {
	{	/* Bluetooth SCO voice uplink: bt -> lpass */
		.dai_name = "btaudio_tx",
		.id = BTAUDIO_TX,
		.capture = {
			.stream_name = "BT Audio Slim Tx Capture",
			/* 8 KHz or 16 KHz */
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000
				| SNDRV_PCM_RATE_8000_192000
				| SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000
				| SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000
				| SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE, /* 16 bits */
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.dai_ops = &btfmslim_hw_dai_ops,
	},
	{	/* Bluetooth SCO voice downlink: lpass -> bt or A2DP Playback */
		.dai_name = "btaudio_rx",
		.id = BTAUDIO_RX,
		.playback = {
			.stream_name = "BT Audio Slim Rx Playback",
			/* 8/16/44.1/48/88.2/96 Khz */
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000
				| SNDRV_PCM_RATE_8000_192000
				| SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000
				| SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000
				| SNDRV_PCM_RATE_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE, /* 16 bits */
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.dai_ops = &btfmslim_hw_dai_ops,
	},
};

static struct hwep_comp_drv btfmslim_hw_driver = {
	.hwep_probe	= btfm_slim_hwep_probe,
	.hwep_remove	= btfm_slim_hwep_remove,
	.hwep_read	= btfm_slim_hwep_read,
	.hwep_write	= btfm_slim_hwep_write,
};

int btfm_slim_register_hw_ep(struct btfmslim *btfm_slim)
{
	struct device *dev = btfm_slim->dev;
	struct hwep_data *hwep_info;
	int ret = 0;

	BTFMSLIM_INFO("Registering with BTFMCODEC HWEP interface\n");
	hwep_info = kzalloc(sizeof(struct hwep_data), GFP_KERNEL);

	if (!hwep_info) {
		BTFMSLIM_ERR("%s: failed to allocate memory\n", __func__);
		ret = -ENOMEM;
		goto end;
	}

	/* Copy EP device parameters as intercations will be on the same device */
	hwep_info->dev = dev;
	strscpy(hwep_info->driver_name, BTFMSLIM_DEV_NAME, DEVICE_NAME_MAX_LEN);
	hwep_info->drv = &btfmslim_hw_driver;
	hwep_info->dai_drv = btfmslim_dai_driver;
	hwep_info->num_dai = ARRAY_SIZE(btfmslim_dai_driver);
	hwep_info->num_mixer_ctrl = ARRAY_SIZE(status_controls);
	hwep_info->mixer_ctrl = status_controls;
	/* Register to hardware endpoint */
	ret = btfmcodec_register_hw_ep(hwep_info);
	kfree(hwep_info);
	if (ret) {
		BTFMSLIM_ERR("failed to register with btfmcodec driver hw interface (%d)", ret);
		goto end;
	}

	BTFMSLIM_INFO("Registered succesfull with BTFMCODEC HWEP interface\n");
	return ret;
end:
	return ret;
}

void btfm_slim_unregister_hwep(void)
{
	BTFMSLIM_INFO("Unregistered with BTFMCODEC HWEP	interface");
	/* Unregister with BTFMCODEC HWEP	driver */
	btfmcodec_unregister_hw_ep(BTFMSLIM_DEV_NAME);

}

MODULE_DESCRIPTION("BTFM Slimbus driver");
MODULE_LICENSE("GPL v2");
