# android_kernel_unihertz_mt6771

The Linux 5.10.257 kernel tree for the Unihertz Titan Pocket (MT6771), as built for the Titan Pocket LineageOS port. Boot image p23 was built from this tree.

## What it is

- Base: Linux 5.10.257 from `evoxdl/android_kernel_motorola_sm6450`, revision `9708121d5855e50a16df8787228d1edd6ed6f99c` (an Android 5.10 kernel with Qualcomm and Motorola additions).
- On top: the MT6771 platform ported from the Unihertz 4.14 kernel source release (MediaTek code) for the Pocket (clocks, pinctrl, power, UFS, USB, display, GPU, camera, audio, modem, sensors, charger and more), as 88 patches. The patch series is in the device repo, `pocket/patches/kernel-upgrade-5.10`, and applies to the base revision. This tree is the base with those patches already applied.
- Pin: the files in this tree hash to `9d3e6535f29560373d772aa016e48b7f33826265c9cd09791e7adf59cce53d74` (`70064` files, see `POCKET_PIN.json`).
- Battery level: `drivers/power/supply/oz8806_ocv.h` is the OCV table measured on a Pocket. Without it the driver uses the placeholder `oz8806_ocv_generic.h`, so the level is approximate.
- The p23 image also turned on two test-only options (`CONFIG_MTK_SPM_SUSPEND_HANG_TEST` and `CONFIG_COMMON_CLK_MT6771_FMETER_SCAN`) whose code is not in this tree, so a build of this tree is not byte for byte p23.

## Building

Needs an LLVM toolchain (NDK r27 clang was used). The config is `allnoconfig` merged with `pocket/configs/kernel-firstboot-5.10.fragment` and `pocket/configs/kernel-firstboot-5.10-enforcing.fragment` from the device repo:

```sh
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- allnoconfig
scripts/kconfig/merge_config.sh -m -O out out/.config firstboot.fragment enforcing.fragment
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- Image scripts_dtc
```

The boot image is packed by `pocket/tools/package-pocket-boot.py` in the device repo.

## Licence

Everything in this tree is GPL-2.0, as the Linux kernel (`COPYING`, `LICENSES/`). Code ported from the Unihertz 4.14 source keeps its file headers, and the source files that had none carry an SPDX GPL-2.0 line. Kconfig and Makefile files with no header are covered by `COPYING`.
