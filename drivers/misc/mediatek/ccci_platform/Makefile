# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/clkbuf_v1
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/clkbuf_v1/$(MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/spm_v4
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/vcorefs_v3
ccflags-y += -I$(srctree)/drivers/misc/mediatek/pmic
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat

obj-$(CONFIG_MTK_CCCI_PLATFORM_DEPS) += ccci_pmic_regmap.o vmd1_pmic.o clkbuf_md.o spm_mdsrc.o ccci_platform_stubs.o
