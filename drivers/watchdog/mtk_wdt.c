// SPDX-License-Identifier: GPL-2.0+
/*
 * Mediatek Watchdog Driver
 *
 * Copyright (C) 2014 Matthias Brugger
 *
 * Matthias Brugger <matthias.bgg@gmail.com>
 *
 * Based on sunxi_wdt.c
 */

#include <dt-bindings/reset-controller/mt2712-resets.h>
#include <dt-bindings/reset-controller/mt8183-resets.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/soc/mediatek/mtk-wdt.h>
#include <linux/types.h>
#include <linux/watchdog.h>

#define WDT_MAX_TIMEOUT		31
#define WDT_MIN_TIMEOUT		1
#define WDT_LENGTH_TIMEOUT(n)	((n) << 5)

#define WDT_LENGTH		0x04
#define WDT_LENGTH_KEY		0x8
#define WDT_LENGTH_MASK		0xffe0

#define WDT_RST			0x08
#define WDT_RST_RELOAD		0x1971

#define WDT_MODE		0x00
#define WDT_MODE_EN		(1 << 0)
#define WDT_MODE_EXT_POL_LOW	(0 << 1)
#define WDT_MODE_EXT_POL_HIGH	(1 << 1)
#define WDT_MODE_EXRST_EN	(1 << 2)
#define WDT_MODE_IRQ_EN		(1 << 3)
#define WDT_MODE_AUTO_START	(1 << 4)
#define WDT_MODE_DUAL_EN	(1 << 6)
#define WDT_MODE_KEY		0x22000000

#define WDT_SWRST		0x14
#define WDT_SWRST_KEY		0x1209

#define WDT_SWSYSRST		0x18U
#define WDT_SWSYS_RST_KEY	0x88000000

/* T's wdt_v2 mtk_wdt.h, for the SPM suspend handover */
#define WDT_STA			0x0c
#define WDT_NONRST_REG2		0x24
#define WDT_NONRST2_SUSPEND_DIS	(1 << 14)
#define WDT_REQ_MODE		0x30
#define WDT_REQ_MODE_KEY	0x33000000
#define WDT_REQ_IRQ_EN		0x34
#define WDT_REQ_IRQ_KEY		0x44000000
#define WDT_REQ_SPM_SCPSYS	(1 << 1)	/* MODE_SPM_SCPSYS and IRQ_SPM_SCPSYS_EN */

#define DRV_NAME		"mtk-wdt"
#define DRV_VERSION		"1.0"

static bool nowayout = WATCHDOG_NOWAYOUT;
static unsigned int timeout;

/* The mt6771 TOPRGU, for mtk_wdt_spm_suspend and resume; set at probe */
static struct mtk_wdt_dev *spm_wdt;
static bool spm_wdt_was_on;

struct mtk_wdt_dev {
	struct watchdog_device wdt_dev;
	void __iomem *wdt_base;
	spinlock_t lock; /* protects WDT_SWSYSRST reg */
	struct reset_controller_dev rcdev;
};

struct mtk_wdt_data {
	int toprgu_sw_rst_num;
	bool adopt_boot_watchdog;
};

/* MT6771 uses the vendor common-v2 RGU layout, not an mt6589 alias. */
static const struct mtk_wdt_data mt6771_data = {
	.adopt_boot_watchdog = true,
};

static const struct mtk_wdt_data mt2712_data = {
	.toprgu_sw_rst_num = MT2712_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8183_data = {
	.toprgu_sw_rst_num = MT8183_TOPRGU_SW_RST_NUM,
};

static int toprgu_reset_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	unsigned int tmp;
	unsigned long flags;
	struct mtk_wdt_dev *data =
		 container_of(rcdev, struct mtk_wdt_dev, rcdev);

	spin_lock_irqsave(&data->lock, flags);

	tmp = readl(data->wdt_base + WDT_SWSYSRST);
	if (assert)
		tmp |= BIT(id);
	else
		tmp &= ~BIT(id);
	tmp |= WDT_SWSYS_RST_KEY;
	writel(tmp, data->wdt_base + WDT_SWSYSRST);

	spin_unlock_irqrestore(&data->lock, flags);

	return 0;
}

static int toprgu_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return toprgu_reset_update(rcdev, id, true);
}

static int toprgu_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return toprgu_reset_update(rcdev, id, false);
}

static int toprgu_reset(struct reset_controller_dev *rcdev,
			unsigned long id)
{
	int ret;

	ret = toprgu_reset_assert(rcdev, id);
	if (ret)
		return ret;

	return toprgu_reset_deassert(rcdev, id);
}

static const struct reset_control_ops toprgu_reset_ops = {
	.assert = toprgu_reset_assert,
	.deassert = toprgu_reset_deassert,
	.reset = toprgu_reset,
};

static int toprgu_register_reset_controller(struct platform_device *pdev,
					    int rst_num)
{
	int ret;
	struct mtk_wdt_dev *mtk_wdt = platform_get_drvdata(pdev);

	spin_lock_init(&mtk_wdt->lock);

	mtk_wdt->rcdev.owner = THIS_MODULE;
	mtk_wdt->rcdev.nr_resets = rst_num;
	mtk_wdt->rcdev.ops = &toprgu_reset_ops;
	mtk_wdt->rcdev.of_node = pdev->dev.of_node;
	ret = devm_reset_controller_register(&pdev->dev, &mtk_wdt->rcdev);
	if (ret != 0)
		dev_err(&pdev->dev,
			"couldn't register wdt reset controller: %d\n", ret);
	return ret;
}

static int mtk_wdt_restart(struct watchdog_device *wdt_dev,
			   unsigned long action, void *data)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;
	u32 reg;

	wdt_base = mtk_wdt->wdt_base;

	/* Enable reset in order to issue a system reset instead of an IRQ */
	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_IRQ_EN;
	writel(reg | WDT_MODE_KEY, wdt_base + WDT_MODE);

	while (1) {
		writel(WDT_SWRST_KEY, wdt_base + WDT_SWRST);
		mdelay(5);
	}

	return 0;
}

static int mtk_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;

	iowrite32(WDT_RST_RELOAD, wdt_base + WDT_RST);

	return 0;
}

static int mtk_wdt_set_timeout(struct watchdog_device *wdt_dev,
				unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	wdt_dev->timeout = timeout;

	/*
	 * One bit is the value of 512 ticks
	 * The clock has 32 KHz
	 */
	reg = WDT_LENGTH_TIMEOUT(timeout << 6) | WDT_LENGTH_KEY;
	iowrite32(reg, wdt_base + WDT_LENGTH);

	mtk_wdt_ping(wdt_dev);

	return 0;
}

static int mtk_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_EN;
	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	return 0;
}

static int mtk_wdt_start(struct watchdog_device *wdt_dev)
{
	u32 reg;
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	int ret;

	ret = mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	if (ret < 0)
		return ret;

	reg = ioread32(wdt_base + WDT_MODE);
	reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	reg |= (WDT_MODE_EN | WDT_MODE_KEY);
	iowrite32(reg, wdt_base + WDT_MODE);

	return 0;
}

static int mtk_wdt_boot_handoff(struct watchdog_device *wdt_dev, bool adopt)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *base = mtk_wdt->wdt_base;
	u32 mode;

	if (!adopt)
		return mtk_wdt_stop(wdt_dev);

	mode = readl(base + WDT_MODE);
	dev_info(wdt_dev->parent, "Boot watchdog mode=%#x length=%#x\n",
		 mode, readl(base + WDT_LENGTH));
	if (!(mode & WDT_MODE_EN)) {
		dev_info(wdt_dev->parent, "Boot watchdog is off; leaving it off\n");
		return 0;
	}

	/* Leave the running timer as a rescue if setup cannot complete. */
	if (!IS_ENABLED(CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED))
		return -EINVAL;
	if (wdt_dev->timeout < WDT_MIN_TIMEOUT ||
	    wdt_dev->timeout > WDT_MAX_TIMEOUT)
		return -EINVAL;

	mtk_wdt_ping(wdt_dev);
	mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	if ((readl(base + WDT_LENGTH) & WDT_LENGTH_MASK) !=
	    WDT_LENGTH_TIMEOUT(wdt_dev->timeout << 6))
		return -EIO;

	/* No IRQ handler is installed. Preserve EXTRA_CNT, DDR and routing bits. */
	mode &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	writel(mode | WDT_MODE_KEY, base + WDT_MODE);
	if ((readl(base + WDT_MODE) &
	     (WDT_MODE_EN | WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN)) != WDT_MODE_EN)
		return -EIO;

	/* watchdog_dev.c feeds this timer until userspace opens the device. */
	set_bit(WDOG_HW_RUNNING, &wdt_dev->status);
	dev_info(wdt_dev->parent, "Adopted boot watchdog (timeout=%u sec)\n",
		 wdt_dev->timeout);
	return 0;
}

static const struct watchdog_info mtk_wdt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops mtk_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= mtk_wdt_start,
	.stop		= mtk_wdt_stop,
	.ping		= mtk_wdt_ping,
	.set_timeout	= mtk_wdt_set_timeout,
	.restart	= mtk_wdt_restart,
};

static int mtk_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_wdt_dev *mtk_wdt;
	const struct mtk_wdt_data *wdt_data;
	int err;

	mtk_wdt = devm_kzalloc(dev, sizeof(*mtk_wdt), GFP_KERNEL);
	if (!mtk_wdt)
		return -ENOMEM;

	platform_set_drvdata(pdev, mtk_wdt);
	spin_lock_init(&mtk_wdt->lock);

	mtk_wdt->wdt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mtk_wdt->wdt_base))
		return PTR_ERR(mtk_wdt->wdt_base);

	mtk_wdt->wdt_dev.info = &mtk_wdt_info;
	mtk_wdt->wdt_dev.ops = &mtk_wdt_ops;
	mtk_wdt->wdt_dev.timeout = WDT_MAX_TIMEOUT;
	mtk_wdt->wdt_dev.max_timeout = WDT_MAX_TIMEOUT;
	mtk_wdt->wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
	mtk_wdt->wdt_dev.parent = dev;

	wdt_data = of_device_get_match_data(dev);
	err = watchdog_init_timeout(&mtk_wdt->wdt_dev, timeout, dev);
	if (err && wdt_data && wdt_data->adopt_boot_watchdog)
		return err;
	watchdog_set_nowayout(&mtk_wdt->wdt_dev, nowayout);
	watchdog_set_restart_priority(&mtk_wdt->wdt_dev, 128);

	watchdog_set_drvdata(&mtk_wdt->wdt_dev, mtk_wdt);

	err = mtk_wdt_boot_handoff(&mtk_wdt->wdt_dev,
				   wdt_data && wdt_data->adopt_boot_watchdog);
	if (err) {
		dev_err(dev, "Watchdog boot handoff failed: %d\n", err);
		return err;
	}

	watchdog_stop_on_reboot(&mtk_wdt->wdt_dev);
	err = devm_watchdog_register_device(dev, &mtk_wdt->wdt_dev);
	if (unlikely(err))
		return err;

	dev_info(dev, "Watchdog enabled (timeout=%d sec, nowayout=%d)\n",
		 mtk_wdt->wdt_dev.timeout, nowayout);
	if (wdt_data == &mt6771_data)
		WRITE_ONCE(spm_wdt, mtk_wdt);

	if (wdt_data && wdt_data->toprgu_sw_rst_num > 0) {
		err = toprgu_register_reset_controller(pdev,
						       wdt_data->toprgu_sw_rst_num);
		if (err)
			return err;
	}
	return 0;
}

/*
 * SPM deep suspend's enter, on CPU0 with IRQs off. T's
 * spmwdt_mode_config(WD_REQ_EN, WD_REQ_RST_MODE): a PCM WDT timeout resets
 * the chip. Then its suspend_notify: the AP timer is turned off for the sleep,
 * as nothing feeds it then, with NONRST_REG2 SUSPEND_DIS set, and pinged.
 * Stock also sets IRQ, EXTEN and dual mode there; the other mode bits stay
 * as the boot handoff left them here.
 */
int mtk_wdt_spm_suspend(void)
{
	struct mtk_wdt_dev *wdt = READ_ONCE(spm_wdt);
	void __iomem *base;
	unsigned long flags;
	u32 mode;

	if (!wdt)
		return -ENODEV;
	base = wdt->wdt_base;
	spin_lock_irqsave(&wdt->lock, flags);
	writel(readl(base + WDT_REQ_MODE) | WDT_REQ_SPM_SCPSYS | WDT_REQ_MODE_KEY,
	       base + WDT_REQ_MODE);
	writel((readl(base + WDT_REQ_IRQ_EN) & ~WDT_REQ_SPM_SCPSYS) | WDT_REQ_IRQ_KEY,
	       base + WDT_REQ_IRQ_EN);
	mode = readl(base + WDT_MODE);
	spm_wdt_was_on = mode & WDT_MODE_EN;
	if (spm_wdt_was_on)
		writel((mode & ~WDT_MODE_EN) | WDT_MODE_KEY, base + WDT_MODE);
	writel(readl(base + WDT_NONRST_REG2) | WDT_NONRST2_SUSPEND_DIS,
	       base + WDT_NONRST_REG2);
	writel(WDT_RST_RELOAD, base + WDT_RST);
	spin_unlock_irqrestore(&wdt->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wdt_spm_suspend);

/*
 * After SPM sleep, still with IRQs off: T's resume_notify (the AP timer back
 * on if it was, SUSPEND_DIS cleared, a ping), then
 * spmwdt_mode_config(WD_REQ_DIS, WD_REQ_RST_MODE).
 */
void mtk_wdt_spm_resume(void)
{
	struct mtk_wdt_dev *wdt = READ_ONCE(spm_wdt);
	void __iomem *base;
	unsigned long flags;

	if (!wdt)
		return;
	base = wdt->wdt_base;
	spin_lock_irqsave(&wdt->lock, flags);
	writel(WDT_RST_RELOAD, base + WDT_RST);
	if (spm_wdt_was_on)
		writel(readl(base + WDT_MODE) | WDT_MODE_EN | WDT_MODE_KEY, base + WDT_MODE);
	writel(readl(base + WDT_NONRST_REG2) & ~WDT_NONRST2_SUSPEND_DIS,
	       base + WDT_NONRST_REG2);
	writel(WDT_RST_RELOAD, base + WDT_RST);
	writel((readl(base + WDT_REQ_MODE) & ~WDT_REQ_SPM_SCPSYS) | WDT_REQ_MODE_KEY,
	       base + WDT_REQ_MODE);
	writel((readl(base + WDT_REQ_IRQ_EN) & ~WDT_REQ_SPM_SCPSYS) | WDT_REQ_IRQ_KEY,
	       base + WDT_REQ_IRQ_EN);
	spin_unlock_irqrestore(&wdt->lock, flags);
}
EXPORT_SYMBOL_GPL(mtk_wdt_spm_resume);

#ifdef CONFIG_PM_SLEEP
static int mtk_wdt_suspend(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&mtk_wdt->wdt_dev))
		mtk_wdt_stop(&mtk_wdt->wdt_dev);
	else if (watchdog_hw_running(&mtk_wdt->wdt_dev))
		/*
		 * Adopted boot watchdog with no opener: it keeps running. In s2idle
		 * the timers run and the core keeps pinging; ping here so sleep
		 * starts with the full timeout.
		 */
		mtk_wdt_ping(&mtk_wdt->wdt_dev);

	return 0;
}

static int mtk_wdt_resume(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&mtk_wdt->wdt_dev)) {
		mtk_wdt_start(&mtk_wdt->wdt_dev);
		mtk_wdt_ping(&mtk_wdt->wdt_dev);
	} else if (watchdog_hw_running(&mtk_wdt->wdt_dev)) {
		mtk_wdt_ping(&mtk_wdt->wdt_dev);
	}

	return 0;
}
#endif

static const struct of_device_id mtk_wdt_dt_ids[] = {
	{ .compatible = "mediatek,mt2712-wdt", .data = &mt2712_data },
	{ .compatible = "mediatek,mt6589-wdt" },
	{ .compatible = "mediatek,mt6771-wdt", .data = &mt6771_data },
	{ .compatible = "mediatek,mt8183-wdt", .data = &mt8183_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_wdt_dt_ids);

static const struct dev_pm_ops mtk_wdt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_wdt_suspend,
				mtk_wdt_resume)
};

static struct platform_driver mtk_wdt_driver = {
	.probe		= mtk_wdt_probe,
	.driver		= {
		.name		= DRV_NAME,
		.pm		= &mtk_wdt_pm_ops,
		.of_match_table	= mtk_wdt_dt_ids,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(mtk_wdt_driver);

module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Brugger <matthias.bgg@gmail.com>");
MODULE_DESCRIPTION("Mediatek WatchDog Timer Driver");
MODULE_VERSION(DRV_VERSION);
