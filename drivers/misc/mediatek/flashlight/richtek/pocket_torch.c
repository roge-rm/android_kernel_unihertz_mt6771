// SPDX-License-Identifier: GPL-2.0
/*
 * Titan Pocket torch: one LED class device, /sys/class/leds/torch, 0 or 1.
 *
 * Stock lit the torch from the camera HAL through /dev/flashlight and
 * flashlights-mt6370, which needs the camera stack. This drives the same two
 * MT6370 channels the same way: channel 1 at torch duty 6
 * (selector 0x0c, 175 mA), channel 2 at duty 0 (selector 0x00, 25 mA), then
 * dual torch mode on channel 1, as flashlights-mt6370 does in couple mode.
 *
 * rt-flashlight calls pocket_torch_update() after each flashlight device is
 * registered or before it is unregistered; the LED is registered once both
 * MT6370 FLED channels are there. The MT6370 calls sleep (i2c), so this is
 * brightness_set_blocking, which the LED core runs from its work queue.
 */

#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include "rt-flashlight.h"

#define POCKET_TORCH_CH1 "mt-flash-led1"
#define POCKET_TORCH_CH2 "mt-flash-led2"
/* Torch current selectors: 25 mA + 12.5 mA per step (mt6370_pmu_fled.c). */
#define POCKET_TORCH_SEL_CH1 0x0c	/* stock duty 6, 175 mA */
#define POCKET_TORCH_SEL_CH2 0x00	/* stock duty 0, 25 mA */

static DEFINE_MUTEX(pocket_torch_lock);
static struct flashlight_device *pocket_torch_ch1;
static struct flashlight_device *pocket_torch_ch2;
static bool pocket_torch_registered;
static bool pocket_torch_on;		/* our light is on (under the lock) */
static bool pocket_torch_camera;	/* the camera flashlight owns the FLEDs */

static int pocket_torch_set(struct led_classdev *led, enum led_brightness value)
{
	int ret;

	mutex_lock(&pocket_torch_lock);
	if (!pocket_torch_ch1 || !pocket_torch_ch2) {
		ret = -ENODEV;
	} else if (pocket_torch_camera) {
		/* The camera owns the channels: no light from us, and an off is already done. */
		ret = value ? -EBUSY : 0;
	} else if (value) {
		ret = flashlight_set_torch_brightness(pocket_torch_ch1,
						      POCKET_TORCH_SEL_CH1);
		if (!ret)
			ret = flashlight_set_torch_brightness(pocket_torch_ch2,
							      POCKET_TORCH_SEL_CH2);
		if (!ret)
			ret = flashlight_set_mode(pocket_torch_ch1,
						  FLASHLIGHT_MODE_DUAL_TORCH);
		if (ret)
			flashlight_set_mode(pocket_torch_ch1,
					    FLASHLIGHT_MODE_DUAL_OFF);
		else
			pocket_torch_on = true;
	} else {
		ret = flashlight_set_mode(pocket_torch_ch1,
					  FLASHLIGHT_MODE_DUAL_OFF);
		pocket_torch_on = false;
	}
	mutex_unlock(&pocket_torch_lock);
	if (ret)
		pr_err("pocket_torch: set %d failed: %d\n", value, ret);
	return ret;
}

/* No LED_CORE_SUSPENDRESUME: the torch stays on in suspend, as stock. */
static struct led_classdev pocket_torch_led = {
	.name = "torch",
	.max_brightness = 1,
	.brightness_set_blocking = pocket_torch_set,
};

/* The camera flashlight is about to light the FLEDs: switch our light off and keep it off. */
void pocket_torch_camera_claim(void)
{
	bool was_on;

	mutex_lock(&pocket_torch_lock);
	was_on = pocket_torch_on;
	if (was_on && pocket_torch_ch1)
		flashlight_set_mode(pocket_torch_ch1, FLASHLIGHT_MODE_DUAL_OFF);
	pocket_torch_on = false;
	pocket_torch_camera = true;
	mutex_unlock(&pocket_torch_lock);

	if (was_on) {
		pr_info("pocket_torch: camera flashlight took over\n");
		/*
		 * The LED core's value follows the light, set the way a sysfs
		 * write does. The queued pocket_torch_set(0) finds the camera
		 * owner and does nothing.
		 */
		if (pocket_torch_led.dev) {
			mutex_lock(&pocket_torch_led.led_access);
			led_set_brightness(&pocket_torch_led, LED_OFF);
			mutex_unlock(&pocket_torch_led.led_access);
			sysfs_notify(&pocket_torch_led.dev->kobj, NULL, "brightness");
		}
	}
}

void pocket_torch_camera_release(void)
{
	mutex_lock(&pocket_torch_lock);
	pocket_torch_camera = false;
	mutex_unlock(&pocket_torch_lock);
}

/*
 * The camera flashlight switches a channel off. Done under our lock, so a
 * pocket_torch light can't come on between the ownership check and the off.
 * Returns false and leaves the channels alone when the light is ours;
 * otherwise *ret is flashlight_set_mode's result.
 */
bool pocket_torch_camera_off(struct flashlight_device *dev,
			     enum flashlight_mode mode, int *ret)
{
	bool owned;

	mutex_lock(&pocket_torch_lock);
	owned = pocket_torch_camera || !pocket_torch_on;
	if (owned)
		*ret = flashlight_set_mode(dev, mode);
	mutex_unlock(&pocket_torch_lock);
	return owned;
}

void pocket_torch_update(struct flashlight_device *dev, bool added)
{
	const char *name = dev_name(&dev->dev);
	struct flashlight_device **slot;
	bool ready;

	if (!strcmp(name, POCKET_TORCH_CH1))
		slot = &pocket_torch_ch1;
	else if (!strcmp(name, POCKET_TORCH_CH2))
		slot = &pocket_torch_ch2;
	else
		return;

	mutex_lock(&pocket_torch_lock);
	*slot = added ? dev : NULL;
	ready = pocket_torch_ch1 && pocket_torch_ch2 && !pocket_torch_registered;
	if (ready)
		pocket_torch_registered = true;
	mutex_unlock(&pocket_torch_lock);

	/* Outside the lock: the LED core may call pocket_torch_set once registered. */
	if (ready && led_classdev_register(NULL, &pocket_torch_led)) {
		pr_err("pocket_torch: led register failed\n");
		mutex_lock(&pocket_torch_lock);
		pocket_torch_registered = false;
		mutex_unlock(&pocket_torch_lock);
	} else if (ready) {
		pr_info("pocket_torch: /sys/class/leds/torch on %s and %s\n",
			POCKET_TORCH_CH1, POCKET_TORCH_CH2);
	}
}
