/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The MTK I2C extension calls the 4.14 camera drivers make, over mainline
 * i2c-mt65xx. T's names, prototypes and flag values; the
 * code is in drivers/misc/mediatek/i2c_shim. T's camera Makefiles add
 * drivers/i2c/busses to their include path, so they find this header as is.
 *
 * mtk_i2c_transfer() is a plain i2c_transfer(): the bus speed comes from the
 * DT clock-frequency and the extension flags are dropped. The hardware
 * trigger calls return -1, as T does on a bus without buffer mode (no stock
 * bus has one). i2c_ccu_enable() and i2c_ccu_disable() are i2c-mt65xx's CCU
 * channel calls (i2c-mt65xx-ccu.h), which also give the
 * CCU driver and imgsensor the controller id and the CCU command page that
 * T reads from its struct mt_i2c.
 */
#ifndef __I2C_MTK_H__
#define __I2C_MTK_H__

#include <linux/i2c.h>
#include <linux/types.h>

#include "i2c-mt65xx-ccu.h"

/* T's largest DMA transfer, imgsensor's burst write length. */
#define MAX_DMA_TRANS_SIZE	4096

/* T's controller id bound, the size of the CCU's per-controller tables */
#define I2C_MAX_CHANNEL		10

/* ext_flag bit for mtk_i2c_transfer(), T's value: no error log on a NACK */
#define I2C_A_FILTER_MSG	0x00000001

int mtk_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num,
		     u32 ext_flag, u32 timing);
int hw_trig_i2c_enable(struct i2c_adapter *adap);
int hw_trig_i2c_disable(struct i2c_adapter *adap);
int hw_trig_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			 int num);
int i2c_ccu_enable(struct i2c_adapter *adap, u16 ch_offset);
int i2c_ccu_disable(struct i2c_adapter *adap);

#endif /* __I2C_MTK_H__ */
