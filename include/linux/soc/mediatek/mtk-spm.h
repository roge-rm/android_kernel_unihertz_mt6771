/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6771 SPM base driver (drivers/misc/mediatek/spm), the names T's spm_v4
 * mtk_spm.c defines for the vcorefs and sleep code. SIP ids are T's
 * mt-plat/mtk_secure_api.h (arm64), SPM_ARGS ids T's spm_v4/mtk_spm.h.
 */
#ifndef __LINUX_SOC_MEDIATEK_MTK_SPM_H__
#define __LINUX_SOC_MEDIATEK_MTK_SPM_H__

#include <linux/types.h>

#define MTK_SIP_KERNEL_SPM_SUSPEND_ARGS		0xc2000220
#define MTK_SIP_KERNEL_SPM_FIRMWARE_STATUS	0xc2000221
#define MTK_SIP_KERNEL_SPM_IRQ0_HANDLER		0xc2000222
#define MTK_SIP_KERNEL_SPM_VCOREFS_ARGS		0xc2000226
#define MTK_SIP_KERNEL_SPM_ARGS			0xc200022a

/* SIP SPM_ARGS sub-ids (x1) */
#define SPM_ARGS_SPMFW_IDX		0
#define SPM_ARGS_SPMFW_INIT		1
#define SPM_ARGS_SUSPEND		2
#define SPM_ARGS_SUSPEND_FINISH		3
#define SPM_ARGS_SODI			4
#define SPM_ARGS_SODI_FINISH		5
#define SPM_ARGS_DPIDLE			6
#define SPM_ARGS_DPIDLE_FINISH		7
#define SPM_ARGS_PCM_WDT		8

/* PCM firmware index in ATF's spmfw table, T's mt6771 SPMFW_* order */
#define SPMFW_LP4X_2CH_3733		0
#define SPMFW_LP4X_2CH_3200		1
#define SPMFW_LP3_1CH_1866		2
#define SPMFW_LP4_2CH_2400		3

#ifdef CONFIG_MTK_SPM_BASE
/* Mapped mediatek,sleep and mediatek,sleep_reg_md, NULL until the driver probes */
extern void __iomem *spm_base;
extern void __iomem *sleep_reg_md_base;
extern u32 spm_irq_0;
extern int __spmfw_idx;

/* ATF's firmware status (SIP 0xc2000221), read once; 0 means not ready */
int spm_load_firmware_status(void);
#else
static inline int spm_load_firmware_status(void)
{
	return 0;
}
#endif

#ifdef CONFIG_MTK_SPM_VCOREFS
/* True once vcorefs init sent CMD_1; *flag gets that PCM flag */
bool spm_vcorefs_init_done(u32 *flag);
#else
static inline bool spm_vcorefs_init_done(u32 *flag)
{
	return false;
}
#endif

#endif
