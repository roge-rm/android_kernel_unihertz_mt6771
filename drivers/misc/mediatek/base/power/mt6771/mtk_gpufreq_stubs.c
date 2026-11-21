// SPDX-License-Identifier: GPL-2.0
/*
 * Stubs for what MT6771 gpufreq and GED call in drivers this port leaves
 * out: PBM (pbm_v3), the static power table (leakage_table_v2), the MT6358
 * PMIC battery throttling (pmic_throttling_dlpt) and the CPU frequency API
 * (cpufreq_v1). Each does nothing, so gpufreq never gets a PBM, low battery
 * or battery percent limit, its power table has no leakage part,
 * and the GED KPI log shows 0 for the CPU frequencies, as T's own branch
 * without MTK_CPUFREQ does. Battery OC comes from mt6358-battery-oc when
 * CONFIG_MT6358_BATTERY_OC is set; without it the stub below leaves
 * gpufreq's boot hold at the battery OC limit in place.
 */
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>
#include "mtk_pbm.h"
#include "mtk_static_power.h"
#include "upmu_sw.h"
#include "mtk_cpufreq_api.h"

void kicker_pbm_by_gpu(bool status, unsigned int loading, int voltage)
{
}

int mt_spower_init(void)
{
	return -ENODEV;
}

int mt_spower_get_leakage(int dev, unsigned int voltage, int degree)
{
	return -ENODEV;
}

void register_low_battery_notify(void (*low_battery_callback)(LOW_BATTERY_LEVEL),
				 LOW_BATTERY_PRIO prio_val)
{
}

void register_battery_percent_notify(void (*battery_percent_callback)(BATTERY_PERCENT_LEVEL),
				     BATTERY_PERCENT_PRIO prio_val)
{
}

#if !IS_ENABLED(CONFIG_MT6358_BATTERY_OC)
void register_battery_oc_notify(void (*battery_oc_callback)(BATTERY_OC_LEVEL),
				BATTERY_OC_PRIO prio_val)
{
}
#endif

unsigned int mt_cpufreq_get_freq_by_idx(unsigned int cluster_id, int idx)
{
	return 0;
}

unsigned int mt_cpufreq_get_cur_freq(unsigned int cluster_id)
{
	return 0;
}

unsigned int mt_cpufreq_get_cur_freq_idx(unsigned int cluster_id)
{
	return 0;
}
