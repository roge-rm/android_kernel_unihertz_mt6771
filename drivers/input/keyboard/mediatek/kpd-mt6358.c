// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 MediaTek Inc.
 *
 * MT6358 power and home key interrupts for mtk-kpd, from the 4.14 vendor
 * mt6358 pmic_irq.c (key_int_handler, PMIC_EINT_SETTING), and the
 * long-press reset fields stock writes through pmic_set_register_value.
 *
 * The IRQs come by name from the stock mediatek,mt-pmic node under the
 * MT6358, which is also the interrupt controller (5.10 mt6358-irq.c). The
 * register writes go through the MT6358 mfd's pwrap regmap.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/mt6358/core.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include "kpd.h"

/* MT6358 TOP_RST_MISC, as the 4.14 mt6771 upmu_hw.h */
#define MT6358_TOP_RST_MISC		0x014c
#define RG_HOMEKEY_RST_EN		BIT(8)
#define RG_PWRKEY_RST_EN		BIT(9)
#define RG_PWRKEY_RST_TD_SHIFT		12
#define RG_PWRKEY_RST_TD_MASK		(0x3 << RG_PWRKEY_RST_TD_SHIFT)

static const char * const kpd_mt6358_irq_names[] = {
	"pwrkey", "pwrkey_r", "homekey", "homekey_r",
};

static struct regmap *kpd_mt6358_map;
static int kpd_mt6358_irqs[ARRAY_SIZE(kpd_mt6358_irq_names)];

static irqreturn_t key_int_handler(int irq, void *data)
{
	struct irq_data *d = irq_get_irq_data(irq);

	if (!d)
		return IRQ_HANDLED;

	switch (irqd_to_hwirq(d)) {
	case MT6358_IRQ_PWRKEY:
		kpd_pwrkey_pmic_handler(0x1);
		break;
	case MT6358_IRQ_PWRKEY_R:
		kpd_pwrkey_pmic_handler(0x0);
		break;
	case MT6358_IRQ_HOMEKEY:
		kpd_pmic_rstkey_handler(0x1);
		break;
	case MT6358_IRQ_HOMEKEY_R:
		kpd_pmic_rstkey_handler(0x0);
		break;
	}
	return IRQ_HANDLED;
}

/*
 * Finds the MT6358 regmap and the four key IRQs. -EPROBE_DEFER until the
 * MT6358 mfd has bound and registered its IRQ domain.
 */
int kpd_mt6358_lookup(void)
{
	struct device_node *np, *pmic;
	struct platform_device *pdev;
	struct mt6397_chip *chip;
	int i, irq, ret = 0;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt-pmic");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}
	for (i = 0; i < ARRAY_SIZE(kpd_mt6358_irq_names); i++) {
		irq = of_irq_get_byname(np, kpd_mt6358_irq_names[i]);
		if (irq <= 0) {
			ret = irq ? irq : -ENXIO;
			goto out;
		}
		kpd_mt6358_irqs[i] = irq;
	}

	pmic = of_get_parent(np);
	pdev = pmic ? of_find_device_by_node(pmic) : NULL;
	of_node_put(pmic);
	if (!pdev) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	chip = platform_get_drvdata(pdev);
	if (!chip || !chip->regmap || !chip->irq_domain)
		ret = -EPROBE_DEFER;
	else
		kpd_mt6358_map = chip->regmap;
	put_device(&pdev->dev);
out:
	of_node_put(np);
	return ret;
}

/* Requests the key IRQs as stock's PMIC_EINT_SETTING does. */
int kpd_mt6358_init(struct device *dev)
{
	int i, ret;

	if (!kpd_mt6358_map)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(kpd_mt6358_irq_names); i++) {
		ret = devm_request_threaded_irq(dev, kpd_mt6358_irqs[i], NULL,
				key_int_handler, IRQF_TRIGGER_NONE,
				kpd_mt6358_irq_names[i], NULL);
		if (ret < 0) {
			kpd_notice("request %s irq fail (%d)\n",
				   kpd_mt6358_irq_names[i], ret);
			return ret;
		}
	}
	return 0;
}

/*
 * RG_PWRKEY_RST_EN, RG_HOMEKEY_RST_EN and, when td >= 0, RG_PWRKEY_RST_TD,
 * each as one read-modify-write like pmic_set_register_value.
 */
int kpd_mt6358_set_lprst(unsigned int pwrkey_en, unsigned int homekey_en,
			 int td)
{
	int ret;

	if (!kpd_mt6358_map)
		return -ENODEV;
	ret = regmap_update_bits(kpd_mt6358_map, MT6358_TOP_RST_MISC,
				 RG_PWRKEY_RST_EN,
				 pwrkey_en ? RG_PWRKEY_RST_EN : 0);
	if (!ret)
		ret = regmap_update_bits(kpd_mt6358_map, MT6358_TOP_RST_MISC,
					 RG_HOMEKEY_RST_EN,
					 homekey_en ? RG_HOMEKEY_RST_EN : 0);
	if (!ret && td >= 0)
		ret = regmap_update_bits(kpd_mt6358_map, MT6358_TOP_RST_MISC,
					 RG_PWRKEY_RST_TD_MASK,
					 td << RG_PWRKEY_RST_TD_SHIFT);
	return ret;
}
