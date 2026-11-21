/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2016 MediaTek Inc.
 */

#ifndef __SPM_V4__MTK_SPM_RESOURCE_REQ_H__
#define __SPM_V4__MTK_SPM_RESOURCE_REQ_H__

#include <linux/types.h>

enum {
	SPM_RESOURCE_RELEASE = 0,
	SPM_RESOURCE_MAINPLL = 1 << 0,
	SPM_RESOURCE_DRAM    = 1 << 1,
	SPM_RESOURCE_CK_26M  = 1 << 2,
	SPM_RESOURCE_AXI_BUS = 1 << 3,
	SPM_RESOURCE_CPU     = 1 << 4,
	NF_SPM_RESOURCE = 5,

	SPM_RESOURCE_ALL = (1 << NF_SPM_RESOURCE) - 1
};

enum {
	SPM_RESOURCE_USER_SPM = 0,
	SPM_RESOURCE_USER_UFS,
	SPM_RESOURCE_USER_SSUSB,
	SPM_RESOURCE_USER_AUDIO,
	SPM_RESOURCE_USER_UART,
	SPM_RESOURCE_USER_CONN,
	SPM_RESOURCE_USER_MSDC,
	SPM_RESOURCE_USER_SCP,
	NF_SPM_RESOURCE_USER
};

/*
 * Stub. Stock records which users need the main PLL, DRAM, 26 MHz, bus and CPU kept up
 * through SPM deep idle and SODI. 5.10 has no SPM idle (spm_v4/mtk_spm_hooks.c only keeps
 * the notifier and flags), so there is nothing to hold and the request always succeeds.
 */
static inline bool spm_resource_req(unsigned int user, unsigned int req_mask)
{
	return user < NF_SPM_RESOURCE_USER;
}

#endif /* __SPM_V4__MTK_SPM_RESOURCE_REQ_H__ */
