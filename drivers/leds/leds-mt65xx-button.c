// SPDX-License-Identifier: GPL-2.0
/*
 * Titan Pocket keyboard backlight: the button-backlight LED of the stock
 * leds-mt65xx driver in GPIO mode (led_mode 2), on GPIO 159.
 *
 * Off drives the line low and holds it 5 ms. On, from off, sends 16 pulses
 * (5 us low, 5 us high) and holds high 3 ms; on while already on does nothing.
 * The LED chip behind the line is unknown; 16 pulses is what stock sends.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define BUTTON_PULSES		16
#define BUTTON_PULSE_US		5
#define BUTTON_OFF_HOLD_US	5000
#define BUTTON_ON_HOLD_US	3000

struct button_led {
	struct led_classdev cdev;
	struct gpio_desc *gpio;
	struct work_struct work;	/* runs on system_wq, as stock */
	enum led_brightness level;	/* last level set, read by the work */
	bool on;			/* only touched by the work */
};

static void button_led_work(struct work_struct *work)
{
	struct button_led *led = container_of(work, struct button_led, work);
	unsigned long flags;
	int i;

	if (READ_ONCE(led->level) == LED_OFF) {
		gpiod_set_value(led->gpio, 0);
		usleep_range(BUTTON_OFF_HOLD_US, BUTTON_OFF_HOLD_US + 100);
		led->on = false;
	} else if (!led->on) {
		led->on = true;
		/* About 160 us with interrupts off, so no pulse is stretched. */
		local_irq_save(flags);
		for (i = 0; i < BUTTON_PULSES; i++) {
			gpiod_set_value(led->gpio, 0);
			udelay(BUTTON_PULSE_US);
			gpiod_set_value(led->gpio, 1);
			udelay(BUTTON_PULSE_US);
		}
		local_irq_restore(flags);
		usleep_range(BUTTON_ON_HOLD_US, BUTTON_ON_HOLD_US + 100);
	}
}

static void button_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct button_led *led = container_of(cdev, struct button_led, cdev);

	WRITE_ONCE(led->level, value);
	queue_work(system_wq, &led->work);
}

static int button_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct button_led *led;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;
	/* Stock only writes the data-out bit; the line keeps the bootloader's level until the first write. */
	led->gpio = devm_gpiod_get(dev, NULL, GPIOD_ASIS);
	if (IS_ERR(led->gpio))
		return dev_err_probe(dev, PTR_ERR(led->gpio), "no gpio\n");
	if (gpiod_direction_output(led->gpio, gpiod_get_value(led->gpio)))
		return dev_err_probe(dev, -EIO, "gpio not an output\n");
	INIT_WORK(&led->work, button_led_work);
	led->cdev.name = "button-backlight";
	led->cdev.max_brightness = LED_FULL;
	led->cdev.brightness_set = button_led_set;
	platform_set_drvdata(pdev, led);
	return devm_led_classdev_register(dev, &led->cdev);
}

static int button_led_remove(struct platform_device *pdev)
{
	struct button_led *led = platform_get_drvdata(pdev);

	devm_led_classdev_unregister(&pdev->dev, &led->cdev);
	/* Unregister sets brightness 0; let that write reach the line. */
	flush_work(&led->work);
	return 0;
}

static const struct of_device_id button_led_of_match[] = {
	{ .compatible = "mediatek,button-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, button_led_of_match);

static struct platform_driver button_led_driver = {
	.driver = {
		.name = "leds-mt65xx-button",
		.of_match_table = button_led_of_match,
	},
	.probe = button_led_probe,
	.remove = button_led_remove,
};
module_platform_driver(button_led_driver);

MODULE_DESCRIPTION("Titan Pocket keyboard backlight on a GPIO pulse line");
MODULE_LICENSE("GPL");
