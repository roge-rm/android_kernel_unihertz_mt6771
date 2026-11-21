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

#ifndef __SCP_HELPER_H__
#define __SCP_HELPER_H__

#include <linux/notifier.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include "scp_reg.h"
#include "scp_feature_define.h"

/* scp config reg. definition*/
#define SCP_TCM_SIZE		(scpreg.total_tcmsize)
#define SCP_A_TCM_SIZE		(scpreg.scp_tcmsize)
#define SCP_TCM			(scpreg.sram)
#define SCP_REGION_INFO_OFFSET 0x400
#define SCP_RTOS_START		(0x800)
#define SCP_A_SHARE_BUFFER	((void __iomem *)(scpreg.sram + \
					SCP_RTOS_START -  SHARE_BUF_SIZE*2))

/* scp dvfs return status flag */
#define SET_PLL_FAIL		(1)
#define SET_PMIC_VOLT_FAIL	(2)

/* This structre need to sync with SCP-side */
struct SCP_IRQ_AST_INFO {
	unsigned int scp_irq_ast_time;
	unsigned int scp_irq_ast_pc_s;
	unsigned int scp_irq_ast_pc_e;
	unsigned int scp_irq_ast_lr_s;
	unsigned int scp_irq_ast_lr_e;
	unsigned int scp_irq_ast_irqd;
};

/* scp notify event */
enum SCP_NOTIFY_EVENT {
	SCP_EVENT_READY = 0,
	SCP_EVENT_STOP,
};

/* scp reset status */
#define SCP_ALL_ENABLE	0x00
#define SCP_ALL_REBOOT	0x01
#define SCP_A_ENABLE	0x10
#define SCP_A_REBOOT	0x11

/* scp semaphore definition*/
enum SEMAPHORE_FLAG {
	SEMAPHORE_CLK_CFG_5 = 0,
	SEMAPHORE_PTP,
	SEMAPHORE_I2C0,
	SEMAPHORE_I2C1,
	SEMAPHORE_TOUCH,
	SEMAPHORE_APDMA,
	SEMAPHORE_SENSOR,
	SEMAPHORE_SCP_A_AWAKE,
	SEMAPHORE_SCP_B_AWAKE,
	NR_FLAG = 9,
};

/* scp reset type, kept for callers; this port has no recovery */
enum SCP_RESET_TYPE {
	RESET_TYPE_WDT = 0,
	RESET_TYPE_AWAKE = 1,
	RESET_TYPE_CMD = 2,
};

/* u8 so the register macros add byte offsets without void pointer arithmetic */
struct scp_regs {
	u8 __iomem *scpsys;
	u8 __iomem *sram;
	u8 __iomem *cfg;
	u8 __iomem *clkctrl;
	u8 __iomem *l1cctrl;
	int irq;
	unsigned int total_tcmsize;
	unsigned int cfgregsize;
	unsigned int scp_tcmsize;
};

/* scp work struct definition*/
struct scp_work_struct {
	struct work_struct work;
	unsigned int flags;
	unsigned int id;
};

/* scp reserve memory ID definition, in stock order */
enum scp_reserve_mem_id_t {
#ifdef CONFIG_MTK_VOW_SUPPORT
	VOW_MEM_ID,
#endif
	SENS_MEM_ID,
#ifdef CONFIG_MTK_AUDIO_TUNNELING_SUPPORT
	MP3_MEM_ID,
#endif
	FLP_MEM_ID,
	SCP_A_LOGGER_MEM_ID,
	AUDIO_IPI_MEM_ID,
#ifdef CONFIG_SND_SOC_MTK_SCP_SMARTPA
	SPK_PROTECT_MEM_ID,
#endif
#ifdef CONFIG_MTK_VOW_BARGE_IN_SUPPORT
	VOW_BARGEIN_MEM_ID,
#endif
	NUMS_MEM_ID,
};

struct scp_reserve_mblock {
	enum scp_reserve_mem_id_t num;
	u64 start_phys;
	u64 start_virt;
	u64 size;
};

/* The region info LK and the SCP loader leave at SCP_REGION_INFO_OFFSET. */
struct scp_region_info_st {
	uint32_t ap_loader_start;
	uint32_t ap_loader_size;
	uint32_t ap_firmware_start;
	uint32_t ap_firmware_size;
	uint32_t ap_dram_start;
	uint32_t ap_dram_size;
	uint32_t ap_dram_backup_start;
	uint32_t struct_size;
	uint32_t scp_log_thru_ap_uart;
	uint32_t TaskContext_ptr;
	uint32_t Il1c_con;
	uint32_t Dl1c_con;
	uint32_t scpctl;
	uint32_t ap_params_start;
};

extern struct scp_regs scpreg;
extern struct scp_region_info_st scp_region_info_copy;

extern irqreturn_t scp_A_irq_handler(int irq, void *dev_id);
extern void scp_A_irq_init(void);
extern void scp_A_ipi_init(void);
extern void scp_A_register_notify(struct notifier_block *nb);
extern void scp_A_unregister_notify(struct notifier_block *nb);
extern void scp_schedule_work(struct scp_work_struct *scp_ws);
extern int get_scp_semaphore(int flag);
extern int release_scp_semaphore(int flag);
extern void memcpy_to_scp(void __iomem *trg,
		const void *src, int size);
extern void memcpy_from_scp(void *trg, const void __iomem *src,
		int size);
extern int reset_scp(int reset);
extern phys_addr_t scp_get_reserve_mem_phys(enum scp_reserve_mem_id_t id);
extern phys_addr_t scp_get_reserve_mem_virt(enum scp_reserve_mem_id_t id);
extern phys_addr_t scp_get_reserve_mem_size(enum scp_reserve_mem_id_t id);
extern int scp_check_resource(void);

extern phys_addr_t scp_mem_base_phys;
extern phys_addr_t scp_mem_base_virt;
extern phys_addr_t scp_mem_size;

extern void scp_extern_notify(enum SCP_NOTIFY_EVENT notify_status);
extern void scp_mark_stopped(void);
extern void scp_reset_awake_counts(void);
extern void scp_awake_init(void);

#endif
