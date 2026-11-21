// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 SPM base driver, the part of T's spm_v4
 * spm_module_init (mtk_spm.c) the vcorefs and sleep code build on:
 * - maps mediatek,sleep (SPM) and mediatek,sleep_reg_md
 * - requests SPM IRQ0; the handler reads SPM_IRQ_STA and ATF clears it
 *   (SIP 0xc2000222, which masks and acks the SPM interrupt)
 * - tells ATF the PCM firmware index (SPM_ARGS SPMFW_IDX), which ATF only
 *   stores, then reads the firmware status (SIP 0xc2000221), a read in ATF
 * - debugfs spm/status
 * A storm guard turns IRQ0 off if it fires more than 1000 times in a second.
 * Nothing here writes an SPM register or starts vcorefs, DVFS or suspend.
 * TWAM, SODI, deep idle, the edge trigger IRQ list and the pm notifier are
 * left out.
 */
#define pr_fmt(fmt) "mtk_spm: " fmt

#include <linux/arm-smccc.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/soc/mediatek/mtk-spm.h>
#include <linux/spinlock.h>

/* T's spm_v4/mtk_spm_reg_mt6771.h */
#define PCM_REG15_DATA		0x13c
#define SPM_IRQ_STA		0x158
#define PWR_STATUS		0x180
#define PWR_STATUS_2ND		0x184
#define SPM_SW_RSV_5		0x61c

/* SPM_IRQ_STA, T's spm_v4/mtk_spm_internal.h */
#define ISRS_TWAM		BIT(2)
#define ISRS_PCM_RETURN		BIT(3)
#define ISRS_SW_INT0		BIT(4)
#define ISRS_SW_INT1		BIT(5)

/*
 * Stock picks the index from the DRAM type and top rate; this phone has
 * LPDDR4X at 3600 (stock recovery logged __spmfw_idx 0x0 (type:3 freq:3600)).
 */
#define SPM_POCKET_SPMFW_IDX	SPMFW_LP4X_2CH_3733
/* Stock's is_ext_buck on mt6771 */
#define SPM_POCKET_EXT_BUCK	0

/* More IRQ0s than this in one second means ATF isn't clearing it */
#define SPM_IRQ0_STORM_LIMIT	1000

void __iomem *spm_base;
void __iomem *sleep_reg_md_base;
u32 spm_irq_0;
int __spmfw_idx = -1;

static DEFINE_SPINLOCK(spm_irq_lock);
static unsigned long spm_irq0_count;
static u32 spm_irq0_last_isr;
static unsigned long spm_irq0_window_start;
static unsigned int spm_irq0_window_count;
static bool spm_irq0_storm;
static int spm_fw_status = -1;

static unsigned long spm_sip(u32 id, unsigned long a1, unsigned long a2,
			     unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(id, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

int spm_load_firmware_status(void)
{
	if (spm_fw_status == -1)
		spm_fw_status = (int)spm_sip(MTK_SIP_KERNEL_SPM_FIRMWARE_STATUS, 0, 0, 0);
	return spm_fw_status;
}

/* T's spm_irq0_handler without TWAM, which nothing here turns on */
static irqreturn_t spm_irq0_handler(int irq, void *dev_id)
{
	unsigned long flags;
	bool tripped = false;
	u32 isr;

	spin_lock_irqsave(&spm_irq_lock, flags);
	isr = readl(spm_base + SPM_IRQ_STA);
	spm_sip(MTK_SIP_KERNEL_SPM_IRQ0_HANDLER, isr, 0, 0);
	spm_irq0_count++;
	spm_irq0_last_isr = isr;
	if (!spm_irq0_window_count ||
	    time_after(jiffies, spm_irq0_window_start + HZ)) {
		spm_irq0_window_start = jiffies;
		spm_irq0_window_count = 0;
	}
	if (++spm_irq0_window_count > SPM_IRQ0_STORM_LIMIT && !spm_irq0_storm) {
		spm_irq0_storm = true;
		tripped = true;
		disable_irq_nosync(irq);
	}
	spin_unlock_irqrestore(&spm_irq_lock, flags);

	if (tripped)
		pr_err("IRQ0 storm, %u in under a second, last SPM_IRQ_STA 0x%x, disabled\n",
		       SPM_IRQ0_STORM_LIMIT + 1, isr);
	if (isr & (ISRS_SW_INT0 | ISRS_SW_INT1 | ISRS_PCM_RETURN))
		pr_err_ratelimited("IRQ0 should not fire (0x%x)\n", isr);
	return IRQ_HANDLED;
}

static int spm_status_show(struct seq_file *m, void *unused)
{
	unsigned long flags, count;
	u32 last;
	bool storm;

	spin_lock_irqsave(&spm_irq_lock, flags);
	count = spm_irq0_count;
	last = spm_irq0_last_isr;
	storm = spm_irq0_storm;
	spin_unlock_irqrestore(&spm_irq_lock, flags);

	seq_printf(m, "firmware_status %d (now %d)\n", spm_load_firmware_status(),
		   (int)spm_sip(MTK_SIP_KERNEL_SPM_FIRMWARE_STATUS, 0, 0, 0));
	seq_printf(m, "spmfw_idx %d\n", __spmfw_idx);
	seq_printf(m, "PCM_REG15 0x%08x\n", readl(spm_base + PCM_REG15_DATA));
	seq_printf(m, "SPM_SW_RSV_5 0x%08x\n", readl(spm_base + SPM_SW_RSV_5));
	seq_printf(m, "PWR_STATUS 0x%08x\n", readl(spm_base + PWR_STATUS));
	seq_printf(m, "PWR_STATUS_2ND 0x%08x\n", readl(spm_base + PWR_STATUS_2ND));
	seq_printf(m, "irq0 %u count %lu last_isr 0x%x%s\n", spm_irq_0, count, last,
		   storm ? " disabled by storm guard" : "");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(spm_status);

static int spm_base_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *md;
	int irq, ret;

	/* of_iomap: the SPM block is also mapped by the scpsys and modem code */
	spm_base = of_iomap(dev->of_node, 0);
	if (!spm_base) {
		dev_err(dev, "no SPM registers\n");
		return -ENOMEM;
	}

	md = of_find_compatible_node(NULL, NULL, "mediatek,sleep_reg_md");
	if (md) {
		sleep_reg_md_base = of_iomap(md, 0);
		of_node_put(md);
	}
	if (!sleep_reg_md_base)
		dev_err(dev, "no sleep_reg_md registers\n");

	irq = platform_get_irq(pdev, 0);
	if (irq > 0) {
		spm_irq_0 = irq;
		/* Stock: level low from the DT, kept on over suspend */
		ret = devm_request_irq(dev, irq, spm_irq0_handler, IRQF_NO_SUSPEND,
				       "SPM", NULL);
		if (ret)
			dev_err(dev, "IRQ0 %d: %d\n", irq, ret);
	}

	__spmfw_idx = SPM_POCKET_SPMFW_IDX;
	spm_sip(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_SPMFW_IDX, __spmfw_idx,
		SPM_POCKET_EXT_BUCK);

	dev_info(dev, "spmfw_idx %d, firmware status %d, irq0 %u\n", __spmfw_idx,
		 spm_load_firmware_status(), spm_irq_0);

	debugfs_create_file("status", 0444, debugfs_create_dir("spm", NULL), NULL,
			    &spm_status_fops);
	return 0;
}

static const struct of_device_id spm_base_of_match[] = {
	{ .compatible = "mediatek,sleep" },
	{ }
};

static struct platform_driver spm_base_driver = {
	.probe = spm_base_probe,
	.driver = {
		.name = "mtk-spm-base",
		.of_match_table = spm_base_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(spm_base_driver);
