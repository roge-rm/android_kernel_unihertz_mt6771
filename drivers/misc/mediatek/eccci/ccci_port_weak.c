// SPDX-License-Identifier: GPL-2.0
/*
 * Weak stand-ins for the port layer (port/port_proxy.c and port_ipc.c),
 * which comes later and overrides these at link time.
 * With no port layer there are no ports: the critical user calls return
 * 0 (no user open), the others -ENODEV or NULL, and nothing for the void
 * calls. A received skb is left to the caller, which frees it on -ENODEV.
 */
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include "ccci_core.h"
#include "ccci_port.h"
#include "port/port_t.h"

/* Time zone the RIL sends through port_ipc; ccci_modem.c reads it for MD runtime data. */
int __weak current_time_zone;

int __weak ccci_port_init(int md_id)
{
	pr_notice("[ccci] md%d: no port layer, ports not set up\n", md_id + 1);
	return -1;
}

void __weak ccci_port_dump_status(int md_id)
{
}

void __weak ccci_port_md_status_notify(int md_id, unsigned int state)
{
}

void __weak ccci_port_queue_status_notify(int md_id, int hif_id, int qno,
	int dir, unsigned int state)
{
}

int __weak ccci_port_recv_skb(int md_id, int hif_id, struct sk_buff *skb,
	unsigned int flag)
{
	return -ENODEV;
}

int __weak ccci_port_check_critical_user(int md_id)
{
	return 0;
}

int __weak ccci_port_get_critical_user(int md_id, unsigned int user_id)
{
	return 0;
}

int __weak ccci_port_send_msg_to_md(int md_id, int ch, unsigned int msg,
	unsigned int resv, int blocking)
{
	return -ENODEV;
}

void __weak ccci_port_set_traffic_flag(int md_id, unsigned int dump_flag)
{
}

struct port_t * __weak port_get_by_channel(int md_id, enum CCCI_CH ch)
{
	return NULL;
}
