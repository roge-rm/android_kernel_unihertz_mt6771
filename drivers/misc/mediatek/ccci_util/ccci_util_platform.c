// SPDX-License-Identifier: GPL-2.0
/*
 * Two calls ccci makes that 4.14 had outside ccci_util.
 *
 * vmap_reserved_mem, from T's memory-amms.c, for ccci_map_phy_addr. The
 * modem regions LK reserves have no no-map, so they keep their struct pages
 * and pfn_valid() is true; ccci maps them write-combine through vmap, as
 * 4.14 did. A region with any page outside RAM is refused. AMMS itself
 * isn't ported.
 *
 * mt_irq_get_pending, from T's GIC code, for modem_sys1's MD watchdog check
 * at power off. It reads the pending bit through the irqchip.
 */
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/vmalloc.h>
#include <mt-plat/mtk_meminfo.h>

void *vmap_reserved_mem(phys_addr_t start, phys_addr_t size, pgprot_t prot)
{
	unsigned long page_count = DIV_ROUND_UP(size, PAGE_SIZE);
	unsigned long pfn = __phys_to_pfn(start);
	struct page **pages;
	unsigned long i;
	void *vaddr;

	/* Only pages with a struct page can go through vmap. */
	for (i = 0; i < page_count; i++)
		if (!pfn_valid(pfn + i))
			return NULL;
	pages = vmalloc(array_size(page_count, sizeof(*pages)));
	if (!pages)
		return NULL;
	for (i = 0; i < page_count; i++)
		pages[i] = pfn_to_page(pfn + i);
	vaddr = vmap(pages, page_count, VM_MAP, prot);
	vfree(pages);
	return vaddr;
}
EXPORT_SYMBOL(vmap_reserved_mem);

/* Linux IRQ number in, 1 if pending, 0 if not or unreadable */
unsigned int mt_irq_get_pending(unsigned int irq)
{
	bool pending = false;

	if (irq_get_irqchip_state(irq, IRQCHIP_STATE_PENDING, &pending))
		return 0;
	return pending;
}
