// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 EMI MPU regions over ATF, as T's emi/submodule/mpu_v1.c.
 *
 * emi_mpu_set_protection() sends SMC EMIMPU_SET once per domain group, group 1
 * first, then group 0: ATF locks a region when the group 0 permission has bit
 * 31 set and refuses every later write to it until reset, so the order matters.
 * At boot the AP region 31 covers all of DRAM and is locked, as stock's
 * protect_ap_region(). SCP (region 6) and the connsys modules (26 and 25) set
 * their own.
 *
 * There is no violation IRQ handler. debugfs emi_mpu/violation reads the latched
 * violation status (EMI_MPUS, EMI_MPUT and the per-domain status) from the
 * central EMI, which nothing clears; emi_mpu/regions reads every region back
 * through SMC EMIMPU_READ, as stock's mpu_config show does.
 */
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/seq_file.h>
#include <mt-plat/mtk_secure_api.h>
#include <mt_emi_api.h>

#define AP_REGION_ID		31

/* Central EMI registers (T emi/mt6771/mt_emi.h) */
#define EMI_MPUD_ST(domain)	(0x160 + 4 * (domain))
#define EMI_MPUD_ST2(domain)	(0x200 + 4 * (domain))
#define EMI_MPUS		0x1f0
#define EMI_MPUT		0x1f8
#define EMI_MPUT_2ND		0x1fc

/* EMI MPU registers, read through ATF (T emi/mt6771/mt_emi.h) */
#define EMI_MPU_SA(region)	(0x100 + (region) * 4)
#define EMI_MPU_EA(region)	(0x200 + (region) * 4)
#define EMI_MPU_APC(region, dgroup)	(0x300 + (region) * 4 + (dgroup) * 0x100)

/* emi_mpu.ap_region=0 on the command line leaves region 31 unset */
static bool ap_region = true;
module_param(ap_region, bool, 0444);
MODULE_PARM_DESC(ap_region, "Set the locked AP region 31 over all of DRAM at boot");

static void __iomem *cen_emi_base;

int emi_mpu_set_protection(struct emi_region_info_t *region_info)
{
	unsigned int start, end;
	size_t rc[EMI_MPU_DGROUP_NUM];
	int i;

	if (region_info->region >= EMI_MPU_REGION_NUM) {
		pr_info("[MPU] can not support region %u\n",
			region_info->region);
		return -1;
	}

	start = (unsigned int)(region_info->start >> EMI_MPU_ALIGN_BITS) |
		(region_info->region << 24);

	for (i = EMI_MPU_DGROUP_NUM - 1; i >= 0; i--) {
		end = (unsigned int)(region_info->end >> EMI_MPU_ALIGN_BITS) |
			(i << 24);
		rc[i] = emi_mpu_smc_protect(start, end, region_info->apc[i]);
	}

	/* ATF: 0 set, -3 bad range, -4 region locked, 0x200c id owned by a secure zone */
	if (rc[0] || rc[1])
		pr_warn("[MPU] region %u 0x%llx-0x%llx apc 0x%x 0x%x refused: %ld %ld\n",
			region_info->region, region_info->start, region_info->end,
			region_info->apc[1], region_info->apc[0], (long)rc[1], (long)rc[0]);
	else
		pr_info("[MPU] region %u 0x%llx-0x%llx apc 0x%x 0x%x\n",
			region_info->region, region_info->start, region_info->end,
			region_info->apc[1], region_info->apc[0]);

	return 0;
}
EXPORT_SYMBOL(emi_mpu_set_protection);

/* T emi/mt6771/mpu_platform.c set_ap_region_permission() */
static void set_ap_region_permission(unsigned int apc[EMI_MPU_DGROUP_NUM])
{
	SET_ACCESS_PERMISSION(apc, LOCK,
		FORBIDDEN, FORBIDDEN, FORBIDDEN, FORBIDDEN,
		SEC_R_NSEC_R, FORBIDDEN, NO_PROTECTION, NO_PROTECTION,
		FORBIDDEN, SEC_R_NSEC_RW, FORBIDDEN, NO_PROTECTION,
		FORBIDDEN, FORBIDDEN, FORBIDDEN, NO_PROTECTION);
}

/* T mpu_v1.c protect_ap_region(): from the first to the last byte memblock knows */
static void protect_ap_region(void)
{
	struct emi_region_info_t region_info;

	region_info.start = (unsigned long long)memblock_start_of_DRAM();
	region_info.end = (unsigned long long)memblock_end_of_DRAM() - 1;
	region_info.region = AP_REGION_ID;
	set_ap_region_permission(region_info.apc);

	emi_mpu_set_protection(&region_info);
}

static int violation_show(struct seq_file *m, void *v)
{
	int d;

	if (!cen_emi_base) {
		seq_puts(m, "no mediatek,emi node\n");
		return 0;
	}
	seq_printf(m, "EMI_MPUS 0x%08x EMI_MPUT 0x%08x EMI_MPUT_2ND 0x%08x\n",
		   readl(cen_emi_base + EMI_MPUS), readl(cen_emi_base + EMI_MPUT),
		   readl(cen_emi_base + EMI_MPUT_2ND));
	for (d = 0; d < EMI_MPU_DOMAIN_NUM; d++)
		seq_printf(m, "d%-2d ST 0x%08x ST2 0x%08x\n", d,
			   readl(cen_emi_base + EMI_MPUD_ST(d)),
			   readl(cen_emi_base + EMI_MPUD_ST2(d)));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(violation);

static int regions_show(struct seq_file *m, void *v)
{
	unsigned long long start, end;
	unsigned int r, sa, ea, apc1, apc0;

	for (r = 0; r < EMI_MPU_REGION_NUM; r++) {
		sa = (unsigned int)emi_mpu_smc_read(EMI_MPU_SA(r));
		ea = (unsigned int)emi_mpu_smc_read(EMI_MPU_EA(r));
		apc1 = (unsigned int)emi_mpu_smc_read(EMI_MPU_APC(r, 1));
		apc0 = (unsigned int)emi_mpu_smc_read(EMI_MPU_APC(r, 0));
		start = ((unsigned long long)sa << EMI_MPU_ALIGN_BITS) + DRAM_OFFSET;
		end = (((unsigned long long)ea + 1) << EMI_MPU_ALIGN_BITS) + DRAM_OFFSET - 1;
		seq_printf(m, "%2u 0x%09llx-0x%09llx apc1 0x%06x apc0 0x%06x\n",
			   r, start, end, apc1, apc0);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regions);

static int __init emi_mpu_init(void)
{
	struct device_node *np;
	struct dentry *dir;

	np = of_find_compatible_node(NULL, NULL, "mediatek,emi");
	if (np) {
		cen_emi_base = of_iomap(np, 0);
		of_node_put(np);
	}
	if (cen_emi_base && readl(cen_emi_base + EMI_MPUS))
		pr_info("[MPU] violation before init: EMI_MPUS 0x%x EMI_MPUT 0x%x\n",
			readl(cen_emi_base + EMI_MPUS), readl(cen_emi_base + EMI_MPUT));

	if (ap_region)
		protect_ap_region();
	else
		pr_info("[MPU] AP region %d left unset (emi_mpu.ap_region=0)\n", AP_REGION_ID);

	dir = debugfs_create_dir("emi_mpu", NULL);
	debugfs_create_file("violation", 0400, dir, NULL, &violation_fops);
	debugfs_create_file("regions", 0400, dir, NULL, &regions_fops);
	return 0;
}
arch_initcall(emi_mpu_init);
