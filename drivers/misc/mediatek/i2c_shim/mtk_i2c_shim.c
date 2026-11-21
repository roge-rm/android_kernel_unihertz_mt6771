// SPDX-License-Identifier: GPL-2.0
/*
 * The MTK I2C extension calls of T's i2c-mtk.c over mainline i2c-mt65xx, for
 * the 4.14 camera drivers. Prototypes in
 * drivers/i2c/busses/i2c-mtk.h.
 */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/i2c.h>

#include "../../../i2c/busses/i2c-mtk.h"

/*
 * T sets the ext flags and the timing for this one transfer. Here the bus
 * runs at its DT clock-frequency (400 kHz on both camera buses, as T's
 * default) and I2C_A_FILTER_MSG is dropped: i2c-mt65xx logs a NACK at debug
 * level only. Returns the number of messages sent or a negative errno, as T.
 */
int mtk_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num,
		     u32 ext_flag, u32 timing)
{
	return i2c_transfer(adap, msgs, num);
}
EXPORT_SYMBOL(mtk_i2c_transfer);

/*
 * Hardware trigger (buffer mode) hands the controller to the ISP. T returns -1
 * on a bus without mediatek,buffermode_used, and no bus in the stock DT has
 * it, so these return -1 as stock does on every bus.
 */
int hw_trig_i2c_enable(struct i2c_adapter *adap)
{
	return -1;
}
EXPORT_SYMBOL(hw_trig_i2c_enable);

int hw_trig_i2c_disable(struct i2c_adapter *adap)
{
	return -1;
}
EXPORT_SYMBOL(hw_trig_i2c_disable);

int hw_trig_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			 int num)
{
	return -1;
}
EXPORT_SYMBOL(hw_trig_i2c_transfer);

/*
 * CCU trigger: the CCU drives the channel at ch_offset itself. T's
 * i2c_ccu_enable() returns its dummy transfer's message count, 1, on success;
 * T keeps the clocks on until i2c_ccu_disable().
 */
int i2c_ccu_enable(struct i2c_adapter *adap, u16 ch_offset)
{
	int ret = mtk_i2c_ccu_enable(adap, ch_offset);

	return ret ? ret : 1;
}
EXPORT_SYMBOL(i2c_ccu_enable);

int i2c_ccu_disable(struct i2c_adapter *adap)
{
	return mtk_i2c_ccu_disable(adap);
}
EXPORT_SYMBOL(i2c_ccu_disable);
