/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the 4.14 display port: vcorefs_v3 (Vcore DVFS) is left out, so
 * every request fails the way T's does when vcorefs can't work. mmdvfs logs
 * "Set vcore step failed" and carries on, as in T.
 */
#ifndef _MTK_VCOREFS_MANAGER_H
#define _MTK_VCOREFS_MANAGER_H

#include <linux/errno.h>
#include "mtk_vcorefs_governor.h"

static inline int is_vcorefs_can_work(void)
{
	return 0;
}

#ifdef CONFIG_MTK_CCCI_PLATFORM_DEPS
/* Logs once per kicker and fails the same way (drivers/misc/mediatek/ccci_platform). */
extern int vcorefs_request_dvfs_opp(enum dvfs_kicker, enum dvfs_opp);
#else
static inline int vcorefs_request_dvfs_opp(enum dvfs_kicker kicker,
					   enum dvfs_opp opp)
{
	return -ENODEV;
}
#endif

#endif	/* _MTK_VCOREFS_MANAGER_H */
