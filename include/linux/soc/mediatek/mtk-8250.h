/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The 8250_mtk sleep request and wake handshake, stock's
 * mtk8250_request_to_sleep and mtk8250_request_to_wakeup, for the SPM
 * suspend path. Called on CPU0 with IRQs off; 0 or -EBUSY.
 */
#ifndef __LINUX_SOC_MEDIATEK_MTK_8250_H__
#define __LINUX_SOC_MEDIATEK_MTK_8250_H__

#include <linux/kconfig.h>

#if IS_BUILTIN(CONFIG_SERIAL_8250_MT6577)
int mtk8250_request_to_sleep(void);
int mtk8250_request_to_wakeup(void);
#else
/* No built-in 8250_mtk, so no UART for the handshake */
static inline int mtk8250_request_to_sleep(void)
{
	return 0;
}

static inline int mtk8250_request_to_wakeup(void)
{
	return 0;
}
#endif

#endif
