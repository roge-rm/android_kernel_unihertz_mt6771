# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_SND_SOC_MEDIATEK) += common/
obj-$(CONFIG_SND_SOC_MT2701) += mt2701/
obj-$(CONFIG_SND_SOC_MT6797) += mt6797/
obj-$(CONFIG_SND_SOC_MT8173) += mt8173/
obj-$(CONFIG_SND_SOC_MT8183) += mt8183/

# MediaTek's 4.14 audio stack, with stock's flags and include paths.
# Paths stock adds that 5.10 doesn't have (audio_ipi) come with the code that needs them.
ifeq ($(CONFIG_MTK_SND_SOC_NEW_ARCH),y)
MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
MTK_CODEC := $(subst ",,$(CONFIG_MTK_SND_SOC_CODEC))

subdir-ccflags-y += -DAUDIO_USING_WRAP_DRIVER
subdir-ccflags-y += -D_MT_IDLE_HEADER
subdir-ccflags-y += -D_REG_SYNC_WRITE

subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/include
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/include/mt-plat
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/base/power/include
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/base/power/$(MTK_PLATFORM)
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/dramc/$(MTK_PLATFORM)/
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/scp/$(MTK_PLATFORM)/
subdir-ccflags-y += -Werror -I$(srctree)/drivers/misc/mediatek/accdet/mt6358
subdir-ccflags-y += -Werror -I$(srctree)/$(src)/common_int
ifneq ($(MTK_CODEC),)
subdir-ccflags-y += -Werror -I$(srctree)/$(src)/codec/$(MTK_CODEC)
endif
subdir-ccflags-y += -Werror -Wno-incompatible-pointer-types
subdir-ccflags-y += -Werror -I$(srctree)/$(src)/$(MTK_PLATFORM)

# Codec first, as stock, so the codec registers before the AFE probes.
ifneq ($(MTK_CODEC),)
obj-y += codec/$(MTK_CODEC)/
endif
obj-y += common_int/
obj-y += $(MTK_PLATFORM)/
# common/ for mtk-mmap-ion.o, as stock builds it with MTK_ION.
obj-$(CONFIG_MTK_ION) += common/
endif
