/******************************************************************************
 * mtk_pwm.c PWM Drvier
 *
 * Copyright (c) 2016, Media Teck.inc
 * Copyright (c) 2018, Media Teck.inc (mt6771/mtk_pwm_hal.c)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public Licence,
 * version 2, as publish by the Free Software Foundation.
 *
 * This program is distributed and in hope it will be useful, but WITHOUT
 * ANY WARRNTY; without even the implied warranty of MERCHANTABITLITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 ******************************************************************************
 */

/*
 * MT6771 PWM (mediatek,pwm at 0x11006000), memory mode only, for the IR
 * transmitter. The register writes and their order are those of MediaTek's
 * pwm_set_spec_config() and mt_pwm_disable() with the mt6771 HAL. Other
 * modes, the PMIC pad, interrupts and the debug files are left out.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <mt-plat/mtk_pwm.h>

#define T "[PWM]"

/* block registers */
#define PWM_ENABLE		0x0000
#define PWM_CK_26M_SEL		0x0210

/* channel registers, PWM1 at 0x10, then every 0x40 */
#define PWM_CH(n)		(0x0010 + 0x40 * (n))
#define PWM_CON			0x00
#define PWM_HDURATION		0x04
#define PWM_LDURATION		0x08
#define PWM_GDURATION		0x0c
#define PWM_BUF0_BASE_ADDR	0x10
#define PWM_BUF0_SIZE		0x14
#define PWM_WAVE_NUM		0x28
#define PWM_BUF_BASE_ADDR2	0x3c

#define PWM_CON_CLKDIV_MASK	GENMASK(2, 0)
#define PWM_CON_CLKSEL		BIT(3)
#define PWM_CON_CLKSEL_OLD	BIT(4)
#define PWM_CON_SRCSEL		BIT(5)
#define PWM_CON_MODE		BIT(6)
#define PWM_CON_IDLE_VALUE	BIT(7)
#define PWM_CON_GUARD_VALUE	BIT(8)
/* memory mode uses 5 of the 6 stop bit position bits */
#define PWM_CON_STOP_BITS_MEM	GENMASK(13, 9)
#define PWM_CON_STOP_BITS_SHIFT	9
#define PWM_CON_OLD_MODE	BIT(15)

/* clock order and names of the DT node */
enum { PWM1_CLK, PWM2_CLK, PWM3_CLK, PWM4_CLK, PWM_HCLK, PWM_CLK, PWM_CLK_NUM };
static const char * const pwm_clk_name[PWM_CLK_NUM] = {
	"PWM1-main", "PWM2-main", "PWM3-main", "PWM4-main",
	"PWM-HCLK-main", "PWM-main"
};

/* u8 so register offsets add as bytes */
static u8 __iomem *pwm_base;
static struct clk *pwm_clk[PWM_CLK_NUM];
/* clocks turned on, one bit per pwm_clk entry */
static unsigned long power_flag;
static DEFINE_MUTEX(pwm_lock);

static void pwm_rmw(u32 off, u32 clr, u32 set)
{
	writel((readl(pwm_base + off) & ~clr) | set, pwm_base + off);
}

static void pwm_clk_on(int i)
{
	int ret;

	if (test_bit(i, &power_flag))
		return;
	ret = clk_prepare_enable(pwm_clk[i]);
	if (ret)
		pr_notice(T "enable %s failed: %d\n", pwm_clk_name[i], ret);
	else
		set_bit(i, &power_flag);
}

static void pwm_clk_off(int i)
{
	if (test_and_clear_bit(i, &power_flag))
		clk_disable_unprepare(pwm_clk[i]);
}

/* Memory mode only: the channel sends BUF0_SIZE + 1 words from BUF0. */
s32 pwm_set_spec_config(struct pwm_spec_config *conf)
{
	u32 ch, con;

	if (!pwm_base)
		return -ENODEV;
	if (conf->pwm_no >= PWM_MAX || conf->mode != PWM_MODE_MEMORY ||
	    conf->clk_div >= CLK_DIV_MAX || conf->pmic_pad ||
	    (conf->clk_src != PWM_CLK_NEW_MODE_BLOCK &&
	     conf->clk_src != PWM_CLK_NEW_MODE_BLOCK_DIV_BY_1625)) {
		pr_notice(T "pwm%u mode %u clk %u not supported\n",
			  conf->pwm_no, conf->mode, conf->clk_src);
		return -EINVAL;
	}

	ch = PWM_CH(conf->pwm_no);
	con = ch + PWM_CON;
	mutex_lock(&pwm_lock);
	/* main and bus clocks first, then the channel clock */
	if (!(power_flag & (BIT(PWM_CLK) | BIT(PWM_HCLK)))) {
		pwm_clk_on(PWM_CLK);
		pwm_clk_on(PWM_HCLK);
	}
	pwm_clk_on(conf->pwm_no);

	pwm_rmw(PWM_CK_26M_SEL, 0, BIT(0));
	pwm_rmw(con, PWM_CON_OLD_MODE, 0);
	pwm_rmw(con, 0, PWM_CON_SRCSEL);
	pwm_rmw(con, PWM_CON_MODE, 0);
	pwm_rmw(con, PWM_CON_IDLE_VALUE,
		conf->PWM_MODE_MEMORY_REGS.IDLE_VALUE == IDLE_TRUE ?
		PWM_CON_IDLE_VALUE : 0);
	pwm_rmw(con, PWM_CON_GUARD_VALUE,
		conf->PWM_MODE_MEMORY_REGS.GUARD_VALUE == GUARD_TRUE ?
		PWM_CON_GUARD_VALUE : 0);
	writel(conf->PWM_MODE_MEMORY_REGS.HDURATION, pwm_base + ch + PWM_HDURATION);
	writel(conf->PWM_MODE_MEMORY_REGS.LDURATION, pwm_base + ch + PWM_LDURATION);
	writel(conf->PWM_MODE_MEMORY_REGS.GDURATION, pwm_base + ch + PWM_GDURATION);
	writel(lower_32_bits(conf->PWM_MODE_MEMORY_REGS.BUF0_BASE_ADDR),
	       pwm_base + ch + PWM_BUF0_BASE_ADDR);
	/* address bits 32-35 */
	pwm_rmw(ch + PWM_BUF_BASE_ADDR2, 0xf,
		upper_32_bits(conf->PWM_MODE_MEMORY_REGS.BUF0_BASE_ADDR) & 0xf);
	writel((u16)conf->PWM_MODE_MEMORY_REGS.BUF0_SIZE, pwm_base + ch + PWM_BUF0_SIZE);
	writel(conf->PWM_MODE_MEMORY_REGS.WAVE_NUM, pwm_base + ch + PWM_WAVE_NUM);
	pwm_rmw(con, PWM_CON_STOP_BITS_MEM,
		(conf->PWM_MODE_MEMORY_REGS.STOP_BITPOS_VALUE <<
		 PWM_CON_STOP_BITS_SHIFT) & PWM_CON_STOP_BITS_MEM);
	pwm_rmw(con, PWM_CON_CLKDIV_MASK, conf->clk_div);
	pwm_rmw(con, PWM_CON_CLKSEL_OLD, 0);
	if (conf->clk_src == PWM_CLK_NEW_MODE_BLOCK)
		pwm_rmw(con, PWM_CON_CLKSEL, 0);
	else
		pwm_rmw(con, 0, PWM_CON_CLKSEL);

	/* buffer and settings visible before the start */
	mb();
	pwm_rmw(PWM_ENABLE, 0, BIT(conf->pwm_no));
	mutex_unlock(&pwm_lock);
	return 0;
}

void mt_pwm_disable(u32 pwm_no, u8 pmic_pad)
{
	if (!pwm_base || pwm_no >= PWM_MAX)
		return;
	mutex_lock(&pwm_lock);
	/* the registers can't be reached with the main clock off */
	if (test_bit(PWM_CLK, &power_flag)) {
		pwm_rmw(PWM_ENABLE, BIT(pwm_no), 0);
		mdelay(1);
	}
	pwm_clk_off(pwm_no);
	pwm_clk_off(PWM_HCLK);
	pwm_clk_off(PWM_CLK);
	mutex_unlock(&pwm_lock);
}

static int mt_pwm_probe(struct platform_device *pdev)
{
	void __iomem *base;
	int i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	for (i = 0; i < PWM_CLK_NUM; i++) {
		pwm_clk[i] = devm_clk_get(&pdev->dev, pwm_clk_name[i]);
		if (IS_ERR(pwm_clk[i]))
			return dev_err_probe(&pdev->dev, PTR_ERR(pwm_clk[i]),
					     "no %s clock\n", pwm_clk_name[i]);
	}
	pwm_base = base;
	dev_info(&pdev->dev, "memory mode ready\n");
	return 0;
}

static const struct of_device_id pwm_of_match[] = {
	{ .compatible = "mediatek,pwm", },
	{},
};

static struct platform_driver pwm_plat_driver = {
	.probe = mt_pwm_probe,
	.driver = {
		.name = "mt-pwm",
		.of_match_table = pwm_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(pwm_plat_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek PWM memory mode");
