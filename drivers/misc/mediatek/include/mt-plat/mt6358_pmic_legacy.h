/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The legacy MT6358 PMIC calls the 4.14 codec and accdet use, on 5.10's mt6397
 * MFD and the pwrap regmap (drivers/misc/mediatek/pmic/mt6358/pmic_legacy.c).
 * pmic_get_auxadc_value, pmic_Read_Efuse_HPOffset and the interrupt calls keep
 * their stock declarations in mtk_auxadc_intf.h and upmu_common.h.
 */
#ifndef __MT6358_PMIC_LEGACY_H__
#define __MT6358_PMIC_LEGACY_H__

/*
 * 0 once the pwrap regmap and the MT6358 IRQ domain are up, -EPROBE_DEFER
 * before, -ENODEV with no MT6358 in the tree. Process context only.
 */
int mt6358_pmic_legacy_ready(void);

/*
 * Held across a PMIC OTP (efuse) read. pmic_Read_Efuse_HPOffset takes it, and
 * the codec's own OTP read must take it too, as both drive the same OTP_CON
 * registers. Sleeps.
 */
void mt6358_otp_lock(void);
void mt6358_otp_unlock(void);

#endif /* __MT6358_PMIC_LEGACY_H__ */
