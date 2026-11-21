/* SPDX-License-Identifier: GPL-2.0 */
/* Stub for the 4.14 display port: no ram console, the aee_rr calls do nothing. */

#ifndef __MTK_RAM_CONSOLE_H__
#define __MTK_RAM_CONSOLE_H__

#include <linux/types.h>

static inline void aee_rr_rec_clk(int id, u32 val) {}
static inline unsigned long *aee_rr_rec_mtk_cpuidle_footprint_va(void) { return NULL; }
static inline unsigned long *aee_rr_rec_mtk_cpuidle_footprint_pa(void) { return NULL; }
static inline void aee_rr_rec_deepidle_val(u32 val) {}
static inline u32 aee_rr_curr_deepidle_val(void) { return 0; }
static inline void aee_rr_rec_spm_suspend_val(u32 val) {}
static inline u32 aee_rr_curr_spm_suspend_val(void) { return 0; }
static inline void aee_rr_rec_vcore_dvfs_status(u32 val) {}
static inline void aee_rr_rec_gpu_dvfs_vgpu(u8 val) {}
static inline void aee_rr_rec_gpu_dvfs_oppidx(u8 val) {}
static inline void aee_rr_rec_gpu_dvfs_status(u8 val) {}
static inline u8 aee_rr_curr_gpu_dvfs_status(void) { return 0; }

#endif
