// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022, 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __LINUX_BTFM_CODEC_BTADV_INTERFACE_H
#define __LINUX_BTFM_CODEC_BTADV_INTERFACE_H

enum transport_type {
	BT = 1,
	BTADV,
	NONE,
};

static char *transport_type_text[] = {"BT", "BTADV", "NONE"};

void btfmcodec_set_current_state(struct btfmcodec_state_machine *, btfmcodec_state);
void btfmcodec_wq_prepare_bearer(struct work_struct *);
void btfmcodec_wq_hwep_shutdown(struct work_struct *);
void btfmcodec_initiate_hwep_shutdown(struct btfmcodec_char_device *btfmcodec_dev);
btfmcodec_state btfmcodec_get_current_transport(struct btfmcodec_state_machine *state);
btfmcodec_state btfmcodec_get_prev_transport(struct btfmcodec_state_machine *state);

void btfmcodec_move_to_next_state(struct btfmcodec_state_machine *state);
void btfmcodec_revert_current_state(struct btfmcodec_state_machine *state);
int btfmcodec_frame_transport_switch_ind_pkt(struct btfmcodec_char_device *btfmcodec_dev,
					     u8 active_transport,
					uint8_t status);
int btfmcodec_frame_prepare_bearer_rsp_pkt(struct btfmcodec_char_device *btfmcodec_dev,
					   u8 active_transport,
					uint8_t status);
int btfmcodec_wait_for_bearer_ind(struct btfmcodec_char_device *btfmcodec_dev);
int btfmcodec_initiate_hwep_configuration(struct btfmcodec_char_device *btfmcodec_dev);
void btfmcodec_configure_hwep(struct btfmcodec_char_device *btfmcodec_dev);
void btfmcodec_prepare_bearer(struct btfmcodec_char_device *btfmcodec_dev,
			      enum transport_type new_transport);
#endif /* __LINUX_BTFM_CODEC_BTADV_INTERFACE_H */
