/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OZ8806 gauge calls, with the stock Titan Pocket names so mtk_battery can
 * call them when it is ported. -1 means not ready, as stock.
 */
#ifndef __LINUX_POWER_OZ8806_H
#define __LINUX_POWER_OZ8806_H

#include <linux/types.h>

struct i2c_client;

int oz8806_get_soc(void);
int oz8806_get_soc_from_ext(void);
int oz8806_get_battry_current(void);	/* mA, charging positive, live read */
int oz8806_get_battery_voltage(void);	/* mV, live read */
int oz8806_get_battery_temp(void);	/* C */
int oz8806_get_charger_temp(void);	/* C, -1 unless the last NTC reading is valid */
int oz8806_get_valid_temp(int *temp_c);	/* 0 and C, or -ENODATA as above */
int oz8806_get_remaincap(void);		/* mAh */
int oz8806_vbus_voltage(void);
int oz8806_get_simulated_temp(void);
int oz8806_get_init_status(void);
int oz8806_gauge_int_done(void);	/* stock gauge_int_done */
int oz8806_get_adapter_status(void);	/* stock get_adapter_status: 0 none, 1 USB, 4 AC */
int oz8806_get_save_capacity(void);
void oz8806_set_save_capacity(int mah);
int oz8806_get_bmu_init_done(void);
struct i2c_client *oz8806_get_client(void);
void oz8806_update(void);
/* the logger's CSV line (see oz8806.c), length or a negative error */
int oz8806_log_line(char *buf, size_t size);

/* For the charger driver, once ported: charger type in the mt_get_charger_type()
 * numbering (0 none, 1 SDP, 2 CDP, others AC) and the charge-done flag. */
void oz8806_set_charger_info(int charger_type, bool charge_done);
/* Battery status from the charger events (POWER_SUPPLY_STATUS_*) */
void oz8806_set_battery_status(int status);
/* The battery supply's hook, called when that status changes */
void oz8806_set_changed_cb(void (*cb)(void *arg), void *arg);

/* For the battery supply: values from the last oz8806_update() */
struct oz8806_report {
	int soc;	/* % */
	int volt;	/* mV */
	int curr;	/* mA */
	int temp;	/* C */
	int temp_ok;	/* 0 when every NTC read failed */
	int rc;		/* mAh */
	int fcc;	/* mAh */
	int status;	/* POWER_SUPPLY_STATUS_* */
};
/* -ENODATA until the gauge has valid readings, and while its polls fail */
int oz8806_get_report(struct oz8806_report *r);

#endif
