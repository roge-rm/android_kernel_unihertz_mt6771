# SPDX-License-Identifier: GPL-2.0
# drivers/misc/mediatek turns on -Wpointer-arith for T's code; the kernel headers
# this file includes trip it, so it is off here.
ccflags-y += -Wno-pointer-arith
obj-$(CONFIG_MTK_SPM_BASE) += mtk_spm_base.o
obj-$(CONFIG_MTK_SPM_VCOREFS) += mtk_spm_vcorefs.o
obj-$(CONFIG_MTK_SPM_SUSPEND) += mtk_spm_suspend.o mtk_sys_cirq.o
