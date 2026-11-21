/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the 4.14 display port: the MM PM QoS driver (mmdvfs/mmdvfs_pmqos.c)
 * is left out, as 5.10 has no MM bandwidth PM QoS classes. The calls SMI and
 * mmdvfs make do nothing.
 */
#ifndef __MMDVFS_PMQOS_H__
#define __MMDVFS_PMQOS_H__

#include <linux/errno.h>
#include <linux/list.h>
#include <linux/plist.h>
#include <linux/pm_qos.h>
#include <linux/types.h>

/* T's request layout, used by smi_ostd_update(). */
struct mm_qos_request {
	struct plist_node owner_node;	/* To update all master once */
	struct list_head larb_node;	/* To update larb setting */
	struct list_head port_node;	/* To update ostd in the same port */
	u32 master_id;	/* larb and port combination */
	u32 bw_value;	/* Master data BW */
	u32 hrt_value;	/* Master hrt BW */
	u32 ostd;	/* Master ostd */
	u32 comp_type;	/* compression type */
	bool init;	/* initialized check */
	bool updated;	/* update check */
	struct pm_qos_request qos_request;	/* EMI setting */
};

static inline void mmdvfs_set_max_camera_hrt_bw(u32 bw)
{
}

static inline int mmdvfs_qos_force_step(int step)
{
	return 0;
}

static inline void mmdvfs_qos_enable(bool enable)
{
}

/* T's size of the freq_steps array mmdvfs_qos_get_freq_steps() fills. */
#define MAX_FREQ_STEP 6

/*
 * Stock fills the steps from the DT mmdvfs tables in mmdvfs_pmqos.c. Without
 * it there are none; CMDQ logs the failure and caps MDP at T's 700.
 */
static inline int mmdvfs_qos_get_freq_steps(u32 pm_qos_class, u64 *freq_steps,
					    u32 *step_size)
{
	return -ENODEV;
}

/*
 * Camera port: the MM QoS calls the camera drivers make
 * (ISP, seninf, JPEG, CCU). Requests are dropped, so the engines run at their
 * default clocks and bandwidth. T's names and BW_COMP_* values.
 */
enum {
	BW_COMP_NONE = 0,
	BW_COMP_DEFAULT,
	BW_COMP_VENC,
	BW_COMP_END
};

static inline s32 mm_qos_add_request(struct plist_head *owner_list,
				     struct mm_qos_request *req, u32 master_id)
{
	return 0;
}

static inline s32 mm_qos_set_request(struct mm_qos_request *req, u32 bw_value,
				     u32 hrt_value, u32 comp_type)
{
	return 0;
}

static inline s32 mm_qos_set_bw_request(struct mm_qos_request *req,
					u32 bw_value, s32 comp_type)
{
	return 0;
}

static inline s32 mm_qos_set_hrt_request(struct mm_qos_request *req,
					 u32 hrt_value)
{
	return 0;
}

static inline void mm_qos_update_all_request(struct plist_head *owner_list)
{
}

static inline void mm_qos_remove_all_request(struct plist_head *owner_list)
{
}

static inline void mm_qos_update_all_request_zero(struct plist_head *owner_list)
{
}

enum mmdvfs_limit_source {
	MMDVFS_LIMIT_THERMAL = 0,
	MMDVFS_LIMIT_CAM,
};

/* No MM DVFS steps without mmdvfs_pmqos.c, so no current freq either. */
static inline u64 mmdvfs_qos_get_freq(u32 pm_qos_class)
{
	return 0;
}

static inline void mmdvfs_qos_limit_config(u32 pm_qos_class, u32 limit_value,
					   enum mmdvfs_limit_source source)
{
}

#endif /* __MMDVFS_PMQOS_H__ */
