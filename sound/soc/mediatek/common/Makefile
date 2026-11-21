# SPDX-License-Identifier: GPL-2.0
# platform driver
snd-soc-mtk-common-objs := mtk-afe-platform-driver.o mtk-afe-fe-dai.o
obj-$(CONFIG_SND_SOC_MEDIATEK) += snd-soc-mtk-common.o

obj-$(CONFIG_SND_SOC_MTK_BTCVSD) += mtk-btcvsd.o

# MediaTek 4.14 audio: ION buffers for AAudio mmap, as stock.
ifeq ($(CONFIG_MTK_SND_SOC_NEW_ARCH),y)
obj-$(CONFIG_MTK_ION) += mtk-mmap-ion.o
CFLAGS_mtk-mmap-ion.o += -I$(srctree)/drivers/staging/android/ion -I$(srctree)/drivers/staging/android/ion/mtk
endif
