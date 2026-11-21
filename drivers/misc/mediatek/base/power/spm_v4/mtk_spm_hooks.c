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
 * The SPM calls DISP and Mali make, without the rest of the SPM. The bodies
 * are T's: mtk_idle.c:236-277, mtk_spm_sodi.c:86,606-614 and
 * mtk_spm_sodi3p0.c:78,416-424. The idle entry code that runs the notifier
 * chain and reads the SODI flags (mtk_idle.c, cpuidle_v3) is not built, so
 * the chain never fires and the flags are only stored.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/kallsyms.h>
#include <linux/printk.h>

#include "mtk_idle.h"
#include "mtk_spm_idle.h"

/* idle_notifier */
static RAW_NOTIFIER_HEAD(mtk_idle_notifier);

int mtk_idle_notifier_register(struct notifier_block *n)
{
	int ret = 0;
	int index = 0;
#ifdef CONFIG_KALLSYMS
	char namebuf[128] = {0};
	const char *symname = NULL;

	symname = kallsyms_lookup((unsigned long)n->notifier_call,
			NULL, NULL, NULL, namebuf);
	if (symname) {
		printk_deferred("[name:spm&][mt_idle_ntf] <%02d>%08lx (%s)\n",
			index++, (unsigned long)n->notifier_call, symname);
	} else {
		printk_deferred("[name:spm&][mt_idle_ntf] <%02d>%08lx\n",
			index++, (unsigned long)n->notifier_call);
	}
#else
	printk_deferred("[name:spm&][mt_idle_ntf] <%02d>%08lx\n",
			index++, (unsigned long)n->notifier_call);
#endif

	ret = raw_notifier_chain_register(&mtk_idle_notifier, n);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_idle_notifier_register);

void mtk_idle_notifier_unregister(struct notifier_block *n)
{
	raw_notifier_chain_unregister(&mtk_idle_notifier, n);
}
EXPORT_SYMBOL_GPL(mtk_idle_notifier_unregister);

void mtk_idle_notifier_call_chain(unsigned long val)
{
	raw_notifier_call_chain(&mtk_idle_notifier, val, NULL);
}
EXPORT_SYMBOL_GPL(mtk_idle_notifier_call_chain);

/* SODI */
static bool gSpm_sodi_en;

void spm_enable_sodi(bool en)
{
	gSpm_sodi_en = en;
}

bool spm_get_sodi_en(void)
{
	return gSpm_sodi_en;
}

/* SODI 3.0 */
static bool gSpm_sodi3_en = true;

void spm_enable_sodi3(bool en)
{
	gSpm_sodi3_en = en;
}

bool spm_get_sodi3_en(void)
{
	return gSpm_sodi3_en;
}
