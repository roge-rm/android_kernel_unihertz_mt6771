// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel symbols the stock connectivity modules import from drivers this
 * 5.10 tree doesn't have: clock buffer control, aee, the conn_md LTE coex
 * bridge (unless conn_md is built), the CPU boost and printk-too-much
 * controls. Each logs once what is missing. emi_mpu_set_protection comes
 * from emi/mt6771/emi_mpu.c (CONFIG_MTK_EMI). The prototypes
 * are the stock 4.14 ones, which the modules are built against.
 */

#define pr_fmt(fmt) "connadp: " fmt

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/mt6397/core.h>

#include <mtk_clkbuf_ctl.h>
#include <upmu_common.h>
#include <aee.h>

/*******************************************************************************
 * Clock buffers (4.14 clkbuf_v1/mt6771)
 *
 * On 4.14, clk_buf_ctrl() for CLK_BUF_CONN and CLK_BUF_NFC writes no register
 * while the buffer is under SW control with the co-clock flag set: it records
 * the caller's state for the BBLPM decision. The modes themselves are set by
 * clk_buf_init at boot, which this tree doesn't have, so the XO_WCN and
 * XO_NFC modes are whatever the bootloader left. This keeps the recorded state
 * and prints the DCXO registers so a device test can see them.
 ******************************************************************************/
/* Written with WRITE_ONCE: 4.14 callers may be atomic, so no lock. */
static bool clkbuf_swctrl[XO_NUMBER];

static struct regmap *clkbuf_pmic_map(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct mt6397_chip *chip;
	struct regmap *map = NULL;

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
	return map;
}

bool clk_buf_ctrl(enum clk_buf_id id, bool onoff)
{
	pr_warn_once("clk_buf_ctrl: no clkbuf driver, XO modes left as the bootloader set them; state recorded only\n");
	switch (id) {
	case CLK_BUF_CONN:
		WRITE_ONCE(clkbuf_swctrl[XO_WCN], onoff);
		return true;
	case CLK_BUF_NFC:
		WRITE_ONCE(clkbuf_swctrl[XO_NFC], onoff);
		return true;
	default:
		pr_info("%s: id=%d isn't supported\n", __func__, id);
		return false;
	}
}
EXPORT_SYMBOL(clk_buf_ctrl);

/*
 * XO enable status from the DCXO debug mux, as 4.14 clk_buf_get_xo_en(): write
 * the select to CW18[5:0], read CW19. Select 5 holds SOC (bit 0) and WCN (bit 6),
 * 6 holds NFC (0), CEL (6) and EXT (12), 7 holds PD (6); AUD isn't read on 4.14.
 * Sleeps (regmap over pwrap); the mutex keeps the select and read together.
 */
static const struct { u8 sel, bit; } xo_en_auxout[XO_NUMBER] = {
	[XO_SOC] = { 5, 0 }, [XO_WCN] = { 5, 6 }, [XO_NFC] = { 6, 0 },
	[XO_CEL] = { 6, 6 }, [XO_AUD] = { 0, 0 }, [XO_PD] = { 7, 6 },
	[XO_EXT] = { 6, 12 },
};
static DEFINE_MUTEX(xo_en_lock);

u8 clk_buf_get_xo_en_sta(enum xo_id id)
{
	struct regmap *map;
	unsigned int auxout = 0;
	int ret;

	if (id < 0 || id >= XO_NUMBER || !xo_en_auxout[id].sel)
		return 0;
	map = clkbuf_pmic_map();
	if (!map) {
		pr_warn_once("clk_buf_get_xo_en_sta: no MT6358 regmap, recorded state only\n");
		return clkbuf_swctrl[id];
	}
	mutex_lock(&xo_en_lock);
	ret = regmap_update_bits(map, PMIC_XO_STATIC_AUXOUT_SEL_ADDR,
				 PMIC_XO_STATIC_AUXOUT_SEL_MASK << PMIC_XO_STATIC_AUXOUT_SEL_SHIFT,
				 xo_en_auxout[id].sel << PMIC_XO_STATIC_AUXOUT_SEL_SHIFT);
	if (!ret)
		ret = regmap_read(map, PMIC_XO_STATIC_AUXOUT_ADDR, &auxout);
	mutex_unlock(&xo_en_lock);
	if (ret)
		return clkbuf_swctrl[id];
	return (auxout >> (PMIC_XO_STATIC_AUXOUT_SHIFT + xo_en_auxout[id].bit)) & 1;
}
EXPORT_SYMBOL(clk_buf_get_xo_en_sta);

void clk_buf_show_status_info(void)
{
	struct regmap *map = clkbuf_pmic_map();
	unsigned int cw00 = 0, cw11 = 0;

	if (map) {
		regmap_read(map, MT6358_DCXO_CW00, &cw00);
		regmap_read(map, MT6358_DCXO_CW11, &cw11);
	}
	pr_info("%s CW00=0x%x, CW11=0x%x, swctrl WCN %u NFC %u%s\n", __func__,
		cw00, cw11, clkbuf_swctrl[XO_WCN], clkbuf_swctrl[XO_NFC],
		map ? "" : " (no MT6358 regmap)");
}
EXPORT_SYMBOL(clk_buf_show_status_info);

/*******************************************************************************
 * aee: no AEE on this kernel. The other aee calls are inlines in aee.h.
 ******************************************************************************/
void aed_combo_exception_api(const int *log, int log_size, const int *phy,
			     int phy_size, const char *detail, const int db_opt)
{
	pr_warn_once("aed_combo_exception_api: no AEE, connsys core dump not saved\n");
	if (detail)
		pr_info("combo exception: %s\n", detail);
}
EXPORT_SYMBOL(aed_combo_exception_api);

/*******************************************************************************
 * conn_md: the LTE coex (IDC) bridge to the modem, when conn_md isn't built.
 * With CONFIG_MTK_CONN_MD the real bridge (drivers/misc/mediatek/conn_md)
 * exports these.
 ******************************************************************************/
#if !IS_ENABLED(CONFIG_MTK_CONN_MD)
struct ipc_ilm;
struct conn_md_bridge_ops;

int mtk_conn_md_bridge_reg(unsigned int u_id, struct conn_md_bridge_ops *p_ops)
{
	pr_warn_once("mtk_conn_md_bridge_reg: no modem bridge, LTE coex off\n");
	return -1;
}
EXPORT_SYMBOL(mtk_conn_md_bridge_reg);

int mtk_conn_md_bridge_unreg(unsigned int u_id)
{
	return 0;
}
EXPORT_SYMBOL(mtk_conn_md_bridge_unreg);

int mtk_conn_md_bridge_send_msg(struct ipc_ilm *ilm)
{
	pr_warn_once("mtk_conn_md_bridge_send_msg: no modem bridge, message dropped\n");
	return -1;
}
EXPORT_SYMBOL(mtk_conn_md_bridge_send_msg);
#endif

/*******************************************************************************
 * CPU boost (cpu_ctrl, PPM) and the printk-too-much switch, both 4.14 only.
 ******************************************************************************/
struct ppm_limit_data;
int update_userlimit_cpu_freq(int kicker, int num_cluster,
			      struct ppm_limit_data *freq_limit)
{
	pr_warn_once("update_userlimit_cpu_freq: no PPM, Wi-Fi CPU boost ignored\n");
	return 0;
}
EXPORT_SYMBOL(update_userlimit_cpu_freq);

void set_logtoomuch_enable(int value)
{
	pr_warn_once("set_logtoomuch_enable: no printk-too-much detection, ignored\n");
}
EXPORT_SYMBOL(set_logtoomuch_enable);
