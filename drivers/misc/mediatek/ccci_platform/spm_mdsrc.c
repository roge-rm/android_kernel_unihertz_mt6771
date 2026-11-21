// SPDX-License-Identifier: GPL-2.0
/*
 * SPM calls eccci makes, from 4.14 spm_v4/mtk_spm_sleep_mt6771.c.
 * spm_ap_mdsrc_req asks ATF (SMC MTK_SIP_KERNEL_SPM_AP_MDSRC_REQ) to raise
 * or drop the AP's request for the modem's 26 MHz source, counted, and on
 * raise waits for the ack in AP_MDSRC_REQ. spm_is_md1_sleep reads MD1's
 * clock enable from PCM_REG13_DATA; eccci polls it when it stops MD1.
 * The kernel only reads SPM here; the request itself is written by ATF.
 * These override the weak versions in ccci_util_dummy.c.
 */

#define pr_fmt(fmt) "[SPM] " fmt

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>

#include <mtk_secure_api.h>

/* SPM registers, from 4.14 mtk_spm_reg_mt6771.h and pcm_def_mt6771.h */
#define PCM_REG13_DATA_OFS	0x134
#define AP_MDSRC_REQ_OFS	0x430
#define R13_MD1_SRCCLKENA	(1U << 2)
#define R13_MD1_APSRC_REQ	(1U << 3)
#define AP_MDSMSRC_ACK_LSB	(1U << 4)

static void __iomem *mdsrc_spm_base;
static DEFINE_SPINLOCK(__spm_lock);
int spm_ap_mdsrc_req_cnt;

#define spm_read(ofs)	readl(mdsrc_spm_base + (ofs))
#define SMC_CALL(_name, _arg0, _arg1, _arg2) \
	mt_secure_call(_name, _arg0, _arg1, _arg2, 0)

/* Maps SPM from the mediatek,sleep node as 4.14 spm_module_init does; the board DT leaves it disabled, which of_iomap ignores. */
static int __init spm_mdsrc_init(void)
{
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "mediatek,sleep");
	if (!node) {
		pr_err("%s: no mediatek,sleep node\n", __func__);
		return 0;
	}
	mdsrc_spm_base = of_iomap(node, 0);
	of_node_put(node);
	if (!mdsrc_spm_base)
		pr_err("%s: SPM map failed\n", __func__);
	return 0;
}
subsys_initcall(spm_mdsrc_init);

bool spm_is_md1_sleep(void)
{
	if (!mdsrc_spm_base)
		return true;
	return !(spm_read(PCM_REG13_DATA_OFS) & R13_MD1_SRCCLKENA);
}
EXPORT_SYMBOL(spm_is_md1_sleep);

void spm_ap_mdsrc_req(u8 set)
{
	unsigned long flags;
	u32 i = 0;
	u32 md_sleep = 0;

	if (!mdsrc_spm_base) {
		pr_err("%s(%u): SPM not mapped\n", __func__, set);
		return;
	}

	if (set) {
		spin_lock_irqsave(&__spm_lock, flags);

		if (spm_ap_mdsrc_req_cnt < 0) {
			pr_err("warning: set = %d, spm_ap_mdsrc_req_cnt = %d\n",
			       set, spm_ap_mdsrc_req_cnt);
			spin_unlock_irqrestore(&__spm_lock, flags);
		} else {
			spm_ap_mdsrc_req_cnt++;

			SMC_CALL(MTK_SIP_KERNEL_SPM_AP_MDSRC_REQ, 1, 0, 0);

			spin_unlock_irqrestore(&__spm_lock, flags);

			/* if md_apsrc_req = 1'b0, */
			/* wait 26M settling time (3ms) */
			if ((spm_read(PCM_REG13_DATA_OFS) &
			     R13_MD1_APSRC_REQ) == 0) {
				md_sleep = 1;
				mdelay(3);
			}

			/* Check ap_mdsrc_ack = 1'b1 */
			while ((spm_read(AP_MDSRC_REQ_OFS) &
				AP_MDSMSRC_ACK_LSB) == 0) {
				if (i++ < 10) {
					mdelay(1);
				} else {
					pr_err("WARNING: MD SLEEP = %d, %s CAN NOT polling AP_MD1SRC_ACK\n",
					       md_sleep, __func__);
					break;
				}
			}
		}
	} else {
		spin_lock_irqsave(&__spm_lock, flags);

		spm_ap_mdsrc_req_cnt--;

		if (spm_ap_mdsrc_req_cnt < 0) {
			pr_err("warning: set = %d, spm_ap_mdsrc_req_cnt = %d\n",
			       set, spm_ap_mdsrc_req_cnt);
		} else {
			if (spm_ap_mdsrc_req_cnt == 0)
				SMC_CALL(MTK_SIP_KERNEL_SPM_AP_MDSRC_REQ,
					 0, 0, 0);
		}

		spin_unlock_irqrestore(&__spm_lock, flags);
	}
}
