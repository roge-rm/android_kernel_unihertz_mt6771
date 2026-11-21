/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

/*
 * MT6771 SCP core for 5.10, from the stock 4.14 scp/v01/scp_helper.c.
 *
 * LK loads the SCP loader and firmware, holds the SCP in reset and sets the
 * scp@10500000 node okay. This driver maps the SCP SRAM and registers, the
 * scp_share reserved memory, releases the reset and waits for the ready IPI.
 * The logger, recovery (reload and reset after a WDT), AED and SPM resource
 * requests of the 4.14 driver are left out. With CONFIG_MTK_EMI it sets EMI MPU
 * region 6 over scp_share, as stock.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/delay.h>
#include "scp_feature_define.h"
#include "scp_ipi.h"
#include "scp_helper.h"
#include "scp_excep.h"
#include "scp_dvfs.h"
#include "scp_reservedmem_define.h"
#if ENABLE_SCP_EMI_PROTECTION
#include <mt_emi_api.h>
#endif

/* scp semaphore timeout count definition */
#define SEMAPHORE_TIMEOUT 5000
/* scp ready timeout definition */
#define SCP_READY_TIMEOUT (30 * HZ) /* 30 seconds*/

/* The SCP sends this with IPI_SCP_ERROR_INFO; the layout is the firmware's. */
#define ERR_MAX_CONTEXT_LEN     32
struct error_info {
	uint32_t case_id;
	uint32_t sensor_id;
	char context[ERR_MAX_CONTEXT_LEN];
};

/* scp ready status for notify*/
unsigned int scp_ready[SCP_CORE_TOTAL];

/* scp enable status*/
unsigned int scp_enable[SCP_CORE_TOTAL];

/* scp dvfs variable*/
unsigned int scp_expected_freq;
unsigned int scp_current_freq;

/*scp awake variable*/
int scp_awake_counts[SCP_CORE_TOTAL];

/* Copy of the SRAM region info, read once before the reset is released. */
struct scp_region_info_st scp_region_info_copy;

phys_addr_t scp_mem_base_phys;
phys_addr_t scp_mem_base_virt;
phys_addr_t scp_mem_size;
struct scp_regs scpreg;

unsigned char *scp_send_buff[SCP_CORE_TOTAL];
unsigned char *scp_recv_buff[SCP_CORE_TOTAL];

static struct workqueue_struct *scp_workqueue;
static struct timer_list scp_ready_timer[SCP_CORE_TOTAL];
static struct scp_work_struct scp_A_notify_work;
static struct scp_work_struct scp_timeout_work;
static struct scp_work_struct scp_stop_work;
static unsigned int scp_timeout_times;

static DEFINE_MUTEX(scp_A_notify_mutex);
static DEFINE_MUTEX(scp_feature_mutex);
static DEFINE_MUTEX(scp_register_sensor_mutex);

char *core_ids[SCP_CORE_TOTAL] = {"SCP A"};
DEFINE_SPINLOCK(scp_awake_spinlock);
/* set flag after driver initial done */
static bool driver_init_done;
/* reset_scp's PLL request, held until the ready IPI; keeps enable and disable paired */
static bool scp_pll_held;

/*
 * memory copy to scp sram, in 32-bit words
 * @param trg: trg address
 * @param src: src address
 * @param size: memory size
 */
void memcpy_to_scp(void __iomem *trg, const void *src, int size)
{
	int i;
	u32 __iomem *t = trg;
	const u32 *s = src;

	for (i = 0; i < ((size + 3) >> 2); i++)
		writel_relaxed(*s++, t++);
}
EXPORT_SYMBOL_GPL(memcpy_to_scp);

/*
 * memory copy from scp sram, in 32-bit words
 * @param trg: trg address
 * @param src: src address
 * @param size: memory size
 */
void memcpy_from_scp(void *trg, const void __iomem *src, int size)
{
	int i;
	u32 *t = trg;
	const u32 __iomem *s = src;

	for (i = 0; i < ((size + 3) >> 2); i++)
		*t++ = readl_relaxed(s++);
}
EXPORT_SYMBOL_GPL(memcpy_from_scp);

/*
 * acquire a hardware semaphore
 * @param flag: semaphore id
 * return  1 :get sema success
 *        -1 :get sema timeout
 */
int get_scp_semaphore(int flag)
{
	int read_back;
	int count = 0;
	int ret = -1;
	unsigned long spin_flags;

	if (!driver_init_done)
		return -1;

	if (scp_awake_lock(SCP_A_ID) == -1) {
		pr_debug("[SCP] %s: awake scp fail\n", __func__);
		return ret;
	}

	spin_lock_irqsave(&scp_awake_spinlock, spin_flags);

	flag = (flag * 2) + 1;

	read_back = (readl(SCP_SEMAPHORE) >> flag) & 0x1;

	if (read_back == 0) {
		writel((1 << flag), SCP_SEMAPHORE);

		while (count != SEMAPHORE_TIMEOUT) {
			/* repeat test if we get semaphore */
			read_back = (readl(SCP_SEMAPHORE) >> flag) & 0x1;
			if (read_back == 1) {
				ret = 1;
				break;
			}
			writel((1 << flag), SCP_SEMAPHORE);
			count++;
		}

		if (ret < 0)
			pr_debug("[SCP] get scp sema. %d TIMEOUT...!\n", flag);
	} else {
		pr_err("[SCP] already hold scp sema. %d\n", flag);
	}

	spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);

	if (scp_awake_unlock(SCP_A_ID) == -1)
		pr_debug("[SCP] %s: scp_awake_unlock fail\n", __func__);

	return ret;
}
EXPORT_SYMBOL_GPL(get_scp_semaphore);

/*
 * release a hardware semaphore
 * @param flag: semaphore id
 * return  1 :release sema success
 *        -1 :release sema fail
 */
int release_scp_semaphore(int flag)
{
	int read_back;
	int ret = -1;
	unsigned long spin_flags;

	if (!driver_init_done)
		return -1;

	if (scp_awake_lock(SCP_A_ID) == -1) {
		pr_debug("[SCP] %s: awake scp fail\n", __func__);
		return ret;
	}
	spin_lock_irqsave(&scp_awake_spinlock, spin_flags);
	flag = (flag * 2) + 1;

	read_back = (readl(SCP_SEMAPHORE) >> flag) & 0x1;

	if (read_back == 1) {
		/* Write 1 clear */
		writel((1 << flag), SCP_SEMAPHORE);
		read_back = (readl(SCP_SEMAPHORE) >> flag) & 0x1;
		if (read_back == 0)
			ret = 1;
		else
			pr_debug("[SCP] release scp sema. %d failed\n", flag);
	} else {
		pr_err("[SCP] try to release sema. %d not own by me\n", flag);
	}

	spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);

	if (scp_awake_unlock(SCP_A_ID) == -1)
		pr_debug("[SCP] %s: scp_awake_unlock fail\n", __func__);

	return ret;
}
EXPORT_SYMBOL_GPL(release_scp_semaphore);

static BLOCKING_NOTIFIER_HEAD(scp_A_notifier_list);
/*
 * register apps notification; calls back at once if the SCP is ready.
 * May block, not for interrupt context.
 * @param nb:   notifier block struct
 */
void scp_A_register_notify(struct notifier_block *nb)
{
	mutex_lock(&scp_A_notify_mutex);
	blocking_notifier_chain_register(&scp_A_notifier_list, nb);

	pr_debug("[SCP] register scp A notify callback..\n");

	if (is_scp_ready(SCP_A_ID))
		nb->notifier_call(nb, SCP_EVENT_READY, NULL);
	mutex_unlock(&scp_A_notify_mutex);
}
EXPORT_SYMBOL_GPL(scp_A_register_notify);

/*
 * unregister apps notification. May block, not for interrupt context.
 * @param nb:     notifier block struct
 */
void scp_A_unregister_notify(struct notifier_block *nb)
{
	mutex_lock(&scp_A_notify_mutex);
	blocking_notifier_chain_unregister(&scp_A_notifier_list, nb);
	mutex_unlock(&scp_A_notify_mutex);
}
EXPORT_SYMBOL_GPL(scp_A_unregister_notify);

void scp_schedule_work(struct scp_work_struct *scp_ws)
{
	queue_work(scp_workqueue, &scp_ws->work);
}

/*
 * Work after the ready IPI: release the PLL request, tell the clients, then
 * register the RTOS feature, which sends the first DVFS request.
 */
static void scp_A_notify_ws(struct work_struct *ws)
{
	struct scp_work_struct *sws =
		container_of(ws, struct scp_work_struct, work);
	unsigned int scp_notify_flag = sws->flags;

	scp_ready[SCP_A_ID] = scp_notify_flag;

	if (scp_notify_flag) {
		/* release pll clock after scp ulposc calibration */
		if (scp_pll_held) {
			scp_pll_ctrl_set(PLL_DISABLE, CLK_26M);
			scp_pll_held = false;
		}
		writel(0xff, SCP_TO_SPM_REG); /* patch: clear SPM interrupt */
		mutex_lock(&scp_A_notify_mutex);
		pr_notice("[SCP] ready, notifying clients\n");
		blocking_notifier_call_chain(&scp_A_notifier_list,
			SCP_EVENT_READY, NULL);
		mutex_unlock(&scp_A_notify_mutex);

		/* stock waits 2 s here before the first DVFS request */
		msleep(2000);
		scp_register_feature(RTOS_FEATURE_ID);
	}
}

/* Ready timeout: log it with the SCP registers. Without recovery the SCP is not restarted. */
static void scp_timeout_ws(struct work_struct *ws)
{
	scp_timeout_times++;
	pr_notice("[SCP] not ready %d s after the reset release, scp_timeout_times=%x\n",
		SCP_READY_TIMEOUT / HZ, scp_timeout_times);
	scp_aed(EXCEP_BOOTUP, SCP_A_ID);
}

/* After a WDT: mark the SCP down and tell the clients, since nothing restarts it. */
static void scp_stop_ws(struct work_struct *ws)
{
	mutex_lock(&scp_A_notify_mutex);
	scp_ready[SCP_A_ID] = 0;
	blocking_notifier_call_chain(&scp_A_notifier_list,
		SCP_EVENT_STOP, NULL);
	mutex_unlock(&scp_A_notify_mutex);
}

void scp_mark_stopped(void)
{
	scp_schedule_work(&scp_stop_work);
}

static void scp_A_set_ready(void)
{
	pr_debug("[SCP] %s()\n", __func__);
	scp_timeout_times = 0;
	del_timer(&scp_ready_timer[SCP_A_ID]);
	scp_A_notify_work.flags = 1;
	scp_schedule_work(&scp_A_notify_work);
}

/* ready timer callback, timer context */
static void scp_wait_ready_timeout(struct timer_list *t)
{
	scp_timeout_work.flags = 0;
	scp_timeout_work.id = SCP_A_ID;
	scp_schedule_work(&scp_timeout_work);
}

/*
 * The SCP sends IPI_SCP_A_READY with its image size once it runs.
 * Interrupt context.
 */
static void scp_A_ready_ipi_handler(int id, void *data, unsigned int len)
{
	unsigned int scp_image_size = *(unsigned int *)data;

	if (!scp_ready[SCP_A_ID])
		scp_A_set_ready();

	/*verify scp image size*/
	if (scp_image_size != SCP_A_TCM_SIZE) {
		pr_err("[SCP]image size ERROR! AP=0x%x,SCP=0x%x\n",
					SCP_A_TCM_SIZE, scp_image_size);
		WARN_ON(1);
	}
}

/* Error report from the SCP (sensor init failures and the like), logged. */
static void scp_err_info_handler(int id, void *data, unsigned int len)
{
	struct error_info *info = (struct error_info *)data;

	if (sizeof(*info) != len) {
		pr_notice("[SCP] error: incorrect size %d of error_info\n",
				len);
		WARN_ON(1);
		return;
	}

	/* Ensure the context[] is terminated by the NULL character. */
	info->context[ERR_MAX_CONTEXT_LEN - 1] = '\0';
	pr_notice("[SCP] Error_info: case id: %u\n", info->case_id);
	pr_notice("[SCP] Error_info: sensor id: %u\n", info->sensor_id);
	pr_notice("[SCP] Error_info: context: %s\n", info->context);
}

/*
 * @return: 1 if scp is ready for running tasks
 */
unsigned int is_scp_ready(enum scp_core_id id)
{
	if (scp_ready[id])
		return 1;
	else
		return 0;
}
EXPORT_SYMBOL_GPL(is_scp_ready);

/*
 * Release the SCP reset and start the ready timer.
 * @param reset: SCP_ALL_ENABLE or SCP_A_ENABLE. A reboot (bit 0 set) needs the
 *               loader copied back into SRAM, which is part of the recovery
 *               this port leaves out, so it returns -EOPNOTSUPP.
 * May block, not for interrupt context.
 */
int reset_scp(int reset)
{
	void __iomem *scp_reset_reg = scpreg.cfg;

	if (((reset & 0xf0) != 0x10) && ((reset & 0xf0) != 0x00)) {
		pr_debug("[SCP] %s: skipped!\n", __func__);
		return 0;
	}
	if (reset & 0x0f) {
		pr_notice("[SCP] %s: reboot is not supported\n", __func__);
		return -EOPNOTSUPP;
	}

	mutex_lock(&scp_A_notify_mutex);
	blocking_notifier_call_chain(&scp_A_notifier_list, SCP_EVENT_STOP,
		NULL);
	mutex_unlock(&scp_A_notify_mutex);

	/* request pll clock before turn on scp */
	if (!scp_pll_held) {
		scp_pll_ctrl_set(PLL_ENABLE, CLK_26M);
		scp_pll_held = true;
	}

	if (scp_enable[SCP_A_ID]) {
		pr_notice("[SCP] reset reg 0x%x, releasing reset\n",
			readl(scp_reset_reg));
		writel(1, scp_reset_reg);  /* release reset */
		dsb(sy);
		mod_timer(&scp_ready_timer[SCP_A_ID],
			jiffies + SCP_READY_TIMEOUT);
	}

	pr_debug("[SCP] %s: done\n", __func__);

	return 0;
}

static ssize_t scp_A_status_show(struct device *kobj,
			struct device_attribute *attr, char *buf)
{
	if (scp_ready[SCP_A_ID])
		return scnprintf(buf, PAGE_SIZE, "SCP A is ready\n");
	else
		return scnprintf(buf, PAGE_SIZE, "SCP A is not ready\n");
}

static DEVICE_ATTR(scp_A_status, 0444, scp_A_status_show, NULL);

static ssize_t scp_A_reg_status_show(struct device *kobj,
			struct device_attribute *attr, char *buf)
{
	int len = 0;

	scp_A_dump_regs();

	if (scp_ready[SCP_A_ID]) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_DEBUG_PC_REG:0x%x\n",
			readl(SCP_A_DEBUG_PC_REG));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_DEBUG_LR_REG:0x%x\n",
			readl(SCP_A_DEBUG_LR_REG));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_DEBUG_PSP_REG:0x%x\n",
			readl(SCP_A_DEBUG_PSP_REG));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_DEBUG_SP_REG:0x%x\n",
			readl(SCP_A_DEBUG_SP_REG));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_GENERAL_REG0:0x%x\n",
			readl(SCP_A_GENERAL_REG0));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_GENERAL_REG1:0x%x\n",
			readl(SCP_A_GENERAL_REG1));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_GENERAL_REG2:0x%x\n",
			readl(SCP_A_GENERAL_REG2));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_GENERAL_REG3:0x%x\n",
			readl(SCP_A_GENERAL_REG3));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_GENERAL_REG4:0x%x\n",
			readl(SCP_A_GENERAL_REG4));
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"[SCP] SCP_A_GENERAL_REG5:0x%x\n",
			readl(SCP_A_GENERAL_REG5));
		return len;
	} else
		return scnprintf(buf, PAGE_SIZE, "SCP A is not ready\n");
}
static DEVICE_ATTR(scp_A_reg_status, 0444, scp_A_reg_status_show, NULL);

static struct attribute *scp_attrs[] = {
	&dev_attr_scp_A_status.attr,
	&dev_attr_scp_A_reg_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(scp);

/* /dev/scp has no file operations of its own without the logger. */
static const struct file_operations scp_file_ops = {
	.owner = THIS_MODULE,
};

static struct miscdevice scp_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "scp",
	.fops = &scp_file_ops,
	.groups = scp_groups,
};

#define SCP_MEM_RESERVED_KEY "mediatek,reserve-memory-scp_share"
static int scp_reserve_mem_of_init(struct reserved_mem *rmem)
{
	scp_mem_base_phys = (phys_addr_t) rmem->base;
	scp_mem_size = (phys_addr_t) rmem->size;

	return 0;
}

RESERVEDMEM_OF_DECLARE(scp_reserve_mem_init,
			SCP_MEM_RESERVED_KEY, scp_reserve_mem_of_init);

phys_addr_t scp_get_reserve_mem_phys(enum scp_reserve_mem_id_t id)
{
	if (id >= NUMS_MEM_ID) {
		pr_err("[SCP] no reserve memory for %d", id);
		return 0;
	} else
		return scp_reserve_mblock[id].start_phys;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_phys);

phys_addr_t scp_get_reserve_mem_virt(enum scp_reserve_mem_id_t id)
{
	if (id >= NUMS_MEM_ID) {
		pr_err("[SCP] no reserve memory for %d", id);
		return 0;
	} else
		return scp_reserve_mblock[id].start_virt;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_virt);

phys_addr_t scp_get_reserve_mem_size(enum scp_reserve_mem_id_t id)
{
	if (id >= NUMS_MEM_ID) {
		pr_err("[SCP] no reserve memory for %d", id);
		return 0;
	} else
		return scp_reserve_mblock[id].size;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_size);

#if ENABLE_SCP_EMI_PROTECTION
/* T's set_scp_mpu(): scp_share open to the AP (d0) and the SCP (d3) only */
static void set_scp_mpu(void)
{
	struct emi_region_info_t region_info;

	region_info.region = MPU_REGION_ID_SCP_SMEM;
	region_info.start = scp_mem_base_phys;
	region_info.end =  scp_mem_base_phys + scp_mem_size - 0x1;

	SET_ACCESS_PERMISSION(region_info.apc, UNLOCK,
			FORBIDDEN, FORBIDDEN, FORBIDDEN, FORBIDDEN,
			FORBIDDEN, FORBIDDEN, FORBIDDEN, FORBIDDEN,
			FORBIDDEN, FORBIDDEN, FORBIDDEN, FORBIDDEN,
			NO_PROTECTION, FORBIDDEN, FORBIDDEN, NO_PROTECTION);

	emi_mpu_set_protection(&region_info);
}
#endif

/*
 * Map scp_share write-combined and split it into the stock blocks, in order.
 * The SCP can only reach 0x4000_0000 to 0x8FFF_FFFF.
 */
static int scp_reserve_memory_ioremap(void)
{
	unsigned int num = (unsigned int)(sizeof(scp_reserve_mblock)
			/ sizeof(scp_reserve_mblock[0]));
	enum scp_reserve_mem_id_t id;
	phys_addr_t accumlate_memory_size = 0;
	void __iomem *base;

	if ((scp_mem_base_phys >= (0x90000000ULL)) ||
			 (scp_mem_base_phys <= 0x0)) {
		pr_err("[SCP] Error: Wrong Address (0x%llx)\n",
			    (uint64_t)scp_mem_base_phys);
		return -EINVAL;
	}

	if (num != NUMS_MEM_ID) {
		pr_err("[SCP] number of entries of reserved memory %u / %u\n",
			num, NUMS_MEM_ID);
		return -EINVAL;
	}

	base = ioremap_wc(scp_mem_base_phys, scp_mem_size);
	if (!base)
		return -ENOMEM;
	scp_mem_base_virt = (phys_addr_t)(size_t)base;
	pr_notice("[SCP] scp_share phys 0x%llx, len 0x%llx\n",
		(uint64_t)scp_mem_base_phys, (uint64_t)scp_mem_size);

	for (id = 0; id < NUMS_MEM_ID; id++) {
		scp_reserve_mblock[id].start_phys = scp_mem_base_phys +
			accumlate_memory_size;
		scp_reserve_mblock[id].start_virt = scp_mem_base_virt +
			accumlate_memory_size;
		accumlate_memory_size += scp_reserve_mblock[id].size;
		pr_debug("[SCP] [%d] phys:0x%llx, virt:0x%llx, len:0x%llx\n",
			id, (uint64_t)scp_reserve_mblock[id].start_phys,
			(uint64_t)scp_reserve_mblock[id].start_virt,
			(uint64_t)scp_reserve_mblock[id].size);
	}

	if (accumlate_memory_size > scp_mem_size) {
		pr_err("[SCP] blocks need 0x%llx, scp_share has 0x%llx\n",
			(uint64_t)accumlate_memory_size, (uint64_t)scp_mem_size);
		iounmap(base);
		scp_mem_base_virt = 0;
		for (id = 0; id < NUMS_MEM_ID; id++) {
			scp_reserve_mblock[id].start_phys = 0;
			scp_reserve_mblock[id].start_virt = 0;
		}
		return -ENOMEM;
	}

	return 0;
}

void scp_register_feature(enum feature_id id)
{
	uint32_t i;
	int ret = 0;

	/*prevent from access when scp is down*/
	if (!scp_ready[SCP_A_ID]) {
		pr_debug("[SCP] %s: not ready, scp=%u\n", __func__,
			scp_ready[SCP_A_ID]);
		return;
	}

	/* feature_table is global, the mutex keeps callers apart */
	mutex_lock(&scp_feature_mutex);

	/*SCP keep awake */
	if (scp_awake_lock(SCP_A_ID) == -1) {
		pr_debug("[SCP] %s: awake scp fail\n", __func__);
		mutex_unlock(&scp_feature_mutex);
		return;
	}

	for (i = 0; i < NUM_FEATURE_ID; i++) {
		if (feature_table[i].feature == id)
			feature_table[i].enable = 1;
	}
	scp_expected_freq = scp_get_freq();

	scp_current_freq = readl(CURRENT_FREQ_REG);
	writel(scp_expected_freq, EXPECTED_FREQ_REG);

	/* send request only when scp is not down */
	if (scp_ready[SCP_A_ID]) {
		if (scp_current_freq != scp_expected_freq) {
			/* set scp freq. */
			ret = scp_request_freq();
			if (ret == -1) {
				pr_err("[SCP]%s request_freq fail\n", __func__);
				WARN_ON(1);
			}
		}
	} else {
		pr_err("[SCP]Not send SCP DVFS request because SCP is down\n");
		WARN_ON(1);
	}

	/*SCP release awake */
	if (scp_awake_unlock(SCP_A_ID) == -1)
		pr_debug("[SCP] %s: awake unlock fail\n", __func__);

	mutex_unlock(&scp_feature_mutex);
}
EXPORT_SYMBOL_GPL(scp_register_feature);

void scp_deregister_feature(enum feature_id id)
{
	uint32_t i;
	int ret = 0;

	/* prevent from access when scp is down */
	if (!scp_ready[SCP_A_ID]) {
		pr_debug("[SCP] %s:not ready, scp=%u\n", __func__,
			scp_ready[SCP_A_ID]);
		return;
	}

	mutex_lock(&scp_feature_mutex);

	/*SCP keep awake */
	if (scp_awake_lock(SCP_A_ID) == -1) {
		pr_debug("[SCP] %s: awake scp fail\n", __func__);
		mutex_unlock(&scp_feature_mutex);
		return;
	}

	for (i = 0; i < NUM_FEATURE_ID; i++) {
		if (feature_table[i].feature == id)
			feature_table[i].enable = 0;
	}
	scp_expected_freq = scp_get_freq();

	scp_current_freq = readl(CURRENT_FREQ_REG);
	writel(scp_expected_freq, EXPECTED_FREQ_REG);

	/* send request only when scp is not down */
	if (scp_ready[SCP_A_ID]) {
		if (scp_current_freq != scp_expected_freq) {
			/* set scp freq. */
			ret = scp_request_freq();
			if (ret == -1) {
				pr_err("[SCP] %s: req_freq fail\n", __func__);
				WARN_ON(1);
			}
		}
	} else {
		pr_err("[SCP]Not send SCP DVFS request because SCP is down\n");
		WARN_ON(1);
	}

	/*SCP release awake */
	if (scp_awake_unlock(SCP_A_ID) == -1)
		pr_debug("[SCP] %s: awake unlock fail\n", __func__);

	mutex_unlock(&scp_feature_mutex);
}
EXPORT_SYMBOL_GPL(scp_deregister_feature);

/*scp sensor type register*/
void scp_register_sensor(enum feature_id id, enum scp_sensor_id sensor_id)
{
	uint32_t i;

	/* prevent from access when scp is down */
	if (!scp_ready[SCP_A_ID])
		return;

	if (id != SENS_FEATURE_ID) {
		pr_debug("[SCP]register sensor id err");
		return;
	}
	mutex_lock(&scp_register_sensor_mutex);
	for (i = 0; i < NUM_SENSOR_TYPE; i++) {
		if (sensor_type_table[i].feature == sensor_id)
			sensor_type_table[i].enable = 1;
	}

	/* register sensor*/
	scp_register_feature(id);
	mutex_unlock(&scp_register_sensor_mutex);
}
EXPORT_SYMBOL_GPL(scp_register_sensor);

/*scp sensor type deregister*/
void scp_deregister_sensor(enum feature_id id, enum scp_sensor_id sensor_id)
{
	uint32_t i;

	/* prevent from access when scp is down */
	if (!scp_ready[SCP_A_ID])
		return;

	if (id != SENS_FEATURE_ID) {
		pr_debug("[SCP]deregister sensor id err");
		return;
	}
	mutex_lock(&scp_register_sensor_mutex);
	for (i = 0; i < NUM_SENSOR_TYPE; i++) {
		if (sensor_type_table[i].feature == sensor_id)
			sensor_type_table[i].enable = 0;
	}

	/* deregister sensor*/
	scp_deregister_feature(id);
	mutex_unlock(&scp_register_sensor_mutex);
}
EXPORT_SYMBOL_GPL(scp_deregister_sensor);

/*
 * apps notification
 */
void scp_extern_notify(enum SCP_NOTIFY_EVENT notify_status)
{
	blocking_notifier_call_chain(&scp_A_notifier_list, notify_status, NULL);
}

/*
 * reset awake counter
 */
void scp_reset_awake_counts(void)
{
	int i;

	for (i = 0; i < SCP_CORE_TOTAL ; i++)
		scp_awake_counts[i] = 0;
}

void scp_awake_init(void)
{
	scp_reset_awake_counts();
}

/* Low power code asks whether SCP needs main_pll; never on MT6771, as stock. */
int scp_check_resource(void)
{
	return 0;
}

/* Read and log the region info LK and the SCP loader left in SRAM. */
static void scp_region_info_init(void)
{
	int region_size = SCP_RTOS_START - SCP_REGION_INFO_OFFSET -
		(SHARE_BUF_SIZE * 2);
	int struct_size = sizeof(scp_region_info_copy);

	if (struct_size > region_size) {
		pr_debug("[SCP] Error: Structure exceeds region info!\n");
		WARN_ON(1);
		return;
	}

	memcpy_from_scp(&scp_region_info_copy,
		SCP_TCM + SCP_REGION_INFO_OFFSET, struct_size);
	pr_notice("[SCP] region info: loader 0x%x+0x%x, firmware 0x%x+0x%x, dram 0x%x+0x%x, size %u\n",
		scp_region_info_copy.ap_loader_start,
		scp_region_info_copy.ap_loader_size,
		scp_region_info_copy.ap_firmware_start,
		scp_region_info_copy.ap_firmware_size,
		scp_region_info_copy.ap_dram_start,
		scp_region_info_copy.ap_dram_size,
		scp_region_info_copy.struct_size);
}

static int scpsys_device_probe(struct platform_device *pdev)
{
	struct resource *res;

	/* scpsys shares infracfg_ao with the clock drivers, so no region request */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	scpreg.scpsys = devm_ioremap(&pdev->dev, res->start,
		resource_size(res));
	if (!scpreg.scpsys) {
		pr_err("[SCP] scpreg.scpsys error\n");
		return -ENOMEM;
	}
	pr_debug("[SCP] scpreg.scpsys = %p\n", scpreg.scpsys);
	return 0;
}

static const struct of_device_id scpsys_of_ids[] = {
	{ .compatible = "mediatek,scpsys", },
	{}
};

static struct platform_driver mtk_scpsys_device = {
	.probe = scpsys_device_probe,
	.driver = {
		.name = "scpsys",
		.of_match_table = scpsys_of_ids,
		.suppress_bind_attrs = true,
	},
};

/*
 * Everything after the resources are mapped: IPI, IRQ, reserved memory and
 * the reset release, in stock order.
 */
static int scp_core_init(struct device *dev)
{
	int ret;

	/* pll maybe gate, request pll before access any scp reg/sram */
	scp_pll_ctrl_set(PLL_ENABLE, CLK_26M);

	if (scp_mem_size == 0) {
		pr_err("[SCP] Reserving memory by of_device for SCP failed.\n");
		ret = -ENOMEM;
		goto err;
	}

	scp_region_info_init();
	scp_awake_init();

	scp_workqueue = alloc_ordered_workqueue("SCP_WQ", 0);
	if (!scp_workqueue) {
		ret = -ENOMEM;
		goto err;
	}

	ret = scp_excep_init();
	if (ret) {
		pr_debug("[SCP]Excep Init Fail\n");
		goto err_wq;
	}

	/* scp ipi initialise */
	scp_send_buff[SCP_A_ID] = devm_kmalloc(dev, SHARE_BUF_SIZE, GFP_KERNEL);
	scp_recv_buff[SCP_A_ID] = devm_kmalloc(dev, SHARE_BUF_SIZE, GFP_KERNEL);
	if (!scp_send_buff[SCP_A_ID] || !scp_recv_buff[SCP_A_ID]) {
		ret = -ENOMEM;
		goto err_wq;
	}

	INIT_WORK(&scp_A_notify_work.work, scp_A_notify_ws);
	INIT_WORK(&scp_timeout_work.work, scp_timeout_ws);
	INIT_WORK(&scp_stop_work.work, scp_stop_ws);

	scp_A_irq_init();
	scp_A_ipi_init();

	scp_ipi_registration(IPI_SCP_A_READY,
			 scp_A_ready_ipi_handler, "scp_A_ready");
	scp_ipi_registration(IPI_SCP_ERROR_INFO,
			 scp_err_info_handler, "scp_err_info_handler");

	ret = misc_register(&scp_device);
	if (ret) {
		pr_err("[SCP] misc register failed\n");
		goto err_wq;
	}

	ret = request_irq(scpreg.irq, scp_A_irq_handler,
			IRQF_TRIGGER_NONE, "SCP A IPC2HOST", NULL);
	if (ret) {
		pr_err("[SCP] CM4 A require irq failed\n");
		goto err_misc;
	}

	ret = scp_reserve_memory_ioremap();
	if (ret) {
		pr_err("[SCP]scp_reserve_memory_ioremap failed\n");
		goto err_irq;
	}

#if ENABLE_SCP_EMI_PROTECTION
	set_scp_mpu();
#endif

	/* remember to release pll */
	scp_pll_ctrl_set(PLL_DISABLE, CLK_26M);

	driver_init_done = true;
	reset_scp(SCP_ALL_ENABLE);

	return 0;

err_irq:
	free_irq(scpreg.irq, NULL);
err_misc:
	misc_deregister(&scp_device);
err_wq:
	destroy_workqueue(scp_workqueue);
	scp_workqueue = NULL;
err:
	/* the devm buffers go with the failed probe */
	scp_send_buff[SCP_A_ID] = NULL;
	scp_recv_buff[SCP_A_ID] = NULL;
	scp_pll_ctrl_set(PLL_DISABLE, CLK_26M);
	return ret;
}

static int scp_device_probe(struct platform_device *pdev)
{
	struct resource *res;
	const char *core_status = NULL;
	struct device *dev = &pdev->dev;

	/* scp_dvfs holds the SCP clock mux and scpsys the awake and reset registers */
	if (!scp_dvfs_ready() || !scpreg.scpsys)
		return -EPROBE_DEFER;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	scpreg.sram = devm_ioremap_resource(dev, res);
	if (IS_ERR((void const *) scpreg.sram)) {
		pr_err("[SCP] scpreg.sram error\n");
		return PTR_ERR(scpreg.sram);
	}
	scpreg.total_tcmsize = (unsigned int)resource_size(res);
	pr_debug("[SCP] sram base = 0x%p %x\n",
		scpreg.sram, scpreg.total_tcmsize);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	scpreg.cfg = devm_ioremap_resource(dev, res);
	if (IS_ERR((void const *) scpreg.cfg)) {
		pr_err("[SCP] scpreg.cfg error\n");
		return PTR_ERR(scpreg.cfg);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	scpreg.clkctrl = devm_ioremap_resource(dev, res);
	if (IS_ERR((void const *) scpreg.clkctrl)) {
		pr_err("[SCP] scpreg.clkctrl error\n");
		return PTR_ERR(scpreg.clkctrl);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	scpreg.l1cctrl = devm_ioremap_resource(dev, res);
	if (IS_ERR((void const *) scpreg.l1cctrl)) {
		pr_err("[SCP] scpreg.l1cctrl error\n");
		return PTR_ERR(scpreg.l1cctrl);
	}

	scpreg.irq = platform_get_irq(pdev, 0);
	if (scpreg.irq < 0)
		return scpreg.irq;

	of_property_read_u32(dev->of_node, "scp_sramSize",
						&scpreg.scp_tcmsize);
	if (!scpreg.scp_tcmsize) {
		pr_err("[SCP] total_tcmsize not found\n");
		return -ENODEV;
	}

	/*scp core 1*/
	of_property_read_string(dev->of_node, "core_1", &core_status);
	if (!core_status || strcmp(core_status, "enable") != 0) {
		pr_err("[SCP] core_1 not enable\n");
		return -ENODEV;
	}
	scp_enable[SCP_A_ID] = 1;

	return scp_core_init(dev);
}

static const struct of_device_id scp_of_ids[] = {
	{ .compatible = "mediatek,scp", },
	{}
};

static struct platform_driver mtk_scp_device = {
	.probe = scp_device_probe,
	.driver = {
		.name = "scp",
		.of_match_table = scp_of_ids,
		.suppress_bind_attrs = true,
	},
};

/*
 * driver initialization entry point. The SCP probes only if LK left
 * scp@10500000 okay, which it does after loading the firmware.
 */
static int __init scp_init(void)
{
	int ret;

	timer_setup(&scp_ready_timer[SCP_A_ID], scp_wait_ready_timeout, 0);
	scp_timeout_times = 0;

	ret = scp_dvfs_init();
	if (ret)
		pr_err("[SCP] scp dvfs init fail %d\n", ret);

	ret = platform_driver_register(&mtk_scpsys_device);
	if (ret)
		pr_err("[SCP] scpsys probe fail\n");

	ret = platform_driver_register(&mtk_scp_device);
	if (ret)
		pr_err("[SCP] scp probe fail\n");

	return ret;
}

module_init(scp_init);
