// SPDX-License-Identifier: GPL-2.0
/*
 * Weak stand-ins for the two eccci port_net calls ccmni makes, so ccmni links
 * before eccci is ported. eccci's port/port_net.c defines both for real and
 * overrides these at link time. They act as port_net does when the port
 * isn't found: no queue to flush, nothing to set up.
 */
#include <linux/compiler.h>
#include <linux/printk.h>
#include <mt-plat/mtk_ccci_common.h>

int __weak mtk_ccci_handle_port_list(int status, char *name)
{
	return -1;
}

void __weak mtk_ccci_net_port_init(char *name)
{
	pr_info_once("[ccmni] no eccci net port for %s\n", name);
}
