// SPDX-License-Identifier: GPL-2.0
/*
 * MT6358 battery over-current throttling, the BATTERY_OC part of T's
 * mt6358 pmic_throttling_dlpt.c.
 *
 * The fuel gauge compares its current against two thresholds. FG_CUR_L fires
 * when the discharge goes over the L threshold: level 1, the registered
 * callbacks limit (gpufreq caps the GPU at its battery OC OPP). FG_CUR_H fires
 * when it drops back under the H threshold: level 0, no limit. Only one of the
 * two IRQs is enabled at a time, so neither can storm while its condition holds.
 *
 * gpufreq holds level 1 from boot. This driver releases it (level 0, as stock
 * boots) only once both IRQs are requested and the gauge is running, so the
 * GPU never runs unlimited without a working OC path.
 */
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <upmu_sw.h>

/*
 * Registers from T's upmu_hw.h: FGADC_CON0 bit 0 FG_ON, FGADC_CUR_CON0 the
 * current (two's complement, discharge negative), CUR_CON1 LTH, CUR_CON2 HTH.
 */

/* T's mtk_pmic.h for mt6771 */
#define POWER_BAT_OC_CURRENT_H	4670	/* mA */
#define POWER_BAT_OC_CURRENT_L	5500	/* mA */

/* T's UNIT_FGCURRENT: one current step is 381470 x 10 nA at a 1 mOhm sense resistor */
#define UNIT_FGCURRENT		381470
#define OCCB_NUM		16

struct battery_oc {
	struct device *dev;
	struct regmap *regmap;
	int irq_h;
	int irq_l;
	int r_fg;		/* 0.1 mOhm, as T's fg_cust_data.r_fg_value */
	int car_tune;		/* T's fg_cust_data.car_tune_value */
	int h_ma;
	int l_ma;
};

/* Guards everything below and serialises the callbacks. */
static DEFINE_MUTEX(boc_lock);
static void (*occb_tb[OCCB_NUM])(BATTERY_OC_LEVEL);
static struct battery_oc *boc_armed;
static int boc_level;

/*
 * T's bat_oc_h_thd for gauge version 30, same order of integer steps:
 * 65535 - (I * r_fg * 1000 / UNIT_FGCURRENT * 95 * 100 / car_tune).
 * The 95% makes the trip point 0.95 of the named current.
 */
static int boc_thd(const struct battery_oc *b, int ma, u16 *reg)
{
	u64 steps;

	if (ma <= 0 || ma > 20000)
		return -EINVAL;
	steps = div_u64((u64)ma * b->r_fg * 1000, UNIT_FGCURRENT);
	steps = div_u64(steps * 95 * 100, b->car_tune);
	if (steps == 0 || steps > 0x7fff)
		return -ERANGE;
	*reg = 65535 - steps;
	return 0;
}

/* Gauge current in mA, discharge negative (T's fgauge_read_current scaling). */
static int boc_current_ma(const struct battery_oc *b, int *ma)
{
	unsigned int val;
	s64 tenth_ma;
	int ret;

	ret = regmap_read(b->regmap, PMIC_FG_CURRENT_OUT_ADDR, &val);
	if (ret)
		return ret;
	tenth_ma = div_s64((s64)(s16)val * UNIT_FGCURRENT, 100000);
	tenth_ma = div_s64(tenth_ma * 100 * b->car_tune, (s64)b->r_fg * 1000);
	*ma = (int)div_s64(tenth_ma, 10);
	return 0;
}

static void boc_run_callbacks(BATTERY_OC_LEVEL level)
{
	int i;

	for (i = 0; i < OCCB_NUM; i++)
		if (occb_tb[i])
			occb_tb[i](level);
}

/* Callbacks first, then swap the IRQs, as T's handlers. Caller holds boc_lock. */
static void boc_set_level(struct battery_oc *b, int level)
{
	if (boc_level == level)
		return;
	boc_level = level;
	dev_info(b->dev, "battery OC level %d\n", level);
	boc_run_callbacks(level ? BATTERY_OC_LEVEL_1 : BATTERY_OC_LEVEL_0);
	if (level) {
		disable_irq_nosync(b->irq_l);
		enable_irq(b->irq_h);
	} else {
		disable_irq_nosync(b->irq_h);
		enable_irq(b->irq_l);
	}
}

static irqreturn_t fg_cur_h_int_handler(int irq, void *data)
{
	mutex_lock(&boc_lock);
	if (boc_armed)
		boc_set_level(data, 0);
	mutex_unlock(&boc_lock);
	return IRQ_HANDLED;
}

static irqreturn_t fg_cur_l_int_handler(int irq, void *data)
{
	mutex_lock(&boc_lock);
	if (boc_armed)
		boc_set_level(data, 1);
	mutex_unlock(&boc_lock);
	return IRQ_HANDLED;
}

/* T's API. A callback registered after arming gets the current level at once. */
void register_battery_oc_notify(void (*battery_oc_callback)(BATTERY_OC_LEVEL),
				BATTERY_OC_PRIO prio_val)
{
	if ((unsigned int)prio_val >= OCCB_NUM)
		return;
	mutex_lock(&boc_lock);
	occb_tb[prio_val] = battery_oc_callback;
	if (boc_armed && battery_oc_callback)
		battery_oc_callback(boc_level ? BATTERY_OC_LEVEL_1 : BATTERY_OC_LEVEL_0);
	mutex_unlock(&boc_lock);
	pr_info("[%s] prio_val=%d\n", __func__, prio_val);
}

static int boc_write_thd(struct battery_oc *b, int h_ma, int l_ma)
{
	u16 hth, lth;
	int ret;

	/* Stock's values are the highest allowed, so the trip points stay reachable. */
	if (h_ma >= l_ma || h_ma > POWER_BAT_OC_CURRENT_H || l_ma > POWER_BAT_OC_CURRENT_L)
		return -EINVAL;
	ret = boc_thd(b, h_ma, &hth);
	if (!ret)
		ret = boc_thd(b, l_ma, &lth);
	if (ret)
		return ret;
	ret = regmap_write(b->regmap, PMIC_FG_CUR_HTH_ADDR, hth);
	if (!ret)
		ret = regmap_write(b->regmap, PMIC_FG_CUR_LTH_ADDR, lth);
	if (ret)
		return ret;
	b->h_ma = h_ma;
	b->l_ma = l_ma;
	return 0;
}

static ssize_t battery_oc_protect_level_show(struct device *dev,
					     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", boc_level);
}
static DEVICE_ATTR_RO(battery_oc_protect_level);

static ssize_t battery_oc_protect_thd_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	struct battery_oc *b = dev_get_drvdata(dev);
	unsigned int hth = 0, lth = 0;
	int ma = 0;

	regmap_read(b->regmap, PMIC_FG_CUR_HTH_ADDR, &hth);
	regmap_read(b->regmap, PMIC_FG_CUR_LTH_ADDR, &lth);
	boc_current_ma(b, &ma);
	return sprintf(buf, "h %d mA 0x%04x, l %d mA 0x%04x, current %d mA\n",
		       b->h_ma, hth, b->l_ma, lth, ma);
}

/* "H L" in mA, H below L, as T's store; for testing the IRQs at a lower current. */
static ssize_t battery_oc_protect_thd_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t size)
{
	struct battery_oc *b = dev_get_drvdata(dev);
	int h_ma, l_ma, ret;

	if (sscanf(buf, "%d %d", &h_ma, &l_ma) != 2)
		return -EINVAL;
	mutex_lock(&boc_lock);
	ret = boc_write_thd(b, h_ma, l_ma);
	mutex_unlock(&boc_lock);
	if (ret)
		return ret;
	dev_info(dev, "thresholds now %d mA, %d mA\n", h_ma, l_ma);
	return size;
}
static DEVICE_ATTR_RW(battery_oc_protect_thd);

static struct attribute *boc_attrs[] = {
	&dev_attr_battery_oc_protect_level.attr,
	&dev_attr_battery_oc_protect_thd.attr,
	NULL,
};
ATTRIBUTE_GROUPS(boc);

/* T reads these from /battery in pmic_throttling_dlpt_probe, times 10. */
static int boc_read_gauge_values(struct battery_oc *b)
{
	struct device_node *np = of_find_node_by_path("/battery");
	u32 r_fg, car_tune;
	int ret = -ENODEV;

	if (np && !of_property_read_u32(np, "R_FG_VALUE", &r_fg) &&
	    !of_property_read_u32(np, "CAR_TUNE_VALUE", &car_tune) &&
	    r_fg && car_tune) {
		b->r_fg = r_fg * 10;
		b->car_tune = car_tune * 10;
		ret = 0;
	}
	of_node_put(np);
	return ret;
}

static int boc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct battery_oc *b;
	unsigned int fg_con = 0;
	int ma, ret;

	b = devm_kzalloc(&pdev->dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = &pdev->dev;
	b->regmap = chip ? chip->regmap : NULL;
	if (!b->regmap)
		return -ENODEV;
	platform_set_drvdata(pdev, b);

	ret = boc_read_gauge_values(b);
	if (ret) {
		dev_err(&pdev->dev, "no /battery R_FG_VALUE or CAR_TUNE_VALUE\n");
		goto fail;
	}

	/* LK turns the gauge on; with it off the thresholds never trip. */
	ret = regmap_read(b->regmap, PMIC_FG_ON_ADDR, &fg_con);
	if (!ret && !((fg_con >> PMIC_FG_ON_SHIFT) & PMIC_FG_ON_MASK))
		ret = -ENODEV;
	if (ret) {
		dev_err(&pdev->dev, "fuel gauge off (FGADC_CON0 0x%x)\n", fg_con);
		goto fail;
	}

	b->irq_h = platform_get_irq_byname(pdev, "fg_cur_h");
	b->irq_l = platform_get_irq_byname(pdev, "fg_cur_l");
	if (b->irq_h < 0 || b->irq_l < 0) {
		ret = b->irq_h < 0 ? b->irq_h : b->irq_l;
		if (ret == -EPROBE_DEFER)
			return ret;
		ret = -ENXIO;
		goto fail;
	}

	/* T's order: maximum H first so fg_cur_h can't trip during setup. */
	ret = regmap_write(b->regmap, PMIC_FG_CUR_HTH_ADDR, 0x7fff);
	if (ret)
		goto fail;
	irq_set_status_flags(b->irq_h, IRQ_NOAUTOEN);
	irq_set_status_flags(b->irq_l, IRQ_NOAUTOEN);
	ret = devm_request_threaded_irq(&pdev->dev, b->irq_h, NULL, fg_cur_h_int_handler,
					IRQF_ONESHOT, "fg_cur_h", b);
	if (ret)
		goto fail;
	ret = devm_request_threaded_irq(&pdev->dev, b->irq_l, NULL, fg_cur_l_int_handler,
					IRQF_ONESHOT, "fg_cur_l", b);
	if (ret)
		goto fail;
	ret = boc_write_thd(b, POWER_BAT_OC_CURRENT_H, POWER_BAT_OC_CURRENT_L);
	if (ret)
		goto fail;

	/* Level 0 as stock boots: release the gpufreq hold, then arm fg_cur_l. */
	mutex_lock(&boc_lock);
	boc_armed = b;
	boc_level = 0;
	boc_run_callbacks(BATTERY_OC_LEVEL_0);
	enable_irq(b->irq_l);
	mutex_unlock(&boc_lock);

	ma = 0;
	boc_current_ma(b, &ma);
	dev_info(&pdev->dev,
		 "armed: %d mA, %d mA (r_fg %d, car_tune %d), current %d mA; GPU battery OC hold released\n",
		 b->h_ma, b->l_ma, b->r_fg, b->car_tune, ma);
	return 0;

fail:
	dev_err(&pdev->dev, "not armed (%d); GPU stays held at the battery OC limit\n", ret);
	return ret;
}

/*
 * Ours, not T's: the PMIC IRQs are suspended with the system, so an edge
 * then can be lost and leave the level wrong. Recheck the current on resume.
 */
static int boc_resume(struct device *dev)
{
	struct battery_oc *b = dev_get_drvdata(dev);
	unsigned int cur, th;

	mutex_lock(&boc_lock);
	if (boc_armed != b || regmap_read(b->regmap, PMIC_FG_CURRENT_OUT_ADDR, &cur))
		goto out;
	/* Discharge is negative: over the L trip is below LTH, back under H is above HTH. */
	if (boc_level == 1) {
		if (!regmap_read(b->regmap, PMIC_FG_CUR_HTH_ADDR, &th) && (s16)cur > (s16)th)
			boc_set_level(b, 0);
	} else if (!regmap_read(b->regmap, PMIC_FG_CUR_LTH_ADDR, &th) && (s16)cur < (s16)th) {
		boc_set_level(b, 1);
	}
out:
	mutex_unlock(&boc_lock);
	return 0;
}

static SIMPLE_DEV_PM_OPS(boc_pm_ops, NULL, boc_resume);

static struct platform_driver boc_driver = {
	.probe = boc_probe,
	.driver = {
		.name = "mt6358-battery-oc",
		.dev_groups = boc_groups,
		.pm = &boc_pm_ops,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(boc_driver);
