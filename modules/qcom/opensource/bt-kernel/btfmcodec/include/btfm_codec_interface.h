// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __LINUX_BTFM_CODEC_INTERFACE
#define __LINUX_BTFM_CODEC_INTERFACE

#include "btfm_codec_hw_interface.h"
struct btfmcodec_data;
int btfm_register_codec(struct hwep_data *hwep_info);
void btfm_unregister_codec(void);

int btfmcodec_hwep_startup(struct btfmcodec_data *btfmcodec);
int btfmcodec_hwep_shutdown(struct btfmcodec_data *btfmcodec, int id,
			    bool disable_master);
void btfmcodec_wq_hwep_shutdown(struct work_struct *work);
int btfmcodec_hwep_hw_params(struct btfmcodec_data *btfmcodec, uint32_t bps,
			     u32 direction, uint8_t num_channels);
bool btfmcodec_is_valid_cache_avb(struct btfmcodec_data *btfmcodec);
int btfmcodec_hwep_prepare(struct btfmcodec_data *btfmcodec, uint32_t sampling_rate,
			   u32 direction, int id, bool seamless);
int btfmcodec_hwep_set_channel_map(void *hwep_info, unsigned int tx_num,
				   unsigned int *tx_slot, unsigned int rx_num,
				unsigned int *rx_slot);
int btfmcodec_hwep_get_channel_map(void *hwep_info, unsigned int *tx_num,
				   unsigned int *tx_slot, unsigned int *rx_num,
				unsigned int *rx_slot, int id);
void btfmcodec_wq_hwep_configure(struct work_struct *work);
#endif /*__LINUX_BTFM_CODEC_INTERFACE */
