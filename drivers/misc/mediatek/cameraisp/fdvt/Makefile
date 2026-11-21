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

# MT6771 uses the FDVT 4.0 driver, larb 5, no secure FD (no GenieZone).
MTK_CUSTOM_PATH := $(srctree)/drivers/misc/mediatek

subdir-ccflags-y += -I$(MTK_CUSTOM_PATH)/cameraisp/fdvt/4.0/inc
subdir-ccflags-y += -DMTK_FD_LARB=5
subdir-ccflags-y += -DMTK_SECURE_FD_SUPPORT=0

obj-$(CONFIG_MTK_CAMERA_ISP_FD_SUPPORT) += 4.0/
