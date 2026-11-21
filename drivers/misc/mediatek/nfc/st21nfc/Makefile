# SPDX-License-Identifier: GPL-2.0
#
# Makefile for ST21NFC
#

ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include/clkbuf_v1
obj-$(CONFIG_NFC_CHIP_SUPPORT) += st21nfc.o
ccflags-$(CONFIG_NFC_DEBUG) += -DDEBUG
