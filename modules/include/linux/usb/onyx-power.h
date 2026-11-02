/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_USB_ONYX_POWER_H
#define __LINUX_USB_ONYX_POWER_H

#include <linux/regulator/consumer.h>

struct onyx_usb_supply {
	struct regulator *regulator;
	int min_uv, max_uv, off_uv, load_ua;
	bool enabled, voltage_set, load_set;
};

/* Keep failed cleanup votes recorded so a later shutdown can retry them. */
static inline int onyx_usb_supplies_off(struct onyx_usb_supply *s, unsigned int count)
{
	int ret, error = 0;

	while (count--) {
		struct onyx_usb_supply *rail = &s[count];

		if (rail->enabled) {
			ret = regulator_disable(rail->regulator);
			if (ret)
				return error ?: ret;
			rail->enabled = false;
		}
		if (rail->voltage_set) {
			ret = regulator_set_voltage(rail->regulator, rail->off_uv, rail->max_uv);
			if (!ret)
				rail->voltage_set = false;
			else if (!error)
				error = ret;
		}
		if (rail->load_set) {
			ret = regulator_set_load(rail->regulator, 0);
			if (ret >= 0)
				rail->load_set = false;
			else if (!error)
				error = ret;
		}
	}
	return error;
}

/* Preserve the vendor's sequential rail order and the first enable error. */
static inline int onyx_usb_supplies_on(struct onyx_usb_supply *s, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		if (s[i].enabled)
			continue;
		ret = regulator_set_load(s[i].regulator, s[i].load_ua);
		if (ret < 0)
			goto failed;
		s[i].load_set = true;
		ret = regulator_set_voltage(s[i].regulator, s[i].min_uv, s[i].max_uv);
		if (ret)
			goto failed;
		s[i].voltage_set = true;
		ret = regulator_enable(s[i].regulator);
		if (ret)
			goto failed;
		s[i].enabled = true;
	}
	return 0;
failed:
	onyx_usb_supplies_off(s, count);
	return ret;
}

#endif /* __LINUX_USB_ONYX_POWER_H */
