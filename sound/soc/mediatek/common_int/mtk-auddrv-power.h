/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MTK_AUDDRV_POWER_H
#define _MTK_AUDDRV_POWER_H

#include <linux/types.h>

/*
 * True while the AFE holds scp_sys_audio (the SYS_AUDIO MTCMOS) with the infra audio bus
 * clock and aud_afe on, from AudDrv_Clk_On to AudDrv_Clk_Off. Audsys registers (0x11220000)
 * hang the bus with it off. In mtk-auddrv-clk.c.
 */
bool AudDrv_Clk_Power_On(void);

/*
 * AudDrv_Clk_Power_Lock takes the power lock and returns AudDrv_Clk_Power_On(); the power
 * can't go off until AudDrv_Clk_Power_Unlock. Hold it only across one register access, any
 * context.
 */
bool AudDrv_Clk_Power_Lock(unsigned long *flags);
void AudDrv_Clk_Power_Unlock(unsigned long flags);

/*
 * Hands the AFE IRQ (requested with IRQ_NOAUTOEN) to the clock code, which keeps it enabled
 * only while the power is on.
 */
void AudDrv_Clk_Set_Irq(int irq);

#endif
