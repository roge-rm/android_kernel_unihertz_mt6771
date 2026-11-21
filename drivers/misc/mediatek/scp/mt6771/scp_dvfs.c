/*
 * Copyright (C) 2017 MediaTek Inc.
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
 * MT6771 SCP DVFS for 5.10, from the stock 4.14 scp/mt6771/scp_dvfs.c.
 *
 * The SCP clock mux (topckgen scp_sel) and its parents come from the
 * /scp_dvfs node, as stock. The SCP stays at CLK_OPP0 (125 MHz, the 0.6 V
 * vcore step): stock's PMIC vcore/vsram SSHUB calls have no 5.10 counterpart
 * here, so nothing changes a voltage. The DVFSRC VCORE_REQUEST [31:30] ask
 * goes out as stock through the SCP_VCORE_REQUEST pm_qos class (0 at OPP0).
 * The sensor features add up to well under 125 MHz, so stock asks for OPP0
 * too.
 * The PMIC SSHUB and V_REQ pin setup is only read and logged: stock's
 * mt_pmic_sshub_init wrote VCORE_SSHUB_EN 1 at 0.6 V (sleep 0.6 V),
 * VSRAM_OTHERS_SSHUB_EN 1 at 0.85 V (sleep 0.85 V), both SLEEP_VOSEL_EN 0,
 * and RG_SRCVOLTEN_LP_EN 1; the boot log shows whether LK set them.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/pm_wakeup.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_qos.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/workqueue.h>
#include "scp_ipi.h"
#include "scp_helper.h"
#include "scp_excep.h"
#include "scp_dvfs.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt)	"[scp_dvfs]: " fmt

#define DRV_Reg32(addr)	readl(addr)
#define DRV_WriteReg32(addr, val) writel(val, addr)
#define DRV_SetReg32(addr, val)	DRV_WriteReg32(addr, DRV_Reg32(addr) | (val))
#define DRV_ClrReg32(addr, val)	DRV_WriteReg32(addr, DRV_Reg32(addr) & ~(val))

/* MT6358 SSHUB registers stock's mt_pmic_sshub_init sets (upmu_hw.h) */
#define MT6358_BUCK_VCORE_SSHUB_CON0		0x14a4	/* bit 0 EN, bit 1 SLEEP_VOSEL_EN */
#define MT6358_BUCK_VCORE_SSHUB_CON1		0x14a6	/* VOSEL [6:0], VOSEL_SLEEP [14:8] */
#define MT6358_LDO_VSRAM_OTHERS_SSHUB_CON0	0x1bc4
#define MT6358_LDO_VSRAM_OTHERS_SSHUB_CON1	0x1bc6
#define MT6358_TOP_CLK_CON0			0x134	/* bit 4 RG_SRCVOLTEN_LP_EN */

/* -1:SCP DVFS OFF, 1:SCP DVFS ON */
static int scp_dvfs_flag = 1;

static int scp_cur_volt = -1;
static int pre_pll_sel = -1;
static struct mt_scp_pll_t *mt_scp_pll;
static struct wakeup_source *scp_suspend_lock;
static bool g_scp_dvfs_init_flag;
static u8 __iomem *gpio_base;

#define RG_GPIO154_MODE		(gpio_base + 0x430)
#define GPIO154_BIT			8
#define GPIO154_MASK		0x7

/*
 * IPI_SCP_PLL_CTRL comes in interrupt context and the clock calls can sleep,
 * so each request is allocated and run in order on the ordered SCP workqueue,
 * the one notify and feature registration use, which serialises pre_pll_sel.
 */
struct scp_pll_req {
	struct scp_work_struct sw;
	unsigned int flag;
	unsigned int sel;
};

bool scp_dvfs_ready(void)
{
	return g_scp_dvfs_init_flag;
}

unsigned int scp_get_dvfs_opp(void)
{
	return (unsigned int)scp_cur_volt;
}

uint32_t scp_get_freq(void)
{
	uint32_t i;
	uint32_t sum = 0;

	for (i = 0; i < NUM_FEATURE_ID; i++) {
		if (feature_table[i].enable == 1)
			sum += feature_table[i].freq;
	}
	for (i = 0; i < NUM_SENSOR_TYPE; i++) {
		if (sensor_type_table[i].enable == 1)
			sum += sensor_type_table[i].freq;
	}

	/* Fixed at OPP0: the higher steps need a vcore raise this port can't do. */
	if (sum > CLK_OPP0)
		pr_notice("request freq %d MHz held at %d MHz\n",
			sum, CLK_OPP0);
	return CLK_OPP0;
}

/*
 * DVFSRC VCORE_REQUEST [31:30] for the SCP, stock's
 * dvfsrc_set_scp_vcore_request: 1 at CLK_OPP2, else 0. The vcore DVFS
 * backend writes it once DVFS runs and keeps it until then, as stock.
 */
static struct pm_qos_request scp_vcore_qos;

static void scp_dvfsrc_vcore_request(unsigned int level)
{
	if (pm_qos_request_active(&scp_vcore_qos))
		pm_qos_update_request(&scp_vcore_qos, level);
	else
		pm_qos_add_request(&scp_vcore_qos, PM_QOS_SCP_VCORE_REQUEST, level);
}

/*
 * Stock sets the PMIC SSHUB vcore and vsram here too; at the fixed OPP0 only
 * the DVFSRC request and the SCP's own level register are written.
 */
static void scp_vcore_request(unsigned int clk_opp)
{
	scp_cur_volt = 2;	/* OPP0, stock's 0.6 V vcore and 0.85 V vsram step */

	/* set DVFSRC_VCORE_REQUEST [31:30] */
	if (scp_expected_freq == CLK_OPP2)
		scp_dvfsrc_vcore_request(0x1);
	else
		scp_dvfsrc_vcore_request(0x0);

	/* Modify scp reg: 0xC0094 [7:0] */
	DRV_ClrReg32(SCP_SCP2SPM_VOL_LV, 0xff);
	if (clk_opp == CLK_OPP0)
		DRV_SetReg32(SCP_SCP2SPM_VOL_LV, 0);
	else if (clk_opp == CLK_OPP1)
		DRV_SetReg32(SCP_SCP2SPM_VOL_LV, 1);
	else
		DRV_SetReg32(SCP_SCP2SPM_VOL_LV, 2);
}

/* scp_request_freq
 * return :-1 means the scp request freq. error
 * return :0  means the request freq. finished
 */
int scp_request_freq(void)
{
	int value = 0;
	int timeout = 250;
	int ret = 0;
	unsigned long spin_flags;
	int is_increasing_freq = 0;

	if (scp_dvfs_flag != 1) {
		pr_debug("warning: SCP DVFS is OFF\n");
		return 0;
	}

	/* the wake lock keeps the AP up while the SCP updates scp_current_freq */
	__pm_stay_awake(scp_suspend_lock);

	if (scp_current_freq != scp_expected_freq) {
		/* do DVS before DFS if increasing frequency */
		if (scp_current_freq < scp_expected_freq) {
			scp_vcore_request(scp_expected_freq);
			is_increasing_freq = 1;
		}

		/*  turn on PLL */
		scp_pll_ctrl_set(PLL_ENABLE, scp_expected_freq);

		do {
			ret = scp_ipi_send(IPI_DVFS_SET_FREQ,
								(void *)&value,
								sizeof(value),
								0,
								SCP_A_ID);
			if (ret != SCP_IPI_DONE)
				pr_debug("SCP send IPI fail - %d\n", ret);

			mdelay(2);
			timeout -= 1; /*try 250 times, total about 500ms*/
			if (timeout <= 0) {
				pr_err("%s: set freq fail, current(%d)!= expect(%d)\n",
				__func__, scp_current_freq, scp_expected_freq);
				scp_pll_ctrl_set(PLL_DISABLE, scp_expected_freq);
				__pm_relax(scp_suspend_lock);
				WARN_ON(1);
				return -1;
			}

			/* read scp_current_freq again */
			spin_lock_irqsave(&scp_awake_spinlock, spin_flags);
			scp_current_freq = readl(CURRENT_FREQ_REG);
			spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);

		} while (scp_current_freq != scp_expected_freq);

		/* turn off PLL */
		scp_pll_ctrl_set(PLL_DISABLE, scp_expected_freq);

		/* do DVS after DFS if decreasing frequency */
		if (is_increasing_freq == 0)
			scp_vcore_request(scp_expected_freq);
	}

	__pm_relax(scp_suspend_lock);
	pr_notice("set freq OK, %d == %d\n",
			scp_expected_freq, scp_current_freq);
	return 0;
}

/*
 * Enable the SCP mux and pick its parent for an OPP, as stock. clk_pll_0 to
 * clk_pll_6 are the scp_sel parents in mux order, clk26m first.
 */
int scp_pll_ctrl_set(unsigned int pll_ctrl_flag, unsigned int pll_sel)
{
	int ret = 0;

	if (!mt_scp_pll)
		return -ENODEV;

	pr_debug("%s(%d, %d)\n", __func__, pll_ctrl_flag, pll_sel);

	if (pll_ctrl_flag == PLL_ENABLE) {
		if (pre_pll_sel != CLK_OPP2) {
			ret = clk_prepare_enable(mt_scp_pll->clk_mux);
			if (ret) {
				pr_err("ERROR: %s: clk_prepare_enable() failed, %d\n",
					__func__, ret);
				WARN_ON(1);
			}
		} else {
			pr_debug("no need to do clk_prepare_enable()\n");
		}

		switch (pll_sel) {
		case CLK_26M:
			ret = clk_set_parent(
					mt_scp_pll->clk_mux,
					mt_scp_pll->clk_pll0);
			break;
		case CLK_OPP0:
			ret = clk_set_parent(
					mt_scp_pll->clk_mux,
					mt_scp_pll->clk_pll1);
			break;
		case CLK_OPP1:
			ret = clk_set_parent(
					mt_scp_pll->clk_mux,
					mt_scp_pll->clk_pll5);
			break;
		case CLK_OPP2:
			ret = clk_set_parent(
					mt_scp_pll->clk_mux,
					mt_scp_pll->clk_pll6);
			break;
		default:
			pr_err("ERROR: %s: not support opp freq %d\n",
			__func__, pll_sel);
			WARN_ON(1);
			break;
		}

		if (ret) {
			pr_err("clk_set_parent() failed, opp=%d, %d\n",
					pll_sel, ret);
			WARN_ON(1);
		}

		if (pre_pll_sel != pll_sel)
			pre_pll_sel = pll_sel;

	} else if (pll_ctrl_flag == PLL_DISABLE
				&& pll_sel != CLK_OPP2) {
		clk_disable_unprepare(mt_scp_pll->clk_mux);
		pr_debug("clk_disable_unprepare()\n");
	} else {
		pr_debug("no need to do clk_disable_unprepare\n");
	}

	return ret;
}

static void scp_pll_ctrl_ws(struct work_struct *ws)
{
	struct scp_pll_req *req = container_of(ws, struct scp_pll_req, sw.work);

	scp_pll_ctrl_set(req->flag, req->sel);
	kfree(req);
}

/*
 * The SCP asks for the PLL. Stock reads pll_sel one byte into the message
 * (void pointer + 1), so it is read the same way here.
 */
static void scp_pll_ctrl_handler(int id, void *data, unsigned int len)
{
	struct scp_pll_req *req;

	if (len < 5)
		return;
	req = kzalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		pr_err("IPI_SCP_PLL_CTRL request dropped, no memory\n");
		return;
	}
	req->flag = *(unsigned int *)data;
	memcpy(&req->sel, (unsigned char *)data + 1, sizeof(req->sel));
	pr_notice_ratelimited("IPI_SCP_PLL_CTRL flag %u sel %u\n",
		req->flag, req->sel);
	INIT_WORK(&req->sw.work, scp_pll_ctrl_ws);
	scp_schedule_work(&req->sw);
}

#ifdef CONFIG_PROC_FS
static int mt_scp_dvfs_state_proc_show(struct seq_file *m, void *v)
{
	unsigned int scp_state;

	if (!is_scp_ready(SCP_A_ID)) {
		seq_puts(m, "scp status: not ready\n");
		return 0;
	}
	scp_state = readl(SCP_A_SLEEP_DEBUG_REG);
	seq_printf(m, "scp status: %s\n",
		((scp_state & IN_DEBUG_IDLE) == IN_DEBUG_IDLE) ? "idle mode"
		: ((scp_state & ENTERING_SLEEP) == ENTERING_SLEEP) ?
			"enter sleep"
		: ((scp_state & IN_SLEEP) == IN_SLEEP) ?
			"sleep mode"
		: ((scp_state & ENTERING_ACTIVE) == ENTERING_ACTIVE) ?
			"enter active"
		: ((scp_state & IN_ACTIVE) == IN_ACTIVE) ?
			"active mode" : "none of state");
	return 0;
}

static int mt_scp_dvfs_ctrl_proc_show(struct seq_file *m, void *v)
{
	unsigned long spin_flags;
	int i;

	if (is_scp_ready(SCP_A_ID)) {
		spin_lock_irqsave(&scp_awake_spinlock, spin_flags);
		scp_current_freq = readl(CURRENT_FREQ_REG);
		scp_expected_freq = readl(EXPECTED_FREQ_REG);
		spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);
	}
	seq_printf(m, "SCP DVFS: %s\n", (scp_dvfs_flag == 1)?"ON":"OFF");
	seq_printf(m, "SCP frequency: cur=%dMHz, expect=%dMHz\n",
				scp_current_freq, scp_expected_freq);

	for (i = 0; i < NUM_FEATURE_ID; i++)
		seq_printf(m, "feature=%d, freq=%d, enable=%d\n",
			feature_table[i].feature, feature_table[i].freq,
			feature_table[i].enable);

	for (i = 0; i < NUM_SENSOR_TYPE; i++)
		seq_printf(m, "sensor id=%d, freq=%d, enable=%d\n",
			sensor_type_table[i].feature, sensor_type_table[i].freq,
			sensor_type_table[i].enable);

	return 0;
}

/* Read-only: stock's debug, sleep and ctrl writes are left out. */
static int mt_scp_dvfs_create_procfs(void)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("scp_dvfs", NULL);
	if (!dir) {
		pr_err("fail to create /proc/scp_dvfs @ %s()\n", __func__);
		return -ENOMEM;
	}
	if (!proc_create_single("scp_dvfs_state", 0444, dir,
			mt_scp_dvfs_state_proc_show) ||
	    !proc_create_single("scp_dvfs_ctrl", 0444, dir,
			mt_scp_dvfs_ctrl_proc_show)) {
		pr_err("fail to create /proc/scp_dvfs/* @ %s()\n", __func__);
		return -ENOMEM;
	}
	return 0;
}
#endif

/*
 * Log the PMIC settings stock's mt_pmic_sshub_init wrote; nothing is written
 * here. Stock: vcore con0 EN 1, SLEEP_VOSEL_EN 0, con1 VOSEL and VOSEL_SLEEP
 * 0.6 V; vsram_others con0 EN 1, SLEEP_VOSEL_EN 0, con1 both 0.85 V;
 * TOP_CLK_CON0 bit 4 (RG_SRCVOLTEN_LP_EN) 1.
 */
static void scp_log_pmic_sshub(void)
{
	static const unsigned int regs[] = {
		MT6358_BUCK_VCORE_SSHUB_CON0, MT6358_BUCK_VCORE_SSHUB_CON1,
		MT6358_LDO_VSRAM_OTHERS_SSHUB_CON0,
		MT6358_LDO_VSRAM_OTHERS_SSHUB_CON1, MT6358_TOP_CLK_CON0,
	};
	unsigned int val[ARRAY_SIZE(regs)] = { 0 };
	struct device_node *np, *wrap;
	struct platform_device *pdev = NULL;
	struct regmap *map = NULL;
	int i;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	wrap = np ? of_get_parent(np) : NULL;
	of_node_put(np);
	if (wrap) {
		pdev = of_find_device_by_node(wrap);
		of_node_put(wrap);
	}
	if (pdev) {
		map = dev_get_regmap(&pdev->dev, NULL);
		put_device(&pdev->dev);
	}
	if (!map) {
		pr_notice("PMIC regmap not found, SSHUB settings not read\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		regmap_read(map, regs[i], &val[i]);
	pr_notice("PMIC SSHUB vcore en %u sleep_vosel_en %u vosel 0x%x vosel_sleep 0x%x (con0 0x%x con1 0x%x)\n",
		val[0] & 1, (val[0] >> 1) & 1, val[1] & 0x7f, (val[1] >> 8) & 0x7f,
		val[0], val[1]);
	pr_notice("PMIC SSHUB vsram_others en %u sleep_vosel_en %u vosel 0x%x vosel_sleep 0x%x (con0 0x%x con1 0x%x)\n",
		val[2] & 1, (val[2] >> 1) & 1, val[3] & 0x7f, (val[3] >> 8) & 0x7f,
		val[2], val[3]);
	pr_notice("PMIC TOP_CLK_CON0 0x%x, RG_SRCVOLTEN_LP_EN %u\n",
		val[4], (val[4] >> 4) & 1);
}

/* Check the V_REQ pin (GPIO 154 mode 1), which the SCP uses for its vcore request. */
static void scp_check_vreq_pin(void)
{
	struct device_node *node;
	unsigned int gpio_mode;

	node = of_find_compatible_node(NULL, NULL, "mediatek,gpio");
	if (!node) {
		pr_err("error: can't find GPIO node\n");
		return;
	}
	gpio_base = of_iomap(node, 0);
	of_node_put(node);
	if (!gpio_base) {
		pr_err("error: iomap fail for GPIO\n");
		return;
	}

	gpio_mode = (DRV_Reg32(RG_GPIO154_MODE)>>GPIO154_BIT)&GPIO154_MASK;
	if (gpio_mode == 1)
		pr_debug("v_req muxpin setting is correct\n");
	else
		pr_err("error: V_REQ muxpin setting is wrong - %d\n",
			gpio_mode);
}

static const struct of_device_id scpdvfs_of_ids[] = {
	{.compatible = "mediatek,scp_dvfs",},
	{}
};

static int mt_scp_dvfs_pdrv_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	static const char *const names[] = {
		"clk_pll_0", "clk_pll_1", "clk_pll_2", "clk_pll_3",
		"clk_pll_4", "clk_pll_5", "clk_pll_6",
	};
	struct clk **plls[ARRAY_SIZE(names)];
	int i;

	mt_scp_pll = devm_kzalloc(dev, sizeof(*mt_scp_pll), GFP_KERNEL);
	if (!mt_scp_pll)
		return -ENOMEM;

	plls[0] = &mt_scp_pll->clk_pll0;
	plls[1] = &mt_scp_pll->clk_pll1;
	plls[2] = &mt_scp_pll->clk_pll2;
	plls[3] = &mt_scp_pll->clk_pll3;
	plls[4] = &mt_scp_pll->clk_pll4;
	plls[5] = &mt_scp_pll->clk_pll5;
	plls[6] = &mt_scp_pll->clk_pll6;

	mt_scp_pll->clk_mux = devm_clk_get(dev, "clk_mux");
	if (IS_ERR(mt_scp_pll->clk_mux)) {
		i = PTR_ERR(mt_scp_pll->clk_mux);
		mt_scp_pll = NULL;
		return dev_err_probe(dev, i, "cannot get clock mux\n");
	}
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		*plls[i] = devm_clk_get(dev, names[i]);
		if (IS_ERR(*plls[i])) {
			int err = PTR_ERR(*plls[i]);

			mt_scp_pll = NULL;
			return dev_err_probe(dev, err, "cannot get %s\n",
				names[i]);
		}
	}

	scp_check_vreq_pin();
	scp_log_pmic_sshub();

	g_scp_dvfs_init_flag = true;
	return 0;
}

static struct platform_driver mt_scp_dvfs_pdrv = {
	.probe = mt_scp_dvfs_pdrv_probe,
	.driver = {
		.name = "scpdvfs",
		.of_match_table = scpdvfs_of_ids,
		.suppress_bind_attrs = true,
	},
};

/* Called from scp_init before the SCP driver registers. */
int __init scp_dvfs_init(void)
{
	int ret;

#ifdef CONFIG_PROC_FS
	if (mt_scp_dvfs_create_procfs())
		pr_err("mt_scp_dvfs_create_procfs fail..\n");
#endif

	scp_suspend_lock = wakeup_source_register(NULL, "scp wakelock");
	if (!scp_suspend_lock)
		return -ENOMEM;

	ret = platform_driver_register(&mt_scp_dvfs_pdrv);
	if (ret) {
		pr_err("fail to register scp dvfs driver @ %s()\n", __func__);
		return ret;
	}

	scp_ipi_registration(IPI_SCP_PLL_CTRL,
						scp_pll_ctrl_handler,
						"IPI_SCP_PLL_CTRL");
	return 0;
}
