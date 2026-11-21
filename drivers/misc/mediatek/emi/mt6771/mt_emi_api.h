/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the 4.14 display port: the EMI driver is left out. The stock
 * kernel log on this phone reports "[EMI] dram_type(3)", TYPE_LPDDR4X.
 *
 * The EMI MPU part is T's mt_emi_api.h and submodule/mpu_v1.h as they are, for
 * emi/mt6771/emi_mpu.c and the callers that set regions (SCP and the connsys
 * modules, with CONFIG_MTK_EMI).
 */
#ifndef __MT_EMI_API_H__
#define __MT_EMI_API_H__

/* Only files with dramc/mt6771 on their include path (smi) use the DRAM type */
#if __has_include(<mtk_dramc.h>)
#include <mtk_dramc.h>

static inline unsigned int get_dram_type(void)
{
	return TYPE_LPDDR4X;
}
#endif

/* EMI MPU: 32 regions, 16 domains, 64 KB granules */
#define EMI_MPU_DOMAIN_NUM	16
#define EMI_MPU_REGION_NUM	32
#define EMI_MPU_ALIGN_BITS	16
#define DRAM_OFFSET		0x40000000

#define NO_PROTECTION	0
#define SEC_RW		1
#define SEC_RW_NSEC_R	2
#define SEC_RW_NSEC_W	3
#define SEC_R_NSEC_R	4
#define FORBIDDEN	5
#define SEC_R_NSEC_RW	6

#define UNLOCK		0
#define LOCK		1

#define EMI_MPU_DGROUP_NUM	(EMI_MPU_DOMAIN_NUM / 8)

/* apc_ary[1] holds domains 15 to 8, apc_ary[0] domains 7 to 0 and the lock bit */
#define SET_ACCESS_PERMISSION(apc_ary, lock, \
	d15, d14, d13, d12, d11, d10, d9, d8, d7, d6, d5, d4, d3, d2, d1, d0) \
do { \
	apc_ary[1] = \
		(((unsigned int) d15) << 21) | (((unsigned int) d14) << 18) | \
		(((unsigned int) d13) << 15) | (((unsigned int) d12) << 12) | \
		(((unsigned int) d11) << 9) | (((unsigned int) d10) << 6) | \
		(((unsigned int) d9) << 3) | ((unsigned int) d8); \
	apc_ary[0] = \
		(((unsigned int) d7) << 21) | (((unsigned int) d6) << 18) | \
		(((unsigned int) d5) << 15) | (((unsigned int) d4) << 12) | \
		(((unsigned int) d3) << 9) | (((unsigned int) d2) << 6) | \
		(((unsigned int) d1) << 3) | ((unsigned int) d0) | \
		((unsigned int) lock << 31); \
} while (0)

struct emi_region_info_t {
	unsigned long long start;
	unsigned long long end;
	unsigned int region;
	unsigned int apc[EMI_MPU_DGROUP_NUM];
};

extern int emi_mpu_set_protection(struct emi_region_info_t *region_info);

#endif /* __MT_EMI_API_H__ */
