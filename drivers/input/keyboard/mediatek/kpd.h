/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2010 MediaTek Inc.
 *
 * MediaTek keypad (mtk-kpd), ported from the Titan Pocket 4.14 vendor kernel.
 */

#ifndef __KPD_H__
#define __KPD_H__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <hal_kpd.h>

struct keypad_dts_data {
	u32 kpd_key_debounce;
	u32 kpd_sw_pwrkey;
	u32 kpd_hw_pwrkey;
	u32 kpd_sw_rstkey;
	u32 kpd_hw_rstkey;
	u32 kpd_use_extend_type;
	u32 kpd_hw_map_num;
	u32 kpd_hw_init_map[KPD_NUM_KEYS];
	u32 kpd_hw_dl_key1;
	u32 kpd_hw_dl_key2;
	u32 kpd_hw_dl_key3;
	u32 kpd_hw_recovery_key;
	u32 kpd_hw_factory_key;
};

extern struct keypad_dts_data kpd_dts_data;
extern int kpd_klog_en;

#define KPD_SAY		"kpd: "

#define kpd_print(fmt, arg...)	do { \
	if (kpd_klog_en) \
		pr_debug(KPD_SAY fmt, ##arg); \
	} while (0)
#define kpd_info(fmt, arg...)	do { \
	if (kpd_klog_en) \
		pr_info(KPD_SAY fmt, ##arg); \
	} while (0)
#define kpd_notice(fmt, arg...)	pr_notice(KPD_SAY fmt, ##arg)

#ifdef CONFIG_KPD_PWRKEY_USE_PMIC
/* MT6358 PWRKEY and HOMEKEY (kpd-mt6358.c) */
void kpd_pwrkey_pmic_handler(unsigned long pressed);
void kpd_pmic_rstkey_handler(unsigned long pressed);
int kpd_mt6358_lookup(void);
int kpd_mt6358_init(struct device *dev);
int kpd_mt6358_set_lprst(unsigned int pwrkey_en, unsigned int homekey_en,
			 int td);
#endif

#endif				/* __KPD_H__ */
