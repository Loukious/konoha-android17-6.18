// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/usb/typec.h>
#include <linux/usb/ucsi_glink.h>
#include <linux/soc/qcom/wcd939x-i2c.h>
#include <linux/qti-regmap-debugfs.h>
#include <linux/pinctrl/consumer.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/pm_runtime.h>
#include <linux/nvmem-consumer.h>
#include <linux/srcu.h>
#include <linux/kthread.h>
#include "wcd-usbss-priv.h"
#include "wcd-usbss-reg-masks.h"
#include "wcd-usbss-reg-shifts.h"
#include "hwid.h"

#define WCD_USBSS_I2C_NAME	"wcd-usbss-i2c-driver"

#define DEFAULT_SURGE_TIMER_PERIOD_MS 15000
#define SEC_TO_MS 1000
#define NUM_RCO_MISC2_READ 10
#define MIN_SURGE_TIMER_PERIOD_SEC 3
#define MAX_SURGE_TIMER_PERIOD_SEC 20
#define PM_RUNTIME_RESUME_CNT 8
#define PM_RUNTIME_RESUME_WAIT_US_MIN  5000
#define MG_BIAS_CURRENT 0xCF
#define WCD_USBSS_TRIMCODE1_MASK_1 0x1F
#define WCD_USBSS_TRIMCODE2_MASK_1 0x03
#define WCD_USBSS_TRIMCODE2_MASK_2 0x7C
#define WCD_USBSS_TRIMCODE1_MASK_2 0xE0
#define BIT_MASK_4 4
#define BIT_MASK_2 2
#define BIT_MASK_F 0x0F


enum {
	WCD_USBSS_AUDIO_MANUAL,
	WCD_USBSS_AUDIO_FSM,
};

enum {
	WCD_USBSS_1_X,
	WCD_USBSS_2_0,
};

enum {
	INVALID_MODE,
	MBHC_MODE1 = 1,
	ULP_MODE2 = 2,
	STD_MODE3 = 3,
	HIFI_MODE4 = 4,
};

enum {
	WCD_USBSS_LPD_USB_MODE_CLEAR = 0,
	WCD_USBSS_LPD_MODE_SET,
	WCD_USBSS_USB_MODE_SET,
	WCD_USBSS_LPD_USB_MODE_SET,
	WCD_USBSS_LPD_CONTINUOUS_1 = 5,
	WCD_USBSS_LPD_CONTINUOUS_2 = 7,
	WCD_USBSS_SDAM_MODE_MAX = WCD_USBSS_LPD_CONTINUOUS_2,
	WCD_USBSS_AUDIO_MODE_SET = WCD_USBSS_SDAM_MODE_MAX + 1,
};

struct wcd_usbss_reg_mask_val {
	u16 reg;
	u8 mask;
	u8 val;
};

/* regulator power supply names */
static const char * const supply_names[] = {
	"vdd-usb-cp",
};

/* Linearlizer coefficients for 32ohm load */
static const struct wcd_usbss_reg_mask_val coeff_init[] = {
	{WCD_USBSS_AUD_COEF_L_K5_0,       0xFF, 0x39},
	{WCD_USBSS_AUD_COEF_R_K5_0,       0xFF, 0x39},
	{WCD_USBSS_GND_COEF_L_K2_0,       0xFF, 0xE8},
	{WCD_USBSS_GND_COEF_L_K4_0,       0xFF, 0x73},
	{WCD_USBSS_GND_COEF_R_K2_0,       0xFF, 0xE8},
	{WCD_USBSS_GND_COEF_R_K4_0,       0xFF, 0x73},
	{WCD_USBSS_RATIO_SPKR_REXT_L_LSB, 0xFF, 0x00},
	{WCD_USBSS_RATIO_SPKR_REXT_R_LSB, 0xFF, 0x00},
};

static DEFINE_MUTEX(wcd_usbss_provider_lock);
DEFINE_STATIC_SRCU(wcd_usbss_srcu);
static struct wcd_usbss_ctxt __rcu *wcd_usbss_provider;
static bool wcd_usbss_claimed;

/* Readers may sleep; removal withdraws the provider and drains them. */
static struct wcd_usbss_ctxt *wcd_usbss_get(int *index)
{
	*index = srcu_read_lock(&wcd_usbss_srcu);
	return srcu_dereference(wcd_usbss_provider, &wcd_usbss_srcu);
}

static int wcd_usbss_read(struct wcd_usbss_ctxt *priv, unsigned int reg,
			  unsigned int *value)
{
	*value = 0;
	if (!priv->io_error)
		priv->io_error = regmap_read(priv->regmap, reg, value);
	return priv->io_error;
}

static int wcd_usbss_write(struct wcd_usbss_ctxt *priv, unsigned int reg,
			   unsigned int value)
{
	if (!priv->io_error)
		priv->io_error = regmap_write(priv->regmap, reg, value);
	return priv->io_error;
}

static int wcd_usbss_update_bits(struct wcd_usbss_ctxt *priv, unsigned int reg,
				 unsigned int mask, unsigned int value)
{
	if (!priv->io_error)
		priv->io_error = regmap_update_bits(priv->regmap, reg, mask, value);
	return priv->io_error;
}

static int wcd_usbss_update_bits_base(struct wcd_usbss_ctxt *priv, unsigned int reg,
		unsigned int mask, unsigned int value, bool *change, bool async, bool force)
{
	if (!priv->io_error)
		priv->io_error = regmap_update_bits_base(priv->regmap, reg, mask, value,
						       change, async, force);
	return priv->io_error;
}

static int wcd_usbss_cache_sync(struct wcd_usbss_ctxt *priv)
{
	if (!priv->io_error)
		priv->io_error = regcache_sync(priv->regmap);
	return priv->io_error;
}

struct wcd_usbss_saved_state {
	unsigned int cable_status;
	int audio_fsm_mode;
	int standby_status;
	bool in_standby;
	bool standby_enabled;
};

static int wcd_usbss_begin(struct wcd_usbss_ctxt *priv, struct wcd_usbss_saved_state *saved)
{
	mutex_lock(&priv->switch_update_lock);
	if (READ_ONCE(priv->removing) || priv->suspended) {
		mutex_unlock(&priv->switch_update_lock);
		return priv->removing ? -ENODEV : -EBUSY;
	}
	priv->io_error = 0;
	saved->cable_status = priv->cable_status;
	saved->audio_fsm_mode = priv->audio_fsm_mode;
	saved->standby_status = priv->wcd_standby_status;
	saved->in_standby = priv->is_in_standby;
	saved->standby_enabled = priv->standby_enable;
	return 0;
}

static int wcd_usbss_end(struct wcd_usbss_ctxt *priv,
			struct wcd_usbss_saved_state *saved, int ret)
{
	if (priv->io_error) {
		ret = priv->io_error;
		regcache_mark_dirty(priv->regmap);
		priv->needs_sync = true;
	}
	if (ret < 0) {
		priv->cable_status = saved->cable_status;
		priv->audio_fsm_mode = saved->audio_fsm_mode;
		priv->wcd_standby_status = saved->standby_status;
		priv->is_in_standby = saved->in_standby;
		priv->standby_enable = saved->standby_enabled;
	}
	mutex_unlock(&priv->switch_update_lock);
	return ret;
}

/* Required for kobj_attributes */
static ssize_t wcd_usbss_surge_enable_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static ssize_t wcd_usbss_surge_period_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static ssize_t wcd_usbss_standby_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static struct kobj_attribute wcd_usbss_surge_enable_attribute =
	__ATTR(surge_enable, 0220, NULL, wcd_usbss_surge_enable_store);

static struct kobj_attribute wcd_usbss_surge_period_attribute =
	__ATTR(surge_period, 0220, NULL, wcd_usbss_surge_period_store);

static struct kobj_attribute wcd_usbss_standby_enable_attribute =
	__ATTR(standby_mode, 0220, NULL, wcd_usbss_standby_store);

static struct attribute *wcd_usbss_attributes[] = {
	&wcd_usbss_surge_enable_attribute.attr,
	&wcd_usbss_surge_period_attribute.attr,
	&wcd_usbss_standby_enable_attribute.attr,
	NULL,
};

static const struct attribute_group wcd_usbss_attribute_group = {
	.attrs = wcd_usbss_attributes,
};

static int acquire_runtime_env(struct wcd_usbss_ctxt *priv)
{
	int rc = 0, retry = 0;

	mutex_lock(&priv->runtime_env_counter_lock);

	if (priv->runtime_env_counter == INT_MAX) {
		mutex_unlock(&priv->runtime_env_counter_lock);
		return -EOVERFLOW;
	}
	priv->runtime_env_counter++;
	if (priv->runtime_env_counter == 1) {
		pm_stay_awake(priv->dev);

		do {
			rc = pm_runtime_resume_and_get(priv->dev);
			if (rc >= 0)
				break;

			if (rc == -EACCES) {
				usleep_range(PM_RUNTIME_RESUME_WAIT_US_MIN,
					PM_RUNTIME_RESUME_WAIT_US_MIN + 500);
			} else {
				dev_err(priv->dev, "%s: pm_runtime_resume_and_get failed: %i\n",
						__func__, rc);
			}
		} while (++retry < PM_RUNTIME_RESUME_CNT);

		if (rc >= 0 && priv->needs_sync) {
			rc = regcache_sync(priv->regmap);
			if (rc)
				pm_runtime_put_autosuspend(priv->dev);
			else
				priv->needs_sync = false;
		}

		if (rc == -EACCES)
			dev_err(priv->dev, "%s: pm runtime in disabled state\n", __func__);

		if (rc < 0) {
			pm_relax(priv->dev);
			priv->runtime_env_counter--;
		}
	} else if (priv->runtime_env_counter <= 0) {
		dev_err(priv->dev, "%s: priv->runtime_env_counter %d underrun\n", __func__,
				priv->runtime_env_counter);
		priv->runtime_env_counter = 0;
	}

	mutex_unlock(&priv->runtime_env_counter_lock);

	return rc;
}

static void release_runtime_env(struct wcd_usbss_ctxt *priv)
{
	mutex_lock(&priv->runtime_env_counter_lock);

	if (!priv->runtime_env_counter) {
		mutex_unlock(&priv->runtime_env_counter_lock);
		return;
	}
	priv->runtime_env_counter--;
	if (priv->runtime_env_counter == 0) {
		pm_runtime_mark_last_busy(priv->dev);
		pm_runtime_put_autosuspend(priv->dev);
		pm_relax(priv->dev);
	} else if (priv->runtime_env_counter < 0) {
		dev_err(priv->dev, "%s: priv->runtime_env_counter %d underrun\n", __func__,
				priv->runtime_env_counter);
		priv->runtime_env_counter = 0;
	}

	mutex_unlock(&priv->runtime_env_counter_lock);
}

/**
 * wcd_usbss_sbu_switch_orientation() - Determine SBU switch orientation based on switch settings.
 *
 * This function is used to determine SBU switch orientation of the WCD USBSS. INVALID_ORIENTATION
 * in enum wcd_usbss_sbu_switch_orientation represents an error state where none of the defined
 * orientations can be inferred by the switch settings.
 *
 * Return: Returns an enum wcd_usbss_sbu_switch_orientation to client. INVALID_ORIENTATION is
 *	   returned if the driver is not probed or if undefined switch settings are discovered.
 */
static enum wcd_usbss_sbu_switch_orientation
wcd_usbss_get_sbu_switch_orientation_ctx(struct wcd_usbss_ctxt *priv)
{
	unsigned int read_val = 0;
	int ret = 0;

	/* check if driver is probed and private context is init'ed */
	if (priv == NULL)
		return INVALID_ORIENTATION;

	if (!priv->regmap)
		return INVALID_ORIENTATION;

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
			__func__, ret);
		return ret;
	}

	wcd_usbss_read(priv, WCD_USBSS_SWITCH_SELECT0, &read_val);
	release_runtime_env(priv);
	if ((read_val & 0x3) == 0x1)
		return GND_SBU1_ORIENTATION_B;
	if ((read_val & 0x3) == 0x2)
		return GND_SBU2_ORIENTATION_A;
	return INVALID_ORIENTATION;
}

/*
 * wcd_usbss_set_switch_settings_enable() - Configure a specified WCD USBSS switch.
 * @switch_type: Switch to be enabled/disabled.
 * @switch_setting: Enable or disable.
 *
 * This function will set or reset a specific bit in the WCD_USBSS_SWITCH_SETTINGS_ENABLE register.
 * There is a check that switch_type represents a bit in this register. Update the definition of
 * enum wcd_usbss_switch_type switch_type if the bits in WCD_USBSS_SWITCH_SETTINGS_ENABLE change.
 *
 * Return : Returns int on whether the switch configuration happened or not. -ENODEV is returned if
 *	    the driver is not probed.
 */
static int wcd_usbss_set_switch_settings_enable_ctx(struct wcd_usbss_ctxt *priv,
		enum wcd_usbss_switch_type switch_type,
					 enum wcd_usbss_switch_state switch_state)
{
	int ret = 0;
	/* check if driver is probed and private context is initialized */
	if (priv == NULL)
		return -ENODEV;

	if ((!priv->regmap) || (switch_type < MIN_SWITCH_TYPE_NUM) ||
	    (switch_type > MAX_SWITCH_TYPE_NUM) ||
	    (switch_state != USBSS_SWITCH_DISABLE && switch_state != USBSS_SWITCH_ENABLE))
		return -EINVAL;

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
					__func__, ret);
		return ret;
	}
	wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
			   1 << switch_type, switch_state << switch_type);

	release_runtime_env(priv);

	return ret;
}

/*
 * wcd_usbss_linearizer_rdac_cal_code_select() - Configure the linearizer calibration codes source.
 *
 * @source: HW (hardware) or SW (software).
 *
 * This function configures the linearizer to use SW or HW as the sources for the calibration codes.
 *
 * Return: Returns int on whether the switch configuration happened or not. -ENODEV is returned if
 *	   the driver is not probed.
 */
static int wcd_usbss_linearizer_rdac_cal_code_select_ctx(struct wcd_usbss_ctxt *priv,
		enum linearizer_rdac_cal_code_select source)
{
	int ret = 0;
	/* check if driver is probed and private context is initialized */
	if (priv == NULL)
		return -ENODEV;

	if ((!priv->regmap) || (source != LINEARIZER_SOURCE_HW &&
					   source != LINEARIZER_SOURCE_SW))
		return -EINVAL;

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
					__func__, ret);
		return ret;
	}

	wcd_usbss_update_bits(priv, WCD_USBSS_FUNCTION_ENABLE, 0x4, source << 2);

	release_runtime_env(priv);

	return 0;
}

/*
 * wcd_usbss_set_linearizer_sw_tap() - Configure linearizer audio and ground software tap values.
 *
 * @aud_tap: 10-bit tap code for the L and R audio software tap registers.
 * @gnd_tap: 10-bit tap code for the L and R ground software tap registers.
 *
 * This function writes tap values to the left and right tap registers for the audio and ground
 * FETs. Note that the tap values are 10 bits and cannot exceed 0x3FF, but they can be 0.
 *
 * Return: Returns int on whether the switch configuration happened or not. -ENODEV is returned if
 *	   the driver is not probed.
 */
static int wcd_usbss_set_linearizer_sw_tap_ctx(struct wcd_usbss_ctxt *priv,
		uint32_t aud_tap, uint32_t gnd_tap)
{
	int ret = 0;
	uint32_t lsb_mask = 0xFF, msb_shift = 8;

	/* check if driver is probed and private context is initialized */
	if (priv == NULL)
		return -ENODEV;

	if ((!priv->regmap) || aud_tap > 0x3FF || gnd_tap > 0x3FF)
		return -EINVAL;

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
					__func__, ret);
		return ret;
	}

	/* Audio left */
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_AUD_L_LSB, 0xFF,
			   aud_tap & lsb_mask);
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_AUD_L_MSB, 0x3,
			   aud_tap >> msb_shift);
	/* Audio right */
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_AUD_R_LSB, 0xFF,
			   aud_tap & lsb_mask);
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_AUD_R_MSB, 0x3,
			   aud_tap >> msb_shift);
	/* Ground left */
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_GND_L_LSB, 0xFF,
			   gnd_tap & lsb_mask);
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_GND_L_MSB, 0x3,
			   gnd_tap >> msb_shift);
	/* Ground right */
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_GND_R_LSB, 0xFF,
			   gnd_tap & lsb_mask);
	wcd_usbss_update_bits(priv, WCD_USBSS_SW_TAP_GND_R_MSB, 0x3,
			   gnd_tap >> msb_shift);

	release_runtime_env(priv);

	return ret;
}

bool wcd_usbss_readable_register(struct device *dev, unsigned int reg)
{
	struct wcd_usbss_ctxt *priv = dev_get_drvdata(dev);

	if (reg <= (WCD_USBSS_BASE + 1) || reg > WCD_USBSS_MAX_REGISTER)
		return false;

	if ((priv && priv->version_valid && priv->version == WCD_USBSS_1_X) &&
			(reg >= WCD_USBSS_EFUSE_CTL &&
			reg <= WCD_USBSS_ANA_CSR_DBG_CTL))
		return false;

	return wcd_usbss_reg_access[WCD_USBSS_REG(reg)] & RD_REG;
}

/*
 * wcd_usbss_register_update() - Write or read multiple USB-SS registers.
 *
 * @reg_arr: Array of {register address, register value} pairs.
 * @write: Bool selecting whether to write values from reg_arr or read values to store in reg_arr.
 * @arr_size: Number of {register address, register value} pairs in reg_arr.
 *
 * This function writes or reads arr_size number of register values, specified in reg_arr. If write
 * is true, this function will write all the values specified in reg_arr to corresponding USB-SS
 * registers. If write is false, this function will read the USB-SS registers specified in reg_arr
 * and write those values to the corresponding register values in reg_arr. If any register write or
 * read fails, this function prints an error message and exits.
 *
 * Return: Returns int on whether the register writes/reads were successful. -ENODEV is
 *	   returned if the driver is not probed.
 */
static int wcd_usbss_register_update_ctx(struct wcd_usbss_ctxt *priv,
		uint32_t reg_arr[][2], bool write, size_t arr_size)
{
	size_t i;
	int rc = 0;
	uint32_t reg_mask = 0xFF;

	/* check if driver is probed and private context is initialized */
	if (priv == NULL)
		return -ENODEV;

	if (!priv->regmap || (!reg_arr && arr_size))
		return -EINVAL;

	rc = acquire_runtime_env(priv);
	if (rc < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
					__func__, rc);
		return rc;
	}

	for (i = 0; i < arr_size; i++) {
		if (write) {
			rc = wcd_usbss_write(priv, reg_arr[i][0],
					  reg_arr[i][1] & reg_mask);
			if (rc != 0) {
				dev_err(priv->dev,
					"%s: USB-SS register 0x%x (value of 0x%x) write failed\n",
					__func__, reg_arr[i][0], reg_arr[i][1]);
				goto err;
			}
		} else {
			rc = wcd_usbss_read(priv, reg_arr[i][0], &reg_arr[i][1]);
			if (rc != 0) {
				dev_err(priv->dev,
					"%s: USB-SS register 0x%x read failed\n", __func__,
					reg_arr[i][0]);
				goto err;
			}
		}
	}
err:
	release_runtime_env(priv);

	return rc;
}

/*
 * wcd_usbss_is_in_reset_state(priv) - Check whether a negative surge ESD event has occurred.
 *
 * This function has a series of three checks to determine whether a negative surge ESD event has
 * occurred. If any of the three check conditions is met, it is concluded that a negative surge
 * ESD event has occurred. The checks include the following:
 * 1. Register WCD_USBSS_CPLDO_CTL2 reads 0xFF
 * 2. Register WCD_USBSS_RCO_MISC2 Bit<1> reads 0 at least once in NUM_RCO_MISC2_READ reads
 * 3. Register 0x06 Bit<0> reads 1 after toggling register WCD_USBSS_PMP_MISC1 Bit<0> from
 *    0 --> 1 --> 0
 *
 * Return: Returns true if any check(s) fail, false otherwise.
 */
static bool wcd_usbss_is_in_reset_state(struct wcd_usbss_ctxt *priv)
{
	bool ret = false;
	int i = 0;
	int rc = 0;
	unsigned int read_val = 0;

	/* Check 1: Read WCD_USBSS_CPLDO_CTL2 */
	rc = wcd_usbss_read(priv, WCD_USBSS_CPLDO_CTL2, &read_val);
	if (rc != 0)
		goto done;

	if (read_val != 0xFF) {
		dev_err(priv->dev, "%s: Surge check #1 failed\n", __func__);
		ret = true;
		goto done;
	}

	/* Check 2: Read WCD_USBSS_RCO_MISC2 */
	for (i = 0; i < NUM_RCO_MISC2_READ; i++) {
		rc = wcd_usbss_read(priv, WCD_USBSS_RCO_MISC2, &read_val);
		if (rc != 0)
			goto done;

		if ((read_val & 0x2) == 0)
			break;
		if (i == (NUM_RCO_MISC2_READ - 1)) {
			dev_err(priv->dev, "%s: Surge check #2 failed\n", __func__);
			ret = true;
			goto done;
		}
	}

	/* MG comparator bias current to 1uA */
	wcd_usbss_write(priv, WCD_USBSS_MG1_BIAS, MG_BIAS_CURRENT);
	wcd_usbss_write(priv, WCD_USBSS_MG2_BIAS, MG_BIAS_CURRENT);

done:
	/* All checks passed, so a negative surge ESD event has not occurred */
	return ret;
}

/*
 * wcd_usbss_reset_routine - Uses cached state to restore USB-SS registers after a negative surge.
 *
 * Return: Returns int return value from wcd_usbss_switch_update()
 */
static int wcd_usbss_reset_routine(struct wcd_usbss_ctxt *priv)
{
	mutex_lock(&priv->io_lock);
	priv->prev_pg_valid = false;
	mutex_unlock(&priv->io_lock);
	/* Mark the cache as dirty to force a flush */
	regcache_mark_dirty(priv->regmap);
	wcd_usbss_cache_sync(priv);
	/* Write 0xFF to WCD_USBSS_CPLDO_CTL2 */
	wcd_usbss_update_bits(priv, WCD_USBSS_CPLDO_CTL2, 0xFF, 0xFF);

	/* Set RCO_EN: WCD_USBSS_USB_SS_CNTL Bit<3> --> 0x0 --> 0x1 */
	wcd_usbss_update_bits(priv, WCD_USBSS_USB_SS_CNTL, 0x8, 0x0);
	wcd_usbss_update_bits(priv, WCD_USBSS_USB_SS_CNTL, 0x8, 0x8);

	return priv->io_error;
}
/* Called with switch_update_lock mutex locked */
static void wcd_usbss_standby_control_locked(struct wcd_usbss_ctxt *priv, bool enter_standby)
{
	int rc = 0;

	if (priv->is_in_standby == enter_standby)
		return;

	if (enter_standby) {
		dev_dbg(priv->dev, "%s: Enabling standby mode\n",
			__func__);
		rc = wcd_usbss_update_bits(priv, WCD_USBSS_USB_SS_CNTL,
				0x10, 0x10);
		if (rc < 0)
			dev_err(priv->dev, "%s: enter standby failed\n", __func__);
		else
			priv->is_in_standby = true;
	} else {
		dev_dbg(priv->dev, "%s: Disabling standby mode\n",
			__func__);
		rc = wcd_usbss_update_bits(priv, WCD_USBSS_USB_SS_CNTL,
				0x10, 0x00);
		if (rc < 0) {
			dev_err(priv->dev, "%s: exit standby failed\n", __func__);
		} else {
			/* 10ms wait recommended to get WCD USBSS out of standby */
			usleep_range(10000, 10100);
			priv->is_in_standby = false;
		}
	}
}

static int wcd_usbss_standby_control(struct wcd_usbss_ctxt *priv, bool enter_standby)
{
	int ret = 0;

	if (!priv->standby_enable)
		return 0;

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
				__func__, ret);
		goto done;
	}

	wcd_usbss_standby_control_locked(priv, enter_standby);
	release_runtime_env(priv);

done:

	return ret;
}

static ssize_t wcd_usbss_surge_enable_store_ctx(struct wcd_usbss_ctxt *priv, struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int enable = 0;

	if (kstrtouint(buf, 10, &enable) < 0 || enable > 1)
		return -EINVAL;

	/* Return if period is 0ms */
	if (!priv->surge_timer_period_ms)
		priv->surge_timer_period_ms = DEFAULT_SURGE_TIMER_PERIOD_MS;

	WRITE_ONCE(priv->surge_enable, enable);

	return count;
}

static ssize_t wcd_usbss_surge_period_store_ctx(struct wcd_usbss_ctxt *priv, struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int period_sec = 0;

	if (kstrtouint(buf, 10, &period_sec) < 0)
		return -EINVAL;

	if (period_sec < MIN_SURGE_TIMER_PERIOD_SEC || period_sec > MAX_SURGE_TIMER_PERIOD_SEC)
		return -EINVAL;
	WRITE_ONCE(priv->surge_timer_period_ms, SEC_TO_MS * period_sec);

	if (!priv->surge_thread)
		return count;

	/* Wake up thread if usb is connected and surge is enabled */
	if (priv->cable_status && priv->surge_enable)
		wake_up_process(priv->surge_thread);

	return count;
}

static ssize_t wcd_usbss_standby_store_ctx(struct wcd_usbss_ctxt *priv, struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int enable = 0;
	int ret;

	if (kstrtouint(buf, 10, &enable) < 0 || enable > 1)
		return -EINVAL;

	/* temporarily enabling standby to force proper state update */
	priv->standby_enable = true;

	if (enable) {
		if (!priv->cable_status)
			ret = wcd_usbss_standby_control(priv, true);
		else
			ret = wcd_usbss_standby_control(priv, false);
	} else {
		ret = wcd_usbss_standby_control(priv, false);
	}

	if (ret)
		return ret;
	priv->standby_enable = enable;

	return count;
}
/*
 * wcd_usbss_surge_kthread_fn - checks for a negative surge reset at a given period interval
 *
 * Returns 0
 */
static int wcd_usbss_surge_kthread_fn(void *p)
{
	struct wcd_usbss_ctxt *priv = p;
	struct wcd_usbss_saved_state saved;
	bool notify;
	int ret;

	while (!kthread_should_stop()) {
		notify = false;
		ret = wcd_usbss_begin(priv, &saved);
		if (!ret) {
			ret = acquire_runtime_env(priv);
			if (!ret) {
				if (READ_ONCE(priv->surge_enable) &&
				    wcd_usbss_is_in_reset_state(priv)) {
					ret = wcd_usbss_reset_routine(priv);
					notify = !ret && (priv->cable_status &
						(BIT(WCD_USBSS_AATC) |
						 BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
						 BIT(WCD_USBSS_HSJ_CONNECT) |
						 BIT(WCD_USBSS_GND_MIC_SWAP_HSJ)));
				}
				release_runtime_env(priv);
			}
			ret = wcd_usbss_end(priv, &saved, ret);
			notify = notify && !ret;
		}
		if (notify && !READ_ONCE(priv->removing))
			blocking_notifier_call_chain(&priv->wcd_usbss_notifier,
						    WCD_USBSS_SURGE_RESET_EVENT, NULL);
		if (ret && ret != -EBUSY && ret != -ENODEV)
			dev_warn_ratelimited(priv->dev, "Surge check failed: %d\n", ret);
		msleep_interruptible(READ_ONCE(priv->surge_timer_period_ms));
	}
	return 0;
}

/*
 * wcd_usbss_enable_surge_kthread - routine for creating and deploying a kthread to handle surge
 *								   protection.
 */
static int wcd_usbss_enable_surge_kthread(struct wcd_usbss_ctxt *priv)
{
	priv->surge_thread = kthread_run(wcd_usbss_surge_kthread_fn, priv, "wcd-usbss-surge");
	if (IS_ERR(priv->surge_thread)) {
		int ret = PTR_ERR(priv->surge_thread);

		priv->surge_thread = NULL;
		return ret;
	}
	return 0;
}

/*
 * wcd_usbss_disable_surge_kthread - routine for stopping a kthread that handles surge
 *								    protection.
 */
static void wcd_usbss_disable_surge_kthread(struct wcd_usbss_ctxt *priv)
{
	if (priv->surge_thread) {
		kthread_stop(priv->surge_thread);
		priv->surge_thread = NULL;
	}
}

static int wcd_usbss_sysfs_init(struct wcd_usbss_ctxt *priv)
{
	int ret;

	priv->surge_kobject = kobject_create_and_add("wcd_usbss", kernel_kobj);
	if (!priv->surge_kobject)
		return -ENOMEM;
	ret = sysfs_create_group(priv->surge_kobject, &wcd_usbss_attribute_group);
	if (ret) {
		kobject_put(priv->surge_kobject);
		priv->surge_kobject = NULL;
	}
	return ret;
}

static int wcd_usbss_usbc_event_changed(struct notifier_block *nb,
				      unsigned long evt, void *ptr)
{
	struct wcd_usbss_ctxt *priv =
			container_of(nb, struct wcd_usbss_ctxt, ucsi_nb);
	struct device *dev;
	enum typec_accessory acc;

	if (!ptr || READ_ONCE(priv->removing))
		return NOTIFY_DONE;
	acc = ((struct ucsi_glink_constat_info *)ptr)->acc;

	if (!priv)
		return -EINVAL;

	dev = priv->dev;
	if (!dev)
		return -EINVAL;

	dev_dbg(dev, "%s: USB change event received, supply mode %d, usbc mode %d, expected %d\n",
			__func__, acc, atomic_read(&priv->usbc_mode),
			TYPEC_ACCESSORY_AUDIO);

	switch (acc) {
	case TYPEC_ACCESSORY_AUDIO:
	case TYPEC_ACCESSORY_NONE:
		if (atomic_read(&(priv->usbc_mode)) == acc)
			break; /* filter notifications received before */
		atomic_set(&(priv->usbc_mode), acc);

		dev_dbg(dev, "%s: queueing usbc_analog_work\n",
			__func__);
		pm_stay_awake(priv->dev);
		queue_work(system_freezable_wq, &priv->usbc_analog_work);
		break;
	default:
		break;
	}

	return 0;
}

static int wcd_usbss_usbc_analog_setup_switches(struct wcd_usbss_ctxt *priv)
{
	int rc = 0;
	int mode;
	struct device *dev;
	bool cable_status_cache = false;

	if (!priv)
		return -EINVAL;

	dev = priv->dev;
	if (!dev)
		return -EINVAL;

	mutex_lock(&priv->notification_lock);
	/* get latest mode again within locked context */
	mode = atomic_read(&(priv->usbc_mode));

	dev_dbg(dev, "%s: setting GPIOs active = %d cable_status = %d mode = %d\n",
		__func__, mode != TYPEC_ACCESSORY_NONE, priv->cable_status, mode);

	switch (mode) {
	/* add all modes WCD USBSS should notify for in here */
	case TYPEC_ACCESSORY_AUDIO:
		/*
		 * If cable_type is already decided, update the cable_status to
		 * avoid reconfiguration of AATC switch settings again
		 */
		if (priv->cable_status & (BIT(WCD_USBSS_AATC) |
					  BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
					  BIT(WCD_USBSS_HSJ_CONNECT) |
					  BIT(WCD_USBSS_GND_MIC_SWAP_HSJ)))
			cable_status_cache = true;
		/* notify call chain on event */
		blocking_notifier_call_chain(&priv->wcd_usbss_notifier,
					     mode, &cable_status_cache);
		break;
	case TYPEC_ACCESSORY_NONE:
		/* notify call chain on event */
		blocking_notifier_call_chain(&priv->wcd_usbss_notifier,
				TYPEC_ACCESSORY_NONE, NULL);
		break;
	default:
		/* ignore other usb connection modes */
		break;
	}

	mutex_unlock(&priv->notification_lock);
	return rc;
}

static int wcd_usbss_validate_display_port_settings(struct wcd_usbss_ctxt *priv,
						enum wcd_usbss_cable_types ctype)
{
	unsigned int sts;
	int rc;

	rc = wcd_usbss_read(priv, WCD_USBSS_SWITCH_STATUS1, &sts);
	if (rc)
		return rc;

	sts &= 0xCC;
	pr_info("DPAUX switch status (MG1/2): %08x\n", sts);

	if (ctype == WCD_USBSS_DP_AUX_CC1 && sts == 0x48)
		return 0;

	if (ctype == WCD_USBSS_DP_AUX_CC2 && sts == 0x84)
		return 0;

	pr_err("Failed to update switch for display port\n");
	rc = -EINVAL;

	return rc;
}

static int wcd_usbss_switch_update_defaults(struct wcd_usbss_ctxt *priv)
{
	dev_dbg(priv->dev, "restoring defaults\n");
	/* Disable all switches */
	wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x00);
	/* Select MG1 for AGND_SWITCHES */
	wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SELECT1, 0x01, 0x00);
	/* Select GSBU1 and MG1 for MIC_SWITCHES */
	wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SELECT0, 0x03, 0x00);
	/* Enable OVP_MG1_BIAS PCOMP_DYN_BST_EN */
	wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS, 0x08, 0x08);
	/* Enable OVP_MG2_BIAS PCOMP_DYN_BST_EN */
	wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS, 0x08, 0x08);
	wcd_usbss_update_bits_base(priv, WCD_USBSS_AUDIO_FSM_START, 0x01,
			0x01, NULL, false, true);
	/* Select DN for DNL_SWITHCES and DP for DPR_SWITCHES */
	wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SELECT0, 0x3C, 0x14);
	wcd_usbss_update_bits(priv, WCD_USBSS_USB_SS_CNTL, 0x07, 0x05); /* Mode5: USB*/
	wcd_usbss_write(priv, WCD_USBSS_PMP_EN, 0x0);
	if (priv->version == WCD_USBSS_2_0)
		wcd_usbss_update_bits(priv, WCD_USBSS_PMP_OUT1,
				0x40, 0x00);
	wcd_usbss_write(priv, WCD_USBSS_EXT_SW_CTRL_1, 0x00);
	wcd_usbss_write(priv, WCD_USBSS_EXT_LIN_EN, 0x00);
	wcd_usbss_write(priv, WCD_USBSS_PMP_CLK, 0x10);
	wcd_usbss_write(priv, WCD_USBSS_SW_LIN_CTRL_1, 0x00);
	wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_2, 0x00);
	wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_3, 0x00);

	/* Once plug-out done, restore to MANUAL mode */
	priv->audio_fsm_mode = WCD_USBSS_AUDIO_MANUAL;
	return 0;
}

static void wcd_usbss_update_reg_init(struct wcd_usbss_ctxt *priv, struct regmap *regmap)
{
	if (priv->audio_fsm_mode == WCD_USBSS_AUDIO_FSM)
		wcd_usbss_update_bits(priv, WCD_USBSS_FUNCTION_ENABLE, 0x03,
				0x02); /* AUDIO_FSM mode */
	else
		wcd_usbss_update_bits(priv, WCD_USBSS_FUNCTION_ENABLE, 0x03,
				0x01); /* AUDIO_MANUAL mode */

	/* Enable dynamic boosting for DP and DN */
	wcd_usbss_update_bits(priv,
						WCD_USBSS_DP_DN_MISC1, 0x09, 0x09);
	/* Enable dynamic boosting for MG1 OVP */
	wcd_usbss_update_bits(priv,
						WCD_USBSS_MG1_MISC, 0x24, 0x24);
	/* Enable dynamic boosting for MG2 OVP */
	wcd_usbss_update_bits(priv,
						WCD_USBSS_MG2_MISC, 0x24, 0x24);

	/* Disable Equalizer */
	wcd_usbss_update_bits(priv, WCD_USBSS_EQUALIZER1,
			WCD_USBSS_EQUALIZER1_EQ_EN_MASK, 0x00);
	/* For surge reset routine: Write WCD_USBSS_CPLDO_CTL2 --> 0xFF */
	wcd_usbss_update_bits(priv, WCD_USBSS_CPLDO_CTL2, 0xFF, 0xFF);
}

#define AUXP_M_EN_MASK	(WCD_USBSS_SWITCH_SETTINGS_ENABLE_DP_AUXM_TO_MGX_SWITCHES_MASK |\
			WCD_USBSS_SWITCH_SETTINGS_ENABLE_DP_AUXP_TO_MGX_SWITCHES_MASK)

static int wcd_usbss_display_port_switch_update(struct wcd_usbss_ctxt *priv,
					enum wcd_usbss_cable_types ctype)
{
	pr_info("Configuring display port for ctype %d\n", ctype);

	/* Disable AUX switches */
	wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE, AUXP_M_EN_MASK, 0x00);

	/* Select MG1 for AUXP and MG2 for AUXM */
	if (ctype == WCD_USBSS_DP_AUX_CC1)
		wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SELECT0, 0xC0, 0x40);
	/* Select MG2 for AUXP and MG1 for AUXM */
	else
		wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SELECT0, 0xC0, 0x80);

	/* Enable DP_AUXP_TO_MGX and DP_AUXM_TO_MGX switches */
	wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE, AUXP_M_EN_MASK, 0x60);
	return wcd_usbss_validate_display_port_settings(priv, ctype);
}

static void wcd_usbss_dpdm_switch_connect(struct wcd_usbss_ctxt *priv, bool connect)
{
	if (connect)
		wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
				0x18, 0x18);
	else
		wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
				0x18, 0x00);
}

static const char *status_to_str(int status)
{
	switch (status) {
	case WCD_USBSS_LPD_USB_MODE_CLEAR:
		return "STANDBY";
	case WCD_USBSS_LPD_MODE_SET:
		return "LPD";
	case WCD_USBSS_USB_MODE_SET:
		return "USB";
	case WCD_USBSS_LPD_USB_MODE_SET:
		return "LPD_USB";
	case WCD_USBSS_AUDIO_MODE_SET:
		return "AUDIO";
	case WCD_USBSS_LPD_CONTINUOUS_1:
		fallthrough;
	case WCD_USBSS_LPD_CONTINUOUS_2:
		return "LPD_CONTINUOUS";
	default:
		return "UNDEFINED";
	}
}

static void wcd_usbss_pd_pu_enable(struct wcd_usbss_ctxt *priv)
{
	wcd_usbss_update_bits(priv, WCD_USBSS_PMP_OUT1, 0x20, 0x00);
	/* Enable D+/D- 1M & 400K PLDN */
	wcd_usbss_update_bits(priv, WCD_USBSS_BIAS_TOP, 0x20, 0x00);

	/* Enable DP/DN 2K PLDN */
	wcd_usbss_update_bits(priv, WCD_USBSS_DP_BIAS, 0x01, 0x01);
	wcd_usbss_update_bits(priv, WCD_USBSS_DN_BIAS, 0x01, 0x01);

	if (!priv->usb_sbu_compliance) {
		/* Enable SBU1/2 2K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS, 0x01, 0x01);
		wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS, 0x01, 0x01);
	}
}

/* to use with DPDM switch selection */
#define DPDM_SEL_MASK       (WCD_USBSS_SWITCH_SELECT0_DPR_SWITCHES_MASK |\
					WCD_USBSS_SWITCH_SELECT0_DNL_SWITCHES_MASK)
#define DPDM_SEL_ENABLE     ((0x1 << WCD_USBSS_SWITCH_SELECT0_DPR_SWITCHES_SHIFT) |\
					(0x1 << WCD_USBSS_SWITCH_SELECT0_DNL_SWITCHES_SHIFT))
#define DPDM_SEL_DISABLE    0x0

/* to use with DPDM switch enable/disable*/
#define DPDM_SW_EN_MASK     (WCD_USBSS_SWITCH_SETTINGS_ENABLE_DPR_SWITCHES_MASK |\
					WCD_USBSS_SWITCH_SETTINGS_ENABLE_DNL_SWITCHES_MASK)
#define DPDM_SW_ENABLE      ((0x1 << WCD_USBSS_SWITCH_SETTINGS_ENABLE_DNL_SWITCHES_SHIFT) |\
				(0x1 << WCD_USBSS_SWITCH_SETTINGS_ENABLE_DPR_SWITCHES_SHIFT))
#define DPDM_SW_DISABLE     0x0

/*
 * wcd_usbss_dpdm_switch_update - configure WCD USBSS DP/DM switch position
 *
 * @sw_en: enable or disable DP/DM switches.
 * @eq_en: enable or disable equalizer. Usually true in case of USB high-speed.
 *
 * Returns zero for success, a negative number on error.
 */
static int wcd_usbss_dpdm_switch_update_ctx(struct wcd_usbss_ctxt *priv, bool sw_en, bool eq_en)
{
	int ret = 0;

	/* check if driver is probed and private context is initialized */
	if (priv == NULL)
		return -ENODEV;

	if (!priv->regmap)
		return -EINVAL;

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
				__func__, ret);
		return ret;
	}
	ret = wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
				DPDM_SW_EN_MASK, (sw_en ? DPDM_SW_ENABLE : DPDM_SW_DISABLE));
	if (ret)
		pr_err("%s(): Failed to write dpdm_en_value ret:%d\n", __func__, ret);

	ret = wcd_usbss_update_bits(priv, WCD_USBSS_EQUALIZER1,
				WCD_USBSS_EQUALIZER1_EQ_EN_MASK,
				(eq_en ? WCD_USBSS_EQUALIZER1_EQ_EN_MASK : 0x0));
	if (ret)
		pr_err("%s(): Failed to write equalizer1_en ret:%d\n", __func__, ret);

	release_runtime_env(priv);

	return ret;
}

static int wcd_usbss_dpdm_switch_update_from_handler(struct wcd_usbss_ctxt *priv,
		bool sw_en, bool eq_en)
{
	int ret = 0;

	ret = wcd_usbss_update_bits(priv, WCD_USBSS_SWITCH_SETTINGS_ENABLE,
				DPDM_SW_EN_MASK, (sw_en ? DPDM_SW_ENABLE : DPDM_SW_DISABLE));
	if (ret)
		pr_err("%s(): Failed to write dpdm_en_value ret:%d\n", __func__, ret);

	ret = wcd_usbss_update_bits(priv, WCD_USBSS_EQUALIZER1,
				WCD_USBSS_EQUALIZER1_EQ_EN_MASK,
				(eq_en ? WCD_USBSS_EQUALIZER1_EQ_EN_MASK : 0x0));
	if (ret)
		pr_err("%s(): Failed to write equalizer1_en ret:%d\n", __func__, ret);

	return ret;
}

/* wcd_usbss_audio_config - configure audio for power mode and Impedance calculations
 *
 * @enable: enable/disable switch settings for MIC and SENSE for impedance readings
 * @config_type: Config type to configure audio
 * @power_mode: power mode type to config
 *
 * Returns int on whether the config happened or not. -ENODEV is returned
 * in case if the driver is not probed.
 */

static int wcd_usbss_audio_config_ctx(struct wcd_usbss_ctxt *priv,
		bool enable, enum wcd_usbss_config_type config_type,
			unsigned int power_mode)
{

	int rc = 0;
	unsigned int current_power_mode;
	unsigned int force_power_mode = HIFI_MODE4;

	/* check if driver is probed and private context is init'ed */
	if (priv == NULL)
		return -ENODEV;

	if (!priv->regmap)
		return -EINVAL;

	pr_info("%s: connect_status = 0x%x, power mode = %d\n",
		__func__, priv->cable_status, power_mode);

	if (!(priv->cable_status & (BIT(WCD_USBSS_AATC) |
					       BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
					       BIT(WCD_USBSS_HSJ_CONNECT) |
					       BIT(WCD_USBSS_GND_MIC_SWAP_HSJ))))
		return 0;

	rc = acquire_runtime_env(priv);
	if (rc < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
				__func__, rc);
		return rc;
	}

	wcd_usbss_read(priv, WCD_USBSS_USB_SS_CNTL, &current_power_mode);
	if ((current_power_mode & 0x07) == power_mode)
		goto exit;

	switch (config_type) {
	case WCD_USBSS_CONFIG_TYPE_POWER_MODE:
		/* switching to MBHC mode */
		if (power_mode == 0x1) {
			wcd_usbss_write(priv, WCD_USBSS_PMP_EN, 0xF);
			wcd_usbss_write(priv, WCD_USBSS_EXT_LIN_EN, 0xC4);
			wcd_usbss_update_bits(priv,
					WCD_USBSS_USB_SS_CNTL, 0x07, power_mode);
			wcd_usbss_write(priv, WCD_USBSS_PMP_CLK, 0x10);
		} else { /* switching to ULP/HiFi/Std */
			power_mode = force_power_mode;
			if (power_mode == 0x2) /* ULP */
				wcd_usbss_write(priv, WCD_USBSS_PMP_CLK, 0x1C);
			else
				wcd_usbss_write(priv, WCD_USBSS_PMP_CLK, 0x10);

			wcd_usbss_update_bits(priv,
					WCD_USBSS_USB_SS_CNTL, 0x07, power_mode);
			wcd_usbss_write(priv, WCD_USBSS_PMP_EN, 0x0);
			wcd_usbss_write(priv, WCD_USBSS_EXT_LIN_EN, 0x00);
		}
		break;
	default:
		pr_err("%s Invalid config type %d\n", __func__, config_type);
		rc = -EINVAL;
	}

exit:
	release_runtime_env(priv);

	return rc;
}

/*
 * wcd_usbss_switch_update - configure WCD USBSS switch position based on
 *  cable type and status
 *
 * @ctype - cable type
 * @connect_status - cable connected/disconnected status
 *
 * Returns int on whether the switch happened or not. -ENODEV is returned
 *  in case if the driver is not probed
 */
static int wcd_usbss_switch_update_ctx(struct wcd_usbss_ctxt *priv,
		enum wcd_usbss_cable_types ctype,
							enum wcd_usbss_cable_status connect_status)
{
	int i = 0, ret = 0;
	bool audio_switch = false;

	if (ctype < 0 || ctype >= WCD_USBSS_CABLE_TYPE_MAX ||
	    (connect_status != WCD_USBSS_CABLE_CONNECT &&
	     connect_status != WCD_USBSS_CABLE_DISCONNECT))
		return -EINVAL;

	/* check if driver is probed and private context is init'ed */
	if (priv == NULL)
		return -ENODEV;

	if (!priv->regmap)
		return -EINVAL;

	pr_info("%s: ctype = %d, connect_status = %d\n",
		__func__, ctype, connect_status);

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
				__func__, ret);
		return ret;
	}

	if (connect_status == WCD_USBSS_CABLE_DISCONNECT) {
		priv->cable_status &= ~BIT(ctype);

		switch (ctype) {
		case WCD_USBSS_USB:
			/* Keep DP/DM switch on but disable EQ */
			if (priv->standby_enable && priv->is_in_standby)
				wcd_usbss_dpdm_switch_update_ctx(priv, false, false);
			else
				wcd_usbss_dpdm_switch_update_ctx(priv, true, false);
			break;
		case WCD_USBSS_DP_AUX_CC1:
			fallthrough;
		case WCD_USBSS_DP_AUX_CC2:
			/* Disable AUX switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT0, 0xC0, 0x00);
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE,
					AUXP_M_EN_MASK, 0x00);
			priv->cable_status &= ~BIT(WCD_USBSS_DP_AUX_CC1);
			priv->cable_status &= ~BIT(WCD_USBSS_DP_AUX_CC2);
			break;
		case WCD_USBSS_AATC:
			priv->cable_status &= ~BIT(WCD_USBSS_GND_MIC_SWAP_AATC);
			audio_switch = true;
			break;
		case WCD_USBSS_GND_MIC_SWAP_AATC:
			priv->cable_status &= ~BIT(WCD_USBSS_AATC);
			audio_switch = true;
			break;
		case WCD_USBSS_HSJ_CONNECT:
			priv->cable_status &= ~BIT(WCD_USBSS_GND_MIC_SWAP_HSJ);
			audio_switch = true;
			break;
		case WCD_USBSS_GND_MIC_SWAP_HSJ:
			priv->cable_status &= ~BIT(WCD_USBSS_HSJ_CONNECT);
			audio_switch = true;
			break;
		default:
			break;
		}

		/* reset to defaults when all cable types are disconnected */
		if (!priv->cable_status && audio_switch) {
			wcd_usbss_switch_update_defaults(priv);
			if (priv->standby_enable) {
				wcd_usbss_dpdm_switch_connect(priv, false);
				wcd_usbss_standby_control_locked(priv, true);
				priv->wcd_standby_status = WCD_USBSS_LPD_USB_MODE_CLEAR;
				dev_dbg(priv->dev, "wcd state transition to %s complete\n",
						status_to_str(priv->wcd_standby_status));
			} else {
				wcd_usbss_dpdm_switch_connect(priv, true);
				priv->wcd_standby_status = WCD_USBSS_USB_MODE_SET;
				dev_dbg(priv->dev, "wcd state transition to %s complete\n",
						status_to_str(priv->wcd_standby_status));
			}
		}
	} else if (connect_status == WCD_USBSS_CABLE_CONNECT) {
		priv->cable_status |= BIT(ctype);

		wcd_usbss_pd_pu_enable(priv);
		wcd_usbss_standby_control_locked(priv, false);

		switch (ctype) {
		case WCD_USBSS_USB:
			wcd_usbss_dpdm_switch_update_ctx(priv, true, true);
			break;
		case WCD_USBSS_AATC:
			/* Update power mode to mode 1 for AATC */
			wcd_usbss_update_bits(priv,
				WCD_USBSS_USB_SS_CNTL, 0x07, 0x01);
			wcd_usbss_write(priv, WCD_USBSS_PMP_EN, 0xF);
			wcd_usbss_write(priv, WCD_USBSS_EXT_SW_CTRL_1, 0x00);
			wcd_usbss_write(priv, WCD_USBSS_EXT_LIN_EN, 0xC4);
			wcd_usbss_write(priv, WCD_USBSS_SW_LIN_CTRL_1, 0x01);
			wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_2, 0x3C);
			wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_3, 0x0F);
			if (priv->version == WCD_USBSS_2_0)
				wcd_usbss_update_bits(priv,
						WCD_USBSS_PMP_OUT1, 0x40, 0x40);
			/* for AATC plug-in, change mode to FSM */
			priv->audio_fsm_mode = WCD_USBSS_AUDIO_FSM;
			/* Disable all switches */
			wcd_usbss_update_bits(priv,
				WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x7F, 0x00);
			if (priv->audio_fsm_mode == WCD_USBSS_AUDIO_FSM) {
				wcd_usbss_update_bits_base(priv,
					WCD_USBSS_AUDIO_FSM_START, 0x01, 0x01, NULL, false, true);
			}
			/* Select L, R, GSBU2, MG1 */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT0, 0x3F, 0x02);
			/* Disable OVP_MG2_BIAS PCOMP_DYN_BST_EN */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_MG2_BIAS, 0x08, 0x00);
			/* Enable SENSE, MIC switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x06, 0x06);
			/* Select MG2 for AGND_SWITCHES */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT1, 0x01, 0x01);
			/* Enable AGND switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x01, 0x01);
			/* Enable DPR, DNL */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x18, 0x18);
			/* Set DELAY_L_SW to CYL_1K */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_DELAY_L_SW, 0xFF, 0x02);
			/* Set DELAY_R_SW to CYL_1K */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_DELAY_R_SW, 0xFF, 0x02);
			/* Set DELAY_MIC_SW to CYL_1K */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_DELAY_MIC_SW, 0xFF, 0x01);
			if (priv->audio_fsm_mode == WCD_USBSS_AUDIO_FSM) {
				wcd_usbss_update_bits_base(priv,
					WCD_USBSS_AUDIO_FSM_START, 0x01, 0x01, NULL, false, true);
			}
			for (i = 0; i < ARRAY_SIZE(coeff_init); ++i)
				wcd_usbss_update_bits(priv, coeff_init[i].reg,
						coeff_init[i].mask, coeff_init[i].val);
			wcd_usbss_update_bits(priv,
					WCD_USBSS_USB_SS_CNTL, 0x08, 0x00);
			wcd_usbss_update_bits(priv,
					WCD_USBSS_USB_SS_CNTL, 0x08, 0x08);
			usleep_range(10000, 10100);
			break;
		case WCD_USBSS_GND_MIC_SWAP_AATC:
			dev_info(priv->dev,
					"%s: GND MIC Swap register updates..\n", __func__);
			/* Update power mode to mode 1 for AATC */
			wcd_usbss_update_bits(priv,
				WCD_USBSS_USB_SS_CNTL, 0x07, 0x01);
			wcd_usbss_write(priv, WCD_USBSS_PMP_EN, 0xF);
			if (priv->version == WCD_USBSS_2_0)
				wcd_usbss_update_bits(priv,
						WCD_USBSS_PMP_OUT1, 0x40, 0x40);

			/* Disable MIC switch */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x02, 0x00);
			/* Disable OVP_MG1_BIAS PCOMP_DYN_BST_EN */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_MG1_BIAS, 0x08, 0x00);
			/* Select MG1 for AGND_SWITCHES */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT1, 0x01, 0x00);
			/* Select L, R, GSBU1, MG2 */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT0, 0x3F, 0x01);
			/* Enable MIC switch */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x02, 0x02);
			break;
		case WCD_USBSS_HSJ_CONNECT:
			/* Update power mode to mode 1 for AATC */
			wcd_usbss_update_bits(priv,
				WCD_USBSS_USB_SS_CNTL, 0x07, 0x01);
			wcd_usbss_write(priv, WCD_USBSS_PMP_EN, 0xF);
			wcd_usbss_write(priv, WCD_USBSS_EXT_SW_CTRL_1, 0x00);
			wcd_usbss_write(priv, WCD_USBSS_EXT_LIN_EN, 0xC4);
			wcd_usbss_write(priv, WCD_USBSS_SW_LIN_CTRL_1, 0x01);
			wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_2, 0x3C);
			wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_3, 0x0F);
			if (priv->version == WCD_USBSS_2_0)
				wcd_usbss_update_bits(priv,
						WCD_USBSS_PMP_OUT1, 0x40, 0x40);
			/* Select MG2, GSBU1 */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT0, 0x03, 0x1);
			/* Select  AGND MG2 */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT1, 0x01, 0x0);
			/* Disable OVP_MG1_BIAS PCOMP_DYN_BST_EN */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_MG1_BIAS, 0x08, 0x00);
			/* Enable SENSE, MIC, AGND switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x07);
			break;
		case WCD_USBSS_GND_MIC_SWAP_HSJ:
			/* Disable SENSE, MIC, AGND switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x00);
			/* Select MG1, GSBU2 */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT0, 0x03, 0x2);
			/* Select  AGND MG2 */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT1, 0x01, 0x1);
			/* Enable SENSE, MIC, AGND switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x07, 0x07);
			break;
		case WCD_USBSS_CHARGER:
			/* Disable DN DP Switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x18, 0x00);
			/* Select DN2 DP2 */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SELECT0, 0x3C, 0x28);
			/* Enable DN DP Switches */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_SWITCH_SETTINGS_ENABLE, 0x18, 0x18);
			break;
		case WCD_USBSS_DP_AUX_CC1:
			fallthrough;
		case WCD_USBSS_DP_AUX_CC2:
			/* Update Leakage Canceller Coefficient for AUXP pins */
			wcd_usbss_update_bits(priv,
					WCD_USBSS_DISP_AUXP_CTL, 0x07, 0x01);
			wcd_usbss_update_bits(priv,
					WCD_USBSS_DISP_AUXP_THRESH, 0xE0, 0xE0);
			ret = wcd_usbss_display_port_switch_update(priv, ctype);
			if (ret) /* clear DP AUX bit if DP switch update fails */
				priv->cable_status &= ~BIT(ctype);
			break;
		default:
			break;
		}
		if ((priv->cable_status & (BIT(WCD_USBSS_AATC) |
						BIT(WCD_USBSS_GND_MIC_SWAP_AATC) |
						BIT(WCD_USBSS_HSJ_CONNECT) |
						BIT(WCD_USBSS_GND_MIC_SWAP_HSJ)))) {
			priv->wcd_standby_status = WCD_USBSS_AUDIO_MODE_SET;
			dev_dbg(priv->dev, "wcd state transition to %s complete\n",
					status_to_str(priv->wcd_standby_status));
		}
	}

	release_runtime_env(priv);

	return ret;
}

/*
 * wcd_usbss_reg_notifier - register notifier block with wcd usbss driver
 *
 * @nb - notifier block of wcd_usbss
 * @node - phandle node to wcd_usbss device
 *
 * Returns 0 on success, or error code
 */
static int wcd_usbss_reg_notifier_ctx(struct wcd_usbss_ctxt *priv, struct notifier_block *nb,
			 struct device_node *node)
{
	int rc = 0;
	if (!nb || !node || priv->dev->of_node != node)
		return -ENODEV;

	rc = blocking_notifier_chain_register
				(&priv->wcd_usbss_notifier, nb);

	dev_dbg(priv->dev, "%s: registered notifier for %s\n",
		__func__, node->name);
	if (rc)
		return rc;

	/*
	 * as part of the init sequence check if there is a connected
	 * USB C analog adapter
	 */
	if (atomic_read(&(priv->usbc_mode)) == TYPEC_ACCESSORY_AUDIO) {
		dev_dbg(priv->dev, "%s: analog adapter already inserted\n",
			__func__);
		rc = wcd_usbss_usbc_analog_setup_switches(priv);
	}

	return rc;
}

/*
 * wcd_usbss_unreg_notifier - unregister notifier block with wcd usbss driver
 *
 * @nb - notifier block of wcd_usbss
 * @node - phandle node to wcd_usbss device
 *
 * Returns 0 on pass, or error code
 */
static int wcd_usbss_unreg_notifier_ctx(struct wcd_usbss_ctxt *priv, struct notifier_block *nb,
			     struct device_node *node)
{
	if (!nb || !node || priv->dev->of_node != node)
		return -ENODEV;

	return blocking_notifier_chain_unregister
					(&priv->wcd_usbss_notifier, nb);
}


/*
 * wcd_usbss_update_default_trim - update default trim for TP < 3
 *
 * Returns 0 on pass, or error code
 */
static int wcd_usbss_update_default_trim_ctx(struct wcd_usbss_ctxt *priv)
{
	int ret = 0;
	if (!priv)
		return -ENODEV;

	if (!priv->regmap)
		return -EINVAL;

	ret = acquire_runtime_env(priv);
	if (ret < 0) {
		dev_err(priv->dev, "%s: acquire_runtime_env failed: %i\n",
				__func__, ret);
		return ret;
	}
	wcd_usbss_write(priv, WCD_USBSS_SW_LIN_CTRL_1, 0x01);
	wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_1, 0x00);
	wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_2, 0x00);
	wcd_usbss_write(priv, WCD_USBSS_DC_TRIMCODE_3, 0x00);

	release_runtime_env(priv);

	return ret;
}

static void wcd_usbss_usbc_analog_work_fn(struct work_struct *work)
{
	struct wcd_usbss_ctxt *priv =
		container_of(work, struct wcd_usbss_ctxt, usbc_analog_work);

	if (!priv) {
		pr_err("%s: wcd usbss container invalid\n", __func__);
		return;
	}
	if (!READ_ONCE(priv->removing))
		wcd_usbss_usbc_analog_setup_switches(priv);
	pm_relax(priv->dev);
}

static int wcd_usbss_init_optional_reset_pins(struct wcd_usbss_ctxt *priv)
{
	priv->rst_pins = devm_pinctrl_get(priv->dev);
	if (IS_ERR_OR_NULL(priv->rst_pins)) {
		dev_dbg(priv->dev, "Cannot get wcd usbss reset pinctrl:%ld\n",
				PTR_ERR(priv->rst_pins));
		return PTR_ERR(priv->rst_pins);
	}

	priv->rst_pins_active = pinctrl_lookup_state(
			priv->rst_pins, "active");
	if (IS_ERR_OR_NULL(priv->rst_pins_active)) {
		dev_dbg(priv->dev, "Cannot get active pinctrl state:%ld\n",
				PTR_ERR(priv->rst_pins_active));
		return PTR_ERR(priv->rst_pins_active);
	}

	if (priv->rst_pins_active)
		return pinctrl_select_state(priv->rst_pins,
				priv->rst_pins_active);

	return 0;
}

/* called from switch_update_lock mutex locked */
static int wcd_usbss_sdam_handle_events_locked(struct wcd_usbss_ctxt *priv, int req_state)
{
	int rc = 0;

	switch (req_state) {
	case WCD_USBSS_LPD_USB_MODE_CLEAR:
		wcd_usbss_update_bits(priv, WCD_USBSS_PMP_OUT1, 0x20, 0x00);
		/* Enable D+/D- 1M & 400K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_BIAS_TOP, 0x20, 0x00);

		/* Enable DP/DN 2K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_DP_BIAS, 0x01, 0x01);
		wcd_usbss_update_bits(priv, WCD_USBSS_DN_BIAS, 0x01, 0x01);

		if (!priv->usb_sbu_compliance) {
			/* Enable SBU1/2 2K PLDN */
			wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS, 0x01, 0x01);
			wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS, 0x01, 0x01);
		} else {
			/* Disable SBU1/2 2K PLDN */
			wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS,
					   0x01, 0x00);
			wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS,
					   0x01, 0x00);
		}
		/* Disconnect D+/D- switch */
		wcd_usbss_dpdm_switch_update_from_handler(priv, false, false);

		/* Enter standby */
		wcd_usbss_standby_control_locked(priv, true);
		break;
	case WCD_USBSS_LPD_CONTINUOUS_1:
		fallthrough;
	case WCD_USBSS_LPD_CONTINUOUS_2:
		wcd_usbss_update_bits(priv, WCD_USBSS_PMP_OUT1, 0x20, 0x20);
		/* Disable SBU1/2 2K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS, 0x01, 0x00);
		wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS, 0x01, 0x00);
		/* USB Mode : Connect D+/D- switch */
		wcd_usbss_dpdm_switch_connect(priv, true);

		/* Exit from standby */
		wcd_usbss_standby_control_locked(priv, false);
		break;
	case WCD_USBSS_LPD_MODE_SET:
		fallthrough;
	case WCD_USBSS_LPD_USB_MODE_SET:
		wcd_usbss_update_bits(priv, WCD_USBSS_PMP_OUT1, 0x20, 0x20);
		/* Disable DP/DN 2K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_DP_BIAS, 0x01, 0x00);
		wcd_usbss_update_bits(priv, WCD_USBSS_DN_BIAS, 0x01, 0x00);
		/* Disable D+/D- 1M & 400K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_BIAS_TOP, 0x20, 0x20);

		if (!priv->usb_sbu_compliance) {
			/* Disable SBU1/2 2K PLDN */
			wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS, 0x01, 0x00);
			wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS, 0x01, 0x00);
		} else {
			/* Disable SBU1/2 2K PLDN */
			wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS,
					   0x01, 0x01);
			wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS,
					   0x01, 0x01);
		}
		/* USB Mode : Connect D+/D- switch */
		wcd_usbss_dpdm_switch_connect(priv, true);

		/* Exit from standby */
		wcd_usbss_standby_control_locked(priv, false);
		break;
	case WCD_USBSS_USB_MODE_SET:
		wcd_usbss_update_bits(priv, WCD_USBSS_PMP_OUT1, 0x20, 0x00);
		/* Enable D+/D- 1M & 400K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_BIAS_TOP, 0x20, 0x00);
		/* Enable DP/DN 2K PLDN */
		wcd_usbss_update_bits(priv, WCD_USBSS_DP_BIAS, 0x01, 0x01);
		wcd_usbss_update_bits(priv, WCD_USBSS_DN_BIAS, 0x01, 0x01);

		if (!priv->usb_sbu_compliance) {
			/* Enable SBU1/2 2K PLDN */
			wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS, 0x01, 0x01);
			wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS, 0x01, 0x01);
		} else {
			/* Disable SBU1/2 2K PLDN */
			wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS,
					   0x01, 0x00);
			wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS,
					   0x01, 0x00);
		}

		/* Connect D+/D- switch */
		wcd_usbss_dpdm_switch_connect(priv, true);

		/* Exit from standby */
		wcd_usbss_standby_control_locked(priv, false);
		break;
	default:
		dev_err(priv->dev, "unexpected state:%d\n", req_state);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static irqreturn_t wcd_usbss_sdam_notifier_handler(int irq, void *data)
{
	struct wcd_usbss_ctxt *priv = data;
	struct wcd_usbss_saved_state saved;
	u8 *buf;
	size_t len;
	int ret, requested;

	if (READ_ONCE(priv->removing))
		return IRQ_HANDLED;
	buf = nvmem_cell_read(priv->nvmem_cell, &len);
	if (IS_ERR(buf)) {
		dev_err_ratelimited(priv->dev, "SDAM read failed: %ld\n", PTR_ERR(buf));
		return IRQ_HANDLED;
	}
	if (!len) {
		kfree(buf);
		return IRQ_HANDLED;
	}
	requested = buf[0] & 0x7;
	kfree(buf);
	mutex_lock(&priv->switch_update_lock);
	if (priv->suspended) {
		priv->defer_writes = true;
		priv->req_state = requested;
		mutex_unlock(&priv->switch_update_lock);
		return IRQ_HANDLED;
	}
	if (READ_ONCE(priv->removing)) {
		mutex_unlock(&priv->switch_update_lock);
		return IRQ_HANDLED;
	}
	priv->io_error = 0;
	saved.cable_status = priv->cable_status;
	saved.audio_fsm_mode = priv->audio_fsm_mode;
	saved.standby_status = priv->wcd_standby_status;
	saved.in_standby = priv->is_in_standby;
	saved.standby_enabled = priv->standby_enable;
	ret = acquire_runtime_env(priv);
	if (!ret) {
		ret = wcd_usbss_sdam_handle_events_locked(priv, requested);
		if (!ret && !priv->io_error)
			priv->wcd_standby_status = requested;
		release_runtime_env(priv);
	}
	ret = wcd_usbss_end(priv, &saved, ret);
	if (ret)
		dev_err_ratelimited(priv->dev, "SDAM transition failed: %d\n", ret);
	return IRQ_HANDLED;
}

static int wcd_usbss_sdam_registration(struct wcd_usbss_ctxt *priv)
{
	int ret;

	if (!priv->client->irq)
		return -ENODEV;
	priv->nvmem_cell = devm_nvmem_cell_get(priv->dev, "usb_mode");
	if (IS_ERR(priv->nvmem_cell))
		return PTR_ERR(priv->nvmem_cell);
	ret = devm_request_threaded_irq(priv->dev, priv->client->irq, NULL,
					wcd_usbss_sdam_notifier_handler, IRQF_ONESHOT,
					"wcd-usbss-sdam", priv);
	if (ret)
		return ret;
	priv->irq_registered = true;
	ret = enable_irq_wake(priv->client->irq);
	if (!ret)
		priv->irq_wake_enabled = true;
	else
		dev_warn(priv->dev, "Could not enable SDAM wake IRQ: %d\n", ret);
	return 0;
}

enum wcd_usbss_sbu_switch_orientation wcd_usbss_get_sbu_switch_orientation(void)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_get_sbu_switch_orientation_ctx(priv);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret < 0 ? INVALID_ORIENTATION : ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_get_sbu_switch_orientation);

int wcd_usbss_set_switch_settings_enable(enum wcd_usbss_switch_type switch_type,
					 enum wcd_usbss_switch_state switch_state)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_set_switch_settings_enable_ctx(priv, switch_type, switch_state);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_set_switch_settings_enable);

int wcd_usbss_linearizer_rdac_cal_code_select(enum linearizer_rdac_cal_code_select source)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_linearizer_rdac_cal_code_select_ctx(priv, source);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_linearizer_rdac_cal_code_select);

int wcd_usbss_set_linearizer_sw_tap(uint32_t aud_tap, uint32_t gnd_tap)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_set_linearizer_sw_tap_ctx(priv, aud_tap, gnd_tap);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_set_linearizer_sw_tap);

int wcd_usbss_register_update(uint32_t reg_arr[][2], bool write, size_t arr_size)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_register_update_ctx(priv, reg_arr, write, arr_size);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_register_update);

int wcd_usbss_dpdm_switch_update(bool sw_en, bool eq_en)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_dpdm_switch_update_ctx(priv, sw_en, eq_en);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_dpdm_switch_update);

int wcd_usbss_audio_config(bool enable, enum wcd_usbss_config_type config_type,
			unsigned int power_mode)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_audio_config_ctx(priv, enable, config_type, power_mode);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_audio_config);

int wcd_usbss_switch_update(enum wcd_usbss_cable_types ctype,
							enum wcd_usbss_cable_status connect_status)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_switch_update_ctx(priv, ctype, connect_status);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_switch_update);

int wcd_usbss_reg_notifier(struct notifier_block *nb,
			 struct device_node *node)
{
	struct wcd_usbss_ctxt *priv;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_reg_notifier_ctx(priv, nb, node);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_reg_notifier);

int wcd_usbss_unreg_notifier(struct notifier_block *nb,
			     struct device_node *node)
{
	struct wcd_usbss_ctxt *priv;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_unreg_notifier_ctx(priv, nb, node);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_unreg_notifier);

int wcd_usbss_update_default_trim(void)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	int ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_update_default_trim_ctx(priv);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}
EXPORT_SYMBOL_GPL(wcd_usbss_update_default_trim);

static ssize_t wcd_usbss_surge_enable_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	ssize_t ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_surge_enable_store_ctx(priv, kobj, attr, buf, count);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}

static ssize_t wcd_usbss_surge_period_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	ssize_t ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_surge_period_store_ctx(priv, kobj, attr, buf, count);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}

static ssize_t wcd_usbss_standby_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct wcd_usbss_ctxt *priv;
	struct wcd_usbss_saved_state saved;
	int index;
	ssize_t ret;

	priv = wcd_usbss_get(&index);
	if (!priv) {
		ret = -ENODEV;
		goto out;
	}
	ret = wcd_usbss_begin(priv, &saved);
	if (ret)
		goto out;
	ret = wcd_usbss_standby_store_ctx(priv, kobj, attr, buf, count);
	ret = wcd_usbss_end(priv, &saved, ret);
out:
	srcu_read_unlock(&wcd_usbss_srcu, index);
	return ret;
}

static void wcd_usbss_release_claim(void *data)
{
	mutex_lock(&wcd_usbss_provider_lock);
	wcd_usbss_claimed = false;
	mutex_unlock(&wcd_usbss_provider_lock);
}

static void wcd_usbss_cleanup(struct wcd_usbss_ctxt *priv)
{
	int ret;

	if (priv->stopped)
		goto disable_supplies;
	priv->stopped = true;
	WRITE_ONCE(priv->removing, true);
	mutex_lock(&wcd_usbss_provider_lock);
	if (rcu_access_pointer(wcd_usbss_provider) == priv)
		RCU_INIT_POINTER(wcd_usbss_provider, NULL);
	mutex_unlock(&wcd_usbss_provider_lock);
	synchronize_srcu(&wcd_usbss_srcu);
	if (priv->surge_kobject) {
		sysfs_remove_group(priv->surge_kobject, &wcd_usbss_attribute_group);
		kobject_put(priv->surge_kobject);
		priv->surge_kobject = NULL;
	}
	if (priv->ucsi_registered) {
		unregister_ucsi_glink_notifier(&priv->ucsi_nb);
		priv->ucsi_registered = false;
	}
	if (priv->irq_registered) {
		if (priv->irq_wake_enabled)
			disable_irq_wake(priv->client->irq);
		devm_free_irq(priv->dev, priv->client->irq, priv);
		priv->irq_registered = false;
	}
	wcd_usbss_disable_surge_kthread(priv);
	cancel_work_sync(&priv->usbc_analog_work);
	if (priv->debugfs_registered) {
		devm_regmap_qti_debugfs_unregister(priv->regmap);
		priv->debugfs_registered = false;
	}
	pm_runtime_barrier(priv->dev);
	pm_runtime_dont_use_autosuspend(priv->dev);
	pm_runtime_disable(priv->dev);
	pm_relax(priv->dev);
	device_init_wakeup(priv->dev, false);
disable_supplies:
	if (priv->supplies_enabled) {
		ret = regulator_bulk_disable(ARRAY_SIZE(supply_names), priv->supplies);
		if (ret)
			dev_err(priv->dev, "Could not disable supplies: %d\n", ret);
		else
			priv->supplies_enabled = false;
	}
}

static int wcd_usbss_probe(struct i2c_client *i2c)
{
	struct wcd_usbss_ctxt *priv;
	struct device *dev = &i2c->dev;
	int rc, i;
	unsigned int ver = 0, efuse_13, efuse_14;
	u32 platform_id, hw_country_ver;
	bool runtime_acquired = false;

	mutex_lock(&wcd_usbss_provider_lock);
	if (wcd_usbss_claimed) {
		mutex_unlock(&wcd_usbss_provider_lock);
		return -EBUSY;
	}
	wcd_usbss_claimed = true;
	mutex_unlock(&wcd_usbss_provider_lock);
	rc = devm_add_action_or_reset(dev, wcd_usbss_release_claim, NULL);
	if (rc)
		return rc;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->client = i2c;
	priv->audio_fsm_mode = WCD_USBSS_AUDIO_MANUAL;
	priv->surge_timer_period_ms = DEFAULT_SURGE_TIMER_PERIOD_MS;
	priv->surge_enable = true;
	priv->wcd_standby_status = WCD_USBSS_USB_MODE_SET;
	mutex_init(&priv->io_lock);
	mutex_init(&priv->switch_update_lock);
	mutex_init(&priv->runtime_env_counter_lock);
	mutex_init(&priv->notification_lock);
	INIT_WORK(&priv->usbc_analog_work, wcd_usbss_usbc_analog_work_fn);
	BLOCKING_INIT_NOTIFIER_HEAD(&priv->wcd_usbss_notifier);
	i2c_set_clientdata(i2c, priv);

	rc = device_init_wakeup(dev, true);
	if (rc)
		return rc;
	pm_runtime_set_autosuspend_delay(dev, 600);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	rc = acquire_runtime_env(priv);
	if (rc)
		goto error;
	runtime_acquired = true;
	for (i = 0; i < ARRAY_SIZE(supply_names); i++)
		priv->supplies[i].supply = supply_names[i];
	rc = devm_regulator_bulk_get(dev, ARRAY_SIZE(supply_names), priv->supplies);
	if (rc)
		goto error;
	rc = regulator_bulk_enable(ARRAY_SIZE(supply_names), priv->supplies);
	if (rc)
		goto error;
	priv->supplies_enabled = true;
	rc = wcd_usbss_init_optional_reset_pins(priv);
	if (rc == -EPROBE_DEFER)
		goto error;
	if (rc && of_find_property(dev->of_node, "pinctrl-names", NULL))
		goto error;
	priv->regmap = wcd_usbss_regmap_init(dev, &wcd_usbss_regmap_config);
	if (IS_ERR(priv->regmap)) {
		rc = PTR_ERR(priv->regmap);
		priv->regmap = NULL;
		goto error;
	}
	if (of_find_property(i2c->dev.of_node, "wcd-usb-sbu-compliance", NULL)) {
		platform_id = get_hw_version_platform();
		hw_country_ver = get_hw_country_version();
		dev_warn(priv->dev, " platform_id:%u, hw_country_ver:%u \n",
			 platform_id, hw_country_ver);
		if ((platform_id == HARDWARE_PROJECT_O3 ||
		     platform_id == HARDWARE_PROJECT_O1) &&
		    (((uint32_t)CountryGlobal == hw_country_ver) ||
		     ((uint32_t)CountryIndia == hw_country_ver))) {
			dev_info(
				priv->dev,
				"disabling SBU pulldowns for USB compliance\n");
			priv->usb_sbu_compliance = true;
			/*
			 * external zener diode installed,
			 * disable OVP protections on SBUx. SBUx pull-downs are disabled.
			 * This is needed for USB compliance requirement (related to
			 * impedance) on SBUx
			 */
			wcd_usbss_update_bits(priv, WCD_USBSS_MG1_BIAS,
					   0x01, 0x00);
			wcd_usbss_update_bits(priv, WCD_USBSS_MG2_BIAS,
					   0x01, 0x00);
		}
	}

	/* OVP-Fuse settings recommended from HW */
	wcd_usbss_update_bits(priv, WCD_USBSS_FSM_OVERRIDE, 0x77, 0x77);
	wcd_usbss_update_bits(priv, WCD_USBSS_DP_EN, 0x0E, 0x08);
	wcd_usbss_update_bits(priv, WCD_USBSS_DN_EN, 0x0E, 0x08);

	/* Display common mode and OVP 4V updates */
	wcd_usbss_update_bits(priv, WCD_USBSS_DISP_AUXP_CTL, 0x07, 0x01);
	wcd_usbss_update_bits(priv, WCD_USBSS_DISP_AUXP_THRESH, 0xE0, 0xE0);
	wcd_usbss_update_bits(priv, WCD_USBSS_DISP_AUXM_THRESH, 0xE0, 0xE0);
	wcd_usbss_update_bits(priv, WCD_USBSS_MG1_EN, 0x0C, 0x0C);
	wcd_usbss_update_bits(priv, WCD_USBSS_MG2_EN, 0x0C, 0x0C);
	/* MG comparator bias current to 1uA */
	wcd_usbss_write(priv, WCD_USBSS_MG1_BIAS, MG_BIAS_CURRENT);
	wcd_usbss_write(priv, WCD_USBSS_MG2_BIAS, MG_BIAS_CURRENT);

	/*
	 * READ WCD_USBSS_EFUSE_REG_13<4:0> write to WCD_USBSS_DC_TRIMCODE_1 <4:0>
	 * READ WCD_USBSS_EFUSE_REG_14<4:0> write its <1:0>
	 * to WCD_USBSS_DC_TRIMCODE_2 <1:0> and its <4:2> to
	 * WCD_USBSS_DC_TRIMCODE_1 <7:5>
	 * To Avoid AUX issues.
	 */

	rc = wcd_usbss_read(priv, WCD_USBSS_EFUSE_REG_13, &efuse_13);

	if (rc == 0) {
		rc = wcd_usbss_read(priv, WCD_USBSS_EFUSE_REG_14, &efuse_14);
		if (rc == 0) {
			wcd_usbss_update_bits(priv,
				WCD_USBSS_DC_TRIMCODE_1, WCD_USBSS_TRIMCODE1_MASK_1,
					efuse_13 & WCD_USBSS_TRIMCODE1_MASK_1);
			wcd_usbss_update_bits(priv,
				WCD_USBSS_DC_TRIMCODE_2, WCD_USBSS_TRIMCODE2_MASK_1,
					efuse_14 & WCD_USBSS_TRIMCODE2_MASK_1);
			wcd_usbss_update_bits(priv,
				WCD_USBSS_DC_TRIMCODE_1, WCD_USBSS_TRIMCODE1_MASK_2,
					((efuse_14 & 0x1C) << BIT_MASK_4));
			wcd_usbss_update_bits(priv,
				WCD_USBSS_DC_TRIMCODE_2, WCD_USBSS_TRIMCODE2_MASK_2,
					BIT_MASK_F << BIT_MASK_2);
			wcd_usbss_update_bits(priv,
				WCD_USBSS_DC_TRIMCODE_3, WCD_USBSS_TRIMCODE1_MASK_1,
					BIT_MASK_F);
		}
	}

	wcd_usbss_read(priv, WCD_USBSS_CHIP_ID1, &ver);
	if (ver == 0x1) { /* Harmonium 2.0 */
		wcd_usbss_update_bits(priv, WCD_USBSS_MG1_EN, 0x2, 0x0);
		wcd_usbss_update_bits(priv, WCD_USBSS_MG2_EN, 0x2, 0x0);
	}
	priv->version = ver;
	priv->version_valid = true;

	/* SET LINIERIZER TAP registers to default to gaurantee */
	/* good THD+N and intermodulation performance. */

	wcd_usbss_write(priv, WCD_USBSS_EXT_SW_CTRL_1, 0x0);
	wcd_usbss_write(priv, WCD_USBSS_SW_TAP_GND_L_LSB, 0x0);
	wcd_usbss_write(priv, WCD_USBSS_SW_TAP_GND_R_LSB, 0x0);
	wcd_usbss_write(priv, WCD_USBSS_FUNCTION_ENABLE, 0x05);

	wcd_usbss_update_reg_init(priv, priv->regmap);
	if (priv->io_error) {
		rc = priv->io_error;
		goto error;
	}
	rc = devm_regmap_qti_debugfs_register(dev, priv->regmap);
	if (rc)
		goto error;
	priv->debugfs_registered = true;
	rc = wcd_usbss_sdam_registration(priv);
	if (rc == -EPROBE_DEFER)
		goto error;
	if (!rc)
		priv->standby_enable = true;
	else
		dev_info(dev, "SDAM standby unavailable: %d\n", rc);

	priv->ucsi_nb.notifier_call = wcd_usbss_usbc_event_changed;
	rc = register_ucsi_glink_notifier(&priv->ucsi_nb);
	if (rc)
		goto error;
	priv->ucsi_registered = true;
	rc = wcd_usbss_sysfs_init(priv);
	if (rc)
		goto error;
	rc = wcd_usbss_enable_surge_kthread(priv);
	if (rc)
		goto error;
	release_runtime_env(priv);
	rcu_assign_pointer(wcd_usbss_provider, priv);
	dev_info(dev, "Probe completed\n");
	return 0;
error:
	if (runtime_acquired)
		release_runtime_env(priv);
	wcd_usbss_cleanup(priv);
	return rc;
}

static void wcd_usbss_remove(struct i2c_client *i2c)
{
	struct wcd_usbss_ctxt *priv = i2c_get_clientdata(i2c);

	wcd_usbss_cleanup(priv);
}

static void wcd_usbss_shutdown(struct i2c_client *i2c)
{
	struct wcd_usbss_ctxt *priv = i2c_get_clientdata(i2c);

	wcd_usbss_cleanup(priv);
}

#ifdef CONFIG_PM_SLEEP
static int wcd_usbss_pm_suspend(struct device *dev)
{
	struct wcd_usbss_ctxt *priv = dev_get_drvdata(dev);

	if (!priv)
		return 0;

	mutex_lock(&priv->switch_update_lock);
	priv->suspended = true;
	mutex_unlock(&priv->switch_update_lock);

	dev_dbg(priv->dev, "wcd usbss pm suspended");
	return 0;
}

static int wcd_usbss_pm_resume(struct device *dev)
{
	struct wcd_usbss_ctxt *priv = dev_get_drvdata(dev);
	struct wcd_usbss_saved_state saved;
	int ret = 0;

	mutex_lock(&priv->switch_update_lock);
	priv->io_error = 0;
	saved.cable_status = priv->cable_status;
	saved.audio_fsm_mode = priv->audio_fsm_mode;
	saved.standby_status = priv->wcd_standby_status;
	saved.in_standby = priv->is_in_standby;
	saved.standby_enabled = priv->standby_enable;
	if (priv->needs_sync && !priv->removing) {
		ret = wcd_usbss_cache_sync(priv);
		if (!ret)
			priv->needs_sync = false;
	}
	if (!ret && priv->defer_writes && !priv->removing) {
		ret = wcd_usbss_sdam_handle_events_locked(priv, priv->req_state);
		if (!ret && !priv->io_error) {
			priv->wcd_standby_status = priv->req_state;
			priv->defer_writes = false;
		}
	}
	priv->suspended = false;
	return wcd_usbss_end(priv, &saved, ret);
}
#endif

static const struct of_device_id wcd_usbss_i2c_dt_match[] = {
	{
		.compatible = "qcom,wcd939x-i2c",
	},
	{}
};
MODULE_DEVICE_TABLE(of, wcd_usbss_i2c_dt_match);

static const struct i2c_device_id wcd_usbss_id_i2c[] = {
	{ "wcd939x", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, wcd_usbss_id_i2c);

static const struct dev_pm_ops wcd_usbss_pm_ops = {
	.suspend = wcd_usbss_pm_suspend,
	.resume = wcd_usbss_pm_resume,
};

static struct i2c_driver wcd_usbss_i2c_driver = {
	.driver = {
		.name = WCD_USBSS_I2C_NAME,
		.of_match_table = wcd_usbss_i2c_dt_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
#ifdef CONFIG_PM_SLEEP
		.pm = &wcd_usbss_pm_ops,
#endif
	},
	.id_table = wcd_usbss_id_i2c,
	.probe = wcd_usbss_probe,
	.remove = wcd_usbss_remove,
	.shutdown = wcd_usbss_shutdown,
};
module_i2c_driver(wcd_usbss_i2c_driver);

MODULE_DESCRIPTION("WCD USBSS I2C driver");
MODULE_LICENSE("GPL");
