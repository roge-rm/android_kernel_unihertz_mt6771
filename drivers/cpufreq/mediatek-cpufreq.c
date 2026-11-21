// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 Linaro Ltd.
 * Author: Pi-Cheng Chen <pi-cheng.chen@linaro.org>
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <asm/unaligned.h>

#define MIN_VOLT_SHIFT		(100000)
#define MAX_VOLT_SHIFT		(200000)
#define MAX_VOLT_LIMIT		(1150000)
#define VOLT_TOL		(10000)

/*
 * The struct mtk_cpu_dvfs_info holds necessary information for doing CPU DVFS
 * on each CPU power/clock domain of Mediatek SoCs. Each CPU cluster in
 * Mediatek SoCs has two voltage inputs, Vproc and Vsram. In some cases the two
 * voltage inputs need to be controlled under a hardware limitation:
 * 100mV < Vsram - Vproc < 200mV
 *
 * When scaling the clock frequency of a CPU clock domain, the clock source
 * needs to be switched to another stable PLL clock temporarily until
 * the original PLL becomes stable at target frequency.
 */
struct mtk_cpu_dvfs_info {
	struct cpumask cpus;
	struct device *cpu_dev;
	struct regulator *proc_reg;
	struct regulator *sram_reg;
	struct clk *cpu_clk;
	struct clk *inter_clk;
	struct list_head list_head;
	int intermediate_voltage;
	int intermediate_min_voltage;	/* its opp-microvolt min, for no A53 floor */
	bool need_voltage_tracking;
	struct opp_table *hw_table;
	/* Under mtk_cci_lock: our last Vproc request, and the current OPP's voltage (uV) */
	int vproc_vote;
	int opp_vproc;
	unsigned long opp_hz;	/* the current OPP, 0 before the first */
	bool active;		/* policy started and not stopped (has an online CPU) */
	bool cci_rail;		/* MT6771 cluster with CPU0: shares VPROC12 with the CCI */
};

static struct platform_device *cpufreq_pdev;

/*
 * MT6771 speed bin, worked out as stock's _mt_cpufreq_get_cpu_level from
 * LK's atag,devinfo words 7, 50 and 120. The OPPs carry opp-supported-hw
 * with BIT(level); -1 means not MT6771, so no supported-hw is set.
 */
#define MT6771_LEVEL_V6		5
#define MT6771_LEVEL_V5_T	6
static int mt6771_cpu_level = -1;

static const char * const mt6771_level_names[] = {
	"V3", "V4", "V5_1", "V5_2", "V5_3", "V6", "V5_T", "V5_4",
};

/* Read devinfo word index from /chosen atag,devinfo (word 0 is the count). */
static int mt6771_read_devinfo(const u8 *tag, int length, int index, u32 *val)
{
	u32 count = get_unaligned_le32(tag);

	if (index >= count || count > (length - sizeof(u32)) / sizeof(u32))
		return -EINVAL;
	*val = get_unaligned_le32(tag + sizeof(u32) * (index + 1));
	return 0;
}

/*
 * Returns the stock CPU level, or a negative error when the bin is unknown
 * or V6. V6 (fy5Tbl) has other frequencies and lower voltages than the
 * board tables, so it never gets them, even with the turbo bit set.
 */
static int mt6771_get_cpu_level(struct device *dev)
{
	struct device_node *chosen;
	const u8 *tag;
	u32 efuse, seg, bin;
	int length, lv, ret = -ENODEV;
	bool segcode, turbo, a_code;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		goto unknown;
	tag = of_get_property(chosen, "atag,devinfo", &length);
	if (!tag || length < (int)sizeof(u32) ||
	    mt6771_read_devinfo(tag, length, 50, &efuse) ||
	    mt6771_read_devinfo(tag, length, 7, &seg) ||
	    mt6771_read_devinfo(tag, length, 120, &bin)) {
		of_node_put(chosen);
		goto unknown;
	}
	of_node_put(chosen);

	segcode = (seg >> 5) & 1;
	turbo = (seg >> 3) & 1;
	a_code = (seg >> 1) & 1;
	bin = (bin >> 4) & 7;

	dev_info(dev, "speed bin fuses: word 50 0x%08x, 7 0x%08x, 120 bin %u\n",
		 efuse, seg, bin);

	/* An unfused word 50 reads 0; stock would call that V3. */
	if (!efuse) {
		ret = -EINVAL;
		goto unknown;
	}
	if (segcode) {
		dev_warn(dev, "V6 speed bin (segment 1): cpufreq not registered, CPUs stay at boot clocks\n");
		return -EPERM;
	}

	if (efuse > 0x40)
		lv = bin == 0 ? 2 : bin == 1 ? 3 : 4;
	else if (efuse == 0x40)
		lv = 1;
	else
		lv = 0;
	if (a_code)
		lv = 7;
	if (turbo)
		lv = MT6771_LEVEL_V5_T;

	dev_info(dev, "speed bin level %d (%s)\n", lv, mt6771_level_names[lv]);
	return lv;

unknown:
	dev_warn(dev, "speed bin unknown (no devinfo fuses): cpufreq not registered, CPUs stay at boot clocks\n");
	return ret;
}

static LIST_HEAD(dvfs_info_list);

static struct mtk_cpu_dvfs_info *mtk_cpu_dvfs_info_lookup(int cpu)
{
	struct mtk_cpu_dvfs_info *info;

	list_for_each_entry(info, &dvfs_info_list, list_head) {
		if (cpumask_test_cpu(cpu, &info->cpus))
			return info;
	}

	return NULL;
}

static int mtk_cpufreq_voltage_tracking(struct mtk_cpu_dvfs_info *info,
					int new_vproc)
{
	struct regulator *proc_reg = info->proc_reg;
	struct regulator *sram_reg = info->sram_reg;
	int old_vproc, old_vsram, new_vsram, vsram, vproc, ret;

	old_vproc = regulator_get_voltage(proc_reg);
	if (old_vproc < 0) {
		pr_err("%s: invalid Vproc value: %d\n", __func__, old_vproc);
		return old_vproc;
	}
	/* Vsram should not exceed the maximum allowed voltage of SoC. */
	new_vsram = min(new_vproc + MIN_VOLT_SHIFT, MAX_VOLT_LIMIT);

	if (old_vproc < new_vproc) {
		/*
		 * When scaling up voltages, Vsram and Vproc scale up step
		 * by step. At each step, set Vsram to (Vproc + 200mV) first,
		 * then set Vproc to (Vsram - 100mV).
		 * Keep doing it until Vsram and Vproc hit target voltages.
		 */
		do {
			old_vsram = regulator_get_voltage(sram_reg);
			if (old_vsram < 0) {
				pr_err("%s: invalid Vsram value: %d\n",
				       __func__, old_vsram);
				return old_vsram;
			}
			old_vproc = regulator_get_voltage(proc_reg);
			if (old_vproc < 0) {
				pr_err("%s: invalid Vproc value: %d\n",
				       __func__, old_vproc);
				return old_vproc;
			}

			vsram = min(new_vsram, old_vproc + MAX_VOLT_SHIFT);

			if (vsram + VOLT_TOL >= MAX_VOLT_LIMIT) {
				vsram = MAX_VOLT_LIMIT;

				/*
				 * If the target Vsram hits the maximum voltage,
				 * try to set the exact voltage value first.
				 */
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram);
				if (ret)
					ret = regulator_set_voltage(sram_reg,
							vsram - VOLT_TOL,
							vsram);

				vproc = new_vproc;
			} else {
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram + VOLT_TOL);

				vproc = vsram - MIN_VOLT_SHIFT;
			}
			if (ret)
				return ret;

			ret = regulator_set_voltage(proc_reg, vproc,
						    vproc + VOLT_TOL);
			if (ret) {
				regulator_set_voltage(sram_reg, old_vsram,
						      old_vsram);
				return ret;
			}
		} while (vproc < new_vproc || vsram < new_vsram);
	} else if (old_vproc > new_vproc) {
		/*
		 * When scaling down voltages, Vsram and Vproc scale down step
		 * by step. At each step, set Vproc to (Vsram - 200mV) first,
		 * then set Vproc to (Vproc + 100mV).
		 * Keep doing it until Vsram and Vproc hit target voltages.
		 */
		do {
			old_vproc = regulator_get_voltage(proc_reg);
			if (old_vproc < 0) {
				pr_err("%s: invalid Vproc value: %d\n",
				       __func__, old_vproc);
				return old_vproc;
			}
			old_vsram = regulator_get_voltage(sram_reg);
			if (old_vsram < 0) {
				pr_err("%s: invalid Vsram value: %d\n",
				       __func__, old_vsram);
				return old_vsram;
			}

			vproc = max(new_vproc, old_vsram - MAX_VOLT_SHIFT);
			ret = regulator_set_voltage(proc_reg, vproc,
						    vproc + VOLT_TOL);
			if (ret)
				return ret;

			if (vproc == new_vproc)
				vsram = new_vsram;
			else
				vsram = max(new_vsram, vproc + MIN_VOLT_SHIFT);

			if (vsram + VOLT_TOL >= MAX_VOLT_LIMIT) {
				vsram = MAX_VOLT_LIMIT;

				/*
				 * If the target Vsram hits the maximum voltage,
				 * try to set the exact voltage value first.
				 */
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram);
				if (ret)
					ret = regulator_set_voltage(sram_reg,
							vsram - VOLT_TOL,
							vsram);
			} else {
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram + VOLT_TOL);
			}

			if (ret) {
				regulator_set_voltage(proc_reg, old_vproc,
						      old_vproc);
				return ret;
			}
		} while (vproc > new_vproc + VOLT_TOL ||
			 vsram > new_vsram + VOLT_TOL);
	}

	return 0;
}

/*
 * The lowest voltage an OPP runs at: the min of an opp-microvolt triplet
 * <target min max>, else its voltage. The board gives the A53 rows under
 * 825 mV a target of 825 mV (the CCI floor) and their worst-bin voltage as min.
 */
static int mtk_cpufreq_opp_min_uv(struct dev_pm_opp *opp)
{
	struct device_node *np = dev_pm_opp_get_of_node(opp);
	int uv = dev_pm_opp_get_voltage(opp);
	u32 min;

	if (np && of_property_count_u32_elems(np, "opp-microvolt") == 3 &&
	    !of_property_read_u32_index(np, "opp-microvolt", 1, &min) &&
	    min && min < uv)
		uv = min;
	of_node_put(np);
	return uv;
}

/*
 * Without voltage tracking the request is (vproc, regulator max): INT_MAX is
 * clamped to the rail's constraint, so another consumer on the same rail (the
 * MT6771 CCI) can ask for more without the ranges failing to overlap.
 */
static int mtk_cpufreq_set_voltage(struct mtk_cpu_dvfs_info *info, int vproc)
{
	int ret;

	if (info->need_voltage_tracking)
		ret = mtk_cpufreq_voltage_tracking(info, vproc);
	else
		ret = regulator_set_voltage(info->proc_reg, vproc, INT_MAX);
	if (!ret)
		info->vproc_vote = vproc;
	return ret;
}

/*
 * MT6771 CCI scaling. The CCI clock is CCIPLL through the
 * mcucfg bus mux and bus divider, on VPROC12 with the CPU0 cluster. Stock's
 * rule: the CCI follows the higher of the two clusters' current OPP voltages,
 * at the fastest CCI OPP at or under it. The CCI has its own vote on VPROC12.
 *
 * mtk_cci_lock is held over every cluster target, policy start and stop and
 * the sysfs knob, so the cluster votes, the CCI vote and the rule's inputs
 * change together. It is taken under policy->rwsem and the hotplug lock, and
 * only clock and regulator calls are made under it, never the other way.
 */
#define MT6771_CCI_FLOOR_UV	825000		/* 598 MHz needs 818.75 mV worst bin */
#define MT6771_CCI_BOOT_PLL	1196000000UL	/* boot state: CCIPLL 1196 MHz, bus /2 */
#define MT6771_CCI_BOOT_DIV	2
#define MT6771_CCI_DIV1_MIN_HZ	750000000UL	/* stock: /1 from 767 MHz, /2 at 689 and below */

struct mtk_cci_opp {
	unsigned long rate;	/* Hz */
	int uv;
};

struct mtk_cci {
	struct device *dev;
	struct clk *mux;	/* "cci", mcu_bus_sel */
	struct clk *div;	/* "div", mcu_bus_div */
	struct clk *pll;	/* "pll", ccipll */
	struct clk *inter;	/* "intermediate", clk26m */
	struct regulator *reg;	/* "proc", VPROC12 */
	struct opp_table *hw_table;
	struct mtk_cci_opp *opps;	/* ascending */
	int nr_opps;
	struct mtk_cpu_dvfs_info *ll;	/* the cluster on the same rail */
	bool managed;		/* the rule runs */
	int vote;		/* uV, 0 when the CCI holds no vote */
	unsigned long rate;	/* Hz, last rate we set or read at probe */
};

/* On by default */
static bool cci_param = true;
module_param_named(cci, cci_param, bool, 0444);
MODULE_PARM_DESC(cci, "MT6771: scale the CCI with the clusters at boot (0 = fixed 598 MHz)");

/*
 * With the floor the A53 OPPs use their target voltage,
 * at least 825 mV. Without it (the default), and with the CCI managed,
 * they use their opp-microvolt min. An unmanaged CCI keeps the 825 mV clamp
 * either way. Changed at run time with the a53_floor file, under mtk_cci_lock.
 */
static bool a53_floor;
module_param(a53_floor, bool, 0444);
MODULE_PARM_DESC(a53_floor, "MT6771: keep A53 OPPs at 825 mV or more at boot (0 = worst-bin voltages while the CCI is managed)");

static DEFINE_MUTEX(mtk_cci_lock);
static struct mtk_cci *mtk_cci;

/* An OPP's voltage as the cluster uses it: its min on the CCI rail without the floor */
static int mtk_cpufreq_opp_uv(struct mtk_cpu_dvfs_info *info,
			      struct dev_pm_opp *opp)
{
	if (info->cci_rail && !a53_floor)
		return mtk_cpufreq_opp_min_uv(opp);
	return dev_pm_opp_get_voltage(opp);
}

static unsigned int mtk_cci_clkdiv(unsigned long rate)
{
	return rate >= MT6771_CCI_DIV1_MIN_HZ ? 1 : 2;
}

/* 0 drops the vote: (0, INT_MAX) is clamped to the rail's full range. */
static int mtk_cci_vote(struct mtk_cci *cci, int uv)
{
	int ret = regulator_set_voltage(cci->reg, uv, INT_MAX);

	if (!ret)
		cci->vote = uv;
	return ret;
}

/* Bus mux to 26 MHz, set CCIPLL and the bus divider, mux back to CCIPLL. */
static int mtk_cci_set_clocks(struct mtk_cci *cci, unsigned long pll_rate,
			      unsigned int clkdiv)
{
	int ret, ret2;

	ret = clk_set_parent(cci->mux, cci->inter);
	if (ret)
		return ret;
	ret = clk_set_rate(cci->pll, pll_rate);
	if (!ret)
		ret = clk_set_rate(cci->div, clk_get_rate(cci->inter) / clkdiv);
	ret2 = clk_set_parent(cci->mux, cci->pll);
	return ret ?: ret2;
}

/*
 * The cluster sharing the rail must hold its own vote before the CCI lowers
 * its one: a rail with no vote drops to its constraint minimum. Unmanaged,
 * that vote is at least the 825 mV floor.
 */
static int mtk_cci_hold_rail(struct mtk_cci *cci)
{
	struct mtk_cpu_dvfs_info *ll = cci->ll;
	int need = ll->opp_vproc;

	if (!cci->managed)
		need = max(need, MT6771_CCI_FLOOR_UV);
	if (ll->vproc_vote >= need)
		return 0;
	return mtk_cpufreq_set_voltage(ll, need);
}

/*
 * Put the CCI back in the boot state: 598 MHz, bus /2, vote dropped. Clocks go
 * down before the vote does, and the A53 vote is covered first. On failure
 * it stops where it got to. Called with mtk_cci_lock held, managed false.
 */
static int mtk_cci_park(struct mtk_cci *cci)
{
	int ret = mtk_cci_hold_rail(cci);

	if (!ret)
		ret = mtk_cci_set_clocks(cci, MT6771_CCI_BOOT_PLL,
					 MT6771_CCI_BOOT_DIV);
	if (!ret) {
		cci->rate = MT6771_CCI_BOOT_PLL / MT6771_CCI_BOOT_DIV;
		ret = mtk_cci_vote(cci, 0);
	}
	if (ret)
		dev_err(cci->dev, "CCI back to 598 MHz failed (%d), left at %lu kHz, vote %d uV\n",
			ret, clk_get_rate(cci->div) / 1000, cci->vote);
	return ret;
}

/*
 * Move the CCI to pll_rate / clkdiv with a vote of uv: raise the vote first,
 * lower it last. On failure the old rate and vote are put back, the rule
 * stops and the CCI goes back to the boot state. Called with mtk_cci_lock held.
 */
static int mtk_cci_switch(struct mtk_cci *cci, unsigned long pll_rate,
			  unsigned int clkdiv, int uv)
{
	unsigned long rate = pll_rate / clkdiv;
	unsigned long old_pll = clk_get_rate(cci->pll);
	unsigned long old_rate = clk_get_rate(cci->div);
	unsigned int old_div = DIV_ROUND_CLOSEST(clk_get_rate(cci->mux),
						 max(old_rate, 1UL));
	int old_vote = cci->vote;
	int ret;

	if (uv > old_vote) {
		ret = mtk_cci_vote(cci, uv);
		if (ret)
			goto off;
	}
	if (rate != cci->rate) {
		ret = mtk_cci_set_clocks(cci, pll_rate, clkdiv);
		if (ret)
			goto restore;
		cci->rate = rate;
	}
	if (uv < old_vote) {
		ret = mtk_cci_hold_rail(cci);
		if (!ret)
			ret = mtk_cci_vote(cci, uv);
		if (ret)
			goto restore;
	}
	return 0;

restore:
	/* The vote only comes back down when the old clocks are back. */
	if (!mtk_cci_set_clocks(cci, old_pll, old_div)) {
		cci->rate = old_rate;
		if (cci->vote > old_vote && !mtk_cci_hold_rail(cci))
			mtk_cci_vote(cci, old_vote);
	}
off:
	cci->managed = false;
	dev_err(cci->dev, "CCI switch to %lu kHz at %d uV failed (%d), CCI scaling off\n",
		rate / 1000, uv, ret);
	mtk_cci_park(cci);
	return ret;
}

/* Apply the rule from the active clusters. Called with mtk_cci_lock held. */
static void mtk_cci_update(void)
{
	struct mtk_cci *cci = mtk_cci;
	struct mtk_cpu_dvfs_info *info;
	const struct mtk_cci_opp *opp;
	int i, v = 0;

	if (!cci || !cci->managed)
		return;
	list_for_each_entry(info, &dvfs_info_list, list_head)
		if (info->active)
			v = max(v, info->opp_vproc);
	if (!v)
		return;

	opp = &cci->opps[0];
	for (i = 1; i < cci->nr_opps; i++)
		if (cci->opps[i].uv <= v)
			opp = &cci->opps[i];
	if (opp->rate == cci->rate && opp->uv == cci->vote)
		return;
	mtk_cci_switch(cci, opp->rate * mtk_cci_clkdiv(opp->rate),
		       mtk_cci_clkdiv(opp->rate), opp->uv);
}

static int mtk_cci_enable(struct mtk_cci *cci)
{
	int ret;

	if (cci->managed)
		return 0;
	/* Without a vote the CCI sits at 598 MHz; cover it before the rule moves it. */
	if (!cci->vote) {
		ret = mtk_cci_hold_rail(cci);
		if (!ret)
			ret = mtk_cci_vote(cci, MT6771_CCI_FLOOR_UV);
		if (ret)
			return ret;
	}
	cci->managed = true;
	mtk_cci_update();
	return cci->managed ? 0 : -EIO;
}

/* Back to 598 MHz (CCIPLL 1196 MHz, /2) with the vote dropped. */
static int mtk_cci_disable(struct mtk_cci *cci)
{
	int ret;

	cci->managed = false;
	ret = mtk_cci_hold_rail(cci);
	if (ret)
		return ret;
	ret = mtk_cci_switch(cci, MT6771_CCI_BOOT_PLL, MT6771_CCI_BOOT_DIV,
			     MT6771_CCI_FLOOR_UV);
	if (ret)
		return ret;
	return mtk_cci_vote(cci, 0);
}

static ssize_t cci_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d\n", READ_ONCE(mtk_cci->managed));
}

static ssize_t cci_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&mtk_cci_lock);
	ret = on ? mtk_cci_enable(mtk_cci) : mtk_cci_disable(mtk_cci);
	cci_param = mtk_cci->managed;
	mutex_unlock(&mtk_cci_lock);
	return ret ?: count;
}

/*
 * Turn the A53 floor on or off at run time, with mtk_cci_lock held, so no
 * cluster target runs. The A53 cluster's current OPP is looked up again with
 * the new rule. Up: the A53 vote goes up first, then the CCI follows. Down:
 * the CCI follows first (it holds its own vote for its OPP), then the A53
 * vote drops, to 825 mV at least while the CCI is unmanaged. A failed raise
 * puts the floor back; a failed drop leaves the higher vote for the next
 * target to retry.
 */
static int mtk_cci_set_floor(struct mtk_cci *cci, bool on)
{
	struct mtk_cpu_dvfs_info *ll = cci->ll;
	unsigned long rate = ll->opp_hz;
	struct dev_pm_opp *opp;
	int uv, vproc, ret = 0;

	if (on == a53_floor)
		return 0;
	a53_floor = on;
	if (!rate)
		return 0;
	opp = dev_pm_opp_find_freq_ceil(ll->cpu_dev, &rate);
	if (IS_ERR(opp)) {
		a53_floor = !on;
		return PTR_ERR(opp);
	}
	uv = mtk_cpufreq_opp_uv(ll, opp);
	dev_pm_opp_put(opp);

	if (on) {
		vproc = cci->managed ? uv : max(uv, MT6771_CCI_FLOOR_UV);
		if (vproc > ll->vproc_vote) {
			ret = mtk_cpufreq_set_voltage(ll, vproc);
			if (ret) {
				a53_floor = false;
				return ret;
			}
		}
		ll->opp_vproc = uv;
		mtk_cci_update();
		return 0;
	}

	ll->opp_vproc = uv;
	mtk_cci_update();
	/* mtk_cci_update may have turned the CCI off after a failed switch */
	vproc = cci->managed ? uv : max(uv, MT6771_CCI_FLOOR_UV);
	if (vproc < ll->vproc_vote)
		ret = mtk_cpufreq_set_voltage(ll, vproc);
	return ret;
}

static ssize_t a53_floor_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sprintf(buf, "%d\n", READ_ONCE(a53_floor));
}

static ssize_t a53_floor_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&mtk_cci_lock);
	ret = mtk_cci_set_floor(mtk_cci, on);
	mutex_unlock(&mtk_cci_lock);
	return ret ?: count;
}

static ssize_t cci_cur_freq_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", clk_get_rate(mtk_cci->div) / 1000);
}

static struct kobj_attribute mtk_cci_attr = __ATTR_RW(cci);
static struct kobj_attribute mtk_cci_cur_freq_attr = __ATTR_RO(cci_cur_freq);
static struct kobj_attribute mtk_a53_floor_attr = __ATTR_RW(a53_floor);

static const struct attribute *mtk_cci_attrs[] = {
	&mtk_cci_attr.attr,
	&mtk_cci_cur_freq_attr.attr,
	&mtk_a53_floor_attr.attr,
	NULL,
};

static void mtk_cci_release(struct mtk_cci *cci)
{
	if (!cci)
		return;
	if (cci->nr_opps)
		dev_pm_opp_of_remove_table(cci->dev);
	if (cci->hw_table)
		dev_pm_opp_put_supported_hw(cci->hw_table);
	if (!IS_ERR_OR_NULL(cci->reg))
		regulator_put(cci->reg);
	if (!IS_ERR_OR_NULL(cci->mux))
		clk_put(cci->mux);
	if (!IS_ERR_OR_NULL(cci->div))
		clk_put(cci->div);
	if (!IS_ERR_OR_NULL(cci->pll))
		clk_put(cci->pll);
	if (!IS_ERR_OR_NULL(cci->inter))
		clk_put(cci->inter);
	kfree(cci->opps);
	put_device(cci->dev);
	kfree(cci);
}

/*
 * Get the CCI node named by cpu0's mediatek,cci: clocks, its own VPROC12
 * consumer and OPP table. Touches no hardware. Returns -EPROBE_DEFER to retry
 * the probe; any other failure leaves the CCI unmanaged.
 */
static int mtk_cci_setup(struct device *dev)
{
	struct device *cpu_dev = get_cpu_device(0);
	struct platform_device *pdev;
	struct device_node *np;
	struct mtk_cci *cci;
	struct dev_pm_opp *opp;
	unsigned long rate;
	int i, ret;

	np = cpu_dev ? of_parse_phandle(cpu_dev->of_node, "mediatek,cci", 0) : NULL;
	if (!np) {
		dev_info(dev, "no mediatek,cci: CCI fixed, A53 floor %d uV\n",
			 MT6771_CCI_FLOOR_UV);
		return 0;
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		ret = -ENODEV;
		goto fail_nodev;
	}

	cci = kzalloc(sizeof(*cci), GFP_KERNEL);
	if (!cci) {
		put_device(&pdev->dev);
		ret = -ENOMEM;
		goto fail_nodev;
	}
	cci->dev = &pdev->dev;
	cci->ll = mtk_cpu_dvfs_info_lookup(0);

	cci->mux = clk_get(cci->dev, "cci");
	cci->div = clk_get(cci->dev, "div");
	cci->pll = clk_get(cci->dev, "pll");
	cci->inter = clk_get(cci->dev, "intermediate");
	cci->reg = regulator_get_optional(cci->dev, "proc");
	ret = PTR_ERR_OR_ZERO(cci->mux) ?: PTR_ERR_OR_ZERO(cci->div) ?:
	      PTR_ERR_OR_ZERO(cci->pll) ?: PTR_ERR_OR_ZERO(cci->inter) ?:
	      PTR_ERR_OR_ZERO(cci->reg);
	if (ret)
		goto fail;
	if (!cci->ll || !cci->ll->cci_rail) {
		ret = -ENODEV;
		goto fail;
	}

	if (mt6771_cpu_level >= 0) {
		u32 hw = BIT(mt6771_cpu_level);

		cci->hw_table = dev_pm_opp_set_supported_hw(cci->dev, &hw, 1);
		if (IS_ERR(cci->hw_table)) {
			ret = PTR_ERR(cci->hw_table);
			cci->hw_table = NULL;
			goto fail;
		}
	}
	ret = dev_pm_opp_of_add_table(cci->dev);
	if (ret)
		goto fail;
	ret = dev_pm_opp_get_opp_count(cci->dev);
	if (ret <= 0) {
		dev_pm_opp_of_remove_table(cci->dev);
		ret = ret ?: -ENODATA;
		goto fail;
	}
	cci->nr_opps = ret;
	cci->opps = kcalloc(cci->nr_opps, sizeof(*cci->opps), GFP_KERNEL);
	if (!cci->opps) {
		ret = -ENOMEM;
		goto fail;
	}
	for (i = 0, rate = 0; i < cci->nr_opps; i++, rate++) {
		opp = dev_pm_opp_find_freq_ceil(cci->dev, &rate);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			goto fail;
		}
		cci->opps[i].rate = rate;
		cci->opps[i].uv = dev_pm_opp_get_voltage(opp);
		dev_pm_opp_put(opp);
	}

	cci->rate = clk_get_rate(cci->div);
	mtk_cci = cci;
	dev_info(dev, "CCI %lu kHz, %d OPPs %lu to %lu kHz, scaling %s, A53 floor %s at boot\n",
		 cci->rate / 1000, cci->nr_opps, cci->opps[0].rate / 1000,
		 cci->opps[cci->nr_opps - 1].rate / 1000, cci_param ? "on" : "off",
		 a53_floor ? "on" : "off");
	return 0;

fail:
	mtk_cci_release(cci);
	if (ret == -EPROBE_DEFER)
		return ret;
fail_nodev:
	dev_err(dev, "CCI not managed (%d): CCI fixed, A53 floor %d uV\n",
		ret, MT6771_CCI_FLOOR_UV);
	return 0;
}

static int __mtk_cpufreq_set_target(struct cpufreq_policy *policy,
				    unsigned int index)
{
	struct cpufreq_frequency_table *freq_table = policy->freq_table;
	struct clk *cpu_clk = policy->clk;
	struct clk *armpll = clk_get_parent(cpu_clk);
	struct mtk_cpu_dvfs_info *info = policy->driver_data;
	struct device *cpu_dev = info->cpu_dev;
	struct dev_pm_opp *opp;
	long freq_hz, old_freq_hz;
	int vproc, opp_vproc, old_vproc, inter_vproc, target_vproc, ret;

	inter_vproc = info->cci_rail && !a53_floor ?
		      info->intermediate_min_voltage : info->intermediate_voltage;

	old_freq_hz = clk_get_rate(cpu_clk);
	/*
	 * Our own request, once there is one: the rail can read higher because
	 * of another consumer's vote, which may go away.
	 */
	old_vproc = info->vproc_vote;
	if (!old_vproc)
		old_vproc = regulator_get_voltage(info->proc_reg);
	if (old_vproc < 0) {
		pr_err("%s: invalid Vproc value: %d\n", __func__, old_vproc);
		return old_vproc;
	}

	freq_hz = freq_table[index].frequency * 1000;

	opp = dev_pm_opp_find_freq_ceil(cpu_dev, &freq_hz);
	if (IS_ERR(opp)) {
		pr_err("cpu%d: failed to find OPP for %ld\n",
		       policy->cpu, freq_hz);
		return PTR_ERR(opp);
	}
	opp_vproc = vproc = mtk_cpufreq_opp_uv(info, opp);
	dev_pm_opp_put(opp);

	/* The CPU0 cluster keeps VPROC12 high enough for a CCI left at 598 MHz. */
	if (info->cci_rail && !(mtk_cci && mtk_cci->managed))
		vproc = max(vproc, MT6771_CCI_FLOOR_UV);

	/*
	 * If the new voltage or the intermediate voltage is higher than the
	 * current voltage, scale up voltage first.
	 */
	target_vproc = (inter_vproc > vproc) ? inter_vproc : vproc;
	if (old_vproc < target_vproc) {
		ret = mtk_cpufreq_set_voltage(info, target_vproc);
		if (ret) {
			pr_err("cpu%d: failed to scale up voltage!\n",
			       policy->cpu);
			mtk_cpufreq_set_voltage(info, old_vproc);
			return ret;
		}
	}

	/* Reparent the CPU clock to intermediate clock. */
	ret = clk_set_parent(cpu_clk, info->inter_clk);
	if (ret) {
		pr_err("cpu%d: failed to re-parent cpu clock!\n",
		       policy->cpu);
		mtk_cpufreq_set_voltage(info, old_vproc);
		WARN_ON(1);
		return ret;
	}

	/* Set the original PLL to target rate. */
	ret = clk_set_rate(armpll, freq_hz);
	if (ret) {
		pr_err("cpu%d: failed to scale cpu clock rate!\n",
		       policy->cpu);
		clk_set_parent(cpu_clk, armpll);
		mtk_cpufreq_set_voltage(info, old_vproc);
		return ret;
	}

	/* Set parent of CPU clock back to the original PLL. */
	ret = clk_set_parent(cpu_clk, armpll);
	if (ret) {
		pr_err("cpu%d: failed to re-parent cpu clock!\n",
		       policy->cpu);
		mtk_cpufreq_set_voltage(info, inter_vproc);
		WARN_ON(1);
		return ret;
	}

	/*
	 * If the new voltage is lower than the intermediate voltage or the
	 * original voltage, scale down to the new voltage. Also when we hold no
	 * request at it yet.
	 */
	if (vproc < inter_vproc || vproc < old_vproc ||
	    info->vproc_vote != vproc) {
		ret = mtk_cpufreq_set_voltage(info, vproc);
		if (ret) {
			pr_err("cpu%d: failed to scale down voltage!\n",
			       policy->cpu);
			clk_set_parent(cpu_clk, info->inter_clk);
			clk_set_rate(armpll, old_freq_hz);
			clk_set_parent(cpu_clk, armpll);
			return ret;
		}
	}

	info->opp_vproc = opp_vproc;
	info->opp_hz = freq_hz;
	return 0;
}

/* After a cluster change the MT6771 CCI follows, under the same lock. */
static int mtk_cpufreq_set_target(struct cpufreq_policy *policy,
				  unsigned int index)
{
	int ret;

	mutex_lock(&mtk_cci_lock);
	ret = __mtk_cpufreq_set_target(policy, index);
	if (!ret)
		mtk_cci_update();
	mutex_unlock(&mtk_cci_lock);
	return ret;
}

#define DYNAMIC_POWER "dynamic-power-coefficient"

static int mtk_cpu_dvfs_info_init(struct mtk_cpu_dvfs_info *info, int cpu)
{
	struct device *cpu_dev;
	struct regulator *proc_reg = ERR_PTR(-ENODEV);
	struct regulator *sram_reg = ERR_PTR(-ENODEV);
	struct clk *cpu_clk = ERR_PTR(-ENODEV);
	struct clk *inter_clk = ERR_PTR(-ENODEV);
	struct dev_pm_opp *opp;
	unsigned long rate;
	int ret;

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev) {
		pr_err("failed to get cpu%d device\n", cpu);
		return -ENODEV;
	}

	cpu_clk = clk_get(cpu_dev, "cpu");
	if (IS_ERR(cpu_clk)) {
		if (PTR_ERR(cpu_clk) == -EPROBE_DEFER)
			pr_warn("cpu clk for cpu%d not ready, retry.\n", cpu);
		else
			pr_err("failed to get cpu clk for cpu%d\n", cpu);

		ret = PTR_ERR(cpu_clk);
		return ret;
	}

	inter_clk = clk_get(cpu_dev, "intermediate");
	if (IS_ERR(inter_clk)) {
		if (PTR_ERR(inter_clk) == -EPROBE_DEFER)
			pr_warn("intermediate clk for cpu%d not ready, retry.\n",
				cpu);
		else
			pr_err("failed to get intermediate clk for cpu%d\n",
			       cpu);

		ret = PTR_ERR(inter_clk);
		goto out_free_resources;
	}

	proc_reg = regulator_get_optional(cpu_dev, "proc");
	if (IS_ERR(proc_reg)) {
		if (PTR_ERR(proc_reg) == -EPROBE_DEFER)
			pr_warn("proc regulator for cpu%d not ready, retry.\n",
				cpu);
		else
			pr_err("failed to get proc regulator for cpu%d\n",
			       cpu);

		ret = PTR_ERR(proc_reg);
		goto out_free_resources;
	}

	/* Both presence and absence of sram regulator are valid cases. */
	sram_reg = regulator_get_exclusive(cpu_dev, "sram");

	/* Get OPP-sharing information from "operating-points-v2" bindings */
	ret = dev_pm_opp_of_get_sharing_cpus(cpu_dev, &info->cpus);
	if (ret) {
		pr_err("failed to get OPP-sharing information for cpu%d\n",
		       cpu);
		goto out_free_resources;
	}

	/* MT6771: only the OPPs whose opp-supported-hw has this bin's bit. */
	if (mt6771_cpu_level >= 0) {
		u32 hw = BIT(mt6771_cpu_level);

		info->hw_table = dev_pm_opp_set_supported_hw(cpu_dev, &hw, 1);
		if (IS_ERR(info->hw_table)) {
			ret = PTR_ERR(info->hw_table);
			info->hw_table = NULL;
			pr_err("failed to set supported hw for cpu%d: %d\n",
			       cpu, ret);
			goto out_free_resources;
		}
	}

	ret = dev_pm_opp_of_cpumask_add_table(&info->cpus);
	if (ret) {
		pr_warn("no OPP table for cpu%d\n", cpu);
		goto out_put_hw;
	}

	/* Search a safe voltage for intermediate frequency. */
	rate = clk_get_rate(inter_clk);
	opp = dev_pm_opp_find_freq_ceil(cpu_dev, &rate);
	if (IS_ERR(opp)) {
		pr_err("failed to get intermediate opp for cpu%d\n", cpu);
		ret = PTR_ERR(opp);
		goto out_free_opp_table;
	}
	info->intermediate_voltage = dev_pm_opp_get_voltage(opp);
	info->intermediate_min_voltage = mtk_cpufreq_opp_min_uv(opp);
	dev_pm_opp_put(opp);

	info->cpu_dev = cpu_dev;
	info->proc_reg = proc_reg;
	info->sram_reg = IS_ERR(sram_reg) ? NULL : sram_reg;
	info->cpu_clk = cpu_clk;
	info->inter_clk = inter_clk;

	/*
	 * If SRAM regulator is present, software "voltage tracking" is needed
	 * for this CPU power domain.
	 */
	info->need_voltage_tracking = !IS_ERR(sram_reg);

	return 0;

out_free_opp_table:
	dev_pm_opp_of_cpumask_remove_table(&info->cpus);

out_put_hw:
	if (info->hw_table) {
		dev_pm_opp_put_supported_hw(info->hw_table);
		info->hw_table = NULL;
	}

out_free_resources:
	if (!IS_ERR(proc_reg))
		regulator_put(proc_reg);
	if (!IS_ERR(sram_reg))
		regulator_put(sram_reg);
	if (!IS_ERR(cpu_clk))
		clk_put(cpu_clk);
	if (!IS_ERR(inter_clk))
		clk_put(inter_clk);

	return ret;
}

static void mtk_cpu_dvfs_info_release(struct mtk_cpu_dvfs_info *info)
{
	if (!IS_ERR(info->proc_reg))
		regulator_put(info->proc_reg);
	if (!IS_ERR(info->sram_reg))
		regulator_put(info->sram_reg);
	if (!IS_ERR(info->cpu_clk))
		clk_put(info->cpu_clk);
	if (!IS_ERR(info->inter_clk))
		clk_put(info->inter_clk);

	dev_pm_opp_of_cpumask_remove_table(&info->cpus);
	if (info->hw_table)
		dev_pm_opp_put_supported_hw(info->hw_table);
}

/* Top of the table, leaving out the turbo-mode OPPs unless boost is on. */
static unsigned int mtk_cpufreq_table_max(struct cpufreq_frequency_table *table,
					  bool boost)
{
	struct cpufreq_frequency_table *pos;
	unsigned int max_freq = 0;

	cpufreq_for_each_valid_entry(pos, table) {
		if (!boost && (pos->flags & CPUFREQ_BOOST_FREQ))
			continue;
		max_freq = max(max_freq, pos->frequency);
	}
	return max_freq;
}

/*
 * Boost on or off: cpuinfo.max_freq follows the table with or without the
 * turbo-mode OPPs, then the limits are worked out again from every QoS
 * request (user, thermal, etc.) clamped to it. Unlike the core's software
 * boost, cpuinfo.max_freq goes back down when boost goes off, so
 * scaling_max_freq can't reach a turbo OPP. The user's scaling_max_freq
 * request is left as it is.
 *
 * Called from cpufreq_boost_trigger_state() with the CPU hotplug lock held
 * for reading and no cpufreq lock, so taking policy->rwsem here is safe.
 */
static int mtk_cpufreq_set_boost(struct cpufreq_policy *policy, int state)
{
	unsigned int max_freq;
	int ret = 0;

	down_write(&policy->rwsem);
	if (!policy->freq_table) {
		ret = -ENXIO;
		goto out;
	}
	max_freq = mtk_cpufreq_table_max(policy->freq_table, state);
	if (!max_freq) {
		ret = -EINVAL;
		goto out;
	}
	policy->cpuinfo.max_freq = max_freq;
	refresh_frequency_limits(policy);
out:
	up_write(&policy->rwsem);
	return ret;
}

static struct freq_attr *mtk_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&cpufreq_freq_attr_scaling_boost_freqs,
	NULL,
};

static int mtk_cpufreq_init(struct cpufreq_policy *policy)
{
	struct mtk_cpu_dvfs_info *info;
	struct cpufreq_frequency_table *freq_table;
	struct dev_pm_opp *opp;
	unsigned long rate;
	int ret;

	info = mtk_cpu_dvfs_info_lookup(policy->cpu);
	if (!info) {
		pr_err("dvfs info for cpu%d is not initialized.\n",
		       policy->cpu);
		return -EINVAL;
	}

	ret = dev_pm_opp_init_cpufreq_table(info->cpu_dev, &freq_table);
	if (ret) {
		pr_err("failed to init cpufreq table for cpu%d: %d\n",
		       policy->cpu, ret);
		return ret;
	}

	/*
	 * A policy brought back after its CPUs were all offline keeps its old
	 * cpuinfo.max_freq, and the core only ever raises it. Set it from the
	 * boost state now, so a boost change while the cluster was offline
	 * holds both ways.
	 */
	policy->cpuinfo.max_freq =
		mtk_cpufreq_table_max(freq_table, cpufreq_boost_enabled());

	cpumask_copy(policy->cpus, &info->cpus);
	policy->freq_table = freq_table;
	policy->driver_data = info;
	policy->clk = info->cpu_clk;

	dev_pm_opp_of_register_em(info->cpu_dev, policy->cpus);

	/* The cluster counts for the CCI again, at the OPP its clock is on. */
	rate = clk_get_rate(info->cpu_clk);
	opp = dev_pm_opp_find_freq_ceil(info->cpu_dev, &rate);
	if (IS_ERR(opp))
		opp = dev_pm_opp_find_freq_floor(info->cpu_dev, &rate);
	mutex_lock(&mtk_cci_lock);
	if (!IS_ERR(opp)) {
		info->opp_vproc = mtk_cpufreq_opp_uv(info, opp);
		info->opp_hz = rate;
		dev_pm_opp_put(opp);
	}
	info->active = true;
	mtk_cci_update();
	mutex_unlock(&mtk_cci_lock);

	return 0;
}

/*
 * Runs when the policy's last CPU goes offline (before the P14 cpuhp dead
 * callback powers the cluster down) and on unregister. The cluster no longer
 * counts for the CCI; its vote stays.
 */
static int mtk_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct mtk_cpu_dvfs_info *info = policy->driver_data;

	mutex_lock(&mtk_cci_lock);
	info->active = false;
	mtk_cci_update();
	mutex_unlock(&mtk_cci_lock);

	dev_pm_opp_free_cpufreq_table(info->cpu_dev, &policy->freq_table);

	return 0;
}

static struct cpufreq_driver mtk_cpufreq_driver = {
	.flags = CPUFREQ_STICKY | CPUFREQ_NEED_INITIAL_FREQ_CHECK |
		 CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
		 CPUFREQ_IS_COOLING_DEV,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = mtk_cpufreq_set_target,
	.get = cpufreq_generic_get,
	.init = mtk_cpufreq_init,
	.exit = mtk_cpufreq_exit,
	.name = "mtk-cpufreq",
	.attr = mtk_cpufreq_attr,
};

static int mtk_cpufreq_probe(struct platform_device *pdev)
{
	struct mtk_cpu_dvfs_info *info, *tmp;
	int cpu, ret;

	/*
	 * MT6771: refuse an unknown or V6 bin before anything is touched. The
	 * turbo-mode OPPs (above 1417 MHz) stay out of the policy until
	 * cpufreq/boost is set to 1; boost is off at boot.
	 */
	if (of_machine_is_compatible("mediatek,mt6771")) {
		mt6771_cpu_level = mt6771_get_cpu_level(&pdev->dev);
		if (mt6771_cpu_level < 0)
			return -ENODEV;
		mtk_cpufreq_driver.set_boost = mtk_cpufreq_set_boost;
	}

	for_each_possible_cpu(cpu) {
		info = mtk_cpu_dvfs_info_lookup(cpu);
		if (info)
			continue;

		info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
		if (!info) {
			ret = -ENOMEM;
			goto release_dvfs_info_list;
		}

		ret = mtk_cpu_dvfs_info_init(info, cpu);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to initialize dvfs info for cpu%d\n",
				cpu);
			goto release_dvfs_info_list;
		}

		info->cci_rail = mt6771_cpu_level >= 0 &&
				 cpumask_test_cpu(0, &info->cpus);
		list_add(&info->list_head, &dvfs_info_list);
	}

	if (mt6771_cpu_level >= 0) {
		ret = mtk_cci_setup(&pdev->dev);
		if (ret)
			goto release_dvfs_info_list;
	}

	ret = cpufreq_register_driver(&mtk_cpufreq_driver);
	if (ret) {
		dev_err(&pdev->dev, "failed to register mtk cpufreq driver\n");
		goto release_dvfs_info_list;
	}

	if (mtk_cci) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(0);

		/* policy0's parent is the cpufreq folder, where boost is. */
		if (!policy || sysfs_create_files(policy->kobj.parent, mtk_cci_attrs))
			dev_warn(&pdev->dev, "no cci files in the cpufreq folder\n");
		if (policy)
			cpufreq_cpu_put(policy);
		if (cci_param) {
			mutex_lock(&mtk_cci_lock);
			ret = mtk_cci_enable(mtk_cci);
			mutex_unlock(&mtk_cci_lock);
			if (ret)
				dev_err(&pdev->dev, "CCI scaling not started: %d\n", ret);
		}
	}

	return 0;

release_dvfs_info_list:
	mtk_cci_release(mtk_cci);
	mtk_cci = NULL;
	list_for_each_entry_safe(info, tmp, &dvfs_info_list, list_head) {
		mtk_cpu_dvfs_info_release(info);
		list_del(&info->list_head);
	}

	return ret;
}

static struct platform_driver mtk_cpufreq_platdrv = {
	.driver = {
		.name	= "mtk-cpufreq",
	},
	.probe		= mtk_cpufreq_probe,
};

/* List of machines supported by this driver */
static const struct of_device_id mtk_cpufreq_machines[] __initconst = {
	{ .compatible = "mediatek,mt2701", },
	{ .compatible = "mediatek,mt2712", },
	{ .compatible = "mediatek,mt6771", },
	{ .compatible = "mediatek,mt7622", },
	{ .compatible = "mediatek,mt7623", },
	{ .compatible = "mediatek,mt817x", },
	{ .compatible = "mediatek,mt8173", },
	{ .compatible = "mediatek,mt8176", },
	{ .compatible = "mediatek,mt8183", },
	{ .compatible = "mediatek,mt8516", },

	{ }
};
MODULE_DEVICE_TABLE(of, mtk_cpufreq_machines);

static int __init mtk_cpufreq_driver_init(void)
{
	struct device_node *np;
	const struct of_device_id *match;
	int err;

	np = of_find_node_by_path("/");
	if (!np)
		return -ENODEV;

	match = of_match_node(mtk_cpufreq_machines, np);
	of_node_put(np);
	if (!match) {
		pr_debug("Machine is not compatible with mtk-cpufreq\n");
		return -ENODEV;
	}

	err = platform_driver_register(&mtk_cpufreq_platdrv);
	if (err)
		return err;

	/*
	 * Since there's no place to hold device registration code and no
	 * device tree based way to match cpufreq driver yet, both the driver
	 * and the device registration codes are put here to handle defer
	 * probing.
	 */
	cpufreq_pdev = platform_device_register_simple("mtk-cpufreq", -1, NULL, 0);
	if (IS_ERR(cpufreq_pdev)) {
		pr_err("failed to register mtk-cpufreq platform device\n");
		platform_driver_unregister(&mtk_cpufreq_platdrv);
		return PTR_ERR(cpufreq_pdev);
	}

	return 0;
}
module_init(mtk_cpufreq_driver_init)

static void __exit mtk_cpufreq_driver_exit(void)
{
	platform_device_unregister(cpufreq_pdev);
	platform_driver_unregister(&mtk_cpufreq_platdrv);
}
module_exit(mtk_cpufreq_driver_exit)

MODULE_DESCRIPTION("MediaTek CPUFreq driver");
MODULE_AUTHOR("Pi-Cheng Chen <pi-cheng.chen@linaro.org>");
MODULE_LICENSE("GPL v2");
