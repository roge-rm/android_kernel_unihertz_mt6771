#
# Copyright (C) 2016 MediaTek Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See http://www.gnu.org/licenses/gpl-2.0.html for more details.
#

# mt6893&mt6885 co-platform config
ifeq (y, $(filter y, $(CONFIG_MACH_MT6893)))
MTK_PLATFORM := mt6885
else
ifeq (y, $(filter y, $(CONFIG_MACH_MT6833)))
MTK_PLATFORM := mt6833
else
MTK_PLATFORM := $(CONFIG_MTK_PLATFORM)
endif
endif

ifeq ($(CONFIG_MTK_ECCCI_DRIVER), y)

obj-$(CONFIG_MTK_ECCCI_DRIVER) := ccci.o
ccflags-y += -I$(srctree)/drivers/misc/mediatek/eccci/inc/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/eccci/$(MTK_PLATFORM)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/eccci/
# U keeps the MT6771 clkbuf, SPM and vcorefs headers in their version folders (as ccci_platform/)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/clkbuf_v1
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/spm_v4
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/vcorefs_v3
ccflags-y += -I$(srctree)/drivers/misc/mediatek/ccmni/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/conn_md/include/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/$(MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/scp/$(MTK_PLATFORM)

# common services
ccci-y := ccci_core.o ccci_bm.o ccci_modem.o modem_sys1.o
# weak stand-ins for ccci_util (M1) and the port layer (M3b), and PBM's empty MD hook
ccci-y += ccci_util_weak.o ccci_port_weak.o ccci_pbm_md.o

# ccci_auxadc.c isn't built: stock has MEDIATEK_MT6577_AUXADC off and reads the
# MD ADC channel through MTK_AUXADC in port_rpc.c; U has the upstream driver on.
# Which way port_rpc reads it is decided with the port layer (M3b).

# sub modules
obj-y += fsm/
obj-y += hif/
obj-y += port/
obj-y += udc/
obj-$(CONFIG_MTK_MIX_DEVICES) +=  mixdev/

ifeq ($(CONFIG_MTK_ECCCI_C2K), y)
ccci-y += modem_sys3.o
endif

# platform code
obj-y += $(subst ",,$(MTK_PLATFORM))/

else
obj-  := dummy.o # avoid build error
endif
