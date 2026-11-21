// SPDX-License-Identifier: GPL-2.0
/*
 * OZ8806 level maths for the Titan Pocket: the polling steps, integer maths
 * and end-of-charge, end-of-discharge and shutdown-voltage handling of the
 * stock O2Micro bmulib. The level follows the coulomb counter against the
 * full-charge capacity, set from the OCV at boot and after a rest
 * (rest_check). Stock's capacity table and its charge and discharge ratios
 * are not used.
 *
 * Chip writes (marked W1..W7) happen only with CONFIG_OZ8806_WRITE,
 * except the CAR write (W7), which CONFIG_OZ8806_WRITE_CAR also turns on,
 * and the OP_CTRL wake (oz8806_wake_if_asleep) and sleep
 * (oz8806_chip_sleep), which are always on.
 * Without it nothing is written, and afe_write_car() keeps a software CAR
 * offset instead, so afe_read_car() returns what the chip would hold after
 * the stock write and the level follows the stock maths. Every place where
 * read-only differs from stock is marked "Read-only:".
 *
 * The file I/O stock does (/data/ocv_flag.dat, /data/offset.dat) is left out:
 * the files can't be opened that early, so a missing file is what stock sees
 * too.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
#include "oz8806_bmulib.h"

#define OZ_REG_STATUS		0x08
#define OZ_REG_OP_CTRL		0x09
#define OZ_REG_CELL_TEMP	0x0c
#define OZ_REG_CELL_VOLT	0x0e
#define OZ_REG_OCV		0x10
#define OZ_REG_CURRENT		0x12
#define OZ_REG_CAR		0x14
#define OZ_REG_BOARD_OFFSET	0x18
#define OZ_REG_OTP_CTRL		0x1a
#define OZ_ADDR			0x2f

#define CFG	(&b->cfg)
#define BI	(&b->bi)
#define GG	(&b->gg)

static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline long long labs64(long long v) { return v < 0 ? -v : v; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ---- register access ---- */

/* 8 tries, no delay; the final failure counts an i2c error */
static int afe_register_read_byte(struct oz8806_bmu *b, u8 reg, u8 *v)
{
	int i, r = -1;

	for (i = 0; i < 8; i++) {
		r = oz8806_hw_read_byte(b, reg);
		if (r >= 0) {
			*v = r;
			return r;
		}
	}
	BI->i2c_errors++;
	return r;
}

static int afe_register_read_word(struct oz8806_bmu *b, u8 reg, u16 *v)
{
	int i, r = -1;

	for (i = 0; i < 8; i++) {
		r = oz8806_hw_read_word(b, reg);
		if (r >= 0) {
			*v = r;
			return r;
		}
	}
	BI->i2c_errors++;
	return r;
}

/* W1, W3, W3b, W4, W5 */
static int afe_register_write_byte(struct oz8806_bmu *b, u8 reg, u8 v)
{
#ifdef CONFIG_OZ8806_WRITE
	int i, r = -1;

	for (i = 0; i < 8; i++) {
		r = oz8806_hw_write_byte(b, reg, v);
		if (r >= 0)
			return r;
	}
	BI->i2c_errors++;
	return r;
#else
	/* Read-only: not written */
	return 0;
#endif
}

/* driver-side byte read: 4 tries, no i2c error count */
static int drv_read_byte(struct oz8806_bmu *b, u8 reg, u8 *v)
{
	int i, r = -1;

	for (i = 0; i < 4; i++) {
		r = oz8806_hw_read_byte(b, reg);
		if (r >= 0) {
			*v = r;
			return 0;
		}
	}
	return r;
}

/* ---- readers ---- */

/* volt maths shared by 0x0c, 0x0e and 0x10: 2.5 mV LSB in the top 12 bits */
static int raw_to_mv(struct oz8806_bmu *b, int raw)
{
	return ((int)(s16)raw / 16) * CFG->volt_lsb / 100;
}

int afe_read_cell_volt(struct oz8806_bmu *b, int *mv)
{
	int k, r = -1, v;

	for (k = 0; k < 8; k++) {
		r = oz8806_hw_read_word(b, OZ_REG_CELL_VOLT);
		if (r >= 0) {
			v = raw_to_mv(b, r);
			if ((unsigned int)v <= 5117) {
				*mv = v;
				b->volt_fresh = 1;
				b->volt_raw = r;
				return r;
			}
			r = -1;
		}
		oz8806_hw_msleep(b, 5);
	}
	BI->i2c_errors++;
	b->volt_fresh = 0;
	return r;
}

static int afe_read_ocv_volt(struct oz8806_bmu *b, int *mv)
{
	int k, r = -1, v;

	for (k = 0; k < 8; k++) {
		r = oz8806_hw_read_word(b, OZ_REG_OCV);
		if (r >= 0) {
			v = raw_to_mv(b, r);
			if ((unsigned int)v <= 5117) {
				*mv = v;
				return r;
			}
			r = -1;
		}
	}
	BI->i2c_errors++;
	return r;
}

/* thermistor voltage, one try, no error count */
int oz8806_temp_read(struct oz8806_bmu *b, int *mv)
{
	int r = oz8806_hw_read_word(b, OZ_REG_CELL_TEMP), v;

	if (r < 0)
		return r;
	v = raw_to_mv(b, r);
	if ((unsigned int)v > 5117)
		return -1;
	*mv = v;
	return r;
}

int afe_read_current(struct oz8806_bmu *b, int *ma)
{
	int k, r = -1, t;

	for (k = 0; k < 8; k++) {
		r = oz8806_hw_read_word(b, OZ_REG_CURRENT);
		if (r >= 0) {
			t = CFG->rsense ? (int)(s16)r * CFG->curr_lsb / CFG->rsense : 0;
			t = t / 100;
			*ma = t / 4;
			b->curr_fresh = 1;
			b->curr_raw = r;
			return r;
		}
		oz8806_hw_msleep(b, 5);
	}
	BI->i2c_errors++;
	b->curr_fresh = 0;
	return r;
}

/* chip CAR raw word, 8 tries 5 ms apart */
static int read_car_raw(struct oz8806_bmu *b)
{
	int k, r = -1;

	for (k = 0; k < 8; k++) {
		r = oz8806_hw_read_word(b, OZ_REG_CAR);
		if (r >= 0)
			return r;
		oz8806_hw_msleep(b, 5);
	}
	BI->i2c_errors++;
	return r;
}

/* Counts the change since the last virtual CAR raw seen. A step of 10 % of
 * the capacity or more in one read is a glitch, as normal_poll treats it,
 * and isn't counted. */
static void cc_track(struct oz8806_bmu *b, u16 vraw)
{
	int d = (s16)(u16)(vraw - b->cc_ref);

	if (b->cc_ref_ok && iabs(d * CFG->car_lsb / CFG->rsense) < CFG->design_mah / 10)
		b->cc_uah += (long long)d * CFG->car_lsb * 1000 / CFG->rsense;
	b->cc_ref = vraw;
	b->cc_ref_ok = 1;
}

static int afe_read_car(struct oz8806_bmu *b, int *mah)
{
	int r = read_car_raw(b);
	s16 raw;

	if (r < 0)
		return r;
	/* Read-only: the virtual CAR is the chip CAR plus the software offset */
	raw = (s16)(u16)((u16)r + b->car_off);
	cc_track(b, (u16)raw);
	*mah = (s16)((int)raw * CFG->car_lsb / CFG->rsense);
	return r;
}

/* W7. With CONFIG_OZ8806_WRITE_CAR the chip is written (car_off stays 0). */
static int afe_write_car(struct oz8806_bmu *b, int mah)
{
	u16 raw = (u16)(mah * CFG->rsense / CFG->car_lsb);
	int r = read_car_raw(b);
#if defined(CONFIG_OZ8806_WRITE) || defined(CONFIG_OZ8806_WRITE_CAR)
	int k;

	/* count up to the chip value before it is replaced */
	if (r >= 0)
		cc_track(b, (u16)r);
	for (k = 0; k < 8; k++) {
		r = oz8806_hw_write_word(b, OZ_REG_CAR, raw);
		if (r >= 0) {
			b->cc_ref = raw;
			return r;
		}
		oz8806_hw_msleep(b, 5);
	}
	BI->i2c_errors++;
	return r;
#else
	/* Read-only: move the software offset so the next read gives raw */
	if (r < 0)
		return r;
	cc_track(b, (u16)((u16)r + b->car_off));
	b->car_off = (u16)(raw - (u16)r);
	b->cc_ref = raw;
	return 0;
#endif
}

static int afe_read_board_offset(struct oz8806_bmu *b, int *ma)
{
	u16 raw;
	int r = afe_register_read_word(b, OZ_REG_BOARD_OFFSET, &raw), v;

	if (r < 0)
		return r;
	v = raw & 0x7ff;
	if (raw & 0x400)
		v |= (int)0xfffffc00;
	*ma = (s16)(v * 78 / (CFG->rsense * 10));
	return r;
}

/* W6 */
static int afe_write_board_offset(struct oz8806_bmu *b, int ma)
{
#ifdef CONFIG_OZ8806_WRITE
	int v, i, r = -1;

	if (ma > 0) {
		v = imin(ma * CFG->rsense * 10 / 78, 0x3ff);
	} else if (ma == 0) {
		v = 0;
	} else {
		ma = imax(ma, -0x13800 / (CFG->rsense * 10));
		v = ((CFG->rsense * ma * 10) / 78 & 0x3ff) | 0x400;
	}
	for (i = 0; i < 8; i++) {
		r = oz8806_hw_write_word(b, OZ_REG_BOARD_OFFSET, v);
		if (r >= 0)
			return r;
	}
	BI->i2c_errors++;
	return r;
#else
	/* Read-only: the chip keeps the last offset written */
	(void)b;
	(void)ma;
	return 0;
#endif
}

/* /data/offset.dat; stock can't open it this early, so: no file */
static int bmu_read_data(void)
{
	return -1;
}

static int afe_read_cell_temp(struct oz8806_bmu *b, int *c)
{
	int k, r = -1, mv = 0, ohm;

	b->temp_valid = 0;
	if (GG->ext_temp) {
		b->temp_ok = 1;
		return 0;
	}
	for (k = 0; k < 8; k++) {
		r = oz8806_temp_read(b, &mv);
		if (r >= 0)
			break;
		oz8806_hw_msleep(b, 5);
	}
	if (r < 0) {
		BI->i2c_errors++;
		b->temp_ok = 0;
		return r;
	}
	b->temp_ok = 1;
	if (mv > 0 && mv < CFG->ntc_ref_mv) {
		ohm = mv * CFG->ntc_pullup / (CFG->ntc_ref_mv - mv);
		*c = one_latitude_table(OZ8806_NTC_N, oz8806_ntc, ohm);
		b->temp_valid = ohm >= oz8806_ntc[0].x && ohm <= oz8806_ntc[OZ8806_NTC_N - 1].x;
	} else {
		*c = 25;
	}
	return r;
}

/* ---- tables ---- */

int one_latitude_table(int n, const struct oz8806_pair *t, int x)
{
	int i, num;

	if (n <= 0 || x < t[0].x)
		return t[0].y;
	if (t[0].x == x)
		return t[0].y;
	for (i = 1; ; i++) {
		if (i == n)
			return t[n - 1].y;
		if (t[i].x == x)
			return t[i].y;
		if (t[i].x >= x)
			break;
	}
	num = (x - t[i - 1].x) * (t[i].y - t[i - 1].y);
	return (t[i].x != t[i - 1].x ? num / (t[i].x - t[i - 1].x) : num) + t[i - 1].y;
}

/* level from the voltage with the IR drop taken out: ri while discharging,
 * ri + 100 mOhm while charging as stock */
int calculate_soc_result(struct oz8806_bmu *b)
{
	int I = BI->curr, v;

	if (I < 0)
		v = BI->volt - I * GG->ri / 1000;
	else
		v = BI->volt - I * (GG->ri + 100) / 1000;
	return one_latitude_table(OZ8806_OCV_N, oz8806_ocv, v);
}

/* ---- chip state ---- */

/* W1: clear OP_CTRL bit 6 (sleep) */
void oz8806_wakeup_full_power(struct oz8806_bmu *b)
{
#ifdef CONFIG_OZ8806_WRITE
	u8 v = 0;

	if (drv_read_byte(b, OZ_REG_OP_CTRL, &v) < 0 || !(v & 0x40))
		return;
	drv_read_byte(b, OZ_REG_OP_CTRL, &v);
	oz8806_hw_write_byte(b, OZ_REG_OP_CTRL, v & ~0x40);
	if (drv_read_byte(b, OZ_REG_OP_CTRL, &v) >= 0 && !(v & 0x40))
		oz8806_hw_reset_power_on_base(b);
#else
	/* Read-only: the 0x20 write of oz8806_wake_if_asleep instead */
	oz8806_wake_if_asleep(b);
#endif
}

/* OP_CTRL write with the same 8 tries as afe_register_write_byte, in both builds */
static int op_ctrl_write(struct oz8806_bmu *b, u8 v)
{
	int i, r = -1;

	for (i = 0; i < 8; i++) {
		r = oz8806_hw_write_byte(b, OZ_REG_OP_CTRL, v);
		if (r >= 0)
			return r;
	}
	BI->i2c_errors++;
	return r;
}

/* the driver clears wake_event once it has logged it; a WROTE not yet logged stays */
static void wake_event(struct oz8806_bmu *b, u8 ev)
{
	if (b->wake_event != OZ_WAKE_WROTE)
		b->wake_event = ev;
}

/* Always on, with or without CONFIG_OZ8806_WRITE (the one write allowed
 * read-only). Reads OP_CTRL; if bit 6 (sleep) is set or the read fails, and
 * reg 8 bit 2 (PEC error) is clear, writes 0x20 (once more if it didn't
 * take) and reads back. Readings count as invalid for 2250 ms after the
 * write (wake_pending, checked in oz8806_reading_valid). */
void oz8806_wake_if_asleep(struct oz8806_bmu *b)
{
	u8 v = 0, st = 0;

	if (b->stopped)
		return;
	if (drv_read_byte(b, OZ_REG_OP_CTRL, &v) >= 0 && !(v & 0x40))
		return;
	if (drv_read_byte(b, OZ_REG_STATUS, &st) < 0) {
		wake_event(b, OZ_WAKE_REG8_FAIL);
		return;
	}
	if (st & 0x04) {
		wake_event(b, OZ_WAKE_PEC);
		return;
	}
	op_ctrl_write(b, 0x20);
	if (drv_read_byte(b, OZ_REG_OP_CTRL, &v) < 0 || (v & 0x40)) {
		op_ctrl_write(b, 0x20);
		if (drv_read_byte(b, OZ_REG_OP_CTRL, &v) < 0 || (v & 0x40)) {
			wake_event(b, OZ_WAKE_NOT_TAKEN);
			goto written;
		}
	}
	wake_event(b, OZ_WAKE_WROTE);
written:
	oz8806_hw_reset_power_on_base(b);
	b->wake_pending = 1;
}

/* Always on. Stock bmu_power_down_chip's write: OP_CTRL 0x60, sleep. */
void oz8806_chip_sleep(struct oz8806_bmu *b)
{
	b->stopped = 1;		/* no wake write after this */
	op_ctrl_write(b, 0x60);
}

/* CRC-8, poly 0x07, init 0 (SMBus PEC) */
static u8 pec_crc8(const u8 *p, int n)
{
	u8 c = 0;
	int i, k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++)
			c = c & 0x80 ? (u8)(c << 1) ^ 0x07 : (u8)(c << 1);
	}
	return c;
}

static void check_oz8806_staus(struct oz8806_bmu *b)
{
	u8 v, reg8 = 0;
	int addr, i, found = 0;

	for (i = 0; i < 4; i++)
		afe_register_read_byte(b, i, &v);
	/* bus probe: 0x2f down to 0x28 until reg 4 answers */
	for (addr = OZ_ADDR; addr >= 0x28; addr--) {
		oz8806_hw_set_addr(b, addr);
		if (oz8806_hw_read_byte(b, 4) >= 0) {
			found = 1;
			break;
		}
	}
	if (!found) {
		oz8806_hw_set_addr(b, OZ_ADDR);
		return;
	}
	if (afe_register_read_byte(b, 8, &reg8) >= 0 && (reg8 & 4)) {
		u8 m[3] = { (u8)(addr << 1), 0x08, (u8)(reg8 & ~4) };
		u8 crc = pec_crc8(m, 3);

		/* W2 */
#ifdef CONFIG_OZ8806_WRITE
		oz8806_hw_write_word(b, 8, (u16)((reg8 & ~4) | crc << 8));
#else
		(void)crc;
#endif
	}
	oz8806_wakeup_full_power(b);
	afe_register_write_byte(b, OZ_REG_OP_CTRL, 0x20);	/* W4 */
	oz8806_hw_msleep(b, 10);
	afe_register_write_byte(b, OZ_REG_OTP_CTRL, 0x02);	/* W3b, force OTP mapping */
	oz8806_hw_msleep(b, 10);
	oz8806_hw_set_addr(b, OZ_ADDR);
	for (i = 0; i <= 10; i++) {
		if (afe_register_read_byte(b, OZ_REG_OTP_CTRL, &v) >= 0 && !(v & 4))
			break;
		if (i < 10)
			oz8806_hw_msleep(b, 10);
	}
	for (i = 0; i < 4; i++)
		afe_register_read_byte(b, i, &v);
}

/* W3: VD23 trim from reg 3 bits 8 and 7 into reg 8 bits 1..0 */
static void trim_bmu_VD23(struct oz8806_bmu *b)
{
	int i, r = -1, trim;
	u8 reg8 = 0, nv;

	for (i = 0; i < 3; i++) {
		r = oz8806_hw_read_word(b, 3);
		if (r >= 0)
			break;
	}
	if (r < 0)
		return;
	trim = (r >> 8 & 1) | (r >> 6 & 2);
	for (i = 0; i < 3; i++) {
		int t = oz8806_hw_read_byte(b, 8);

		if (t >= 0) {
			reg8 = t;
			break;
		}
		if (i == 2)
			return;
	}
	nv = trim == 2 ? (reg8 & 0xfc) | 3 : (reg8 & 0xfc) | trim;
#ifdef CONFIG_OZ8806_WRITE
	for (i = 0; i < 3; i++)
		if (oz8806_hw_write_byte(b, 8, nv) >= 0)
			break;
#else
	(void)nv;
#endif
}

void oz8806_bmu_setup(struct oz8806_bmu *b, int ntc_pullup)
{
	struct oz8806_gas_gauge *g = GG;
	void *hw = b->hw;
	u8 wake_pending = b->wake_pending, wake_event = b->wake_event, stopped = b->stopped;

	memset(b, 0, sizeof(*b));
	b->hw = hw;
	/* the probe wake's 2250 ms window and log stay across the setup */
	b->wake_pending = wake_pending;
	b->wake_event = wake_event;
	b->stopped = stopped;
	b->cfg = oz8806_board_config;
	b->cfg.ntc_pullup = ntc_pullup;

	b->soc_from_ext_en = 1;
	b->car_tol_pct = 50;
	b->trust_car = 0;
	b->file_save = 0;
	b->retry_max = 3;
	b->ext_soc = -1;

	g->car_max = 0x28000 / CFG->rsense;
	g->fcc = CFG->design_mah;
	g->chg_end_curr = CFG->end_current_ma + 2;
	g->dis_th = -10;
	g->ri = 18;
	g->ext_temp = 0;

	b->onepct = CFG->design_mah / 100;
	b->rest_ocv_mv = -1;
	b->rest_soc = -1;
	b->rest_target = -1;
}

/* bmulib_init after the parameter and state setup */
void bmulib_init_chip(struct oz8806_bmu *b)
{
	unsigned int boot = oz8806_hw_boot_ms(b);
	int bo, o;
	u16 w;
	u8 v;

	check_oz8806_staus(b);
	trim_bmu_VD23(b);
	if (boot < 625) {
		oz8806_hw_msleep(b, 625 - boot);
		afe_register_read_word(b, OZ_REG_BOARD_OFFSET, &w);
	}
	if (afe_read_board_offset(b, &bo) >= 0) {
		o = CFG->board_offset_ma;
		if (o == 0) {
			if ((unsigned int)(bo - 1) < 10)
				goto skip;
			o = 7;
		}
		afe_write_board_offset(b, o);			/* W6 */
	}
skip:
	afe_register_write_byte(b, OZ_REG_OP_CTRL, 0x20);	/* W4 */
	if (afe_register_read_byte(b, OZ_REG_OP_CTRL, &v) < 0 || (v & 0x40))
		afe_register_write_byte(b, OZ_REG_OP_CTRL, 0x20);			/* W4 */
	GG->ext_temp = 0;
}

static void cap_car(struct oz8806_bmu *b)
{
	if (BI->car >= GG->car_max - 10)
		BI->car = BI->prev_car = GG->car_max - GG->car_max / 100;
}

/* first poll: wait for the ADC and take the power-on OCV if the chip has one */
static void wait_ocv_flag_fun(struct oz8806_bmu *b)
{
	unsigned int boot = oz8806_hw_boot_ms(b), pon;
	int k, got = 0, v;
	u16 f = 0;
	u8 r;

	if (boot < 2000)
		oz8806_hw_msleep(b, 2000 - boot);
	b->flag_first = 1;
	for (k = 0; k < 3; k++) {
		if (afe_register_read_word(b, OZ_REG_OCV, &f) >= 0) {
			got = 1;
			boot = oz8806_hw_boot_ms(b);
			pon = oz8806_hw_power_on_ms(b);
			if ((f & 3) == 0) {
				if (boot < 2250)
					oz8806_hw_msleep(b, 2250 - boot);
			} else if (pon < 2250) {
				oz8806_hw_msleep(b, 2250 - pon);
			}
			break;
		}
	}
	afe_read_cell_volt(b, &BI->volt);
	afe_read_current(b, &BI->curr);
	afe_read_cell_temp(b, &BI->temp);
	afe_read_car(b, &BI->car);
	BI->soc = BI->car * 100 / CFG->design_mah;
	if (!got) {
		;
	} else if (!(f & 1)) {
		if (f & 2) {
			afe_read_ocv_volt(b, &BI->ocv);
			b->flag_ocv_ok = 1;
		}
	} else {
		b->flag_ocv_boot = 1;
		afe_read_ocv_volt(b, &BI->ocv);
		afe_read_cell_volt(b, &BI->volt);
		if (BI->ocv > CFG->cv_mv + 70) {
			afe_read_cell_volt(b, &BI->volt);
			BI->ocv = BI->volt;
		}
		afe_read_current(b, &BI->curr);
		v = BI->ocv;
		if (BI->curr > 50)
			v -= 100;
		BI->soc = one_latitude_table(OZ8806_OCV_N, oz8806_ocv, v);
		if ((unsigned int)BI->soc > 100)
			BI->soc = 50;
		BI->car = BI->soc * CFG->design_mah / 100;
		afe_read_current(b, &BI->curr);
		cap_car(b);
		afe_write_car(b, BI->car);			/* W7 */
		BI->prev_car = BI->car;
		GG->fcc = CFG->design_mah;
		BI->rc = GG->rc_mirror = BI->soc * CFG->design_mah / 100;
		GG->used = CFG->design_mah - BI->rc;
	}
	afe_register_read_byte(b, 0, &r);
	afe_register_read_byte(b, OZ_REG_OP_CTRL, &r);
}

static void check_shutdwon_voltage(struct oz8806_bmu *b)
{
	int s;

	afe_read_cell_volt(b, &BI->volt);
	if (!b->flag_ocv_ok)
		return;
	s = imax(one_latitude_table(OZ8806_OCV_N, oz8806_ocv, BI->ocv), b->init_soc);
	if (BI->soc - s > 20 || (s < 6 && BI->volt < CFG->end_volt_mv - 50)) {
		s = imax(s, 0);
		BI->soc = s;
		BI->rc = BI->car = s * GG->fcc / 100;
		if (BI->car > GG->car_max - 20)
			BI->car = GG->car_max - 20;
		afe_write_car(b, BI->car);			/* W7 */
		BI->prev_car = BI->car;
		GG->rc_mirror = BI->rc;
		GG->used = imax(GG->fcc - BI->rc, 0);
	}
}

void charge_end_process(struct oz8806_bmu *b)
{
	GG->fcc = CFG->design_mah;
	BI->rc = GG->fcc + GG->fcc / 100 - 1;
	afe_write_car(b, BI->rc);				/* W7; car, prev_car kept */
	b->chg_end_done = 1;
	BI->soc = 100;
	GG->chg_end = GG->b6 = GG->b7 = 1;
	GG->used = 0;
	b->flag_ocv_boot = 0;
}

void discharge_end_process(struct oz8806_bmu *b)
{
	GG->fcc = CFG->design_mah;
	BI->rc = GG->fcc / 100 - 1;
	afe_write_car(b, BI->rc);				/* W7 */
	BI->prev_car = BI->rc;
	BI->soc = 0;
	GG->dis_end = GG->b6 = GG->b7 = 1;
	GG->rc_mirror = 0;
}

/* Counted charge allowed over dt seconds at rest: OZ8806_REST_MA plus
 * OZ8806_REST_SLACK_UAH for the CAR's 0.5 mAh steps. uAh. */
static long long rest_allow_uah(long long dt)
{
	return OZ8806_REST_MA * dt * 10 / 36 + OZ8806_REST_SLACK_UAH;
}

/* Move the level one point toward rest_target and write the CAR to match. */
static void rest_step(struct oz8806_bmu *b, long long now)
{
	int s = BI->soc + (b->rest_target > BI->soc ? 1 : -1);

	BI->soc = s;
	/* the middle of the point, so the next count doesn't drop it */
	BI->rc = BI->car = s * GG->fcc / 100 + GG->fcc / 200;
	afe_write_car(b, BI->car);				/* W7 */
	BI->prev_car = BI->car;
	GG->rc_mirror = BI->rc;
	GG->used = imax(GG->fcc - BI->rc, 0);
	GG->dis_end = 0;
	GG->chg_end = 0;
	b->rest_step_t = now;
	if (s == b->rest_target)
		b->rest_target = -1;
}

/* Rest tracking and the OCV correction, after each steady poll. The counted
 * charge covers the time in suspend, when there are no polls: it must stay
 * within the rest current both since the last poll and since the rest began. */
static void rest_check(struct oz8806_bmu *b)
{
	long long now = oz8806_hw_boottime_sec(b), dt = now - b->rest_prev_t;
	int quiet, s, v;

	quiet = b->rest_prev_t && dt >= 0 && !oz8806_hw_adapter(b) &&
		iabs(BI->curr) <= OZ8806_REST_MA;
	if (quiet)
		quiet = labs64(b->cc_uah - b->rest_prev_cc) <= rest_allow_uah(dt) &&
			labs64(b->cc_uah - b->rest_cc0) <= rest_allow_uah(now - b->rest_t0);
	b->rest_prev_t = now;
	b->rest_prev_cc = b->cc_uah;
	if (!quiet) {
		b->rest_t0 = now;
		b->rest_cc0 = b->cc_uah;
		b->rest_done = 0;
		b->rest_target = -1;
		return;
	}
	if (b->rest_target >= 0) {
		if (now - b->rest_step_t >= OZ8806_REST_STEP_S)
			rest_step(b, now);
		return;
	}
	if (b->rest_done || now - b->rest_t0 < OZ8806_REST_S)
		return;
	b->rest_done = 1;
	if (!b->temp_valid || BI->temp < OZ8806_REST_TMIN || BI->temp > OZ8806_REST_TMAX)
		return;
	v = BI->volt - BI->curr * GG->ri / 1000;
	s = one_latitude_table(OZ8806_OCV_N, oz8806_ocv, v);
	b->rest_ocv_mv = v;
	b->rest_soc = s;
	if (iabs(s - BI->soc) < OZ8806_REST_DEADBAND)
		return;
	/* 0 and 100 % stay with the end voltage and the end of charge */
	b->rest_target = imax(1, imin(s, 99));
	if (b->rest_target == BI->soc) {
		b->rest_target = -1;
		return;
	}
	rest_step(b, now);
}

/* steady poll, once GG->init_ok is set */
static void normal_poll(struct oz8806_bmu *b)
{
	int c = 0, r, delta, bo, o, lim;
	u8 op;

	BI->prev_car = BI->car;
	afe_read_cell_temp(b, &BI->temp);
	afe_read_cell_volt(b, &BI->volt);
	afe_read_current(b, &BI->curr);
	r = afe_read_car(b, &c);
	if (CFG->debug && c == 0)
		b->car_zero_cnt++;
	if (r >= 0 && c > 0) {
		if (iabs(c - BI->prev_car) < CFG->design_mah * 10 / 100) {
			BI->car = c;
			b->car_err = 0;
		} else if (++b->car_err > 3) {
			b->car_err = 0;
			afe_write_car(b, BI->rc);		/* W7 */
			BI->prev_car = BI->car = BI->rc;
		}
	}
	if (BI->volt < CFG->end_volt_mv && BI->curr > 0 && BI->prev_car < BI->car) {
		BI->car = BI->prev_car;
		afe_write_car(b, BI->prev_car);			/* W7 */
	}
	GG->rc_mirror += BI->car - BI->prev_car;
	GG->used += BI->prev_car - BI->car;
	if (GG->rc_mirror < 0)
		GG->rc_mirror = 0;
	if (GG->used < 0)
		GG->used = 0;

	if (oz8806_hw_adapter(b) == 0 || BI->curr <= GG->dis_th) {
		/* discharging */
		if (BI->soc < 100)
			GG->chg_end = 0;
		delta = (s16)((s16)BI->prev_car - (s16)BI->car);
		/* at the end only a rest above empty moves the level (rest_check) */
		if (delta > 0 && !GG->dis_end) {
			BI->rc -= delta;
			BI->soc = GG->fcc ? BI->rc * 100 / GG->fcc : 0;
			/* 0 % only at the end voltage; until then hold 1 % */
			if (BI->soc < 1) {
				if (BI->volt > CFG->end_volt_mv) {
					BI->soc = 1;
					BI->rc = b->onepct;
				} else {
					GG->dis_end = 1;
				}
			}
		}
	} else {
		/* charging: adapter present and curr above dis_th */
		GG->dis_end = 0;
		if (!GG->chg_end && BI->soc < 100) {
			delta = (s16)((s16)BI->car - (s16)BI->prev_car);
			if (delta > 0) {
				BI->rc += delta;
				BI->soc = GG->fcc ? BI->rc * 100 / GG->fcc : 0;
			}
			/* 100 % only at the end point (taper current at the charge
			 * voltage); the driver's full detection also gets there */
			if (BI->soc >= 100) {
				if (BI->curr < GG->chg_end_curr && BI->volt >= CFG->cv_mv - 50) {
					GG->chg_end = 1;
				} else {
					BI->soc = 99;
					BI->rc = GG->fcc - 1;
				}
			}
		}
	}
	rest_check(b);
	if (GG->chg_end) {
		if (!b->chg_end_done) {
			b->chg_end_done = 1;
			charge_end_process(b);			/* W7 */
		}
	} else {
		b->chg_end_done = 0;
	}
	if (GG->dis_end) {
		if (!b->dis_end_done) {
			b->dis_end_done = 1;
			discharge_end_process(b);		/* W7 */
		}
	} else {
		b->dis_end_done = 0;
	}
	if (GG->dis_end && BI->soc > 0)
		GG->dis_end = 0;
	if (BI->soc < 100)
		GG->chg_end = 0;

	if (BI->car < 0) {
		if (BI->volt < CFG->cv_mv - 200)
			BI->car = BI->soc < 1 ? BI->rc : BI->soc * GG->fcc / 100 - 1;
		else
			BI->car = GG->fcc - 1;
		afe_write_car(b, BI->car);			/* W7 */
		BI->prev_car = BI->car;
	}
	if (afe_read_car(b, &c) >= 0) {
		if (c < 5 || c > (CFG->car_lsb << 15) / CFG->rsense - 5) {
			afe_write_car(b, BI->rc);		/* W7 */
			BI->prev_car = BI->car = BI->rc;
		}
		/* keeps CAR slaved to rc */
		if (iabs(c - BI->rc) > b->onepct && BI->rc < GG->car_max && BI->rc > 0) {
			afe_write_car(b, BI->rc);		/* W7 */
			BI->prev_car = BI->car = BI->rc;
		}
		lim = GG->fcc + GG->fcc / 100 - 1;
		if (BI->car > lim) {
			BI->car = BI->prev_car = lim;
			afe_write_car(b, lim);			/* W7 */
		}
	}
	b->last_sec = b->now_sec;
	b->now_sec = oz8806_hw_now_sec(b);
	lim = GG->fcc + GG->fcc / 100 - 1;
	if (BI->rc < GG->fcc / 100 - 1)
		BI->rc = GG->fcc / 100 - 1;
	if (BI->rc > lim)
		BI->rc = lim;
	GG->fcc = CFG->design_mah;
	if (BI->soc != b->last_soc) {
		b->last_soc = BI->soc;
		return;
	}
	op = 0;
	r = afe_register_read_byte(b, OZ_REG_OP_CTRL, &op);
	BI->op_bit0 = op & 1;
	if (r < 0)
		BI->op_bit0 = 0;
	if (r < 0 || (op & 0x40))
		afe_register_write_byte(b, OZ_REG_OP_CTRL, 0x20);	/* W4 */
	if (afe_read_board_offset(b, &bo) >= 0) {
		o = CFG->board_offset_ma;
		if (o == 0) {
			if ((unsigned int)(bo - 1) > 9)
				afe_write_board_offset(b, 7);	/* W6 */
		} else if (iabs(bo - o) > 3) {
			afe_write_board_offset(b, o);		/* W6 */
		}
	}
	if (BI->i2c_errors) {
		check_oz8806_staus(b);
		trim_bmu_VD23(b);
		BI->i2c_errors = 0;
	}
}

void bmu_polling_loop(struct oz8806_bmu *b)
{
	int cap, s, c, sc, bo = 0, o, r, fbo;
	unsigned int pon;
	u16 f;
	u8 op = 0;

	if (GG->init_ok) {
		normal_poll(b);
		return;
	}
	if (!b->flag_first) {
		wait_ocv_flag_fun(b);
		goto after_init;
	}

	/* init polls */
	b->wait_cnt++;
	oz8806_wakeup_full_power(b);				/* W1 */
	r = afe_register_read_byte(b, OZ_REG_OP_CTRL, &op);
	BI->op_bit0 = op & 1;
	if (r < 0 || (op & 0x40)) {
		afe_register_write_byte(b, OZ_REG_OP_CTRL, 0x20);	/* W4 */
		afe_register_read_byte(b, OZ_REG_OP_CTRL, &op);
	}
	if (afe_register_read_word(b, OZ_REG_OCV, &f) >= 0 && (f & 1))
		wait_ocv_flag_fun(b);
	pon = oz8806_hw_power_on_ms(b);
	if (pon < 2250)
		oz8806_hw_msleep(b, 2250 - pon);
	afe_read_cell_temp(b, &BI->temp);
	afe_read_cell_volt(b, &BI->volt);
	afe_read_current(b, &BI->curr);
	afe_read_car(b, &BI->car);
	b->init_soc = calculate_soc_result(b);
	b->init_cap = b->init_soc * CFG->design_mah / 100;
	if (oz8806_hw_boot_ms(b) < 5000)
		goto after_init;				/* wait for the ADC */
	GG->fcc = CFG->design_mah;
	cap = BI->car;
	if (cap > 5 && !b->flag_ocv_boot)
		goto choose;					/* trust the chip's CAR */
	if (b->soc_from_ext_en) {
		b->ext_soc = oz8806_hw_soc_from_ext(b);		/* RTC spare - 1 */
		if (b->ext_soc != -1 && b->ext_soc <= 100) {
			cap = GG->fcc * b->ext_soc / 100;
			goto choose;
		}
		b->retry = 0;
		b->soc_from_ext_en = 0;
	}
	sc = oz8806_hw_save_capacity(b);
	if (sc == -2) {
		if (b->retry < 11) {
			b->retry++;
			goto after_init;
		}
		goto no_source;
	}
	if (sc != -1) {
		cap = sc;
		goto choose;
	}
	goto no_source;

choose:
	s = b->ext_soc != -1 ? b->ext_soc : cap * 100 / GG->fcc;
	if (!b->flag_ocv_boot) {
		GG->ocv_flag = 0;
		afe_read_car(b, &BI->car);
		if (BI->curr < 1 ? BI->car > 0 : BI->car >= 3) {
			c = BI->car;
			if (cap < c)
				BI->rc = (c - cap > b->onepct || (unsigned int)b->ext_soc <= 100) ? c : cap;
			else
				BI->rc = (cap - c > b->onepct) ? c : cap;
		} else {
			if (iabs(b->init_soc - s) > 30 || (s < 6 && BI->volt < CFG->end_volt_mv - 50))
				BI->rc = b->init_cap;
			else
				BI->rc = cap;
		}
	} else {
		GG->ocv_flag = 1;
		if (s < 91 || BI->soc < 91) {
			if (BI->curr < 0)
				BI->rc = (iabs(b->init_soc - s) < 21 &&
					  (s > 5 || BI->volt >= CFG->end_volt_mv - 50)) ? cap : b->init_cap;
			else
				BI->rc = iabs(s - b->init_soc) < 31 ? cap : b->init_cap;
		} else {
			BI->rc = cap;
		}
	}
	if (cap >= 0)
		goto finish;
no_source:
	if (!b->flag_ocv_boot) {
		afe_read_car(b, &BI->car);
		if (BI->curr < 1 ? BI->car < 1 : BI->car < 3) {
			BI->rc = b->init_cap;
		} else {
			c = BI->car;
			if (c < b->onepct)
				b->car_tol_pct = 30;
			BI->rc = iabs(b->init_cap - c) > b->car_tol_pct * CFG->design_mah / 100 ?
				 b->init_cap : c;
			if (b->trust_car)
				BI->rc = BI->car;
		}
	} else {
		BI->rc = b->init_cap;
	}
finish:
	if (BI->rc < 1) {
		afe_read_cell_volt(b, &BI->volt);
		BI->soc = (unsigned int)b->init_soc > 100 ? 50 : b->init_soc;
		BI->car = BI->soc * CFG->design_mah / 100;
		cap_car(b);
		afe_write_car(b, BI->car);			/* W7 */
		BI->prev_car = BI->rc = BI->car;
	}
	BI->rc = imin(BI->rc, GG->fcc + GG->fcc / 100 - 1);
	BI->soc = BI->rc * 100 / GG->fcc;
	if ((unsigned int)b->ext_soc <= 100 && BI->rc == b->ext_soc * GG->fcc / 100)
		BI->soc = b->ext_soc;
	BI->car = BI->rc;
	cap_car(b);
	afe_write_car(b, BI->car);				/* W7, every boot */
	BI->prev_car = BI->car;
	GG->rc_mirror = BI->rc;
	GG->used = imax(GG->fcc - BI->rc, 0);
	/* the counter says empty but the voltage doesn't: take the OCV level */
	if (BI->soc < 1 && BI->volt > CFG->end_volt_mv && b->init_soc > 0) {
		BI->soc = imin(b->init_soc, 99);
		BI->rc = BI->car = BI->soc * GG->fcc / 100;
		afe_write_car(b, BI->car);			/* W7 */
		BI->prev_car = BI->car;
		GG->rc_mirror = BI->rc;
		GG->used = imax(GG->fcc - BI->rc, 0);
	}
	if (BI->soc < 1) {
		GG->dis_end = 1;
		BI->soc = 0;
		GG->rc_mirror = 0;
		GG->ocv_flag = 0;
	}
	if (BI->soc > 99) {
		b->chg_end_done = 1;
		GG->chg_end = 1;
		BI->soc = 100;
		GG->used = 0;
		GG->b7 = 1;
		GG->ocv_flag = 0;
	}
	if (b->flag_ocv_ok)
		check_shutdwon_voltage(b);
	b->last_soc = BI->soc;
	GG->init_ok = 1;

after_init:
	if (!GG->init_ok)
		return;
	/* board offset, once, right after init */
	r = afe_read_board_offset(b, &bo);
	o = CFG->board_offset_ma;
	if (o != bo && o != 0) {
		afe_write_board_offset(b, o);			/* W6 */
		return;
	}
	if (bmu_read_data() < 0) {
		if ((unsigned int)(bo - 1) > 9)
			afe_write_board_offset(b, 7);		/* W6 */
		if (r >= 0 && (unsigned int)(bo - 1) < 9)
			bo = bmu_read_data();			/* write dropped, read fails */
	} else {
		fbo = bmu_read_data();
		if ((unsigned int)(fbo - bo + 2) > 4)
			afe_write_board_offset(b, fbo);		/* W6 */
	}
	afe_read_board_offset(b, &bo);
	if ((unsigned int)(bo - 1) > 9)
		afe_write_board_offset(b, 7);			/* W6 */
	afe_read_board_offset(b, &bo);
}

/* resume with no charger */
void bmu_wake_up_chip(struct oz8806_bmu *b)
{
	int dis, k, c = 0, est;
	long long dt;
	u8 op;

	dis = oz8806_hw_adapter(b) == 0 || BI->curr <= GG->dis_th;
	b->resumed = 1;
	b->now_sec = oz8806_hw_now_sec(b);
	dt = b->now_sec - b->last_sec;
	oz8806_wakeup_full_power(b);				/* W1 */
	if (afe_register_read_byte(b, OZ_REG_OP_CTRL, &op) < 0 || (op & 0x40))
		afe_register_write_byte(b, OZ_REG_OP_CTRL, 0x20);	/* W4 */
	if (BI->soc > 99) {
		BI->soc = 100;
		BI->rc = imin(BI->rc, GG->fcc + GG->fcc / 100 - 1);
	}
	afe_read_current(b, &BI->curr);
	afe_read_cell_volt(b, &BI->volt);
	for (k = 0; k < 3; k++) {
		if (afe_read_car(b, &c) >= 0) {
			if (c != 0 && (dis || c >= 0))
				break;
			b->car_zero_cnt++;
		}
	}
	/* port guard: no earlier poll, a clock step back, or a very long sleep */
	if (b->last_sec == 0 || dt < 0)
		dt = 60;
	else if (dt > 864000)
		dt = 864000;				/* 10 days keeps dt * 500 in an int */
	est = dis ? (int)dt * -12 : (int)dt * 500;		/* expected change x1000 */
	if (iabs(BI->car - c) > CFG->design_mah * 10 / 100 && (c - BI->car) * 1000 - est < 0) {
		c = BI->car + est / 1000;
		afe_write_car(b, c);				/* W7 */
	}
	if (c < 1) {
		GG->b6 = GG->b7 = 0;
		if (c < 0) {
			if (BI->volt < CFG->cv_mv - 50)
				c = BI->soc < 1 ? BI->rc : BI->soc * GG->fcc / 100 - 1;
			else
				c = GG->fcc - 1;
			afe_write_car(b, c);			/* W7 */
			BI->car = BI->prev_car = c;
		}
		if (dis) {
			int s;

			check_shutdwon_voltage(b);
			s = calculate_soc_result(b);
			if (s < BI->soc) {
				BI->soc = s;
				BI->rc = BI->car = BI->prev_car = s * GG->fcc / 100;
				afe_write_car(b, BI->rc);	/* W7 */
			}
			if (BI->soc > 0)
				return;
			if (BI->volt > CFG->end_volt_mv) {
				BI->soc = 1;
				BI->rc = GG->fcc / 100;
				GG->dis_end = 0;
				return;
			}
			discharge_end_process(b);		/* W7 */
			return;
		}
	} else if (dis) {
		if (c > BI->car) {
			c = 2 * BI->car - c;
			afe_write_car(b, c);			/* W7: no rise while discharging */
		}
		return;
	}
	if (c < BI->car) {
		c = 2 * BI->car - c;
		afe_write_car(b, c);				/* W7: no fall while charging */
	}
}

void bmu_power_down_chip(struct oz8806_bmu *b)
{
	afe_read_cell_temp(b, &BI->temp);
	afe_read_cell_volt(b, &BI->volt);
	afe_read_current(b, &BI->curr);
	afe_read_car(b, &BI->car);
	oz8806_chip_sleep(b);					/* W5 */
}

/* ---- reading checks and the logger sample, not in stock ---- */

/* 1 if the readings of the last poll can be reported: the voltage and current
 * were read and the voltage is a cell voltage, and the chip answers and isn't
 * asleep (OP_CTRL bit 6). Also flags readings stuck while current flows
 * (volt_frozen: the same raw voltage and current words 3 polls in a row).
 * Clears the fresh flags, so the next check needs new reads. */
int oz8806_reading_valid(struct oz8806_bmu *b)
{
	int ok = b->volt_fresh && b->curr_fresh &&
		 BI->volt >= OZ8806_VALID_MIN_MV && BI->volt <= OZ8806_VALID_MAX_MV;
	u8 op = 0;

	if (b->wake_pending) {
		if ((int)oz8806_hw_power_on_ms(b) < 2250)
			ok = 0;
		else
			b->wake_pending = 0;
	}
	if (afe_register_read_byte(b, OZ_REG_OP_CTRL, &op) < 0) {
		ok = 0;
	} else {
		b->asleep = !!(op & 0x40);
		if (b->asleep)
			ok = 0;
	}
	if (ok && iabs(BI->curr) > OZ8806_FROZEN_MA && b->volt_raw == b->prev_volt_raw &&
	    b->curr_raw == b->prev_curr_raw)
		b->volt_same++;
	else
		b->volt_same = 0;
	b->prev_volt_raw = ok ? b->volt_raw : -1;
	b->prev_curr_raw = ok ? b->curr_raw : -1;
	b->volt_frozen = b->volt_same >= 2;
	b->volt_fresh = b->curr_fresh = 0;
	return ok;
}

/* before bmulib init: read the voltage and current and check them. The wake
 * runs in both builds: stock's own wakes (W2, W4) are in init, which waits
 * for this check to pass, so an asleep chip would never get there. */
int oz8806_check_chip(struct oz8806_bmu *b)
{
	b->volt_fresh = b->curr_fresh = 0;
	oz8806_wake_if_asleep(b);
	afe_read_cell_volt(b, &BI->volt);
	afe_read_current(b, &BI->curr);
	return oz8806_reading_valid(b);
}

/* one bmu_polling_loop, then whether its readings are valid */
int oz8806_poll(struct oz8806_bmu *b)
{
	b->volt_fresh = b->curr_fresh = 0;
#ifndef CONFIG_OZ8806_WRITE
	oz8806_wake_if_asleep(b);
#endif
	bmu_polling_loop(b);
	return oz8806_reading_valid(b);
}

/* data_ok: set by a valid poll once init is done, cleared after
 * OZ8806_BAD_POLLS invalid polls in a row. valid < 0: no poll ran.
 * Returns 1 when data_ok changed. */
int oz8806_track_valid_core(struct oz8806_bmu *b, int valid)
{
	int was = b->data_ok;

	if (valid < 0)
		return 0;
	if (valid) {
		b->bad_polls = 0;
		if (GG->init_ok)
			b->data_ok = 1;
	} else if (++b->bad_polls >= OZ8806_BAD_POLLS) {
		b->data_ok = 0;
	}
	return was != b->data_ok;
}

/* check_shutdwon_voltage() and the driver's step-down act below this */
int oz8806_shutdown_mv(struct oz8806_bmu *b)
{
	return CFG->end_volt_mv - 50;
}

/* SOC to report: 0 % only once the voltage is at or below the shutdown
 * voltage, else 1 %, so a drifted counter can't shut down a battery that
 * isn't empty. */
int oz8806_report_soc(struct oz8806_bmu *b)
{
	if (BI->soc < 1 && BI->volt > oz8806_shutdown_mv(b))
		return 1;
	return BI->soc;
}

/* Fresh voltage, current and CAR for the logger, outside the polls. Only the
 * readings and the counted charge change, not the level. */
int oz8806_sample(struct oz8806_bmu *b, struct oz8806_sample *s)
{
	int car = 0, ocv = 0, r;

	r = afe_read_cell_volt(b, &BI->volt);
	if (r >= 0)
		r = afe_read_current(b, &BI->curr);
	if (r >= 0)
		r = afe_read_car(b, &car);
	if (r < 0)
		return r;
	if (afe_read_ocv_volt(b, &ocv) < 0)
		ocv = -1;
	s->t_s = oz8806_hw_boottime_sec(b);
	s->volt = BI->volt;
	s->curr = BI->curr;
	s->chip_ocv = ocv;
	s->temp = BI->temp;
	s->soc = BI->soc;
	s->rc = BI->rc;
	s->cc_uah = b->cc_uah;
	s->rest_s = (int)(b->rest_prev_t - b->rest_t0);
	return 0;
}
