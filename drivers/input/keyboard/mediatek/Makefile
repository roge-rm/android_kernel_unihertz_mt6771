# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(srctree)/drivers/input/keyboard/mediatek
ccflags-y += -I$(srctree)/drivers/input/keyboard/mediatek/mt6771
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include

obj-$(CONFIG_KEYBOARD_MTK) += mtk-kpd.o
mtk-kpd-y := kpd.o mt6771/hal_kpd.o
mtk-kpd-$(CONFIG_KPD_PWRKEY_USE_PMIC) += kpd-mt6358.o
