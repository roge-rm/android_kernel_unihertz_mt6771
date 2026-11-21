/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6771 SSPM mailbox and IPI core (drivers/misc/mediatek/sspm_ipi), the calls
 * T's sspm v1 exports under the same names. IDs, options and return codes
 * are T's sspm_ipi_pin.h and sspm_ipi.h for mt6771.
 */
#ifndef __MTK_SSPM_H__
#define __MTK_SSPM_H__

#include <linux/errno.h>
#include <linux/types.h>

/* IPI ids: 0 to 4 send on mailbox 0, 5 to 10 on mailbox 1 */
#define IPI_ID_PLATFORM		0
#define IPI_ID_CPU_DVFS		1
#define IPI_ID_QOS		2
#define IPI_ID_TST1		3
#define IPI_ID_FHCTL		4
#define IPI_ID_MCDI		5
#define IPI_ID_SPM_SUSPEND	6
#define IPI_ID_PMIC		7
#define IPI_ID_PPM		8
#define IPI_ID_THERMAL		9
#define IPI_ID_UPOWER		10
#define IPI_ID_TOTAL		11

/* sspm_ipi_send_sync() opts */
#define IPI_OPT_WAIT		0
#define IPI_OPT_POLLING		1

/* sspm_ipi_send_sync() return codes */
#define IPI_DONE		0
#define IPI_TIMEOUT_ACK		-3
#define IPI_HW_ERROR		-5
#define IPI_NO_MEMORY		-6
#define IPI_USED_IN_WAIT	-7
#define IPI_SERVICE_NOT_AVAILABLE	-100
#define IPI_SERVICE_NOT_INITED	-101

/* Mailbox 3 is shared memory: MCDI in slots 0 to 19, timestamps in 20 to 25 */
#define SHAREMBOX_NO_MCDI	3
#define SHAREMBOX_OFFSET_MCDI	0
#define SHAREMBOX_SIZE_MCDI	20

/* Mailbox 3 MCDI slots, T's mcdi/mt6771/mtk_mcdi_reg.h. 0 to 7 AP writes, 8 to 17 AP reads. */
#define MCDI_MBOX_CLUSTER_0_CAN_POWER_OFF	0
#define MCDI_MBOX_CLUSTER_1_CAN_POWER_OFF	1
#define MCDI_MBOX_BUCK_POWER_OFF_MASK		2
#define MCDI_MBOX_CLUSTER_0_ATF_ACTION_DONE	3
#define MCDI_MBOX_CLUSTER_1_ATF_ACTION_DONE	4
#define MCDI_MBOX_RESERVED			5
#define MCDI_MBOX_PAUSE_ACTION			6
#define MCDI_MBOX_AVAIL_CPU_MASK		7
#define MCDI_MBOX_CPU_CLUSTER_PWR_STAT		8
#define MCDI_MBOX_ACTION_STAT			9
#define MCDI_MBOX_CLUSTER_0_CNT			10
#define MCDI_MBOX_CLUSTER_1_CNT			11
#define MCDI_MBOX_CPU_ISOLATION_MASK		12
#define MCDI_MBOX_PAUSE_ACK			13
#define MCDI_MBOX_PENDING_ON_EVENT		14
#define MCDI_MBOX_PROF_CMD			15
#define MCDI_MBOX_PROF_CLUSTER			16
#define MCDI_MBOX_AP_READY			17

/* Blocks of reserve-memory-sspm_share, in T's sspm_reservedmem_define.h order */
enum {
	SSPM_MEM_ID = 0,
	PWRAP_MEM_ID,
	PMIC_MEM_ID,
	UPD_MEM_ID,
	QOS_MEM_ID,
	SWPM_MEM_ID,
	MET_MEM_ID,
	NUMS_MEM_ID,
};

#if IS_ENABLED(CONFIG_MTK_SSPM_IPI)
int sspm_ipi_send_sync(int mid, int opts, void *buffer, int slot,
		       void *retbuf, int retslot);
int sspm_ipi_is_inited(void);
unsigned int is_sspm_ready(void);
int sspm_mbox_read(unsigned int mbox, unsigned int slot, void *data,
		   unsigned int len);
int sspm_mcdi_mbox_write(unsigned int slot, u32 val);
bool is_sspm_ipi_lock_spm(void);
phys_addr_t sspm_reserve_mem_get_phys(unsigned int id);
phys_addr_t sspm_reserve_mem_get_virt(unsigned int id);
phys_addr_t sspm_reserve_mem_get_size(unsigned int id);
#else
static inline int sspm_ipi_send_sync(int mid, int opts, void *buffer, int slot,
				     void *retbuf, int retslot)
{
	return IPI_SERVICE_NOT_INITED;
}
static inline int sspm_ipi_is_inited(void) { return 0; }
static inline unsigned int is_sspm_ready(void) { return 0; }
static inline int sspm_mbox_read(unsigned int mbox, unsigned int slot,
				 void *data, unsigned int len)
{
	return -ENODEV;
}
static inline int sspm_mcdi_mbox_write(unsigned int slot, u32 val) { return -ENODEV; }
static inline bool is_sspm_ipi_lock_spm(void) { return false; }
static inline phys_addr_t sspm_reserve_mem_get_phys(unsigned int id) { return 0; }
static inline phys_addr_t sspm_reserve_mem_get_virt(unsigned int id) { return 0; }
static inline phys_addr_t sspm_reserve_mem_get_size(unsigned int id) { return 0; }
#endif

#endif
