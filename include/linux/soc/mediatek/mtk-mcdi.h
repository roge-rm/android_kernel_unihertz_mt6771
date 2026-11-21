/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6771 MCDI client (drivers/misc/mediatek/mcdi_sspm) calls for SPM suspend.
 */
#ifndef __MTK_MCDI_H__
#define __MTK_MCDI_H__

#include <linux/errno.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_MTK_MCDI_SSPM)
int mtk_mcdi_task_pause(bool paused);
#else
static inline int mtk_mcdi_task_pause(bool paused) { return 0; }
#endif

#endif
