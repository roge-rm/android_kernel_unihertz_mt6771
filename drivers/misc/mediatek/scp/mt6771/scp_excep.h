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

#ifndef __SCP_EXCEP_H__
#define __SCP_EXCEP_H__

#include "scp_helper.h"
#include "scp_ipi.h"

/* Exceptions are logged to the kernel log only: no AED, no memory dump. */
enum scp_excep_id {
	EXCEP_LOAD_FIRMWARE = 0,
	EXCEP_RESET,
	EXCEP_BOOTUP,
	EXCEP_RUNTIME,
	SCP_NR_EXCEP,
};

extern void scp_aed(enum scp_excep_id type, enum scp_core_id id);
extern void scp_aed_reset(enum scp_excep_id type, enum scp_core_id id);
extern void scp_A_dump_regs(void);
extern uint32_t scp_dump_pc(void);
extern uint32_t scp_dump_lr(void);
extern int scp_excep_init(void);

#endif
