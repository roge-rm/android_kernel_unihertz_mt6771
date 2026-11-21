// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * MediaTek vibrator (MT6358 VIBR LDO) for 5.10, from the stock 4.14
 * drivers/misc/mediatek/vibrator/vibrator_drv.c and mt6771/vibrator.c.
 *
 * Same sysfs as stock: /sys/class/leds/vibrator/{activate,duration,state},
 * which the stock vendor HAL drives. The PMIC register calls are replaced by
 * the mt6358-regulator "vib" supply (the same VIBR_CON0 enable bit and
 * VIBR_ANA_CON0 VOSEL field) and the VIBR_OC interrupt from the DT.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define VIB_DEVICE		"mtk_vibrator"
#define VIB_TAG			"[vibrator]"

/* Stock waits 3 ms after the LDO turns on before it unmasks VIBR_OC. */
#define OC_INTR_INIT_DELAY_US	3000
/* Longest single vibration, in ms, as stock. */
#define VIB_MAX_MS		15000

/*
 * VIBR VOSEL codes (mt6771/vibrator.c) in uV. The stock DT sets vib_vol 0xb.
 * Codes not listed here are not valid on the MT6358.
 */
static const struct {
	u32 code;
	int uV;
} vibr_vosel[] = {
	{ 0x0, 1200000 }, { 0x1, 1300000 }, { 0x2, 1500000 }, { 0x4, 1800000 },
	{ 0x5, 2000000 }, { 0x9, 2800000 }, { 0xb, 3000000 }, { 0xd, 3300000 },
};

struct mt_vibr {
	struct workqueue_struct *vibr_queue;
	struct work_struct vibr_work;
	struct hrtimer vibr_timer;
	struct regulator *supply;
	int oc_irq;
	atomic_t oc_armed;	/* 1 while VIBR_OC is enabled; whoever clears it disables the IRQ */
	int ldo_state;		/* only touched from vibr_work and shutdown */
	int shutdown_flag;
	atomic_t vibr_dur;
	spinlock_t vibr_lock;
	atomic_t vibr_state;
	u32 vib_timer;		/* ms, minimum on time (stock DT 25) */
	u32 vib_limit;		/* ms, requests at or below this are not stretched (stock DT 9) */
	u32 vib_vol;		/* VOSEL code (stock DT 0xb, 3.0 V) */
};

static struct mt_vibr *g_mt_vib;
static atomic_t vib_state;

static void vibr_Enable_HW(struct mt_vibr *vibr)
{
	int ret = regulator_enable(vibr->supply);

	if (ret) {
		pr_err(VIB_TAG "enable failed: %d\n", ret);
		return;
	}
	vibr->ldo_state = 1;
	usleep_range(OC_INTR_INIT_DELAY_US, OC_INTR_INIT_DELAY_US + 500);
	if (vibr->oc_irq > 0) {
		atomic_set(&vibr->oc_armed, 1);
		enable_irq(vibr->oc_irq);
	}
}

static void vibr_Disable_HW(struct mt_vibr *vibr)
{
	if (vibr->oc_irq > 0 && atomic_xchg(&vibr->oc_armed, 0))
		disable_irq_nosync(vibr->oc_irq);
	regulator_disable(vibr->supply);
	vibr->ldo_state = 0;
}

static void update_vibrator(struct work_struct *work)
{
	struct mt_vibr *vibr = container_of(work, struct mt_vibr, vibr_work);

	if (atomic_read(&vibr->vibr_state) == 0) {
		if (vibr->ldo_state)
			vibr_Disable_HW(vibr);
	} else if (!vibr->ldo_state) {
		vibr_Enable_HW(vibr);
	}
}

static void vibrator_enable(unsigned int dur, unsigned int activate)
{
	unsigned long flags;
	struct mt_vibr *vibr = g_mt_vib;

	spin_lock_irqsave(&vibr->vibr_lock, flags);
	hrtimer_cancel(&vibr->vibr_timer);

	if (activate == 0 || vibr->shutdown_flag == 1) {
		atomic_set(&vibr->vibr_state, 0);
	} else {
		if (dur > vibr->vib_limit && dur < vibr->vib_timer)
			dur = vibr->vib_timer;
		dur = min_t(unsigned int, dur, VIB_MAX_MS);
		atomic_set(&vibr->vibr_state, 1);
		hrtimer_start(&vibr->vibr_timer,
			      ktime_set(dur / 1000, (dur % 1000) * 1000000),
			      HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&vibr->vibr_lock, flags);
	queue_work(vibr->vibr_queue, &vibr->vibr_work);
}

static irqreturn_t vibrator_oc_handler(int irq, void *data)
{
	struct mt_vibr *vibr = data;

	/*
	 * VIBR_OC is level high: mask it until vibr_work has the LDO off, or it
	 * refires as soon as this oneshot thread returns. vibr_Enable_HW unmasks it.
	 */
	if (atomic_xchg(&vibr->oc_armed, 0))
		disable_irq_nosync(irq);
	pr_err(VIB_TAG "over-current, vibrator off\n");
	vibrator_enable(0, 0);
	return IRQ_HANDLED;
}

static enum hrtimer_restart vibrator_timer_func(struct hrtimer *timer)
{
	struct mt_vibr *vibr = container_of(timer, struct mt_vibr, vibr_timer);

	atomic_set(&vibr->vibr_state, 0);
	queue_work(vibr->vibr_queue, &vibr->vibr_work);
	return HRTIMER_NORESTART;
}

static ssize_t vibr_activate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&g_mt_vib->vibr_state));
}

static ssize_t vibr_activate_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int activate = 0;
	ssize_t ret;

	ret = kstrtouint(buf, 10, &activate);
	if (ret) {
		pr_err(VIB_TAG "set activate fail\n");
		return ret;
	}
	vibrator_enable(atomic_read(&g_mt_vib->vibr_dur), activate);
	return size;
}

/* state is stored and read back only, as stock. */
static ssize_t vibr_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&vib_state));
}

static ssize_t vibr_state_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int state = 0;
	ssize_t ret;

	ret = kstrtouint(buf, 10, &state);
	if (ret) {
		pr_err(VIB_TAG "set state fail\n");
		return ret;
	}
	atomic_set(&vib_state, state);
	return size;
}

static ssize_t vibr_duration_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int duration = 0;
	ssize_t ret;

	ret = kstrtouint(buf, 10, &duration);
	if (ret) {
		pr_err(VIB_TAG "set duration fail\n");
		return ret;
	}
	atomic_set(&g_mt_vib->vibr_dur, duration);
	return size;
}

static DEVICE_ATTR(activate, 0644, vibr_activate_show, vibr_activate_store);
static DEVICE_ATTR(state, 0644, vibr_state_show, vibr_state_store);
static DEVICE_ATTR(duration, 0644, NULL, vibr_duration_store);

static struct attribute *vibr_attrs[] = {
	&dev_attr_activate.attr,
	&dev_attr_state.attr,
	&dev_attr_duration.attr,
	NULL,
};

static const struct attribute_group vibr_attr_group = {
	.attrs = vibr_attrs,
};

static const struct attribute_group *vibr_group[] = {
	&vibr_attr_group,
	NULL
};

static struct led_classdev led_vibr = {
	.name		= "vibrator",
	.groups		= vibr_group,
};

static int vibr_uV(u32 code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vibr_vosel); i++)
		if (vibr_vosel[i].code == code)
			return vibr_vosel[i].uV;
	return -EINVAL;
}

static int vib_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt_vibr *vibr;
	int uV, ret;

	vibr = devm_kzalloc(dev, sizeof(*vibr), GFP_KERNEL);
	if (!vibr)
		return -ENOMEM;

	/* Stock defaults when the DT has no value. */
	if (of_property_read_u32(dev->of_node, "vib_timer", &vibr->vib_timer))
		vibr->vib_timer = 25;
	if (of_property_read_u32(dev->of_node, "vib_limit", &vibr->vib_limit))
		vibr->vib_limit = 9;
	if (of_property_read_u32(dev->of_node, "vib_vol", &vibr->vib_vol))
		vibr->vib_vol = 0x05;
	uV = vibr_uV(vibr->vib_vol);
	if (uV < 0) {
		dev_err(dev, "vib_vol 0x%x is not a VIBR VOSEL code\n", vibr->vib_vol);
		return uV;
	}

	vibr->supply = devm_regulator_get(dev, "vib");
	if (IS_ERR(vibr->supply))
		return dev_err_probe(dev, PTR_ERR(vibr->supply), "no vib supply\n");
	ret = regulator_set_voltage(vibr->supply, uV, uV);
	if (ret) {
		dev_err(dev, "set %d uV failed: %d\n", uV, ret);
		return ret;
	}
	/* Nothing else uses VIBR; if the bootloader left it on, turn it off. */
	if (regulator_is_enabled(vibr->supply) > 0) {
		ret = regulator_enable(vibr->supply);
		if (!ret)
			regulator_disable(vibr->supply);
		dev_info(dev, "VIBR was on at probe, turned off (%d)\n", ret);
	}

	/* VIBR_OC is optional: without it the driver runs as stock less the OC cut. */
	atomic_set(&vibr->oc_armed, 0);
	vibr->oc_irq = platform_get_irq_optional(pdev, 0);
	if (vibr->oc_irq > 0) {
		ret = devm_request_threaded_irq(dev, vibr->oc_irq, NULL,
						vibrator_oc_handler,
						IRQF_ONESHOT | IRQF_NO_AUTOEN,
						"vibr_oc", vibr);
		if (ret) {
			dev_err(dev, "VIBR_OC irq %d: %d\n", vibr->oc_irq, ret);
			return ret;
		}
	} else if (vibr->oc_irq == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	}

	vibr->vibr_queue = create_singlethread_workqueue(VIB_DEVICE);
	if (!vibr->vibr_queue)
		return -ENOMEM;

	INIT_WORK(&vibr->vibr_work, update_vibrator);
	spin_lock_init(&vibr->vibr_lock);
	vibr->shutdown_flag = 0;
	atomic_set(&vibr->vibr_state, 0);
	hrtimer_init(&vibr->vibr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vibr->vibr_timer.function = vibrator_timer_func;

	dev_set_drvdata(dev, vibr);
	g_mt_vib = vibr;

	ret = devm_led_classdev_register(dev, &led_vibr);
	if (ret < 0) {
		pr_err(VIB_TAG "led class register fail\n");
		g_mt_vib = NULL;
		destroy_workqueue(vibr->vibr_queue);
		return ret;
	}

	dev_info(dev, "%d uV (vosel 0x%x), timer %u ms, limit %u ms, oc irq %d\n",
		 uV, vibr->vib_vol, vibr->vib_timer, vibr->vib_limit, vibr->oc_irq);
	return 0;
}

static int vib_remove(struct platform_device *pdev)
{
	struct mt_vibr *vibr = dev_get_drvdata(&pdev->dev);

	devm_led_classdev_unregister(&pdev->dev, &led_vibr);
	hrtimer_cancel(&vibr->vibr_timer);
	atomic_set(&vibr->vibr_state, 0);
	queue_work(vibr->vibr_queue, &vibr->vibr_work);
	flush_workqueue(vibr->vibr_queue);
	destroy_workqueue(vibr->vibr_queue);
	g_mt_vib = NULL;
	return 0;
}

static void vib_shutdown(struct platform_device *pdev)
{
	unsigned long flags;
	struct mt_vibr *vibr = dev_get_drvdata(&pdev->dev);

	spin_lock_irqsave(&vibr->vibr_lock, flags);
	vibr->shutdown_flag = 1;
	atomic_set(&vibr->vibr_state, 0);
	spin_unlock_irqrestore(&vibr->vibr_lock, flags);
	hrtimer_cancel(&vibr->vibr_timer);
	flush_workqueue(vibr->vibr_queue);
	if (vibr->ldo_state)
		vibr_Disable_HW(vibr);
}

static const struct of_device_id vibr_of_ids[] = {
	{ .compatible = "mediatek,vibrator", },
	{}
};
MODULE_DEVICE_TABLE(of, vibr_of_ids);

static struct platform_driver vibrator_driver = {
	.probe = vib_probe,
	.remove = vib_remove,
	.shutdown = vib_shutdown,
	.driver = {
		.name = VIB_DEVICE,
		.of_match_table = vibr_of_ids,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(vibrator_driver);

MODULE_AUTHOR("MediaTek Inc.");
MODULE_DESCRIPTION("MTK Vibrator Driver (VIB)");
MODULE_LICENSE("GPL");
