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

# BTIF driver for AD DIE, built from the kernel tree only
MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat

obj-y += common/
ifeq ($(CONFIG_MTK_ENG_BUILD), y)
subdir-ccflags-y += -D BTIF_DBG_SUPPORT=1
else
subdir-ccflags-y += -D BTIF_DBG_SUPPORT=0
endif
