/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the 4.14 GPU port: mtk_gpu_bw.c is left out. Stock asks the SSPM
 * for the GPU bandwidth and sends PM_QOS_GPU_MEMORY_BANDWIDTH requests, whose
 * listener is the vcore DVFS QoS, and neither SSPM nor vcorefs is ported.
 * These are T's own bodies for MTK_QOS_SUPPORT off.
 */
#ifndef _MT_GPU_BW_H_
#define _MT_GPU_BW_H_

static inline void mt_gpu_bw_toggle(int i32Restore) { }
static inline void mt_gpu_bw_qos_vcore(unsigned int ui32BW) { }
static inline unsigned int mt_gpu_bw_get_BW(int type) { return -1; }
static inline void mt_gpu_bw_init(void) { }

#endif /* _MT_GPU_BW_H_ */
