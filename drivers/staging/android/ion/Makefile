# SPDX-License-Identifier: GPL-2.0
# The MTK include paths below and in mtk/ expand the platform name.
export MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))

obj-$(CONFIG_ION) +=	ion.o ion-ioctl.o ion_heap.o \
			ion_page_pool.o ion_system_heap.o \
			ion_carveout_heap.o ion_chunk_heap.o ion_cma_heap.o
ifdef CONFIG_COMPAT
obj-$(CONFIG_ION) += compat_ion.o
endif

obj-$(CONFIG_MTK_ION) += mtk/
ccflags-$(CONFIG_MTK_ION)  += -I$(srctree)/drivers/staging/android/ion/mtk
subdir-ccflags-$(CONFIG_MTK_ION)  += -I$(srctree)/drivers/staging/android/ion/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/mmp/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include

ccflags-y += -DION_HISTORY_RECORD
ifeq ($(CONFIG_MTK_ENG_BUILD), y)
ccflags-y += -DION_RUNTIME_DEBUGGER=1
else
ccflags-y += -DION_RUNTIME_DEBUGGER=0
endif
