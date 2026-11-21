/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Yong Wu <yong.wu@mediatek.com>
 */
#ifndef MTK_IOMMU_SMI_H
#define MTK_IOMMU_SMI_H

#include <linux/bitops.h>
#include <linux/device.h>

#if IS_ENABLED(CONFIG_MTK_SMI)

#define MTK_SMI_MMU_EN(port)	BIT(port)

struct mtk_smi_larb_iommu {
	struct device *dev;
	unsigned int   mmu;
	unsigned char  bank[32];
};

/*
 * mtk_smi_larb_get: Enable the power domain and clocks for this local arbiter.
 *                   It also initialize some basic setting(like iommu).
 * mtk_smi_larb_put: Disable the power domain and clocks for this local arbiter.
 * Both should be called in non-atomic context.
 *
 * Returns 0 if successful, negative on failure.
 */
int mtk_smi_larb_get(struct device *larbdev);
void mtk_smi_larb_put(struct device *larbdev);

#else

static inline int mtk_smi_larb_get(struct device *larbdev)
{
	return 0;
}

static inline void mtk_smi_larb_put(struct device *larbdev) { }

#endif

#if IS_ENABLED(CONFIG_MTK_SMI_EXT)
/* The 4.14 MediaTek SMI EXT interface (drivers/memory/mtk-smi.c, misc/mediatek/smi). */
#include <linux/platform_device.h>

struct mtk_smi_pair {
	u32 off;
	u32 val;
};

struct mtk_smi_dev {
	u32 id;
	struct device *dev;
	void __iomem *base;
	u32 *mmu;

	u32 nr_clks;
	struct clk **clks;
	atomic_t clk_cnts;

	u32 nr_conf_pairs;
	struct mtk_smi_pair *conf_pairs;

	u32 nr_scen_pairs;
	struct mtk_smi_pair **scen_pairs;
};

s32 mtk_smi_clk_enable(struct mtk_smi_dev *smi);
void mtk_smi_clk_disable(struct mtk_smi_dev *smi);

struct mtk_smi_dev *mtk_smi_dev_get(const u32 id);
s32 mtk_smi_conf_set(const struct mtk_smi_dev *smi, const u32 scen_id);

s32 smi_register(void);
s32 smi_larb_port_check(void);
#endif

#endif
