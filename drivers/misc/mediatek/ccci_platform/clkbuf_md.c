// SPDX-License-Identifier: GPL-2.0
/*
 * The clock buffer calls eccci makes, from 4.14 clkbuf_v1 mtk_clkbuf_ctl.c
 * and mt6771/mtk_clkbuf_hw.c. MT6771 runs the buffers from the MT6358
 * (is_pmic_clkbuf is true), so the RF driving current and AFC calls return
 * without touching anything, and flight mode sets or clears XO_BB_LPM_CEL
 * (DCXO_CW23 bit 0).
 *
 * Stock's clk_buf_init makes no PMIC write on this board in the kernel
 * (the init writes are under #ifndef __KERNEL__, CLKBUF_USE_BBLPM isn't
 * defined for MT6771, the board boots from UFS, has NFC and enables
 * XO 2, 3, 4 and 7), so it isn't ported. clk_buf_ctrl and the XO status
 * calls stay in the connectivity adapter (0040).
 */

#define pr_fmt(fmt) "ccci_plat: " fmt

#include <linux/kernel.h>
#include <linux/regmap.h>

#include <mtk_clkbuf_ctl.h>
#include <upmu_common.h>
#include "ccci_platform.h"

/* false: rf_clkbuf, true: pmic_clkbuf */
static const bool is_pmic_clkbuf = true;
static bool g_is_flightmode_on;

void clk_buf_ctrl_bblpm_hw(short on)
{
	struct regmap *map;
	unsigned int cw23 = 0;

	if (!is_pmic_clkbuf)
		return;

	map = ccci_pmic_regmap();
	if (!map) {
		pr_err("%s(%u): no MT6358 regmap\n", __func__, on ? 1 : 0);
		return;
	}
	if (on)
		regmap_update_bits(map, PMIC_XO_BB_LPM_CEL_ADDR,
				   PMIC_XO_BB_LPM_CEL_MASK << PMIC_XO_BB_LPM_CEL_SHIFT,
				   0x1 << PMIC_XO_BB_LPM_CEL_SHIFT);
	else
		regmap_update_bits(map, PMIC_XO_BB_LPM_CEL_ADDR,
				   PMIC_XO_BB_LPM_CEL_MASK << PMIC_XO_BB_LPM_CEL_SHIFT,
				   0x0 << PMIC_XO_BB_LPM_CEL_SHIFT);

	regmap_read(map, PMIC_XO_BB_LPM_CEL_ADDR, &cw23);
	pr_debug("%s(%u): CW23=0x%x\n", __func__, (on ? 1 : 0), cw23);
}

/* for spm driver use */
bool is_clk_buf_under_flightmode(void)
{
	return g_is_flightmode_on;
}

/* for ccci driver to notify this */
void clk_buf_set_by_flightmode(bool is_flightmode_on)
{
	g_is_flightmode_on = is_flightmode_on;
	if (is_flightmode_on)
		clk_buf_ctrl_bblpm_hw(true);
	else
		clk_buf_ctrl_bblpm_hw(false);
}

void clk_buf_get_swctrl_status(enum CLK_BUF_SWCTRL_STATUS_T *status)
{
	/* stock returns here for a PMIC clock buffer */
}

/*
 * Let caller get driving current setting of RF clock buffer
 * Caller: ccci & ccci will send it to modem
 */
void clk_buf_get_rf_drv_curr(void *rf_drv_curr)
{
	/* stock returns here for a PMIC clock buffer */
}

/* Called by ccci driver to keep afcdac value sent from modem */
void clk_buf_save_afc_val(unsigned int afcdac)
{
	/* stock returns here for a PMIC clock buffer */
}

bool is_clk_buf_from_pmic(void)
{
	return true;
}
