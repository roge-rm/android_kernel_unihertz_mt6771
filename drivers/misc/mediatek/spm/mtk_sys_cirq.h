/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6771 SYS_CIRQ and the GIC mask around SPM suspend (mtk_sys_cirq.c), for
 * mtk_spm_suspend.c only. Everything but mtk_cirq_init runs in suspend enter
 * on CPU0 with IRQs off.
 */
#ifndef __MTK_SYS_CIRQ_H__
#define __MTK_SYS_CIRQ_H__

#include <linux/types.h>

/* An IRQ named by a DT node, looked up at run time without making a mapping */
struct mtk_dt_irq {
	struct irq_domain *domain;
	unsigned long hwirq;
};

int mtk_dt_irq_parse(const char *compatible, int index, struct mtk_dt_irq *out);
unsigned int mtk_dt_irq_virq(const struct mtk_dt_irq *dt);

int mtk_cirq_init(void);
bool mtk_cirq_ready(void);

/* T's mt_irq_mask_all, mt_irq_unmask_for_sleep_ex and the mt6771 edge list */
void mtk_gic_mask_all_for_sleep(void);
void mtk_gic_unmask_for_sleep(unsigned int virq);
void mtk_gic_unmask_edge_trig_irqs(void);
void mtk_gic_mask_restore(void);

/* T's fast clone and flush */
void mtk_cirq_clone_gic(void);
void mtk_cirq_enable(void);
unsigned int mtk_cirq_flush(void);
void mtk_cirq_disable(void);

#endif
