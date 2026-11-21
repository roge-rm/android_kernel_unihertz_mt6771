// SPDX-License-Identifier: GPL-2.0
/*
 * DPMAIF stand-ins for MT6771. MD_GENERATION 6293 never sets DPMAIF in
 * hif_flag and ccci_hif[DPMAIF_HIF_ID] stays NULL, so ccci_hif.c only
 * reaches these through branches that can't run here. They fail with
 * -ENODEV and do nothing.
 *
 * Also the one call modem_sys1.c makes into net_speed_monitor.c, which only
 * DPMAIF starts: the "net_speed" sysfs toggle flips a flag nothing reads.
 */
#include <linux/errno.h>
#include "ccci_hif_dpmaif.h"

int ccci_dpmaif_hif_init(unsigned char hif_id, unsigned char md_id)
{
	return -ENODEV;
}

int dpmaif_start(unsigned char hif_id)
{
	return -ENODEV;
}

int dpmaif_stop(unsigned char hif_id)
{
	return -ENODEV;
}

void dpmaif_stop_hw(void)
{
}

static int s_show_speed_inf;
int mtk_ccci_toggle_net_speed_log(void)
{
	s_show_speed_inf = !s_show_speed_inf;
	return s_show_speed_inf;
}
