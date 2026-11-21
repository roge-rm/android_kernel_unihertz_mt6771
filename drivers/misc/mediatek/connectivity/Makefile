# SPDX-License-Identifier: GPL-2.0
# Connectivity built-in adapter (connadp) for the out-of-tree connsys modules.

ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/clkbuf_v1
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/clkbuf_v1/$(MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat

obj-$(CONFIG_MTK_COMBO) += connadp.o
connadp-objs := common/connectivity_build_in_adapter.o
connadp-objs += common/wmt_build_in_adapter.o
connadp-objs += common/connectivity_platform_stubs.o
