// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include "btfm_codec.h"
#include "btfm_codec_hw_interface.h"
#include "btfm_codec_interface.h"
#include "btfm_codec_btadv_interface.h"

static DEFINE_MUTEX(btfmcodec_registration_lock);

int btfmcodec_register_hw_ep(struct hwep_data *ep_info)
{
	struct btfmcodec_data *codec = btfm_get_btfmcodec();
	struct hwep_data *copy;
	int ret;

	if (!codec)
		return -EPROBE_DEFER;
	if (!ep_info || ep_info->num_dai <= 0 || ep_info->num_dai > 64 ||
	    ep_info->num_mixer_ctrl < 0 || ep_info->num_mixer_ctrl > 64 ||
	    !ep_info->dai_drv || !ep_info->dev ||
	    (ep_info->num_mixer_ctrl && !ep_info->mixer_ctrl) ||
	    !memchr(ep_info->driver_name, '\0', DEVICE_NAME_MAX_LEN))
		return -EINVAL;
	mutex_lock(&btfmcodec_registration_lock);
	mutex_lock(&codec->hwep_drv_lock);
	if (codec->hwep_info) {
		ret = -EBUSY;
		goto unlock;
	}
	copy = kmemdup(ep_info, sizeof(*copy), GFP_KERNEL);
	if (!copy) {
		ret = -ENOMEM;
		goto unlock;
	}
	get_device(copy->dev);
	WRITE_ONCE(codec->hwep_stopping, true);
	WRITE_ONCE(codec->hwep_info, copy);
	mutex_unlock(&codec->hwep_drv_lock);
	ret = btfm_register_codec(copy);
	mutex_lock(&codec->hwep_drv_lock);
	if (ret) {
		WRITE_ONCE(codec->hwep_info, NULL);
		put_device(copy->dev);
		kfree(copy);
	} else {
		WRITE_ONCE(codec->hwep_stopping, false);
	}
unlock:
	mutex_unlock(&codec->hwep_drv_lock);
	mutex_unlock(&btfmcodec_registration_lock);
	return ret;
}

int btfmcodec_unregister_hw_ep(char *driver_name)
{
	struct btfmcodec_data *codec = btfm_get_btfmcodec();
	struct hwep_data *endpoint;
	struct hwep_configurations *config, *next;
	int ret = 0;

	if (!codec || !driver_name)
		return -EINVAL;
	mutex_lock(&btfmcodec_registration_lock);
	mutex_lock(&codec->hwep_drv_lock);
	endpoint = codec->hwep_info;
	if (!endpoint || strncmp(endpoint->driver_name, driver_name, DEVICE_NAME_MAX_LEN)) {
		ret = -ENOENT;
		mutex_unlock(&codec->hwep_drv_lock);
		goto unlock;
	}
	WRITE_ONCE(codec->hwep_stopping, true);
	mutex_unlock(&codec->hwep_drv_lock);
	btfmcodec_stop_commands(codec);
	synchronize_srcu(&btfmcodec_endpoint_srcu);
	btfmcodec_drain_commands(codec);
	/* ASoC removal may call the endpoint's remove callback. */
	btfm_unregister_codec();
	mutex_lock(&codec->btfmcodec_dev->read_lock);
	skb_queue_purge(&codec->btfmcodec_dev->txq);
	mutex_unlock(&codec->btfmcodec_dev->read_lock);
	mutex_lock(&codec->config_lock);
	list_for_each_entry_safe(config, next, &codec->config_head, dai_list) {
		list_del(&config->dai_list);
		kfree(config);
	}
	mutex_unlock(&codec->config_lock);
	btfmcodec_set_current_state(&codec->states, IDLE);
	mutex_lock(&codec->hwep_drv_lock);
	WRITE_ONCE(codec->hwep_info, NULL);
	mutex_unlock(&codec->hwep_drv_lock);
	put_device(endpoint->dev);
	kfree(endpoint);
unlock:
	mutex_unlock(&btfmcodec_registration_lock);
	return ret;
}

EXPORT_SYMBOL(btfmcodec_register_hw_ep);
EXPORT_SYMBOL(btfmcodec_unregister_hw_ep);
