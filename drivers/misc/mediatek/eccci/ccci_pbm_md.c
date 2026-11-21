// SPDX-License-Identifier: GPL-2.0
/*
 * PBM's MD section level setup. Stock MT6771 builds pbm_v3 with
 * DISABLE_PBM_FEATURE, where this is empty, as is kicker_pbm_by_md
 * (the logging stub in ccci_platform).
 */
#include <linux/types.h>
#include <mach/mtk_pbm.h>

void init_md_section_level(enum pbm_kicker kicker)
{
}
