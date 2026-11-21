/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6771 TOPRGU calls for SPM deep suspend (drivers/watchdog/mtk_wdt.c), T's
 * wd_api spmwdt_mode_config and suspend_notify and their resume pair.
 */
#ifndef __MTK_WDT_H__
#define __MTK_WDT_H__

#include <linux/errno.h>

#if IS_BUILTIN(CONFIG_MEDIATEK_WATCHDOG)
int mtk_wdt_spm_suspend(void);
void mtk_wdt_spm_resume(void);
#else
static inline int mtk_wdt_spm_suspend(void) { return -ENODEV; }
static inline void mtk_wdt_spm_resume(void) { }
#endif

#endif
