/******************************************************************************
 * Copyright (C) 2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2022 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ******************************************************************************/
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 *****************************************************************************/
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/pinctrl/qcom-pinctrl.h>
#include "common.h"
static bool secure_peripheral_not_found __maybe_unused = true;
DEFINE_SRCU(nfc_io_srcu);
static DEFINE_MUTEX(nfc_registry_lock);
static struct nfc_dev *nfc_active;

static void nfc_dev_release_ref(struct kref *ref)
{
	struct nfc_dev *dev = container_of(ref, struct nfc_dev, ref);

	kfree(dev->read_kbuf);
	kfree(dev->write_kbuf);
	kfree(dev);
}

void nfc_dev_put(struct nfc_dev *dev)
{
	kref_put(&dev->ref, nfc_dev_release_ref);
}

int nfc_dev_publish(struct nfc_dev *dev)
{
	int ret = 0;

	mutex_lock(&nfc_registry_lock);
	if (nfc_active)
		ret = -EBUSY;
	else {
		nfc_active = dev;
		dev->published = true;
	}
	mutex_unlock(&nfc_registry_lock);
	return ret;
}

void nfc_dev_unpublish(struct nfc_dev *dev)
{
	mutex_lock(&nfc_registry_lock);
	if (nfc_active == dev)
		nfc_active = NULL;
	WRITE_ONCE(dev->removing, true);
	mutex_unlock(&nfc_registry_lock);
	WRITE_ONCE(dev->release_read, true);
	wake_up_interruptible(&dev->read_wq);
	wake_up_interruptible(&dev->cold_reset.read_wq);
	/* Failed probes never had readers; do not wait for another device's I/O. */
	if (dev->published)
		synchronize_srcu(&nfc_io_srcu);
}


int nfc_parse_dt(struct device *dev, struct platform_configs *nfc_configs,
		 uint8_t interface)
{
	int ret;
	struct device_node *np = dev->of_node;
	struct platform_gpio *nfc_gpio = &nfc_configs->gpio;
	struct platform_ldo *ldo = &nfc_configs->ldo;

	if (!np) {
		pr_err("NxpDrv: %s: nfc of_node NULL\n", __func__);
		return -EINVAL;
	}

	nfc_gpio->irq = -EINVAL;
	nfc_gpio->dwl_req = -EINVAL;
	nfc_gpio->ven = -EINVAL;
	nfc_gpio->clkreq = -EINVAL;

	/* irq required for i2c based chips only */
	if (interface == PLATFORM_IF_I2C) {
		nfc_gpio->irq = of_get_named_gpio(np, DTS_IRQ_GPIO_STR, 0);
		if ((!gpio_is_valid(nfc_gpio->irq))) {
			pr_err("NxpDrv: %s: irq gpio invalid %d\n", __func__,
			       nfc_gpio->irq);
			return nfc_gpio->irq;
		}
		pr_info("NxpDrv: %s: irq %d\n", __func__, nfc_gpio->irq);
	}
	nfc_gpio->ven = of_get_named_gpio(np, DTS_VEN_GPIO_STR, 0);
	if ((!gpio_is_valid(nfc_gpio->ven))) {
		pr_err("NxpDrv: %s: ven gpio invalid %d\n", __func__, nfc_gpio->ven);
		return nfc_gpio->ven;
	}
	/* some products like sn220 does not required fw dwl pin */
	nfc_gpio->dwl_req = of_get_named_gpio(np, DTS_FWDN_GPIO_STR, 0);
	if ((int)nfc_gpio->dwl_req == -EPROBE_DEFER)
		return -EPROBE_DEFER;
        /* not returning failure for dwl gpio as it is optional for sn220 */
	if ((!gpio_is_valid(nfc_gpio->dwl_req))) {
		pr_warn("NxpDrv: %s: dwl_req gpio invalid %d\n", __func__,
			nfc_gpio->dwl_req);
        }
        /* Read clkreq GPIO pin number from DTSI */
	nfc_gpio->clkreq = of_get_named_gpio(np, DTS_CLKREQ_GPIO_STR, 0);
	if (!gpio_is_valid(nfc_gpio->clkreq)) {
		   dev_err(dev, "NxpDrv: clkreq gpio invalid %d\n", nfc_gpio->clkreq);
		return (int)nfc_gpio->clkreq < 0 ? (int)nfc_gpio->clkreq : -EINVAL;
        }

#ifdef NFC_SECURE_PERIPHERAL_ENABLED
	/* Read DTS_SZONE_STR to check secure zone support */
	if (of_property_read_string(np, DTS_SZONE_STR, &nfc_configs->szone)) {
		nfc_configs->CNSS_NFC_HW_SECURE_ENABLE = false;
	}else
		nfc_configs->CNSS_NFC_HW_SECURE_ENABLE = true;
#endif
	pr_info("NxpDrv: %s: irq %d, ven %d, dwl %d, clkreq %d \n", __func__, nfc_gpio->irq, nfc_gpio->ven,
		nfc_gpio->dwl_req, nfc_gpio->clkreq);

	/* optional property */
	ret = of_property_read_u32_array(np, NFC_LDO_VOL_DT_NAME,
			(u32 *) ldo->vdd_levels,
			ARRAY_SIZE(ldo->vdd_levels));
	if (ret) {
		dev_err(dev, "NxpDrv: error reading NFC VDDIO min and max value\n");
		// set default as per datasheet
		ldo->vdd_levels[0] = NFC_VDDIO_MIN;
		ldo->vdd_levels[1] = NFC_VDDIO_MAX;
	}

	/* optional property */
	ret = of_property_read_u32(np, NFC_LDO_CUR_DT_NAME, &ldo->max_current);
	if (ret) {
		dev_err(dev, "NxpDrv: error reading NFC current value\n");
		// set default as per datasheet
		ldo->max_current = NFC_CURRENT_MAX;
	}

	return 0;
}

void set_valid_gpio(int gpio, int value)
{
	if (gpio_is_valid(gpio)) {
		pr_debug("NxpDrv: %s: gpio %d value %d\n", __func__, gpio, value);
		gpio_set_value(gpio, value);
		/* hardware dependent delay */
		usleep_range(NFC_GPIO_SET_WAIT_TIME_US,
			     NFC_GPIO_SET_WAIT_TIME_US + 100);
	}
}

static int get_valid_gpio(int gpio)
{
	int value = -EINVAL;

	if (gpio_is_valid(gpio)) {
		value = gpio_get_value(gpio);
		pr_debug("NxpDrv: %s: gpio %d value %d\n", __func__, gpio, value);
	}
	return value;
}

void gpio_set_ven(struct nfc_dev *nfc_dev, int value)
{
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	if (gpio_get_value(nfc_gpio->ven) != value) {
		pr_debug("NxpDrv: %s: value %d\n", __func__, value);
#ifdef NFC_SECURE_PERIPHERAL_ENABLED
		if(secure_peripheral_not_found)
		{
			/*secure peripheral feature is not enabled*/
			gpio_set_value(nfc_gpio->ven, value);
		}
		else
		{
			/*secure peripheral feature is enabled*/
			if(!nfc_hw_secure_check())
				gpio_set_value(nfc_gpio->ven, value);
		}
#else
		gpio_set_value(nfc_gpio->ven, value);
#endif

		/* hardware dependent delay */
		usleep_range(NFC_GPIO_SET_WAIT_TIME_US,
				NFC_GPIO_SET_WAIT_TIME_US + 100);
	}
}

int configure_gpio(unsigned int gpio, int flag)
{
	int ret;

	pr_debug("NxpDrv: %s: nfc gpio [%d] flag [%01x]\n", __func__, gpio, flag);
	if (gpio_is_valid(gpio)) {
		ret = gpio_request(gpio, "nfc_gpio");
		if (ret) {
			pr_err("NxpDrv: %s: unable to request nfc gpio [%d]\n",
			       __func__, gpio);
			return ret;
		}
		/* set direction and value for output pin */
		if (flag & GPIO_OUTPUT) {
			ret = gpio_direction_output(gpio, (GPIO_HIGH & flag));
			pr_debug("NxpDrv: %s: nfc o/p gpio %d level %d\n", __func__,
				 gpio, gpio_get_value(gpio));
		} else {
			ret = gpio_direction_input(gpio);
			pr_debug("NxpDrv: %s: nfc i/p gpio %d\n", __func__, gpio);
		}

		if (ret) {
			pr_err("NxpDrv: %s: unable to set direction for nfc gpio [%d]\n", __func__, gpio);
			gpio_free(gpio);
			return ret;
		}
		/* Consider value as control for input IRQ pin */
		if (flag & GPIO_IRQ) {
			ret = gpio_to_irq(gpio);
			if (ret < 0) {
				pr_err("NxpDrv: %s: unable to set irq [%d]\n", __func__,
				       gpio);
				gpio_free(gpio);
				return ret;
			}
			pr_debug("NxpDrv: %s: gpio_to_irq successful [%d]\n", __func__,
				 gpio);
			return ret;
		}
	} else {
		pr_err("NxpDrv: %s: invalid gpio\n", __func__);
		ret = -EINVAL;
	}
	return ret;
}

void gpio_free_all(struct nfc_dev *dev)
{
	if (dev->clkreq_wake_enabled) {
		msm_gpio_mpm_wake_set(dev->clkreq_hwirq, false);
		dev->clkreq_wake_enabled = false;
	}
	if (dev->gpio_owned & BIT(2))
		gpio_free(dev->configs.gpio.dwl_req);
	if (dev->gpio_owned & BIT(1))
		gpio_free(dev->configs.gpio.ven);
	if (dev->gpio_owned & BIT(0))
		gpio_free(dev->configs.gpio.irq);
	dev->gpio_owned = 0;
}



void nfc_misc_unregister(struct nfc_dev *dev, int count)
{
	if (dev->nfc_device) {
		device_destroy(dev->nfc_class, dev->devno);
		dev->nfc_device = NULL;
	}
	if (dev->c_dev) {
		if (dev->cdev_added)
			cdev_del(dev->c_dev);
		else
			kobject_put(&dev->c_dev->kobj);
		dev->c_dev = NULL;
		dev->cdev_added = false;
	}
	if (dev->nfc_class) {
		class_destroy(dev->nfc_class);
		dev->nfc_class = NULL;
	}
	if (dev->region_registered) {
		unregister_chrdev_region(dev->devno, count);
		dev->region_registered = false;
	}
}

int nfc_misc_register(struct nfc_dev *dev,
		      const struct file_operations *fops, int count,
		      char *devname, const char *classname)
{
	int ret;

	ret = alloc_chrdev_region(&dev->devno, 0, count, devname);
	if (ret)
		return ret;
	dev->region_registered = true;
	dev->nfc_class = class_create(classname);
	if (IS_ERR(dev->nfc_class)) {
		ret = PTR_ERR(dev->nfc_class);
		dev->nfc_class = NULL;
		goto fail;
	}
	dev->c_dev = cdev_alloc();
	if (!dev->c_dev) {
		ret = -ENOMEM;
		goto fail;
	}
	dev->c_dev->owner = THIS_MODULE;
	dev->c_dev->ops = fops;
	ret = cdev_add(dev->c_dev, dev->devno, count);
	if (ret)
		goto fail;
	dev->cdev_added = true;
	dev->nfc_device = device_create(dev->nfc_class, &dev->i2c_dev.client->dev,
					dev->devno, dev, devname);
	if (IS_ERR(dev->nfc_device)) {
		ret = PTR_ERR(dev->nfc_device);
		dev->nfc_device = NULL;
		goto fail;
	}
	return 0;
fail:
	nfc_misc_unregister(dev, count);
	return ret;
}

/**
 * nfc_gpio_info() - gets the status of nfc gpio pins and encodes into a byte.
 * @nfc_dev:	nfc device data structure
 * @arg:		userspace buffer
 *
 * Encoding can be done in following manner
 * 1) map the gpio value into INVALID(-2), SET(1), RESET(0).
 * 2) mask the first 2 bits of gpio.
 * 3) left shift the 2 bits as multiple of 2.
 * 4) multiply factor can be defined as position of gpio pin in struct platform_gpio
 *
 * Return: -EFAULT, if unable to copy the data from kernel space to userspace, 0
 * if Success(or no issue)
 */

static int nfc_gpio_info(struct nfc_dev *nfc_dev, unsigned long arg)
{
	unsigned int gpios_status = 0;
	int value = 0;
	int gpio_no = 0;
	int i;
	int ret = 0;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;

	for (i = 0; i < sizeof(struct platform_gpio) / sizeof(unsigned int);
	     i++) {
		gpio_no = *((unsigned int *)nfc_gpio + i);
		value = get_valid_gpio(gpio_no);
		if (value < 0)
			value = -2;
		gpios_status |= (value & GPIO_STATUS_MASK_BITS)<<(GPIO_POS_SHIFT_VAL*i);
	}
	ret = copy_to_user((uint32_t *) arg, &gpios_status, sizeof(value));
	if (ret) {
		pr_err("NxpDrv: %s : Unable to copy data from kernel space to user space", __func__);
		return -EFAULT;
	}
	return 0;
}

/**
 * nfc_ioctl_power_states() - power control
 * @nfc_dev:    nfc device data structure
 * @arg:    mode that we want to move to
 *
 * Device power control. Depending on the arg value, device moves to
 * different states, refer common.h for args
 *
 * Return: -ENOIOCTLCMD if arg is not supported, 0 if Success(or no issue)
 * and error ret code otherwise
 */
static int nfc_ioctl_power_states(struct nfc_dev *nfc_dev, unsigned long arg)
{
	int ret = 0;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;

	if (arg == NFC_POWER_OFF) {
		/*
		 * We are attempting a hardware reset so let us disable
		 * interrupts to avoid spurious notifications to upper
		 * layers.
		 */
		nfc_dev->nfc_disable_intr(nfc_dev);
		set_valid_gpio(nfc_gpio->dwl_req, 0);
		gpio_set_ven(nfc_dev, 0);
		wake_up_interruptible(&nfc_dev->read_wq);
		nfc_dev->nfc_ven_enabled = false;
		nfc_dev->nfc_state = NFC_STATE_NCI;
	} else if (arg == NFC_POWER_ON) {
		ret = nfc_dev->nfc_enable_intr(nfc_dev);
		if (ret)
			return ret;
		set_valid_gpio(nfc_gpio->dwl_req, 0);

		gpio_set_ven(nfc_dev, 1);
		nfc_dev->nfc_ven_enabled = true;
		nfc_dev->nfc_state = NFC_STATE_NCI;
	} else if (arg == NFC_FW_DWL_VEN_TOGGLE) {
		/*
		 * We are switching to download Mode, toggle the enable pin
		 * in order to set the NFCC in the new mode
		 */
		nfc_dev->nfc_disable_intr(nfc_dev);
		set_valid_gpio(nfc_gpio->dwl_req, 1);
		nfc_dev->nfc_state = NFC_STATE_FW_DWL;
		gpio_set_ven(nfc_dev, 0);
		gpio_set_ven(nfc_dev, 1);
		ret = nfc_dev->nfc_enable_intr(nfc_dev);
	} else if (arg == NFC_FW_DWL_HIGH) {
		/*
		 * Setting firmware download gpio to HIGH
		 * before FW download start
		 */
		pr_debug("NxpDrv: set fw gpio high\n");
		set_valid_gpio(nfc_gpio->dwl_req, 1);
		nfc_dev->nfc_state = NFC_STATE_FW_DWL;

	} else if (arg == NFC_VEN_FORCED_HARD_RESET) {
		nfc_dev->nfc_disable_intr(nfc_dev);
		gpio_set_ven(nfc_dev, 0);
		gpio_set_ven(nfc_dev, 1);
		ret = nfc_dev->nfc_enable_intr(nfc_dev);
		pr_info("NxpDrv: %s VEN forced reset done\n", __func__);

	} else if (arg == NFC_FW_DWL_LOW) {
		/*
		 * Setting firmware download gpio to LOW
		 * FW download finished
		 */
		pr_debug("NxpDrv: set fw gpio LOW\n");
		set_valid_gpio(nfc_gpio->dwl_req, 0);
		nfc_dev->nfc_state = NFC_STATE_NCI;

	} else if (arg == NFC_ENABLE) {
		/* Setting flag true when NFC is enabled */
		WRITE_ONCE(nfc_dev->cold_reset.is_nfc_enabled, true);
	} else if (arg == NFC_DISABLE) {
		/* Setting flag true when NFC is disabled */
		WRITE_ONCE(nfc_dev->cold_reset.is_nfc_enabled, false);
	} else {
		pr_err("NxpDrv: %s: bad arg %lu\n", __func__, arg);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
/**
 * nfc_dev_compat_ioctl - used to set or get data from upper layer.
 * @pfile   file node for opened device.
 * @cmd     ioctl type from upper layer.
 * @arg     ioctl arg from upper layer.
 *
 * NFC and ESE Device power control, based on the argument value
 *
 * Return: -ENOIOCTLCMD if arg is not supported
 * 0 if Success(or no issue)
 * 0 or 1 in case of arg is ESE_GET_PWR/ESE_POWER_STATE
 * and error ret code otherwise
 */
long nfc_dev_compat_ioctl(struct file *pfile, unsigned int cmd,
			  unsigned long arg)
{
	int ret = 0;

	arg = (compat_u64) arg;
	pr_debug("NxpDrv: %s: cmd = %x arg = %zx\n", __func__, cmd, arg);
	ret = nfc_dev_ioctl(pfile, cmd, arg);
	return ret;
}
#endif

/**
 * nfc_post_init() - Configuraing Ven GPIO and hardware check
 * @nfc_dev:    nfc device data structure
 *
 * Configure GPIOs post notification from TZ, ensuring it's a non-secure zone.
 *
 * Return: 0 if Success(or no issue) and error ret code otherwise
 */
int nfc_post_init(struct nfc_dev *dev)
{
	struct platform_gpio *gpio = &dev->configs.gpio;
	int ret;

	if (dev->post_init_done)
		return 0;
	ret = configure_gpio(gpio->ven, GPIO_OUTPUT);
	if (ret)
		return ret;
	dev->gpio_owned |= BIT(1);
	if (gpio_is_valid(gpio->dwl_req)) {
		ret = configure_gpio(gpio->dwl_req, GPIO_OUTPUT);
		if (ret)
			return ret;
		dev->gpio_owned |= BIT(2);
	}
#ifndef NFC_CLK_REQ_GPIO_WAKEUP
	ret = of_property_read_u32_index(dev->i2c_dev.client->dev.of_node,
					DTS_CLKREQ_GPIO_STR, 1, &dev->clkreq_hwirq);
	if (ret)
		return ret;
	ret = msm_gpio_mpm_wake_set(dev->clkreq_hwirq, true);
	if (ret)
		return ret;
	dev->clkreq_wake_enabled = true;
#endif
	ret = nfcc_hw_check(dev);
	if (ret)
		return ret;
	if (dev->nfc_state == NFC_STATE_UNKNOWN)
		return -ENODEV;
	dev->post_init_done = true;
	return 0;
}


#ifdef NFC_SECURE_PERIPHERAL_ENABLED
/**
 * nfc_hw_secure_check() - Checks the NFC secure zone status
 *
 * Queries the TZ secure libraries if NFC is in secure zone statue or not.
 *
 * Return: 0 if FEATURE_NOT_SUPPORTED or PERIPHERAL_NOT_FOUND or nfc_sec_state = 2(non-secure zone) and
 *  return 1 if nfc_sec_state = 1(secure zone) or error otherwise
 */

bool nfc_hw_secure_check(void)
{
	struct Object client_env;
	struct Object app_object;
	u32 nfc_uid = HW_NFC_UID;
	union ObjectArg obj_arg[2] = {{{0, 0}}};
	int ret;
	bool retstat = 1;
	u8 nfc_sec_state = 0;
	/* get rootObj */
	ret = get_client_env_object(&client_env);
	if (ret) {
		pr_err("NxpDrv: Failed to get client_env_object, ret: %d\n", ret);
		return retstat;
	}

	ret = IClientEnv_open(client_env, HW_STATE_UID, &app_object);
	if (ret) {
		pr_debug("NxpDrv: Failed to get app_object, ret: %d\n",  ret);
		if (ret == FEATURE_NOT_SUPPORTED) {
			retstat = 0; /* Do not Assert */
			pr_debug("NxpDrv: Secure HW feature not supported\n");
		}
		goto exit_release_clientenv;
	}

	obj_arg[0].b = (struct ObjectBuf) {&nfc_uid, sizeof(u32)};
	obj_arg[1].b = (struct ObjectBuf) {&nfc_sec_state, sizeof(u8)};
	ret = Object_invoke(app_object, HW_OP_GET_STATE, obj_arg,
			ObjectCounts_pack(1, 1, 0, 0));

	pr_info("NxpDrv: TZ ret: %d nfc_sec_state: %d\n", ret, nfc_sec_state);
	if (ret) {
		if (ret == PERIPHERAL_NOT_FOUND) {
			retstat = 0; /* Do not Assert */
			pr_debug("NxpDrv: Secure HW mode is not updated. Peripheral not found\n");
		}
		goto exit_release_app_obj;
	}

	secure_peripheral_not_found = false;

	/* Refer peripheral state utilities for different states of NFC peripherals */
	if (nfc_sec_state == 1) {
		/*Secure Zone*/
		retstat = 1;
	} else {
		/*Non-Secure Zone*/
		retstat = 0;
	}

	exit_release_app_obj:
		Object_release(app_object);
	exit_release_clientenv:
		Object_release(client_env);

	return  retstat;
}

/**
 * nfc_dynamic_protection_ioctl() - dynamic protection control
 * @nfc_dev:    nfc device data structure
 * @sec_zone_trans:    mode that we want to move to
 * If sec_zone_trans = 1; transition from non-secure zone to secure zone
 * If sec_zone_trans = 0; transition from secure zone to non - secure zone
 *
 * nfc  periheral  dynamic protection control. Depending on the sec_zone_trans value, device moves to
 * secure zone and non-secure  zone
 *
 * Return: -ENOIOCTLCMD if sec_zone_trans val is not supported, 0 if Success(or no issue)
 * and error ret code otherwise
 */
int nfc_dynamic_protection_ioctl(struct nfc_dev *nfc_dev, unsigned long sec_zone_trans)
{
	int ret = 0;

	static int init_flag=1;

	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	if(sec_zone_trans == 1) {
		/*check NFC is disabled, only then set Ven GPIO low*/
		if(nfc_dev->cold_reset.is_nfc_enabled == false) {
			pr_debug("NxpDrv: %s: value %d\n", __func__, gpio_get_value(nfc_gpio->ven));

			chk_eSE_pwr_off = 1;
			/*check if eSE is active, if yes, wait max of 1sec, until it's inactive  */
			if(nfc_dev->is_ese_session_active == true) {
				if(down_timeout(&sem_eSE_pwr_off, msecs_to_jiffies(1000))) {
					/*waited for 1sec yet eSE not turned off, so, ignoring eSE power off*/
					pr_info("NxpDrv: Forcefull shutdown of eSE\n");
				}
			}
			ret = nfc_ioctl_power_states(nfc_dev, 0);
			/*set driver as secure zone, such that no ioctl calls are allowed*/
			nfc_dev->secure_zone = true;
			pr_info("NxpDrv: Driver Secure flag set successful\n");
		} else {
			ret = -1;
		}
	}
	else if(sec_zone_trans == 0) {
		chk_eSE_pwr_off = 0;
		nfc_dev->secure_zone = false;

		if(init_flag) {
			/*Initialize once,only during the  first non-secure entry*/
			ret = nfc_post_init(nfc_dev);
			if(ret == 0)
				init_flag=0;
		}
		else {
			if(!gpio_get_value(nfc_gpio->ven))
				ret = nfc_ioctl_power_states(nfc_dev, 1);
		}
		pr_info("NxpDrv: Func Driver Secure flag clear successful\n");
	} else {
		pr_info("NxpDrv: INVALID ARG\n");
		ret = -ENOIOCTLCMD;
	}

	return ret;
}
#endif

/**
 * nfc_dev_ioctl - used to set or get data from upper layer.
 * @pfile   file node for opened device.
 * @cmd     ioctl type from upper layer.
 * @arg     ioctl arg from upper layer.
 *
 * NFC and ESE Device power control, based on the argument value
 *
 * Return: -ENOIOCTLCMD if arg is not supported
 * 0 if Success(or no issue)
 * 0 or 1 in case of arg is ESE_GET_PWR/ESE_POWER_STATE
 * and error ret code otherwise
 */
long nfc_dev_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg)
{
	guard(srcu)(&nfc_io_srcu);
	int ret = 0;
	struct nfc_dev *nfc_dev = pfile->private_data;

	if (!nfc_dev || READ_ONCE(nfc_dev->removing))
		return -ENODEV;

#ifdef NFC_SECURE_PERIPHERAL_ENABLED
	if( nfc_dev->configs.CNSS_NFC_HW_SECURE_ENABLE == true) {
	    /*Avoiding ioctl call in secure zone*/
	    if(nfc_dev->secure_zone) {
		    if(cmd!=NFC_SECURE_ZONE) {
			   pr_debug("NxpDrv: nfc_dev_ioctl failed\n");
			   return -1;
		    }
	    }
	}
#endif
	pr_debug("NxpDrv: %s: cmd = %x arg = %zx\n", __func__, cmd, arg);
	switch (cmd) {
	case NFC_SET_PWR:
		ret = nfc_ioctl_power_states(nfc_dev, arg);
		break;
	case NFC_SET_RESET_READ_PENDING:
		if (arg == NFC_SET_READ_PENDING) {
			nfc_dev->cold_reset.is_nfc_read_pending = true;
			/* Set default NFC state as NCI for Nfc read pending request */
			nfc_dev->nfc_state = NFC_STATE_NCI;
		} else if (arg == NFC_RESET_READ_PENDING) {
			nfc_dev->cold_reset.is_nfc_read_pending = false;
		} else {
			ret = -EINVAL;
		}
		break;
	case ESE_SET_PWR:
		ret = nfc_ese_pwr(nfc_dev, arg);
		break;
	case ESE_GET_PWR:
		ret = nfc_ese_pwr(nfc_dev, ESE_POWER_STATE);
		break;
	case NFC_GET_GPIO_STATUS:
		ret = nfc_gpio_info(nfc_dev, arg);
		break;
	case NFCC_GET_INFO:
		ret = nfc_ioctl_nfcc_info(pfile, arg);
		break;
	case ESE_COLD_RESET:
		pr_debug("NxpDrv: nfc ese cold reset ioctl\n");
		ret = ese_cold_reset_ioctl(nfc_dev, arg);
		break;
#ifdef NFC_SECURE_PERIPHERAL_ENABLED
	case NFC_SECURE_ZONE:
		if( nfc_dev->configs.CNSS_NFC_HW_SECURE_ENABLE == true) {
		  ret = nfc_dynamic_protection_ioctl(nfc_dev, arg);
		}
		break;
#endif
	default:
		pr_err("NxpDrv: %s: bad cmd %lu\n", __func__, arg);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

int nfc_dev_open(struct inode *inode, struct file *file)
{
	guard(srcu)(&nfc_io_srcu);
	struct nfc_dev *dev;
	int ret;

	mutex_lock(&nfc_registry_lock);
	dev = nfc_active;
	if (!dev || inode->i_cdev != dev->c_dev) {
		mutex_unlock(&nfc_registry_lock);
		return -ENODEV;
	}
	kref_get(&dev->ref);
	mutex_unlock(&nfc_registry_lock);
	ret = nonseekable_open(inode, file);
	if (ret)
		goto put;
	mutex_lock(&dev->dev_ref_mutex);
	if (READ_ONCE(dev->removing)) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!dev->dev_ref_count) {
		set_valid_gpio(dev->configs.gpio.dwl_req, 0);
		ret = dev->nfc_enable_intr(dev);
		if (ret)
			goto unlock;
	}
	dev->dev_ref_count++;
	file->private_data = dev;
unlock:
	mutex_unlock(&dev->dev_ref_mutex);
	if (!ret)
		return 0;
put:
	nfc_dev_put(dev);
	return ret;
}


int nfc_dev_flush(struct file *pfile, fl_owner_t id)
{
	guard(srcu)(&nfc_io_srcu);
	struct nfc_dev *nfc_dev = pfile->private_data;

	if (!nfc_dev || READ_ONCE(nfc_dev->removing))
		return -ENODEV;
	/*
	 * release blocked user thread waiting for pending read during close
	 */
	if (!mutex_trylock(&nfc_dev->read_mutex)) {
		WRITE_ONCE(nfc_dev->release_read, true);
		nfc_dev->nfc_disable_intr(nfc_dev);
		wake_up(&nfc_dev->read_wq);
		pr_debug("NxpDrv: %s: waiting for release of blocked read\n", __func__);
		mutex_lock(&nfc_dev->read_mutex);
		WRITE_ONCE(nfc_dev->release_read, false);
	} else {
		pr_debug("NxpDrv: %s: read thread already released\n", __func__);
	}
	mutex_unlock(&nfc_dev->read_mutex);
	return 0;
}

int nfc_dev_close(struct inode *inode, struct file *file)
{
	guard(srcu)(&nfc_io_srcu);
	struct nfc_dev *dev = file->private_data;

	if (!dev)
		return 0;
	mutex_lock(&dev->dev_ref_mutex);
	if (dev->dev_ref_count == 1 && !READ_ONCE(dev->removing)) {
		dev->nfc_disable_intr(dev);
		set_valid_gpio(dev->configs.gpio.dwl_req, 0);
	}
	if (dev->dev_ref_count)
		dev->dev_ref_count--;
	file->private_data = NULL;
	mutex_unlock(&dev->dev_ref_mutex);
	nfc_dev_put(dev);
	return 0;
}


int validate_nfc_state_nci(struct nfc_dev *nfc_dev)
{
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	if(!nfc_dev->secure_zone) {
		if (!gpio_get_value(nfc_gpio->ven)) {
			pr_err("NxpDrv: %s: ven low - nfcc powered off\n", __func__);
			return -ENODEV;
		}
	}
	if (get_valid_gpio(nfc_gpio->dwl_req) == 1) {
		pr_err("NxpDrv: %s: fw download in-progress\n", __func__);
		return -EBUSY;
	}
	if (nfc_dev->nfc_state != NFC_STATE_NCI) {
		pr_err("NxpDrv: %s: fw download state\n", __func__);
		return -EBUSY;
	}
	return 0;
}
