/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RAM_POCKET_DIAGNOSTIC_H
#define _RAM_POCKET_DIAGNOSTIC_H

/*
 * One measured firmware profile. Never reinterpret the vendor tail as console.
 * pmsg is off or 64 KiB (stock's size), taken from the dump records in the prefix.
 */
static inline bool ramoops_pocket_layout_ok(const struct ramoops_platform_data *p)
{
	return p->mem_address == 0x54410000ULL && p->mem_size == 0x8f000 &&
		p->record_size == 0x1000 && p->console_size == 0x40000 &&
		!p->ftrace_size && (!p->pmsg_size || p->pmsg_size == 0x10000) &&
		!p->ecc_info.ecc_size && !p->flags && !p->mem_type;
}

static inline bool ramoops_pocket_reservation_ok(u64 base, u64 size,
						bool no_map, bool reusable)
{
	return base == 0x54410000ULL && size == 0xe0000 &&
		no_map && !reusable;
}
#endif
