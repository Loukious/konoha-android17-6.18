/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 */
#include "lpass-cdc-comp.h"
#include <linux/limits.h>
#include <linux/overflow.h>

int lpass_cdc_load_compander_coeff(struct snd_soc_component *component,
				   u16 lsb_reg, u16 msb_reg,
				   struct comp_coeff_val *comp_coeff_table,
				   u16 arr_size)
{
	int i = 0, ret;

	if (!component || (!comp_coeff_table && arr_size))
		return -EINVAL;

	/* Load Compander Coeff */
	for (i = 0; i < arr_size; i++) {
		ret = snd_soc_component_write(component, lsb_reg,
					      comp_coeff_table[i].lsb);
		if (ret)
			return ret;
		ret = snd_soc_component_write(component, msb_reg,
					      comp_coeff_table[i].msb);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(lpass_cdc_load_compander_coeff);

int lpass_cdc_update_compander_setting(struct snd_soc_component *component,
				u16 start_addr,
				struct lpass_cdc_comp_setting *comp_setting)
{
	int zone2_rms, zone3_rms, zone4_rms, zone5_rms, zone6_rms;
	int path_gain;
	int max_attn;
	int zone1_rms = 6;
	int upper_gain_int, lower_gain_int, ana_addr_map, upper_gain_dig_int;
	int ret;

	if (!component || !comp_setting)
		return -EINVAL;
	upper_gain_int = comp_setting->upper_gain_int;
	lower_gain_int = comp_setting->lower_gain_int;
	ana_addr_map = comp_setting->ana_addr_map;
	if (lower_gain_int == INT_MIN ||
	    check_sub_overflow(upper_gain_int, lower_gain_int, &upper_gain_dig_int) ||
	    check_sub_overflow(upper_gain_dig_int, abs(lower_gain_int), &path_gain) ||
	    start_addr > U16_MAX - 0x28)
		return -EINVAL;

	/* skip comp_ctl8, comp_ctl9 default settings is fine */

	/* apply zone settings */
	ret = snd_soc_component_write(component,
				      start_addr + 8,
				      zone1_rms);
	if (ret)
		return ret;

	if (upper_gain_dig_int >= 24)
		zone2_rms = 18;
	else if (upper_gain_dig_int >= 18)
		zone2_rms = 12;
	else
		zone2_rms = upper_gain_dig_int;
	ret = snd_soc_component_write(component,
				      start_addr + 0xC,
				      zone2_rms);
	if (ret)
		return ret;

	if (upper_gain_dig_int >= 66)
		zone3_rms = 33;
	else if (upper_gain_dig_int >= 36)
		zone3_rms = 30;
	else if (upper_gain_dig_int >= 30)
		zone3_rms = 24;
	else
		zone3_rms = upper_gain_dig_int;
	ret = snd_soc_component_write(component,
				      start_addr + 0x10,
				      zone3_rms);
	if (ret)
		return ret;

	if (upper_gain_dig_int >= 66)
		zone4_rms = 48;
	else if (upper_gain_dig_int >= 48)
		zone4_rms = 42;
	else if (upper_gain_dig_int >= 42)
		zone4_rms = 36;
	else
		zone4_rms = upper_gain_dig_int;
	ret = snd_soc_component_write(component,
				      start_addr + 0x14,
				      zone4_rms);
	if (ret)
		return ret;

	if (upper_gain_dig_int >= 69)
		zone5_rms = 63;
	else if (upper_gain_dig_int >= 66)
		zone5_rms = 60;
	else if (upper_gain_dig_int >= 60)
		zone5_rms = 54;
	else if (upper_gain_dig_int >= 54)
		zone5_rms = 48;
	else
		zone5_rms = upper_gain_dig_int;
	ret = snd_soc_component_write(component,
				      start_addr + 0x18,
				      zone5_rms);
	if (ret)
		return ret;

	zone6_rms = upper_gain_dig_int;
	ret = snd_soc_component_write(component,
				      start_addr + 0x1C,
				      zone6_rms);
	if (ret)
		return ret;

	if (lower_gain_int < 0)
		max_attn = 256 + lower_gain_int;
	else
		max_attn = lower_gain_int;
	ret = snd_soc_component_write(component,
				      start_addr + 0x20,
				      max_attn);
	if (ret)
		return ret;

	path_gain = zone6_rms - abs(lower_gain_int);
	ret = snd_soc_component_write(component,
				      start_addr + 0x24,
				      path_gain);
	if (ret)
		return ret;

	ret = snd_soc_component_write(component,
				      start_addr + 0x28,
				      ana_addr_map);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(lpass_cdc_update_compander_setting);
