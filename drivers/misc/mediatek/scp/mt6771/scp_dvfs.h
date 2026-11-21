/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#ifndef __SCP_DVFS_H__
#define __SCP_DVFS_H__

#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define PLL_ENABLE				(1)
#define PLL_DISABLE				(0)

enum scp_state_enum {
	IN_DEBUG_IDLE = 1,
	ENTERING_SLEEP = 2,
	IN_SLEEP = 4,
	ENTERING_ACTIVE = 8,
	IN_ACTIVE = 16,
};

/* SCP frequencies in MHz, as the firmware writes them to CURRENT_FREQ_REG */
enum clk_opp_enum {
	CLK_26M	 = 26,
	CLK_OPP0 = 125,
	CLK_OPP1 = 330,
	CLK_OPP2 = 416,
};

struct mt_scp_pll_t {
	struct clk *clk_mux;
	struct clk *clk_pll0;
	struct clk *clk_pll1;
	struct clk *clk_pll2;
	struct clk *clk_pll3;
	struct clk *clk_pll4;
	struct clk *clk_pll5;
	struct clk *clk_pll6;
};

extern int scp_pll_ctrl_set(unsigned int pll_ctrl_flag, unsigned int pll_sel);
extern unsigned int scp_get_dvfs_opp(void);
extern uint32_t scp_get_freq(void);
extern int scp_request_freq(void);
extern bool scp_dvfs_ready(void);
extern int __init scp_dvfs_init(void);

extern unsigned int scp_expected_freq;
extern unsigned int scp_current_freq;
extern spinlock_t scp_awake_spinlock;

#endif  /* __SCP_DVFS_H__ */
