// SPDX-License-Identifier: GPL-2.0
/*
 * 4.14 snd_soc_platform drivers on 5.10 components (mtk-soc-pcm-component.h). Only the
 * ops a stock table sets get a component op, since the core checks which ones exist
 * (copy_user, page and mmap change how it moves data).
 */
#include <linux/device.h>
#include "mtk-soc-pcm-component.h"

static const struct mtk_soc_platform_driver *
to_mtk_drv(struct snd_soc_component *component)
{
	return container_of(component->driver, struct mtk_soc_platform_driver,
			    component);
}

static int mtk_soc_pcm_open(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	return to_mtk_drv(component)->ops->open(substream);
}

static int mtk_soc_pcm_close(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	return to_mtk_drv(component)->ops->close(substream);
}

static int mtk_soc_pcm_ioctl(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream,
			     unsigned int cmd, void *arg)
{
	return to_mtk_drv(component)->ops->ioctl(substream, cmd, arg);
}

static int mtk_soc_pcm_hw_params(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	return to_mtk_drv(component)->ops->hw_params(substream, params);
}

static int mtk_soc_pcm_hw_free(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	return to_mtk_drv(component)->ops->hw_free(substream);
}

static int mtk_soc_pcm_prepare(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	return to_mtk_drv(component)->ops->prepare(substream);
}

static int mtk_soc_pcm_trigger(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream, int cmd)
{
	return to_mtk_drv(component)->ops->trigger(substream, cmd);
}

static snd_pcm_uframes_t mtk_soc_pcm_pointer(struct snd_soc_component *component,
					     struct snd_pcm_substream *substream)
{
	return to_mtk_drv(component)->ops->pointer(substream);
}

static int mtk_soc_pcm_copy_user(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream,
				 int channel, unsigned long pos,
				 void __user *buf, unsigned long bytes)
{
	return to_mtk_drv(component)->ops->copy_user(substream, channel, pos,
						      buf, bytes);
}

static struct page *mtk_soc_pcm_page(struct snd_soc_component *component,
				     struct snd_pcm_substream *substream,
				     unsigned long offset)
{
	return to_mtk_drv(component)->ops->page(substream, offset);
}

static int mtk_soc_pcm_mmap(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream,
			    struct vm_area_struct *vma)
{
	return to_mtk_drv(component)->ops->mmap(substream, vma);
}

static int mtk_soc_pcm_ack(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	return to_mtk_drv(component)->ops->ack(substream);
}

static int mtk_soc_pcm_construct(struct snd_soc_component *component,
				 struct snd_soc_pcm_runtime *rtd)
{
	return to_mtk_drv(component)->pcm_new(rtd);
}

static void mtk_soc_pcm_destruct(struct snd_soc_component *component,
				 struct snd_pcm *pcm)
{
	to_mtk_drv(component)->pcm_free(pcm);
}

int mtk_soc_register_platform(struct device *dev,
			      struct mtk_soc_platform_driver *drv)
{
	struct snd_soc_component_driver *c = &drv->component;
	const struct snd_pcm_ops *ops = drv->ops;

	if (ops) {
		c->open = ops->open ? mtk_soc_pcm_open : NULL;
		c->close = ops->close ? mtk_soc_pcm_close : NULL;
		c->ioctl = ops->ioctl ? mtk_soc_pcm_ioctl : NULL;
		c->hw_params = ops->hw_params ? mtk_soc_pcm_hw_params : NULL;
		c->hw_free = ops->hw_free ? mtk_soc_pcm_hw_free : NULL;
		c->prepare = ops->prepare ? mtk_soc_pcm_prepare : NULL;
		c->trigger = ops->trigger ? mtk_soc_pcm_trigger : NULL;
		c->pointer = ops->pointer ? mtk_soc_pcm_pointer : NULL;
		c->copy_user = ops->copy_user ? mtk_soc_pcm_copy_user : NULL;
		c->page = ops->page ? mtk_soc_pcm_page : NULL;
		c->mmap = ops->mmap ? mtk_soc_pcm_mmap : NULL;
		c->ack = ops->ack ? mtk_soc_pcm_ack : NULL;
	}
	c->pcm_construct = drv->pcm_new ? mtk_soc_pcm_construct : NULL;
	c->pcm_destruct = drv->pcm_free ? mtk_soc_pcm_destruct : NULL;

	return snd_soc_register_component(dev, c, NULL, 0);
}
EXPORT_SYMBOL_GPL(mtk_soc_register_platform);
