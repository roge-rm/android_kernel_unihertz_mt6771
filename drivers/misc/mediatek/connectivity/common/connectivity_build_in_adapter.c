/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

/*
 * The 4.14 connectivity built-in adapter (connadp) on 5.10. The WMT, Wi-Fi,
 * BT, GPS and FM modules call into the kernel only through these exports.
 *
 * Differences from the 4.14 file:
 * - gConEmiPhyBase and gConEmiSize are not defined here. On 4.15 and later
 *   the WMT module defines and exports them itself and reads the consys
 *   node's memory-region (ALLOCATE_CONNSYS_EMI_FROM_KO in
 *   mtk_wcn_consys_hw.c), so a kernel copy would clash at insmod. The
 *   reserved-memory hook stays, only to log where the region landed.
 * - The PMIC calls write the MT6358 through its mfd regmap. Only the flags
 *   the mt6771 platform file uses are mapped.
 * - clk_buf_ctrl is the shim in connectivity_platform_stubs.c.
 * - The MMC, GPIO dump and gps-reserve-memory paths are for external combo
 *   chips and other SoCs; they are stubs that log once.
 */

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[CONNADP]"
#include "connectivity_build_in_adapter.h"

#include <linux/export.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/trace_events.h>

#include <mtk_clkbuf_ctl.h>
#include <upmu_common.h>

#ifndef TASK_STATE_TO_CHAR_STR
#define TASK_STATE_TO_CHAR_STR "RSDTtXZxKWPNn"
#endif

/* Reserved memory: the region is reserved by the 5.10 core; this only logs it, as stock does. */
static int __init reserve_memory_consys_fn(struct reserved_mem *rmem)
{
	pr_info(DFT_TAG "[W]%s: name: %s,base: 0x%llx,size: 0x%llx\n",
		__func__, rmem->name, (unsigned long long)rmem->base,
		(unsigned long long)rmem->size);
	return 0;
}
RESERVEDMEM_OF_DECLARE(reserve_memory_test, "mediatek,consys-reserve-memory",
			reserve_memory_consys_fn);

void connectivity_export_show_stack(struct task_struct *tsk, unsigned long *sp)
{
	show_stack(tsk, sp, KERN_INFO);
}
EXPORT_SYMBOL(connectivity_export_show_stack);

void connectivity_export_tracing_record_cmdline(struct task_struct *tsk)
{
#ifdef CONFIG_TRACING
	tracing_record_cmdline(tsk);
#endif
}
EXPORT_SYMBOL(connectivity_export_tracing_record_cmdline);

/*******************************************************************************
 * Clock Buffer Control
 ******************************************************************************/
#ifdef CONNADP_HAS_CLOCK_BUF_CTRL
void connectivity_export_clk_buf_ctrl(enum clk_buf_id id, bool onoff)
{
	clk_buf_ctrl(id, onoff);
}
EXPORT_SYMBOL(connectivity_export_clk_buf_ctrl);

void connectivity_export_clk_buf_show_status_info(void)
{
	clk_buf_show_status_info();
}
EXPORT_SYMBOL(connectivity_export_clk_buf_show_status_info);

int connectivity_export_clk_buf_get_xo_en_sta(/*enum xo_id id*/ int id)
{
	return clk_buf_get_xo_en_sta(id);
}
EXPORT_SYMBOL(connectivity_export_clk_buf_get_xo_en_sta);
#endif

/*******************************************************************************
 * PMIC
 *
 * The MT6358 regmap from the mfd driver, looked up on first use. Callers are
 * the consys power-on and power-off paths, which can sleep (they call the
 * regulator API next to these writes).
 ******************************************************************************/
static struct regmap *connadp_pmic_map(void)
{
	static struct regmap *map;
	struct device_node *np;
	struct platform_device *pdev;
	struct mt6397_chip *chip;

	if (map)
		return map;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	if (!np)
		goto fail;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		goto fail;
	chip = platform_get_drvdata(pdev);
	if (chip && chip->regmap)
		map = chip->regmap;
	put_device(&pdev->dev);
	if (map)
		return map;
fail:
	pr_warn_once(DFT_TAG "[E]%s: MT6358 regmap not found, PMIC writes dropped\n",
		     __func__);
	return NULL;
}

void connectivity_export_pmic_config_interface(unsigned int RegNum,
		unsigned int val, unsigned int MASK, unsigned int SHIFT)
{
	struct regmap *map = connadp_pmic_map();

	if (map)
		regmap_update_bits(map, RegNum, MASK << SHIFT,
				   (val & MASK) << SHIFT);
}
EXPORT_SYMBOL(connectivity_export_pmic_config_interface);

void connectivity_export_pmic_read_interface(unsigned int RegNum,
		unsigned int *val, unsigned int MASK, unsigned int SHIFT)
{
	struct regmap *map = connadp_pmic_map();
	unsigned int reg = 0;

	if (map)
		regmap_read(map, RegNum, &reg);
	*val = (reg >> SHIFT) & MASK;
}
EXPORT_SYMBOL(connectivity_export_pmic_read_interface);

/*
 * The flags the mt6771 platform file passes (mt6771.c, consys_vcn28_hw_mode_ctrl
 * and the VCN33 hw mode paths), with addresses from mach/upmu_hw.h. The flag
 * values are this tree's enum, which the rebuilt modules compile against.
 */
struct connadp_pmic_flag {
	int flag;
	unsigned int addr;
	unsigned int mask;
	unsigned int shift;
};

#define CONNADP_FLAG(name) { name, name##_ADDR, name##_MASK, name##_SHIFT }
static const struct connadp_pmic_flag connadp_pmic_flags[] = {
	CONNADP_FLAG(PMIC_RG_LDO_VCN28_HW0_OP_EN),
	CONNADP_FLAG(PMIC_RG_LDO_VCN28_HW0_OP_CFG),
	CONNADP_FLAG(PMIC_RG_LDO_VCN33_HW0_OP_EN),
	CONNADP_FLAG(PMIC_RG_LDO_VCN33_HW0_OP_CFG),
};

static const struct connadp_pmic_flag *connadp_find_flag(int flagname)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(connadp_pmic_flags); i++)
		if (connadp_pmic_flags[i].flag == flagname)
			return &connadp_pmic_flags[i];
	pr_warn_once(DFT_TAG "[E]%s: PMIC flag %d not mapped, dropped\n",
		     __func__, flagname);
	return NULL;
}

void connectivity_export_pmic_set_register_value(int flagname, unsigned int val)
{
	const struct connadp_pmic_flag *f = connadp_find_flag(flagname);

	if (f)
		connectivity_export_pmic_config_interface(f->addr, val,
							  f->mask, f->shift);
}
EXPORT_SYMBOL(connectivity_export_pmic_set_register_value);

unsigned short connectivity_export_pmic_get_register_value(int flagname)
{
	const struct connadp_pmic_flag *f = connadp_find_flag(flagname);
	unsigned int val = 0;

	if (f)
		connectivity_export_pmic_read_interface(f->addr, &val,
							f->mask, f->shift);
	return val;
}
EXPORT_SYMBOL(connectivity_export_pmic_get_register_value);

/* Whole 16-bit register write, as 4.14 upmu_set_reg_value (mask 0xffff, shift 0). */
void connectivity_export_upmu_set_reg_value(unsigned int reg,
		unsigned int reg_val)
{
	connectivity_export_pmic_config_interface(reg, reg_val, 0xffff, 0);
}
EXPORT_SYMBOL(connectivity_export_upmu_set_reg_value);

/*******************************************************************************
 * MMC: only the external SDIO combo chips use this; MT6771 connsys is on AHB.
 ******************************************************************************/
int connectivity_export_mmc_io_rw_direct(struct mmc_card *card,
				int write, unsigned int fn,
				unsigned int addr, u8 in, u8 *out)
{
	pr_warn_once(DFT_TAG "[W]%s: no SDIO combo chip support, returns -ENODEV\n",
		     __func__);
	return -ENODEV;
}
EXPORT_SYMBOL(connectivity_export_mmc_io_rw_direct);

/******************************************************************************
 * GPIO dump information: no MTK GPIO dump on 5.10.
 ******************************************************************************/
void connectivity_export_dump_gpio_info(int start, int end)
{
	pr_warn_once(DFT_TAG "[W]%s: no GPIO register dump on this kernel\n",
		     __func__);
}
EXPORT_SYMBOL(connectivity_export_dump_gpio_info);

/* Prints each thread whose name starts with name, with its stack. */
void connectivity_export_dump_thread_state(const char *name)
{
	static const char stat_nam[] = TASK_STATE_TO_CHAR_STR;
	struct task_struct *p;

	if (name == NULL || strlen(name) > 255) {
		pr_info("invalid name:%p or thread name too long\n", name);
		return;
	}

	pr_info("start to show debug info of %s\n", name);

	rcu_read_lock();
	for_each_process(p) {
		unsigned long state;

		if (strncmp(p->comm, name, strlen(name)) != 0)
			continue;
		state = p->state;
		if (state)
			state = __ffs(state) + 1;
		pr_info("%d:%-15.15s %c cpu=%d on_cpu=%d\n", p->pid, p->comm,
			state < sizeof(stat_nam) - 1 ? stat_nam[state] : '?',
			task_cpu(p), p->on_cpu);
		sched_show_task(p);
		break;
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(connectivity_export_dump_thread_state);
