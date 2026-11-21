// SPDX-License-Identifier: GPL-2.0
/*
 * Logging stubs for the eccci calls into drivers this tree leaves out:
 * PBM (pbm_v3), vcorefs (vcorefs_v3) and AEE. Prototypes as 4.14.
 */

#define pr_fmt(fmt) "ccci_plat: " fmt

#include <linux/bitops.h>
#include <linux/kernel.h>

#include <mach/mtk_pbm.h>
#include <mtk_vcorefs_manager.h>
#include <mt-plat/aee.h>

/* PBM budgets MD1's power against the CPU and GPU; without pbm_v3 nothing is budgeted. */
void kicker_pbm_by_md(enum pbm_kicker kicker, bool status)
{
	pr_info("%s(%d, %d): no PBM\n", __func__, kicker, status);
}

/*
 * No Vcore DVFS: every request fails with -ENODEV as the inline stub this
 * replaces did, so mmdvfs and audio see what they saw before. Logged once
 * per kicker; mmdvfs and audio call this often.
 */
static unsigned long vcorefs_logged;

int vcorefs_request_dvfs_opp(enum dvfs_kicker kicker, enum dvfs_opp opp)
{
	if ((unsigned int)kicker < BITS_PER_LONG &&
	    !test_and_set_bit(kicker, &vcorefs_logged))
		pr_info("%s(%d, %d): no vcorefs, -ENODEV (logged once per kicker)\n",
			__func__, kicker, opp);
	return -ENODEV;
}
EXPORT_SYMBOL(vcorefs_request_dvfs_opp);

/* Modem exception report; no AEE, so only the detail line is kept, in dmesg. */
void aed_md_exception_api(const int *log, int log_size, const int *phy,
			  int phy_size, const char *detail, const int db_opt)
{
	pr_err("%s: no AEE, modem exception not saved (log %d, phy %d bytes): %s\n",
	       __func__, log_size, phy_size, detail ? detail : "");
}
EXPORT_SYMBOL(aed_md_exception_api);
