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
 * SCP mailbox IPI: one receive and one send share_obj in SCP SRAM just below
 * SCP_RTOS_START, and the GIPC register to ring the SCP.
 */

#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/hardirq.h>
#include "scp_ipi.h"
#include "scp_helper.h"
#include "scp_excep.h"

#define PRINT_THRESHOLD 10000

enum ipi_id scp_ipi_id_record;
enum ipi_id scp_ipi_mutex_owner[SCP_CORE_TOTAL];
enum ipi_id scp_ipi_owner[SCP_CORE_TOTAL];
unsigned int scp_ipi_id_record_count;
unsigned int scp_to_ap_ipi_count;
unsigned int ap_to_scp_ipi_count;

struct scp_ipi_desc scp_ipi_desc[SCP_NR_IPI];
static struct share_obj __iomem *scp_send_obj[SCP_CORE_TOTAL];
static struct share_obj __iomem *scp_rcv_obj[SCP_CORE_TOTAL];
struct mutex scp_ipi_mutex[SCP_CORE_TOTAL];

/*
 * find an ipi handler and invoke it. Interrupt context: the handlers run
 * in the SCP IRQ, as stock.
 */
void scp_A_ipi_handler(void)
{
	enum ipi_id scp_id;
	unsigned int len;

	scp_id = readl(&scp_rcv_obj[SCP_A_ID]->id);
	len = readl(&scp_rcv_obj[SCP_A_ID]->len);
	if (len > sizeof(scp_rcv_obj[SCP_A_ID]->share_buf))
		len = sizeof(scp_rcv_obj[SCP_A_ID]->share_buf);

	if (scp_id >= SCP_NR_IPI || scp_id <= 0) {
		/* ipi id abnormal*/
		pr_debug("[SCP] A ipi handler id abnormal, id=%d\n", scp_id);
	} else if (scp_ipi_desc[scp_id].handler) {
		memcpy_from_scp(scp_recv_buff[SCP_A_ID],
			scp_rcv_obj[SCP_A_ID]->share_buf, len);

		scp_ipi_desc[scp_id].recv_count++;
		scp_to_ap_ipi_count++;
		scp_ipi_desc[scp_id].handler(scp_id, scp_recv_buff[SCP_A_ID],
			len);
		/* After the handler, a direct IPI keeps the SCP from waiting
		 * in its IPI busy idle state.
		 */
		writel((1 << SCP_A_IPI_AWAKE_NUM), SCP_GIPC_IN_REG);
	} else {
		/* scp_ipi_handler is null or ipi id abnormal */
		pr_debug("[SCP] A ipi handler is null or abnormal, id=%d\n",
								scp_id);
	}
	/* AP side write 1 to clear SCP to SPM reg.
	 * scp side write 1 to set SCP to SPM reg.
	 * scp set	  bit[0]
	 */
	writel(0x1, SCP_TO_SPM_REG);
}

/*
 * ipi initialize
 */
void scp_A_ipi_init(void)
{
	mutex_init(&scp_ipi_mutex[SCP_A_ID]);
	scp_rcv_obj[SCP_A_ID] = SCP_A_SHARE_BUFFER;
	scp_send_obj[SCP_A_ID] = scp_rcv_obj[SCP_A_ID] + 1;
	pr_debug("[SCP] scp_rcv_obj[A] = 0x%p\n", scp_rcv_obj[SCP_A_ID]);
	pr_debug("[SCP] scp_send_obj[A] = 0x%p\n", scp_send_obj[SCP_A_ID]);
	memset_io(scp_send_obj[SCP_A_ID], 0, SHARE_BUF_SIZE);
	scp_to_ap_ipi_count = 0;
	ap_to_scp_ipi_count = 0;
}

/*
 * API let apps can register an ipi handler to receive IPI
 * @param id:	   IPI ID
 * @param handler:  IPI handler, called in interrupt context
 * @param name:	 IPI name
 */
enum scp_ipi_status scp_ipi_registration(enum ipi_id id,
	void (*ipi_handler)(int id, void *data, unsigned int len),
	const char *name)
{
	if (id < SCP_NR_IPI) {
		scp_ipi_desc[id].name = name;

		if (ipi_handler == NULL)
			return SCP_IPI_ERROR;

		scp_ipi_desc[id].handler = ipi_handler;
		return SCP_IPI_DONE;
	} else {
		return SCP_IPI_ERROR;
	}
}
EXPORT_SYMBOL_GPL(scp_ipi_registration);

/*
 * API let apps unregister an ipi handler
 * @param id:	   IPI ID
 */
enum scp_ipi_status scp_ipi_unregistration(enum ipi_id id)
{
	if (id < SCP_NR_IPI) {
		scp_ipi_desc[id].name = "";
		scp_ipi_desc[id].handler = NULL;
		return SCP_IPI_DONE;
	} else {
		return SCP_IPI_ERROR;
	}
}
EXPORT_SYMBOL_GPL(scp_ipi_unregistration);

/*
 * API for apps to send an IPI to scp
 * @param id:   IPI ID
 * @param buf:  the pointer of data
 * @param len:  data length
 * @param wait: If true, spin until the SCP has taken the data
 */
enum scp_ipi_status scp_ipi_send(enum ipi_id id, void *buf,
	unsigned int  len, unsigned int wait, enum scp_core_id scp_id)
{
	/* reading the id back checks the SRAM took the write */
	enum ipi_id rb_id;

	if (id >= SCP_NR_IPI) {
		pr_err("[SCP] %s: ipi id %d wrong\n", __func__, id);
		return SCP_IPI_ERROR;
	}

	/*avoid scp log print too much*/
	if (scp_ipi_id_record == id)
		scp_ipi_id_record_count++;
	else
		scp_ipi_id_record_count = 0;

	scp_ipi_id_record = id;

	if (scp_id >= SCP_CORE_TOTAL) {
		pr_err("[SCP] %s: scp_id:%d wrong\n", __func__, scp_id);
		scp_ipi_desc[id].error_count++;
		return SCP_IPI_ERROR;
	}

	if (in_interrupt()) {
		if (wait) {
			pr_err("[SCP] %s: cannot use in isr\n", __func__);
			scp_ipi_desc[id].error_count++;
			return SCP_IPI_ERROR;
		}
	}

	if (is_scp_ready(scp_id) == 0) {
		pr_notice("[SCP] %s: %s not ready\n", __func__,
			core_ids[scp_id]);
		scp_ipi_desc[id].error_count++;
		return SCP_IPI_ERROR;
	}

	if (len > sizeof(scp_send_obj[scp_id]->share_buf) || buf == NULL) {
		pr_err("[SCP] %s: %s buffer error\n", __func__,
			core_ids[scp_id]);
		scp_ipi_desc[id].error_count++;
		return SCP_IPI_ERROR;
	}

	if (mutex_trylock(&scp_ipi_mutex[scp_id]) == 0) {
		/*avoid scp ipi send log print too much*/
		if ((scp_ipi_id_record_count % PRINT_THRESHOLD == 0) ||
			(scp_ipi_id_record_count % PRINT_THRESHOLD == 1)) {
			pr_err("[SCP] %s:%s %d mutex busy, owner=%d\n",
				__func__, core_ids[scp_id], id,
				scp_ipi_mutex_owner[scp_id]);
		}
		scp_ipi_desc[id].busy_count++;
		return SCP_IPI_BUSY;
	}

	/* keep scp awake for sram copy*/
	if (scp_awake_lock(scp_id) == -1) {
		mutex_unlock(&scp_ipi_mutex[scp_id]);
		pr_err("[SCP] %s: %s ipi error, awake scp fail\n", __func__,
			core_ids[scp_id]);
		scp_ipi_desc[id].error_count++;
		return SCP_IPI_ERROR;
	}

	/*get scp ipi mutex owner*/
	scp_ipi_mutex_owner[scp_id] = id;

	if ((readl(SCP_GIPC_IN_REG) & (1<<scp_id)) > 0) {
		/*avoid scp ipi send log print too much*/
		if ((scp_ipi_id_record_count % PRINT_THRESHOLD == 0) ||
			(scp_ipi_id_record_count % PRINT_THRESHOLD == 1)) {
			pr_err("[SCP] %s:%s %d ap->scp busy,last time=%d\n",
				__func__, core_ids[scp_id], id,
				scp_ipi_owner[scp_id]);
			scp_A_dump_regs();
		}
		if (scp_awake_unlock(scp_id) == -1)
			pr_debug("[SCP] %s: ap->scp busy awake unlock -1\n",
				__func__);
		scp_ipi_desc[id].busy_count++;
		mutex_unlock(&scp_ipi_mutex[scp_id]);
		return SCP_IPI_BUSY;
	}

	/*get scp ipi send owner*/
	scp_ipi_owner[scp_id] = id;

	memcpy(scp_send_buff[scp_id], buf, len);
	memcpy_to_scp(scp_send_obj[scp_id]->share_buf,
		scp_send_buff[scp_id], len);
	writel(len, &scp_send_obj[scp_id]->len);
	writel(id, &scp_send_obj[scp_id]->id);

	rb_id = readl(&scp_send_obj[scp_id]->id);
	if (rb_id != id) {
		pr_debug("[SCP]ERR: write/read id failed, %d, %d\n", id, rb_id);
		WARN_ON(1);
	}

	dsb(sy);

	scp_ipi_desc[id].success_count++;
	ap_to_scp_ipi_count++;

	/*send host to scp ipi*/
	writel((1<<scp_id), SCP_GIPC_IN_REG);

	if (wait)
		while ((readl(SCP_GIPC_IN_REG) & (1<<scp_id)) > 0)
			;
	/*send host to scp ipi cpmplete, unlock mutex*/
	if (scp_awake_unlock(scp_id) == -1)
		pr_debug("[SCP] %s: awake unlock fail\n", __func__);

	mutex_unlock(&scp_ipi_mutex[scp_id]);

	return SCP_IPI_DONE;
}
EXPORT_SYMBOL_GPL(scp_ipi_send);

static void scp_ipi_info_dump(enum ipi_id id)
{
	pr_notice("%u\t%u\t%u\t%u\t%u\t%s\n",
				id,
				scp_ipi_desc[id].recv_count,
				scp_ipi_desc[id].success_count,
				scp_ipi_desc[id].busy_count,
				scp_ipi_desc[id].error_count,
				scp_ipi_desc[id].name);
}

void scp_ipi_status_dump_id(enum ipi_id id)
{
	pr_notice("[SCP]id\trecv\tsuccess\tbusy\terror\tname\n");
	scp_ipi_info_dump(id);
}

void scp_ipi_status_dump(void)
{
	enum ipi_id id;

	pr_notice("[SCP]id\trecv\tsuccess\tbusy\terror\tname\n");
	for (id = 0; id < SCP_NR_IPI; id++) {
		if (scp_ipi_desc[id].recv_count > 0 ||
			scp_ipi_desc[id].success_count > 0 ||
			scp_ipi_desc[id].busy_count > 0 ||
			scp_ipi_desc[id].error_count > 0)
			scp_ipi_info_dump(id);
	}
	pr_notice("ap->scp total=%u scp->ap total=%u\n",
			ap_to_scp_ipi_count,
			scp_to_ap_ipi_count);
}
