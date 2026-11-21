#
# Copyright (C) 2015 MediaTek Inc.
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

MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include
subdir-ccflags-$(CONFIG_USB_MU3D_DRV) += -I$(srctree)/drivers/misc/mediatek/mu3d/drv
subdir-ccflags-$(CONFIG_USB_MTK_HDRC) += -I$(srctree)/drivers/misc/mediatek/usb20/
subdir-ccflags-$(CONFIG_MTK_PMIC_CHIP_MT6355) += -I$(srctree)/drivers/misc/mediatek/pmic/include/mt6355
subdir-ccflags-$(CONFIG_MTK_PMIC_CHIP_MT6356) += -I$(srctree)/drivers/misc/mediatek/pmic/include/mt6356
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/pmic/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/typec/tcpc/inc
subdir-ccflags-y += -I$(srctree)/drivers/power/supply/mediatek/misc/
subdir-ccflags-y += -I$(srctree)/drivers/power/supply/mediatek/battery/

# Stock also builds mtk_pdc_intf, mtk_pe40_intf, mtk_pe40, mtk_pdc, mtk_pd_adapter,
# mtk_intf (PE4.0 and PD, which need tcpc), mtk_linear_charging and
# mtk_switch_charging2 (other algorithms the Pocket DT doesn't pick). They come
# with the tcpc step; until then mtk_charger_glue.c gives the no-adapter answers.
obj-y += charger_class.o mtk_charger.o mtk_switch_charging.o\
	 mtk_chg_type_det.o adapter_class.o mtk_charger_glue.o

obj-$(CONFIG_MTK_DUAL_CHARGER_SUPPORT) += mtk_dual_switch_charging.o
obj-$(CONFIG_MTK_PUMP_EXPRESS_PLUS_SUPPORT) += mtk_pe_intf.o
obj-$(CONFIG_MTK_PUMP_EXPRESS_PLUS_20_SUPPORT) += mtk_pe20_intf.o
obj-$(CONFIG_MTK_PUMP_EXPRESS_50_SUPPORT)	+= mtk_pe50_intf.o mtk_pe50.o
obj-$(CONFIG_RT_PROP_CHGALGO) += prop_chgalgo_class.o pca_pps_ta.o \
				 pca_mtk_chg.o pca_dv2_algo.o
obj-$(CONFIG_CHARGER_RT9465) += rt9465.o
obj-$(CONFIG_CHARGER_RT9466) += rt9466.o
obj-$(CONFIG_CHARGER_RT9458) += rt9458.o
obj-$(CONFIG_CHARGER_RT9467) += rt9467.o
obj-$(CONFIG_CHARGER_RT9471) += rt9471.o
obj-$(CONFIG_CHARGER_RT9759) += rt9759.o
obj-$(CONFIG_CHARGER_HL7005) += hl7005.o
obj-$(CONFIG_CHARGER_BQ25601) += bq25601.o
obj-$(CONFIG_LOAD_SWITCH_RT9750) += rt9750.o
obj-$(CONFIG_CHARGER_MT6362) += mt6362-chg.o
