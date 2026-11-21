#
# Copyright (C) 2015 MediaTek Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#

ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/$(MTK_PLATFORM)/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/spm_v4/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/spm_v4/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include/mach/

# T builds the whole SPM here (sleep, deep idle, SODI, vcorefs, the fs and
# sysfs nodes). With PM_SLEEP and CPU_IDLE off nothing enters those states,
# so only the calls DISP and Mali make are built, in mtk_spm_hooks.o.
obj-$(CONFIG_MACH_MT6771) += mtk_spm_hooks.o
