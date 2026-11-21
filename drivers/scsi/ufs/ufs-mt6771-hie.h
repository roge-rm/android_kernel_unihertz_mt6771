/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UFS_MT6771_HIE_H
#define _UFS_MT6771_HIE_H
#include <linux/errno.h>
#include <linux/types.h>

/* Wire-plan only, not a registered keyslot callback. All words are CPU endian.
 * Preserve the BSP's final metadata word (17 input words, not just 16 key words).
 * Caller must validate actual UFS capability ordering and wipe this plan after use.
 */
static inline int ufs_mt6771_hie_plan(const u32 words[17], int slot,
				    unsigned int bsp_capability_index, bool enable,
				    u32 requests[6][3])
{
	unsigned int i, row;
	u32 mode;

	if (slot < 0 || slot > 255 || bsp_capability_index > 1)
		return -EINVAL;
	mode = bsp_capability_index ? (1U << 5) : (1U << 3);
	requests[0][0] = ((u32)slot << 24) | (mode << 16) | (0x40U << 8) |
			 (!enable);
	requests[0][1] = words[0];
	requests[0][2] = words[1];
	for (i = 2, row = 1; i < 16; i += 3, row++) {
		requests[row][0] = words[i];
		requests[row][1] = words[i + 1];
		requests[row][2] = words[i + 2];
	}
	return 0;
}
#endif
