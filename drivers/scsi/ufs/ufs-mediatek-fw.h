/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UFS_MEDIATEK_FW_H
#define _UFS_MEDIATEK_FW_H

#include <linux/errno.h>
#include <linux/types.h>

/* These command numbers belong to SiP service 0x276, not service 0x270.
 * MT6771 crypto uses a separate service, and reference-clock control uses
 * its PMIC clock-buffer driver. Neither is implemented by aliasing 0x276.
 */
static inline int ufs_mtk_fw_validate(bool mt6771, u32 command, u32 value)
{
	if (value > 1)
		return -EINVAL;

	switch (command) {
	case 1: /* VA09 power control */
	case 2: /* device reset */
		return 0;
	case 4: /* newer firmware crypto control */
	case 8: /* newer firmware reference-clock notification */
		return mt6771 ? -EOPNOTSUPP : 0;
	default:
		return -EINVAL;
	}
}

/* The installed MT6771 ATF dispatch sign-extends its int result into x0.
 * Accept both 32-bit and sign-extended SiP error encodings, never extra data.
 */
static inline int ufs_mt6771_fw_result(u64 result)
{
	u32 value = (u32)result;

	if (result != (u64)value && result != (u64)(s64)(s32)value)
		return -EIO;
	switch (value) {
	case 0:
		return 0;
	case 0xffffffffU:
		return -EOPNOTSUPP;
	case 0xfffffffeU:
		return -EINVAL;
	case 0xfffffffdU:
		return -ERANGE;
	case 0xfffffffcU:
		return -EACCES;
	default:
		return -EIO;
	}
}

#endif
