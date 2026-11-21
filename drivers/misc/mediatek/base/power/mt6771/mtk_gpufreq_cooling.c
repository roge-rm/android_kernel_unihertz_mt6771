// SPDX-License-Identifier: GPL-2.0
/*
 * Thermal cooling device for the MT6771 GPU, on the gpufreq DT node.
 *
 * States are gpufreq OPP indexes: 0 is no limit, a higher state caps the GPU at that OPP through
 * mt_gpufreq_thermal_protect(), the hook stock's ATM uses, given the state's table power. The
 * power callbacks make it a power actor for power_allocator, which budgets it with the CPUs as
 * stock's ATM does.
 *
 * The table power is gpufreq's: dynamic power scaled from 1285 mW at 900 MHz and 900 mV, at a
 * fixed 40 C. Leakage is 0 here (no static power table on 5.10, mtk_gpufreq_stubs.c).
 *
 * All callbacks run from the thermal workqueue or sysfs, in process context. set_cur_state
 * takes gpufreq's mutexes through mt_gpufreq_thermal_protect(); the others only read the table
 * (written in place with the same values under mt_gpufreq_lock), the current OPP index and the
 * GED or Mali loading, each a single word.
 */
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/thermal.h>

#include "mtk_gpufreq.h"
#include "mtk_gpu_utility.h"

/* Stock's ATM GPU floor (thermal.conf clatm_setting min_gpu_pwr) */
#define GPU_COOLING_FLOOR_MW	250

static DEFINE_MUTEX(gpu_cooling_lock);
static unsigned long gpu_cooling_state;
/* Loading (1-100) behind the last requested power, to undo the scaling in power2state. */
static unsigned int gpu_cooling_last_load = 100;

static unsigned long gpu_cooling_max_state(void)
{
	unsigned int num = mt_gpufreq_get_dvfs_table_num();

	return num ? num - 1 : 0;
}

/* The last OPP whose table power is at or above the floor (the table goes down with the index). */
static unsigned long gpu_cooling_floor_state(void)
{
	unsigned long max = gpu_cooling_max_state();
	unsigned long s;

	for (s = max; s > 0; s--)
		if (mt_gpufreq_get_power_by_idx(s) >= GPU_COOLING_FLOOR_MW)
			return s;
	return 0;
}

static int gpu_cooling_get_max_state(struct thermal_cooling_device *cdev,
				     unsigned long *state)
{
	*state = gpu_cooling_max_state();
	return 0;
}

static int gpu_cooling_get_cur_state(struct thermal_cooling_device *cdev,
				     unsigned long *state)
{
	mutex_lock(&gpu_cooling_lock);
	*state = gpu_cooling_state;
	mutex_unlock(&gpu_cooling_lock);
	return 0;
}

static int gpu_cooling_set_cur_state(struct thermal_cooling_device *cdev,
				     unsigned long state)
{
	if (state > gpu_cooling_max_state())
		return -EINVAL;

	mutex_lock(&gpu_cooling_lock);
	/*
	 * Always passed on: gpufreq skips an unchanged limit itself, and a repeat retries a cap it
	 * ignored or a procfs write changed. It caps at the first OPP whose power is at or under the limit.
	 */
	mt_gpufreq_thermal_protect(state ? mt_gpufreq_get_power_by_idx(state) : 0);
	gpu_cooling_state = state;
	mutex_unlock(&gpu_cooling_lock);
	return 0;
}

/* Table power at the current OPP times the GPU loading, 0 with the GPU clock off. */
static int gpu_cooling_get_requested_power(struct thermal_cooling_device *cdev,
					   u32 *power)
{
	unsigned int load = 100;
	unsigned int idx;

	if (!mt_gpufreq_clock_is_on()) {
		*power = 0;
		WRITE_ONCE(gpu_cooling_last_load, 100);
		return 0;
	}
	/* No loading from GED or Mali yet: count the GPU as fully loaded. */
	if (!mtk_get_gpu_loading(&load) || load > 100)
		load = 100;
	/* A load of 0 is kept as 100, so power2state then compares the unscaled table power. */
	if (!load)
		load = 100;
	WRITE_ONCE(gpu_cooling_last_load, load);
	idx = mt_gpufreq_get_cur_freq_index();
	*power = mt_gpufreq_get_power_by_idx(idx) * load / 100;
	return 0;
}

static int gpu_cooling_state2power(struct thermal_cooling_device *cdev,
				   unsigned long state, u32 *power)
{
	if (state > gpu_cooling_max_state())
		return -EINVAL;
	*power = mt_gpufreq_get_power_by_idx(state);
	return 0;
}

/* The highest OPP within the power (scaled back up by the last loading), never past the floor. */
static int gpu_cooling_power2state(struct thermal_cooling_device *cdev,
				   u32 power, unsigned long *state)
{
	unsigned long floor = gpu_cooling_floor_state();
	unsigned long s;
	u64 full = div_u64((u64)power * 100, READ_ONCE(gpu_cooling_last_load));

	for (s = 0; s < floor; s++)
		if (mt_gpufreq_get_power_by_idx(s) <= full)
			break;
	*state = s;
	return 0;
}

static const struct thermal_cooling_device_ops gpu_cooling_ops = {
	.get_max_state = gpu_cooling_get_max_state,
	.get_cur_state = gpu_cooling_get_cur_state,
	.set_cur_state = gpu_cooling_set_cur_state,
	.get_requested_power = gpu_cooling_get_requested_power,
	.state2power = gpu_cooling_state2power,
	.power2state = gpu_cooling_power2state,
};

/*
 * Called once from the end of a successful gpufreq probe, with the OPP and power tables set up.
 * A failure leaves the GPU out of thermal control and gpufreq running.
 */
void mt_gpufreq_cooling_register(struct device_node *np)
{
	struct thermal_cooling_device *cdev;
	unsigned long floor;

	if (!np || !of_find_property(np, "#cooling-cells", NULL)) {
		pr_info("[GPU/DVFS][INFO]@%s: no #cooling-cells on the gpufreq node, no GPU cooling\n",
			__func__);
		return;
	}
	if (!mt_gpufreq_get_dvfs_table_num() || !mt_gpufreq_get_power_by_idx(0)) {
		pr_err("[GPU/DVFS][ERROR]@%s: no power table, no GPU cooling\n", __func__);
		return;
	}

	cdev = thermal_of_cooling_device_register(np, "mtk-gpufreq", NULL, &gpu_cooling_ops);
	if (IS_ERR(cdev)) {
		pr_err("[GPU/DVFS][ERROR]@%s: cooling device not registered: %ld\n",
		       __func__, PTR_ERR(cdev));
		return;
	}

	floor = gpu_cooling_floor_state();
	pr_info("[GPU/DVFS][INFO]@%s: %s, states 0-%lu, %u to %u mW, floor state %lu (%u KHz, %u mW)\n",
		__func__, dev_name(&cdev->device), gpu_cooling_max_state(),
		mt_gpufreq_get_power_by_idx(gpu_cooling_max_state()), mt_gpufreq_get_power_by_idx(0),
		floor, mt_gpufreq_get_freq_by_idx(floor), mt_gpufreq_get_power_by_idx(floor));
}
