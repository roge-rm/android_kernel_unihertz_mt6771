// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 SYS_CIRQ for SPM deep suspend, T's cirq/mtk_sys_cirq.c with
 * CONFIG_FAST_CIRQ_CLONE_FLUSH (mt6771 selects it) and the GIC helpers from
 * T's ext_gic/mtk-gic-v3-extend.c that spm_go_to_sleep uses:
 * - mask every SPI in the distributor, then unmask SPM IRQ0 and stock's
 *   mt6771 edge list (infracfg_ao IRQ 0, kp IRQ 0, mdcldma IRQ 3), as the
 *   stock Image's unmask_edge_trig_irqs_for_cirq
 * - clone those edge IRQs into CIRQ (edge, polarity, mask from the GIC
 *   enable bit) and turn CIRQ on, so an edge while MCUSYS and the GIC are off
 *   is latched
 * - on resume set each latched one pending in the GIC, mask and ack CIRQ,
 *   turn it off and put the GIC enable bits back
 * Nothing touches the hardware until mtk_cirq_init, which only runs when deep
 * suspend is offered. The CIRQ IRQ itself is never requested: CIRQ is only on
 * inside suspend enter, with the GIC masked.
 */
#define pr_fmt(fmt) "spm_cirq: " fmt

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include "mtk_sys_cirq.h"

/* T's mtk_sys_cirq.h register map */
#define CIRQ_STA_BASE		0x000
#define CIRQ_ACK_BASE		0x040
#define CIRQ_MASK_BASE		0x080
#define CIRQ_MASK_SET_BASE	0x0c0
#define CIRQ_MASK_CLR_BASE	0x100
#define CIRQ_SENS_BASE		0x140
#define CIRQ_SENS_SET_BASE	0x180
#define CIRQ_SENS_CLR_BASE	0x1c0
#define CIRQ_POL_BASE		0x200
#define CIRQ_POL_SET_BASE	0x240
#define CIRQ_POL_CLR_BASE	0x280
#define CIRQ_CON		0x300

#define CIRQ_CON_EN_BITS	0
#define CIRQ_CON_EDGE_ONLY_BITS	1
#define CIRQ_CON_FLUSH_BITS	2
#define CIRQ_CON_BITS_MASK	0x7
#define CIRQ_CON_EN		0x1
#define CIRQ_CON_EDGE_ONLY	0x1

#define GIC_PRIVATE_SIGNALS	32

/* The stock DT has 231 CIRQ lines; room for a little more */
#define CIRQ_MAX_REGS		8
/* GICD_ISENABLER words, SGIs and PPIs in word 0 included */
#define GIC_MAX_REGS		32

/* T's mt6771 edge_trig_irqs in spm_module_init (auxadc left out, as stock) */
static const struct {
	const char *compatible;
	int index;
} edge_trig_src[] = {
	{ "mediatek,infracfg_ao", 0 },
	{ "mediatek,kp", 0 },
	{ "mediatek,mdcldma", 3 },
};
#define NF_EDGE_TRIG_IRQS	ARRAY_SIZE(edge_trig_src)

static void __iomem *cirq_base;
static void __iomem *dist_base;
static struct irq_domain *gic_domain;
static u32 cirq_irq_num, cirq_spi_start;
static unsigned int cirq_regs, dist_regs;
static struct mtk_dt_irq edge_trig[NF_EDGE_TRIG_IRQS];
static bool edge_trig_ok[NF_EDGE_TRIG_IRQS];

/* Rebuilt by each clone: the edge IRQs that have a mapping, and their CIRQ bits */
struct cirq_event {
	u32 cirq;
	u32 gic;
	bool neg;
};
static struct cirq_event events[NF_EDGE_TRIG_IRQS];
static unsigned int nr_events;
static u32 event_mask[CIRQ_MAX_REGS];
static u32 saved_enable[GIC_MAX_REGS];

#define CIRQ_TO_GIC(cirq)	((cirq) + GIC_PRIVATE_SIGNALS + cirq_spi_start)

/* The GIC's hwirq under a Linux IRQ, through the sysirq level; 0 if not on the GIC */
static u32 gic_hwirq(unsigned int virq)
{
	struct irq_data *d = irq_get_irq_data(virq);

	while (d && d->parent_data)
		d = d->parent_data;
	if (!d || d->domain != gic_domain)
		return 0;
	return d->hwirq;
}

int mtk_dt_irq_parse(const char *compatible, int index, struct mtk_dt_irq *out)
{
	struct of_phandle_args args;
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np)
		return -ENODEV;
	ret = of_irq_parse_one(np, index, &args);
	of_node_put(np);
	if (ret)
		return ret;
	out->domain = irq_find_host(args.np);
	of_node_put(args.np);
	/* Three cells, SPI only: sysirq's hwirq is the SPI number, the GIC's adds 32 */
	if (!out->domain || args.args_count != 3 || args.args[0] != 0)
		return -EINVAL;
	out->hwirq = args.args[1];
	if (out->domain == gic_domain)
		out->hwirq += GIC_PRIVATE_SIGNALS;
	return 0;
}

/* 0 until a driver or the platform code has mapped it */
unsigned int mtk_dt_irq_virq(const struct mtk_dt_irq *dt)
{
	return dt->domain ? irq_find_mapping(dt->domain, dt->hwirq) : 0;
}

bool mtk_cirq_ready(void)
{
	return cirq_base && dist_base;
}

static void gic_wait_for_rwp(void)
{
	unsigned int i;

	for (i = 0; i < 1000; i++) {
		if (!(readl_relaxed(dist_base + GICD_CTLR) & GICD_CTLR_RWP))
			return;
		udelay(1);
	}
	pr_err("GICD RWP timeout\n");
}

/* T's mt_irq_mask_all: save the SPI enable words, then clear them all */
void mtk_gic_mask_all_for_sleep(void)
{
	unsigned int i;

	for (i = 1; i < dist_regs; i++) {
		saved_enable[i] = readl(dist_base + GICD_ISENABLER + i * 4);
		writel(0xffffffff, dist_base + GICD_ICENABLER + i * 4);
	}
	gic_wait_for_rwp();
}

/* T's mt_irq_unmask_for_sleep_ex */
void mtk_gic_unmask_for_sleep(unsigned int virq)
{
	u32 hw = gic_hwirq(virq);

	if (hw < GIC_PRIVATE_SIGNALS || hw >= dist_regs * 32) {
		pr_err("IRQ %u: no SPI to unmask\n", virq);
		return;
	}
	writel(BIT(hw % 32), dist_base + GICD_ISENABLER + hw / 32 * 4);
	gic_wait_for_rwp();
}

/* The stock Image's unmask_edge_trig_irqs_for_cirq: each of the edge list with a mapping */
void mtk_gic_unmask_edge_trig_irqs(void)
{
	unsigned int i, virq;

	for (i = 0; i < NF_EDGE_TRIG_IRQS; i++) {
		virq = edge_trig_ok[i] ? mtk_dt_irq_virq(&edge_trig[i]) : 0;
		if (virq)
			mtk_gic_unmask_for_sleep(virq);
	}
}

/*
 * T's mt_irq_mask_restore writes the saved words back to ISENABLER; this
 * also clears what the sleep path unmasked, so the GIC ends as it started.
 */
void mtk_gic_mask_restore(void)
{
	unsigned int i;

	for (i = 1; i < dist_regs; i++) {
		writel(~saved_enable[i], dist_base + GICD_ICENABLER + i * 4);
		writel(saved_enable[i], dist_base + GICD_ISENABLER + i * 4);
	}
	gic_wait_for_rwp();
}

/* T's mt_irq_get_pending_vec: 32 GIC pending bits from start_irq on */
static u32 gic_pending_vec(u32 start_irq)
{
	u32 reg = start_irq / 32, shift = start_irq % 32, lo, hi = 0;

	if (reg >= dist_regs)
		return 0;
	lo = readl_relaxed(dist_base + GICD_ISPENDR + reg * 4);
	if (!shift)
		return lo;
	if (reg + 1 < dist_regs)
		hi = readl_relaxed(dist_base + GICD_ISPENDR + (reg + 1) * 4);
	return (lo >> shift) | (hi << (32 - shift));
}

/* T's mt_cirq_ack_all: ack all but what is pending in the GIC and unmasked here */
static void cirq_ack_all(void)
{
	u32 pend, mask;
	unsigned int i;

	for (i = 0; i < cirq_regs; i++) {
		pend = gic_pending_vec(CIRQ_TO_GIC(i * 32));
		mask = readl(cirq_base + CIRQ_MASK_BASE + i * 4);
		writel_relaxed(~pend | mask, cirq_base + CIRQ_ACK_BASE + i * 4);
	}
	mb();
}

static void cirq_mask_all(void)
{
	unsigned int i;

	for (i = 0; i < cirq_regs; i++)
		writel_relaxed(0xffffffff, cirq_base + CIRQ_MASK_SET_BASE + i * 4);
	mb();
}

static void cirq_write_bit(u32 cirq, u32 set_base, u32 clr_base, bool set)
{
	writel(BIT(cirq % 32), cirq_base + (set ? set_base : clr_base) + cirq / 32 * 4);
}

/* T's collect_all_wakeup_events: the edge list that has a mapping, in CIRQ's range */
static void cirq_collect_events(void)
{
	unsigned int i, virq, type;
	u32 hw;

	nr_events = 0;
	memset(event_mask, 0, sizeof(event_mask));
	for (i = 0; i < NF_EDGE_TRIG_IRQS; i++) {
		virq = edge_trig_ok[i] ? mtk_dt_irq_virq(&edge_trig[i]) : 0;
		hw = virq ? gic_hwirq(virq) : 0;
		if (hw < CIRQ_TO_GIC(0) || hw >= CIRQ_TO_GIC(cirq_irq_num))
			continue;
		type = irq_get_trigger_type(virq);
		events[nr_events].gic = hw;
		events[nr_events].cirq = hw - CIRQ_TO_GIC(0);
		/* sysirq inverts low and falling ones, which mt_irq_get_pol_hw reads as set */
		events[nr_events].neg = type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_EDGE_FALLING);
		event_mask[events[nr_events].cirq / 32] |= BIT(events[nr_events].cirq % 32);
		nr_events++;
	}
}

/*
 * T's __cirq_fast_clone: edge only, then each event's polarity, and its mask
 * from the GIC enable bit as the sleep path left it.
 */
void mtk_cirq_clone_gic(void)
{
	unsigned int i;
	u32 en;

	cirq_collect_events();
	for (i = 0; i < cirq_regs; i++)
		if (event_mask[i])
			writel(event_mask[i], cirq_base + CIRQ_SENS_CLR_BASE + i * 4);
	for (i = 0; i < nr_events; i++) {
		const struct cirq_event *e = &events[i];

		/* POL set is positive, clear is negative */
		cirq_write_bit(e->cirq, CIRQ_POL_SET_BASE, CIRQ_POL_CLR_BASE, !e->neg);
		en = readl(dist_base + GICD_ISENABLER + e->gic / 32 * 4) & BIT(e->gic % 32);
		cirq_write_bit(e->cirq, CIRQ_MASK_CLR_BASE, CIRQ_MASK_SET_BASE, en);
	}
}

/* T's mt_cirq_enable: ack, then EN and EDGE_ONLY */
void mtk_cirq_enable(void)
{
	u32 st;

	cirq_ack_all();
	st = readl(cirq_base + CIRQ_CON);
	st |= (CIRQ_CON_EN << CIRQ_CON_EN_BITS) |
	      (CIRQ_CON_EDGE_ONLY << CIRQ_CON_EDGE_ONLY_BITS);
	writel(st & CIRQ_CON_BITS_MASK, cirq_base + CIRQ_CON);
}

/*
 * T's cirq_fast_sw_flush, then mask and ack all: every latched event goes
 * pending in the GIC. Returns how many.
 */
unsigned int mtk_cirq_flush(void)
{
	unsigned int i, n = 0, bit;
	unsigned long pend;
	u32 cirq, hw;

	for (i = 0; i < cirq_regs; i++) {
		if (!event_mask[i])
			continue;
		pend = readl(cirq_base + CIRQ_STA_BASE + i * 4) & event_mask[i];
		for_each_set_bit(bit, &pend, 32) {
			cirq = i * 32 + bit;
			hw = CIRQ_TO_GIC(cirq);
			writel(BIT(hw % 32), dist_base + GICD_ISPENDR + hw / 32 * 4);
			n++;
		}
	}
	cirq_mask_all();
	cirq_ack_all();
	return n;
}

/* T's mt_cirq_disable */
void mtk_cirq_disable(void)
{
	u32 st = readl(cirq_base + CIRQ_CON);

	st &= ~(CIRQ_CON_EN << CIRQ_CON_EN_BITS);
	writel(st & CIRQ_CON_BITS_MASK, cirq_base + CIRQ_CON);
}

/*
 * T's mt_cirq_init and setup_cirq_settings: map CIRQ and the GIC
 * distributor, read the line count and SPI offset, find stock's edge list in
 * the DT and mask every CIRQ line.
 */
int mtk_cirq_init(void)
{
	struct device_node *np, *gic_np;
	unsigned int i;
	u32 typer;

	gic_np = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!gic_np)
		return -ENODEV;
	gic_domain = irq_find_host(gic_np);
	dist_base = gic_domain ? of_iomap(gic_np, 0) : NULL;
	of_node_put(gic_np);
	if (!dist_base)
		return -ENXIO;
	typer = readl(dist_base + GICD_TYPER);
	dist_regs = min_t(unsigned int, GICD_TYPER_SPIS(typer) / 32, GIC_MAX_REGS);

	np = of_find_compatible_node(NULL, NULL, "mediatek,sys_cirq");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		goto no_cirq;
	}
	if (of_property_read_u32(np, "mediatek,cirq_num", &cirq_irq_num) ||
	    of_property_read_u32(np, "mediatek,spi_start_offset", &cirq_spi_start) ||
	    !cirq_irq_num || DIV_ROUND_UP(cirq_irq_num, 32) > CIRQ_MAX_REGS ||
	    CIRQ_TO_GIC(cirq_irq_num) > dist_regs * 32) {
		pr_err("bad cirq_num or spi_start_offset\n");
		of_node_put(np);
		goto no_cirq;
	}
	cirq_regs = DIV_ROUND_UP(cirq_irq_num, 32);
	cirq_base = of_iomap(np, 0);
	of_node_put(np);
	if (!cirq_base)
		goto no_cirq;

	for (i = 0; i < NF_EDGE_TRIG_IRQS; i++)
		edge_trig_ok[i] = !mtk_dt_irq_parse(edge_trig_src[i].compatible,
						    edge_trig_src[i].index, &edge_trig[i]);
	cirq_mask_all();
	pr_info("%u lines from SPI %u, GIC %u SPIs, edge list %d %d %d\n",
		cirq_irq_num, cirq_spi_start, dist_regs * 32 - GIC_PRIVATE_SIGNALS,
		edge_trig_ok[0], edge_trig_ok[1], edge_trig_ok[2]);
	return 0;

no_cirq:
	iounmap(dist_base);
	dist_base = NULL;
	return -ENODEV;
}
