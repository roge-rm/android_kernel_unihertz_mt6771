// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 vcorefs init, T's spm_vcorefs_init
 * (spm_v4/mtk_spm_vcorefs_mt6771.c). By default on its "VCORE DVFS IS
 * DISABLE" branch, the one stock recovery ran:
 * - seg, efuse and hw_rsv words from LK's atag,devinfo, the Vcore OPP table
 *   (helio-dvfsrc-opp-mt6771.c) and lt_opp as stock
 * - SSPM QOS IPIs (P11c): QOS_ENABLE before the table, then VCORE_OPP and
 *   DDR_OPP for each OPP, as vcorefs_v3's vcorefs_init_opp_table
 * - SIP VCOREFS CMD_3 twice: ATF stores the 725 and 800 mV PMIC values in
 *   SPM_DVFS_CMD0 and CMD1, the PCM firmware's PMIC wrap table
 * - CMD_0 (boot OPP 0) and CMD_1 with flag 0x418: ATF sets up the SPM DVFS
 *   level registers and hands the PCM firmware RUN_COMMON_SCENARIO with
 *   DIS_VCORE_DVS and DIS_VCORE_DFS
 * - dvfsrc_init with the LP4X 3733 values, QOS_EN 0, then the TOPRGU DVFSRC
 *   enable and latch bits (mtk_rgu_cfg_dvfsrc)
 * - the pm_qos backend (P11d): helio-dvfsrc.c's commit_data puts the EMI_OPP,
 *   VCORE_OPP, FIXED_OPP and the 5 bandwidth classes into DVFSRC_SW_REQ,
 *   VCORE_REQUEST2, FORCE and SW_BW_0..4; the SCP's VCORE_REQUEST [31:30]
 *   is kept until DVFS runs, as stock; the fb blank notifier writes
 *   EMI_MD2SPM0 as stock's
 * With spm_vcorefs.dvfs=1 as well (P11e) and stock's feature check passed,
 * the other branch: QOS_ENABLE with dvfs_en 1, CMD_1 with spm_dvfs_flag_init's
 * flag (0x8400, DVS and DFS on), QOS_EN 0x407f and EMI_REQUEST3, the
 * LATE_INIT_OPP kick, dvfsrc_init_qos_opp, and in the backend the PCM status
 * check before DVFS, the force and skip checks and the Vcore read-back.
 * init and dvfs are both on by default (P11f). Writing 1 to
 * /sys/module/spm_vcorefs/parameters/dvfs after a boot with
 * spm_vcorefs.dvfs=0 runs the same branch then (P13), one way.
 * Debugfs spm/force_opp pins an OPP for tests. The PCM firmware then moves
 * Vcore and DRAM; nothing here writes the PMIC or the DRAMC. It runs only
 * with spm_vcorefs.init=1 (the default); without it the pm_qos classes are
 * only kept (kernel/power/qos_mtk.c) and debugfs spm/qos shows them.
 */
#define pr_fmt(fmt) "spm_vcorefs: " fmt

#include <linux/arm-smccc.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_qos.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/soc/mediatek/mtk-spm.h>
#include <linux/soc/mediatek/mtk-sspm.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "spm_vcorefs."

/* On by default; spm_vcorefs.init=0 leaves vcorefs off */
static bool init_en = true;
module_param_named(init, init_en, bool, 0444);
MODULE_PARM_DESC(init, "Run vcorefs init at boot");

/* T's spm_v4/mtk_spm_reg_mt6771.h */
#define PCM_REG15_DATA		0x13c
#define SPM_SW_RSV_5		0x61c
#define SPM_DFS_LEVEL		0x6b0
#define SPM_DVS_LEVEL		0x6b4

/* T's spm_v4/mtk_dvfsrc_reg_mt6771.h */
#define DVFSRC_BASIC_CONTROL	0x000
#define DVFSRC_SW_REQ		0x004
#define DVFSRC_EMI_REQUEST	0x00c
#define DVFSRC_EMI_REQUEST2	0x010
#define DVFSRC_EMI_REQUEST3	0x014
#define DVFSRC_EMI_QOS0		0x024
#define DVFSRC_EMI_QOS1		0x028
#define DVFSRC_EMI_MD2SPM0	0x030
#define DVFSRC_EMI_MD2SPM1	0x034
#define DVFSRC_VCORE_REQUEST	0x048
#define DVFSRC_VCORE_REQUEST2	0x04c
#define DVFSRC_VCORE_MD2SPM0	0x068
#define DVFSRC_MD_SW_CONTROL	0x084
#define DVFSRC_INT_EN		0x09c
#define DVFSRC_TIMEOUT_NEXTREQ	0x0d8
#define DVFSRC_LEVEL		0x0dc
#define DVFSRC_LEVEL_LABEL_0_1	0x0e0
#define DVFSRC_LEVEL_LABEL_2_3	0x0e4
#define DVFSRC_LEVEL_LABEL_4_5	0x0e8
#define DVFSRC_LEVEL_LABEL_6_7	0x0ec
#define DVFSRC_LEVEL_LABEL_8_9	0x0f0
#define DVFSRC_LEVEL_LABEL_10_11	0x0f4
#define DVFSRC_LEVEL_LABEL_12_13	0x0f8
#define DVFSRC_LEVEL_LABEL_14_15	0x0fc
#define DVFSRC_QOS_EN		0x180
#define DVFSRC_SW_BW_0		0x160
#define DVFSRC_SW_BW_1		0x164
#define DVFSRC_SW_BW_2		0x168
#define DVFSRC_SW_BW_3		0x16c
#define DVFSRC_SW_BW_4		0x170
#define DVFSRC_FORCE		0x300
#define DVFSRC_RSRV_1		0x604

/* TOPRGU, T's wdt_v2/mtk_wdt.h */
#define MTK_WDT_LATCH_CTL		0x044
#define MTK_WDT_DEBUG_CTL2		0x0a0
#define MTK_WDT_DEBUG_CTL2_KEY		0x55000000
#define MTK_WDT_DEBUG_CTL_DVFSRC_EN	BIT(9)
#define MTK_WDT_LATCH_CTL_KEY		0x95000000
#define MTK_WDT_LATCH_CTL_DVFSRC	BIT(13)

/* SIP VCOREFS sub-ids, T's spm_v4/mtk_spm_vcore_dvfs_mt6771.h */
#define VCOREFS_SMC_CMD_0	0
#define VCOREFS_SMC_CMD_1	1
#define VCOREFS_SMC_CMD_3	3

/* PCM flags, T's spm_v4/sleep_def_mt6771.h */
#define SPM_FLAG_DIS_VCORE_DVS		BIT(3)
#define SPM_FLAG_DIS_VCORE_DFS		BIT(4)
#define SPM_FLAG_RUN_COMMON_SCENARIO	BIT(10)
#define SPM_FLAG_DISABLE_MMSYS_DVFS	BIT(15)
/* T's VMODEM_VCORE_COBUCK branch, 0x418 in stock recovery's log */
#define SPM_VCOREFS_FLAG_OFF	(SPM_FLAG_RUN_COMMON_SCENARIO | \
				 SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS)

/* T's mtk_spm_vcore_dvfs_mt6771.h: DVS and DFS on, MM clock DVFS off */
#define SPM_VCORE_DVS_EN	1
#define SPM_DDR_DFS_EN		1
#define SPM_MM_CLK_EN		0

/* dvfsrc_init with MTK_QOS_SUPPORT, as the stock Image (DVS and DFS on only) */
#define QOS_EN_QOS_SUPPORT	0x0000407f
#define EMI_REQUEST3_QOS_SUPPORT	0x09000000

/* MT6358 VCORE VOSEL: 500 mV plus 6.25 mV a step, rounded up */
#define VCORE_BASE_UV		500000
#define VCORE_STEP_UV		6250
#define vcore_uv_to_pmic(uv)	DIV_ROUND_UP((uv) - VCORE_BASE_UV, VCORE_STEP_UV)

#define NUM_OPP			4
/* T's BOOT_UP_OPP, what spm_vcorefs_get_opp returns while vcorefs can't work */
#define BOOT_UP_OPP		0
/* T's LATE_INIT_OPP for mt6771: the lowest OPP, where the init kick asks for */
#define LATE_INIT_OPP		(NUM_OPP - 1)

/* T's helio-dvfsrc-opp-mt6771.h: DRAM and Vcore levels the requests name */
#define DDR_OPP_NUM		3
#define VCORE_OPP_NUM		2
/* build_vcore_opp_table for LP4X 3733: each OPP's Vcore and DRAM level */
static const int vcore_dvfs_to_vcore_opp[NUM_OPP] = { 0, 0, 1, 1 };
static const int vcore_dvfs_to_ddr_opp[NUM_OPP] = { 0, 1, 1, 2 };

/* T's DVFSRC_TIMEOUT and mt6771 SPM_DVFS_TIMEOUT, in 1 us polls */
#define DVFSRC_TIMEOUT		1000
#define SPM_DVFS_TIMEOUT	1000

/* dvfsrc_md_scenario_update for LP4X 3733: screen on, screen off */
#define EMI_MD2SPM0_SCREEN_ON	0x80f8
#define EMI_MD2SPM0_SCREEN_OFF	0x80c0

/* T's vcore_opp_L4_2CH_CASE2 (LP4X 3733), columns by soc efuse */
static const unsigned int vcore_opp_lp4x_3733[NUM_OPP][2] = {
	{ 800000, 800000 },
	{ 800000, 800000 },
	{ 725000, 700000 },
	{ 725000, 700000 },
};

/* DRAM rate per OPP in kHz, from stock recovery's "opp n" lines */
static const unsigned int ddr_opp_khz[NUM_OPP] = {
	3600000, 3200000, 3200000, 1600000
};

/* seg_info_init: these segments can't run DRAM above 3.3 GHz with DVFS */
#define SEG_P38_6M		0x24
#define SEG_P38_5M		0x34
#define P38_DDR_MAX_KHZ		3300000

/*
 * lt_opp as stock: tscpu_min_temperature is the weak one in the stock Image
 * (40), above enter_lt_opp_temp (0), so init never adds the 12.5 mV.
 */
static int lt_opp_init_temp = 40;
static int enter_lt_opp_temp;
#define LT_OPP_ADJ_UV		12500

/* SSPM QOS IPI commands and payload, T's spm_v4/mtk_spm_vcore_dvfs_ipi_mt6771.h */
#define QOS_IPI_QOS_ENABLE	0
#define QOS_IPI_VCORE_OPP	1
#define QOS_IPI_DDR_OPP		2

struct qos_data {
	u32 cmd;
	union {
		struct {
			u32 enable;
			u32 dvfs_en;
			u32 spm_dram_type;
		} qos_init;
		struct {
			u32 opp;
			u32 vcore_uv;
		} vcore_opp;
		struct {
			u32 opp;
			u32 ddr_khz;
		} ddr_opp;
	} u;
};

static DEFINE_SPINLOCK(vcorefs_lock);
static void __iomem *dvfsrc_base;
static void __iomem *qos_sram_base;
static void __iomem *toprgu_base;
static unsigned int vcore_opp_uv[NUM_OPP];
static u32 seg_info, soc_efuse_word, hw_rsv;
static bool lt_opp_feature_en = true;
/* the governor's vcore_dvs and ddr_dfs; seg_info_init can turn both off */
static bool vcore_dvs_en = SPM_VCORE_DVS_EN;
static bool ddr_dfs_en = SPM_DDR_DFS_EN;
/* is_vcorefs_feature_enable() at init, only asked with spm_vcorefs.dvfs=1 */
static bool feature_en;
static u32 pcm_flag;
static bool init_done;
static bool qos_on;
static int qos_err;
static unsigned int qos_sent;

/*
 * On the command line it picks the init branch, on by default (P11f). Written
 * 1 after a boot with dvfs=0, init done on the off branch, it turns DVS and
 * DFS on then (one way).
 */
static bool dvfs_en = true;
static bool vcorefs_booted;	/* the late_initcall ran: a write is a run-time switch */
static int vcorefs_dvfs_turn_on(void);
static int vcorefs_dvfs_check(void);

/*
 * The run-time turn-on runs here, not in the parameter write: sysfs holds the
 * global param_lock around .set, and the turn-on waits on SSPM and the PCM
 * firmware. The result is in dmesg; the parameter reads Y once it is on.
 */
static void vcorefs_dvfs_on_fn(struct work_struct *work)
{
	int ret = vcorefs_dvfs_turn_on();

	if (ret)
		pr_err("DVS and DFS not turned on at run time (%d)\n", ret);
}
static DECLARE_WORK(vcorefs_dvfs_on_work, vcorefs_dvfs_on_fn);

static int dvfs_param_set(const char *val, const struct kernel_param *kp)
{
	bool on = true;
	int ret;

	if (val) {
		ret = kstrtobool(val, &on);
		if (ret)
			return ret;
	}
	if (!READ_ONCE(vcorefs_booted)) {
		dvfs_en = on;
		return 0;
	}
	if (!on)
		return READ_ONCE(feature_en) ? -EPERM : 0;
	ret = vcorefs_dvfs_check();
	if (ret <= 0)
		return ret;
	/* freezable: suspend waits for a turn-on in progress */
	queue_work(system_freezable_wq, &vcorefs_dvfs_on_work);
	return 0;
}

static const struct kernel_param_ops dvfs_param_ops = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = dvfs_param_set,
	.get = param_get_bool,
};
module_param_cb(dvfs, &dvfs_param_ops, &dvfs_en, 0644);
MODULE_PARM_DESC(dvfs, "Turn on Vcore DVS and DRAM DFS (needs init=1); 1 after boot turns them on, no way back");

static void vcorefs_sip(unsigned long cmd, unsigned long a2, unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, cmd, a2, a3, 0, 0, 0, 0,
		      &res);
}

/* LK's atag,devinfo in /chosen; word 0 is the count */
static int vcorefs_devinfo(int index, u32 *val)
{
	struct device_node *chosen;
	const u8 *tag;
	int length, ret = -ENODEV;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return ret;
	tag = of_get_property(chosen, "atag,devinfo", &length);
	if (tag && length >= (int)sizeof(u32) &&
	    (u32)index < get_unaligned_le32(tag) &&
	    (index + 2) * sizeof(u32) <= (size_t)length) {
		*val = get_unaligned_le32(tag + sizeof(u32) * (index + 1));
		ret = 0;
	}
	of_node_put(chosen);
	return ret;
}

static void seg_info_init(void)
{
	pr_info("seg_info 0x%x\n", seg_info);
	if (((seg_info & 0xff) == SEG_P38_6M || (seg_info & 0xff) == SEG_P38_5M) &&
	    ddr_opp_khz[0] > P38_DDR_MAX_KHZ) {
		vcore_dvs_en = false;
		ddr_dfs_en = false;
		pr_info("disable dvfs due to segment\n");
	}
}

/*
 * T's dram_can_support_fh without the DRAMC driver: No_DummyRead is clear when
 * /memory has LK's orig_dram_info with 1 or 2 ranks and each rank's
 * dummy-read page is reserved (at least 4 KB). The second test, segment bit
 * 0x20 at a 3600 data rate, uses 3600 since we boot and init at OPP 0.
 * The kernel only checks; the pages' addresses never go to ATF on mt6771.
 */
#define DRAM_RSV_SIZE		0x1000
#define SEG_NO_FH_AT_3600	0x20

static bool dram_rank_rsv_ok(const char *path)
{
	struct device_node *np = of_find_node_by_path(path);
	struct reserved_mem *rmem;

	if (!np)
		return false;
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	return rmem && rmem->size >= DRAM_RSV_SIZE;
}

static bool dram_can_support_fh(void)
{
	struct device_node *np;
	const u8 *info;
	u32 rank_num = 0;
	int len;

	np = of_find_node_by_path("/memory");
	if (np) {
		info = of_get_property(np, "orig_dram_info", &len);
		if (info && len >= (int)sizeof(u32))
			rank_num = get_unaligned_le32(info);
		of_node_put(np);
	}
	pr_info("orig_dram_info rank_num %u\n", rank_num);
	if (rank_num != 1 && rank_num != 2)
		return false;
	if (!dram_rank_rsv_ok("/reserved-memory/reserve-memory-dram_r0_dummy_read"))
		return false;
	if (rank_num == 2 &&
	    !dram_rank_rsv_ok("/reserved-memory/reserve-memory-dram_r1_dummy_read"))
		return false;
	return !(seg_info & SEG_NO_FH_AT_3600);
}

/*
 * is_vcorefs_feature_enable (vcorefs_v3 governor). Stock's LP4 2400
 * firmware status 2 test is left out: init already needs LP4X 3733.
 */
static bool is_vcorefs_feature_enable(void)
{
	if (!dram_can_support_fh()) {
		pr_err("DISABLE DVFS DUE TO NOT SUPPORT DRAM FH\n");
		return false;
	}
	if (!spm_load_firmware_status()) {
		pr_err("SPM FIRMWARE IS NOT READY\n");
		return false;
	}
	if (!vcore_dvs_en && !ddr_dfs_en) {
		pr_err("DISABLE DVFS DUE TO BOTH DVS & DFS DISABLE\n");
		return false;
	}
	return true;
}

/* spm_dvfs_flag_init: 0x8400 on this phone (DVS and DFS on, MMSYS DVFS off) */
static u32 spm_dvfs_flag_init(void)
{
	u32 flag = SPM_FLAG_RUN_COMMON_SCENARIO;

	if (!vcore_dvs_en)
		flag |= SPM_FLAG_DIS_VCORE_DVS;
	if (!ddr_dfs_en)
		flag |= SPM_FLAG_DIS_VCORE_DFS;
	if (!SPM_MM_CLK_EN)
		flag |= SPM_FLAG_DISABLE_MMSYS_DVFS;
	return flag;
}

/* build_vcore_opp_table for LP4X 3733: OPP 0 and 1 stay 800 mV */
static void vcore_opp_init(void)
{
	unsigned int soc_efuse = (soc_efuse_word >> 12) & 0x3;
	int i;

	pr_info("efuse 0x%x soc_efuse 0x%x\n", soc_efuse_word, soc_efuse);
	if (soc_efuse > 1) {
		pr_info("wrong vcore efuse %u, default table\n", soc_efuse);
		soc_efuse = 0;
	}
	for (i = 0; i < NUM_OPP; i++)
		vcore_opp_uv[i] = vcore_opp_lp4x_3733[i][i < 2 ? 0 : soc_efuse];
	for (i = NUM_OPP - 2; i >= 0; i--)
		vcore_opp_uv[i] = max(vcore_opp_uv[i], vcore_opp_uv[i + 1]);
	pr_info("table %u %u %u %u\n", vcore_opp_uv[0], vcore_opp_uv[1],
		vcore_opp_uv[2], vcore_opp_uv[3]);
}

static void vcorefs_temp_opp_init(void)
{
	if (!lt_opp_feature_en)
		return;
	pr_info("lt_opp init temp %d, enter below %d\n", lt_opp_init_temp,
		enter_lt_opp_temp);
	if (lt_opp_init_temp < enter_lt_opp_temp) {
		vcore_opp_uv[0] += LT_OPP_ADJ_UV;
		vcore_opp_uv[1] = vcore_opp_uv[0];
	}
}

/* spm_vcorefs_pwarp_cmd: PMIC wrap table entries 0 (OPP 3) and 1 (OPP 0) */
static void spm_vcorefs_pwarp_cmd(void)
{
	unsigned long flags;

	spin_lock_irqsave(&vcorefs_lock, flags);
	vcorefs_sip(VCOREFS_SMC_CMD_3, 0, vcore_uv_to_pmic(vcore_opp_uv[3]));
	vcorefs_sip(VCOREFS_SMC_CMD_3, 1, vcore_uv_to_pmic(vcore_opp_uv[0]));
	spin_unlock_irqrestore(&vcorefs_lock, flags);
}

/*
 * qos_ipi_to_sspm_command: polling send on IPI_ID_QOS with one ack slot, as
 * stock. The first failure (no ack, or SSPM IPI not up) turns QoS off and
 * the rest of the QOS IPIs are skipped; vcorefs init goes on without them.
 * Polling holds the pin's spinlock, so don't call it under vcorefs_lock.
 */
static void qos_ipi_to_sspm_command(struct qos_data *qos_d, int slot)
{
	int ack_data = 0, ret;

	if (!qos_on)
		return;
	ret = sspm_ipi_send_sync(IPI_ID_QOS, IPI_OPT_POLLING, qos_d, slot,
				 &ack_data, 1);
	if (ret) {
		qos_on = false;
		qos_err = ret;
		pr_err("SSPM QOS cmd %u failed (%d), QoS left off\n", qos_d->cmd,
		       ret);
		return;
	}
	qos_sent++;
}

/* helio_dvfsrc_sspm_ipi_init without the qos_recv thread (no receive in 0069) */
static void helio_dvfsrc_sspm_ipi_init(int dvfs_en, int dram_type)
{
	struct qos_data qos_d = { .cmd = QOS_IPI_QOS_ENABLE };

	qos_d.u.qos_init.enable = 1;
	qos_d.u.qos_init.dvfs_en = dvfs_en;
	qos_d.u.qos_init.spm_dram_type = dram_type;
	qos_on = true;
	qos_ipi_to_sspm_command(&qos_d, 4);
}

static void dvfsrc_update_sspm_vcore_opp_table(int opp, unsigned int vcore_uv)
{
	struct qos_data qos_d = { .cmd = QOS_IPI_VCORE_OPP };

	qos_d.u.vcore_opp.opp = opp;
	qos_d.u.vcore_opp.vcore_uv = vcore_uv;
	qos_ipi_to_sspm_command(&qos_d, 3);
}

static void dvfsrc_update_sspm_ddr_opp_table(int opp, unsigned int ddr_khz)
{
	struct qos_data qos_d = { .cmd = QOS_IPI_DDR_OPP };

	qos_d.u.ddr_opp.opp = opp;
	qos_d.u.ddr_opp.ddr_khz = ddr_khz;
	qos_ipi_to_sspm_command(&qos_d, 3);
}

/* vcorefs_init_opp_table: each OPP's Vcore then DRAM rate to SSPM, then pwarp */
static void vcorefs_init_opp_table(void)
{
	int opp;

	for (opp = 0; opp < NUM_OPP; opp++) {
		dvfsrc_update_sspm_vcore_opp_table(opp, vcore_opp_uv[opp]);
		dvfsrc_update_sspm_ddr_opp_table(opp, ddr_opp_khz[opp]);
		pr_info("opp %d: vcore_uv %u ddr_khz %u\n", opp, vcore_opp_uv[opp],
			ddr_opp_khz[opp]);
	}
	spm_vcorefs_pwarp_cmd();
}

static void spm_dvfsfw_init(int curr_opp)
{
	unsigned long flags;

	spin_lock_irqsave(&vcorefs_lock, flags);
	vcorefs_sip(VCOREFS_SMC_CMD_0, curr_opp, 0);
	spin_unlock_irqrestore(&vcorefs_lock, flags);
}

static void spm_go_to_vcorefs(u32 flag)
{
	unsigned long flags;

	/* 0x418, or stock's DVS and DFS on flag once the feature check passed */
	if (WARN_ON(flag != SPM_VCOREFS_FLAG_OFF &&
		    !(feature_en && flag == spm_dvfs_flag_init())))
		return;
	pr_info("pcm_flag 0x%x\n", flag);
	spin_lock_irqsave(&vcorefs_lock, flags);
	vcorefs_sip(VCOREFS_SMC_CMD_1, flag, 0);
	spin_unlock_irqrestore(&vcorefs_lock, flags);
	pcm_flag = flag;
}

/*
 * For the deep suspend ops (mtk_spm_suspend.c): true once init sent CMD_1,
 * with that flag. ATF's suspend path copies its DVS, DFS and MMSYS bits
 * into the suspend flags.
 */
bool spm_vcorefs_init_done(u32 *flag)
{
	if (flag)
		*flag = pcm_flag;
	return init_done && pcm_flag;
}

/* mtk_rgu_cfg_dvfsrc(1): TOPRGU DVFSRC enable and latch */
static void mtk_rgu_cfg_dvfsrc(void)
{
	u32 dbg_ctl, latch;

	if (!toprgu_base)
		return;
	dbg_ctl = readl(toprgu_base + MTK_WDT_DEBUG_CTL2);
	latch = readl(toprgu_base + MTK_WDT_LATCH_CTL);
	writel(dbg_ctl | MTK_WDT_DEBUG_CTL_DVFSRC_EN | MTK_WDT_DEBUG_CTL2_KEY,
	       toprgu_base + MTK_WDT_DEBUG_CTL2);
	writel(latch | MTK_WDT_LATCH_CTL_DVFSRC | MTK_WDT_LATCH_CTL_KEY,
	       toprgu_base + MTK_WDT_LATCH_CTL);
	pr_info("rgu DEBUG_CTL2 0x%x LATCH_CTL 0x%x\n",
		readl(toprgu_base + MTK_WDT_DEBUG_CTL2),
		readl(toprgu_base + MTK_WDT_LATCH_CTL));
}

static bool force_opp_enable;
/* the last fb blank event; written with EMI_MD2SPM0 under vcorefs_lock */
static bool is_screen_off;

/*
 * dvfsrc_init, LP4X 3733 branch. With DVS and DFS off QOS_EN stays 0 (P11b);
 * on, it writes MTK_QOS_SUPPORT's QOS_EN 0x407f and EMI_REQUEST3 in its place,
 * as the stock Image.
 */
static void dvfsrc_init(void)
{
	static const u32 regs[][2] = {
		{ DVFSRC_LEVEL_LABEL_0_1, 0x00100000 },
		{ DVFSRC_LEVEL_LABEL_2_3, 0x00210011 },
		{ DVFSRC_LEVEL_LABEL_4_5, 0x01100100 },
		{ DVFSRC_LEVEL_LABEL_6_7, 0x01210111 },
		{ DVFSRC_LEVEL_LABEL_8_9, 0x02100200 },
		{ DVFSRC_LEVEL_LABEL_10_11, 0x02210211 },
		{ DVFSRC_LEVEL_LABEL_12_13, 0x03210321 },
		{ DVFSRC_LEVEL_LABEL_14_15, 0x03210321 },
		{ DVFSRC_EMI_QOS0, 0x32 },
		{ DVFSRC_EMI_QOS1, 0x66 },
		{ DVFSRC_EMI_MD2SPM0, 0x80f8 },
		{ DVFSRC_EMI_MD2SPM1, 0x0 },
		{ DVFSRC_VCORE_MD2SPM0, 0x80c0 },
		{ DVFSRC_RSRV_1, 0x1c },
		{ DVFSRC_TIMEOUT_NEXTREQ, 0x13 },
		{ DVFSRC_INT_EN, 0x2 },
		{ DVFSRC_EMI_REQUEST, 0x00290209 },
		{ DVFSRC_EMI_REQUEST2, 0x0 },
		{ DVFSRC_VCORE_REQUEST, 0x00150000 },
		{ DVFSRC_QOS_EN, 0x0 },
		{ DVFSRC_FORCE, 0x00400000 },
		{ DVFSRC_BASIC_CONTROL, 0xc07b },
		{ DVFSRC_BASIC_CONTROL, 0x017b },
	};
	unsigned long flags;
	int i;

	if (force_opp_enable)
		return;
	spin_lock_irqsave(&vcorefs_lock, flags);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		if (feature_en && regs[i][0] == DVFSRC_QOS_EN) {
			writel(QOS_EN_QOS_SUPPORT, dvfsrc_base + DVFSRC_QOS_EN);
			mb();
			writel(EMI_REQUEST3_QOS_SUPPORT,
			       dvfsrc_base + DVFSRC_EMI_REQUEST3);
			mb();
			continue;
		}
		if (regs[i][0] == DVFSRC_EMI_MD2SPM0) {
			/* stock's 0x80f8 at boot; the screen state on a run-time turn-on */
			writel(is_screen_off ? EMI_MD2SPM0_SCREEN_OFF :
			       EMI_MD2SPM0_SCREEN_ON, dvfsrc_base + DVFSRC_EMI_MD2SPM0);
			mb();
			continue;
		}
		writel(regs[i][1], dvfsrc_base + regs[i][0]);
		mb();
	}
	mtk_rgu_cfg_dvfsrc();
	spin_unlock_irqrestore(&vcorefs_lock, flags);
}

/*
 * Stock's is_vcorefs_can_work() == 1: init done with DVS and DFS on (P11e,
 * spm_vcorefs.dvfs=1). Until then the SPM OPP reads as BOOT_UP_OPP and the
 * SCP request is only kept, as stock on the DVS and DFS off branch.
 */
static bool vcorefs_can_work(void)
{
	return init_done &&
	       !(pcm_flag & (SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS));
}

/* spm_vcorefs_get_opp: the DVFSRC level, DVFSRC_LEVEL [31:16] */
static int spm_vcorefs_get_opp(void)
{
	u32 level;

	if (!vcorefs_can_work())
		return BOOT_UP_OPP;
	level = readl(dvfsrc_base + DVFSRC_LEVEL) >> 16;
	/*
	 * One-hot: 0x8 is OPP 0 down to 0x1 OPP 3, then 0x10 is 4 up to 0x8000
	 * 15. Anything else is returned as read, as stock.
	 */
	switch (level) {
	case 0x8:
		return 0;
	case 0x4:
		return 1;
	case 0x2:
		return 2;
	case 0x1:
		return 3;
	default:
		if (level >= 0x10 && is_power_of_2(level))
			return __ffs(level);
		return level;
	}
}

/* spm_vcorefs_get_dvfs_opp: the OPP the PCM firmware reports in SW_RSV_5 */
static int spm_vcorefs_get_dvfs_opp(void)
{
	if (!vcorefs_can_work())
		return BOOT_UP_OPP;
	switch (readl(spm_base + SPM_SW_RSV_5) & 0xffff) {
	case 0x8: case 0x80: case 0x800:
		return 0;
	case 0x4: case 0x40: case 0x400:
		return 1;
	case 0x2: case 0x20: case 0x200:
		return 2;
	case 0x1: case 0x10: case 0x100: case 0x1000:
		return 3;
	default:
		return BOOT_UP_OPP;
	}
}

/* get_min_opp_for_ddr and get_min_opp_for_vcore: the lowest OPP that meets a level */
static int get_min_opp(const int *table, int level)
{
	int i;

	for (i = NUM_OPP - 1; i >= 0; i--)
		if (table[i] == level)
			return i;
	return 0;
}

/* T's wait_for_completion macro: 1 us polls, -EBUSY on timeout */
static int dvfsrc_wait_idle(void)
{
	int i;

	for (i = 0; readl(dvfsrc_base + DVFSRC_LEVEL) & 0xffff; i++) {
		if (i >= DVFSRC_TIMEOUT)
			return -EBUSY;
		udelay(1);
	}
	return 0;
}

static int spm_wait_dvfs_opp(int opp, bool exact)
{
	int i, cur;

	for (i = 0; ; i++) {
		cur = spm_vcorefs_get_dvfs_opp();
		if (exact ? cur == opp : cur <= opp)
			return 0;
		if (i >= SPM_DVFS_TIMEOUT)
			return -EBUSY;
		udelay(1);
	}
}

static void dvfsrc_rmw(u32 reg, u32 clear, u32 set)
{
	unsigned long flags;

	spin_lock_irqsave(&vcorefs_lock, flags);
	writel((readl(dvfsrc_base + reg) & ~clear) | set, dvfsrc_base + reg);
	spin_unlock_irqrestore(&vcorefs_lock, flags);
}

static unsigned int pcm_rekicks;

/*
 * spm_check_status_before_dvfs: PCM_REG15 reads 0 when the PCM firmware isn't
 * running; kick it again with CMD_0 and CMD_1. Only called on the DVS and
 * DFS on branch, so the flag is always the on one.
 */
static void spm_check_status_before_dvfs(void)
{
	if (readl(spm_base + PCM_REG15_DATA))
		return;
	pcm_rekicks++;
	spm_dvfsfw_init(spm_vcorefs_get_opp());
	spm_go_to_vcorefs(spm_dvfs_flag_init());
}

/* dvfsrc_hw_policy_mask(0): the hardware request sources back on */
static void dvfsrc_hw_policy_unmask(void)
{
	unsigned long flags;

	spin_lock_irqsave(&vcorefs_lock, flags);
	writel(0x00290209, dvfsrc_base + DVFSRC_EMI_REQUEST);
	writel(EMI_REQUEST3_QOS_SUPPORT, dvfsrc_base + DVFSRC_EMI_REQUEST3);
	writel(0x00150000, dvfsrc_base + DVFSRC_VCORE_REQUEST);
	writel(readl(dvfsrc_base + DVFSRC_MD_SW_CONTROL) & ~BIT(3),
	       dvfsrc_base + DVFSRC_MD_SW_CONTROL);
	writel(readl(dvfsrc_base + DVFSRC_MD_SW_CONTROL) & ~BIT(0),
	       dvfsrc_base + DVFSRC_MD_SW_CONTROL);
	writel(readl(dvfsrc_base + DVFSRC_MD_SW_CONTROL) & ~BIT(5),
	       dvfsrc_base + DVFSRC_MD_SW_CONTROL);
	spin_unlock_irqrestore(&vcorefs_lock, flags);
}

/*
 * vcorefs_late_init_dvfs: stock's KIR_LATE_INIT kick to LATE_INIT_OPP through
 * spm_set_vcore_dvfs and spm_trigger_dvfs, not fixed: the status check, the
 * hardware policy unmasked, SW_REQ [3:2] and [1:0] set to OPP 3's requests
 * (none), then the wait. vcorefs can't work yet, so the SPM OPP reads
 * BOOT_UP_OPP and the wait ends at once, as stock. Then vcorefs_drv_init's
 * init_done; a timeout doesn't stop it, as stock.
 */
static void vcorefs_late_init_dvfs(void)
{
	static const u32 vcore_req[NUM_OPP] = { 0x1, 0x1, 0x0, 0x0 };
	static const u32 emi_req[NUM_OPP] = { 0x2, 0x1, 0x1, 0x0 };
	const int opp = LATE_INIT_OPP;

	spm_check_status_before_dvfs();
	force_opp_enable = false;
	dvfsrc_hw_policy_unmask();
	if (dvfsrc_wait_idle()) {
		pr_warn("late init: wait idle timeout, LEVEL 0x%x\n",
			readl(dvfsrc_base + DVFSRC_LEVEL));
	} else {
		dvfsrc_rmw(DVFSRC_SW_REQ, 0x3 << 2, vcore_req[opp] << 2);
		dvfsrc_rmw(DVFSRC_SW_REQ, 0x3, emi_req[opp]);
		if (spm_wait_dvfs_opp(opp, false))
			pr_warn("late init: wait complete timeout\n");
	}
	init_done = true;
	pr_info("late_init_opp %d, SW_REQ 0x%x LEVEL 0x%x SW_RSV_5 0x%x\n", opp,
		readl(dvfsrc_base + DVFSRC_SW_REQ),
		readl(dvfsrc_base + DVFSRC_LEVEL),
		readl(spm_base + SPM_SW_RSV_5));
}

/*
 * dvfsrc_init_qos_opp: the EMI_OPP and VCORE_OPP requests made before init
 * into SW_REQ [1:0] and VCORE_REQUEST2 [25:24], then OPP 0's DRAM rate to
 * SSPM again. A negative request is taken as none here; stock indexed with it.
 */
static void dvfsrc_init_qos_opp(void)
{
	static const u32 vcore_req[VCORE_OPP_NUM] = { 0x1, 0x0 };
	static const u32 emi_req[DDR_OPP_NUM] = { 0x2, 0x1, 0x0 };
	int emi_opp = pm_qos_request(PM_QOS_EMI_OPP);
	int vcore_opp = pm_qos_request(PM_QOS_VCORE_OPP);

	if (emi_opp >= DDR_OPP_NUM || emi_opp < 0)
		emi_opp = DDR_OPP_NUM - 1;
	if (vcore_opp >= VCORE_OPP_NUM || vcore_opp < 0)
		vcore_opp = VCORE_OPP_NUM - 1;
	pr_info("pm_qos curr opp: emi %d (req %u), vcore %d (req %u)\n", emi_opp,
		emi_req[emi_opp], vcore_opp, vcore_req[vcore_opp]);
	if (vcore_req[vcore_opp])
		dvfsrc_rmw(DVFSRC_VCORE_REQUEST2, 0x03000000,
			   vcore_req[vcore_opp] << 24);
	if (emi_req[emi_opp])
		dvfsrc_rmw(DVFSRC_SW_REQ, 0x3, emi_req[emi_opp]);
	pr_info("pm_qos init opp (sw_req 0x%x, vcore_req2 0x%x)\n",
		readl(dvfsrc_base + DVFSRC_SW_REQ),
		readl(dvfsrc_base + DVFSRC_VCORE_REQUEST2));
	dvfsrc_update_sspm_ddr_opp_table(0, ddr_opp_khz[0]);
}

/*
 * The classes helio-dvfsrc.c listens to, plus the SCP's. The MM clock
 * classes stay with nobody (mmdvfs_pmqos.c isn't ported).
 */
#define DVFSRC_QOS_CLASSES	(BIT(PM_QOS_MEMORY_BANDWIDTH) |		\
				 BIT(PM_QOS_CPU_MEMORY_BANDWIDTH) |	\
				 BIT(PM_QOS_GPU_MEMORY_BANDWIDTH) |	\
				 BIT(PM_QOS_MM_MEMORY_BANDWIDTH) |	\
				 BIT(PM_QOS_MD_PERI_MEMORY_BANDWIDTH) |	\
				 BIT(PM_QOS_EMI_OPP) |			\
				 BIT(PM_QOS_VCORE_OPP) |		\
				 BIT(PM_QOS_VCORE_DVFS_FIXED_OPP) |	\
				 BIT(PM_QOS_SCP_VCORE_REQUEST))

static DEFINE_MUTEX(qos_commit_lock);
static unsigned long qos_dirty;
static bool qos_forward;
static unsigned int qos_commits[PM_QOS_NUM_CLASSES];
static int qos_committed[PM_QOS_NUM_CLASSES];
static unsigned int qos_timeouts;
/* stock's dvfsrc->skip (helio-dvfsrc-sysfs.c), debugfs spm/dvfsrc_skip */
static bool dvfsrc_skip;
/* the Vcore read-back: the MT6358 buck, read only */
static struct regulator *vcore_reg;
static unsigned int vcore_fails;

static void qos_timeout(const char *what, int type, int data)
{
	qos_timeouts++;
	pr_warn_ratelimited("%s, class %d data 0x%x LEVEL 0x%x SW_RSV_5 0x%x\n",
			    what, type, data, readl(dvfsrc_base + DVFSRC_LEVEL),
			    readl(spm_base + SPM_SW_RSV_5));
}

/*
 * helio-dvfsrc.c commit_data for mt6771, run from qos_work under
 * qos_commit_lock. With DVS and DFS off the SPM OPP stays BOOT_UP_OPP, so
 * the OPP waits end at once, and the PCM status check and the Vcore
 * read-back wait for DVS and DFS on: the check's CMD_1 carries the on flag.
 */
static void commit_data(int type, int data)
{
	int level, opp, uv, ret = 0;

	if (force_opp_enable || dvfsrc_skip)
		return;
	if (vcorefs_can_work())
		spm_check_status_before_dvfs();

	if (type == PM_QOS_EMI_OPP || type == PM_QOS_VCORE_OPP ||
	    type == PM_QOS_VCORE_DVFS_FIXED_OPP) {
		if (dvfsrc_wait_idle())
			qos_timeout("wait no idle", type, data);
	}

	switch (type) {
	case PM_QOS_MEMORY_BANDWIDTH:
		if (data == PM_QOS_MEMORY_BANDWIDTH_DEFAULT_VALUE)
			break;
		writel(data / 100, dvfsrc_base + DVFSRC_SW_BW_0);
		break;
	case PM_QOS_CPU_MEMORY_BANDWIDTH:
		writel(data / 100, dvfsrc_base + DVFSRC_SW_BW_1);
		break;
	case PM_QOS_GPU_MEMORY_BANDWIDTH:
		writel(data / 100, dvfsrc_base + DVFSRC_SW_BW_2);
		break;
	case PM_QOS_MM_MEMORY_BANDWIDTH:
		writel(data / 75, dvfsrc_base + DVFSRC_SW_BW_3);	/* x 4/3 on mt6771 */
		break;
	case PM_QOS_MD_PERI_MEMORY_BANDWIDTH:
		writel(data / 100, dvfsrc_base + DVFSRC_SW_BW_4);
		break;
	case PM_QOS_EMI_OPP:
		if (data >= DDR_OPP_NUM || data < 0)
			data = DDR_OPP_NUM - 1;
		level = DDR_OPP_NUM - data - 1;
		dvfsrc_rmw(DVFSRC_SW_REQ, 0x3, level);
		udelay(1);
		dvfsrc_wait_idle();
		udelay(1);
		ret = spm_wait_dvfs_opp(get_min_opp(vcore_dvfs_to_ddr_opp, data), false);
		break;
	case PM_QOS_VCORE_OPP:
		if (data >= VCORE_OPP_NUM || data < 0)
			data = VCORE_OPP_NUM - 1;
		level = (VCORE_OPP_NUM - data - 1) << 24;
		dvfsrc_rmw(DVFSRC_VCORE_REQUEST2, 0x03000000, level);
		udelay(1);
		dvfsrc_wait_idle();
		udelay(1);
		opp = get_min_opp(vcore_dvfs_to_vcore_opp, data);
		ret = spm_wait_dvfs_opp(opp, false);
		if (vcore_reg && vcorefs_can_work()) {
			uv = regulator_get_voltage(vcore_reg);
			if (uv < (int)vcore_opp_uv[opp]) {
				vcore_fails++;
				pr_warn_ratelimited("DVFS FAIL %d %u LEVEL 0x%x opp %d\n",
						    uv, vcore_opp_uv[opp],
						    readl(dvfsrc_base + DVFSRC_LEVEL),
						    spm_vcorefs_get_dvfs_opp());
			}
		}
		break;
	case PM_QOS_VCORE_DVFS_FIXED_OPP:
		/* T takes anything at or above NUM_OPP as no fixed OPP; a negative one too here */
		if (data >= NUM_OPP || data < 0) {
			dvfsrc_rmw(DVFSRC_BASIC_CONTROL, BIT(15), 0);
			dvfsrc_rmw(DVFSRC_FORCE, 0xffff, 0);
		} else {
			writel(1 << (NUM_OPP - data - 1), dvfsrc_base + DVFSRC_FORCE);
			dvfsrc_rmw(DVFSRC_BASIC_CONTROL, 0, BIT(15));
			/* with DVS off the SPM OPP never reads back as this one */
			if (dvfs_en)
				ret = spm_wait_dvfs_opp(data, true);
		}
		break;
	default:
		return;
	}
	if (ret)
		qos_timeout("not done", type, data);
}

/*
 * dvfsrc_set_scp_vcore_request: VCORE_REQUEST [31:30] once vcorefs can
 * work; until then the class keeps the level, as stock kept scp_vcore_level.
 */
static void dvfsrc_set_scp_vcore_request(unsigned int level)
{
	int opp = get_min_opp(vcore_dvfs_to_vcore_opp, VCORE_OPP_NUM - 1 - (level & 0x3));

	if (force_opp_enable)
		return;
	if (!vcorefs_can_work())
		return;
	if (dvfsrc_wait_idle()) {
		qos_timeout("scp wait idle", PM_QOS_SCP_VCORE_REQUEST, level);
		return;
	}
	dvfsrc_rmw(DVFSRC_VCORE_REQUEST, 0x3 << 30, (level & 0x3) << 30);
	if (spm_wait_dvfs_opp(opp, false))
		qos_timeout("scp not done", PM_QOS_SCP_VCORE_REQUEST, level);
}

static void dvfsrc_qos_work_fn(struct work_struct *work)
{
	int type, data;

	mutex_lock(&qos_commit_lock);
	/* pinned or skipping: leave the dirty bits for the next run */
	if (force_opp_enable || dvfsrc_skip) {
		mutex_unlock(&qos_commit_lock);
		return;
	}
	for (type = 0; type < PM_QOS_NUM_CLASSES; type++) {
		if (!test_and_clear_bit(type, &qos_dirty))
			continue;
		data = pm_qos_request(type);
		if (type == PM_QOS_SCP_VCORE_REQUEST)
			dvfsrc_set_scp_vcore_request(data);
		else
			commit_data(type, data);
		qos_commits[type]++;
		qos_committed[type] = data;
	}
	mutex_unlock(&qos_commit_lock);
}
static DECLARE_WORK(qos_work, dvfsrc_qos_work_fn);

/* The pm_qos listener: any context, so it only marks the class and queues */
static void dvfsrc_qos_changed(int type)
{
	if (type < 0 || type >= PM_QOS_NUM_CLASSES ||
	    !(DVFSRC_QOS_CLASSES & BIT(type)))
		return;
	set_bit(type, &qos_dirty);
	queue_work(system_highpri_wq, &qos_work);
}

/* dvfsrc_md_scenario_update for LP4X 3733 */
static void dvfsrc_md_scenario_update(bool suspend)
{
	unsigned long flags;

	spin_lock_irqsave(&vcorefs_lock, flags);
	is_screen_off = suspend;
	writel(suspend ? EMI_MD2SPM0_SCREEN_OFF : EMI_MD2SPM0_SCREEN_ON,
	       dvfsrc_base + DVFSRC_EMI_MD2SPM0);
	spin_unlock_irqrestore(&vcorefs_lock, flags);
}

static int spm_vcorefs_fb_notifier_callback(struct notifier_block *self,
					    unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int blank;

	if (event != FB_EVENT_BLANK || !evdata || !evdata->data)
		return 0;
	blank = *(int *)evdata->data;
	switch (blank) {
	case FB_BLANK_UNBLANK:
		dvfsrc_md_scenario_update(false);
		pr_info("SCREEN ON (emi_md2spm0: 0x%x)\n",
			readl(dvfsrc_base + DVFSRC_EMI_MD2SPM0));
		break;
	case FB_BLANK_POWERDOWN:
		dvfsrc_md_scenario_update(true);
		pr_info("SCREEN OFF (emi_md2spm0: 0x%x)\n",
			readl(dvfsrc_base + DVFSRC_EMI_MD2SPM0));
		break;
	default:
		break;
	}
	return 0;
}

static struct notifier_block spm_vcorefs_fb_notif = {
	.notifier_call = spm_vcorefs_fb_notifier_callback,
};

/*
 * After vcorefs init: commit every DVFSRC class someone already asked for
 * (stock's dvfsrc_init_qos_opp did EMI and VCORE), then follow changes.
 */
static void dvfsrc_qos_commit_requested(void)
{
	int type;

	for (type = 0; type < PM_QOS_NUM_CLASSES; type++)
		if ((DVFSRC_QOS_CLASSES & BIT(type)) && pm_qos_class_requests(type))
			set_bit(type, &qos_dirty);
	queue_work(system_highpri_wq, &qos_work);
}

/* Commit every class from its current aggregate value, requested or not */
static void dvfsrc_qos_commit_all(void)
{
	int type;

	for (type = 0; type < PM_QOS_NUM_CLASSES; type++)
		if (DVFSRC_QOS_CLASSES & BIT(type))
			set_bit(type, &qos_dirty);
	queue_work(system_highpri_wq, &qos_work);
}

static void dvfsrc_qos_start(void)
{
	int ret;

	BUILD_BUG_ON(PM_QOS_NUM_CLASSES > BITS_PER_LONG);
	pm_qos_set_class_listener(dvfsrc_qos_changed);
	qos_forward = true;
	dvfsrc_qos_commit_requested();

	ret = fb_register_client(&spm_vcorefs_fb_notif);
	if (ret)
		pr_err("fb client not registered (%d)\n", ret);
}

/*
 * spm/force_opp (DVS and DFS on only): write 0 to 3 to pin that OPP with
 * DVFSRC_FORCE [15:0] one-hot and BASIC_CONTROL bit 15, as stock's fixed OPP,
 * and wait for the SPM to reach it; write -1 to let go. While pinned the
 * pm_qos commits, the SCP request and dvfsrc_init are skipped as stock's
 * is_force_opp_enable(); letting go commits every class again.
 */
static int force_opp = -1;

static int force_opp_get(void *data, u64 *val)
{
	*val = (u64)(s64)force_opp;
	return 0;
}

static int force_opp_set(void *data, u64 val)
{
	s64 req = (s64)val;
	int opp = (int)req;
	int ret = 0;

	mutex_lock(&qos_commit_lock);
	if (req >= 0 && req < NUM_OPP) {
		force_opp_enable = true;
		force_opp = opp;
		spm_check_status_before_dvfs();
		if (dvfsrc_wait_idle())
			qos_timeout("force wait idle", -1, opp);
		dvfsrc_rmw(DVFSRC_FORCE, 0xffff, 1 << (NUM_OPP - opp - 1));
		dvfsrc_rmw(DVFSRC_BASIC_CONTROL, 0, BIT(15));
		if (spm_wait_dvfs_opp(opp, true)) {
			qos_timeout("force not done", -1, opp);
			ret = -ETIMEDOUT;
		}
		pr_info("force_opp %d: LEVEL 0x%x SW_RSV_5 0x%x\n", opp,
			readl(dvfsrc_base + DVFSRC_LEVEL),
			readl(spm_base + SPM_SW_RSV_5));
	} else {
		dvfsrc_rmw(DVFSRC_BASIC_CONTROL, BIT(15), 0);
		dvfsrc_rmw(DVFSRC_FORCE, 0xffff, 0);
		force_opp_enable = false;
		force_opp = -1;
		pr_info("force_opp off\n");
		dvfsrc_qos_commit_all();
	}
	mutex_unlock(&qos_commit_lock);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE_SIGNED(force_opp_fops, force_opp_get, force_opp_set, "%lld\n");

static int dvfsrc_skip_get(void *data, u64 *val)
{
	*val = dvfsrc_skip;
	return 0;
}

/* Clearing the skip commits every class again */
static int dvfsrc_skip_set(void *data, u64 val)
{
	mutex_lock(&qos_commit_lock);
	dvfsrc_skip = !!val;
	mutex_unlock(&qos_commit_lock);
	if (!val)
		dvfsrc_qos_commit_all();
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(dvfsrc_skip_fops, dvfsrc_skip_get, dvfsrc_skip_set, "%llu\n");

static int qos_show(struct seq_file *m, void *unused)
{
	int type;

	seq_printf(m, "forward %d can_work %d timeouts %u screen_off %d\n",
		   qos_forward, vcorefs_can_work(), qos_timeouts, is_screen_off);
	for (type = 0; type < PM_QOS_NUM_CLASSES; type++) {
		if (!(DVFSRC_QOS_CLASSES & BIT(type)))
			continue;
		seq_printf(m, "%-24s value %d requests %u commits %u last %d\n",
			   pm_qos_class_name(type), pm_qos_request(type),
			   pm_qos_class_requests(type), qos_commits[type],
			   qos_committed[type]);
	}
	/* Only after init: the DVFSRC clock is untested before it */
	if (!init_done)
		return 0;
	seq_printf(m, "SW_REQ 0x%08x VCORE_REQUEST 0x%08x VCORE_REQUEST2 0x%08x\n",
		   readl(dvfsrc_base + DVFSRC_SW_REQ),
		   readl(dvfsrc_base + DVFSRC_VCORE_REQUEST),
		   readl(dvfsrc_base + DVFSRC_VCORE_REQUEST2));
	seq_printf(m, "SW_BW 0x%x 0x%x 0x%x 0x%x 0x%x\n",
		   readl(dvfsrc_base + DVFSRC_SW_BW_0),
		   readl(dvfsrc_base + DVFSRC_SW_BW_1),
		   readl(dvfsrc_base + DVFSRC_SW_BW_2),
		   readl(dvfsrc_base + DVFSRC_SW_BW_3),
		   readl(dvfsrc_base + DVFSRC_SW_BW_4));
	seq_printf(m, "FORCE 0x%08x BASIC_CONTROL 0x%08x LEVEL 0x%08x EMI_MD2SPM0 0x%x\n",
		   readl(dvfsrc_base + DVFSRC_FORCE),
		   readl(dvfsrc_base + DVFSRC_BASIC_CONTROL),
		   readl(dvfsrc_base + DVFSRC_LEVEL),
		   readl(dvfsrc_base + DVFSRC_EMI_MD2SPM0));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qos);

static int vcorefs_show(struct seq_file *m, void *unused)
{
	int i;

	seq_printf(m, "init %d pcm_flag 0x%x dvs %d dfs %d\n", init_done,
		   pcm_flag, vcore_dvs_en, ddr_dfs_en);
	seq_printf(m, "dvfs %d feature %d force_opp %d skip %d rekicks %u vcore_fails %u\n",
		   dvfs_en, feature_en, force_opp, dvfsrc_skip, pcm_rekicks,
		   vcore_fails);
	if (vcore_reg)
		seq_printf(m, "vcore_uv %d opp %d dvfs_opp %d\n",
			   regulator_get_voltage(vcore_reg), spm_vcorefs_get_opp(),
			   spm_vcorefs_get_dvfs_opp());
	seq_printf(m, "qos %d sent %u err %d\n", qos_on, qos_sent, qos_err);
	seq_printf(m, "seg 0x%x efuse 0x%x hw_rsv 0x%x lt_opp %d\n", seg_info,
		   soc_efuse_word, hw_rsv, lt_opp_feature_en);
	for (i = 0; i < NUM_OPP; i++)
		seq_printf(m, "opp %d vcore %u (pmic 0x%x) ddr %u\n", i,
			   vcore_opp_uv[i], vcore_uv_to_pmic(vcore_opp_uv[i]),
			   ddr_opp_khz[i]);
	seq_printf(m, "PCM_REG15 0x%08x SPM_SW_RSV_5 0x%08x\n",
		   readl(spm_base + PCM_REG15_DATA),
		   readl(spm_base + SPM_SW_RSV_5));
	seq_printf(m, "SPM_DFS_LEVEL 0x%08x SPM_DVS_LEVEL 0x%08x\n",
		   readl(spm_base + SPM_DFS_LEVEL), readl(spm_base + SPM_DVS_LEVEL));
	seq_printf(m, "DVFSRC_BASIC_CONTROL 0x%08x LEVEL 0x%08x\n",
		   readl(dvfsrc_base + DVFSRC_BASIC_CONTROL),
		   readl(dvfsrc_base + DVFSRC_LEVEL));
	seq_printf(m, "DVFSRC_SW_REQ 0x%08x VCORE_REQUEST 0x%08x VCORE_REQUEST2 0x%08x\n",
		   readl(dvfsrc_base + DVFSRC_SW_REQ),
		   readl(dvfsrc_base + DVFSRC_VCORE_REQUEST),
		   readl(dvfsrc_base + DVFSRC_VCORE_REQUEST2));
	if (toprgu_base)
		seq_printf(m, "RGU DEBUG_CTL2 0x%08x LATCH_CTL 0x%08x\n",
			   readl(toprgu_base + MTK_WDT_DEBUG_CTL2),
			   readl(toprgu_base + MTK_WDT_LATCH_CTL));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vcorefs);

/* The test-only files, made once DVS and DFS are on */
static void vcorefs_dvfs_debugfs(void)
{
	struct dentry *dir = debugfs_lookup("spm", NULL);

	debugfs_create_file_unsafe("force_opp", 0644, dir, NULL, &force_opp_fops);
	debugfs_create_file_unsafe("dvfsrc_skip", 0644, dir, NULL,
				   &dvfsrc_skip_fops);
	dput(dir);
}

static void vcorefs_get_vcore_reg(void)
{
	vcore_reg = regulator_get_optional(NULL, "vcore");
	if (IS_ERR(vcore_reg)) {
		pr_warn("no vcore regulator (%ld), no read-back\n",
			PTR_ERR(vcore_reg));
		vcore_reg = NULL;
	}
}

/*
 * The write's quick answer: 0 already on, -ENODEV without init on the off
 * branch or with the feature check failing, 1 to queue the turn-on.
 */
static int vcorefs_dvfs_check(void)
{
	int ret = 1;

	mutex_lock(&qos_commit_lock);
	if (feature_en)
		ret = 0;
	else if (!init_done || !is_vcorefs_feature_enable())
		ret = -ENODEV;
	mutex_unlock(&qos_commit_lock);
	return ret;
}

/*
 * spm_vcorefs.dvfs written 1 after a boot with init=1 on the off branch: the
 * boot's DVS and DFS on branch, in its order, from QOS_ENABLE with dvfs_en 1
 * and the OPP rows (the table built at boot, lt_opp not applied again) to
 * dvfsrc_init_qos_opp, then every class committed again. qos_commit_lock is
 * held throughout, so no pm_qos commit or SCP request runs in between. Runs
 * from vcorefs_dvfs_on_work, outside param_lock.
 * One way: stock's way back (vcorefs_v3 feature_en 0) only kicks OPP 0 and
 * leaves the PCM flag; spm/force_opp 0 does that here.
 */
static int vcorefs_dvfs_turn_on(void)
{
	mutex_lock(&qos_commit_lock);
	if (feature_en) {
		mutex_unlock(&qos_commit_lock);
		return 0;
	}
	if (!init_done || !is_vcorefs_feature_enable()) {
		mutex_unlock(&qos_commit_lock);
		return -ENODEV;
	}
	pr_info("DVS and DFS on at run time\n");
	feature_en = true;
	dvfs_en = true;
	vcorefs_get_vcore_reg();
	helio_dvfsrc_sspm_ipi_init(feature_en, __spmfw_idx);
	vcorefs_init_opp_table();
	spm_dvfsfw_init(spm_vcorefs_get_opp());
	spm_go_to_vcorefs(spm_dvfs_flag_init());
	/* EMI_MD2SPM0 from is_screen_off, under the lock the fb notifier takes */
	dvfsrc_init();
	vcorefs_late_init_dvfs();
	dvfsrc_init_qos_opp();
	pr_info("VCORE DVFS IS ENABLE (pcm_flag 0x%x, firmware status %d)\n",
		pcm_flag, spm_load_firmware_status());
	dvfsrc_qos_commit_all();
	mutex_unlock(&qos_commit_lock);
	vcorefs_dvfs_debugfs();
	return 0;
}

static int __init spm_vcorefs_init(void)
{
	struct device_node *node;
	struct dentry *dir;
	bool want_dvfs = dvfs_en;

	WRITE_ONCE(vcorefs_booted, true);
	/* reads Y only once DVS and DFS are on */
	dvfs_en = false;

	/* spm/qos shows the kept pm_qos values with or without init */
	dir = debugfs_lookup("spm", NULL);
	if (dir) {
		debugfs_create_file("qos", 0444, dir, NULL, &qos_fops);
		dput(dir);
	}

	if (!init_en)
		return 0;
	if (!spm_base || __spmfw_idx != SPMFW_LP4X_2CH_3733) {
		pr_err("no SPM or spmfw_idx %d, skipped\n", __spmfw_idx);
		return 0;
	}

	if (vcorefs_devinfo(30, &seg_info) || vcorefs_devinfo(65, &soc_efuse_word) ||
	    vcorefs_devinfo(120, &hw_rsv)) {
		pr_err("no atag,devinfo, skipped\n");
		return 0;
	}

	/* dvfsrc_register_init: DVFSRC and the SSPM QOS SRAM */
	node = of_find_compatible_node(NULL, NULL, "mediatek,dvfsrc_top");
	if (node) {
		dvfsrc_base = of_iomap(node, 0);
		qos_sram_base = of_iomap(node, 1);
		of_node_put(node);
	}
	if (!dvfsrc_base || !qos_sram_base) {
		pr_err("no dvfsrc_top, skipped\n");
		return 0;
	}
	/* The watchdog driver owns TOPRGU; this only sets the two DVFSRC bits */
	node = of_find_compatible_node(NULL, NULL, "mediatek,mt6771-wdt");
	if (node) {
		toprgu_base = of_iomap(node, 0);
		of_node_put(node);
	}
	if (!toprgu_base)
		pr_err("no toprgu, DVFSRC reset latch not set\n");

	seg_info_init();
	/* QOS_ENABLE's dvfs_en is the same test that picks the branch below */
	feature_en = want_dvfs && is_vcorefs_feature_enable();
	dvfs_en = feature_en;
	helio_dvfsrc_sspm_ipi_init(feature_en, __spmfw_idx);
	/* plat_info_init */
	if ((hw_rsv >> 1) & 0x1)
		lt_opp_feature_en = false;
	pr_info("hw_rsv 0x%x lt_opp_feature_en %d\n", hw_rsv, lt_opp_feature_en);
	vcore_opp_init();
	vcorefs_temp_opp_init();
	vcorefs_init_opp_table();

	if (feature_en) {
		/* The DVS and DFS on branch; the SCP request follows in dvfsrc_qos_start */
		vcorefs_get_vcore_reg();
		spm_dvfsfw_init(spm_vcorefs_get_opp());
		spm_go_to_vcorefs(spm_dvfs_flag_init());
		dvfsrc_init();
		vcorefs_late_init_dvfs();
		dvfsrc_init_qos_opp();
		pr_info("VCORE DVFS IS ENABLE (pcm_flag 0x%x, firmware status %d)\n",
			pcm_flag, spm_load_firmware_status());
	} else {
		spm_dvfsfw_init(BOOT_UP_OPP);
		spm_go_to_vcorefs(SPM_VCOREFS_FLAG_OFF);
		dvfsrc_init();
		init_done = true;
		pr_info("VCORE DVFS IS DISABLE (firmware status %d)\n",
			spm_load_firmware_status());
	}
	dvfsrc_qos_start();

	dir = debugfs_lookup("spm", NULL);
	debugfs_create_file("vcorefs", 0444, dir, NULL, &vcorefs_fops);
	dput(dir);
	if (feature_en)
		vcorefs_dvfs_debugfs();
	return 0;
}
late_initcall(spm_vcorefs_init);
