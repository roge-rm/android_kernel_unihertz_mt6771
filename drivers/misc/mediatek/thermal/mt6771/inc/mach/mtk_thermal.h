/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the 4.14 GPU port: the MTK thermal driver is left out, so
 * gpufreq takes its fixed 40 C branches (MT_GPUFREQ_STOCK_THERMAL is 0) and
 * registers no power table with thermal. It keeps T's includes, which give
 * gpufreq the DRV_Reg32 register macros.
 */
#ifndef __MTK_THERMAL_H__
#define __MTK_THERMAL_H__

#include <linux/io.h>
#include "mt-plat/sync_write.h"
#include "mtk_thermal_typedefs.h"
#include "mtk_gpufreq.h"
#endif /* __MTK_THERMAL_H__ */
