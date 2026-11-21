# Copyright (c) 2015-2018 TrustKernel Incorporated
# All Rights Reserved.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# version 2 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.

obj-y += tkcore_drv.o

TKCORE_ROOT := $(srctree)/drivers/misc/mediatek/tkcore

ccflags-y += -I$(TKCORE_ROOT)/include/arm_common
ccflags-y += -I$(TKCORE_ROOT)/include/linux
ccflags-y += -I$(TKCORE_ROOT)/include
ccflags-y += -I$(TKCORE_ROOT)/core

tkcore_drv-objs:= \
		tee_tz_drv.o \
		tee_smc_xfer.o \
		tee_mem.o \
		handle.o
