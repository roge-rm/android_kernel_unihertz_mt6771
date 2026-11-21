// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 core and cluster power-down after CPU hotplug.
 *
 * ATF's PSCI CPU_OFF leaves the core powered in its final WFI, so the next
 * CPU_ON finds it already on, no reset reaches it and it never comes online.
 * Stock's cpuhp_cpu_dead (T's mtk_cpuhp_core.c and mtk_cpuhp_mt6771.c) powers
 * it off from a surviving CPU once it is dead:
 * - SIP 0xc2000212 (MTK_SIP_POWER_DOWN_CORE), x1 = CPU number, x2 to x7 = 0
 * - SIP 0xc2000210 (MTK_SIP_POWER_DOWN_CLUSTER), x1 = cluster, when no CPU of
 *   that cluster is online any more. ATF's PSCI on handler powers the cluster
 *   back on at affinity level 1 on the next CPU_ON.
 * ATF takes the CPU number as cluster * 4 + core (it rejects numbers above 7),
 * so it comes from the MPIDR here; on this board that equals the logical
 * number stock passes.
 * Stock also turns the cluster's ARM PLL and VPROC11/VSRAM11 off with the
 * cluster; we keep both on, so those steps are left out.
 *
 * The callback is a CPUHP_BP_PREPARE_DYN teardown, which runs on a control CPU
 * after the dying CPU is dead (after CPUHP_BRINGUP_CPU's teardown). Dynamic
 * states tear down highest number first and get numbers in registration
 * order, so this registers at device_initcall, before mtk_mcdi's late_initcall:
 * mtk_mcdi's AVAIL_CPU_MASK write then runs first, as on stock.
 *
 * The cluster step only runs for a CPU that reached CPUHP_AP_ONLINE_DYN. A
 * failed bringup also runs this teardown, and that CPU can still count as on
 * in ATF, which would then skip the cluster power-on at the next CPU_ON.
 * Stock powers the cluster down in that case too.
 */
#define pr_fmt(fmt) "mt6771_cpuhp: " fmt

#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>

#define MTK_SIP_POWER_DOWN_CLUSTER	0xc2000210
#define MTK_SIP_POWER_DOWN_CORE		0xc2000212

#define CPUHP_CORES_PER_CLUSTER		4	/* ATF's linear CPU number */
#define CPUHP_NR_CLUSTERS		2

/* CPUs that reached CPUHP_AP_ONLINE_DYN since they were last powered down */
static struct cpumask cpuhp_reached_online;

static unsigned int cpuhp_cluster(unsigned int cpu)
{
	return MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
}

static unsigned int cpuhp_core(unsigned int cpu)
{
	return MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0);
}

/* No online CPU left in the cluster; the dead CPU is already out of the mask */
static bool cpuhp_cluster_empty(unsigned int cluster)
{
	unsigned int cpu;

	for_each_online_cpu(cpu)
		if (cpuhp_cluster(cpu) == cluster)
			return false;
	return true;
}

static int cpuhp_cpu_online(unsigned int cpu)
{
	cpumask_set_cpu(cpu, &cpuhp_reached_online);
	return 0;
}

static int cpuhp_cpu_dead(unsigned int cpu)
{
	bool reached_online = cpumask_test_and_clear_cpu(cpu, &cpuhp_reached_online);
	unsigned int cluster = cpuhp_cluster(cpu);
	unsigned int core = cpuhp_core(cpu);
	unsigned long id = cluster * CPUHP_CORES_PER_CLUSTER + core;
	struct arm_smccc_res res;

	if (cluster >= CPUHP_NR_CLUSTERS || core >= CPUHP_CORES_PER_CLUSTER) {
		pr_err("CPU%u: MPIDR %#llx out of range, core left on\n",
		       cpu, cpu_logical_map(cpu));
		return 0;
	}

	arm_smccc_smc(MTK_SIP_POWER_DOWN_CORE, id, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		pr_err("CPU%u: core %lu power-down returned %#lx\n", cpu, id, res.a0);
	else
		pr_info("CPU%u: core %lu powered down (%#lx)\n", cpu, id, res.a0);

	if (!reached_online) {
		pr_info("CPU%u never came online, cluster %u left on\n", cpu, cluster);
		return 0;
	}

	/* Never cluster 0 while CPU0 is up; covered by the empty check, kept explicit */
	if (!cpuhp_cluster_empty(cluster) || (cluster == 0 && cpu_online(0)))
		return 0;

	arm_smccc_smc(MTK_SIP_POWER_DOWN_CLUSTER, cluster, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		pr_err("cluster %u power-down returned %#lx\n", cluster, res.a0);
	else
		pr_info("cluster %u powered down (%#lx)\n", cluster, res.a0);
	return 0;
}

static int __init mt6771_cpuhp_init(void)
{
	int online_state, ret;

	if (!of_machine_is_compatible("mediatek,mt6771"))
		return 0;

	/* Runs the online callback on every CPU already up */
	online_state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mt6771_cpuhp:online",
					 cpuhp_cpu_online, NULL);
	if (online_state < 0) {
		pr_err("cpuhp online callback: %d\n", online_state);
		return online_state;
	}

	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "mt6771_cpuhp:dead",
					NULL, cpuhp_cpu_dead);
	if (ret < 0) {
		pr_err("cpuhp dead callback: %d\n", ret);
		cpuhp_remove_state(online_state);
		return ret;
	}
	pr_info("core power-down on hotplug, cpuhp states %d and %d\n",
		online_state, ret);
	return 0;
}
/* Before mtk_mcdi (late_initcall), so its teardown runs first; see above */
device_initcall(mt6771_cpuhp_init);
