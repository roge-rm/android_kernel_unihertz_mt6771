// SPDX-License-Identifier: GPL-2.0
/*
 * Weak stand-ins for the ccci_util calls eccci makes (LK modem info,
 * reserved memory layout, image header, sysfs and the ccci log buffers).
 * The 4.14 ccci_util port overrides them at link time.
 *
 * Until then get_modem_is_enabled() (weak, ccci_util_dummy.c) returns 0,
 * so the mdcldma probe stops at "modem 1 not enable, exit" before any of
 * these run. Each reports nothing: no memory, no image, zero sizes.
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <mt-plat/mtk_ccci_common.h>

/* ccci_private_log.c: the ccci ring buffers; the legacy printk in each log macro still prints */
int __weak ccci_dump_write(int md_id, int buf_type, unsigned int flag,
	const char *fmt, ...)
{
	return 0;
}

int __weak ccci_event_log(const char *fmt, ...)
{
	return 0;
}

void __weak ccci_util_mem_dump(int md_id, int buf_type, void *start_addr,
	int len)
{
}

/* ccci_util_lib_load_img.c */
int __weak ccci_get_md_check_hdr_inf(int md_id, void *img_inf,
	char post_fix[])
{
	return -1;
}

char * __weak ccci_get_md_info_str(int md_id)
{
	return "";
}

int __weak ccci_init_security(void)
{
	return 0;
}

/* ccci_util_lib_sys.c */
int __weak ccci_sysfs_add_modem(int md_id, void *kobj, void *ktype,
	get_status_func_t get_sta_func, boot_md_func_t boot_func)
{
	return -1;
}

/* ccci_util_broadcast.c */
void __weak inject_md_status_event(int md_id, int event_type, char reason[])
{
}

/* ccci_util_lib_fo.c: what LK passed in the device tree */
void __iomem * __weak ccci_map_phy_addr(phys_addr_t phy_addr,
	unsigned int size)
{
	return NULL;
}

int __weak check_md_type(int data)
{
	return 0;
}

int __weak get_legacy_md_type(int md_id)
{
	return 0;
}

int __weak get_md_img_type(int md_id)
{
	return 0;
}

int __weak get_md_type_from_lk(int md_id)
{
	return 0;
}

int __weak set_modem_support_cap(int md_id, int new_val)
{
	return -1;
}

unsigned int __weak get_mtee_is_enabled(void)
{
	return 0;
}

unsigned int __weak get_wm_bitmap_for_ubin(void)
{
	return 0;
}

unsigned int __weak is_cdma2000_enable(int md_id)
{
	return 0;
}

int __weak get_md_resv_mem_info(int md_id, phys_addr_t *r_rw_base,
	unsigned int *r_rw_size, phys_addr_t *srw_base, unsigned int *srw_size)
{
	if (r_rw_base)
		*r_rw_base = 0;
	if (r_rw_size)
		*r_rw_size = 0;
	if (srw_base)
		*srw_base = 0;
	if (srw_size)
		*srw_size = 0;
	return -1;
}

int __weak get_md_resv_ccb_info(int md_id, phys_addr_t *ccb_data_base,
	unsigned int *ccb_data_size)
{
	*ccb_data_base = 0;
	*ccb_data_size = 0;
	return -1;
}

int __weak get_md_resv_udc_info(int md_id, unsigned int *udc_noncache_size,
	unsigned int *udc_cache_size)
{
	*udc_noncache_size = 0;
	*udc_cache_size = 0;
	return -1;
}

int __weak get_md1_md3_resv_smem_info(int md_id, phys_addr_t *rw_base,
	unsigned int *rw_size)
{
	*rw_base = 0;
	*rw_size = 0;
	return -1;
}

int __weak get_md_resv_csmem_info(int md_id, phys_addr_t *buf_base,
	unsigned int *buf_size)
{
	*buf_base = 0;
	*buf_size = 0;
	return -1;
}

int __weak get_md_cache_region_info(int region_id, unsigned int *buf_base,
	unsigned int *buf_size)
{
	*buf_base = 0;
	*buf_size = 0;
	return -1;
}

unsigned int __weak get_md_resv_phy_cap_size(int md_id)
{
	return 0;
}

unsigned int __weak get_md_resv_sib_size(int md_id)
{
	return 0;
}

int __weak get_smem_amms_pos_size(int md_id)
{
	return 0;
}

int __weak get_smem_align_padding_size(int md_id)
{
	return 0;
}

int __weak get_md_smem_dfd_size(int md_id)
{
	return 0;
}

unsigned int __weak get_md_smem_cachable_offset(int md_id)
{
	return 0;
}
