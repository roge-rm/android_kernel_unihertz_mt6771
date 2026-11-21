/* SPDX-License-Identifier: GPL-2.0 */
/* Stub for the 4.14 display port: MET is left out, every call does nothing. */
/* The backlight tag calls come from the leds driver's own weak functions. */

#ifndef MET_DRV
#define MET_DRV

#include <linux/types.h>

#define met_tag_init() ({ 0; })
#define met_tag_uninit() ({ 0; })
#define met_tag_start(id, name) ({ 0; })
#define met_tag_end(id, name) ({ 0; })
#define met_tag_async_start(id, name, cookie) ({ 0; })
#define met_tag_async_end(id, name, cookie) ({ 0; })
#define met_tag_oneshot(id, name, value) ({ 0; })
#define met_tag_userdata(pData) ({ 0; })
#define met_tag_dump(id, name, data, length) ({ 0; })
#define met_tag_disable(id) ({ 0; })
#define met_tag_enable(id) ({ 0; })
#define met_set_dump_buffer(size) ({ 0; })
#define met_save_dump_buffer(pathname) ({ 0; })
#define met_save_log(pathname) ({ 0; })
#define met_record_on() ({ 0; })
#define met_record_off() ({ 0; })
#define met_show_bw_limiter() ({ 0; })
#define met_reg_bw_limiter() ({ 0; })
#define met_show_clk_tree() ({ 0; })
#define met_reg_clk_tree() ({ 0; })
#define met_show_pmic_info(RegNum, pmic_reg) ({ 0; })

#define met_ccf_clk_enable(clk) ({ 0; })
#define met_ccf_clk_disable(clk) ({ 0; })
#define met_ccf_clk_set_rate(clk, top) ({ 0; })
#define met_ccf_clk_set_parent(clk, parent) ({ 0; })
#define met_fh_print_dds(pll_id, dds_value) ({ 0; })

static inline void met_mmsys_event_gce_thread_begin(ulong thread_no,
		ulong task_handle, ulong engineFlag, void *pCmd, ulong size)
{
}

static inline void met_mmsys_event_gce_thread_end(ulong thread_no,
		ulong task_handle, ulong engineFlag)
{
}

static inline void met_mmsys_event_disp_sof(int mutex_id)
{
}

static inline void met_mmsys_event_disp_mutex_eof(int mutex_id)
{
}

static inline void met_mmsys_event_disp_ovl_eof(int ovl_id)
{
}

static inline void met_mmsys_config_isp_base_addr(unsigned long *isp_reg_list)
{
}

static inline void met_mmsys_event_isp_pass1_begin(int sensor_id)
{
}

static inline void met_mmsys_event_isp_pass1_end(int sensor_id)
{
}

#endif /* MET_DRV */
