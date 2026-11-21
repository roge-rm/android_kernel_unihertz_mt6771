// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 SPM deep suspend, T's spm_v4 mtk_sleep.c
 * slp_suspend_ops and mtk_spm_sleep.c spm_go_to_sleep, for mem only:
 * - registered only with spm_suspend.deep=1 (the default), a non-zero
 *   firmware status and vcorefs init done (spm_vcorefs.init=1, the default);
 *   mem_sleep is then deep (P12d) unless mem_sleep_default=s2idle is given,
 *   and "echo s2idle > /sys/power/mem_sleep" switches at run time
 * - enter: PCM WDT on (SPM_ARGS PCM_WDT, 1, 30 s), SIP SUSPEND_ARGS with the
 *   stock Image's flags, PSCI CPU_SUSPEND 0x1010005 on CPU0 (ATF's
 *   go_to_sleep_after_wfi), then the wake status from the SPM registers,
 *   logged as stock's "wake up by" line, and PCM WDT off
 * - around enter (P12b): stock's SSPM IPI lock check, the MCDI PAUSE_ACTION
 *   handshake, the TOPRGU handover and SSPM SPM_SUSPEND and SPM_RESUME
 * - begin and end: SSPM SPM_SUSPEND_PREPARE and SPM_POST_SUSPEND, stock's
 *   PM notifier commands, only for mem; the core calls begin and end for
 *   s2idle too, so both check the state
 * - around the PSCI call (P12c): every SPI masked but SPM IRQ0 and stock's
 *   edge list, those cloned into SYS_CIRQ (mtk_sys_cirq.c) and replayed into
 *   the GIC on resume, and the 8250_mtk sleep request and wake handshake
 * - the wake reason in dmesg and, for the R12 sources that map to one IRQ,
 *   /sys/power/pm_wakeup_irq
 * - debugfs spm/suspend: flags, wake mask, counts and the last wake status
 */
#define pr_fmt(fmt) "spm_suspend: " fmt

#include <clocksource/arm_arch_timer.h>
#include <linux/arm-smccc.h>
#include <linux/build_bug.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/psci.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mtk-8250.h>
#include <linux/soc/mediatek/mtk-mcdi.h>
#include <linux/soc/mediatek/mtk-spm.h>
#include <linux/soc/mediatek/mtk-sspm.h>
#include <linux/soc/mediatek/mtk-wdt.h>
#include <linux/suspend.h>

#include "mtk_sys_cirq.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "spm_suspend."

/* On by default */
static bool deep_en = true;
module_param_named(deep, deep_en, bool, 0444);
MODULE_PARM_DESC(deep, "Offer SPM deep suspend as mem_sleep deep (needs spm_vcorefs.init=1)");

/* Stock's SPM_WAKE_PERIOD, used when the gauge gives no period */
static unsigned int wake_sec = 600;
module_param(wake_sec, uint, 0644);
MODULE_PARM_DESC(wake_sec, "PCM timer wake in seconds, 0 for none");

/* T's mt-plat/mtk_secure_api.h (arm64) */
#define MTK_SIP_KERNEL_SPM_GET_PWR_CTRL_ARGS	0xc200022b
/* T's spm_v4 mtk_spm_internal.h: SPM_PWR_CTRL_SUSPEND and mt6771's PW_WAKE_SRC */
#define SPM_PWR_CTRL_SUSPEND	17
#define PW_WAKE_SRC		12

/* T's spm_v4 sleep_def_mt6771.h */
#define SPM_FLAG_DIS_CPU_PDN		(1U << 0)
#define SPM_FLAG_DIS_INFRA_PDN		(1U << 1)
#define SPM_FLAG_DIS_VCORE_DVS		(1U << 3)
#define SPM_FLAG_DIS_VCORE_DFS		(1U << 4)
#define SPM_FLAG_DIS_ATF_ABORT		(1U << 7)
#define SPM_FLAG_DISABLE_MMSYS_DVFS	(1U << 15)
#define SPM_FLAG_SUSPEND_OPTION		(1U << 19)
#define SPM_FLAG1_BIG_BUCK_OFF_ENABLE	(1U << 8)
#define SPM_FLAG1_BIG_BUCK_ON_ENABLE	(1U << 9)

/*
 * slp_spm_flags as the stock Image has it (0x8009a): T's mt6771 list plus
 * DIS_INFRA_PDN, so infra and the UART stay powered.
 */
#define SLP_SPM_FLAGS	(SPM_FLAG_DIS_INFRA_PDN | SPM_FLAG_DIS_VCORE_DVS |	\
			 SPM_FLAG_DIS_VCORE_DFS | SPM_FLAG_DIS_ATF_ABORT |	\
			 SPM_FLAG_SUSPEND_OPTION)
/*
 * flags1: stock passes 0 and __sync_big_buck_ctrl_pcm_flag adds both big
 * buck bits, as its mcdi_is_buck_off and cpuhp_is_buck_off are the weak
 * ones returning false.
 */
#define SLP_SPM_FLAGS1	(SPM_FLAG1_BIG_BUCK_OFF_ENABLE | SPM_FLAG1_BIG_BUCK_ON_ENABLE)
/* ATF's suspend set-up takes these bits from the vcorefs PCM flag */
#define ATF_VCOREFS_SYNC_MASK	(SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS | \
				 SPM_FLAG_DISABLE_MMSYS_DVFS)
/* ATF's CMD_1 keeps DVS and DFS off bits over this base */
#define ATF_VCOREFS_FLAG_BASE	0x8400

/* The stock DT's idle-states/suspend, MTK_SUSPEND_MODE's PSCI power state */
#define MTK_SUSPEND_PSCI_STATE	0x1010005

/* PCM timer and watchdog ticks are 32 kHz; T's PCM_TIMER_MAX leaves room for the 30 s WDT */
#define PCM_TICKS_PER_SEC	32768
#define PCM_WDT_TIMEOUT_SEC	30
#define PCM_TIMER_MAX		(0xffffffff - PCM_WDT_TIMEOUT_SEC * PCM_TICKS_PER_SEC)
/* _spm_get_wake_period's cap */
#define SPM_WAKE_PERIOD_MAX	(36 * 3600)

/* T's spm_v4 mtk_spm_reg_mt6771.h */
#define PCM_REG_DATA_INI	0x028
#define SPM_SRC_REQ		0x0b8
#define PCM_REG13_DATA		0x134
#define PCM_REG12_EXT_DATA	0x144
#define SPM_IRQ_STA		0x158
#define SPM_WAKEUP_STA		0x15c
#define SPM_WAKEUP_EXT_STA	0x160
#define SUBSYS_IDLE_STA		0x170
#define SRC_REQ_STA		0x17c
#define SPM_BSI_D0_SR		0x41c
#define SPM_BSI_D1_SR		0x420
#define SPM_BSI_D2_SR		0x424
#define SPM_SW_FLAG		0x600
#define SPM_SW_DEBUG		0x604
#define SPM_SW_RSV_0		0x608
#define SPM_SW_RSV_2		0x610
#define WDT_LATCH_SPARE0_FIX	0x780

/* T's spm_v4 pcm_def_mt6771.h and mtk_spm_internal.h */
#define R12_PCM_TIMER		(1U << 0)
#define WAKE_MISC_TWAM		(1U << 18)
#define WAKE_MISC_PCM_TIMER	(1U << 19)
#define WAKE_MISC_CPU_WAKE	(1U << 20)

/* T's spm_v4 mtk_spm.h */
enum {
	WR_NONE = 0,
	WR_UART_BUSY = 1,
	WR_PCM_ASSERT = 2,
	WR_PCM_TIMER = 3,
	WR_WAKE_SRC = 4,
	WR_UNKNOWN = 5,
};

/* T's spm_v4 mtk_spm_internal.h SSPM commands on IPI_ID_SPM_SUSPEND */
enum {
	SPM_SUSPEND = 0,
	SPM_RESUME = 1,
	SPM_SUSPEND_PREPARE = 8,
	SPM_POST_SUSPEND = 9,
};

/* T's struct spm_data: the command and five words for suspend, SPM_D_LEN 8 words in all */
#define SPM_D_LEN	8
struct spm_data {
	u32 cmd;
	union {
		struct {
			u32 sys_timestamp_l;
			u32 sys_timestamp_h;
			u32 sys_src_clk_l;
			u32 sys_src_clk_h;
			u32 spm_opt;
		} suspend;
		u32 args[7];
	} u;
};
static_assert(sizeof(struct spm_data) == SPM_D_LEN * sizeof(u32));

/* PCM_REG12 wake bits, T's mt6771 wakesrc_str (pcm_def_mt6771.h R12_* names) */
static const char *const wakesrc_str[32] = {
	[0] = " R12_PCM_TIMER",
	[1] = " R12_SSPM_WDT_EVENT_B",
	[2] = " R12_KP_IRQ_B",
	[3] = " R12_APWDT_EVENT_B",
	[4] = " R12_APXGPT1_EVENT_B",
	[5] = " R12_CONN2AP_SPM_WAKEUP_B",
	[6] = " R12_EINT_EVENT_B",
	[7] = " R12_CONN_WDT_IRQ_B",
	[8] = " R12_CCIF0_EVENT_B",
	[9] = " R12_LOWBATTERY_IRQ_B",
	[10] = " R12_SSPM_SPM_IRQ_B",
	[11] = " R12_SCP_SPM_IRQ_B",
	[12] = " R12_SCP_WDT_EVENT_B",
	[13] = " R12_PCM_WDT_WAKEUP_B",
	[14] = " R12_USB_CDSC_B",
	[15] = " R12_USB_POWERDWN_B",
	[16] = " R12_SYS_TIMER_EVENT_B",
	[17] = " R12_EINT_EVENT_SECURE_B",
	[18] = " R12_CCIF1_EVENT_B",
	[19] = " R12_UART0_IRQ_B",
	[20] = " R12_AFE_IRQ_MCU_B",
	[21] = " R12_THERM_CTRL_EVENT_B",
	[22] = " R12_SYS_CIRQ_IRQ_B",
	[23] = " R12_MD2AP_PEER_EVENT_B",
	[24] = " R12_CSYSPWREQ_B",
	[25] = " R12_MD1_WDT_B",
	[26] = " R12_CLDMA_EVENT_B",
	[27] = " R12_SEJ_WDT_GPT_B",
	[28] = " R12_ALL_SSPM_WAKEUP_B",
	[29] = " R12_CPU_IRQ_B",
	[30] = " R12_CPU_WFI_AND_B",
	[31] = " R12_MCUSYS_IDLE_TO_EMI_ALL_B",
};

/*
 * R12 sources with one IRQ behind them, for pm_wakeup_irq: the keypad and the
 * modem's four (md_sys1_platform.c's mdcldma order: CLDMA, CCIF0, CCIF1,
 * MD WDT). EINT is left to the IRQ core, which names the EINT child itself
 * when a wake IRQ fires after resume.
 */
static const struct {
	u32 r12;
	const char *compatible;
	int index;
} wake_irq_src[] = {
	{ BIT(2), "mediatek,kp", 0 },		/* R12_KP_IRQ_B */
	{ BIT(8), "mediatek,mdcldma", 1 },	/* R12_CCIF0_EVENT_B */
	{ BIT(18), "mediatek,mdcldma", 2 },	/* R12_CCIF1_EVENT_B */
	{ BIT(25), "mediatek,mdcldma", 3 },	/* R12_MD1_WDT_B */
	{ BIT(26), "mediatek,mdcldma", 0 },	/* R12_CLDMA_EVENT_B */
};
static struct mtk_dt_irq wake_irq[ARRAY_SIZE(wake_irq_src)];
static bool wake_irq_ok[ARRAY_SIZE(wake_irq_src)];

/* T's struct wake_status, read as __spm_get_wakeup_status */
struct wake_status {
	u32 assert_pc;
	u32 r12;
	u32 r12_ext;
	u32 raw_sta;
	u32 raw_ext_sta;
	u32 wake_misc;
	u32 timer_out;
	u32 r13;
	u32 idle_sta;
	u32 req_sta;
	u32 debug_flag;
	u32 debug_flag1;
	u32 event_reg;
	u32 isr;
	u32 sw_flag;
	u32 sw_rsv_2;
	u32 src_req;
};

/* Only touched in enter, with the other CPUs off and IRQs off, and by debugfs */
static bool registered;
static struct wake_status last_wakesta;
static unsigned int last_wr = WR_NONE;
static u32 last_wake_src, last_timer;
static int last_psci_ret;
static unsigned int sleep_count, assert_count;
/* SSPM, MCDI and TOPRGU results; begin and end run in process context, the rest in enter */
static bool sspm_prepared;
static int last_prepare_ret, last_post_ret, last_suspend_ret, last_resume_ret;
static int last_mcdi_ret, last_wdt_ret;
static unsigned int ipi_lock_refusals, mcdi_refusals, prepare_refusals;
/* P12c results, set in enter */
static int last_uart_ret, last_uart_wake_ret;
static unsigned int uart_busy_count, last_cirq_replayed, last_wakeup_irq;

static unsigned long spm_sip(u32 id, unsigned long a1, unsigned long a2,
			     unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(id, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

/* The suspend flags after ATF takes the vcorefs bits; what CMD_1 left in ATF */
static u32 spm_suspend_effective_flags(void)
{
	u32 vflag = 0, atf;

	spm_vcorefs_init_done(&vflag);
	atf = ATF_VCOREFS_FLAG_BASE |
	      (vflag & (SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS));
	return (SLP_SPM_FLAGS & ~ATF_VCOREFS_SYNC_MASK) | (atf & ATF_VCOREFS_SYNC_MASK);
}

/* _spm_get_wake_period with no gauge, then __spm_get_pcm_timer_val */
static u32 spm_suspend_timer_val(void)
{
	unsigned int sec = min_t(unsigned int, READ_ONCE(wake_sec), SPM_WAKE_PERIOD_MAX);

	return sec ? sec * PCM_TICKS_PER_SEC : PCM_TIMER_MAX;
}

/*
 * T's spm_to_sspm_command: polling, the whole spm_data, one ack word; a
 * negative ack is the result. Polling works with IRQs off and gives up after
 * SSPM's 10 ms.
 */
static int spm_to_sspm_command(u32 cmd, struct spm_data *d)
{
	int ack = 0, ret;

	d->cmd = cmd;
	ret = sspm_ipi_send_sync(IPI_ID_SPM_SUSPEND, IPI_OPT_POLLING, d, SPM_D_LEN,
				 &ack, 1);
	if (ret) {
		pr_err("SSPM cmd %u: IPI %d\n", cmd, ret);
		return ret;
	}
	if (ack < 0) {
		pr_err("SSPM cmd %u: ack %d\n", cmd, ack);
		return ack;
	}
	return 0;
}

/* sspm_timesync_ts_get and clk_get: sched_clock ns and the arch counter, low word first */
static void spm_suspend_timestamps(struct spm_data *d)
{
	u64 ts = sched_clock(), clk = arch_timer_read_counter();

	d->u.suspend.sys_timestamp_l = lower_32_bits(ts);
	d->u.suspend.sys_timestamp_h = upper_32_bits(ts);
	d->u.suspend.sys_src_clk_l = lower_32_bits(clk);
	d->u.suspend.sys_src_clk_h = upper_32_bits(clk);
}

/* T's mt6771 spm_suspend_pre_process: SPM_SUSPEND, spm_opt 0 (no GPS flag); a failure only logs */
static int spm_suspend_pre_process(void)
{
	struct spm_data d = {};

	spm_suspend_timestamps(&d);
	d.u.suspend.spm_opt = 0;
	return spm_to_sspm_command(SPM_SUSPEND, &d);
}

/* T's mt6771 spm_suspend_post_process: SPM_RESUME with the timestamps */
static int spm_suspend_post_process(void)
{
	struct spm_data d = {};

	spm_suspend_timestamps(&d);
	return spm_to_sspm_command(SPM_RESUME, &d);
}

static void spm_get_wakeup_status(struct wake_status *ws)
{
	ws->assert_pc = readl(spm_base + PCM_REG_DATA_INI);
	ws->r12 = readl(spm_base + SPM_SW_RSV_0);
	ws->r12_ext = readl(spm_base + PCM_REG12_EXT_DATA);
	ws->raw_sta = readl(spm_base + SPM_WAKEUP_STA);
	ws->raw_ext_sta = readl(spm_base + SPM_WAKEUP_EXT_STA);
	ws->wake_misc = readl(spm_base + SPM_BSI_D0_SR);
	ws->timer_out = readl(spm_base + SPM_BSI_D1_SR);
	ws->r13 = readl(spm_base + PCM_REG13_DATA);
	ws->idle_sta = readl(spm_base + SUBSYS_IDLE_STA);
	ws->req_sta = readl(spm_base + SRC_REQ_STA);
	ws->debug_flag = readl(spm_base + SPM_SW_DEBUG);
	ws->debug_flag1 = readl(spm_base + WDT_LATCH_SPARE0_FIX);
	ws->event_reg = readl(spm_base + SPM_BSI_D2_SR);
	ws->isr = readl(spm_base + SPM_IRQ_STA);
	ws->sw_flag = readl(spm_base + SPM_SW_FLAG);
	ws->sw_rsv_2 = readl(spm_base + SPM_SW_RSV_2);
	ws->src_req = readl(spm_base + SPM_SRC_REQ);
}

/* The wake reason names, __spm_output_wake_reason's order */
static unsigned int spm_wake_reason(const struct wake_status *ws, char *buf,
				    size_t size)
{
	unsigned int wr = WR_UNKNOWN;
	size_t len = 0;
	int i;

	buf[0] = '\0';
	if (ws->assert_pc)
		return WR_PCM_ASSERT;
	if (ws->r12 & R12_PCM_TIMER) {
		if (ws->wake_misc & WAKE_MISC_PCM_TIMER) {
			len += scnprintf(buf + len, size - len, " PCM_TIMER");
			wr = WR_PCM_TIMER;
		}
		if (ws->wake_misc & WAKE_MISC_TWAM) {
			len += scnprintf(buf + len, size - len, " TWAM");
			wr = WR_WAKE_SRC;
		}
		if (ws->wake_misc & WAKE_MISC_CPU_WAKE) {
			len += scnprintf(buf + len, size - len, " CPU");
			wr = WR_WAKE_SRC;
		}
	}
	for (i = 1; i < 32; i++) {
		if (ws->r12 & BIT(i)) {
			len += scnprintf(buf + len, size - len, "%s", wakesrc_str[i]);
			wr = WR_WAKE_SRC;
		}
	}
	return wr;
}

static void spm_output_wake_reason(const struct wake_status *ws, unsigned int wr,
				   const char *buf)
{
	if (wr == WR_PCM_ASSERT) {
		pr_err("PCM ASSERT AT 0x%x (suspend), r13 = 0x%x, debug_flag = 0x%x 0x%x\n",
		       ws->assert_pc, ws->r13, ws->debug_flag, ws->debug_flag1);
		return;
	}
	pr_info("wake up by %s, timer_out = %u, r13 = 0x%x, debug_flag = 0x%x 0x%x, r12 = 0x%x, r12_ext = 0x%x, raw_sta = 0x%x, idle_sta = 0x%x, req_sta = 0x%x, event_reg = 0x%x, isr = 0x%x, raw_ext_sta = 0x%x, wake_misc = 0x%x, pcm_flag = 0x%x 0x%x, req = 0x%x\n",
		buf, ws->timer_out, ws->r13, ws->debug_flag, ws->debug_flag1,
		ws->r12, ws->r12_ext, ws->raw_sta, ws->idle_sta, ws->req_sta,
		ws->event_reg, ws->isr, ws->raw_ext_sta, ws->wake_misc,
		ws->sw_flag, ws->sw_rsv_2, ws->src_req);
}

/*
 * The first R12 source in wake_irq_src with a mapped IRQ goes to
 * pm_system_irq_wakeup, unless the IRQ core has named one already.
 */
static unsigned int spm_report_wakeup_irq(const struct wake_status *ws)
{
	unsigned int i, virq;

	for (i = 0; i < ARRAY_SIZE(wake_irq_src); i++) {
		if (!(ws->r12 & wake_irq_src[i].r12) || !wake_irq_ok[i])
			continue;
		virq = mtk_dt_irq_virq(&wake_irq[i]);
		if (!virq)
			continue;
		if (!pm_wakeup_irq())
			pm_system_irq_wakeup(virq);
		return virq;
	}
	return 0;
}

static int spm_suspend_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}

/*
 * Stock's spm_pm_event PM_SUSPEND_PREPARE: SSPM SPM_SUSPEND_PREPARE, a
 * failure aborts suspend. Called for s2idle as well, which sends nothing.
 */
static int spm_suspend_begin(suspend_state_t state)
{
	struct spm_data d = {};
	int ret;

	sspm_prepared = false;
	if (state != PM_SUSPEND_MEM)
		return 0;
	ret = spm_to_sspm_command(SPM_SUSPEND_PREPARE, &d);
	last_prepare_ret = ret;
	if (ret) {
		prepare_refusals++;
		pr_err("SSPM SPM_SUSPEND_PREPARE: %d, suspend aborted\n", ret);
		return -EBUSY;
	}
	sspm_prepared = true;
	return 0;
}

/* PM_POST_SUSPEND: SSPM SPM_POST_SUSPEND, only after a prepare that went through */
static void spm_suspend_end(void)
{
	struct spm_data d = {};

	if (!sspm_prepared)
		return;
	sspm_prepared = false;
	last_post_ret = spm_to_sspm_command(SPM_POST_SUSPEND, &d);
}

/*
 * Called by the suspend core on the boot CPU with the others offline and
 * IRQs off, after syscore_suspend (which runs the CPU PM notifiers).
 */
static int spm_suspend_enter(suspend_state_t state)
{
	struct wake_status ws;
	char buf[256];
	u32 timer;
	int ret;

	if (state != PM_SUSPEND_MEM)
		return -EINVAL;
	if (!spm_load_firmware_status()) {
		pr_err("SPM FIRMWARE IS NOT READY\n");
		return -EPERM;
	}
	if (WARN_ON_ONCE(smp_processor_id() != 0))
		return -EPERM;
	if (is_sspm_ipi_lock_spm()) {
		ipi_lock_refusals++;
		pr_err("SSPM IPI in flight, suspend refused\n");
		return -EBUSY;
	}
	/* Stock spins on PAUSE_ACK; with no ack nothing is armed yet, so give up */
	ret = mtk_mcdi_task_pause(true);
	last_mcdi_ret = ret;
	if (ret) {
		mtk_mcdi_task_pause(false);
		mcdi_refusals++;
		return -EBUSY;
	}

	last_wake_src = spm_sip(MTK_SIP_KERNEL_SPM_GET_PWR_CTRL_ARGS,
				SPM_PWR_CTRL_SUSPEND, PW_WAKE_SRC, 0);
	timer = spm_suspend_timer_val();
	last_timer = timer;
	spm_sip(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_PCM_WDT, 1, PCM_WDT_TIMEOUT_SEC);
	last_wdt_ret = mtk_wdt_spm_suspend();
	if (last_wdt_ret)
		pr_err("no TOPRGU: %d\n", last_wdt_ret);
	/* Only SPM IRQ0 and the edge list stay on in the GIC; CIRQ watches the edge list */
	mtk_gic_mask_all_for_sleep();
	mtk_gic_unmask_for_sleep(spm_irq_0);
	mtk_gic_unmask_edge_trig_irqs();
	mtk_cirq_clone_gic();
	mtk_cirq_enable();

	pr_info("sec = %u, wakesrc = 0x%x (%u)(%u), flags 0x%x 0x%x\n",
		timer / PCM_TICKS_PER_SEC, last_wake_src,
		!(SLP_SPM_FLAGS & SPM_FLAG_DIS_CPU_PDN),
		!(SLP_SPM_FLAGS & SPM_FLAG_DIS_INFRA_PDN),
		SLP_SPM_FLAGS, SLP_SPM_FLAGS1);
	last_suspend_ret = spm_suspend_pre_process();
	spm_sip(MTK_SIP_KERNEL_SPM_SUSPEND_ARGS, SLP_SPM_FLAGS, SLP_SPM_FLAGS1, timer);

	/* A UART that won't ack skips the PSCI call, as stock's RESTORE_IRQ jump */
	ret = 0;
	last_uart_ret = mtk8250_request_to_sleep();
	if (!last_uart_ret) {
		ret = psci_cpu_suspend_enter(MTK_SUSPEND_PSCI_STATE);
		last_uart_wake_ret = mtk8250_request_to_wakeup();
	} else {
		uart_busy_count++;
	}
	last_psci_ret = ret;

	spm_get_wakeup_status(&ws);
	last_resume_ret = spm_suspend_post_process();
	last_wr = last_uart_ret ? WR_UART_BUSY : spm_wake_reason(&ws, buf, sizeof(buf));
	last_wakesta = ws;
	sleep_count++;
	if (last_wr == WR_PCM_ASSERT)
		assert_count++;
	if (ret)
		pr_err("PSCI CPU_SUSPEND 0x%x: %d\n", MTK_SUSPEND_PSCI_STATE, ret);
	if (last_uart_ret)
		pr_err("UART busy (%d), sleep skipped\n", last_uart_ret);
	else
		spm_output_wake_reason(&ws, last_wr, buf);
	last_wakeup_irq = last_wr == WR_WAKE_SRC ? spm_report_wakeup_irq(&ws) : 0;
	/* Edges CIRQ latched while the GIC was off go pending before the mask comes back */
	last_cirq_replayed = mtk_cirq_flush();
	mtk_cirq_disable();
	mtk_gic_mask_restore();
	if (!last_wdt_ret)
		mtk_wdt_spm_resume();
	spm_sip(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_PCM_WDT, 0, 0);
	/* Stock's rekick_vcorefs_scenario after a PCM assert does nothing on mt6771 */
	/* slp_suspend_ops_enter's mcdi_task_pause(false); a timeout there only logs */
	mtk_mcdi_task_pause(false);
	return 0;
}

static const struct platform_suspend_ops spm_suspend_ops = {
	.valid = spm_suspend_valid,
	.begin = spm_suspend_begin,
	.enter = spm_suspend_enter,
	.end = spm_suspend_end,
};

static int spm_suspend_show(struct seq_file *m, void *unused)
{
	const struct wake_status *ws = &last_wakesta;
	u32 vflag = 0;
	bool vdone = spm_vcorefs_init_done(&vflag);
	char buf[256];

	seq_printf(m, "deep %d registered %d firmware_status %d\n", deep_en,
		   registered, spm_load_firmware_status());
	seq_printf(m, "vcorefs init %d pcm_flag 0x%x\n", vdone, vflag);
	seq_printf(m, "flags 0x%x flags1 0x%x, after ATF's vcorefs sync 0x%x\n",
		   SLP_SPM_FLAGS, SLP_SPM_FLAGS1, spm_suspend_effective_flags());
	seq_printf(m, "psci_state 0x%x wake_sec %u timer %u\n",
		   MTK_SUSPEND_PSCI_STATE, READ_ONCE(wake_sec), spm_suspend_timer_val());
	seq_printf(m, "sleep_count %u pcm_asserts %u\n", sleep_count, assert_count);
	seq_printf(m, "refused: ipi_lock %u mcdi %u sspm_prepare %u\n", ipi_lock_refusals,
		   mcdi_refusals, prepare_refusals);
	seq_printf(m, "last sspm prepare %d suspend %d resume %d post %d, mcdi %d, toprgu %d\n",
		   last_prepare_ret, last_suspend_ret, last_resume_ret, last_post_ret,
		   last_mcdi_ret, last_wdt_ret);
	seq_printf(m, "cirq %d, uart busy %u, last uart sleep %d wake %d, cirq replayed %u, wakeup_irq %u\n",
		   mtk_cirq_ready(), uart_busy_count, last_uart_ret, last_uart_wake_ret,
		   last_cirq_replayed, last_wakeup_irq);
	if (!sleep_count)
		return 0;
	spm_wake_reason(ws, buf, sizeof(buf));
	seq_printf(m, "last wakesrc 0x%x timer %u psci %d wr %u by%s\n", last_wake_src,
		   last_timer, last_psci_ret, last_wr, buf);
	seq_printf(m, "last assert_pc 0x%x r12 0x%x r12_ext 0x%x r13 0x%x wake_misc 0x%x timer_out %u\n",
		   ws->assert_pc, ws->r12, ws->r12_ext, ws->r13, ws->wake_misc,
		   ws->timer_out);
	seq_printf(m, "last raw_sta 0x%x raw_ext_sta 0x%x idle_sta 0x%x req_sta 0x%x event_reg 0x%x isr 0x%x\n",
		   ws->raw_sta, ws->raw_ext_sta, ws->idle_sta, ws->req_sta,
		   ws->event_reg, ws->isr);
	seq_printf(m, "last debug_flag 0x%x 0x%x pcm_flag 0x%x 0x%x req 0x%x\n",
		   ws->debug_flag, ws->debug_flag1, ws->sw_flag, ws->sw_rsv_2,
		   ws->src_req);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(spm_suspend);

/* After the vcorefs late_initcall */
static int __init spm_suspend_init(void)
{
	struct dentry *dir;
	unsigned int i;
	u32 vflag = 0;
	int ret;

	if (!spm_base)
		return 0;
	dir = debugfs_lookup("spm", NULL);
	debugfs_create_file("suspend", 0444, dir, NULL, &spm_suspend_fops);
	dput(dir);

	if (!deep_en)
		return 0;
	if (!spm_load_firmware_status()) {
		pr_err("firmware status 0, deep suspend left off\n");
		return 0;
	}
	if (!spm_vcorefs_init_done(&vflag)) {
		pr_err("vcorefs init not done (spm_vcorefs.init=1), deep suspend left off\n");
		return 0;
	}
	if (!psci_power_state_is_valid(MTK_SUSPEND_PSCI_STATE)) {
		pr_err("PSCI state 0x%x not valid, deep suspend left off\n",
		       MTK_SUSPEND_PSCI_STATE);
		return 0;
	}
	if (!spm_irq_0) {
		pr_err("no SPM IRQ0, deep suspend left off\n");
		return 0;
	}
	ret = mtk_cirq_init();
	if (ret) {
		pr_err("no SYS_CIRQ (%d), deep suspend left off\n", ret);
		return 0;
	}
	for (i = 0; i < ARRAY_SIZE(wake_irq_src); i++)
		wake_irq_ok[i] = !mtk_dt_irq_parse(wake_irq_src[i].compatible,
						   wake_irq_src[i].index, &wake_irq[i]);

	/* makes deep current unless mem_sleep_default=s2idle was given */
	suspend_set_ops(&spm_suspend_ops);
	registered = true;
	pr_info("deep suspend offered (flags 0x%x 0x%x, 0x%x after vcorefs 0x%x), mem_sleep %s\n",
		SLP_SPM_FLAGS, SLP_SPM_FLAGS1, spm_suspend_effective_flags(), vflag,
		mem_sleep_current == PM_SUSPEND_MEM ? "deep" : "s2idle");
	return 0;
}
late_initcall_sync(spm_suspend_init);
