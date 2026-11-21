/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

/*
 * The battery calls as the stock Titan Pocket kernel has them: every value
 * comes from the OZ8806 gauge, not from GM3.0, which isn't built. Currents
 * are in mA with charging positive, as oz8806_get_battry_current() gives.
 * battery_get_bat_avg_voltage() is left out with GM3.0 (stock read
 * gauge_get_nag_vbat(), and nothing here calls it).
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/power/oz8806.h>
#include <mt-plat/mtk_battery.h>
#include <mt-plat/mtk_boot.h>

extern int pmic_get_vbus(void);
extern bool pmic_is_battery_exist(void);
extern bool is_kernel_power_off_charging(void);

signed int battery_get_bat_voltage(void)
{
	return oz8806_get_battery_voltage();
}

signed int battery_get_bat_current(void)
{
	return abs(oz8806_get_battry_current());
}

signed int battery_get_bat_current_mA(void)
{
	return abs(oz8806_get_battry_current()) / 10;
}

signed int battery_get_soc(void)
{
	return oz8806_get_soc();
}

signed int battery_get_uisoc(void)
{
	return oz8806_get_soc();
}

signed int battery_get_bat_temperature(void)
{
	return oz8806_get_charger_temp();
}

signed int battery_get_ibus(void)
{
	return oz8806_get_battry_current();
}

signed int battery_get_vbus(void)
{
	return pmic_get_vbus();
}

signed int battery_get_bat_avg_current(void)
{
	return oz8806_get_battry_current();
}

signed int battery_get_bat_uisoc(void)
{
	return oz8806_get_soc();
}

signed int battery_get_bat_soc(void)
{
	return oz8806_get_soc();
}

bool battery_get_bat_current_sign(void)
{
	return oz8806_get_battry_current() > 0;
}

int get_ui_soc(void)
{
	return oz8806_get_soc();
}

signed int battery_meter_get_battery_current(void)
{
	return abs(oz8806_get_battry_current());
}

bool battery_meter_get_battery_current_sign(void)
{
	return oz8806_get_battry_current() > 0;
}

signed int battery_meter_get_battery_temperature(void)
{
	return oz8806_get_charger_temp();
}

signed int battery_meter_get_charger_voltage(void)
{
	return pmic_get_vbus();
}

unsigned long BAT_Get_Battery_Current(int polling_mode)
{
	return (long)oz8806_get_battry_current();
}

unsigned long BAT_Get_Battery_Voltage(int polling_mode)
{
	return (long)oz8806_get_battery_voltage();
}

unsigned int bat_get_ui_percentage(void)
{
	return oz8806_get_soc();
}

unsigned int battery_get_is_kpoc(void)
{
	return is_kernel_power_off_charging() & 1;
}

bool battery_is_battery_exist(void)
{
	return pmic_is_battery_exist();
}

void wake_up_bat(void)
{
}
EXPORT_SYMBOL(wake_up_bat);

/* GM25 only */
int en_intr_VBATON_UNDET(int en)
{
	return 0;
}

int reg_VBATON_UNDET(void (*callback)(void))
{
	return 0;
}
