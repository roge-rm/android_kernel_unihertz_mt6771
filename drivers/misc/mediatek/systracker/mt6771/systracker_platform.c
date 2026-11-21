/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/io.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <linux/atomic.h>
#include <asm/system_misc.h>
#include <asm/traps.h>
#include <linux/signal.h>
#include "../systracker_v2.h"

#define IOMEM(a)	((void __force __iomem *)((a)))

#ifdef SYSTRACKER_TEST_SUIT
void __iomem *p1;
void __iomem *mm_area1;
#endif

/*
 * Pocket 5.10: arm64 has no hook_fault_code or register_async_abort_handler
 * any more. A read that times out still ends in a synchronous external abort
 * (die, DIE_OOPS) and a write in an SError (panic), so the entries stock's
 * read and write timeout handlers printed are printed from the die and panic
 * notifiers instead, at emergency level so they reach the pstore console. The
 * oops or panic prints the backtrace that aee_dump_backtrace kept in AEE SRAM.
 */
static atomic_t systracker_reported = ATOMIC_INIT(0);

static void systracker_timeout_report(const char *why)
{
	unsigned int con;
	int i = 0;

	if (!smp_load_acquire(&systracker_probed))
		return;
	con = readl(IOMEM(BUS_DBG_CON));
	if (!(con & BUS_DBG_CON_TIMEOUT) ||
	    atomic_xchg(&systracker_reported, 1))
		return;

	pr_emerg("[systracker] %s, BUS_DBG_CON=0x%x\n", why, con);

	if (con & (BUS_DBG_CON_IRQ_AR_STA0|BUS_DBG_CON_IRQ_AR_STA1)) {
		pr_emerg("%s:%d: read timeout\n", __func__, __LINE__);
		for (i = 0; i < BUS_DBG_NUM_TRACKER; i++) {
			pr_emerg("AR_TRACKER Timeout Entry[%d]: ReadAddr:0x%x, Length:0x%x, TransactionID:0x%x!\n",
			       i,
			       readl(IOMEM(BUS_DBG_AR_TRACK_L(i))),
			       readl(IOMEM(BUS_DBG_AR_TRACK_H(i))),
			       readl(IOMEM(BUS_DBG_AR_TRANS_TID(i))));
		}
	}

	if (con & (BUS_DBG_CON_IRQ_AW_STA0|BUS_DBG_CON_IRQ_AW_STA1)) {
		pr_emerg("%s:%d: write timeout\n", __func__, __LINE__);
		for (i = 0; i < BUS_DBG_NUM_TRACKER; i++) {
			pr_emerg("AW_TRACKER Timeout Entry[%d]: WriteAddr:0x%x, Length:0x%x, TransactionID:0x%x!\n",
			       i,
			       readl(IOMEM(BUS_DBG_AW_TRACK_L(i))),
			       readl(IOMEM(BUS_DBG_AW_TRACK_H(i))),
			       readl(IOMEM(BUS_DBG_AW_TRANS_TID(i))));
		}
	}
}

static int systracker_die_handler(struct notifier_block *nb,
	unsigned long val, void *data)
{
	if (val == DIE_OOPS)
		systracker_timeout_report("oops");
	return NOTIFY_DONE;
}

static int systracker_panic_handler(struct notifier_block *nb,
	unsigned long val, void *data)
{
	systracker_timeout_report("panic");
	return NOTIFY_DONE;
}

static struct notifier_block systracker_die_nb = {
	.notifier_call = systracker_die_handler,
	.priority = INT_MAX,
};

static struct notifier_block systracker_panic_nb = {
	.notifier_call = systracker_panic_handler,
	.priority = INT_MAX,
};

static int __init systracker_platform_hook_fault(void)
{
	int ret;

	ret = register_die_notifier(&systracker_die_nb);
	if (ret) {
		pr_notice("%s:%d: register_die_notifier failed\n",
			__func__, __LINE__);
		return -1;
	}
	atomic_notifier_chain_register(&panic_notifier_list,
		&systracker_panic_nb);

	return 0;
}

#ifdef SYSTRACKER_TEST_SUIT
void __iomem *p1;
void __iomem *mm_area1;

static int systracker_platform_test_init(void)
{
	p1 = ioremap(0x10001220, 0x4);
	/* use mmsys reg base for our test */
	mm_area1 = ioremap(0x14000000, 0x4);

	return 0;
}

static void systracker_platform_test_cleanup(void)
{
	writel(readl(p1) & ~(0x1 << 6), p1);
}

static void systracker_platform_wp_test(void)
{
	void __iomem *ptr;
	/* use eint reg base as our watchpoint */
	ptr = ioremap(0x1000b000, 0x4);
	pr_debug("%s:%d: we got p = 0x%p\n", __func__, __LINE__, ptr);
	systracker_set_watchpoint_addr(0x1000b000);
	systracker_watchpoint_enable();
	/* touch it */
	writel(0, ptr);
	pr_debug("after we touched watchpoint\n");
	iounmap(ptr);
}

static void systracker_platform_read_timeout_test(void)
{
	/* FIXME: testing
	 * track_config.enable_slave_err = 0;
	 * systracker_enable();
	 */

	writel(readl(p1) | (0x1 << 6), p1);
	readl(mm_area1);
	writel(readl(p1) & ~(0x1 << 6), p1);
}

static void systracker_platform_write_timeout_test(void)
{
	writel(readl(p1) | (0x1 << 6), p1);
	writel(0x0, mm_area1);
	writel(readl(p1) & ~(0x1 << 6), p1);
}

static void systracker_platform_withrecord_test(void)
{
	writel(readl(IOMEM(BUS_DBG_CON)) |
		BUS_DBG_CON_HALT_ON_EN, IOMEM(BUS_DBG_CON));
	writel(readl(p1) | (0x1 << 6), p1);
	readl(mm_area1);
#if 0
	/* FIXME: related function not ready on FPGA */
	wdt_arch_reset(1);
#endif
}

static void systracker_platform_notimeout_test(void)
{
	writel(readl(p1) | (0x1 << 6), p1);
	/* disable timeout */
	writel(readl(IOMEM(BUS_DBG_CON)) &
		~(BUS_DBG_CON_TIMEOUT_EN), IOMEM(BUS_DBG_CON));
	/* read it, should cause bus hang */
	readl(mm_area1);
	/* never come back */
	pr_debug("failed??\n");
}
#endif
/* end of SYSTRACKER_TEST_SUIT */

/*
 * mt_systracker_init: initialize driver.
 * Always return 0.
 */
static int __init mt_systracker_init(void)
{
	struct mt_systracker_driver *systracker_drv;

	systracker_drv = get_mt_systracker_drv();

	systracker_drv->systracker_hook_fault =
		systracker_platform_hook_fault;
#ifdef SYSTRACKER_TEST_SUIT
	systracker_drv->systracker_test_init =
		systracker_platform_test_init;
	systracker_drv->systracker_test_cleanup =
		systracker_platform_test_cleanup;
	systracker_drv->systracker_wp_test =
		systracker_platform_wp_test;
	systracker_drv->systracker_read_timeout_test =
		systracker_platform_read_timeout_test;
	systracker_drv->systracker_write_timeout_test =
		systracker_platform_write_timeout_test;
	systracker_drv->systracker_withrecord_test =
		systracker_platform_withrecord_test;
	systracker_drv->systracker_notimeout_test =
		systracker_platform_notimeout_test;
#endif
	return 0;
}

arch_initcall(mt_systracker_init);
MODULE_DESCRIPTION("system tracker driver v2.0");
