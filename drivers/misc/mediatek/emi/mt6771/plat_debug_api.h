/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_drv.c includes this with CONFIG_MTK_EMI. Stock's EMI debug driver (the
 * outstanding transaction dump on an SMI hang) isn't ported, so the dump is a
 * no-op.
 */
#ifndef __PLAT_DEBUG_API_H__
#define __PLAT_DEBUG_API_H__

static inline void dump_emi_outstanding(void)
{
}

#endif /* __PLAT_DEBUG_API_H__ */
