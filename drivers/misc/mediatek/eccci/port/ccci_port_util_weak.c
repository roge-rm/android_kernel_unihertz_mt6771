// SPDX-License-Identifier: GPL-2.0
/*
 * Weak stand-in for the one ccci_util call the port layer adds
 * (port_ipc's CCCI_IPC_WAIT_TIME_UPDATE). ccci_util
 * overrides it at link time. Returns -1 as 4.14 does before its time
 * API is set up.
 */
#include <linux/compiler.h>
#include <mt-plat/mtk_ccci_common.h>

int __weak wait_time_update_notify(void)
{
	return -1;
}
