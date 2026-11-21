// SPDX-License-Identifier: GPL-2.0
/* MT6358 EXTBUF7 provider for the MT6771 UFS reference clock. */
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include "clk-mt6358-ufs.h"

struct mt6358_ufs_clk {
	struct clk_hw hw;
	struct regmap *map;
	struct device *dev;
};
#define to_mt6358_ufs(hw) container_of(hw, struct mt6358_ufs_clk, hw)

static int mt6358_ufs_prepare(struct clk_hw *hw)
{
	return mt6358_ufs_set(to_mt6358_ufs(hw)->map, true);
}

static void mt6358_ufs_unprepare(struct clk_hw *hw)
{
	struct mt6358_ufs_clk *clk = to_mt6358_ufs(hw);
	int ret = mt6358_ufs_set(clk->map, false);

	/* CCF unprepare cannot return an error. Keep failures visible. */
	if (ret)
		dev_err(clk->dev, "EXTBUF7 disable failed: %d\n", ret);
}

static int mt6358_ufs_is_prepared(struct clk_hw *hw)
{
	unsigned int value;
	int ret = regmap_read(to_mt6358_ufs(hw)->map, MT6358_UFS_CW11, &value);

	return ret ? ret : !!(value & MT6358_UFS_ENABLE);
}

static const struct clk_ops mt6358_ufs_ops = {
	.prepare = mt6358_ufs_prepare,
	.unprepare = mt6358_ufs_unprepare,
	.is_prepared = mt6358_ufs_is_prepared,
};

static int mt6358_ufs_clk_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct clk_init_data init = {};
	struct mt6358_ufs_clk *clk;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;
	if (!chip || !chip->regmap)
		return -EPROBE_DEFER;
	clk = devm_kzalloc(&pdev->dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return -ENOMEM;
	init.name = dev_name(&pdev->dev);
	init.ops = &mt6358_ufs_ops;
	/* Preserve bootloader-on clock until a qualified consumer owns it. */
	init.flags = CLK_IGNORE_UNUSED;
	clk->hw.init = &init;
	clk->map = chip->regmap;
	clk->dev = &pdev->dev;
	ret = devm_clk_hw_register(&pdev->dev, &clk->hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get,
					 &clk->hw);
}

static const struct of_device_id mt6358_ufs_clk_of_match[] = {
	{ .compatible = "mediatek,mt6358-ufs-clock" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6358_ufs_clk_of_match);
static struct platform_driver mt6358_ufs_clk_driver = {
	.probe = mt6358_ufs_clk_probe,
	.driver = {
		.name = "mt6358-ufs-clock",
		.of_match_table = mt6358_ufs_clk_of_match,
	},
};
module_platform_driver(mt6358_ufs_clk_driver);
MODULE_DESCRIPTION("MT6358 UFS EXTBUF7 clock provider");
MODULE_LICENSE("GPL");
