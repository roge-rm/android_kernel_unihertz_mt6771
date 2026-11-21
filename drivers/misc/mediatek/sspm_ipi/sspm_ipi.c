// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 SSPM mailbox and IPI core, cut down from T's sspm v1 (helper, mbox,
 * ipi_mbox, reservedmem, plt, logger) and its mt6771 files.
 *
 * The preloader loads and starts SSPM on every boot, so this driver loads no
 * firmware and releases no reset. It maps cfgreg and mailboxes 0 to 4, checks
 * the pin tags SSPM wrote into mailboxes 0 to 2 against T's pin layout, then
 * sends PLT_INIT with the logger buffer in reserve-memory-sspm_share.
 *
 * Every wait has a timeout. A silent SSPM only logs: no BUG, no panic, and
 * PLT_INIT runs from a work item so boot never waits for it.
 *
 * Mailbox 3 (MCDI and timestamps) and mailbox 4 are mapped without their
 * IRQs. The only write to either is sspm_mcdi_mbox_write(): three MCDI data
 * slots of mailbox 3 for the MCDI client. Nothing
 * writes their control registers.
 */
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk-sspm.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define SSPM_MBOX_MAX		5
#define SSPM_MBOX_USED		4	/* T's IPI_MBOX_TOTAL: mailboxes 0 to 3 */
#define SSPM_MBOX_SLOTS		32	/* 4-byte slots, IPI_MBOX_MODE 0 */
#define SSPM_MBOX_IN_IRQ	0x0
#define SSPM_MBOX_OUT_IRQ	0x4
#define SSPM_TIMEOUT_MS		2000	/* T's TIMEOUT_COMPLETE, for IPI_OPT_WAIT */
/* Polling spins with a spinlock held and IRQs off, so it stops after 10 ms */
#define SSPM_POLL_TIMEOUT_US	10000

#define PLT_INIT		0x504C5401
#define PLT_LOG_ENABLE		0x504C5402
#define PLT_LOGGER_BUF_LEN	0x100000

#define SSPM_MEM_COMPAT		"mediatek,reserve-memory-sspm_share"

/* A send pin: mailbox, first slot, slots used, ack returns data */
struct sspm_pin {
	u8 mbox, slot, size, retdata;
};

/* T's mt6771 sspm_ipi_define.h send_pintable, slots from sspm_ipi_mbox_layout.h */
static const struct sspm_pin send_pins[IPI_ID_TOTAL] = {
	[IPI_ID_PLATFORM]	= { 0, 0, 3, 1 },
	[IPI_ID_CPU_DVFS]	= { 0, 3, 4, 1 },
	[IPI_ID_QOS]		= { 0, 7, 4, 1 },
	[IPI_ID_TST1]		= { 0, 11, 4, 1 },
	[IPI_ID_FHCTL]		= { 0, 15, 9, 1 },
	[IPI_ID_MCDI]		= { 1, 0, 2, 1 },
	[IPI_ID_SPM_SUSPEND]	= { 1, 2, 8, 1 },
	[IPI_ID_PMIC]		= { 1, 10, 5, 1 },
	[IPI_ID_PPM]		= { 1, 15, 7, 1 },
	[IPI_ID_THERMAL]	= { 1, 22, 4, 1 },
	[IPI_ID_UPOWER]		= { 1, 26, 4, 1 },
};

/* recv_pintable: PLATFORM, CPU_DVFS, QOS, TST1 on mailbox 2 */
static const struct sspm_pin recv_pins[] = {
	{ 2, 0, 3, 1 }, { 2, 3, 4, 1 }, { 2, 7, 4, 1 }, { 2, 11, 4, 1 },
};

/* mbox_table: first and last pin, mode 2 send, 1 receive, 0 off */
static const struct { u8 start, end, mode; } mbox_table[SSPM_MBOX_USED] = {
	{ 0, 4, 2 }, { 5, 10, 2 }, { 0, 3, 1 }, { 0, 0, 0 },
};

/* reserve-memory-sspm_share blocks in id order, T's sspm_reservedmem_define.h */
static const u32 mem_block_size[NUMS_MEM_ID] = {
	[SSPM_MEM_ID]	= 0x400 + PLT_LOGGER_BUF_LEN,
	[PWRAP_MEM_ID]	= 0x300,
	[PMIC_MEM_ID]	= 0xC00,
	[UPD_MEM_ID]	= 0x1800,
	[QOS_MEM_ID]	= 0x1000,
	[SWPM_MEM_ID]	= 0xC00,
	[MET_MEM_ID]	= 0x400000,
};

/* plt_ctrl_s with the logger only (stock has coredump and timesync off) */
struct plt_ctrl {
	u32 magic, size, mem_sz, logger_ofs;
};

struct log_ctrl {
	u32 base, size, enable, info_ofs, buff_ofs, buff_size;
};

struct log_pos {
	u32 r_pos, w_pos;
};

struct sspm {
	struct device *dev;
	u8 __iomem *cfg;
	u8 __iomem *base[SSPM_MBOX_MAX];
	u8 __iomem *ctrl[SSPM_MBOX_MAX];
	int irq_ipc;
	spinlock_t mbox_lock[SSPM_MBOX_USED];
	unsigned long irq_count[SSPM_MBOX_USED];

	struct mutex pin_mutex[IPI_ID_TOTAL];
	spinlock_t poll_lock[IPI_ID_TOTAL];
	struct completion pin_ack[IPI_ID_TOTAL];
	atomic_t acked[IPI_ID_TOTAL];
	u32 seqno[IPI_ID_TOTAL], timeouts[IPI_ID_TOTAL];

	phys_addr_t mem_phys, mem_size;
	void *mem_virt;
	phys_addr_t block_phys[NUMS_MEM_ID];

	int pin_check;		/* 0 ok, else slots that did not match */
	int plt_ret;		/* 1 not sent yet, then the IPI result */
	u32 plt_ack;
	struct log_ctrl *log_ctl;
	struct log_pos *log_pos;
	struct mutex log_mutex;
	struct work_struct plt_work;
	struct dentry *debugfs;
};

/* Static, so the IRQ handlers never see it freed after a failed probe */
static struct sspm sspm_dev, *const sspm = &sspm_dev;
static bool sspm_probed, sspm_ready, ipi_ready;
/* Sends in flight on any pin but SPM_SUSPEND, T's ipi_lock_cnt */
static atomic_t ipi_lock_cnt = ATOMIC_INIT(0);

static void mbox_read(int mbox, int slot, u32 *data, int len)
{
	const u32 __iomem *src = (u32 __iomem *)(sspm->base[mbox] + slot * 4);

	while (len--)
		*data++ = readl_relaxed(src++);
}

/* Mailboxes 0 and 1 only: send pins never sit on 3 or 4 */
static int mbox_send(int mbox, int slot, int irq, const u32 *data, int len)
{
	u32 __iomem *dst;
	unsigned long flags;

	if (mbox > 1 || slot + len > SSPM_MBOX_SLOTS)
		return -EINVAL;
	if (readl(sspm->ctrl[mbox] + SSPM_MBOX_OUT_IRQ) & BIT(irq)) {
		dev_err(sspm->dev, "mbox%d out_irq %d not clear\n", mbox, irq);
		return -EBUSY;
	}
	dst = (u32 __iomem *)(sspm->base[mbox] + slot * 4);
	while (len--)
		writel_relaxed(*data++, dst++);
	spin_lock_irqsave(&sspm->mbox_lock[mbox], flags);
	writel(BIT(irq), sspm->ctrl[mbox] + SSPM_MBOX_IN_IRQ);	/* ordered after the data */
	spin_unlock_irqrestore(&sspm->mbox_lock[mbox], flags);
	return 0;
}

/* Take the ack bit if SSPM set it: clear it and copy the return data */
static bool mbox_poll(int mid, u32 *retbuf, int retlen)
{
	const struct sspm_pin *pin = &send_pins[mid];
	int bit = mid - mbox_table[pin->mbox].start;
	u8 __iomem *out = sspm->ctrl[pin->mbox] + SSPM_MBOX_OUT_IRQ;
	unsigned long flags;
	bool hit;

	spin_lock_irqsave(&sspm->mbox_lock[pin->mbox], flags);
	hit = readl(out) & BIT(bit);
	if (hit)
		writel(BIT(bit), out);
	spin_unlock_irqrestore(&sspm->mbox_lock[pin->mbox], flags);
	if (hit || atomic_read(&sspm->acked[mid])) {
		if (retbuf)
			mbox_read(pin->mbox, pin->slot, retbuf, retlen);
		return true;
	}
	return false;
}

static irqreturn_t sspm_mbox_irq(int irq, void *data)
{
	int mbox = (long)data, i;
	u32 irqs;

	spin_lock(&sspm->mbox_lock[mbox]);
	irqs = readl(sspm->ctrl[mbox] + SSPM_MBOX_OUT_IRQ);
	writel(irqs, sspm->ctrl[mbox] + SSPM_MBOX_OUT_IRQ);
	spin_unlock(&sspm->mbox_lock[mbox]);
	sspm->irq_count[mbox]++;

	/* Acks on the send mailboxes; receive pins have no users yet */
	if (mbox_table[mbox].mode == 2)
		for (i = mbox_table[mbox].start; i <= mbox_table[mbox].end; i++)
			if (irqs & BIT(i - mbox_table[mbox].start)) {
				atomic_set(&sspm->acked[i], 1);
				complete(&sspm->pin_ack[i]);
			}
	return IRQ_HANDLED;
}

/*
 * Send buffer (slot words, 0 for the pin's size) on pin mid and wait for the
 * ack, copying retslot words back. IPI_OPT_WAIT sleeps on the mailbox IRQ,
 * IPI_OPT_POLLING spins with IRQs off and works in atomic context. Wait gives
 * up after 2 s, polling after 10 ms, with IPI_TIMEOUT_ACK and a log line.
 */
int sspm_ipi_send_sync(int mid, int opts, void *buffer, int slot,
		       void *retbuf, int retslot)
{
	const struct sspm_pin *pin;
	unsigned long flags = 0;
	int ret, us;

	if (!ipi_ready)
		return IPI_SERVICE_NOT_INITED;
	if (mid < 0 || mid >= IPI_ID_TOTAL)
		return IPI_SERVICE_NOT_AVAILABLE;
	pin = &send_pins[mid];
	if (slot > pin->size || retslot > pin->size || slot < 0 || retslot < 0)
		return IPI_NO_MEMORY;
	if (!slot)
		slot = pin->size;
	if (!pin->retdata || !retslot)
		retbuf = NULL;
	if (mid != IPI_ID_SPM_SUSPEND)
		atomic_inc(&ipi_lock_cnt);

	if (opts & IPI_OPT_POLLING) {
		spin_lock_irqsave(&sspm->poll_lock[mid], flags);
		if (mutex_is_locked(&sspm->pin_mutex[mid])) {
			spin_unlock_irqrestore(&sspm->poll_lock[mid], flags);
			dev_err(sspm->dev, "IPI %d busy in wait mode\n", mid);
			ret = IPI_USED_IN_WAIT;
			goto unlock_spm;
		}
	} else {
		if (WARN_ONCE(in_atomic() || irqs_disabled(),
			      "sspm: IPI %d wait mode from atomic context\n", mid)) {
			ret = IPI_USED_IN_WAIT;
			goto unlock_spm;
		}
		mutex_lock(&sspm->pin_mutex[mid]);
	}

	sspm->seqno[mid]++;
	atomic_set(&sspm->acked[mid], 0);
	reinit_completion(&sspm->pin_ack[mid]);
	ret = mbox_send(pin->mbox, pin->slot, mid - mbox_table[pin->mbox].start,
			buffer, slot);
	if (ret) {
		ret = IPI_HW_ERROR;
	} else if (opts & IPI_OPT_POLLING) {
		ret = IPI_TIMEOUT_ACK;
		for (us = 0; us < SSPM_POLL_TIMEOUT_US; us++) {
			if (mbox_poll(mid, retbuf, retslot)) {
				ret = IPI_DONE;
				break;
			}
			udelay(1);
		}
	} else {
		wait_for_completion_timeout(&sspm->pin_ack[mid],
					    msecs_to_jiffies(SSPM_TIMEOUT_MS));
		/* Check the bit too, in case the IRQ never came */
		ret = mbox_poll(mid, retbuf, retslot) ? IPI_DONE : IPI_TIMEOUT_ACK;
	}
	atomic_set(&sspm->acked[mid], 0);
	if (ret == IPI_TIMEOUT_ACK) {
		sspm->timeouts[mid]++;
		dev_err(sspm->dev, "IPI %d (opts %d, seq %u) no ack in %d us\n",
			mid, opts, sspm->seqno[mid], opts & IPI_OPT_POLLING ?
			SSPM_POLL_TIMEOUT_US : SSPM_TIMEOUT_MS * 1000);
	}

	if (opts & IPI_OPT_POLLING)
		spin_unlock_irqrestore(&sspm->poll_lock[mid], flags);
	else
		mutex_unlock(&sspm->pin_mutex[mid]);
unlock_spm:
	if (mid != IPI_ID_SPM_SUSPEND)
		atomic_dec(&ipi_lock_cnt);
	return ret;
}
EXPORT_SYMBOL_GPL(sspm_ipi_send_sync);

/* T's is_sspm_ipi_lock_spm: true while another pin waits for an ack; SPM suspend must wait */
bool is_sspm_ipi_lock_spm(void)
{
	return atomic_read(&ipi_lock_cnt) != 0;
}
EXPORT_SYMBOL_GPL(is_sspm_ipi_lock_spm);

int sspm_ipi_is_inited(void)
{
	return ipi_ready;
}
EXPORT_SYMBOL_GPL(sspm_ipi_is_inited);

unsigned int is_sspm_ready(void)
{
	return sspm_ready;
}
EXPORT_SYMBOL_GPL(is_sspm_ready);

/* Read len slots from any mailbox, 0 to 4 */
int sspm_mbox_read(unsigned int mbox, unsigned int slot, void *data,
		   unsigned int len)
{
	if (!sspm_ready || mbox >= SSPM_MBOX_MAX || slot + len > SSPM_MBOX_SLOTS)
		return -EINVAL;
	mbox_read(mbox, slot, data, len);
	return 0;
}
EXPORT_SYMBOL_GPL(sspm_mbox_read);

/*
 * The one mailbox 3 writer, for mtk_mcdi: AVAIL_CPU_MASK, CPU_ISOLATION_MASK
 * and PAUSE_ACTION only. A plain data write as T's sspm_mbox_write, no IRQ.
 */
int sspm_mcdi_mbox_write(unsigned int slot, u32 val)
{
	if (!sspm_ready)
		return -ENODEV;
	if (slot != MCDI_MBOX_AVAIL_CPU_MASK && slot != MCDI_MBOX_CPU_ISOLATION_MASK &&
	    slot != MCDI_MBOX_PAUSE_ACTION)
		return -EINVAL;
	writel(val, sspm->base[SHAREMBOX_NO_MCDI] + (SHAREMBOX_OFFSET_MCDI + slot) * 4);
	return 0;
}
EXPORT_SYMBOL_GPL(sspm_mcdi_mbox_write);

phys_addr_t sspm_reserve_mem_get_phys(unsigned int id)
{
	return sspm_ready && sspm->mem_virt && id < NUMS_MEM_ID ? sspm->block_phys[id] : 0;
}
EXPORT_SYMBOL_GPL(sspm_reserve_mem_get_phys);

phys_addr_t sspm_reserve_mem_get_virt(unsigned int id)
{
	if (!sspm_ready || !sspm->mem_virt || id >= NUMS_MEM_ID)
		return 0;
	return (phys_addr_t)(uintptr_t)sspm->mem_virt + (sspm->block_phys[id] - sspm->mem_phys);
}
EXPORT_SYMBOL_GPL(sspm_reserve_mem_get_virt);

phys_addr_t sspm_reserve_mem_get_size(unsigned int id)
{
	return sspm_ready && sspm->mem_virt && id < NUMS_MEM_ID ? mem_block_size[id] : 0;
}
EXPORT_SYMBOL_GPL(sspm_reserve_mem_get_size);

/*
 * T's check_table_tag: SSPM fills each slot of each pin in mailboxes 0 to 2
 * with 'M' << 24 | dir << 16 | pin << 8 | size, dir 1 for send, 2 for receive.
 * Returns the number of slots that differ.
 */
static int sspm_check_pins(void)
{
	int mbox, j, k, bad = 0;

	for (mbox = 0; mbox < SSPM_MBOX_USED; mbox++) {
		int mode = mbox_table[mbox].mode;

		if (!mode)
			continue;
		for (j = mbox_table[mbox].start; j <= mbox_table[mbox].end; j++) {
			const struct sspm_pin *pin = mode == 2 ? &send_pins[j] : &recv_pins[j];
			u32 tag = 'M' << 24 | (mode == 2 ? 1 : 2) << 16 | j << 8 | pin->size;

			for (k = 0; k < pin->size; k++) {
				u32 got;

				mbox_read(mbox, pin->slot + k, &got, 1);
				if (got != tag && bad++ < 4)
					dev_err(sspm->dev, "pin tag mbox%d pin %d slot %d: %08x, want %08x\n",
						mbox, j, pin->slot + k, got, tag);
			}
		}
	}
	return bad;
}

/*
 * reserve-memory-sspm_share: no-map and placed by the kernel at boot (LK
 * only sets its status). Blocks follow each other from the base, as T.
 */
static int sspm_map_memory(void)
{
	struct device_node *np;
	struct reserved_mem *rmem;
	phys_addr_t ofs = 0;
	int id;

	np = of_find_compatible_node(NULL, NULL, SSPM_MEM_COMPAT);
	rmem = np ? of_reserved_mem_lookup(np) : NULL;
	of_node_put(np);
	if (!rmem || !rmem->size) {
		dev_err(sspm->dev, "no %s region\n", SSPM_MEM_COMPAT);
		return -ENOMEM;
	}
	for (id = 0; id < NUMS_MEM_ID; id++) {
		sspm->block_phys[id] = rmem->base + ofs;
		ofs += mem_block_size[id];
	}
	if (ofs > rmem->size || rmem->base + rmem->size > SZ_4G) {
		dev_err(sspm->dev, "region %pa size %pa can't hold 0x%llx below 4 GB\n",
			&rmem->base, &rmem->size, (u64)ofs);
		return -ENOMEM;
	}
	sspm->mem_virt = memremap(rmem->base, rmem->size, MEMREMAP_WC);
	if (!sspm->mem_virt)
		return -ENOMEM;
	sspm->mem_phys = rmem->base;
	sspm->mem_size = rmem->size;
	dev_info(sspm->dev, "share %pa size %pa\n", &rmem->base, &rmem->size);
	return 0;
}

/* T's sspm_plt_init and sspm_logger_init: the headers, then PLT_INIT */
static void sspm_plt_work(struct work_struct *work)
{
	u32 mem_sz = mem_block_size[SSPM_MEM_ID];
	u8 *virt = (u8 *)(uintptr_t)sspm_reserve_mem_get_virt(SSPM_MEM_ID);
	struct plt_ctrl *plt = (struct plt_ctrl *)virt;
	struct log_ctrl *log = (struct log_ctrl *)(virt + sizeof(*plt));
	struct log_pos *pos = (struct log_pos *)(log + 1);
	u32 msg[3];

	memset(plt, 0, sizeof(*plt));
	*(u32 *)(virt + mem_sz - 4) = PLT_INIT;
	plt->magic = PLT_INIT;
	plt->size = sizeof(*plt);
	plt->mem_sz = mem_sz;
	plt->logger_ofs = sizeof(*plt);

	log->base = PLT_LOG_ENABLE;
	log->size = sizeof(*log);
	log->enable = 0;
	log->info_ofs = sizeof(*log);
	log->buff_ofs = sizeof(*log) + sizeof(*pos);
	log->buff_size = PLT_LOGGER_BUF_LEN;
	pos->r_pos = 0;
	pos->w_pos = 0;
	wmb();

	msg[0] = PLT_INIT;
	msg[1] = sspm_reserve_mem_get_phys(SSPM_MEM_ID);
	msg[2] = mem_sz;
	sspm->plt_ret = sspm_ipi_send_sync(IPI_ID_PLATFORM, IPI_OPT_WAIT, msg,
					   ARRAY_SIZE(msg), &sspm->plt_ack, 1);
	if (sspm->plt_ret || !sspm->plt_ack) {
		dev_err(sspm->dev, "PLT_INIT failed: ret %d ack %u\n",
			sspm->plt_ret, sspm->plt_ack);
		return;
	}
	sspm->log_ctl = log;
	sspm->log_pos = pos;
	dev_info(sspm->dev, "SSPM platform service is ready\n");
}

static int sspm_log_enable(u32 enable)
{
	u32 msg[3] = { PLT_LOG_ENABLE, enable, 0 }, ack = 0;
	int ret;

	ret = sspm_ipi_send_sync(IPI_ID_PLATFORM, IPI_OPT_WAIT, msg,
				 ARRAY_SIZE(msg), &ack, 1);
	if (ret || ack != enable)
		return ret ? ret : -EIO;
	WRITE_ONCE(sspm->log_ctl->enable, enable);
	return 0;
}

static const char * const mcdi_slot_name[SHAREMBOX_SIZE_MCDI] = {
	"CLUSTER_0_CAN_POWER_OFF", "CLUSTER_1_CAN_POWER_OFF", "BUCK_POWER_OFF_MASK",
	"CLUSTER_0_ATF_ACTION_DONE", "CLUSTER_1_ATF_ACTION_DONE", "RESERVED",
	"PAUSE_ACTION", "AVAIL_CPU_MASK", "CPU_CLUSTER_PWR_STAT", "ACTION_STAT",
	"CLUSTER_0_CNT", "CLUSTER_1_CNT", "CPU_ISOLATION_MASK", "PAUSE_ACK",
	"PENDING_ON_EVENT", "PROF_CMD", "PROF_CLUSTER", "AP_READY", "", "",
};

static int mbox3_show(struct seq_file *m, void *v)
{
	u32 val[SHAREMBOX_SIZE_MCDI];
	int i;

	mbox_read(SHAREMBOX_NO_MCDI, SHAREMBOX_OFFSET_MCDI, val, SHAREMBOX_SIZE_MCDI);
	for (i = 0; i < SHAREMBOX_SIZE_MCDI; i++)
		seq_printf(m, "%2d 0x%08x %s\n", i, val[i], mcdi_slot_name[i]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mbox3);

static int status_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "pin check: %s (%d bad slots)\n", ipi_ready ? "ok" : "failed",
		   sspm->pin_check);
	seq_printf(m, "PLT_INIT: ret %d ack %u\n", sspm->plt_ret, sspm->plt_ack);
	seq_printf(m, "share: %pa size %pa\n", &sspm->mem_phys, &sspm->mem_size);
	seq_printf(m, "ipc irq: %d\n", sspm->irq_ipc);
	for (i = 0; i < SSPM_MBOX_USED; i++)
		seq_printf(m, "mbox%d irqs: %lu\n", i, sspm->irq_count[i]);
	for (i = 0; i < IPI_ID_TOTAL; i++)
		seq_printf(m, "ipi %2d: sent %u timeouts %u\n", i, sspm->seqno[i],
			   sspm->timeouts[i]);
	if (sspm->log_ctl)
		seq_printf(m, "logger: enable %u r_pos 0x%x w_pos 0x%x\n",
			   READ_ONCE(sspm->log_ctl->enable),
			   READ_ONCE(sspm->log_pos->r_pos), READ_ONCE(sspm->log_pos->w_pos));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

/* T's sspm_log_read: hands out what SSPM wrote and moves r_pos on */
static ssize_t log_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	u32 r, w, n;
	ssize_t ret = 0;

	if (!sspm->log_ctl)
		return 0;
	mutex_lock(&sspm->log_mutex);
	r = READ_ONCE(sspm->log_pos->r_pos);
	w = READ_ONCE(sspm->log_pos->w_pos);
	if (r >= PLT_LOGGER_BUF_LEN || w >= PLT_LOGGER_BUF_LEN) {
		ret = -EIO;
		goto out;
	}
	if (r == w)
		goto out;
	n = r > w ? PLT_LOGGER_BUF_LEN - r : w - r;
	n = min_t(size_t, n, len);
	if (copy_to_user(ubuf, (u8 *)sspm->log_ctl + sspm->log_ctl->buff_ofs + r, n)) {
		ret = -EFAULT;
		goto out;
	}
	r += n;
	WRITE_ONCE(sspm->log_pos->r_pos, r >= PLT_LOGGER_BUF_LEN ? 0 : r);
	ret = n;
out:
	mutex_unlock(&sspm->log_mutex);
	return ret;
}

static const struct file_operations log_fops = {
	.read = log_read,
	.llseek = noop_llseek,
};

static int log_enable_get(void *data, u64 *val)
{
	*val = sspm->log_ctl ? READ_ONCE(sspm->log_ctl->enable) : 0;
	return 0;
}

static int log_enable_set(void *data, u64 val)
{
	int ret;

	if (!sspm->log_ctl)
		return -ENODEV;
	mutex_lock(&sspm->log_mutex);
	ret = sspm_log_enable(!!val);
	mutex_unlock(&sspm->log_mutex);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(log_enable_fops, log_enable_get, log_enable_set, "%llu\n");

static int sspm_probe(struct platform_device *pdev)
{
	char name[16];
	long i;
	int irq, ret;

	if (sspm_probed)
		return -EBUSY;
	sspm_probed = true;
	sspm->dev = &pdev->dev;
	sspm->plt_ret = 1;
	mutex_init(&sspm->log_mutex);
	INIT_WORK(&sspm->plt_work, sspm_plt_work);
	for (i = 0; i < IPI_ID_TOTAL; i++) {
		mutex_init(&sspm->pin_mutex[i]);
		spin_lock_init(&sspm->poll_lock[i]);
		init_completion(&sspm->pin_ack[i]);
	}

	sspm->cfg = devm_platform_ioremap_resource_byname(pdev, "cfgreg");
	if (IS_ERR(sspm->cfg))
		goto fail_map;
	for (i = 0; i < SSPM_MBOX_MAX; i++) {
		snprintf(name, sizeof(name), "mbox%ld_base", i);
		sspm->base[i] = devm_platform_ioremap_resource_byname(pdev, name);
		snprintf(name, sizeof(name), "mbox%ld_ctrl", i);
		sspm->ctrl[i] = devm_platform_ioremap_resource_byname(pdev, name);
		if (IS_ERR(sspm->base[i]) || IS_ERR(sspm->ctrl[i]))
			goto fail_map;
	}
	/* T reads the ipc IRQ but never requests it */
	sspm->irq_ipc = platform_get_irq_byname_optional(pdev, "ipc");

	/* T's sspm_mbox_init requests mailboxes 0 to 3; 3 is left alone here */
	for (i = 0; i < SSPM_MBOX_USED; i++) {
		spin_lock_init(&sspm->mbox_lock[i]);
		if (!mbox_table[i].mode)
			continue;
		snprintf(name, sizeof(name), "mbox%ld", i);
		irq = platform_get_irq_byname(pdev, name);
		ret = irq < 0 ? irq : devm_request_irq(&pdev->dev, irq, sspm_mbox_irq,
						       0, "SSPM_MBOX", (void *)i);
		if (ret) {
			dev_err(&pdev->dev, "mbox%ld IRQ: %d\n", i, ret);
			return ret;
		}
	}
	sspm_ready = true;

	sspm->debugfs = debugfs_create_dir("sspm", NULL);
	debugfs_create_file("mbox3", 0444, sspm->debugfs, NULL, &mbox3_fops);
	debugfs_create_file("status", 0444, sspm->debugfs, NULL, &status_fops);
	debugfs_create_file("log", 0400, sspm->debugfs, NULL, &log_fops);
	debugfs_create_file_unsafe("log_enable", 0600, sspm->debugfs, NULL, &log_enable_fops);

	sspm->pin_check = sspm_check_pins();
	if (sspm->pin_check) {
		dev_err(&pdev->dev, "pin layout differs from T's, IPI off\n");
		return 0;
	}
	ipi_ready = true;
	dev_info(&pdev->dev, "SSPM is ready to service IPI\n");

	if (sspm_map_memory())
		dev_err(&pdev->dev, "no share memory, PLT_INIT not sent\n");
	else
		schedule_work(&sspm->plt_work);
	return 0;

fail_map:
	dev_err(&pdev->dev, "can't map cfgreg or mailboxes\n");
	return -ENXIO;
}

static const struct of_device_id sspm_of_match[] = {
	{ .compatible = "mediatek,sspm" },
	{}
};

static struct platform_driver sspm_driver = {
	.probe = sspm_probe,
	.driver = {
		.name = "mtk-sspm",
		.of_match_table = sspm_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(sspm_driver);
