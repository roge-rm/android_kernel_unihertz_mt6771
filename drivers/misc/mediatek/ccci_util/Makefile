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

# Only the weak modem stubs, which stock links whatever the modem config
# (mtkfb_probe calls register_ccci_sys_call_back for two Oppo panels).
obj-y += ccci_util_dummy.o

# ccci_util (LK modem info, reserved memory, image header, /sys/kernel/ccci, the ccci
# logs and /dev/ccci_md*_sta), built with eccci as T does. Its strong calls override the weak ones.
ifeq ($(CONFIG_MTK_ECCCI_DRIVER),y)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/ccci_util
obj-y += ccci_util_lib.o
ccci_util_lib-y := ccci_util_lib_fo.o ccci_util_lib_load_img.o ccci_util_lib_sys.o ccci_private_log.o \
	ccci_util_lib_time.o ccci_util_lib_main.o ccci_util_ld_md_errno.o ccci_util_broadcast.o ccci_util_sib.o \
	ccci_util_platform.o
endif
