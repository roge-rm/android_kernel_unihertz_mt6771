# The stock battery calls (mtk_battery_intf), read from the OZ8806 gauge
subdir-ccflags-y += -I$(srctree)/drivers/power/supply/mediatek/charger
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include

obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
