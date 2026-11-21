// SPDX-License-Identifier: GPL-2.0
/*
 * O2Micro OZ8806 fuel gauge on the Titan Pocket (i2c6 0x2f, "mediatek,coulomb"),
 * with the calls and timing of the stock 4.14 kernel's oz8806 driver. The
 * level maths is in oz8806_bmulib.c.
 *
 * Without CONFIG_OZ8806_WRITE the driver writes only the CAR (with
 * CONFIG_OZ8806_WRITE_CAR) and OP_CTRL (the wake at
 * probe and on each poll, and the sleep at shutdown) and never the PMIC RTC
 * spare register; places where that differs from stock say "Read-only:".
 *
 * Locking: one mutex guards the bmulib state, as stock. The battery work runs
 * on system_freezable_wq every 2 s until bmulib init is done, then stops; after
 * that oz8806_update(), called by the battery supply every 10 s, drives the
 * polls. Both works are freezable so no I2C runs while the bus is suspended.
 * oz_api_lock guards oz_drv for the calls the battery supply makes, so remove
 * can clear it before the state is freed.
 *
 * Port additions, not in stock: probe fails if the chip doesn't answer, bmulib
 * init waits for a valid reading (oz8806_check_chip), and the report is only
 * given while the polls are valid (data_ok).
 */
#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/power/oz8806.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/sched/clock.h>
#include <linux/suspend.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include "oz8806_bmulib.h"

/* MT6358 RTC_AL_MTH bits 15:8 hold the stock SOC + 1 (hal_rtc RTC_FGSOC) */
#define MT6358_RTC_BBPU		0x0588
#define MT6358_RTC_AL_MTH	0x05aa
#define MT6358_RTC_WRTGR	0x05c2
#define RTC_FGSOC_MASK		0xff00
#define RTC_FGSOC_SHIFT		8
#define RTC_BBPU_CBUSY		BIT(6)

#define OZ8806_FULL_MS		300000	/* full after 5 min at the end point */
#define OZ8806_STUCK99_MS	600000	/* or 10 min stuck at 99 % */
#define OZ8806_RAMP_MS		30000	/* +1 % every 30 s while full */
#define OZ8806_PMIC_TRIES	5	/* 2 s waits for the PMIC before init goes on without it */

struct oz8806_drv {
	struct i2c_client *client;
	struct device *dev;
	struct oz8806_bmu bmu;
	struct mutex lock;
	struct delayed_work work;
	struct wakeup_source *ws;
	struct notifier_block pm_nb;
	struct regmap *pmic;

	unsigned long power_on_base;
	unsigned int interval;
	int soc, volt, curr, temp, remcap, fcc, dis_th, chg_full;
	bool bmulib_ready;	/* bmulib_init done, callbacks registered */
	int init_done;		/* GG->init_ok copy */
	int bmu_init_done;
	int suspended;
	int adapter;
	int rtc_cache;
	int save_capacity;
	int charger_type;	/* -1 until a charger driver reports */
	bool chg_done;
	bool stopping;		/* remove or shutdown: no more work */
	int pullup;		/* NTC pull-up, -1 until read */
	int pmic_tries;
	int wake_logged;	/* last wake event logged */
	int chip_state;		/* last logged pre-init state: 0, 1 asleep, 2 bad reading */

	/* full detection in battery_func */
	unsigned long last, t_full_start, t_ramp, t99;
	unsigned int t_full;
	int ramp_soc, f99, full;

	struct oz8806_report rep;
};

static struct oz8806_drv *oz_drv;
/* POWER_SUPPLY_STATUS_* from the charger events, 0 until one comes. Kept
 * outside oz_drv so an event before the gauge probes is not lost. Under
 * oz_api_lock. */
static int oz_bat_status;
static void (*oz_changed_cb)(void *arg);
static void *oz_changed_arg;
static DEFINE_MUTEX(oz_api_lock);
static int drv_ready;

/* ---- hooks for oz8806_bmulib.c ---- */

static inline struct oz8806_drv *to_drv(struct oz8806_bmu *b)
{
	return b->hw;
}

int oz8806_hw_read_byte(struct oz8806_bmu *b, u8 reg)
{
	return i2c_smbus_read_byte_data(to_drv(b)->client, reg);
}

int oz8806_hw_read_word(struct oz8806_bmu *b, u8 reg)
{
	return i2c_smbus_read_word_data(to_drv(b)->client, reg);
}

int oz8806_hw_write_byte(struct oz8806_bmu *b, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(to_drv(b)->client, reg, val);
}

#if defined(CONFIG_OZ8806_WRITE) || defined(CONFIG_OZ8806_WRITE_CAR)
int oz8806_hw_write_word(struct oz8806_bmu *b, u8 reg, u16 val)
{
	return i2c_smbus_write_word_data(to_drv(b)->client, reg, val);
}
#endif

/* stock's bus probe moves client->addr; called under the lock */
void oz8806_hw_set_addr(struct oz8806_bmu *b, u8 addr)
{
	to_drv(b)->client->addr = addr;
}

void oz8806_hw_msleep(struct oz8806_bmu *b, unsigned int ms)
{
	msleep(ms);
}

/* stock oz8806_get_boot_up_time(): sched_clock in ms */
unsigned int oz8806_hw_boot_ms(struct oz8806_bmu *b)
{
	return div_u64(local_clock(), NSEC_PER_MSEC);
}

unsigned int oz8806_hw_power_on_ms(struct oz8806_bmu *b)
{
	return jiffies_to_msecs(jiffies - to_drv(b)->power_on_base);
}

static void oz8806_set_power_on_base(struct oz8806_drv *d)
{
	unsigned int boot = div_u64(local_clock(), NSEC_PER_MSEC);

	d->power_on_base = jiffies;
	if (boot < 2000)
		d->power_on_base += msecs_to_jiffies(2000 - boot);
}

void oz8806_hw_reset_power_on_base(struct oz8806_bmu *b)
{
	oz8806_set_power_on_base(to_drv(b));
}

long long oz8806_hw_now_sec(struct oz8806_bmu *b)
{
	return ktime_get_real_seconds();
}

long long oz8806_hw_boottime_sec(struct oz8806_bmu *b)
{
	return ktime_get_boottime_seconds();
}

int oz8806_hw_adapter(struct oz8806_bmu *b)
{
	return to_drv(b)->adapter;
}

static int oz8806_ext_soc(struct oz8806_drv *d);

int oz8806_hw_soc_from_ext(struct oz8806_bmu *b)
{
	return oz8806_ext_soc(to_drv(b));
}

int oz8806_hw_save_capacity(struct oz8806_bmu *b)
{
	return to_drv(b)->save_capacity;
}

/* ---- MT6358 RTC spare register ---- */

static struct regmap *oz8806_pmic(struct oz8806_drv *d)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct mt6397_chip *chip;

	if (d->pmic)
		return d->pmic;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	chip = platform_get_drvdata(pdev);
	if (chip && chip->regmap)
		d->pmic = chip->regmap;
	put_device(&pdev->dev);
	return d->pmic;
}

/* stock get_rtc_spare_fg_value(); -1 if the PMIC isn't there yet */
static int get_rtc_spare_fg_value(struct oz8806_drv *d)
{
	struct regmap *map = oz8806_pmic(d);
	unsigned int v;

	if (!map || regmap_read(map, MT6358_RTC_AL_MTH, &v))
		return -1;
	return (v & RTC_FGSOC_MASK) >> RTC_FGSOC_SHIFT;
}

/* the RTC value, cached once it has been read; 0 while it can't be read */
static int oz8806_rtc_soc(struct oz8806_drv *d)
{
	int v;

	if (d->rtc_cache != -1)
		return d->rtc_cache;
	v = get_rtc_spare_fg_value(d);
	if (v < 0) {
		dev_warn_once(d->dev, "RTC spare not readable yet\n");
		return 0;
	}
	d->rtc_cache = v;
	return v;
}

/* W10, stock set_rtc_spare_fg_value() */
static void set_rtc_spare_fg_value(struct oz8806_drv *d, int val)
{
#ifdef CONFIG_OZ8806_WRITE
	struct regmap *map = oz8806_pmic(d);
	unsigned int bbpu;

	if (!map)
		return;
	regmap_update_bits(map, MT6358_RTC_AL_MTH, RTC_FGSOC_MASK,
			   (val << RTC_FGSOC_SHIFT) & RTC_FGSOC_MASK);
	regmap_write(map, MT6358_RTC_WRTGR, 1);
	regmap_read_poll_timeout(map, MT6358_RTC_BBPU, bbpu, !(bbpu & RTC_BBPU_CBUSY),
				 100, 500000);
#else
	/* Read-only: the RTC spare keeps what stock last wrote */
#endif
}

/* ---- NTC pull-up from SoC GPIO 5, stock agold_gpio_get_in(5) ---- */

static int oz8806_match_pio(struct gpio_chip *gc, void *data)
{
	return gc->of_node && of_device_is_compatible(gc->of_node, "mediatek,mt6771-pinctrl");
}

/* Reads the pin's DIN through the pinctrl gpio_chip without requesting it, so
 * the pin mode is left alone, as stock. */
static int oz8806_ntc_pullup(struct oz8806_drv *d)
{
	struct gpio_chip *gc = gpiochip_find(NULL, oz8806_match_pio);
	int v = gc && gc->get ? gc->get(gc, OZ8806_HWVER_GPIO) : -ENODEV;

	if (v < 0) {
		/* 24 kOhm: hw_ver_id 1, what the stock firmware reads on a retail phone */
		dev_warn(d->dev, "hw_ver gpio not readable (%d), NTC pull-up %d\n",
			 v, OZ8806_HWVER1_PULLUP);
		return OZ8806_HWVER1_PULLUP;
	}
	dev_info(d->dev, "hw_ver_id=%d\n", v);
	return v == 1 ? OZ8806_HWVER1_PULLUP : OZ8806_HWVER0_PULLUP;
}

/* log what oz8806_wake_if_asleep did; skips a repeat of the same failure */
static void oz8806_log_wake(struct oz8806_drv *d)
{
	int ev = d->bmu.wake_event;

	d->bmu.wake_event = OZ_WAKE_NONE;
	if (ev == OZ_WAKE_NONE || (ev != OZ_WAKE_WROTE && ev == d->wake_logged))
		return;
	d->wake_logged = ev;
	switch (ev) {
	case OZ_WAKE_WROTE:
		dev_info(d->dev, "chip asleep, wrote OP_CTRL 0x20, readings wait 2250 ms\n");
		break;
	case OZ_WAKE_NOT_TAKEN:
		dev_warn(d->dev, "chip asleep, OP_CTRL 0x20 write did not take\n");
		break;
	case OZ_WAKE_PEC:
		dev_warn(d->dev, "chip asleep, PEC error (reg 8 bit 2), OP_CTRL not written\n");
		break;
	case OZ_WAKE_REG8_FAIL:
		dev_warn(d->dev, "chip asleep, reg 8 unreadable, OP_CTRL not written\n");
		break;
	}
}

/* ---- charger state ---- */

/* stock: mt_get_charger_type() 0 -> 0, 1 or 2 (SDP, CDP) -> 1 USB, others -> 4 AC */
static int oz8806_adapter_now(struct oz8806_drv *d)
{
	int t = d->charger_type;

	/* none (0) until a charger driver calls oz8806_set_charger_info() */
	return t <= 0 ? 0 : (t == 1 || t == 2) ? 1 : 4;
}

void oz8806_set_charger_info(int charger_type, bool charge_done)
{
	struct oz8806_drv *d;

	mutex_lock(&oz_api_lock);
	d = oz_drv;
	if (d) {
		mutex_lock(&d->lock);
		d->charger_type = charger_type;
		d->chg_done = charge_done;
		mutex_unlock(&d->lock);
	}
	mutex_unlock(&oz_api_lock);
}

/* ---- stock oz8806_battery_func ---- */

static void oz8806_copy_state(struct oz8806_drv *d)
{
	d->soc = d->bmu.bi.soc;
	d->volt = d->bmu.bi.volt;
	d->curr = d->bmu.bi.curr;
	d->remcap = d->bmu.bi.rc;
	d->chg_full = d->bmu.gg.chg_end;
	d->init_done = d->bmu.gg.init_ok;
	d->temp = d->bmu.bi.temp;
}

static void oz8806_clear_full(struct oz8806_drv *d)
{
	d->t_full = 0;
	d->t_full_start = 0;
	d->t_ramp = 0;
	d->t99 = 0;
	d->f99 = 0;
	d->full = 0;
}

/* the report follows bmu.data_ok; log when it changes */
static void oz8806_track_valid(struct oz8806_drv *d, int valid)
{
	struct oz8806_bmu *b = &d->bmu;

	if (!oz8806_track_valid_core(b, valid))
		return;
	if (b->data_ok)
		dev_info(d->dev, "readings valid, volt %d curr %d\n", b->bi.volt, b->bi.curr);
	else
		dev_warn(d->dev, "%d invalid polls (volt %d, %s, %d i2c errors), no battery data\n",
			 b->bad_polls, b->bi.volt, b->asleep ? "asleep" : "awake", b->bi.i2c_errors);
}

static void oz8806_battery_func(struct oz8806_drv *d)
{
	struct oz8806_bmu *b = &d->bmu;
	struct oz8806_batt_info *bi = &b->bi;
	struct oz8806_gas_gauge *gg = &b->gg;
	int prev_soc, I, th = 0, valid = -1;

	d->last = jiffies;
	mutex_lock(&d->lock);
	if (d->stopping) {		/* shut down: the sleep write must stay the last */
		mutex_unlock(&d->lock);
		return;
	}
	d->adapter = oz8806_adapter_now(d);
	prev_soc = d->soc;
	if (d->bmulib_ready && !d->suspended) {
		valid = oz8806_poll(b);
		oz8806_log_wake(d);
	}
	oz8806_track_valid(d, valid);
	if (!d->bmulib_ready)
		goto out;
	oz8806_copy_state(d);

	if (d->adapter == 0) {
		oz8806_clear_full(d);
		goto out;
	}
	if (!d->chg_done) {
		I = bi->curr;
		if (I < -10 || I > b->cfg.end_current_ma) {
			oz8806_clear_full(d);
			goto out;
		}
		th = d->adapter == 1 ? 100 : d->adapter == 4 ? b->cfg.end_current_ma : 0;
		if (I < th && bi->volt >= b->cfg.cv_mv - 50 && !gg->chg_end) {
			if (!d->t_full_start)
				d->t_full_start = jiffies;
			d->t_full = jiffies_to_msecs(jiffies - d->t_full_start);
			d->full = d->t_full >= OZ8806_FULL_MS;
		} else {
			d->t_full = 0;
			d->t_full_start = 0;
			d->full = 0;
		}
	} else {
		d->full = 1;
		th = 0;
	}

	/* stuck at 99 % */
	if (bi->soc == 99 && !d->f99) {
		if (bi->curr > th) {
			d->f99 = 1;
			d->t99 = jiffies;
		}
	} else if (bi->soc == 99) {
		if (bi->curr < th) {
			d->f99 = 0;
		} else if (bi->curr > th &&
			   jiffies_to_msecs(jiffies - d->t99) > OZ8806_STUCK99_MS) {
			d->full = 1;
			d->f99 = 0;
		}
	} else {
		d->f99 = 0;
	}

	if (d->full) {
		if (!gg->chg_end) {
			if (bi->soc < 100) {
				if (bi->soc <= prev_soc) {
					if (!d->t_ramp || d->ramp_soc != bi->soc) {
						d->t_ramp = jiffies;
						d->ramp_soc = bi->soc;
					}
					/* stock compares (ms >> 4) >= 0x753 */
					if ((jiffies_to_msecs(jiffies - d->t_ramp) >> 4) >= 0x753) {
						d->t_ramp = 0;
						bi->soc++;
						bi->rc = d->fcc * bi->soc / 100 + 1;
					}
					if (bi->soc > 100) {
						bi->soc = 100;
						bi->rc = d->fcc + 1;
					}
				}
				d->ramp_soc = bi->soc;
				goto out;			/* keeps full set */
			}
			gg->chg_end = 1;
			charge_end_process(b);			/* W7 */
		}
		d->full = 0;
	}
	d->t_ramp = 0;
out:
	/* port: only on a fresh valid voltage that isn't stuck */
	if (d->adapter == 0 && d->bmulib_ready && valid == 1 && !b->volt_frozen &&
	    bi->volt < oz8806_shutdown_mv(b) && bi->soc > 0) {
		bi->soc--;				/* once per update below 3400 mV */
		bi->rc = d->fcc * bi->soc / 100;
	}
	mutex_unlock(&d->lock);
	if (d->adapter == 4)
		__pm_stay_awake(d->ws);
	else
		__pm_relax(d->ws);
}

/* stock bmulib_init and oz8806_register_bmu_callback: setup, one poll. Port:
 * waits (returns false) until the chip gives a valid reading, so the init
 * SOC isn't worked out from a chip that reads 0 or is asleep. */
static bool oz8806_bmulib_init(struct oz8806_drv *d)
{
	int ok, state;

	if (d->pullup < 0)
		d->pullup = oz8806_ntc_pullup(d);
	if (!oz8806_pmic(d) && d->pmic_tries < OZ8806_PMIC_TRIES) {
		d->pmic_tries++;			/* for the RTC SOC at init */
		return false;
	}
	mutex_lock(&d->lock);
	oz8806_bmu_setup(&d->bmu, d->pullup);
	ok = oz8806_check_chip(&d->bmu);
	oz8806_log_wake(d);
	state = ok ? 0 : d->bmu.asleep ? 1 : 2;
	if (state != d->chip_state) {
		if (state == 1)
			dev_warn(d->dev, "chip asleep (OP_CTRL bit 6), no battery data\n");
		else if (state == 2)
			dev_warn(d->dev, "no valid reading (volt %d, %d i2c errors), no battery data\n",
				 d->bmu.bi.volt, d->bmu.bi.i2c_errors);
		d->chip_state = state;
	}
	if (!ok) {
		mutex_unlock(&d->lock);
		return false;
	}
	bmulib_init_chip(&d->bmu);
	d->fcc = d->bmu.gg.fcc;
	d->dis_th = d->bmu.gg.dis_th;
	d->bmulib_ready = true;
	oz8806_track_valid(d, oz8806_poll(&d->bmu));
	mutex_unlock(&d->lock);
	dev_info(d->dev, "bmulib init, parameters %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		 d->bmu.cfg.rsense, d->bmu.cfg.ntc_pullup, d->bmu.cfg.ntc_ref_mv,
		 d->bmu.cfg.car_lsb, d->bmu.cfg.curr_lsb, d->bmu.cfg.volt_lsb,
		 d->bmu.cfg.design_mah, d->bmu.cfg.cv_mv, d->bmu.cfg.end_current_ma,
		 d->bmu.cfg.end_volt_mv, d->bmu.cfg.board_offset_ma, d->bmu.cfg.debug);
	return true;
}

static void oz8806_requeue(struct oz8806_drv *d, unsigned int ms)
{
	mutex_lock(&d->lock);
	if (!d->stopping)
		queue_delayed_work(system_freezable_wq, &d->work, msecs_to_jiffies(ms));
	mutex_unlock(&d->lock);
}

static void oz8806_battery_work(struct work_struct *work)
{
	struct oz8806_drv *d = container_of(to_delayed_work(work), struct oz8806_drv, work);

	if (d->stopping)
		return;
	if (!d->bmulib_ready && !oz8806_bmulib_init(d)) {
		d->interval = 2000;
		oz8806_requeue(d, 2000);
		return;
	}
	if (d->init_done)
		return;
	oz8806_battery_func(d);
	/* Stock spins here until mtk_battery's capacity matches; our supply
	 * reads oz8806_get_soc() directly, so it matches at once. */
	if (!d->bmu_init_done && d->init_done) {
		d->bmu_init_done = 1;
		dev_info(d->dev, "init done, soc %d rc %d car %d volt %d curr %d temp %d\n",
			 d->bmu.bi.soc, d->bmu.bi.rc, d->bmu.bi.car, d->bmu.bi.volt,
			 d->bmu.bi.curr, d->bmu.bi.temp);
	}
	if (!d->init_done) {
		d->interval = 2000;
		oz8806_requeue(d, 2000);
	} else {
		d->interval = 10000;
	}
}

/* ---- getters ---- */

int oz8806_get_soc(void)
{
	struct oz8806_drv *d = oz_drv;
	int r;

	if (!d)
		return -1;
	if (d->suspended == 1)
		return d->bmulib_ready ? d->bmu.bi.soc : -1;
	mutex_lock(&d->lock);
	if (d->init_done) {
		r = oz8806_report_soc(&d->bmu);
	} else {
		r = oz8806_rtc_soc(d);
		if (r <= 0) {
			if (d->rtc_cache != -1)
				d->rtc_cache = 20;	/* 20, not 19, as stock */
			r = 20;
		} else {
			r--;
		}
	}
	mutex_unlock(&d->lock);
	return r;
}

/* called under the lock from the init poll */
static int oz8806_ext_soc(struct oz8806_drv *d)
{
	return oz8806_rtc_soc(d) - 1;
}

int oz8806_get_soc_from_ext(void)
{
	struct oz8806_drv *d = oz_drv;

	return d ? oz8806_ext_soc(d) : -1;
}

static int oz8806_live_current(struct oz8806_drv *d)
{
	int r;

	if (!d || !d->init_done)
		return -1;
	mutex_lock(&d->lock);
	r = afe_read_current(&d->bmu, &d->bmu.bi.curr) < 0 ? -1 : d->bmu.bi.curr;
	mutex_unlock(&d->lock);
	return r;
}

int oz8806_get_battry_current(void)
{
	int r;

	mutex_lock(&oz_api_lock);
	r = oz8806_live_current(oz_drv);
	mutex_unlock(&oz_api_lock);
	return r;
}

int oz8806_get_battery_voltage(void)
{
	struct oz8806_drv *d = oz_drv;
	int r;

	if (!d || !d->init_done)
		return -1;
	mutex_lock(&d->lock);
	r = afe_read_cell_volt(&d->bmu, &d->bmu.bi.volt) < 0 ? -1 : d->bmu.bi.volt;
	mutex_unlock(&d->lock);
	return r;
}

int oz8806_get_battery_temp(void)
{
	struct oz8806_drv *d = oz_drv;

	if (!d || !d->init_done)
		return -1;
	return d->bmu.bi.temp;
}

/*
 * For the charger, a deliberate change from stock (which used
 * oz8806_get_battery_temp): -1, the value that stops charging, unless the
 * polls are valid and the last NTC reading was good and inside the table.
 */
int oz8806_get_charger_temp(void)
{
	struct oz8806_drv *d = oz_drv;
	int r = -1;

	if (!d || !d->init_done)
		return -1;
	mutex_lock(&d->lock);
	if (d->bmu.data_ok && d->bmu.temp_ok && d->bmu.temp_valid)
		r = d->bmu.bi.temp;
	mutex_unlock(&d->lock);
	return r;
}

/*
 * The same reading as oz8806_get_charger_temp() in *temp_c (C), with
 * -ENODATA instead of -1 when there is none, so -1 C stays a reading.
 */
int oz8806_get_valid_temp(int *temp_c)
{
	struct oz8806_drv *d = oz_drv;
	int ret = -ENODATA;

	if (!d || !d->init_done)
		return -ENODATA;
	mutex_lock(&d->lock);
	if (d->bmu.data_ok && d->bmu.temp_ok && d->bmu.temp_valid) {
		*temp_c = d->bmu.bi.temp;
		ret = 0;
	}
	mutex_unlock(&d->lock);
	return ret;
}

/*
 * One CSV line for the OCV run logger: boot time (s), voltage (mV), current
 * (mA, charging positive), counted charge (uAh), the chip's OCV register
 * (mV), temperature (C), level (%), remaining capacity (mAh), charger type
 * (0 none, 1 USB, 4 AC), seconds at rest. Voltage, current and charge are
 * read now.
 */
int oz8806_log_line(char *buf, size_t size)
{
	struct oz8806_sample s;
	struct oz8806_drv *d;
	int ret = -ENODEV;

	mutex_lock(&oz_api_lock);
	d = oz_drv;
	if (d) {
		mutex_lock(&d->lock);
		ret = -ENODATA;
		if (d->init_done && d->bmu.data_ok && !d->stopping && !d->suspended &&
		    !oz8806_sample(&d->bmu, &s))
			ret = scnprintf(buf, size, "%lld,%d,%d,%lld,%d,%d,%d,%d,%d,%d\n",
					s.t_s, s.volt, s.curr, s.cc_uah, s.chip_ocv, s.temp,
					oz8806_report_soc(&d->bmu), s.rc, d->adapter, s.rest_s);
		mutex_unlock(&d->lock);
	}
	mutex_unlock(&oz_api_lock);
	return ret;
}

int oz8806_get_remaincap(void)
{
	struct oz8806_drv *d = oz_drv;

	return d && d->bmulib_ready ? d->bmu.bi.rc : -1;
}

/* no caller found in stock */
int oz8806_vbus_voltage(void)
{
	struct oz8806_drv *d = oz_drv;
	int mv = 0;

	if (!d || !d->bmulib_ready)
		return -1;
	mutex_lock(&d->lock);
	oz8806_temp_read(&d->bmu, &mv);
	mutex_unlock(&d->lock);
	return mv * 73100 / 5100;
}

int oz8806_get_simulated_temp(void)
{
	return 25;
}

int oz8806_get_init_status(void)
{
	return oz_drv ? oz_drv->init_done : 0;
}

int oz8806_gauge_int_done(void)
{
	return drv_ready;
}

int oz8806_get_adapter_status(void)
{
	return oz_drv ? oz_drv->adapter : 0;
}

int oz8806_get_save_capacity(void)
{
	int r;

	mutex_lock(&oz_api_lock);
	r = oz_drv ? oz_drv->save_capacity : -2;
	mutex_unlock(&oz_api_lock);
	return r;
}

void oz8806_set_save_capacity(int mah)
{
	struct oz8806_drv *d;

	mutex_lock(&oz_api_lock);
	d = oz_drv;
	if (d) {
		mutex_lock(&d->lock);
		d->save_capacity = mah;
		if (!d->init_done && !d->stopping)
			mod_delayed_work(system_freezable_wq, &d->work, 0);
		mutex_unlock(&d->lock);
	}
	mutex_unlock(&oz_api_lock);
}

int oz8806_get_bmu_init_done(void)
{
	int r;

	mutex_lock(&oz_api_lock);
	r = oz_drv ? oz_drv->bmu_init_done : 0;
	mutex_unlock(&oz_api_lock);
	return r;
}

struct i2c_client *oz8806_get_client(void)
{
	return oz_drv ? oz_drv->client : NULL;
}

/* Before the first charger event: from the gauge current. A charging
 * status at 100 % is Full, as Android expects when plugged in. */
static int oz8806_status(struct oz8806_drv *d, int curr)
{
	int st = oz_bat_status;

	if (!d->init_done)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	if (!st)
		st = curr > 0 ? POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_DISCHARGING;
	if (st == POWER_SUPPLY_STATUS_CHARGING && d->rep.soc >= 100)
		st = POWER_SUPPLY_STATUS_FULL;
	return st;
}

/* The battery supply registers a function to call when the status changes. */
void oz8806_set_changed_cb(void (*cb)(void *arg), void *arg)
{
	mutex_lock(&oz_api_lock);
	oz_changed_cb = cb;
	oz_changed_arg = arg;
	mutex_unlock(&oz_api_lock);
}

/* Stock battery_callback plus Full at end of charge: the status is what the
 * last charger event set. Called from the charger driver's sleepable
 * context. The reported status updates now and the supply is told. */
void oz8806_set_battery_status(int status)
{
	struct oz8806_drv *d;

	mutex_lock(&oz_api_lock);
	oz_bat_status = status;
	d = oz_drv;
	if (d) {
		mutex_lock(&d->lock);
		d->rep.status = oz8806_status(d, d->rep.curr);
		mutex_unlock(&d->lock);
	}
	if (oz_changed_cb)
		oz_changed_cb(oz_changed_arg);
	mutex_unlock(&oz_api_lock);
}

/* stock oz8806_update(): battery_update_data, the getters, then the RTC spare */
void oz8806_update(void)
{
	struct oz8806_drv *d;
	int soc, temp, volt, curr;

	mutex_lock(&oz_api_lock);
	d = oz_drv;
	if (!d) {
		mutex_unlock(&oz_api_lock);
		return;
	}
	if (d->init_done && drv_ready)
		oz8806_battery_func(d);
	soc = oz8806_get_soc();
	temp = oz8806_get_battery_temp();
	volt = oz8806_get_battery_voltage();
	curr = oz8806_live_current(d);
	if (soc != -1)
		set_rtc_spare_fg_value(d, soc + 1);		/* W10 */

	mutex_lock(&d->lock);
	d->rep.soc = soc;
	if (volt >= 0)
		d->rep.volt = volt;
	if (d->init_done) {
		d->rep.curr = curr;
		d->rep.temp = temp;
	}
	d->rep.rc = d->bmulib_ready ? d->bmu.bi.rc : d->remcap;
	d->rep.fcc = d->fcc;
	d->rep.temp_ok = d->init_done && d->bmu.temp_ok;
	d->rep.status = oz8806_status(d, d->rep.curr);
	mutex_unlock(&d->lock);
	mutex_unlock(&oz_api_lock);
}

/* -ENODATA until init is done on valid readings, and while the polls fail */
int oz8806_get_report(struct oz8806_report *r)
{
	struct oz8806_drv *d;
	int ret = -ENODEV;

	mutex_lock(&oz_api_lock);
	d = oz_drv;
	if (d) {
		mutex_lock(&d->lock);
		ret = d->bmu.data_ok ? 0 : -ENODATA;
		if (!ret)
			*r = d->rep;
		mutex_unlock(&d->lock);
	}
	mutex_unlock(&oz_api_lock);
	return ret;
}

/* ---- suspend notifier ---- */

static int oz8806_suspend_notifier(struct notifier_block *nb, unsigned long event, void *unused)
{
	struct oz8806_drv *d = container_of(nb, struct oz8806_drv, pm_nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		d->adapter = oz8806_adapter_now(d);
		d->suspended = 1;
		break;
	case PM_POST_SUSPEND:
		d->adapter = oz8806_adapter_now(d);
		mutex_lock(&d->lock);
		if (d->adapter == 0 && d->bmulib_ready && !d->stopping)
			bmu_wake_up_chip(&d->bmu);
		oz8806_log_wake(d);
		if (d->bmulib_ready)
			oz8806_copy_state(d);
		mutex_unlock(&d->lock);
		if (d->adapter == 0 && d->curr >= d->dis_th && d->bmulib_ready)
			d->bmu.bi.curr = d->curr = -20;
		d->suspended = 0;
		break;
	}
	return NOTIFY_DONE;
}

/* ---- sysfs on the i2c device ---- */

static const u8 oz8806_dump_regs[] = { 0x00, 0x04, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
	0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19 };

static ssize_t registers_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct oz8806_drv *d = dev_get_drvdata(dev);
	int i, n = 0, v;

	mutex_lock(&d->lock);
	for (i = 0; i < ARRAY_SIZE(oz8806_dump_regs); i++) {
		v = i2c_smbus_read_byte_data(d->client, oz8806_dump_regs[i]);
		n += scnprintf(buf + n, PAGE_SIZE - n, "0x%2x ", v < 0 ? 0 : v);
	}
	mutex_unlock(&d->lock);
	n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	return n;
}

#ifdef CONFIG_OZ8806_WRITE
/* W8: "RRVV" in hex, only regs 0x08, 0x09, 0x14, 0x15, 0x18, 0x19 */
static ssize_t registers_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct oz8806_drv *d = dev_get_drvdata(dev);
	unsigned long v;

	if (kstrtoul(buf, 16, &v) || v > 0xffff)
		return -EINVAL;
	if ((v >> 8) < 32 && (0x3300300UL >> (v >> 8) & 1)) {
		mutex_lock(&d->lock);
		i2c_smbus_write_byte_data(d->client, v >> 8, v & 0xff);
		mutex_unlock(&d->lock);
	}
	return count;
}
static DEVICE_ATTR_RW(registers);
#else
/* Read-only: no store */
static DEVICE_ATTR_RO(registers);
#endif

static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct oz8806_drv *d = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", d->bmu.cfg.debug);
}

static ssize_t debug_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct oz8806_drv *d = dev_get_drvdata(dev);
	int v;

	if (kstrtoint(buf, 10, &v) || (v != 0 && v != 1))
		return -EINVAL;
	d->bmu.cfg.debug = v;
	return count;
}
static DEVICE_ATTR_RW(debug);

static struct attribute *oz8806_attrs[] = {
	&dev_attr_registers.attr,
	&dev_attr_debug.attr,
	NULL,
};
ATTRIBUTE_GROUPS(oz8806);

/* ---- i2c driver ---- */

static int oz8806_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct oz8806_drv *d;
	int ret, k;

	/* port: the chip has to answer before anything registers */
	for (k = 0; k < 3; k++) {
		ret = i2c_smbus_read_byte_data(client, 0x09);	/* OP_CTRL */
		if (ret >= 0)
			break;
		msleep(5);
	}
	if (ret < 0) {
		dev_err(&client->dev, "no answer at 0x%02x (%d)\n", client->addr, ret);
		return -ENODEV;
	}
	dev_info(&client->dev, "OP_CTRL 0x%02x%s\n", ret, ret & 0x40 ? ", asleep" : "");

	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->client = client;
	d->dev = &client->dev;
	d->bmu.hw = d;
	mutex_init(&d->lock);
	i2c_set_clientdata(client, d);

	d->interval = 10000;
	d->soc = 50;
	d->volt = 3999;
	d->curr = -300;
	d->temp = 27;
	d->remcap = 4000;
	d->fcc = oz8806_board_config.design_mah;
	d->dis_th = -10;
	d->chg_full = 0;
	d->rtc_cache = -1;
	d->save_capacity = -2;
	d->charger_type = -1;
	d->pullup = -1;
	d->rep = (struct oz8806_report){ .soc = -1, .volt = d->volt, .curr = d->curr,
		.temp = d->temp, .rc = d->remcap, .fcc = d->fcc,
		.status = POWER_SUPPLY_STATUS_UNKNOWN };
	oz8806_set_power_on_base(d);

	mutex_lock(&d->lock);
	oz8806_wakeup_full_power(&d->bmu);		/* W1 */
	oz8806_log_wake(d);
	mutex_unlock(&d->lock);

	ret = devm_device_add_groups(&client->dev, oz8806_groups);
	if (ret)
		return -EIO;

	d->pm_nb.notifier_call = oz8806_suspend_notifier;
	register_pm_notifier(&d->pm_nb);
	d->ws = wakeup_source_register(&client->dev, "oz8806_wake");
	INIT_DELAYED_WORK(&d->work, oz8806_battery_work);
	mutex_lock(&oz_api_lock);
	oz_drv = d;
	drv_ready = 1;
	mutex_unlock(&oz_api_lock);

	/* stock bmulib_init runs from a later initcall and queues the work */
	queue_delayed_work(system_freezable_wq, &d->work, 0);
	dev_info(&client->dev, "OZ8806 gauge, %s\n",
		 IS_ENABLED(CONFIG_OZ8806_WRITE) ? "chip writes on" :
		 IS_ENABLED(CONFIG_OZ8806_WRITE_CAR) ? "CAR write only" : "read-only");
	return 0;
}

static void oz8806_power_down(struct oz8806_drv *d)
{
	mutex_lock(&d->lock);
	d->stopping = true;
	mutex_unlock(&d->lock);
	cancel_delayed_work_sync(&d->work);
	mutex_lock(&d->lock);
	if (d->bmulib_ready) {
		bmu_power_down_chip(&d->bmu);		/* W5 */
		oz8806_copy_state(d);
	} else {
		oz8806_chip_sleep(&d->bmu);		/* W5 */
	}
	mutex_unlock(&d->lock);
}

static int oz8806_remove(struct i2c_client *client)
{
	struct oz8806_drv *d = i2c_get_clientdata(client);

	/* no supply call can reach d after this */
	mutex_lock(&oz_api_lock);
	drv_ready = 0;
	oz_drv = NULL;
	mutex_unlock(&oz_api_lock);
	unregister_pm_notifier(&d->pm_nb);
	oz8806_power_down(d);
	wakeup_source_unregister(d->ws);
	return 0;
}

static void oz8806_shutdown(struct i2c_client *client)
{
	oz8806_power_down(i2c_get_clientdata(client));
}

static const struct of_device_id oz8806_of_match[] = {
	{ .compatible = "mediatek,coulomb" },
	{ }
};
MODULE_DEVICE_TABLE(of, oz8806_of_match);

static const struct i2c_device_id oz8806_id[] = {
	{ "coulomb", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, oz8806_id);

static struct i2c_driver oz8806_driver = {
	.driver = {
		.name = "coulomb",
		.of_match_table = oz8806_of_match,
	},
	.probe = oz8806_probe,
	.remove = oz8806_remove,
	.shutdown = oz8806_shutdown,
	.id_table = oz8806_id,
};
module_i2c_driver(oz8806_driver);

MODULE_DESCRIPTION("O2Micro OZ8806 fuel gauge for the Titan Pocket");
MODULE_LICENSE("GPL");
