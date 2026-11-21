/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Level maths for the OZ8806 gauge on the Titan Pocket. It follows the
 * polling steps of the stock O2Micro bmulib ("2016.08.24/7.00.04") with the
 * level counted by the coulomb counter and corrected from the OCV at rest.
 * The core has no kernel dependencies so it also builds on the host
 * (tools/check-pocket-oz8806-soc.py).
 *
 * All maths is 32-bit signed int with truncating division unless a comment
 * says otherwise.
 */
#ifndef OZ8806_BMULIB_H
#define OZ8806_BMULIB_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef int64_t s64;
#endif

struct oz8806_pair { int32_t x; int32_t y; };

/* Board and battery values the level maths uses */
struct oz8806_config {
	int32_t rsense;		/* current sense resistor, mOhm */
	int32_t ntc_pullup;	/* NTC pull-up to ntc_ref_mv, Ohm */
	int32_t ntc_ref_mv;	/* NTC reference, mV */
	int32_t car_lsb;	/* CAR mAh = raw * car_lsb / rsense */
	int32_t curr_lsb;	/* mA = raw * curr_lsb / rsense / 100 / 4 */
	int32_t volt_lsb;	/* mV = (raw >> 4) * volt_lsb / 100 */
	int32_t design_mah;	/* full-charge capacity */
	int32_t cv_mv;		/* charge voltage */
	int32_t end_current_ma;	/* charge taper current that counts as full */
	int32_t end_volt_mv;	/* empty, under load */
	int32_t board_offset_ma; /* current offset written to the chip */
	uint8_t debug;		/* only counts CAR reads of 0 */
};

#include "oz8806_ntc.h"
/* OCV to level, (mV, %) with mV rising: the measured table from
 * tools/make-pocket-oz8806-ocv.py when it is there, else the placeholder */
#if __has_include("oz8806_ocv.h")
#include "oz8806_ocv.h"
#else
#include "oz8806_ocv_generic.h"
#endif

#define OZ8806_NTC_N	((int)(sizeof(oz8806_ntc) / sizeof(oz8806_ntc[0])))
#define OZ8806_OCV_N	((int)(sizeof(oz8806_ocv) / sizeof(oz8806_ocv[0])))

/* SoC GPIO 5 (hw_ver) selects the NTC pull-up: 1 is 24 kOhm, 0 is 120 kOhm */
#define OZ8806_HWVER_GPIO	5
#define OZ8806_HWVER1_PULLUP	24000
#define OZ8806_HWVER0_PULLUP	120000

#ifdef OZ8806_OCV_CAPACITY_MAH
#define OZ8806_DESIGN_MAH	OZ8806_OCV_CAPACITY_MAH
#else
/* battery label; the stock driver used 4250 */
#define OZ8806_DESIGN_MAH	4000
#endif

static const struct oz8806_config oz8806_board_config = {
	.rsense = 10,
	.ntc_pullup = OZ8806_HWVER1_PULLUP,
	.ntc_ref_mv = 1800,
	.car_lsb = 5,
	.curr_lsb = 781,
	.volt_lsb = 250,
	.design_mah = OZ8806_DESIGN_MAH,
	.cv_mv = 4400,
	.end_current_ma = 300,
	.end_volt_mv = 3450,
	.board_offset_ma = 30,
	.debug = 0,
};

/* battery state */
struct oz8806_batt_info {
	int op_bit0;	/* OP_CTRL bit 0 */
	int car;	/* mAh read from the chip */
	int soc;	/* % */
	int volt;	/* mV */
	int curr;	/* mA, charging positive */
	int ocv;	/* mV */
	int temp;	/* C */
	int prev_car;
	int rc;		/* remaining capacity, mAh */
	int i2c_errors;
};

/* gas gauge state */
struct oz8806_gas_gauge {
	int car_max;		/* 0x28000 / rsense */
	u8 dis_end;
	u8 chg_end;
	u8 b6, b7;		/* end flags, set by the end processes */
	int rc_mirror;
	int fcc;
	int used;		/* fcc minus rc */
	int chg_end_curr;	/* end_current + 2 */
	int dis_th;		/* -10 mA: at or below is discharging */
	u8 ocv_flag;		/* level came from a power-on OCV */
	int ri;			/* cell resistance for the OCV lookups, mOhm */
	u8 ext_temp;
	u8 init_ok;
};

enum {
	OZ_WAKE_NONE,
	OZ_WAKE_WROTE,		/* wrote 0x20, bit 6 clear after */
	OZ_WAKE_NOT_TAKEN,	/* wrote 0x20 twice, bit 6 still set or unreadable */
	OZ_WAKE_PEC,		/* reg 8 bit 2 set, not written */
	OZ_WAKE_REG8_FAIL,	/* reg 8 unreadable, not written */
};

struct oz8806_bmu {
	struct oz8806_config cfg;	/* oz8806_board_config, ntc_pullup chosen */
	struct oz8806_batt_info bi;
	struct oz8806_gas_gauge gg;
	int onepct;			/* design / 100 */

	u8 flag_ocv_boot, flag_first, flag_ocv_ok, resumed;
	u8 chg_end_done, dis_end_done, soc_from_ext_en, trust_car;
	u8 file_save, retry, retry_max, car_tol_pct, car_err;
	int init_soc, init_cap, last_soc, ext_soc, wait_cnt, car_zero_cnt;
	long long last_sec, now_sec;

	/* Read-only CAR emulation: virtual CAR raw = chip raw + car_off (16-bit) */
	u16 car_off;

	/* Whether the readings can be trusted.
	 * The readers set volt_fresh, curr_fresh and temp_ok; oz8806_poll()
	 * clears the fresh flags before each poll and checks them after.
	 * data_ok: init done and the polls valid, so the readings can be reported. */
	u8 volt_fresh, curr_fresh, temp_ok, asleep, volt_frozen;
	/* temp_valid: the last temperature came from an NTC reading inside the
	 * table, not the 25 C fallback or an old value; the charger needs it. */
	u8 temp_valid;
	int volt_raw, prev_volt_raw, curr_raw, prev_curr_raw, volt_same;
	int data_ok, bad_polls;

	/* Wake and sleep writes that are on without CONFIG_OZ8806_WRITE
	 * (OP_CTRL only). wake_pending: readings are invalid until 2250 ms
	 * after the wake write. wake_event: what the last wake attempt did,
	 * for the driver to log. */
	u8 wake_pending;
	u8 wake_event;
	u8 stopped;		/* sleep written at shutdown: no more wake writes */

	/* Charge counted from the CAR, uAh, charging positive. Not changed by
	 * the CAR writes: each write first counts up to the chip value, then
	 * moves the reference (cc_ref, virtual CAR raw) to the value written. */
	long long cc_uah;
	u16 cc_ref;
	u8 cc_ref_ok;

	/* Rest: no charger, |current| at most OZ8806_REST_MA, and the counted
	 * charge within that current since the last poll and since the rest
	 * began (rest_t0, boot time s; rest_cc0, uAh). After OZ8806_REST_S the
	 * OCV lookup is taken once (rest_done). If it differs by
	 * OZ8806_REST_DEADBAND or more, the level moves one point every
	 * OZ8806_REST_STEP_S toward it (rest_target, -1 if none) while the rest
	 * lasts. */
	long long rest_t0, rest_prev_t, rest_prev_cc, rest_cc0, rest_step_t;
	u8 rest_done;
	int rest_target;
	int rest_ocv_mv, rest_soc;	/* last rested OCV and its level, -1 if none */

	void *hw;			/* driver or host harness context */
};

/* Hardware hooks, given by the driver or the host harness. Reads are one try
 * each and return the value (>= 0) or a negative error. */
int oz8806_hw_read_byte(struct oz8806_bmu *b, u8 reg);
int oz8806_hw_read_word(struct oz8806_bmu *b, u8 reg);
int oz8806_hw_write_byte(struct oz8806_bmu *b, u8 reg, u8 val);
#if defined(CONFIG_OZ8806_WRITE) || defined(CONFIG_OZ8806_WRITE_CAR)
int oz8806_hw_write_word(struct oz8806_bmu *b, u8 reg, u16 val);
#endif
void oz8806_hw_set_addr(struct oz8806_bmu *b, u8 addr);
void oz8806_hw_msleep(struct oz8806_bmu *b, unsigned int ms);
unsigned int oz8806_hw_boot_ms(struct oz8806_bmu *b);
unsigned int oz8806_hw_power_on_ms(struct oz8806_bmu *b);
void oz8806_hw_reset_power_on_base(struct oz8806_bmu *b);
long long oz8806_hw_now_sec(struct oz8806_bmu *b);
long long oz8806_hw_boottime_sec(struct oz8806_bmu *b);	/* runs in suspend */
int oz8806_hw_adapter(struct oz8806_bmu *b);
int oz8806_hw_soc_from_ext(struct oz8806_bmu *b);
int oz8806_hw_save_capacity(struct oz8806_bmu *b);

/* entry points with the stock names */
void oz8806_bmu_setup(struct oz8806_bmu *b, int ntc_pullup);
void bmulib_init_chip(struct oz8806_bmu *b);
void bmu_polling_loop(struct oz8806_bmu *b);
void bmu_wake_up_chip(struct oz8806_bmu *b);
void bmu_power_down_chip(struct oz8806_bmu *b);
void charge_end_process(struct oz8806_bmu *b);
void discharge_end_process(struct oz8806_bmu *b);
void oz8806_wakeup_full_power(struct oz8806_bmu *b);
void oz8806_wake_if_asleep(struct oz8806_bmu *b);
void oz8806_chip_sleep(struct oz8806_bmu *b);
int oz8806_temp_read(struct oz8806_bmu *b, int *mv);
int afe_read_current(struct oz8806_bmu *b, int *ma);
int afe_read_cell_volt(struct oz8806_bmu *b, int *mv);

/* SOC core, exposed for the host test */
int one_latitude_table(int n, const struct oz8806_pair *t, int x);
int calculate_soc_result(struct oz8806_bmu *b);

/* Reading checks, not in stock */
#define OZ8806_VALID_MIN_MV	2500
#define OZ8806_VALID_MAX_MV	4600
#define OZ8806_FROZEN_MA	50	/* |current| above this should move the readings */
#define OZ8806_BAD_POLLS	3	/* invalid polls in a row before data_ok drops */
int oz8806_reading_valid(struct oz8806_bmu *b);
int oz8806_check_chip(struct oz8806_bmu *b);
int oz8806_poll(struct oz8806_bmu *b);
int oz8806_track_valid_core(struct oz8806_bmu *b, int valid);
int oz8806_shutdown_mv(struct oz8806_bmu *b);
int oz8806_report_soc(struct oz8806_bmu *b);

/* Rest and OCV. 30 mA is about C/140: the IR drop is a few mV and the cell
 * relaxes to within about 5 mV of its OCV in 30 min at room temperature.
 * 3 % is about what 5 mV of OCV error is worth in the flat middle of the
 * curve, so smaller differences are left to the counter. Away from 10..45 C
 * the OCV moves with temperature, so no correction there. */
#define OZ8806_REST_MA		30
#define OZ8806_REST_S		1800
#define OZ8806_REST_DEADBAND	3
#define OZ8806_REST_STEP_S	60	/* one point a minute, no visible jumps */
#define OZ8806_REST_SLACK_UAH	1000	/* two CAR steps */
#define OZ8806_REST_TMIN	10
#define OZ8806_REST_TMAX	45

/* one sample for the OCV run logger: fresh voltage, current and counted charge */
struct oz8806_sample {
	long long t_s;		/* boot time */
	int volt, curr, chip_ocv, temp, soc, rc;
	long long cc_uah;
	int rest_s;		/* seconds at rest, 0 if not */
};
int oz8806_sample(struct oz8806_bmu *b, struct oz8806_sample *s);

#endif
