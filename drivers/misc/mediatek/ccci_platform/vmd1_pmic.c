// SPDX-License-Identifier: GPL-2.0
/*
 * VMODEM for MD1, from 4.14 pmic/mt6358/v1/pmic.c. Stock reads the VMODEM
 * voltage the bootloader set (RG_BUCK_VMODEM_VOSEL) at PMIC probe and
 * writes the same value back each time eccci powers MD1 on. Power off only
 * logs. Same here, with the read at late_initcall_sync once the MT6358
 * regmap exists; a call before that records the value then, as stock's
 * fallback does.
 */

#define pr_fmt(fmt) "ccci_plat: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/regmap.h>

#include <upmu_common.h>
#include "include/pmic_api_buck.h"
#include "ccci_platform.h"

static unsigned short g_vmodem_vosel;

static int vmodem_get(struct regmap *map, unsigned int addr,
		      unsigned int mask, unsigned int shift)
{
	unsigned int val;

	if (regmap_read(map, addr, &val))
		return -EIO;
	return (val >> shift) & mask;
}

static int record_md_vosel(void)
{
	struct regmap *map = ccci_pmic_regmap();
	int vosel;

	if (!map)
		return -EPROBE_DEFER;
	vosel = vmodem_get(map, PMIC_RG_BUCK_VMODEM_VOSEL_ADDR,
			   PMIC_RG_BUCK_VMODEM_VOSEL_MASK,
			   PMIC_RG_BUCK_VMODEM_VOSEL_SHIFT);
	if (vosel < 0)
		return vosel;
	g_vmodem_vosel = vosel;
	pr_info("[%s] vmodem_vosel = 0x%x\n", __func__, g_vmodem_vosel);
	return 0;
}

static int __init vmd1_record_init(void)
{
	if (record_md_vosel())
		pr_notice("[%s] no MT6358 regmap yet, vmodem vosel not recorded\n",
			  __func__);
	return 0;
}
late_initcall_sync(vmd1_record_init);

/* [Export API] */
void vmd1_pmic_setting_on(void)
{
	struct regmap *map = ccci_pmic_regmap();
	int da;

	if (!map) {
		pr_err("[%s] no MT6358 regmap, vmodem left as is\n", __func__);
		return;
	}
	/* 1.Call PMIC driver API configure VMODEM voltage */
	if (g_vmodem_vosel != 0) {
		regmap_update_bits(map, PMIC_RG_BUCK_VMODEM_VOSEL_ADDR,
				   PMIC_RG_BUCK_VMODEM_VOSEL_MASK <<
				   PMIC_RG_BUCK_VMODEM_VOSEL_SHIFT,
				   g_vmodem_vosel <<
				   PMIC_RG_BUCK_VMODEM_VOSEL_SHIFT);
	} else {
		pr_notice("[%s] vmodem vosel has not recorded!\n", __func__);
		record_md_vosel();
	}
	da = vmodem_get(map, PMIC_DA_VMODEM_VOSEL_ADDR,
			PMIC_DA_VMODEM_VOSEL_MASK, PMIC_DA_VMODEM_VOSEL_SHIFT);
	if (da != g_vmodem_vosel)
		pr_notice("[%s] vmodem vosel = 0x%x, da_vosel = 0x%x",
			  __func__,
			  vmodem_get(map, PMIC_RG_BUCK_VMODEM_VOSEL_ADDR,
				     PMIC_RG_BUCK_VMODEM_VOSEL_MASK,
				     PMIC_RG_BUCK_VMODEM_VOSEL_SHIFT), da);
}

void vmd1_pmic_setting_off(void)
{
	pr_debug("%s\n", __func__);
}
