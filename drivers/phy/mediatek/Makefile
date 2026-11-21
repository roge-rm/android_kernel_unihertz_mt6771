# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_PHY_MTK_MT6771_USB) += mt6771-usb-phy.o
mt6771-usb-phy-y := phy-mt6771-core.o phy-mtk-mt6771.o
#
# Makefile for the phy drivers.
#

obj-$(CONFIG_PHY_MTK_TPHY)		+= phy-mtk-tphy.o
obj-$(CONFIG_PHY_MTK_UFS)		+= phy-mtk-ufs.o
obj-$(CONFIG_PHY_MTK_XSPHY)		+= phy-mtk-xsphy.o

phy-mtk-hdmi-drv-y			:= phy-mtk-hdmi.o
phy-mtk-hdmi-drv-y			+= phy-mtk-hdmi-mt2701.o
phy-mtk-hdmi-drv-y			+= phy-mtk-hdmi-mt8173.o
obj-$(CONFIG_PHY_MTK_HDMI)		+= phy-mtk-hdmi-drv.o
