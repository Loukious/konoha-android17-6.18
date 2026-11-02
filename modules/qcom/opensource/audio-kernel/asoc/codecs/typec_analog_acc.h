/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024, Xiaomi Corporation, Inc. All rights reserved.
 */

#ifndef _TYPEC_ANALOG_ACC_H
#define _TYPEC_ANALOG_ACC_H

struct snd_soc_component;

int typec_analog_acc_init(struct snd_soc_component *component);
/* Must run before component/card teardown and outside jack callbacks. */
void typec_analog_acc_exit(struct snd_soc_component *component);

#endif
