/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KONOHA_RT2X00_NATIVE_LED_HELPERS_H
#define KONOHA_RT2X00_NATIVE_LED_HELPERS_H

#include <linux/errno.h>
#include <linux/leds.h>
#include <linux/workqueue.h>

/*
 * Exact pinned common 6.18 LED suspend/resume closure under module-local names.
 * GKI KMI trimming omits these exported helpers. Existing LED class state,
 * callbacks and brightness work remain owned by the original LED provider.
 */
static inline int konoha_led_set_brightness_direct(struct led_classdev *led_cdev, unsigned int value)
{
	if (!led_cdev->brightness_set)
		return -ENOTSUPP;

	led_cdev->brightness_set(led_cdev, value);

	return 0;
}

static inline void konoha_led_set_brightness_nopm(struct led_classdev *led_cdev, unsigned int value)
{
	/* Use brightness_set op if available, it is guaranteed not to sleep */
	if (!konoha_led_set_brightness_direct(led_cdev, value))
		return;

	/*
	 * Brightness setting can sleep, delegate it to a work queue task.
	 * value 0 / LED_OFF is special, since it also disables hw-blinking
	 * (sw-blink disable is handled in led_set_brightness()).
	 * To avoid a hw-blink-disable getting lost when a second brightness
	 * change is done immediately afterwards (before the work runs),
	 * it uses a separate work_flag.
	 */
	led_cdev->delayed_set_value = value;
	/* Ensure delayed_set_value is seen before work_flags modification */
	smp_mb__before_atomic();

	if (value)
		set_bit(LED_SET_BRIGHTNESS, &led_cdev->work_flags);
	else {
		clear_bit(LED_SET_BRIGHTNESS, &led_cdev->work_flags);
		clear_bit(LED_SET_BLINK, &led_cdev->work_flags);
		set_bit(LED_SET_BRIGHTNESS_OFF, &led_cdev->work_flags);
	}

	queue_work(led_cdev->wq, &led_cdev->set_brightness_work);
}

static inline void konoha_led_classdev_suspend(struct led_classdev *led_cdev)
{
	led_cdev->flags |= LED_SUSPENDED;
	konoha_led_set_brightness_nopm(led_cdev, 0);
	flush_work(&led_cdev->set_brightness_work);
}

static inline void konoha_led_classdev_resume(struct led_classdev *led_cdev)
{
	konoha_led_set_brightness_nopm(led_cdev, led_cdev->brightness);

	if (led_cdev->flash_resume)
		led_cdev->flash_resume(led_cdev);

	led_cdev->flags &= ~LED_SUSPENDED;
}

#endif
