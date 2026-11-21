/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CCU channel calls of i2c-mt65xx on a multi-channel MT6771 bus
 *. Each returns -ENODEV for an adapter that isn't an
 * i2c-mt65xx bus.
 *
 * mtk_i2c_hw_id: the vendor DT controller id ("id"), -1 if the node has none.
 * mtk_i2c_ccu_dma_buf: the CCU's coherent command page, PAGE_SIZE bytes.
 * mtk_i2c_ccu_enable: clocks on and the channel at ch_offset set up for the
 *   CCU, as the vendor i2c_ccu_enable(). 0, or -EINVAL for a bus with no AP
 *   channel or a bad offset, -EBUSY if already on.
 * mtk_i2c_ccu_disable: waits for a transfer in flight, clocks off.
 * mtk_i2c_ccu_writew: a register write in the bus window while the CCU is on.
 */
#ifndef __I2C_MT65XX_CCU_H__
#define __I2C_MT65XX_CCU_H__

#include <linux/types.h>

struct i2c_adapter;

int mtk_i2c_hw_id(struct i2c_adapter *adap);
int mtk_i2c_ccu_dma_buf(struct i2c_adapter *adap, void **va, dma_addr_t *pa);
int mtk_i2c_ccu_enable(struct i2c_adapter *adap, u16 ch_offset);
int mtk_i2c_ccu_disable(struct i2c_adapter *adap);
int mtk_i2c_ccu_writew(struct i2c_adapter *adap, u16 val, u16 offset);

#endif /* __I2C_MT65XX_CCU_H__ */
