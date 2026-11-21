// SPDX-License-Identifier: GPL-2.0
/*
 * Power off through the MT6358 PMIC, as the MT6771 4.14 vendor kernel does
 * (mt_power_off -> hal_rtc_bbpu_pwdn -> PMIC_POWER_HOLD(0)).
 *
 * Every write and its order comes from the 4.14 mt6358 RTC/PMIC HAL and the
 * stock kernel image. Addresses are absolute MT6358 registers on the pwrap
 * regmap. The watchdog isn't stopped, as stock. If the PMIC stays on (a
 * charger is plugged in), it ends with stock's arch_reset(0, "charger"): a
 * TOPRGU software reset without the bypass-power-key flag, which LK takes
 * as a reason to look for a charger instead of booting Android.
 * Unlike stock, a set RTC alarm is cleared first, so the phone stays off.
 *
 * Restart goes through stock's arch_reset too: the RTC
 * PDN1 mark LK reads for recovery, fastboot or kpoc (from a reboot notifier,
 * before device_shutdown), then a TOPRGU software
 * reset with the bypass-power-key flag set for a plain restart or a panic
 * and cleared otherwise, ahead of PSCI's reset, which leaves the flag to the
 * ATF.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/string.h>
#ifdef CONFIG_MTK_CHARGER
#include <mt-plat/mtk_charger.h>
#endif

#define MT6358_TOP_TMA_KEY		0x03a8
#define MT6358_SCK_TOP_CKPDN_CON0_CLR	0x051e
#define MT6358_RTC_BBPU			0x0588
#define MT6358_RTC_IRQ_STA		0x058a
#define MT6358_RTC_IRQ_EN		0x058c
#define MT6358_RTC_AL_MASK		0x0590
#define MT6358_RTC_PDN1			0x05b4
#define MT6358_RTC_PROT			0x05bc
#define MT6358_RTC_WRTGR		0x05c2
#define MT6358_RTC_CON			0x05c4
#define MT6358_DCXO_CW02		0x0790
#define MT6358_PPCCTL0			0x0a08
#define MT6358_PSEQ_ELR11		0x0a62

#define CKPDN_RTC_EOSC32		BIT(2)
#define DCXO_XO_EN32K_MAN		BIT(0)
#define BBPU_KEY			(0x43 << 8)
#define BBPU_PWREN			BIT(0)
#define BBPU_CLR			BIT(1)
#define BBPU_RELOAD			BIT(5)
#define BBPU_CBUSY			BIT(6)
#define AL_MASK_DOW			BIT(4)
#define IRQ_EN_ONESHOT_AL		(BIT(2) | BIT(0))
#define PDN1_GPIO_USERS			0x1f00	/* Wi-Fi, GPS, BT, FM, PMIC */
/* rtc_spare_reg marks LK reads: FAC_RESET (bits 5:4 = 1), FAST_BOOT, KPOC */
#define PDN1_FAC_RESET_MASK		0x0030
#define PDN1_FAC_RESET			0x0010
#define PDN1_FAST_BOOT			0x2000
#define PDN1_KPOC			0x4000
/* rtc_writeif_unlock: two keys, each followed by a write trigger */
#define RTC_PROT_UNLOCK1		0x586a
#define RTC_PROT_UNLOCK2		0x9136
#define CON_F32KOB			BIT(5)
#define TMA_KEY_UNLOCK			0x9ca7
#define PSEQ_SDN_DLY_ENB		BIT(10)
#define PPCCTL0_PWRHOLD			BIT(0)

/* TOPRGU, as the 4.14 wdt_v2 mtk_wdt.h */
#define WDT_MODE			0x00
#define WDT_RESTART			0x08
#define WDT_STATUS			0x0c
#define WDT_SWRST			0x14
#define WDT_NONRST_REG			0x20
#define WDT_NONRST_REG2			0x24
#define WDT_MODE_KEY			0x22000000
#define WDT_MODE_EXTEN			BIT(2)
#define WDT_MODE_IRQ			BIT(3)
#define WDT_MODE_EXTRA_CNT		BIT(4)
#define WDT_MODE_DUAL_MODE		BIT(6)
#define WDT_MODE_DDR_RESERVE		BIT(7)
#define WDT_RESTART_KEY			0x1971
#define WDT_SWRST_KEY			0x1209
#define WDT_NONRST2_BYPASS_PWR_KEY	BIT(13)
#define WDT_NONRST2_REASON		0xf	/* read once by LK; 2 is recovery */

/* Ahead of PSCI (129) and mtk_wdt (128), as stock's own handler replaced both. */
#define MT6358_RESTART_PRIORITY		192

/* PWRAP_WRAP_EN in the 4.14 mt6771 pwrap register table */
#define PWRAP_WRAP_EN			0x04

/* mt_power_off: 100 ms per round, reset once a charger shows or after 11 rounds. */
#define POWER_OFF_ROUNDS		10

/* Stock waits up to 500 ms on sched_clock for CBUSY and CLR. */
#define RTC_WAIT_US			500000

static struct regmap *mt6358_map;
static void __iomem *toprgu_base;
static void __iomem *pwrap_base;

static unsigned int rtc_read(unsigned int reg)
{
	unsigned int val = 0;

	regmap_read(mt6358_map, reg, &val);
	return val;
}

static void rtc_wait(unsigned int mask)
{
	unsigned int val;

	if (regmap_read_poll_timeout_atomic(mt6358_map, MT6358_RTC_BBPU, val,
					    !(val & mask), 0, RTC_WAIT_US))
		pr_err("mt6358-poweroff: RTC_BBPU 0x%x stuck, mask 0x%x\n",
		       val, mask);
}

static void rtc_write_trigger(void)
{
	regmap_write(mt6358_map, MT6358_RTC_WRTGR, 1);
	rtc_wait(BBPU_CBUSY);
}

/*
 * wdt_arch_reset(bypass): hardware reboot mode, the bypass-power-key flag
 * cleared, then set only with bypass, then a software reset with pwrap off.
 * Without bypass (arch_reset(0, "charger") and the marked restarts) LK looks
 * for a charger instead of booting Android. Stock's dbgtop DFD and
 * DRAM-reserve steps are left out, as the 5.10 restart path leaves them
 * out; DDR reserve in the RGU mode is cleared.
 */
static void mt6358_charger_reset(bool bypass)
{
	u32 mode, non_rst2;

	pr_emerg("mt6358-poweroff: arch_reset, bypass power key %d\n", bypass);
	if (!toprgu_base)
		return;
	writel(WDT_RESTART_KEY, toprgu_base + WDT_RESTART);
	mb();

	non_rst2 = readl(toprgu_base + WDT_NONRST_REG2);
	mode = readl(toprgu_base + WDT_MODE);
	non_rst2 &= ~WDT_NONRST2_BYPASS_PWR_KEY;
	if (bypass)
		non_rst2 |= WDT_NONRST2_BYPASS_PWR_KEY;
	mode &= ~(WDT_MODE_IRQ | WDT_MODE_DUAL_MODE);
	mode |= WDT_MODE_KEY | WDT_MODE_EXTEN;
	writel(non_rst2, toprgu_base + WDT_NONRST_REG2);
	mb();
	writel(readl(toprgu_base + WDT_STATUS) |
	       readl(toprgu_base + WDT_NONRST_REG),
	       toprgu_base + WDT_NONRST_REG);
	mb();
	mode &= ~WDT_MODE_EXTRA_CNT;
	writel(mode, toprgu_base + WDT_MODE);
	mb();
	writel(readl(toprgu_base + WDT_STATUS), toprgu_base + WDT_NONRST_REG);
	mb();

	/* mtk_rgu_dram_reserved(0) */
	writel((readl(toprgu_base + WDT_MODE) & ~WDT_MODE_DDR_RESERVE) |
	       WDT_MODE_KEY, toprgu_base + WDT_MODE);
	mb();
	udelay(200);

	/* pwrap_disable(), then trigger the reset */
	if (pwrap_base) {
		writel(0, pwrap_base + PWRAP_WRAP_EN);
		mb();
		udelay(10);
	}
	writel(WDT_SWRST_KEY, toprgu_base + WDT_SWRST);
	mb();
}

static bool mt6358_charger_exists(void)
{
#ifdef CONFIG_MTK_CHARGER
	unsigned char exist = 0;

	mtk_chr_is_charger_exist(&exist);
	return exist == 1;
#else
	return false;
#endif
}

/* Runs with interrupts off and the other CPUs stopped. */
static void mt6358_power_off(void)
{
	struct regmap *map = mt6358_map;
	int count = 0;
	bool reset_tried = false;

	pr_emerg("mt6358-poweroff: dropping PWRHOLD\n");

	/* rtc_enable_k_eosc(): EOSC calibration clock on, DCXO to RTC mode. */
	regmap_update_bits(map, MT6358_SCK_TOP_CKPDN_CON0_CLR,
			   CKPDN_RTC_EOSC32, CKPDN_RTC_EOSC32);
	regmap_update_bits(map, MT6358_DCXO_CW02, DCXO_XO_EN32K_MAN, 0);

	/* Stop the 32K export when no RTC_GPIO user is left. */
	if (!(rtc_read(MT6358_RTC_PDN1) & PDN1_GPIO_USERS)) {
		regmap_write(map, MT6358_RTC_CON,
			     rtc_read(MT6358_RTC_CON) | CON_F32KOB);
		rtc_write_trigger();
	}

	/*
	 * Turn off the RTC alarm so it can't power the phone back on, as
	 * stock's hal_rtc_clear_alarm (alarm enable off, IRQ_STA read to
	 * clear). PDN2 and the alarm time are left alone: LK owns the PDN bits.
	 */
	regmap_update_bits(map, MT6358_RTC_IRQ_EN, IRQ_EN_ONESHOT_AL, 0);
	rtc_write_trigger();
	rtc_read(MT6358_RTC_IRQ_STA);

	/* Clear the low-power shutdown state and mask the day-of-week alarm. */
	regmap_write(map, MT6358_RTC_BBPU, BBPU_KEY | BBPU_CLR | BBPU_PWREN);
	regmap_write(map, MT6358_RTC_AL_MASK, AL_MASK_DOW);
	rtc_write_trigger();
	rtc_wait(BBPU_CLR);

	/* wk_pmic_enable_sdn_delay(): shutdown delay on, behind the TMA key. */
	regmap_update_bits(map, MT6358_TOP_TMA_KEY, 0xffff, TMA_KEY_UNLOCK);
	regmap_update_bits(map, MT6358_PSEQ_ELR11, PSEQ_SDN_DLY_ENB, 0);
	regmap_update_bits(map, MT6358_TOP_TMA_KEY, 0xffff, 0);

	regmap_write(map, MT6358_RTC_BBPU,
		     rtc_read(MT6358_RTC_BBPU) | BBPU_KEY | BBPU_RELOAD);
	rtc_write_trigger();

	/* PMIC_POWER_HOLD(0): the PMIC turns off once this clears. */
	regmap_update_bits(map, MT6358_PPCCTL0, PPCCTL0_PWRHOLD, 0);

	/*
	 * mt_power_off: with a charger in, power stays on, so reset into LK
	 * with arch_reset(0, "charger"). Without MTK_CHARGER stock loops here.
	 */
	while (1) {
		mdelay(100);
		if (IS_ENABLED(CONFIG_MTK_CHARGER) && !reset_tried &&
		    (mt6358_charger_exists() || count > POWER_OFF_ROUNDS)) {
			reset_tried = true;
			pr_emerg("mt6358-poweroff: still powered after %d ms\n",
				 (count + 1) * 100);
			mt6358_charger_reset(false);
		}
		if (count == POWER_OFF_ROUNDS)
			pr_emerg("mt6358-poweroff: still powered, waiting for the watchdog\n");
		count++;
	}
}

/* PDN1 = (PDN1 & ~clear) | set, then a write trigger, as hal_rtc_set_spare_register. */
static void rtc_mark(unsigned int clear, unsigned int set)
{
	regmap_write(mt6358_map, MT6358_RTC_PDN1,
		     (rtc_read(MT6358_RTC_PDN1) & ~clear) | set);
	rtc_write_trigger();
}

/*
 * Stock's arch_reset as two steps. The PDN1 mark LK reads ("recovery",
 * "bootloader", "kpoc") goes from a reboot notifier, in process context
 * before device_shutdown, since the pwrap regmap takes a mutex. Stock's
 * recovery mark also cleared the RTC alarm and power-on time; that's left
 * out here. A panic restart skips the reboot notifiers, so it never marks.
 */
static int mt6358_reboot_mark(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	const char *cmd = data;
	unsigned int clear = 0, set = 0;

	if (action != SYS_RESTART || !cmd)
		return NOTIFY_DONE;
	if (!strcmp(cmd, "recovery")) {
		clear = PDN1_FAC_RESET_MASK;
		set = PDN1_FAC_RESET;
	} else if (!strcmp(cmd, "bootloader")) {
		set = PDN1_FAST_BOOT;
	} else if (!strcmp(cmd, "kpoc")) {
		set = PDN1_KPOC;
	} else {
		return NOTIFY_DONE;
	}
	pr_info("mt6358-poweroff: PDN1 mark for %s\n", cmd);

	/* rtc_writeif_unlock, then the mark */
	regmap_write(mt6358_map, MT6358_RTC_PROT, RTC_PROT_UNLOCK1);
	rtc_write_trigger();
	regmap_write(mt6358_map, MT6358_RTC_PROT, RTC_PROT_UNLOCK2);
	rtc_write_trigger();
	rtc_mark(clear, set);
	return NOTIFY_DONE;
}

static struct notifier_block mt6358_reboot_nb = {
	.notifier_call = mt6358_reboot_mark,
};

/*
 * The reset itself: "recovery", "bootloader", "kpoc" and "charger" clear
 * the bypass flag; anything else (NULL, "reboot", a panic, unknown
 * commands) sets it. Only TOPRGU MMIO here, never the pwrap regmap, as
 * this runs with interrupts off and the other CPUs stopped.
 */
static int mt6358_restart(struct notifier_block *nb, unsigned long mode,
			  void *data)
{
	const char *cmd = data;
	bool bypass = !(cmd && (!strcmp(cmd, "charger") ||
				!strcmp(cmd, "recovery") ||
				!strcmp(cmd, "bootloader") ||
				!strcmp(cmd, "kpoc")));

	pr_emerg("mt6358-poweroff: restart, cmd %s\n", cmd ? cmd : "NULL");
	if (!toprgu_base)
		return NOTIFY_DONE;
	/* A stale reason nibble of 2 would send LK to recovery. */
	writel(readl(toprgu_base + WDT_NONRST_REG2) & ~WDT_NONRST2_REASON,
	       toprgu_base + WDT_NONRST_REG2);
	mb();
	mt6358_charger_reset(bypass);
	mdelay(1000);
	pr_emerg("mt6358-poweroff: still running after the TOPRGU reset\n");
	return NOTIFY_DONE;
}

static struct notifier_block mt6358_restart_nb = {
	.notifier_call = mt6358_restart,
	.priority = MT6358_RESTART_PRIORITY,
};

/* After deferred probes, so the pwrap and MT6358 mfd are bound. */
static int __init mt6358_poweroff_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct mt6397_chip *chip;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	if (!np)
		return 0;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		pr_err("mt6358-poweroff: no MT6358 device\n");
		return -ENODEV;
	}
	chip = platform_get_drvdata(pdev);
	if (!chip || !chip->regmap) {
		pr_err("mt6358-poweroff: MT6358 mfd not bound\n");
		put_device(&pdev->dev);
		return -ENODEV;
	}
	mt6358_map = chip->regmap;

	/* For the charger reset: TOPRGU and the pwrap above the MT6358. */
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6771-wdt");
	if (np) {
		toprgu_base = of_iomap(np, 0);
		of_node_put(np);
	}
	np = pdev->dev.of_node ? of_get_parent(pdev->dev.of_node) : NULL;
	if (np) {
		pwrap_base = of_iomap(np, 0);
		of_node_put(np);
	}
	if (!toprgu_base || !pwrap_base)
		pr_warn("mt6358-poweroff: no TOPRGU or pwrap mapping, no charger reset\n");
	pm_power_off = mt6358_power_off;
	if (toprgu_base && pwrap_base) {
		if (register_reboot_notifier(&mt6358_reboot_nb))
			pr_err("mt6358-poweroff: no reboot notifier\n");
		if (register_restart_handler(&mt6358_restart_nb))
			pr_err("mt6358-poweroff: no restart handler\n");
	}
	pr_info("mt6358-poweroff: registered\n");
	return 0;
}
late_initcall_sync(mt6358_poweroff_init);
