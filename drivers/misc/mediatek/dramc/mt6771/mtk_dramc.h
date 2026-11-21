/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the 4.14 display port: the DRAMC driver is left out. The stock
 * kernel log on this phone reports dram type 3 (LPDDR4X), 2 channels and a
 * 3600 data rate, so dram_steps_freq() returns T's LPDDR4X 3600 table
 * (dramc/mt6771/mtk_dramc.c, lp4_highfreq_3600 set).
 */
#ifndef __MTK_DRAMC_H__
#define __MTK_DRAMC_H__

enum DDRTYPE {
	TYPE_LPDDR3 = 1,
	TYPE_LPDDR4,
	TYPE_LPDDR4X
};

static inline int dram_steps_freq(unsigned int step)
{
	switch (step) {
	case 0:
		return 3600;
	case 1:
	case 2:
		return 3200;
	case 3:
		return 1600;
	default:
		return -1;
	}
}

/* DISP's OVL golden settings and HRT layering table pick by these. */
static inline int get_ddr_type(void)
{
	return TYPE_LPDDR4X;
}

static inline int get_emi_ch_num(void)
{
	return 2;
}

#endif /* __MTK_DRAMC_H__ */
