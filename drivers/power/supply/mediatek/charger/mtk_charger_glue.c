// SPDX-License-Identifier: GPL-2.0
/*
 * 5.10 glue for the stock MTK charger stack on the Titan Pocket.
 *
 * Stock builds PE4 and PD but they only act with a tcpc PD adapter, which
 * isn't built here (no CONFIG_TCPC_CLASS), so their calls return "not
 * connected", the path stock takes with no PD adapter. PE+ and PE2 are off
 * in stock and have inline stubs in their headers.
 *
 * The pmic_* calls stock read from the MT6358 AUXADC, and the USB calls
 * stock had in mtu3/mt6771/mtu3_phy_hal.c, are here too.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/mutex.h>
#include <linux/iio/consumer.h>
#include <linux/platform_device.h>
#include <asm/unaligned.h>
#include <linux/regmap.h>
#include <linux/phy/phy.h>
#include <linux/phy/mediatek/mtk_usb_phy.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/power/oz8806.h>
#include <mt-plat/mtk_boot.h>
#include <mt-plat/charger_class.h>

/* mtk_charger_intf.h has a weak is_usb_rdy() that this file replaces */
#define is_usb_rdy mtk_charger_intf_is_usb_rdy
#include "mtk_charger_intf.h"
#undef is_usb_rdy
#include "mtk_pe40_intf.h"
#include "mtk_pdc_intf.h"

/* ---- PE4 (stock built, unused with no PD adapter) ---- */

bool mtk_pe40_init(struct charger_manager *pinfo)
{
	return false;
}

bool mtk_pe40_is_ready(struct charger_manager *pinfo)
{
	return false;
}

bool mtk_pe40_get_is_connect(struct charger_manager *pinfo)
{
	return false;
}

void mtk_pe40_set_is_enable(struct charger_manager *pinfo, bool enable)
{
}

bool mtk_pe40_get_is_enable(struct charger_manager *pinfo)
{
	return false;
}

int mtk_pe40_init_state(struct charger_manager *pinfo)
{
	return 0;
}

int mtk_pe40_tune1_state(struct charger_manager *pinfo)
{
	return 0;
}

int mtk_pe40_tune2_state(struct charger_manager *pinfo)
{
	return 0;
}

int mtk_pe40_cc_state(struct charger_manager *pinfo)
{
	return 0;
}

void mtk_pe40_plugout_reset(struct charger_manager *pinfo)
{
}

void mtk_pe40_end(struct charger_manager *pinfo, int type, bool retry)
{
}

/* ---- PD (stock built, unused with no PD adapter) ---- */

bool mtk_pdc_check_charger(struct charger_manager *info)
{
	return false;
}

void mtk_pdc_plugout_reset(struct charger_manager *info)
{
}

void mtk_pdc_set_max_watt(struct charger_manager *info, int watt)
{
}

int mtk_pdc_get_max_watt(struct charger_manager *info)
{
	return 0;
}

int mtk_pdc_get_setting(struct charger_manager *info, int *vbus,
			int *cur, int *idx)
{
	return -ENOTSUPP;
}

void mtk_pdc_init_table(struct charger_manager *info)
{
}

bool mtk_pdc_init(struct charger_manager *info)
{
	return false;
}

int mtk_pdc_setup(struct charger_manager *info, int idx)
{
	return -ENOTSUPP;
}

void mtk_pdc_plugout(struct charger_manager *info)
{
}

void mtk_pdc_check_cable_impedance(struct charger_manager *info)
{
}

void mtk_pdc_reset(struct charger_manager *info)
{
}

bool mtk_pdc_check_leave(struct charger_manager *info)
{
	return true;
}

/* SwitchCharging2 is only picked for algorithm "SwitchCharging2"; the board uses SwitchCharging */
int mtk_switch_charging_init2(struct charger_manager *info)
{
	return -ENOTSUPP;
}

/*
 * Board thermistor, as stock's mtk_ts_bts with the mt6771 defaults
 * (tmp_bts.h, and thermal.conf sets the same): SoC AUXADC channel 0 with a
 * 390 kOhm pull-up to 1.8 V and an NCP15WF104F03RC 100 k NTC (table 7, the
 * precise version stock builds). The channel is io-channels "bts" on
 * /charger. Gives whole degrees times 1000 as stock. With no reading it
 * gives 41 C, which makes swchg take its 800 mA step.
 */
#define POCKET_BTS_NO_READING	41000
#define BTS_PULL_UP_R		390000	/* Ohm */
#define BTS_PULL_UP_MV		1800
#define BTS_OVER_CRITICAL_LOW	4397119	/* Ohm, -40 C */
/* AUXADC efuse calibration, devinfo word 113, as stock's mtk_auxadc */
#define AUXADC_DEVINFO_INDEX	113
#define AUXADC_CALI_EN		BIT(20)

struct bts_ntc {
	int temp;	/* C */
	int r;		/* Ohm */
};

static const struct bts_ntc bts_table7[] = {
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

/* mtkts_bts_thermistor_conver_temp: linear between rows, whole degrees */
static int bts_r_to_temp(int r)
{
	int i, n = ARRAY_SIZE(bts_table7);
	int r1, t1, r2, t2;

	if (r >= bts_table7[0].r)
		return -40;
	if (r <= bts_table7[n - 1].r)
		return 125;
	r1 = bts_table7[0].r;
	t1 = bts_table7[0].temp;
	for (i = 0; i < n; i++) {
		if (r >= bts_table7[i].r)
			break;
		r1 = bts_table7[i].r;
		t1 = bts_table7[i].temp;
	}
	r2 = bts_table7[i].r;
	t2 = bts_table7[i].temp;
	return ((r - r2) * t1 + (r1 - r) * t2) / (r1 - r2);
}

/*
 * mtk_ts_bts_volt_to_temp: above the -40 C voltage, clamp to the -40 C resistance.
 * Stock's unsigned mV wraps when calibration goes below zero, which reads cold.
 */
static int bts_mv_to_temp(int mv)
{
	u64 v_cri = (u64)BTS_OVER_CRITICAL_LOW * BTS_PULL_UP_MV;
	int r;

	do_div(v_cri, BTS_OVER_CRITICAL_LOW + BTS_PULL_UP_R);
	if (mv < 0 || mv > (int)v_cri)
		r = BTS_OVER_CRITICAL_LOW;
	else
		r = BTS_PULL_UP_R * mv / (BTS_PULL_UP_MV - mv);
	return bts_r_to_temp(r);
}

static DEFINE_MUTEX(bts_lock);
static struct iio_channel *bts_chan;
static bool bts_gave_up;
static int bts_cali_oe, bts_cali_ge;	/* ADC counts, 0 when not fused */

/* Stock's mt_auxadc_update_cali from LK's atag,devinfo (word 0 is the count). */
static void bts_read_cali(void)
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
			bts_cali_oe = (int)(w & 0x3ff) - 512;
			bts_cali_ge = (int)((w >> 10) & 0x3ff) - 512;
		}
		pr_info("mtk_charger: AUXADC devinfo 0x%x, offset %d, gain %d\n",
			w, bts_cali_oe, bts_cali_ge);
	}
	of_node_put(chosen);
}

/* Called from the charger thread; may sleep. */
static struct iio_channel *bts_get_channel(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct iio_channel *chan;

	if (bts_chan || bts_gave_up)
		return bts_chan;
	np = of_find_compatible_node(NULL, NULL, "mediatek,charger");
	pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!pdev) {
		pr_warn("mtk_charger: no /charger device, board thermistor reads 41 C\n");
		bts_gave_up = true;
		return NULL;
	}
	chan = iio_channel_get(&pdev->dev, "bts");
	put_device(&pdev->dev);
	if (IS_ERR(chan)) {
		/* The AUXADC may not be bound yet; try again next time. */
		if (PTR_ERR(chan) != -EPROBE_DEFER) {
			pr_warn("mtk_charger: no bts channel (%ld), board thermistor reads 41 C\n",
				PTR_ERR(chan));
			bts_gave_up = true;
		}
		return NULL;
	}
	bts_read_cali();
	bts_chan = chan;
	return chan;
}

int mtkts_bts_get_hw_temp(void)
{
	struct iio_channel *chan;
	int mv, raw, ret, t;

	mutex_lock(&bts_lock);
	chan = bts_get_channel();
	ret = chan ? iio_read_channel_processed(chan, &mv) : -ENODEV;
	mutex_unlock(&bts_lock);
	if (ret < 0) {
		pr_warn_ratelimited("mtk_charger: board thermistor read failed (%d)\n", ret);
		return POCKET_BTS_NO_READING;
	}
	/* mt6577_auxadc gives raw * 1500 / 4096; redo it with stock's calibration. */
	if (bts_cali_oe || bts_cali_ge) {
		raw = DIV_ROUND_CLOSEST(mv * 4096, 1500);
		mv = (raw - bts_cali_oe) * 1500 / (4096 + bts_cali_ge);
	}
	t = bts_mv_to_temp(mv);
	pr_info_once("mtk_charger: board thermistor on AUXADC channel 0: %d mV, %d C\n",
		     mv, t);
	return t * 1000;
}

/* ---- boot mode ---- */

bool is_kernel_power_off_charging(void)
{
	int boot_mode = get_boot_mode();

	return boot_mode == KERNEL_POWER_OFF_CHARGING_BOOT ||
	       boot_mode == LOW_POWER_OFF_CHARGING_BOOT;
}
EXPORT_SYMBOL(is_kernel_power_off_charging);

/* ---- USB, stock mtu3/mt6771/mtu3_phy_hal.c ---- */

static DEFINE_MUTEX(chg_phy_lock);
static struct phy *chg_phy;

static struct phy *chg_usb_phy(void)
{
	struct device_node *np;
	struct phy *phy;

	mutex_lock(&chg_phy_lock);
	if (!chg_phy) {
		np = of_find_compatible_node(NULL, NULL, "mediatek,mt6771-mtu3");
		if (np) {
			phy = of_phy_get(np, "port0_phy");
			if (!IS_ERR(phy))
				chg_phy = phy;
			of_node_put(np);
		}
	}
	mutex_unlock(&chg_phy_lock);
	return chg_phy;
}

/* Hands D+/D- to BC1.2 detection, as stock */
void Charger_Detect_Init(void)
{
	struct phy *phy = chg_usb_phy();

	if (phy)
		usb_mtkphy_switch_to_bc11(phy, true);
	else
		pr_notice("%s: no USB phy\n", __func__);
}
EXPORT_SYMBOL(Charger_Detect_Init);

void Charger_Detect_Release(void)
{
	struct phy *phy = chg_usb_phy();

	if (phy)
		usb_mtkphy_switch_to_bc11(phy, false);
}
EXPORT_SYMBOL(Charger_Detect_Release);

/*
 * Stock waits here (up to 25 s) for USB before BC1.2 on a CDP. Ready once
 * mtu3 has probed and initialised the PHY.
 */
bool is_usb_rdy(void)
{
	struct phy *phy = chg_usb_phy();

	return phy && phy->init_count > 0;
}

/* ---- pmic, stock drivers/misc/mediatek/pmic/mtk_battery_adc_intf.c ---- */

/*
 * mV. Stock read the MT6358 AUXADC VCDT channel, which has no 5.10 driver,
 * so this reads the MT6370 VBUS ADC. -1 if it can't be read.
 */
int pmic_get_vbus(void)
{
	struct charger_device *chg = get_charger_by_name("primary_chg");
	u32 vbus = 0;

	if (!chg || charger_dev_get_vbus(chg, &vbus) < 0)
		return -1;
	return vbus / 1000;
}

/* Stock returns 0 here with the power path in the charger */
int pmic_get_charging_current(void)
{
	return 0;
}

int pmic_get_battery_voltage(void)
{
	return oz8806_get_battery_voltage();
}

int pmic_is_bif_exist(void)
{
	return 0;
}

int pmic_get_bif_battery_voltage(int *vbat)
{
	return -ENOTSUPP;
}

/* Only PE and PD call this */
int pmic_enable_hw_vbus_ovp(bool enable)
{
	return 0;
}

/* MT6358 BATON_ANA_CON0, RG_BATON_EN is bit 0 */
#define MT6358_BATON_ANA_CON0	0x0e08
#define MT6358_RG_BATON_EN	BIT(0)
/* MT6358 BATON_ANA_MON0, AD_BATON_UNDET is bit 1 (1 = no battery) */
#define MT6358_BATON_ANA_MON0	0x0e0a
#define MT6358_AD_BATON_UNDET	BIT(1)

static struct regmap *chg_pmic_map(void)
{
	static struct regmap *map;
	struct device_node *np;
	struct platform_device *pdev;
	struct mt6397_chip *chip;

	if (map)
		return map;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	chip = platform_get_drvdata(pdev);
	if (chip && chip->regmap)
		map = chip->regmap;
	put_device(&pdev->dev);
	return map;
}

/*
 * As stock: AD_BATON_UNDET clear means the battery is there. A failed read counts as present.
 * Stock PMIC_check_battery sets RG_BATON_EN before the first reading, so do that once here.
 * The detector isn't trusted until RG_BATON_EN is on.
 */
bool pmic_is_battery_exist(void)
{
	static bool baton_en;
	struct regmap *map = chg_pmic_map();
	unsigned int v;

	if (!map)
		return true;
	if (!baton_en) {
		if (regmap_update_bits(map, MT6358_BATON_ANA_CON0,
				       MT6358_RG_BATON_EN, MT6358_RG_BATON_EN))
			return true;
		baton_en = true;
		if (!regmap_read(map, MT6358_BATON_ANA_MON0, &v))
			pr_info("%s: BATON_ANA_MON0 0x%x after RG_BATON_EN\n",
				__func__, v);
	}
	if (regmap_read(map, MT6358_BATON_ANA_MON0, &v))
		return true;
	return !(v & MT6358_AD_BATON_UNDET);
}

/* Stock MT6358 RGS_CHRDET, CHR_CON0 bit 5: VBUS present. 0 if it can't be read. */
#define MT6358_CHR_CON0		0x0a88
#define MT6358_RGS_CHRDET	BIT(5)

unsigned int upmu_get_rgs_chrdet(void)
{
	struct regmap *map = chg_pmic_map();
	unsigned int v;

	if (!map || regmap_read(map, MT6358_CHR_CON0, &v))
		return 0;
	return !!(v & MT6358_RGS_CHRDET);
}
