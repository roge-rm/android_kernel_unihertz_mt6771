// SPDX-License-Identifier: GPL-2.0
/*
 * Modem PA thermistor (mtktsbtsmdpa) as a 5.10 OF thermal sensor.
 *
 * Stock's mtk_ts_btsmdpa reads an NTC on SoC AUXADC channel 1 (AUX_IN1_NTC)
 * with a 390 kOhm pull-up to 1.8 V and the NCP15WF104F03RC 100 k table 7
 * (T thermal/mt6771/inc/tmp_bts.h, and thermal.conf's tzbtspa_param sets the
 * same). This keeps its conversion: calibrated mV, R = 390000 * mV /
 * (1800 - mV), clamped to the -40 C resistance, then linear between table
 * rows in whole degrees, times 1000. The calibration and table are the same
 * as the board NTC in mtk_charger_glue.c.
 *
 * The zone itself, its trips and the modem coolers bound to them are in the
 * board DT (/thermal-zones/mtktsbtsmdpa), in place of stock's
 * /proc/driver/thermal/tzbtspa written by thermal_manager.
 */

#define pr_fmt(fmt) "mtktsbtsmdpa: " fmt

#include <linux/iio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <asm/unaligned.h>

#define BTSMDPA_PULL_UP_R	390000	/* Ohm */
#define BTSMDPA_PULL_UP_MV	1800
#define BTSMDPA_OVER_CRITICAL_LOW	4397119	/* Ohm, -40 C */
/* AUXADC efuse calibration, devinfo word 113, as stock's mtk_auxadc */
#define AUXADC_DEVINFO_INDEX	113
#define AUXADC_CALI_EN		BIT(20)

struct btsmdpa_ntc {
	int temp;	/* C */
	int r;		/* Ohm */
};

/* Table 7 with APPLY_PRECISE_NTC_TABLE, which tmp_bts.h sets for mt6771 */
static const struct btsmdpa_ntc btsmdpa_table7[] = {
	{-40, 4397119}, {-35, 3088599}, {-30, 2197225}, {-25, 1581881},
	{-20, 1151037}, {-15, 846579}, {-10, 628988}, {-5, 471632},
	{0, 357012}, {5, 272500}, {10, 209710}, {15, 162651},
	{20, 127080}, {25, 100000}, {30, 79222}, {35, 63167},
	{40, 50677}, {41, 48528}, {42, 46482}, {43, 44533},
	{44, 42675}, {45, 40904}, {46, 39213}, {47, 37601},
	{48, 36063}, {49, 34595}, {50, 33195}, {51, 31859},
	{52, 30584}, {53, 29366}, {54, 28203}, {55, 27091},
	{56, 26028}, {57, 25013}, {58, 24042}, {59, 23113},
	{60, 22224}, {61, 21374}, {62, 20560}, {63, 19782},
	{64, 19036}, {65, 18322}, {66, 17640}, {67, 16986},
	{68, 16360}, {69, 15759}, {70, 15184}, {71, 14631},
	{72, 14100}, {73, 13591}, {74, 13103}, {75, 12635},
	{76, 12187}, {77, 11756}, {78, 11343}, {79, 10946},
	{80, 10565}, {81, 10199}, {82, 9847}, {83, 9509},
	{84, 9184}, {85, 8872}, {86, 8572}, {87, 8283},
	{88, 8005}, {89, 7738}, {90, 7481}, {95, 6337},
	{100, 5384}, {105, 4594}, {110, 3934}, {115, 3380},
	{120, 2916}, {125, 2522},
};

struct btsmdpa {
	struct device *dev;
	struct iio_channel *chan;
	struct mutex lock;
	int cali_oe, cali_ge;	/* ADC counts, 0 when not fused */
};

/* mtkts_btsmdpa_thermistor_conver_temp: linear between rows, whole degrees */
static int btsmdpa_r_to_temp(int r)
{
	int i, n = ARRAY_SIZE(btsmdpa_table7);
	int r1, t1, r2, t2;

	if (r >= btsmdpa_table7[0].r)
		return -40;
	if (r <= btsmdpa_table7[n - 1].r)
		return 125;
	r1 = btsmdpa_table7[0].r;
	t1 = btsmdpa_table7[0].temp;
	for (i = 0; i < n; i++) {
		if (r >= btsmdpa_table7[i].r)
			break;
		r1 = btsmdpa_table7[i].r;
		t1 = btsmdpa_table7[i].temp;
	}
	r2 = btsmdpa_table7[i].r;
	t2 = btsmdpa_table7[i].temp;
	return ((r - r2) * t1 + (r1 - r) * t2) / (r1 - r2);
}

/* mtk_ts_btsmdpa_volt_to_temp; a negative calibrated mV reads -40 C */
static int btsmdpa_mv_to_temp(int mv)
{
	u64 v_cri = (u64)BTSMDPA_OVER_CRITICAL_LOW * BTSMDPA_PULL_UP_MV;
	int r;

	do_div(v_cri, BTSMDPA_OVER_CRITICAL_LOW + BTSMDPA_PULL_UP_R);
	if (mv < 0 || mv > (int)v_cri)
		r = BTSMDPA_OVER_CRITICAL_LOW;
	else
		r = BTSMDPA_PULL_UP_R * mv / (BTSMDPA_PULL_UP_MV - mv);
	return btsmdpa_r_to_temp(r);
}

/* Stock's mt_auxadc_update_cali from LK's atag,devinfo (word 0 is the count). */
static void btsmdpa_read_cali(struct btsmdpa *b)
{
	struct device_node *chosen = of_find_node_by_path("/chosen");
	const u8 *tag;
	int length;
	u32 w;

	if (!chosen)
		return;
	tag = of_get_property(chosen, "atag,devinfo", &length);
	if (tag && length >= sizeof(u32) * (AUXADC_DEVINFO_INDEX + 2) &&
	    get_unaligned_le32(tag) > AUXADC_DEVINFO_INDEX) {
		w = get_unaligned_le32(tag + sizeof(u32) * (AUXADC_DEVINFO_INDEX + 1));
		if (w & AUXADC_CALI_EN) {
			b->cali_oe = (int)(w & 0x3ff) - 512;
			b->cali_ge = (int)((w >> 10) & 0x3ff) - 512;
		}
	}
	of_node_put(chosen);
}

/* Called by the thermal core in process context; may sleep. */
static int btsmdpa_get_temp(void *data, int *temp)
{
	struct btsmdpa *b = data;
	int mv, raw, ret, t;

	mutex_lock(&b->lock);
	ret = iio_read_channel_processed(b->chan, &mv);
	mutex_unlock(&b->lock);
	if (ret < 0) {
		dev_warn_ratelimited(b->dev, "AUXADC read failed (%d)\n", ret);
		return ret;
	}
	/* mt6577_auxadc gives raw * 1500 / 4096; redo it with stock's calibration. */
	if (b->cali_oe || b->cali_ge) {
		raw = DIV_ROUND_CLOSEST(mv * 4096, 1500);
		mv = (raw - b->cali_oe) * 1500 / (4096 + b->cali_ge);
	}
	t = btsmdpa_mv_to_temp(mv);
	dev_info_once(b->dev, "first reading on AUXADC channel 1: %d mV, %d C\n",
		      mv, t);
	*temp = t * 1000;
	return 0;
}

static const struct thermal_zone_of_device_ops btsmdpa_ops = {
	.get_temp = btsmdpa_get_temp,
};

static int btsmdpa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tz;
	struct btsmdpa *b;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	mutex_init(&b->lock);

	/* the channel name stock's driver asks for */
	b->chan = devm_iio_channel_get(dev, "thermistor-ch1");
	if (IS_ERR(b->chan))
		return dev_err_probe(dev, PTR_ERR(b->chan), "no thermistor-ch1 channel\n");
	btsmdpa_read_cali(b);
	dev_info(dev, "AUXADC calibration offset %d, gain %d\n", b->cali_oe, b->cali_ge);

	tz = devm_thermal_zone_of_sensor_register(dev, 0, b, &btsmdpa_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz), "zone register failed\n");
	return 0;
}

static const struct of_device_id btsmdpa_of_match[] = {
	{ .compatible = "mediatek,mtboard-thermistor2" },
	{ }
};
MODULE_DEVICE_TABLE(of, btsmdpa_of_match);

static struct platform_driver btsmdpa_driver = {
	.probe = btsmdpa_probe,
	.driver = {
		.name = "mtktsbtsmdpa",
		.of_match_table = btsmdpa_of_match,
	},
};
module_platform_driver(btsmdpa_driver);

MODULE_DESCRIPTION("MediaTek modem PA thermistor");
MODULE_LICENSE("GPL v2");
