# Glue for the 4.14 MediaTek drivers. Each directory is added by its milestone
# with the symbol T uses. The include paths are the ones T's Makefile sets.

export MTK_PLATFORM_NAME := $(CONFIG_MTK_PLATFORM)
export MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))

subdir-ccflags-y += -Wpointer-arith

subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include/
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/emi/$(MTK_PLATFORM)

obj-$(CONFIG_TRUSTKERNEL_TEE_SUPPORT) += tkcore/

# Milestone 1
obj-$(CONFIG_MTK_BASE_POWER) += base/
# Milestone 2
obj-$(CONFIG_MTK_SMI_EXT) += smi/
obj-$(CONFIG_MTK_M4U) += m4u/
obj-$(CONFIG_MEDIATEK_SOLUTION) += atf/
obj-$(CONFIG_MTK_CACHE_FLUSH_BY_SF) += cmo/
# Milestone 3
obj-$(CONFIG_MTK_SSMR) += memory-ssmr/
obj-$(CONFIG_ZONE_MOVABLE_CMA) += zone_movable_cma/
# Milestone 4
obj-$(CONFIG_MTK_SYNC) += sync/
obj-$(CONFIG_MTK_CMDQ) += cmdq/
obj-$(CONFIG_MTK_CMDQ) += mdp/
# Milestone 5
obj-$(CONFIG_MTK_LEDS) += leds/
obj-$(CONFIG_MTK_LCM) += lcm/
obj-$(CONFIG_MTK_LCM) += ext_disp/
obj-$(CONFIG_MTK_FB) += video/
obj-$(CONFIG_MTK_BOOT) += boot/
obj-$(CONFIG_MTK_FB) += ccci_util/
# Milestone 6
obj-$(CONFIG_MTK_GPU_SUPPORT) += gpu/
obj-$(CONFIG_MTK_FREQ_HOPPING) += freqhopping/
# Charger milestone 2
obj-$(CONFIG_RT_REGMAP) += rt-regmap/
obj-$(CONFIG_RT_FLASHLIGHT) += flashlight/richtek/
obj-y += pmic/
# mtk_charger
obj-$(CONFIG_MTK_CHARGER) += power/
# Vibrator: the MT6358 VIBR LDO behind /sys/class/leds/vibrator
obj-$(CONFIG_MTK_VIBRATOR) += vibrator/
# SCP core: mailbox IPI and ready notify for the sensor hub
obj-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp/
# Connectivity built-in adapter for the out-of-tree connsys modules
obj-$(CONFIG_MTK_COMBO) += connectivity/
# BTIF, the AP side of the connsys STP link
obj-$(CONFIG_MTK_BTIF) += btif/
# ST21NFC, /dev/st21nfc for the stock NFC HAL
obj-$(CONFIG_NFC_CHIP_SUPPORT) += nfc/
# FS16xx smart amp raw I2C node /dev/smartpa_i2c
obj-$(CONFIG_MTK_SMARTPA_FS16XX_I2C) += smartpa/
# ACCDET headset jack detect, /dev/accdet and the ACCDET input device
obj-$(CONFIG_MTK_ACCDET) += accdet/
# eccci platform calls: VMODEM, clock buffer flight mode, SPM MD source, PBM/vcorefs/AEE stubs
obj-$(CONFIG_MTK_CCCI_PLATFORM_DEPS) += ccci_platform/
# eccci core, fsm, CLDMA and CCIF hif and the MT6771 platform code
obj-$(CONFIG_MTK_ECCCI_DRIVER) += eccci/
# ccci_util with eccci too, so its strong calls never depend on MTK_FB
obj-$(CONFIG_MTK_ECCCI_DRIVER) += ccci_util/
# ccmni network interfaces for eccci and the set_rps_map they use
obj-$(CONFIG_MTK_NET_CCMNI) += ccmni/ rps/
# conn_md LTE coex bridge, the modem PA thermistor and the modem coolers
obj-$(CONFIG_MTK_CONN_MD) += conn_md/
obj-$(CONFIG_MTK_THERMAL_BTSMDPA) += thermal/common/thermal_zones/
obj-$(CONFIG_MTK_COOLER_MUTT) += thermal/common/coolers/
# Sensor hub stack: hwmon misc devices and the hub sensors over the SCP
ifeq ($(subst ",,$(CONFIG_MTK_SENSOR_ARCHITECTURE)),1.0)
obj-y += sensors-1.0/
endif
# Sunwave fingerprint: /dev/sunwave_fp and the fingerprint_key input device
obj-$(CONFIG_SUNWAVE_FP) += sunwave/
# Systracker: a bus access that hangs ends in an abort naming the address
obj-$(CONFIG_MTK_SYSTRACKER_V2) += systracker/
# Video codec: /dev/Vcodec for the stock OMX codecs
obj-$(CONFIG_MTK_VIDEOCODEC_DRIVER) += videocodec/
# Camera: the MTK I2C calls imgsensor and cam_cal make, over i2c-mt65xx
obj-$(CONFIG_MTK_I2C_SHIM) += i2c_shim/
# Camera sensor path: imgsensor and seninf, /dev/kd_camera_hw and /dev/seninf
obj-$(CONFIG_MTK_IMGSENSOR) += imgsensor/src/
# Camera calibration EEPROM, /dev/CAM_CAL_DRV
obj-$(CONFIG_MTK_CAM_CAL) += cam_cal/src/
# Lens actuators, /dev/MAINAF and /dev/SUBAF
obj-$(CONFIG_MTK_LENS)   += lens/
# Camera flashlight: /dev/flashlight and the MT6370 channels
obj-$(CONFIG_MTK_FLASHLIGHT) += flashlight/
# Archcounter to monotonic time sync for the camera ISP
obj-$(CONFIG_MTK_ARCHCOUNTER_TIMESYNC) += timer/timesync/
# Camera ISP, DIP and WPE: /dev/camera-isp, /dev/camera-dip, /dev/camera-wpe
obj-$(CONFIG_MTK_CAMERA_ISP) += cameraisp/
# Camera CCU: /dev/ccu
obj-$(CONFIG_MTK_CCU) += ccu/src/
# Camera JPEG encoder: /proc/mtk_jpeg
obj-$(CONFIG_MTK_JPEG) += jpeg/
# SSPM mailbox and IPI core
obj-$(CONFIG_MTK_SSPM_IPI) += sspm_ipi/
# EMI MPU regions over ATF
obj-$(CONFIG_MTK_EMI) += emi/
# MCDI core-off through SSPM mailbox 3
obj-$(CONFIG_MTK_MCDI_SSPM) += mcdi_sspm/
# Core power-down after CPU hotplug
obj-$(CONFIG_MTK_CPUHP_POWER_DOWN) += cpuhp/
# SPM base driver: SPM map, IRQ0, firmware index and status
obj-$(CONFIG_MTK_SPM_BASE) += spm/
# PWM memory mode for the IR transmitter
obj-$(CONFIG_MTK_PWM) += pwm/
# IR transmitter, /dev/irtx
obj-$(CONFIG_MTK_IRTX_PWM_SUPPORT) += irtx/
