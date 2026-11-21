/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 4.14 snd_soc_platform drivers on 5.10 components. A stock PCM file keeps its
 * snd_pcm_ops table and pcm_new/pcm_free; the component ops call through to them.
 */
#ifndef _MTK_SOC_PCM_COMPONENT_H
#define _MTK_SOC_PCM_COMPONENT_H

#include <sound/pcm.h>
#include <sound/soc.h>

struct mtk_soc_platform_driver {
	struct snd_soc_component_driver component;
	const struct snd_pcm_ops *ops;
	int (*pcm_new)(struct snd_soc_pcm_runtime *rtd);
	void (*pcm_free)(struct snd_pcm *pcm);
};

/* name is the component name in debugfs; dai links still match the device name. */
#define MTK_SOC_PLATFORM_DRIVER(_name, _ops, _probe) {	\
	.component = {					\
		.name = _name,				\
		.probe = _probe,			\
	},						\
	.ops = _ops,					\
}

/* Fills in the component ops for the stock ops that are set, then registers it. */
int mtk_soc_register_platform(struct device *dev,
			      struct mtk_soc_platform_driver *drv);

#endif
