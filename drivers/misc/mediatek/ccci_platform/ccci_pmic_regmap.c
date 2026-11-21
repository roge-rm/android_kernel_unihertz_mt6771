// SPDX-License-Identifier: GPL-2.0
/*
 * The MT6358 regmap for the eccci platform calls. 4.14 wrote these PMIC
 * registers with pmic_config_interface; U has no such API, so the writes
 * go through the regmap the MT6358 mfd uses, as the connectivity adapter
 * (0040) and the audio codec (0044) do.
 */

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/mt6397/core.h>

#include "ccci_platform.h"

static struct regmap *pmic_map;

struct regmap *ccci_pmic_regmap(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct mt6397_chip *chip;
	struct regmap *map = READ_ONCE(pmic_map);

	if (map)
		return map;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	chip = platform_get_drvdata(pdev);
	if (chip)
		map = chip->regmap;
	put_device(&pdev->dev);
	if (map)
		WRITE_ONCE(pmic_map, map);
	return map;
}
