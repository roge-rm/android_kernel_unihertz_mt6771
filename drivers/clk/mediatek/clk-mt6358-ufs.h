/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CLK_MT6358_UFS_H
#define __CLK_MT6358_UFS_H

/* MT6771 BSP upmu_hw.h: EXTBUF7 mode[12:11], enable[13]. */
#define MT6358_UFS_CW11 0x7a2
#define MT6358_UFS_CW11_SET 0x7a4
#define MT6358_UFS_CW11_CLR 0x7a6
#define MT6358_UFS_MODE_MASK (3U << 11)
#define MT6358_UFS_ENABLE (1U << 13)

/* Sleepable regmap transaction. Never take over a hardware-controlled buffer. */
static int mt6358_ufs_set(struct regmap *map, bool on)
{
	unsigned int value;
	int ret;

	ret = regmap_read(map, MT6358_UFS_CW11, &value);
	if (ret)
		return ret;
	if (value & MT6358_UFS_MODE_MASK)
		return -EOPNOTSUPP;

	ret = regmap_write(map, on ? MT6358_UFS_CW11_SET :
				    MT6358_UFS_CW11_CLR, MT6358_UFS_ENABLE);
	if (ret)
		return ret;
	if (on)
		usleep_range(400, 450);
	return 0;
}
#endif
