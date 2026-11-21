// SPDX-License-Identifier: GPL-2.0
/*
 * The 4.14 MTK PM QoS classes (the enum in pm_qos.h) that the MT6771
 * display, CMDQ MDP, camera, JPEG, vcodec, eccci, GED and SCP drivers ask
 * for. Each class keeps its requests on a plist and their MIN, MAX or SUM,
 * with T's types and defaults (kernel/power/qos.c in the 4.14 tree).
 *
 * One listener, the vcore DVFS backend, is told the class after each change
 * of a class's value. Callers may hold spinlocks or run in IRQ context, so
 * this takes only a raw spinlock and calls the listener outside it; the
 * listener must not sleep and reads the value back with pm_qos_request().
 * With no listener the values are only kept.
 */
#include <linux/kernel.h>
#include <linux/plist.h>
#include <linux/pm_qos.h>
#include <linux/spinlock.h>
#include <linux/string.h>

enum { QOS_AGG_MIN, QOS_AGG_MAX, QOS_AGG_SUM };

struct mtk_qos_class {
	struct pm_qos_constraints c;	/* list and target_value */
	const char *name;
	u8 agg;
	u16 requests;
};

#define MTK_QOS(id, n, a, d)						\
	[id] = {							\
		.c = {							\
			.list = PLIST_HEAD_INIT(mtk_qos[id].c.list),	\
			.target_value = (d),				\
			.default_value = (d),				\
			.no_constraint_value = (d),			\
			.type = PM_QOS_MIN,				\
		},							\
		.name = (n),						\
		.agg = (a),						\
	}

static struct mtk_qos_class mtk_qos[PM_QOS_NUM_CLASSES] = {
	MTK_QOS(PM_QOS_CPU_DMA_LATENCY, "cpu_dma_latency", QOS_AGG_MIN,
		PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_NETWORK_LATENCY, "network_latency", QOS_AGG_MIN,
		PM_QOS_NETWORK_LAT_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_NETWORK_THROUGHPUT, "network_throughput", QOS_AGG_MAX,
		PM_QOS_NETWORK_THROUGHPUT_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_MEMORY_BANDWIDTH, "memory_bandwidth", QOS_AGG_SUM,
		PM_QOS_MEMORY_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_CPU_MEMORY_BANDWIDTH, "cpu_memory_bandwidth", QOS_AGG_SUM,
		PM_QOS_CPU_MEMORY_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_GPU_MEMORY_BANDWIDTH, "gpu_memory_bandwidth", QOS_AGG_SUM,
		PM_QOS_GPU_MEMORY_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_MM_MEMORY_BANDWIDTH, "mm_memory_bandwidth", QOS_AGG_SUM,
		PM_QOS_MM_MEMORY_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_MD_PERI_MEMORY_BANDWIDTH, "md_peri_memory_bandwidth",
		QOS_AGG_SUM, PM_QOS_MD_PERI_MEMORY_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_OTHER_MEMORY_BANDWIDTH, "other_memory_bandwidth",
		QOS_AGG_SUM, PM_QOS_OTHER_MEMORY_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_MM0_BANDWIDTH_LIMITER, "mm0_bandwidth_limiter",
		QOS_AGG_SUM, PM_QOS_MM_BANDWIDTH_LIMITER_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_MM1_BANDWIDTH_LIMITER, "mm1_bandwidth_limiter",
		QOS_AGG_SUM, PM_QOS_MM_BANDWIDTH_LIMITER_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_DDR_OPP, "ddr_opp", QOS_AGG_MIN,
		PM_QOS_DDR_OPP_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_EMI_OPP, "emi_opp", QOS_AGG_MIN,
		PM_QOS_EMI_OPP_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_VCORE_OPP, "vcore_opp", QOS_AGG_MIN,
		PM_QOS_VCORE_OPP_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_VCORE_DVFS_FIXED_OPP, "vcore_dvfs_fixed_opp", QOS_AGG_MIN,
		PM_QOS_VCORE_DVFS_FIXED_OPP_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_SCP_VCORE_REQUEST, "scp_vcore_req", QOS_AGG_MAX,
		PM_QOS_SCP_VCORE_REQUEST_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_POWER_MODEL_DDR_REQUEST, "power_model_ddr_req",
		QOS_AGG_MAX, PM_QOS_POWER_MODEL_DDR_REQUEST_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_POWER_MODEL_VCORE_REQUEST, "power_model_vcore_req",
		QOS_AGG_MAX, PM_QOS_POWER_MODEL_VCORE_REQUEST_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_VCORE_DVFS_FORCE_OPP, "vcore_dvfs_force_opp", QOS_AGG_MIN,
		PM_QOS_VCORE_DVFS_FORCE_OPP_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_DISP_FREQ, "disp_freq", QOS_AGG_MAX,
		PM_QOS_MM_FREQ_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_MDP_FREQ, "mdp_freq", QOS_AGG_MAX,
		PM_QOS_MM_FREQ_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_VDEC_FREQ, "vdec_freq", QOS_AGG_MAX,
		PM_QOS_MM_FREQ_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_VENC_FREQ, "venc_freq", QOS_AGG_MAX,
		PM_QOS_MM_FREQ_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_IMG_FREQ, "img_freq", QOS_AGG_MAX,
		PM_QOS_MM_FREQ_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_CAM_FREQ, "cam_freq", QOS_AGG_MAX,
		PM_QOS_MM_FREQ_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_DPE_FREQ, "dpe_freq", QOS_AGG_MAX,
		PM_QOS_MM_FREQ_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_ISP_HRT_BANDWIDTH, "isp_hrt_bandwidth", QOS_AGG_SUM,
		PM_QOS_ISP_HRT_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_APU_MEMORY_BANDWIDTH, "apu_memory_bandwidth", QOS_AGG_SUM,
		PM_QOS_APU_MEMORY_BANDWIDTH_DEFAULT_VALUE),
	MTK_QOS(PM_QOS_VVPU_OPP, "vvpu_opp", QOS_AGG_MIN,
		PM_QOS_VVPU_OPP_DEFAULT_VALUE),
};

static DEFINE_RAW_SPINLOCK(mtk_qos_lock);
static void (*mtk_qos_listener)(int pm_qos_class);

static bool mtk_qos_valid(int pm_qos_class)
{
	return pm_qos_class > PM_QOS_RESERVED &&
	       pm_qos_class < PM_QOS_NUM_CLASSES && mtk_qos[pm_qos_class].name;
}

/* The class a request was added to, or NULL (never added, removed, or a 5.10 cpu_latency_qos one) */
static struct mtk_qos_class *mtk_qos_of(struct pm_qos_request *req)
{
	int i;

	if (!req || !req->qos)
		return NULL;
	for (i = PM_QOS_RESERVED + 1; i < PM_QOS_NUM_CLASSES; i++)
		if (req->qos == &mtk_qos[i].c)
			return mtk_qos[i].name ? &mtk_qos[i] : NULL;
	return NULL;
}

/* T's pm_qos_get_value; called with mtk_qos_lock held */
static s32 mtk_qos_value(struct mtk_qos_class *q)
{
	struct plist_node *node;
	s32 sum = 0;

	if (plist_head_empty(&q->c.list))
		return q->c.no_constraint_value;
	switch (q->agg) {
	case QOS_AGG_MIN:
		return plist_first(&q->c.list)->prio;
	case QOS_AGG_MAX:
		return plist_last(&q->c.list)->prio;
	default:
		plist_for_each(node, &q->c.list)
			sum += node->prio;
		return sum;
	}
}

/* T's pm_qos_update_target without the mutex and the blocking notifiers */
static void mtk_qos_update(struct mtk_qos_class *q, struct plist_node *node,
			   enum pm_qos_req_action action, s32 value)
{
	void (*listener)(int pm_qos_class);
	unsigned long flags;
	s32 prev, curr;

	if (value == PM_QOS_DEFAULT_VALUE)
		value = q->c.default_value;

	raw_spin_lock_irqsave(&mtk_qos_lock, flags);
	prev = q->c.target_value;
	switch (action) {
	case PM_QOS_REMOVE_REQ:
		plist_del(node, &q->c.list);
		q->requests--;
		break;
	case PM_QOS_UPDATE_REQ:
		plist_del(node, &q->c.list);
		plist_node_init(node, value);
		plist_add(node, &q->c.list);
		break;
	case PM_QOS_ADD_REQ:
		plist_node_init(node, value);
		plist_add(node, &q->c.list);
		q->requests++;
		break;
	}
	curr = mtk_qos_value(q);
	WRITE_ONCE(q->c.target_value, curr);
	raw_spin_unlock_irqrestore(&mtk_qos_lock, flags);

	listener = READ_ONCE(mtk_qos_listener);
	if (prev != curr && listener)
		listener(q - mtk_qos);
}

void pm_qos_add_request(struct pm_qos_request *req, int pm_qos_class,
			s32 value)
{
	/* T warns on a second add; the old stubs took it, so it is ignored */
	if (!req || !mtk_qos_valid(pm_qos_class) || mtk_qos_of(req))
		return;
	req->qos = &mtk_qos[pm_qos_class].c;
	mtk_qos_update(&mtk_qos[pm_qos_class], &req->node, PM_QOS_ADD_REQ,
		       value);
}

void pm_qos_update_request(struct pm_qos_request *req, s32 new_value)
{
	struct mtk_qos_class *q = mtk_qos_of(req);

	/* T warns on an update before add; the old stubs took it */
	if (!q)
		return;
	if (new_value != req->node.prio)
		mtk_qos_update(q, &req->node, PM_QOS_UPDATE_REQ, new_value);
}

void pm_qos_remove_request(struct pm_qos_request *req)
{
	struct mtk_qos_class *q = mtk_qos_of(req);

	if (!q)
		return;
	mtk_qos_update(q, &req->node, PM_QOS_REMOVE_REQ, PM_QOS_DEFAULT_VALUE);
	memset(req, 0, sizeof(*req));
}

int pm_qos_request(int pm_qos_class)
{
	if (!mtk_qos_valid(pm_qos_class))
		return PM_QOS_DEFAULT_VALUE;
	return READ_ONCE(mtk_qos[pm_qos_class].c.target_value);
}

int pm_qos_request_active(struct pm_qos_request *req)
{
	return mtk_qos_of(req) != NULL;
}

unsigned int pm_qos_class_requests(int pm_qos_class)
{
	return mtk_qos_valid(pm_qos_class) ?
		READ_ONCE(mtk_qos[pm_qos_class].requests) : 0;
}

const char *pm_qos_class_name(int pm_qos_class)
{
	return mtk_qos_valid(pm_qos_class) ? mtk_qos[pm_qos_class].name : NULL;
}

void pm_qos_set_class_listener(void (*listener)(int pm_qos_class))
{
	WRITE_ONCE(mtk_qos_listener, listener);
}
