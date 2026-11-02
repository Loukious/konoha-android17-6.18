/*

 * aw_haptic.c

 *

 *

 *

 * Copyright (c) 2021 AWINIC Technology CO., LTD

 *

 * Author: <chelvming@awinic.com.cn>

 *

 * This program is free software; you can redistribute  it and/or modify it

 * under  the terms of  the GNU General  Public License as published by the

 * Free Software Foundation;  either version 2 of the  License, or (at your

 * option) any later version.

 */

#include <linux/module.h>

#include <linux/kernel.h>

#include <linux/i2c.h>

#include <linux/of_gpio.h>

#include <linux/delay.h>

#include <linux/device.h>

#include <linux/firmware.h>

#include <linux/slab.h>

#include <linux/version.h>

#include <linux/input.h>

#include <linux/interrupt.h>

#include <linux/debugfs.h>

#include <linux/miscdevice.h>

#include <linux/platform_device.h>

#include <asm/uaccess.h>

#include <linux/syscalls.h>

#include <linux/power_supply.h>

#include <linux/pm_qos.h>

#include "ringbuffer.h"

#include "aw_haptic.h"

#include "aw8697.h"

#include "aw86927.h"

#include "xm-haptic.h"

#include <linux/pinctrl/consumer.h>

#define AW_DRIVER_VERSION "v0.5.1.1"

/******************************************************

 *

 * Value

 *

 ******************************************************/

#ifdef ENABLE_PIN_CONTROL

static const char *const pctl_names[] = {

	"awinic_reset_reset",

	"awinic_reset_active",

	"awinic_interrupt_active",

};

#endif

char *awinic_ram_name[] = { "aw8697_haptic.bin", "aw8697_haptic_nv.bin" };

#ifdef TEST_RTP

char awinic_rtp_name[][AWINIC_RTP_NAME_MAX] = {

	{ "aw86927_rtp_1.bin" },

};

#else

char awinic_rtp_name[][AWINIC_RTP_NAME_MAX] = {

	{ "aw8697_rtp_1.bin" }, /*8*/

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	/*{"aw8697_rtp_1.bin"},*/

	/*{"aw8697_rtp_1.bin"},*/

	{ "AcousticGuitar_RTP.bin" }, /*21*/

	{ "Blues_RTP.bin" },

	{ "Candy_RTP.bin" },

	{ "Carousel_RTP.bin" },

	{ "Celesta_RTP.bin" },

	{ "Childhood_RTP.bin" },

	{ "Country_RTP.bin" },

	{ "Cowboy_RTP.bin" },

	{ "Echo_RTP.bin" },

	{ "Fairyland_RTP.bin" },

	{ "Fantasy_RTP.bin" },

	{ "Field_Trip_RTP.bin" },

	{ "Glee_RTP.bin" },

	{ "Glockenspiel_RTP.bin" },

	{ "Ice_Latte_RTP.bin" },

	{ "Kung_Fu_RTP.bin" },

	{ "Leisure_RTP.bin" },

	{ "Lollipop_RTP.bin" },

	{ "MiMix2_RTP.bin" },

	{ "Mi_RTP.bin" },

	{ "MiHouse_RTP.bin" },

	{ "MiJazz_RTP.bin" },

	{ "MiRemix_RTP.bin" },

	{ "Mountain_Spring_RTP.bin" },

	{ "Orange_RTP.bin" },

	{ "WindChime_RTP.bin" },

	{ "Space_Age_RTP.bin" },

	{ "ToyRobot_RTP.bin" },

	{ "Vigor_RTP.bin" },

	{ "Bottle_RTP.bin" },

	{ "Bubble_RTP.bin" },

	{ "Bullfrog_RTP.bin" },

	{ "Burst_RTP.bin" },

	{ "Chirp_RTP.bin" },

	{ "Clank_RTP.bin" },

	{ "Crystal_RTP.bin" },

	{ "FadeIn_RTP.bin" },

	{ "FadeOut_RTP.bin" },

	{ "Flute_RTP.bin" },

	{ "Fresh_RTP.bin" },

	{ "Frog_RTP.bin" },

	{ "Guitar_RTP.bin" },

	{ "Harp_RTP.bin" },

	{ "IncomingMessage_RTP.bin" },

	{ "MessageSent_RTP.bin" },

	{ "Moment_RTP.bin" },

	{ "NotificationXylophone_RTP.bin" },

	{ "Potion_RTP.bin" },

	{ "Radar_RTP.bin" },

	{ "Spring_RTP.bin" },

	{ "Swoosh_RTP.bin" }, /*71*/

	{ "Gesture_UpSlide_RTP.bin" },

	{ "FOD_Motion_Planet_RTP.bin" },

	{ "Charge_Wire_RTP.bin" },

	{ "Charge_Wireless_RTP.bin" },

	{ "Unlock_Failed_RTP.bin" },

	{ "FOD_Motion1_RTP.bin" },

	{ "FOD_Motion2_RTP.bin" },

	{ "FOD_Motion3_RTP.bin" },

	{ "FOD_Motion4_RTP.bin" },

	{ "FOD_Motion_Aurora_RTP.bin" },

	{ "FaceID_Wrong2_RTP.bin" }, /*82*/

	{ "uninstall_animation_rtp.bin" },

	{ "uninstall_dialog_rtp.bin" },

	{ "screenshot_rtp.bin" },

	{ "lockscreen_camera_entry_rtp.bin" },

	{ "launcher_edit_rtp.bin" },

	{ "launcher_icon_selection_rtp.bin" },

	{ "taskcard_remove_rtp.bin" },

	{ "task_cleanall_rtp.bin" },

	{ "new_iconfolder_rtp.bin" },

	{ "notification_remove_rtp.bin" },

	{ "notification_cleanall_rtp.bin" },

	{ "notification_setting_rtp.bin" },

	{ "game_turbo_rtp.bin" },

	{ "NFC_card_rtp.bin" },

	{ "wakeup_voice_assistant_rtp.bin" },

	{ "NFC_card_slow_rtp.bin" },

	{ "aw8697_rtp_1.bin" }, /*99*/

	{ "aw8697_rtp_1.bin" }, /*100*/

	{ "offline_countdown_RTP.bin" },

	{ "scene_bomb_injury_RTP.bin" },

	{ "scene_bomb_RTP.bin" }, /*103*/

	{ "door_open_RTP.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "scene_step_RTP.bin" }, /*106*/

	{ "crawl_RTP.bin" },

	{ "scope_on_RTP.bin" },

	{ "scope_off_RTP.bin" },

	{ "magazine_quick_RTP.bin" },

	{ "grenade_RTP.bin" },

	{ "scene_getshot_RTP.bin" }, /*112*/

	{ "grenade_explosion_RTP.bin" },

	{ "punch_RTP.bin" },

	{ "pan_RTP.bin" },

	{ "bandage_RTP.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "scene_jump_RTP.bin" },

	{ "vehicle_plane_RTP.bin" }, /*119*/

	{ "scene_openparachute_RTP.bin" }, /*120*/

	{ "scene_closeparachute_RTP.bin" }, /*121*/

	{ "vehicle_collision_RTP.bin" },

	{ "vehicle_buggy_RTP.bin" }, /*123*/

	{ "vehicle_dacia_RTP.bin" }, /*124*/

	{ "vehicle_moto_RTP.bin" }, /*125*/

	{ "firearms_akm_RTP.bin" }, /*126*/

	{ "firearms_m16a4_RTP.bin" }, /*127*/

	{ "aw8697_rtp_1.bin" },

	{ "firearms_awm_RTP.bin" }, /*129*/

	{ "firearms_mini14_RTP.bin" }, /*130*/

	{ "firearms_vss_RTP.bin" }, /*131*/

	{ "firearms_qbz_RTP.bin" }, /*132*/

	{ "firearms_ump9_RTP.bin" }, /*133*/

	{ "firearms_dp28_RTP.bin" }, /*134*/

	{ "firearms_s1897_RTP.bin" }, /*135*/

	{ "aw8697_rtp_1.bin" },

	{ "firearms_p18c_RTP.bin" }, /*137*/

	{ "aw8697_rtp_1.bin" },

	{ "aw8697_rtp_1.bin" },

	{ "CFM_KillOne_RTP.bin" },

	{ "CFM_Headshot_RTP.bin" }, /*141*/

	{ "CFM_MultiKill_RTP.bin" },

	{ "CFM_KillOne_Strong_RTP.bin" },

	{ "CFM_Headshot_Strong_RTP.bin" },

	{ "CFM_MultiKill_Strong_RTP.bin" },

	{ "CFM_Weapon_Grenade_Explode_RTP.bin" },

	{ "CFM_Weapon_Grenade_KillOne_RTP.bin" },

	{ "CFM_ImpactFlesh_Normal_RTP.bin" },

	{ "CFM_Weapon_C4_Installed_RTP.bin" },

	{ "CFM_Hero_Appear_RTP.bin" },

	{ "CFM_UI_Reward_OpenBox_RTP.bin" },

	{ "CFM_UI_Reward_Task_RTP.bin" },

	{ "CFM_Weapon_BLT_Shoot_RTP.bin" }, /*153*/

	{ "Atlantis_RTP.bin" },

	{ "DigitalUniverse_RTP.bin" },

	{ "Reveries_RTP.bin" },

	{ "FOD_Motion_Triang_RTP.bin" },

	{ "FOD_Motion_Flare_RTP.bin" },

	{ "FOD_Motion_Ripple_RTP.bin" },

	{ "FOD_Motion_Spiral_RTP.bin" },

	{ "gamebox_launch_rtp.bin" }, /*161*/

	{ "Gesture_Back_Pull_RTP.bin" }, /*162*/

	{ "Gesture_Back_Release_RTP.bin" }, /*163*/

	{ "alert_rtp.bin" }, /*164*/

	{ "feedback_negative_light_rtp.bin" }, /*165*/

	{ "feedback_neutral_rtp.bin" }, /*166*/

	{ "feedback_positive_rtp.bin" }, /*167*/

	{ "fingerprint_record_rtp.bin" }, /*168*/

	{ "lockdown_rtp.bin" }, /*169*/

	{ "sliding_damping_rtp.bin" }, /*170*/

	{ "todo_alldone_rtp.bin" }, /*171*/

	{ "uninstall_animation_icon_rtp.bin" }, /*172*/

	{ "signal_button_highlight_rtp.bin" }, /*173*/

	{ "signal_button_negative_rtp.bin" },

	{ "signal_button_rtp.bin" },

	{ "signal_clock_high_rtp.bin" }, /*176*/

	{ "signal_clock_rtp.bin" },

	{ "signal_clock_unit_rtp.bin" },

	{ "signal_inputbox_rtp.bin" },

	{ "signal_key_high_rtp.bin" },

	{ "signal_key_unit_rtp.bin" }, /*181*/

	{ "signal_list_highlight_rtp.bin" },

	{ "signal_list_rtp.bin" },

	{ "signal_picker_rtp.bin" },

	{ "signal_popup_rtp.bin" },

	{ "signal_seekbar_rtp.bin" }, /*186*/

	{ "signal_switch_rtp.bin" },

	{ "signal_tab_rtp.bin" },

	{ "signal_text_rtp.bin" },

	{ "signal_transition_light_rtp.bin" },

	{ "signal_transition_rtp.bin" }, /*191*/

	{ "haptics_video_rtp.bin" }, /*192*/

	{ "keyboard_clicky_down_rtp.bin" },

	{ "keyboard_clicky_up_rtp.bin" },

	{ "keyboard_linear_down_rtp.bin" },

	{ "keyboard_linear_up_rtp.bin" }, /*196*/

};

#endif

int awinic_rtp_name_len = sizeof(awinic_rtp_name) / AWINIC_RTP_NAME_MAX;

int CUSTOME_WAVE_ID;

/******************************************************

 *

 * i2c read/write

 *

 ******************************************************/

static int awinic_i2c_read(struct awinic *awinic,

			   unsigned char reg_addr, unsigned char *reg_data)

{
	int ret = -1;

	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_smbus_read_byte_data(awinic->i2c, reg_addr);

		if (ret < 0) {
			aw_err("%s: i2c_read cnt=%d error=%d\n",

			       __func__, cnt, ret);

		} else {
			*reg_data = ret;
			break;
		}

		cnt++;
		msleep(AW_I2C_RETRY_DELAY);
	}

	return ret;
}

static int awinic_i2c_write(struct awinic *awinic,

			    unsigned char reg_addr, unsigned char reg_data)

{
	int ret = -1;

	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret =

			i2c_smbus_write_byte_data(awinic->i2c, reg_addr,
						  reg_data);

		if (ret < 0) {
			aw_err("%s: i2c_write cnt=%d error=%d\n",

			       __func__, cnt, ret);

		} else {
			break;
		}

		cnt++;

		msleep(AW_I2C_RETRY_DELAY);
	}

	return ret;
}

#ifdef ENABLE_PIN_CONTROL

static int awinic_select_pin_ctl(struct awinic *awinic, const char *name)

{
	size_t i;

	int rc;

	for (i = 0; i < ARRAY_SIZE(awinic->pinctrl_state); i++) {
		const char *n = pctl_names[i];

		if (!strncmp(n, name, strlen(n))) {
			rc = pinctrl_select_state(awinic->awinic_pinctrl,

						  awinic->pinctrl_state[i]);

			if (rc)

				aw_err("%s: cannot select '%s'\n", __func__,

				       name);

			else

				aw_err("%s: Selected '%s'\n", __func__, name);

			goto exit;
		}
	}

	rc = -EINVAL;

	aw_info("%s: '%s' not found\n", __func__, name);

exit:

	return rc;
}

static int awinic_set_interrupt(struct awinic *awinic)

{
	int rc = awinic_select_pin_ctl(awinic, "awinic_interrupt_active");

	return rc;
}

#endif

static int awinic_sw_reset(struct awinic *awinic)

{
	aw_info("%s enter\n", __func__);

	awinic_i2c_write(awinic, 0x00, 0xAA);

	usleep_range(2000, 2500);

	return 0;
}

static int awinic_control_rest_pin(struct awinic *awinic)

{
	int ret = 0;

	ret = awinic_select_pin_ctl(awinic, "awinic_reset_active");

	if (ret < 0) {
		aw_err("%s select reset failed!\n", __func__);

		return ret;
	}

	usleep_range(5000, 5500);

	ret = awinic_select_pin_ctl(awinic, "awinic_reset_reset");

	if (ret < 0) {
		aw_err("%s select reset failed!\n", __func__);

		return ret;
	}

	usleep_range(5000, 5500);

	ret = awinic_select_pin_ctl(awinic, "awinic_reset_active");
	usleep_range(8000, 8500);
	if (ret < 0) {
		aw_err("%s select reset failed!\n", __func__);

		return ret;
	}

	return 0;
}

static int awinic_hw_reset(struct awinic *awinic)

{
	int rc = 0;

	if (!awinic->enable_pin_control) {
		if (awinic && gpio_is_valid(awinic->reset_gpio)) {
			gpio_set_value_cansleep(awinic->reset_gpio, 0);

			aw_info("%s pull down1\n", __func__);

			usleep_range(5000, 5500);

			gpio_set_value_cansleep(awinic->reset_gpio, 1);

			aw_info("%s pull up1\n", __func__);

			usleep_range(8000, 8500);

		} else {
			aw_err("%s:  failed\n", __func__);
		}

	} else {
		rc = awinic_control_rest_pin(awinic);
		if (rc < 0)

			return rc;
	}

	return 0;
}

/******************************************************

 *

 * check chip id

 *

 ******************************************************/

static int awinic_read_chipid(struct awinic *awinic, unsigned int *reg_val)

{
	unsigned char value[2] = { 0 };

	unsigned char chipid_addr[2] = { AW_REG_IDH, AW_REG_IDL };

	unsigned char i = 0;

	int ret = -1;

	aw_info("%s enter!\n", __func__);

	/* try to read aw8697 chip id */

	ret = awinic_i2c_read(awinic, 0x00, &value[0]);

	if (ret < 0)

		return ret;

	if (value[0] == AW8697_CHIP_ID) {
		*reg_val = value[0];
		return 0;
	}

	/* try to read aw86927 chip id */

	for (i = 0; i < 2; i++) {
		ret = awinic_i2c_read(awinic, chipid_addr[i], &value[i]);
		if (ret < 0)
			return ret;
	}

	*reg_val = value[0] << 8 | value[1];

	return 0;
}

static int awinic_parse_chipid(struct awinic *awinic)

{
	int ret = -1;

	unsigned char cnt = 0;

	unsigned int reg_val = 0;

	while (cnt < AW_READ_CHIPID_RETRIES) {
		/* hardware reset */

		ret = awinic_hw_reset(awinic);

		if (ret < 0) {
			aw_err("%s: hardware reset failed!\n", __func__);

			break;
		}

		ret = awinic_read_chipid(awinic, &reg_val);

		if (ret < 0) {
			aw_err("%s: failed to read AW_REG_ID: %d\n",

			       __func__, ret);

			break;
		}

		switch (reg_val) {
		case AW8697_CHIP_ID:

			aw_info("%s aw8697 detected\n", __func__);

			awinic->name = AW8697;

			awinic_sw_reset(awinic);

			return 0;

		case AW86927_CHIP_ID:

			aw_info("%s aw86927 detected\n", __func__);

			awinic->name = AW86927;

			awinic_sw_reset(awinic);

			return 0;

		default:

			aw_info("%s unsupported device revision (0x%x)\n",

				__func__, reg_val);

			break;
		}

		cnt++;

		usleep_range(2000, 3000);
	}

	return -EINVAL;
}

/******************************************************

 *

 * parse dts

 *

 ******************************************************/

static int awinic_parse_dt(struct awinic *awinic, struct device *dev,

			   struct device_node *np)

{
	awinic->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);

	if (awinic->reset_gpio >= 0) {
		aw_err("%s: reset gpio provided ok\n", __func__);

	} else {
		awinic->reset_gpio = -1;

		aw_err("%s: no reset gpio provided, will not HW reset device\n",

		       __func__);

		return -ERANGE;
	}

	awinic->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);

	if (awinic->irq_gpio < 0) {
		aw_err("%s: no irq gpio provided.\n", __func__);

		awinic->IsUsedIRQ = false;

	} else {
		aw_err("%s: irq gpio provided ok.\n", __func__);

		awinic->IsUsedIRQ = true;
	}

	return 0;
}

/******************************************************

 *

 * i2c driver

 *

 ******************************************************/

static void awinic_cleanup(struct awinic *awinic)
{
	struct input_dev *input_dev = awinic->input_dev;

	if (awinic->backend_initialized) {
		if (awinic->name == AW8697) {
			WRITE_ONCE(awinic->aw8697->stopping, true);
			atomic_set(&awinic->aw8697->exit_in_rtp_loop, 1);
			wake_up_interruptible(&awinic->aw8697->wait_q);
			wake_up_interruptible(&awinic->aw8697->stop_wait_q);
		} else {
			WRITE_ONCE(awinic->aw86927->stopping, true);
			atomic_set(&awinic->aw86927->exit_in_rtp_loop, 1);
			wake_up_interruptible(&awinic->aw86927->wait_q);
			wake_up_interruptible(&awinic->aw86927->stop_wait_q);
		}
	}
	if (awinic->ring_owned)
		rb_force_exit();
	if (awinic->sysfs_registered) {
		sysfs_remove_group(&awinic->i2c->dev.kobj,
			awinic->name == AW8697 ? &aw8697_vibrator_attribute_group :
						 &aw86927_vibrator_attribute_group);
		awinic->sysfs_registered = false;
	}
	if (awinic->input_registered) {
		input_unregister_device(input_dev);
		awinic->input_registered = false;
	}
	if (awinic->misc_registered) {
		aw8697_misc_unregister();
		awinic->misc_registered = false;
	}
	/* An admitted threaded handler can hold a pointer into the RTP buffer. */
	if (awinic->irq_registered)
		synchronize_irq(awinic->irq);
	if (awinic->backend_initialized) {
		if (awinic->name == AW8697)
			aw8697_shutdown(awinic->aw8697);
		else
			aw86927_shutdown(awinic->aw86927);
		awinic->backend_initialized = false;
	}
	if (awinic->irq_registered) {
		devm_free_irq(awinic->dev, awinic->irq,
			awinic->name == AW8697 ? (void *)awinic->aw8697 :
						 (void *)awinic->aw86927);
		awinic->irq_registered = false;
	}
	if (awinic->ring_owned) {
		release_rb();
		awinic->ring_owned = false;
	}
	device_init_wakeup(awinic->dev, false);
}

static int awinic_i2c_probe(struct i2c_client *i2c)
{
	struct awinic *awinic;
	struct input_dev *input_dev;
	struct device_node *np = i2c->dev.of_node;
	int ret;
	int rc;
#ifdef ENABLE_PIN_CONTROL
	int i;
#endif
	aw_info("%s enter\n", __func__);
	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		aw_err("check_functionality failed\n");
		return -EIO;
	}
	awinic = devm_kzalloc(&i2c->dev, sizeof(struct awinic), GFP_KERNEL);
	if (awinic == NULL)
		return -ENOMEM;
	input_dev = devm_input_allocate_device(&i2c->dev);
	if (!input_dev)
		return -ENOMEM;
	input_dev->name = "awinic_haptic";
	awinic->dev = &i2c->dev;
	awinic->i2c = i2c;
	awinic->input_dev = input_dev;
	device_init_wakeup(awinic->dev, true);
	i2c_set_clientdata(i2c, awinic);
	/* awinic rst & int */
	if (np) {
		ret = awinic_parse_dt(awinic, &i2c->dev, np);
		if (ret) {
			aw_err("%s: failed to parse device tree node\n",
			       __func__);
			goto error;
		}
	} else {
		awinic->reset_gpio = -1;
		awinic->irq_gpio = -1;
	}
	awinic->enable_pin_control = 0;
#ifdef ENABLE_PIN_CONTROL
	awinic->awinic_pinctrl = devm_pinctrl_get(&i2c->dev);
	if (IS_ERR(awinic->awinic_pinctrl)) {
		if (PTR_ERR(awinic->awinic_pinctrl) == -EPROBE_DEFER) {
			aw_info("%s pinctrl not ready\n", __func__);
			rc = -EPROBE_DEFER;
			ret = rc;
			goto error;
		}
		aw_info("%s Target does not use pinctrl\n", __func__);
		awinic->awinic_pinctrl = NULL;
		rc = -EINVAL;
		ret = rc;
		goto error;
	}
	for (i = 0; i < ARRAY_SIZE(awinic->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		struct pinctrl_state *state =
			pinctrl_lookup_state(awinic->awinic_pinctrl, n);
		if (!IS_ERR(state)) {
			aw_info("%s: found pin control %s\n", __func__, n);
			awinic->pinctrl_state[i] = state;
			awinic->enable_pin_control = 1;
			awinic_set_interrupt(awinic);
			continue;
		}
		aw_info("%s cannot find '%s'\n", __func__, n);
	}
#endif
	if (!awinic->enable_pin_control) {
		if (gpio_is_valid(awinic->reset_gpio)) {
			ret = devm_gpio_request_one(&i2c->dev,
						    awinic->reset_gpio,
						    GPIOF_OUT_INIT_LOW,
						    "awinic_rst");
			if (ret) {
				aw_err("%s: rst request failed, ret = %d\n",
				       __func__, ret);
				if (ret == -EBUSY)
					ret = -EPROBE_DEFER;
				goto error;
			}
		}
	}
	if (gpio_is_valid(awinic->irq_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, awinic->irq_gpio,
					    GPIOF_IN, "awinic_int");
		if (ret) {
			aw_err("%s: int request failed, ret = %d\n", __func__,
			       ret);
			if (ret == -EBUSY)
				ret = -EPROBE_DEFER;
			goto error;
		}
	}
	ret = awinic_parse_chipid(awinic);
	if (ret < 0) {
		aw_err("%s: read_chipid failed ret=%d\n", __func__, ret);
		goto error;
	}
	/* Admit only one hardware owner before creating global stream interfaces. */
	ret = create_rb();
	if (ret)
		goto error;
	awinic->ring_owned = true;
	xm_hap_driver_init(true);
	if (awinic->name == AW8697) {
		awinic->aw8697 = devm_kzalloc(awinic->dev, sizeof(*awinic->aw8697), GFP_KERNEL);
		if (!awinic->aw8697) {
			ret = -ENOMEM;
			goto error;
		}
		awinic->aw8697->dev = awinic->dev;
		awinic->aw8697->i2c = i2c;
		awinic->aw8697->reset_gpio = awinic->reset_gpio;
		awinic->aw8697->irq_gpio = awinic->irq_gpio;
		if (np) {
			ret = aw8697_parse_dt(&i2c->dev, awinic->aw8697, np);
			if (ret)
				goto error;
		}
		awinic->aw8697->work_queue = create_singlethread_workqueue("aw8697_vibrator");
		if (!awinic->aw8697->work_queue) {
			ret = -ENOMEM;
			goto error;
		}
		aw8697_vibrator_init(awinic->aw8697);
		awinic->backend_initialized = true;
		ret = aw8697_haptic_init(awinic->aw8697);
		if (ret < 0)
			goto error;
		if (gpio_is_valid(awinic->irq_gpio) &&
		    !(awinic->aw8697->flags & AW8697_FLAG_SKIP_INTERRUPTS)) {
			awinic->irq = gpio_to_irq(awinic->irq_gpio);
			if (awinic->irq < 0) {
				ret = awinic->irq;
				goto error;
			}
			aw8697_interrupt_setup(awinic->aw8697);
			ret = devm_request_threaded_irq(awinic->dev, awinic->irq,
				NULL, aw8697_irq, IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"aw8697", awinic->aw8697);
			if (ret)
				goto error;
			awinic->irq_registered = true;
		} else {
			awinic->aw8697->flags |= AW8697_FLAG_SKIP_INTERRUPTS;
		}
		CUSTOME_WAVE_ID = awinic->aw8697->info.effect_max;
		input_set_drvdata(input_dev, awinic->aw8697);
		awinic->aw8697->input_dev = input_dev;
		input_set_capability(input_dev, EV_FF, FF_CONSTANT);
		input_set_capability(input_dev, EV_FF, FF_GAIN);
		if (awinic->aw8697->effects_count) {
			input_set_capability(input_dev, EV_FF, FF_PERIODIC);
			input_set_capability(input_dev, EV_FF, FF_CUSTOM);
		}
		ret = input_ff_create(input_dev,
			max_t(int, FF_EFFECT_COUNT_MAX, awinic->aw8697->effects_count + 1));
		if (ret)
			goto error;
		input_dev->ff->upload = aw8697_haptics_upload_effect;
		input_dev->ff->playback = aw8697_haptics_playback;
		input_dev->ff->erase = aw8697_haptics_erase;
		input_dev->ff->set_gain = aw8697_haptics_set_gain;
		aw8697_ram_init(awinic->aw8697);
	} else if (awinic->name == AW86927) {
		awinic->aw86927 = devm_kzalloc(awinic->dev, sizeof(*awinic->aw86927), GFP_KERNEL);
		if (!awinic->aw86927) {
			ret = -ENOMEM;
			goto error;
		}
		awinic->aw86927->dev = awinic->dev;
		awinic->aw86927->i2c = i2c;
		awinic->aw86927->reset_gpio = awinic->reset_gpio;
		awinic->aw86927->irq_gpio = awinic->irq_gpio;
#ifdef AW_CHECK_QUAL
		if (aw86927_check_qualify(awinic->aw86927)) {
			ret = -ENODEV;
			goto error;
		}
#endif
		if (np) {
			ret = aw86927_parse_dt(awinic->aw86927, &i2c->dev, np);
			if (ret)
				goto error;
		}
		awinic->aw86927->work_queue = create_singlethread_workqueue("aw86927_vibrator");
		if (!awinic->aw86927->work_queue) {
			ret = -ENOMEM;
			goto error;
		}
		aw86927_vibrator_init(awinic->aw86927);
		awinic->backend_initialized = true;
		ret = aw86927_haptic_init(awinic->aw86927);
		if (ret < 0)
			goto error;
		if (gpio_is_valid(awinic->irq_gpio) &&
		    !(awinic->aw86927->flags & AW86927_FLAG_SKIP_INTERRUPTS)) {
			awinic->irq = gpio_to_irq(awinic->irq_gpio);
			if (awinic->irq < 0) {
				ret = awinic->irq;
				goto error;
			}
			aw86927_interrupt_setup(awinic->aw86927);
			ret = devm_request_threaded_irq(awinic->dev, awinic->irq,
				NULL, aw86927_irq, IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"aw86927", awinic->aw86927);
			if (ret)
				goto error;
			awinic->irq_registered = true;
		} else {
			awinic->aw86927->flags |= AW86927_FLAG_SKIP_INTERRUPTS;
		}
		CUSTOME_WAVE_ID = awinic->aw86927->info.effect_max;
		input_set_drvdata(input_dev, awinic->aw86927);
		awinic->aw86927->input_dev = input_dev;
		input_set_capability(input_dev, EV_FF, FF_CONSTANT);
		input_set_capability(input_dev, EV_FF, FF_GAIN);
		if (awinic->aw86927->effects_count) {
			input_set_capability(input_dev, EV_FF, FF_PERIODIC);
			input_set_capability(input_dev, EV_FF, FF_CUSTOM);
		}
		ret = input_ff_create(input_dev,
			max_t(int, FF_EFFECT_COUNT_MAX, awinic->aw86927->effects_count + 1));
		if (ret)
			goto error;
		input_dev->ff->upload = aw86927_haptics_upload_effect;
		input_dev->ff->playback = aw86927_haptics_playback;
		input_dev->ff->erase = aw86927_haptics_erase;
		input_dev->ff->set_gain = aw86927_haptics_set_gain;
		aw86927_ram_init(awinic->aw86927);
	} else {
		ret = -ENODEV;
		goto error;
	}
	/* Every callback dependency is ready before the first visible interface. */
	ret = input_register_device(input_dev);
	if (ret)
		goto error;
	awinic->input_registered = true;
	ret = sysfs_create_group(&i2c->dev.kobj,
		awinic->name == AW8697 ? &aw8697_vibrator_attribute_group :
					 &aw86927_vibrator_attribute_group);
	if (ret)
		goto error;
	awinic->sysfs_registered = true;
	if (awinic->name == AW8697) {
		ret = aw8697_misc_register(awinic->aw8697);
		if (ret)
			goto error;
		awinic->misc_registered = true;
	}
	return 0;
error:
	if (awinic->backend_initialized)
		XM_HAP_REGISTER_EXCEPTION("Hardware", "haptics_hw_init");
	awinic_cleanup(awinic);
	i2c_set_clientdata(i2c, NULL);
	return ret;
}

static void awinic_i2c_remove(struct i2c_client *i2c)
{
	struct awinic *awinic = i2c_get_clientdata(i2c);

	awinic_cleanup(awinic);
	i2c_set_clientdata(i2c, NULL);
}

static const struct i2c_device_id awinic_i2c_id[] = {

	{ AW_I2C_NAME, 0 },

	{}

};

MODULE_DEVICE_TABLE(i2c, awinic_i2c_id);

static const struct of_device_id aw_dt_match[] = {

	{ .compatible = "awinic,awinic_haptic" },

	{},

};

MODULE_DEVICE_TABLE(of, aw_dt_match);

static struct i2c_driver awinic_i2c_driver = {

	.driver = {

		   .name = AW_I2C_NAME,

		   .owner = THIS_MODULE,

		   .of_match_table = of_match_ptr(aw_dt_match),

		  },

	.probe = awinic_i2c_probe,

	.remove = awinic_i2c_remove,

	.id_table = awinic_i2c_id,

};

static int __init awinic_i2c_init(void)

{
	int ret = 0;

	aw_info("%s awinic driver version %s\n", __func__, AW_DRIVER_VERSION);

	ret = i2c_add_driver(&awinic_i2c_driver);

	if (ret) {
		aw_err("%s fail to add awinic device into i2c\n", __func__);

		return ret;
	}

	aw_info("%s aw_i2c_init success", __func__);

	return 0;
}

module_init(awinic_i2c_init);

static void __exit awinic_i2c_exit(void)

{
	i2c_del_driver(&awinic_i2c_driver);
}

module_exit(awinic_i2c_exit);

MODULE_DESCRIPTION("AWinic Haptic Driver");

MODULE_LICENSE("GPL v2");
