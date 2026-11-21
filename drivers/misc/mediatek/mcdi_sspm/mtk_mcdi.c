// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 MCDI core-off, the AP side.
 *
 * cpuidle-psci enters the mcdi-cpu state (PSCI 0x10001). ATF saves the core,
 * puts its GIC interface to sleep and flags SSPM; the MCDI task in the SSPM
 * firmware powers the core off and back on. This driver does what T's mcdi_v1
 * does on mt6771 through SSPM mailbox 3:
 * - AVAIL_CPU_MASK, the online CPUs, at enable and after every hotplug
 *   (mcdi_avail_cpu_cluster_update)
 * - CPU_ISOLATION_MASK 0 at enable (mcdi_init)
 * - around hotplug, stock's cpuhp callbacks: keep CPUs out of the state, wake
 *   them all and wait until none is in it, then write the new mask and resume
 * - PAUSE_ACTION and the wait for PAUSE_ACK around SPM deep suspend's enter,
 *   only when enabled (mtk_mcdi_task_pause, T's _mcdi_task_pause)
 * Stock mt6771 writes no AP_READY (mcdi_ap_ready is empty without
 * MCDI_CPC_MODE), and the any-core path's PAUSE_ACTION isn't ported.
 *
 * Off by default. Until mtk_mcdi.enable is 1 nothing is written to mailbox 3
 * and every state past WFI is disabled by the driver
 * (CPUIDLE_STATE_DISABLED_BY_DRIVER), which both the menu governor and s2idle's
 * find_deepest_state skip, whatever cpuidle/stateN/disable says. The same
 * happens on any pause: enable=0, a failed enable, hotplug. A CPU that picked
 * the state just before a pause is refused by the CPU PM notifier and goes
 * round the idle loop once; it can't pick the state again, so it never spins.
 * A 2 us CPU latency request (stock's pause value) is held while paused too.
 * The state also starts disabled for the user on every CPU
 * (mediatek,default-off in the DT): each CPU needs cpuidle/state1/disable
 * set to 0 as well.
 */
#define pr_fmt(fmt) "mtk_mcdi: " fmt

#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/cpuhotplug.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mtk-mcdi.h>
#include <linux/soc/mediatek/mtk-sspm.h>

#define MCDI_NR_CPUS		8	/* T's NF_CPU, bits 0 to 7 of AVAIL_CPU_MASK */
#define MCDI_PAUSE_LATENCY_US	2	/* T's set_mcdi_enable_by_pm_qos(false) */
#define MCDI_WAIT_US		1000000	/* T spins with no limit */
#define MCDI_PAUSE_ACK_US	10000	/* T spins with no limit; SSPM's polling IPI limit */

#define MCDI_PAUSE_OFF		BIT(0)	/* mtk_mcdi.enable is 0 */
#define MCDI_PAUSE_HOTPLUG	BIT(1)
#define MCDI_PAUSE_SUSPEND	BIT(2)	/* PM_SUSPEND_PREPARE to PM_POST_SUSPEND */

static DEFINE_MUTEX(mcdi_lock);
static bool mcdi_enable;		/* the parameter; mcdi_lock once mcdi_inited */
static bool mcdi_inited;
static bool mcdi_broken;		/* no hotplug callbacks: never enable */
static unsigned int mcdi_pauseby = MCDI_PAUSE_OFF;	/* mcdi_lock */
static bool mcdi_paused = true;		/* read by the notifier in idle */
static u32 mcdi_avail;			/* last AVAIL_CPU_MASK written */
static struct pm_qos_request mcdi_qos;

static unsigned int mcdi_task_pauses, mcdi_task_timeouts;	/* suspend enter only */
static DEFINE_PER_CPU(int, mcdi_in_state);
static DEFINE_PER_CPU(unsigned long, mcdi_entered);
static DEFINE_PER_CPU(unsigned long, mcdi_refused);

/* Runs on the idling CPU with IRQs off: no locks, no printk */
static int mcdi_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	if (!is_idle_task(current))
		return NOTIFY_OK;

	switch (action) {
	case CPU_PM_ENTER:
		this_cpu_write(mcdi_in_state, 1);
		smp_mb();	/* in_state before paused; pairs with mcdi_pause() */
		if (READ_ONCE(mcdi_paused)) {
			this_cpu_write(mcdi_in_state, 0);
			this_cpu_inc(mcdi_refused);
			return notifier_from_errno(-EBUSY);
		}
		this_cpu_inc(mcdi_entered);
		break;
	case CPU_PM_ENTER_FAILED:
	case CPU_PM_EXIT:
		this_cpu_write(mcdi_in_state, 0);
		break;
	}
	return NOTIFY_OK;
}

/* First in the chain, so a refusal needs no ENTER_FAILED calls to the others */
static struct notifier_block mcdi_pm_nb = {
	.notifier_call = mcdi_pm_notify,
	.priority = INT_MAX,
};

static u32 mcdi_online_mask(void)
{
	return cpumask_bits(cpu_online_mask)[0] & GENMASK(MCDI_NR_CPUS - 1, 0);
}

static void mcdi_write_avail(void)
{
	mcdi_avail = mcdi_online_mask();
	sspm_mcdi_mbox_write(MCDI_MBOX_AVAIL_CPU_MASK, mcdi_avail);
}

/*
 * Disable or allow every state past WFI on every CPU (cpuidle-psci has one
 * driver per CPU). Process context: takes cpuidle_lock.
 */
static void mcdi_states_disable(bool off)
{
	struct cpuidle_device *dev;
	struct cpuidle_driver *drv;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		dev = per_cpu(cpuidle_devices, cpu);
		drv = dev ? cpuidle_get_cpu_driver(dev) : NULL;
		if (!drv)
			continue;
		for (i = 1; i < drv->state_count; i++)
			cpuidle_driver_state_disabled(drv, i, off);
	}
}

/* T's mcdi_wakeup_all_cpu: kick idle CPUs and wait until none is in the state */
static void mcdi_wait_all_on(void)
{
	ktime_t end = ktime_add_us(ktime_get(), MCDI_WAIT_US);
	int cpu, busy;

	wake_up_all_online_idle_cpus();
	do {
		busy = -1;
		for_each_online_cpu(cpu)
			if (READ_ONCE(per_cpu(mcdi_in_state, cpu)))
				busy = cpu;
		if (busy < 0)
			return;
		udelay(10);
	} while (ktime_before(ktime_get(), end));
	pr_err("CPU%d still in mcdi after %d us\n", busy, MCDI_WAIT_US);
}

/* mcdi_lock held. Any pause bit keeps every CPU out of the state. */
static void mcdi_pause(unsigned int bit, bool on)
{
	unsigned int old = mcdi_pauseby;

	mcdi_pauseby = on ? old | bit : old & ~bit;
	if (!old == !mcdi_pauseby)
		return;
	if (mcdi_pauseby) {
		mcdi_states_disable(true);
		WRITE_ONCE(mcdi_paused, true);
		smp_mb();	/* paused before reading in_state */
		cpu_latency_qos_update_request(&mcdi_qos, MCDI_PAUSE_LATENCY_US);
		mcdi_wait_all_on();
	} else {
		cpu_latency_qos_update_request(&mcdi_qos, PM_QOS_DEFAULT_VALUE);
		WRITE_ONCE(mcdi_paused, false);
		mcdi_states_disable(false);
	}
}

/* T's mcdi_cpuhp_notify_enter */
static int mcdi_cpuhp_enter(unsigned int cpu)
{
	mutex_lock(&mcdi_lock);
	mcdi_pause(MCDI_PAUSE_HOTPLUG, true);
	mutex_unlock(&mcdi_lock);
	return 0;
}

/* T's mcdi_cpuhp_notify_leave */
static int mcdi_cpuhp_leave(unsigned int cpu)
{
	mutex_lock(&mcdi_lock);
	if (mcdi_enable)
		mcdi_write_avail();
	mcdi_pause(MCDI_PAUSE_HOTPLUG, false);
	mutex_unlock(&mcdi_lock);
	return 0;
}

/* Keeps every CPU out of core-off for any suspend; deep suspend's enter adds PAUSE_ACTION */
static int mcdi_suspend_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
		mutex_lock(&mcdi_lock);
		mcdi_pause(MCDI_PAUSE_SUSPEND, true);
		mutex_unlock(&mcdi_lock);
		break;
	case PM_POST_SUSPEND:
		mutex_lock(&mcdi_lock);
		mcdi_pause(MCDI_PAUSE_SUSPEND, false);
		mutex_unlock(&mcdi_lock);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block mcdi_suspend_nb = {
	.notifier_call = mcdi_suspend_notify,
};

/*
 * T's _mcdi_task_pause for SPM deep suspend: PAUSE_ACTION 1 (or 0) on
 * mailbox 3, then wait until SSPM's MCDI task answers with PAUSE_ACK. Does
 * nothing when MCDI is off, as stock's is_mcdi_working. Called from suspend
 * enter on CPU0 with the other CPUs offline and IRQs off, so it spins and
 * reads mcdi_enable without mcdi_lock (userspace is frozen). Returns
 * -ETIMEDOUT when SSPM doesn't answer within MCDI_PAUSE_ACK_US.
 */
int mtk_mcdi_task_pause(bool paused)
{
	u32 ack = !paused;
	int us, ret;

	if (!READ_ONCE(mcdi_enable))
		return 0;
	ret = sspm_mcdi_mbox_write(MCDI_MBOX_PAUSE_ACTION, paused);
	if (ret)
		return ret;
	for (us = 0; us < MCDI_PAUSE_ACK_US; us++) {
		if (!sspm_mbox_read(SHAREMBOX_NO_MCDI,
				    SHAREMBOX_OFFSET_MCDI + MCDI_MBOX_PAUSE_ACK, &ack, 1) &&
		    ack == paused) {
			if (paused)
				mcdi_task_pauses++;
			return 0;
		}
		udelay(1);
	}
	mcdi_task_timeouts++;
	pr_err("no PAUSE_ACK %d from SSPM in %d us (ack %u)\n", paused,
	       MCDI_PAUSE_ACK_US, ack);
	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(mtk_mcdi_task_pause);

static int mcdi_set_enable(bool on)
{
	int ret = 0;

	cpus_read_lock();
	mutex_lock(&mcdi_lock);
	if (on == mcdi_enable)
		goto out;
	if (on) {
		/* SSPM wrote its pin layout, so its firmware runs */
		if (mcdi_broken || !is_sspm_ready() || !sspm_ipi_is_inited()) {
			pr_err("SSPM IPI or hotplug callbacks not up, not enabled\n");
			ret = -ENODEV;
			goto out;
		}
		/* T's mcdi_init order: the mask (mcdi_governor_init), then isolation 0 */
		mcdi_write_avail();
		sspm_mcdi_mbox_write(MCDI_MBOX_CPU_ISOLATION_MASK, 0);
		mcdi_enable = true;
		mcdi_pause(MCDI_PAUSE_OFF, false);
		pr_info("enabled, avail 0x%02x, isolation 0\n", mcdi_avail);
	} else {
		mcdi_pause(MCDI_PAUSE_OFF, true);
		mcdi_enable = false;
		pr_info("disabled\n");
	}
out:
	mutex_unlock(&mcdi_lock);
	cpus_read_unlock();
	return ret;
}

static int mcdi_enable_param_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (!READ_ONCE(mcdi_inited)) {
		/* on the command line: mcdi_init acts on it */
		mcdi_enable = on;
		return 0;
	}
	return mcdi_set_enable(on);
}

static const struct kernel_param_ops mcdi_enable_ops = {
	.set = mcdi_enable_param_set,
	.get = param_get_bool,
};
module_param_cb(enable, &mcdi_enable_ops, &mcdi_enable, 0644);
MODULE_PARM_DESC(enable, "Send SSPM the CPU masks and allow the mcdi-cpu idle state");

static int mcdi_status_show(struct seq_file *m, void *v)
{
	u32 slot[SHAREMBOX_SIZE_MCDI];
	int cpu;

	mutex_lock(&mcdi_lock);
	seq_printf(m, "enable %d pauseby 0x%x avail 0x%02x\n", mcdi_enable, mcdi_pauseby, mcdi_avail);
	mutex_unlock(&mcdi_lock);
	seq_printf(m, "suspend task pauses %u timeouts %u\n", mcdi_task_pauses, mcdi_task_timeouts);
	for_each_possible_cpu(cpu)
		seq_printf(m, "cpu%d entered %lu refused %lu\n", cpu,
			   per_cpu(mcdi_entered, cpu), per_cpu(mcdi_refused, cpu));
	if (!sspm_mbox_read(SHAREMBOX_NO_MCDI, SHAREMBOX_OFFSET_MCDI, slot, SHAREMBOX_SIZE_MCDI))
		seq_printf(m, "avail 0x%08x pwr_stat 0x%08x action_stat %u iso 0x%08x cnt %u %u pause %u ack %u\n",
			   slot[MCDI_MBOX_AVAIL_CPU_MASK], slot[MCDI_MBOX_CPU_CLUSTER_PWR_STAT],
			   slot[MCDI_MBOX_ACTION_STAT], slot[MCDI_MBOX_CPU_ISOLATION_MASK],
			   slot[MCDI_MBOX_CLUSTER_0_CNT], slot[MCDI_MBOX_CLUSTER_1_CNT],
			   slot[MCDI_MBOX_PAUSE_ACTION], slot[MCDI_MBOX_PAUSE_ACK]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mcdi_status);

/* After cpuidle-psci (device_initcall) and the SSPM probe, as T's late_initcall mcdi_init */
static int __init mcdi_init(void)
{
	struct dentry *dir;
	bool want;
	int ret;

	/* mcdi_pauseby starts as MCDI_PAUSE_OFF */
	mcdi_states_disable(true);
	cpu_latency_qos_add_request(&mcdi_qos, MCDI_PAUSE_LATENCY_US);
	ret = cpu_pm_register_notifier(&mcdi_pm_nb);
	if (ret)
		return ret;
	ret = register_pm_notifier(&mcdi_suspend_nb);
	if (ret)
		return ret;
	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "mtk_mcdi:prepare",
					mcdi_cpuhp_enter, mcdi_cpuhp_leave);
	if (ret >= 0)
		ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "mtk_mcdi:online",
						mcdi_cpuhp_leave, mcdi_cpuhp_enter);
	if (ret < 0) {
		pr_err("cpuhp callbacks: %d\n", ret);
		mcdi_broken = true;
	}

	dir = debugfs_create_dir("mtk_mcdi", NULL);
	debugfs_create_file("status", 0444, dir, NULL, &mcdi_status_fops);

	want = mcdi_enable;
	mcdi_enable = false;
	WRITE_ONCE(mcdi_inited, true);
	if (want)
		mcdi_set_enable(true);
	return 0;
}
late_initcall(mcdi_init);
