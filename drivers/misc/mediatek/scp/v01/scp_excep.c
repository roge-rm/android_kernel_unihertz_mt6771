/*
 * Copyright (C) 2016 MediaTek Inc.
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

/*
 * SCP exceptions, cut down to the kernel log: the type, PC, LR and the
 * status registers. No AED and no SRAM or L1C dump.
 */

#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include "scp_ipi.h"
#include "scp_helper.h"
#include "scp_excep.h"

static struct scp_work_struct scp_aed_work;
static DEFINE_MUTEX(scp_excep_mutex);

/*
 * return last lr for debugging
 */
uint32_t scp_dump_lr(void)
{
	if (is_scp_ready(SCP_A_ID))
		return readl(SCP_A_DEBUG_LR_REG);
	else
		return 0xFFFFFFFF;
}

/*
 * return last pc for debugging
 */
uint32_t scp_dump_pc(void)
{
	if (is_scp_ready(SCP_A_ID))
		return readl(SCP_A_DEBUG_PC_REG);
	else
		return 0xFFFFFFFF;
}

/*
 * dump scp register for debugging. Writes the BUS_CTRL debug select to
 * read the four bus latches, as stock.
 */
void scp_A_dump_regs(void)
{
	uint32_t tmp, latch[4];
	int i;

	pr_notice("[SCP]%s PC:0x%x,LR:0x%x,PSP:0x%x,SP:0x%x\n",
		is_scp_ready(SCP_A_ID) ? "ready" : "not ready",
		readl(SCP_A_DEBUG_PC_REG), readl(SCP_A_DEBUG_LR_REG),
		readl(SCP_A_DEBUG_PSP_REG), readl(SCP_A_DEBUG_SP_REG));
	pr_notice("[SCP]GIPC 0x%x BUS_CTRL 0x%x SLEEP_STATUS 0x%x INFRA_STATUS 0x%x\n",
		readl(SCP_GIPC_IN_REG), readl(SCP_BUS_CTRL),
		readl(SCP_CPU_SLEEP_STATUS), readl(INFRA_CTRL_STATUS));
	pr_notice("[SCP]IRQ STATUS 0x%x ENABLE 0x%x SLEEP 0x%x, MSB 0x%x 0x%x 0x%x\n",
		readl(SCP_INTC_IRQ_STATUS), readl(SCP_INTC_IRQ_ENABLE),
		readl(SCP_INTC_IRQ_SLEEP), readl(SCP_INTC_IRQ_STATUS_MSB),
		readl(SCP_INTC_IRQ_ENABLE_MSB), readl(SCP_INTC_IRQ_SLEEP_MSB));
	pr_notice("[SCP]CLK_SW_SEL 0x%x CLK_ENABLE 0x%x SLEEP_DEBUG 0x%x WDT 0x%x\n",
		readl(SCP_CLK_SW_SEL), readl(SCP_CLK_ENABLE),
		readl(SCP_A_SLEEP_DEBUG_REG), readl(SCP_A_WDT_REG));

	tmp = readl(SCP_BUS_CTRL)&(~dbg_irq_info_sel_mask);
	for (i = 0; i < 4; i++) {
		writel(tmp | (i << dbg_irq_info_sel_shift), SCP_BUS_CTRL);
		latch[i] = readl(SCP_DEBUG_IRQ_INFO);
	}
	pr_notice("[SCP]BUS latch INFRA 0x%x DCACHE 0x%x ICACHE 0x%x PC 0x%x\n",
		latch[0], latch[1], latch[2], latch[3]);
}

static const char *const scp_excep_names[SCP_NR_EXCEP] = {
	[EXCEP_LOAD_FIRMWARE] = "load firmware",
	[EXCEP_RESET] = "reset",
	[EXCEP_BOOTUP] = "boot",
	[EXCEP_RUNTIME] = "runtime",
};

/*
 * log an exception with the SCP state. May block.
 * @param type: exception type
 */
void scp_aed(enum scp_excep_id type, enum scp_core_id id)
{
	mutex_lock(&scp_excep_mutex);
	pr_notice("[SCP] %s %s exception\n", core_ids[id],
		type < SCP_NR_EXCEP ? scp_excep_names[type] : "unknown");
	if (type != EXCEP_LOAD_FIRMWARE)
		scp_A_dump_regs();
	scp_ipi_status_dump();
	mutex_unlock(&scp_excep_mutex);
}

static void scp_aed_reset_ws(struct work_struct *ws)
{
	struct scp_work_struct *sws = container_of(ws,
			struct scp_work_struct, work);

	scp_aed((enum scp_excep_id) sws->flags, (enum scp_core_id) sws->id);
}

/*
 * schedule a work to log an exception; safe in interrupt context.
 * Nothing is reset: that is the recovery this port leaves out.
 * @param type: exception type
 */
void scp_aed_reset(enum scp_excep_id type, enum scp_core_id id)
{
	scp_aed_work.flags = (unsigned int) type;
	scp_aed_work.id = (unsigned int) id;
	scp_schedule_work(&scp_aed_work);
}

int scp_excep_init(void)
{
	INIT_WORK(&scp_aed_work.work, scp_aed_reset_ws);
	return 0;
}
