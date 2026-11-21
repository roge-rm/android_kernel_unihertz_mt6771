// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 * Authors:
 *	Stanley Chu <stanley.chu@mediatek.com>
 *	Peter Wang <peter.wang@mediatek.com>
 */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#include "ufshcd.h"
#include "ufshcd-crypto.h"
#include "ufshcd-pltfrm.h"
#include "ufs_quirks.h"
#include "unipro.h"
#include "ufs-mediatek.h"
#include "ufs-mediatek-fw.h"

#define CREATE_TRACE_POINTS
#include "ufs-mediatek-trace.h"

/*
 * MT6771 system sleep depth, read at each system suspend:
 * 0 leaves host, link and device active,
 * 1 sends SSU (sleep), puts the link in Hibern8 with UniPro power-down and
 *   gates the host clocks,
 * 2 also powers the M-PHY down and gates XO_UFS, in stock's order.
 * VA09 and VCC stay on at every level. Runtime PM stays pinned.
 */
static unsigned int mt6771_sleep = 2;
module_param(mt6771_sleep, uint, 0644);
MODULE_PARM_DESC(mt6771_sleep, "MT6771 UFS system sleep level (0-2)");

static bool ufs_mtk_uses_mt6771_fw(struct ufs_hba *hba)
{
	return of_device_is_compatible(hba->dev->of_node,
				       "mediatek,mt6771-ufshci");
}

static int ufs_mtk_smc(struct ufs_hba *hba, u32 command, u32 value,
		       struct arm_smccc_res *res)
{
	int ret = ufs_mtk_fw_validate(ufs_mtk_uses_mt6771_fw(hba),
				    command, value);

	if (ret) {
		memset(res, 0, sizeof(*res));
		res->a0 = ret;
		return ret;
	}

	arm_smccc_smc(MTK_SIP_UFS_CONTROL,
		      command, value, 0, 0, 0, 0, 0, res);
	return 0;
}

static int ufs_mtk_smc_checked(struct ufs_hba *hba, u32 command, u32 value,
			       struct arm_smccc_res *res)
{
	int ret = ufs_mtk_smc(hba, command, value, res);

	if (ret || !ufs_mtk_uses_mt6771_fw(hba))
		return ret;
	return ufs_mt6771_fw_result(res->a0);
}

#define ufs_mtk_va09_pwr_ctrl(hba, res, on) \
	ufs_mtk_smc_checked(hba, UFS_MTK_SIP_VA09_PWR_CTRL, on, &(res))

#define ufs_mtk_crypto_ctrl(hba, res, enable) \
	ufs_mtk_smc(hba, UFS_MTK_SIP_CRYPTO_CTRL, enable, &(res))

#define ufs_mtk_ref_clk_notify(hba, on, res) \
	ufs_mtk_smc(hba, UFS_MTK_SIP_REF_CLK_NOTIFICATION, on, &(res))

#define ufs_mtk_device_reset_ctrl(hba, high, res) \
	ufs_mtk_smc_checked(hba, UFS_MTK_SIP_DEVICE_RESET, high, &(res))

static struct ufs_dev_fix ufs_mtk_dev_fixups[] = {
	UFS_FIX(UFS_VENDOR_MICRON, UFS_ANY_MODEL,
		UFS_DEVICE_QUIRK_DELAY_AFTER_LPM),
	UFS_FIX(UFS_VENDOR_SKHYNIX, "H9HQ21AFAMZDAR",
		UFS_DEVICE_QUIRK_SUPPORT_EXTENDED_FEATURES),
	END_FIX
};

static const struct of_device_id ufs_mtk_of_match[] = {
	{ .compatible = "mediatek,mt8183-ufshci" },
	/* Explicit RAM storage engineering profile only; probe checks its opt-in. */
	{ .compatible = "mediatek,mt6771-ufshci" },
	{},
};

static bool ufs_mtk_is_boost_crypt_enabled(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return !!(host->caps & UFS_MTK_CAP_BOOST_CRYPT_ENGINE);
}

static bool ufs_mtk_is_va09_supported(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return !!(host->caps & UFS_MTK_CAP_VA09_PWR_CTRL);
}

static bool ufs_mtk_is_broken_vcc(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return !!(host->caps & UFS_MTK_CAP_BROKEN_VCC);
}

static void ufs_mtk_cfg_unipro_cg(struct ufs_hba *hba, bool enable)
{
	u32 tmp;

	if (enable) {
		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), &tmp);
		tmp = tmp |
		      (1 << RX_SYMBOL_CLK_GATE_EN) |
		      (1 << SYS_CLK_GATE_EN) |
		      (1 << TX_CLK_GATE_EN);
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), tmp);

		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), &tmp);
		tmp = tmp & ~(1 << TX_SYMBOL_CLK_REQ_FORCE);
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), tmp);
	} else {
		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), &tmp);
		tmp = tmp & ~((1 << RX_SYMBOL_CLK_GATE_EN) |
			      (1 << SYS_CLK_GATE_EN) |
			      (1 << TX_CLK_GATE_EN));
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), tmp);

		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), &tmp);
		tmp = tmp | (1 << TX_SYMBOL_CLK_REQ_FORCE);
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), tmp);
	}
}

static void ufs_mtk_crypto_enable(struct ufs_hba *hba)
{
	struct arm_smccc_res res;

	ufs_mtk_crypto_ctrl(hba, res, 1);
	if (res.a0) {
		dev_info(hba->dev, "%s: crypto enable failed, err: %lu\n",
			 __func__, res.a0);
		hba->caps &= ~UFSHCD_CAP_CRYPTO;
	}
}

static void ufs_mtk_host_reset(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	reset_control_assert(host->hci_reset);
	reset_control_assert(host->crypto_reset);
	reset_control_assert(host->unipro_reset);

	usleep_range(100, 110);

	reset_control_deassert(host->unipro_reset);
	reset_control_deassert(host->crypto_reset);
	reset_control_deassert(host->hci_reset);
}

static void ufs_mtk_init_reset_control(struct ufs_hba *hba,
				       struct reset_control **rc,
				       char *str)
{
	*rc = devm_reset_control_get(hba->dev, str);
	if (IS_ERR(*rc)) {
		dev_info(hba->dev, "Failed to get reset control %s: %ld\n",
			 str, PTR_ERR(*rc));
		*rc = NULL;
	}
}

static int ufs_mt6771_init_reset(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	host->hci_reset = devm_reset_control_get_exclusive(hba->dev, "hci_rst");
	if (IS_ERR(host->hci_reset))
		return PTR_ERR(host->hci_reset);
	host->unipro_reset = devm_reset_control_get_exclusive(hba->dev, "unipro_rst");
	if (IS_ERR(host->unipro_reset))
		return PTR_ERR(host->unipro_reset);
	host->crypto_reset = devm_reset_control_get_exclusive(hba->dev, "crypto_rst");
	if (IS_ERR(host->crypto_reset))
		return PTR_ERR(host->crypto_reset);
	return 0;
}

static void ufs_mtk_init_reset(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	ufs_mtk_init_reset_control(hba, &host->hci_reset,
				   "hci_rst");
	ufs_mtk_init_reset_control(hba, &host->unipro_reset,
				   "unipro_rst");
	ufs_mtk_init_reset_control(hba, &host->crypto_reset,
				   "crypto_rst");
}

static int ufs_mtk_hce_enable_notify(struct ufs_hba *hba,
				     enum ufs_notify_change_status status)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	unsigned long flags;

	if (status == PRE_CHANGE) {
		if (host->unipro_lpm) {
			hba->vps->hba_enable_delay_us = 0;
		} else {
			hba->vps->hba_enable_delay_us = 600;
			if (!ufs_mtk_uses_mt6771_fw(hba))
				ufs_mtk_host_reset(hba);
		}

		if (!ufs_mtk_uses_mt6771_fw(hba) &&
		    (hba->caps & UFSHCD_CAP_CRYPTO))
			ufs_mtk_crypto_enable(hba);

		if (host->caps & UFS_MTK_CAP_DISABLE_AH8) {
			spin_lock_irqsave(hba->host->host_lock, flags);
			ufshcd_writel(hba, 0,
				      REG_AUTO_HIBERNATE_IDLE_TIMER);
			spin_unlock_irqrestore(hba->host->host_lock,
					       flags);

			hba->capabilities &= ~MASK_AUTO_HIBERN8_SUPPORT;
			hba->ahit = 0;
		}
	} else if (status == POST_CHANGE && ufs_mtk_uses_mt6771_fw(hba)) {
		struct arm_smccc_res res;

		/* Exact shipped HCE callback: legacy restore, then disable the
		 * unsupported UFSHCI reference-clock-request path. This is not
		 * the newer 0x276 crypto command and registers no key callback.
		 * The shipped callback does not interpret the SMC return value.
		 */
		arm_smccc_smc(MTK_SIP_SMC_CMD(0x270), 4, 0, 0, 0, 0, 0, 0, &res);
		ufshcd_writel(hba, 0, REG_UFS_REFCLK_CTRL);
	}

	return 0;
}

static int ufs_mtk_bind_mphy(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;
	int err = 0;

	host->mphy = devm_of_phy_get_by_index(dev, np, 0);

	if (host->mphy == ERR_PTR(-EPROBE_DEFER)) {
		/*
		 * UFS driver might be probed before the phy driver does.
		 * In that case we would like to return EPROBE_DEFER code.
		 */
		err = -EPROBE_DEFER;
		dev_info(dev,
			 "%s: required phy hasn't probed yet. err = %d\n",
			__func__, err);
	} else if (IS_ERR(host->mphy)) {
		err = PTR_ERR(host->mphy);
		if (err != -ENODEV) {
			dev_info(dev, "%s: PHY get failed %d\n", __func__,
				 err);
		}
	}

	if (err)
		host->mphy = NULL;
	/*
	 * Allow unbound mphy because not every platform needs specific
	 * mphy control.
	 */
	if (err == -ENODEV)
		err = 0;

	return err;
}

/* Kept outside hba->clk_list_head: the generic list cannot own the
 * card/Hibern8-dependent PMIC clock-buffer sequence.
 */
static int ufs_mt6771_init_ref_clk(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct device_node *node;

	node = of_get_child_by_name(hba->dev->of_node, "reference-clock");
	if (!node)
		return -ENODEV;
	if (!of_device_is_available(node)) {
		of_node_put(node);
		return -ENODEV;
	}
	host->legacy_ref_clk = devm_get_clk_from_child(hba->dev, node,
						    "ref_clk");
	of_node_put(node);
	if (IS_ERR(host->legacy_ref_clk))
		return PTR_ERR(host->legacy_ref_clk);
	return 0;
}

static int ufs_mt6771_setup_ref_clk(struct ufs_hba *hba, bool on)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	unsigned int wait_us;
	int ret;

	/* Stock leaves the inherited-on reference clock alone before discovery. */
	if (!hba->dev_info.wmanufacturerid)
		return 0;
	if (host->ref_clk_enabled == on)
		return 0;
	if (!host->legacy_ref_clk)
		return -ENODEV;
	/*
	 * Off only in a level 2 system sleep. Level 1 keeps it on and lets the
	 * host clocks gate. Stock off also drops the UFS spm_resource_req vote,
	 * which only deep idle and SODI read, so system suspend doesn't need it.
	 */
	if (!on) {
		if (host->sleep_level < 2)
			return host->sleep_level ? 0 : -EOPNOTSUPP;
		/* Stock wait between Hibern8 and gating, in us. */
		switch (hba->dev_info.wmanufacturerid) {
		case UFS_VENDOR_TOSHIBA:
			udelay(100);
			break;
		case UFS_VENDOR_SAMSUNG:
			udelay(1);
			break;
		default:
			udelay(30);
			break;
		}
		clk_disable_unprepare(host->legacy_ref_clk);
		host->ref_clk_enabled = false;
		return 0;
	}
	ret = clk_prepare_enable(host->legacy_ref_clk);
	if (ret)
		return ret;
	host->ref_clk_enabled = true;
	wait_us = hba->dev_info.wmanufacturerid == UFS_VENDOR_SAMSUNG ||
		  hba->dev_info.wmanufacturerid == UFS_VENDOR_TOSHIBA ? 32 : 30;
	udelay(wait_us);
	return 0;
}

static int ufs_mtk_setup_ref_clk(struct ufs_hba *hba, bool on)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct arm_smccc_res res;
	ktime_t timeout, time_checked;
	u32 value;
	int ret;

	if (ufs_mtk_uses_mt6771_fw(hba))
		return ufs_mt6771_setup_ref_clk(hba, on);

	/* Newer profiles retain their register handshake and notification. */
	ret = ufs_mtk_fw_validate(ufs_mtk_uses_mt6771_fw(hba),
				  UFS_MTK_SIP_REF_CLK_NOTIFICATION, on);
	if (ret)
		return ret;

	if (host->ref_clk_enabled == on)
		return 0;

	if (on) {
		ufs_mtk_ref_clk_notify(hba, on, res);
		ufshcd_delay_us(host->ref_clk_ungating_wait_us, 10);
		ufshcd_writel(hba, REFCLK_REQUEST, REG_UFS_REFCLK_CTRL);
	} else {
		ufshcd_writel(hba, REFCLK_RELEASE, REG_UFS_REFCLK_CTRL);
	}

	/* Wait for ack */
	timeout = ktime_add_us(ktime_get(), REFCLK_REQ_TIMEOUT_US);
	do {
		time_checked = ktime_get();
		value = ufshcd_readl(hba, REG_UFS_REFCLK_CTRL);

		/* Wait until ack bit equals to req bit */
		if (((value & REFCLK_ACK) >> 1) == (value & REFCLK_REQUEST))
			goto out;

		usleep_range(100, 200);
	} while (ktime_before(time_checked, timeout));

	dev_err(hba->dev, "missing ack of refclk req, reg: 0x%x\n", value);

	ufs_mtk_ref_clk_notify(hba, host->ref_clk_enabled, res);

	return -ETIMEDOUT;

out:
	host->ref_clk_enabled = on;
	if (!on) {
		ufshcd_delay_us(host->ref_clk_gating_wait_us, 10);
		ufs_mtk_ref_clk_notify(hba, on, res);
	}

	return 0;
}

static void ufs_mtk_setup_ref_clk_wait_us(struct ufs_hba *hba,
					  u16 gating_us, u16 ungating_us)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	if (hba->dev_info.clk_gating_wait_us) {
		host->ref_clk_gating_wait_us =
			hba->dev_info.clk_gating_wait_us;
	} else {
		host->ref_clk_gating_wait_us = gating_us;
	}

	host->ref_clk_ungating_wait_us = ungating_us;
}

static int ufs_mtk_wait_link_state(struct ufs_hba *hba, u32 state,
				   unsigned long max_wait_ms)
{
	ktime_t timeout, time_checked;
	u32 val;

	timeout = ktime_add_ms(ktime_get(), max_wait_ms);
	do {
		time_checked = ktime_get();
		ufshcd_writel(hba, 0x20, REG_UFS_DEBUG_SEL);
		val = ufshcd_readl(hba, REG_UFS_PROBE);
		val = val >> 28;

		if (val == state)
			return 0;

		/* Sleep for max. 200us */
		usleep_range(100, 200);
	} while (ktime_before(time_checked, timeout));

	if (val == state)
		return 0;

	return -ETIMEDOUT;
}

static int ufs_mtk_mphy_power_off(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct arm_smccc_res res;
	int ret;

	if (host->mphy_enabled) {
		ret = phy_power_off(host->mphy);
		if (ret)
			return ret;
		host->mphy_enabled = false;
	}
	if (host->va09_fw_requested) {
		ret = ufs_mtk_va09_pwr_ctrl(hba, res, 0);
		if (ret)
			return ret;
		host->va09_fw_requested = false;
	}
	if (host->va09_reg_enabled) {
		ret = regulator_disable(host->reg_va09);
		if (ret)
			return ret;
		host->va09_reg_enabled = false;
	}
	return 0;
}

static int ufs_mtk_mphy_power_on(struct ufs_hba *hba, bool on)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct arm_smccc_res res;
	int ret = 0, cleanup;

	if (!host->mphy || (on && host->mphy_powered_on))
		return 0;

	if (!on) {
		ret = ufs_mtk_mphy_power_off(hba);
		goto out;
	}

	if (ufs_mtk_is_va09_supported(hba)) {
		if (!host->va09_reg_enabled) {
			ret = regulator_enable(host->reg_va09);
			if (ret)
				goto out;
			host->va09_reg_enabled = true;
			/* Preserve the BSP's VA09 stabilization interval. */
			usleep_range(200, 210);
		}
		if (!host->va09_fw_requested) {
			ret = ufs_mtk_va09_pwr_ctrl(hba, res, 1);
			if (ret)
				goto unwind;
			host->va09_fw_requested = true;
		}
	}
	if (!host->mphy_enabled) {
		ret = phy_power_on(host->mphy);
		if (ret)
			goto unwind;
		host->mphy_enabled = true;
	}
	goto out;

unwind:
	cleanup = ufs_mtk_mphy_power_off(hba);
	if (cleanup)
		dev_warn(hba->dev, "PHY power cleanup failed: %d\n", cleanup);
out:
	host->mphy_powered_on = host->mphy_enabled &&
		(!ufs_mtk_is_va09_supported(hba) ||
		 (host->va09_reg_enabled && host->va09_fw_requested));
	if (ret)
		dev_info(hba->dev, "failed to %s PHY power: %d\n",
			 on ? "enable" : "disable", ret);
	return ret;
}

static int ufs_mtk_get_host_clk(struct device *dev, const char *name,
				struct clk **clk_out)
{
	struct clk *clk;
	int err = 0;

	clk = devm_clk_get(dev, name);
	if (IS_ERR(clk))
		err = PTR_ERR(clk);
	else
		*clk_out = clk;

	return err;
}

static void ufs_mtk_boost_crypt(struct ufs_hba *hba, bool boost)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_mtk_crypt_cfg *cfg;
	struct regulator *reg;
	int volt, ret;

	if (!ufs_mtk_is_boost_crypt_enabled(hba))
		return;

	cfg = host->crypt;
	volt = cfg->vcore_volt;
	reg = cfg->reg_vcore;

	ret = clk_prepare_enable(cfg->clk_crypt_mux);
	if (ret) {
		dev_info(hba->dev, "clk_prepare_enable(): %d\n",
			 ret);
		return;
	}

	if (boost) {
		ret = regulator_set_voltage(reg, volt, INT_MAX);
		if (ret) {
			dev_info(hba->dev,
				 "failed to set vcore to %d\n", volt);
			goto out;
		}

		ret = clk_set_parent(cfg->clk_crypt_mux,
				     cfg->clk_crypt_perf);
		if (ret) {
			dev_info(hba->dev,
				 "failed to set clk_crypt_perf\n");
			regulator_set_voltage(reg, 0, INT_MAX);
			goto out;
		}
	} else {
		ret = clk_set_parent(cfg->clk_crypt_mux,
				     cfg->clk_crypt_lp);
		if (ret) {
			dev_info(hba->dev,
				 "failed to set clk_crypt_lp\n");
			goto out;
		}

		ret = regulator_set_voltage(reg, 0, INT_MAX);
		if (ret) {
			dev_info(hba->dev,
				 "failed to set vcore to MIN\n");
		}
	}
out:
	clk_disable_unprepare(cfg->clk_crypt_mux);
}

static int ufs_mtk_init_host_clk(struct ufs_hba *hba, const char *name,
				 struct clk **clk)
{
	int ret;

	ret = ufs_mtk_get_host_clk(hba->dev, name, clk);
	if (ret) {
		dev_info(hba->dev, "%s: failed to get %s: %d", __func__,
			 name, ret);
	}

	return ret;
}

static void ufs_mtk_init_boost_crypt(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_mtk_crypt_cfg *cfg;
	struct device *dev = hba->dev;
	struct regulator *reg;
	u32 volt;

	host->crypt = devm_kzalloc(dev, sizeof(*(host->crypt)),
				   GFP_KERNEL);
	if (!host->crypt)
		goto disable_caps;

	reg = devm_regulator_get_optional(dev, "dvfsrc-vcore");
	if (IS_ERR(reg)) {
		dev_info(dev, "failed to get dvfsrc-vcore: %ld",
			 PTR_ERR(reg));
		goto disable_caps;
	}

	if (of_property_read_u32(dev->of_node, "boost-crypt-vcore-min",
				 &volt)) {
		dev_info(dev, "failed to get boost-crypt-vcore-min");
		goto disable_caps;
	}

	cfg = host->crypt;
	if (ufs_mtk_init_host_clk(hba, "crypt_mux",
				  &cfg->clk_crypt_mux))
		goto disable_caps;

	if (ufs_mtk_init_host_clk(hba, "crypt_lp",
				  &cfg->clk_crypt_lp))
		goto disable_caps;

	if (ufs_mtk_init_host_clk(hba, "crypt_perf",
				  &cfg->clk_crypt_perf))
		goto disable_caps;

	cfg->reg_vcore = reg;
	cfg->vcore_volt = volt;
	host->caps |= UFS_MTK_CAP_BOOST_CRYPT_ENGINE;

disable_caps:
	return;
}

static int ufs_mtk_init_va09_pwr_ctrl(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	/* Required when requested: optional-get refuses dummy supply fallback. */
	host->reg_va09 = devm_regulator_get_optional(hba->dev, "va09");
	if (IS_ERR(host->reg_va09))
		return PTR_ERR(host->reg_va09);
	host->caps |= UFS_MTK_CAP_VA09_PWR_CTRL;
	return 0;
}

static int ufs_mtk_init_host_caps(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct device_node *np = hba->dev->of_node;
	int ret;

	if (ufs_mtk_uses_mt6771_fw(hba)) {
		host->ram_probe = of_property_read_bool(np,
						      "unihertz,ram-storage-probe");
		ret = ufs_mt6771_init_ref_clk(hba);
		if (ret)
			return ret;
	}

	if (of_property_read_bool(np, "mediatek,ufs-boost-crypt"))
		ufs_mtk_init_boost_crypt(hba);

	if (of_property_read_bool(np, "mediatek,ufs-support-va09")) {
		ret = ufs_mtk_init_va09_pwr_ctrl(hba);
		if (ret)
			return ret;
	}

	if (of_property_read_bool(np, "mediatek,ufs-disable-ah8"))
		host->caps |= UFS_MTK_CAP_DISABLE_AH8;

	if (of_property_read_bool(np, "mediatek,ufs-broken-vcc"))
		host->caps |= UFS_MTK_CAP_BROKEN_VCC;

	dev_info(hba->dev, "caps: 0x%x", host->caps);
	return 0;
}

/**
 * ufs_mtk_setup_clocks - enables/disable clocks
 * @hba: host controller instance
 * @on: If true, enable clocks else disable them.
 * @status: PRE_CHANGE or POST_CHANGE notify
 *
 * Returns 0 on success, non-zero on failure.
 */
static int ufs_mtk_setup_clocks(struct ufs_hba *hba, bool on,
				enum ufs_notify_change_status status)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	bool clk_pwr_off = false;
	bool ref_was_enabled;
	bool phy_acquired = false;
	bool base_acquired = false;
	int cleanup;
	int ret = 0;

	/*
	 * In case ufs_mtk_init() is not yet done, simply ignore.
	 * This ufs_mtk_setup_clocks() shall be called from
	 * ufs_mtk_init() after init is done.
	 */
	if (!host)
		return 0;
	ref_was_enabled = host->ref_clk_enabled;

	if (!on && status == PRE_CHANGE) {
		if (ufshcd_is_link_off(hba)) {
			clk_pwr_off = true;
		} else if (ufshcd_is_link_hibern8(hba) ||
			 (!ufshcd_can_hibern8_during_gating(hba) &&
			 ufshcd_is_auto_hibern8_enabled(hba))) {
			/*
			 * Gate ref-clk and poweroff mphy if link state is in
			 * OFF or Hibern8 by either Auto-Hibern8 or
			 * ufshcd_link_state_transition().
			 */
			ret = ufs_mtk_wait_link_state(hba,
						      VS_LINK_HIBERN8,
						      15);
			if (!ret)
				clk_pwr_off = true;
		}

		if (clk_pwr_off) {
			ufs_mtk_boost_crypt(hba, on);
			ret = ufs_mtk_setup_ref_clk(hba, on);
			if (ret) {
				ufs_mtk_boost_crypt(hba, true);
				return ret;
			}
			ret = host->clk_phy_voted ?
				phy_power_off(host->mphy) : 0;
			if (!ret)
				host->clk_phy_voted = false;
			if (ret) {
				cleanup = ufs_mtk_setup_ref_clk(hba,
							  ref_was_enabled);
				if (cleanup)
					dev_warn(hba->dev,
						 "refclk rollback failed: %d\n", cleanup);
				ufs_mtk_boost_crypt(hba, true);
			}
		}
	} else if (on && status == POST_CHANGE) {
		/* Generic resume restores clocks before the variant resume hook. */
		if (ufs_mtk_uses_mt6771_fw(hba)) {
			/* Stock turns XO_UFS on before it releases the M-PHY. */
			ret = ufs_mtk_setup_ref_clk(hba, true);
			if (ret)
				return ret;
		}
		if (ufs_mtk_uses_mt6771_fw(hba) && !host->mphy_powered_on) {
			ret = ufs_mtk_mphy_power_on(hba, true);
			if (ret)
				return ret;
			base_acquired = true;
		}
		if (!host->clk_phy_voted) {
			ret = phy_power_on(host->mphy);
			if (ret)
				goto unwind_base;
			host->clk_phy_voted = true;
			phy_acquired = true;
		}
		ret = ufs_mtk_setup_ref_clk(hba, on);
		if (ret) {
			if (phy_acquired) {
				cleanup = phy_power_off(host->mphy);
				if (cleanup)
					dev_warn(hba->dev,
						 "PHY rollback failed: %d\n", cleanup);
				else
					host->clk_phy_voted = false;
			}
			goto unwind_base;
		}
		ufs_mtk_boost_crypt(hba, on);
	}

	return ret;

unwind_base:
	/* A failed callback-vote rollback must retain its underlying power. */
	if (base_acquired && !host->clk_phy_voted) {
		cleanup = ufs_mtk_mphy_power_on(hba, false);
		if (cleanup)
			dev_warn(hba->dev, "resume base-power cleanup failed: %d\n",
				 cleanup);
	}
	return ret;
}

static void ufs_mtk_get_controller_version(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int ret, ver = 0;

	if (host->hw_ver.major)
		return;

	/* Set default (minimum) version anyway */
	host->hw_ver.major = 2;

	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(PA_LOCALVERINFO), &ver);
	if (!ret) {
		if (ver >= UFS_UNIPRO_VER_1_8) {
			host->hw_ver.major = 3;
			/*
			 * Fix HCI version for some platforms with
			 * incorrect version
			 */
			if (hba->ufs_version < ufshci_version(3, 0))
				hba->ufs_version = ufshci_version(3, 0);
		}
	}
}

static u32 ufs_mtk_get_ufs_hci_version(struct ufs_hba *hba)
{
	return hba->ufs_version;
}

/**
 * ufs_mtk_init - find other essential mmio bases
 * @hba: host controller instance
 *
 * Binds PHY with controller and powers up PHY enabling clocks
 * and regulators.
 *
 * Returns -EPROBE_DEFER if binding fails, returns negative error
 * on phy power up failure and returns zero on success.
 */
static int ufs_mtk_init(struct ufs_hba *hba)
{
	const struct of_device_id *id;
	struct device *dev = hba->dev;
	struct ufs_mtk_host *host;
	int err = 0, cleanup;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host) {
		err = -ENOMEM;
		dev_info(dev, "%s: no memory for mtk ufs host\n", __func__);
		goto out;
	}

	host->hba = hba;
	ufshcd_set_variant(hba, host);

	id = of_match_device(ufs_mtk_of_match, dev);
	if (!id) {
		err = -EINVAL;
		goto out_variant_clear;
	}

	/* Initialize host capability */
	err = ufs_mtk_init_host_caps(hba);
	if (err)
		goto out_variant_clear;

	err = ufs_mtk_bind_mphy(hba);
	if (err)
		goto out_variant_clear;

	if (ufs_mtk_uses_mt6771_fw(hba)) {
		err = ufs_mt6771_init_reset(hba);
		if (err)
			goto out_variant_clear;
	} else {
		ufs_mtk_init_reset(hba);
	}

	/*
	 * ufshcd_vops_init() is invoked after
	 * ufshcd_setup_clock(true) in ufshcd_hba_init() thus
	 * phy clock setup is skipped.
	 *
	 * Enable phy clocks specifically here.
	 */
	err = ufs_mtk_mphy_power_on(hba, true);
	if (err)
		goto out_power_off;
	err = ufs_mtk_setup_clocks(hba, true, POST_CHANGE);
	if (err)
		goto out_power_off;

	if (host->ram_probe) {
		/* SPM off-path and production PM are not implemented or qualified. */
		hba->caps &= ~(UFSHCD_CAP_RPM_AUTOSUSPEND |
			       UFSHCD_CAP_CLK_GATING |
			       UFSHCD_CAP_HIBERN8_WITH_CLK_GATING |
			       UFSHCD_CAP_WB_EN);
	} else {
		hba->caps |= UFSHCD_CAP_RPM_AUTOSUSPEND | UFSHCD_CAP_CLK_GATING;
	}

	/* MT6771 has no registered legacy crypto/key lifecycle implementation. */
	if (ufs_mtk_uses_mt6771_fw(hba))
		hba->caps &= ~UFSHCD_CAP_CRYPTO;
	else
		hba->caps |= UFSHCD_CAP_CRYPTO;

	if (!host->ram_probe) {
		hba->caps |= UFSHCD_CAP_WB_EN;
		hba->quirks |= UFSHCI_QUIRK_SKIP_MANUAL_WB_FLUSH_CTRL;
		hba->vps->wb_flush_threshold = UFS_WB_BUF_REMAIN_PERCENT(80);
	}

	if (!host->ram_probe && (host->caps & UFS_MTK_CAP_DISABLE_AH8))
		hba->caps |= UFSHCD_CAP_HIBERN8_WITH_CLK_GATING;

	goto out;

out_power_off:
	/* Release a leftover callback vote before removing base PHY power. */
	if (host->clk_phy_voted) {
		cleanup = phy_power_off(host->mphy);
		if (cleanup) {
			dev_warn(dev, "init PHY vote cleanup failed: %d\n", cleanup);
			goto out_variant_clear;
		}
		host->clk_phy_voted = false;
	}
	cleanup = ufs_mtk_mphy_power_on(hba, false);
	if (cleanup)
		dev_warn(dev, "init power cleanup failed: %d\n", cleanup);
out_variant_clear:
	ufshcd_set_variant(hba, NULL);
out:
	return err;
}

static int ufs_mtk_pre_pwr_change(struct ufs_hba *hba,
				  struct ufs_pa_layer_attr *dev_max_params,
				  struct ufs_pa_layer_attr *dev_req_params)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_dev_params host_cap;
	u32 adapt_val;
	int ret;

	host_cap.tx_lanes = UFS_MTK_LIMIT_NUM_LANES_TX;
	host_cap.rx_lanes = UFS_MTK_LIMIT_NUM_LANES_RX;
	host_cap.hs_rx_gear = UFS_MTK_LIMIT_HSGEAR_RX;
	host_cap.hs_tx_gear = UFS_MTK_LIMIT_HSGEAR_TX;
	host_cap.pwm_rx_gear = UFS_MTK_LIMIT_PWMGEAR_RX;
	host_cap.pwm_tx_gear = UFS_MTK_LIMIT_PWMGEAR_TX;
	host_cap.rx_pwr_pwm = UFS_MTK_LIMIT_RX_PWR_PWM;
	host_cap.tx_pwr_pwm = UFS_MTK_LIMIT_TX_PWR_PWM;
	host_cap.rx_pwr_hs = UFS_MTK_LIMIT_RX_PWR_HS;
	host_cap.tx_pwr_hs = UFS_MTK_LIMIT_TX_PWR_HS;
	host_cap.hs_rate = UFS_MTK_LIMIT_HS_RATE;
	host_cap.desired_working_mode =
				UFS_MTK_LIMIT_DESIRED_MODE;

	if (ufs_mtk_uses_mt6771_fw(hba)) {
		/* Captured board is one lane; stock defaults to HSG3B. */
		if (hba->lanes_per_direction < 1 || hba->lanes_per_direction > 2)
			return -EINVAL;
		host_cap.tx_lanes = hba->lanes_per_direction;
		host_cap.rx_lanes = hba->lanes_per_direction;
		host_cap.hs_rx_gear = UFS_HS_G3;
		host_cap.hs_tx_gear = UFS_HS_G3;
	}

	ret = ufshcd_get_pwr_dev_param(&host_cap,
				       dev_max_params,
				       dev_req_params);
	if (ret) {
		pr_info("%s: failed to determine capabilities\n",
			__func__);
	}

	if (ufs_mtk_uses_mt6771_fw(hba)) {
		unsigned int i;
		static const u32 timing[] = {
			0x1fff, 0xffff, 0x7fff, 0x1fff, 0xffff, 0x7fff
		};

		if (ret)
			return ret;
		/* Exact shipped PA power-mode timeout values. Fail, do not
		 * silently proceed, if this engineering controller rejects them.
		 */
		for (i = 0; i < ARRAY_SIZE(timing); i++) {
			ret = ufshcd_dme_set(hba,
				UIC_ARG_MIB(PA_PWRMODEUSERDATA0 + i), timing[i]);
			if (ret)
				return ret;
		}
		return 0;
	}

	if (host->hw_ver.major >= 3) {
		if (dev_req_params->gear_tx == UFS_HS_G4)
			adapt_val = PA_INITIAL_ADAPT;
		else
			adapt_val = PA_NO_ADAPT;
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(PA_TXHSADAPTTYPE),
			       adapt_val);
	}

	return ret;
}

static int ufs_mtk_pwr_change_notify(struct ufs_hba *hba,
				     enum ufs_notify_change_status stage,
				     struct ufs_pa_layer_attr *dev_max_params,
				     struct ufs_pa_layer_attr *dev_req_params)
{
	int ret = 0;

	switch (stage) {
	case PRE_CHANGE:
		ret = ufs_mtk_pre_pwr_change(hba, dev_max_params,
					     dev_req_params);
		break;
	case POST_CHANGE:
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int ufs_mtk_unipro_set_pm(struct ufs_hba *hba, bool lpm)
{
	int ret;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	ret = ufshcd_dme_set(hba,
			     UIC_ARG_MIB_SEL(VS_UNIPROPOWERDOWNCONTROL, 0),
			     lpm);
	if (!ret || !lpm) {
		/*
		 * Forcibly set as non-LPM mode if UIC commands is failed
		 * to use default hba_enable_delay_us value for re-enabling
		 * the host.
		 */
		host->unipro_lpm = lpm;
	}

	return ret;
}

static int ufs_mt6771_pre_link(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	u32 value;
	int ret;

	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(VS_UNIPROPOWERDOWNCONTROL), &value);
	if (ret)
		return ret;
	if (value == 1) {
		ret = ufshcd_dme_set(hba,
				     UIC_ARG_MIB(VS_UNIPROPOWERDOWNCONTROL), 0);
		if (ret)
			return ret;
	}
	host->unipro_lpm = false;
	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(VS_SAVEPOWERCONTROL), &value);
	if (ret)
		return ret;
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(VS_SAVEPOWERCONTROL), value & ~BIT(6));
	if (ret)
		return ret;
	/* Captured board has stock default scrambling disabled. */
	return ufshcd_dme_set(hba, UIC_ARG_MIB(UFS_MT6771_PA_SCRAMBLING), 0);
}

static int ufs_mt6771_post_link(struct ufs_hba *hba)
{
	int ret;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_LOCAL_TX_LCC_ENABLE), 0);
	/* The shipped callback reports this optional LCC failure and continues. */
	if (ret)
		dev_warn(hba->dev, "MT6771 post-link LCC disable failed: %d\n", ret);
	ufs_mtk_cfg_unipro_cg(hba, true);
	return 0;
}

static int ufs_mtk_pre_link(struct ufs_hba *hba)
{
	int ret;
	u32 tmp;

	if (ufs_mtk_uses_mt6771_fw(hba))
		return ufs_mt6771_pre_link(hba);
	ufs_mtk_get_controller_version(hba);

	ret = ufs_mtk_unipro_set_pm(hba, false);
	if (ret)
		return ret;

	/*
	 * Setting PA_Local_TX_LCC_Enable to 0 before link startup
	 * to make sure that both host and device TX LCC are disabled
	 * once link startup is completed.
	 */
	ret = ufshcd_disable_host_tx_lcc(hba);
	if (ret)
		return ret;

	/* disable deep stall */
	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(VS_SAVEPOWERCONTROL), &tmp);
	if (ret)
		return ret;

	tmp &= ~(1 << 6);

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(VS_SAVEPOWERCONTROL), tmp);

	return ret;
}

static void ufs_mtk_setup_clk_gating(struct ufs_hba *hba)
{
	unsigned long flags;
	u32 ah_ms;

	if (ufshcd_is_clkgating_allowed(hba)) {
		if (ufshcd_is_auto_hibern8_supported(hba) && hba->ahit)
			ah_ms = FIELD_GET(UFSHCI_AHIBERN8_TIMER_MASK,
					  hba->ahit);
		else
			ah_ms = 10;
		spin_lock_irqsave(hba->host->host_lock, flags);
		hba->clk_gating.delay_ms = ah_ms + 5;
		spin_unlock_irqrestore(hba->host->host_lock, flags);
	}
}

static int ufs_mtk_post_link(struct ufs_hba *hba)
{
	if (ufs_mtk_uses_mt6771_fw(hba))
		return ufs_mt6771_post_link(hba);
	/* enable unipro clock gating feature */
	ufs_mtk_cfg_unipro_cg(hba, true);

	/* configure auto-hibern8 timer to 10ms */
	if (ufshcd_is_auto_hibern8_supported(hba)) {
		ufshcd_auto_hibern8_update(hba,
			FIELD_PREP(UFSHCI_AHIBERN8_TIMER_MASK, 10) |
			FIELD_PREP(UFSHCI_AHIBERN8_SCALE_MASK, 3));
	}

	ufs_mtk_setup_clk_gating(hba);

	return 0;
}

static int ufs_mtk_link_startup_notify(struct ufs_hba *hba,
				       enum ufs_notify_change_status stage)
{
	int ret = 0;

	switch (stage) {
	case PRE_CHANGE:
		ret = ufs_mtk_pre_link(hba);
		break;
	case POST_CHANGE:
		ret = ufs_mtk_post_link(hba);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int ufs_mtk_device_reset(struct ufs_hba *hba)
{
	struct arm_smccc_res res;
	int ret;

	/* disable hba before device reset */
	ufshcd_hba_stop(hba);

	ret = ufs_mtk_device_reset_ctrl(hba, 0, res);
	if (ret)
		return ret;

	/*
	 * The reset signal is active low. UFS devices shall detect
	 * more than or equal to 1us of positive or negative RST_n
	 * pulse width.
	 *
	 * To be on safe side, keep the reset low for at least 10us.
	 */
	usleep_range(10, 15);

	ret = ufs_mtk_device_reset_ctrl(hba, 1, res);
	if (ret)
		return ret;

	/* Some devices may need time to respond to rst_n */
	usleep_range(10000, 15000);

	dev_info(hba->dev, "device reset done\n");

	return 0;
}

static int ufs_mtk_link_set_hpm(struct ufs_hba *hba)
{
	int err;

	err = ufshcd_hba_enable(hba);
	if (err)
		return err;

	err = ufs_mtk_unipro_set_pm(hba, false);
	if (err)
		return err;

	err = ufshcd_uic_hibern8_exit(hba);
	if (!err)
		ufshcd_set_link_active(hba);
	else
		return err;

	err = ufshcd_make_hba_operational(hba);
	if (err)
		return err;

	return 0;
}

static int ufs_mtk_link_set_lpm(struct ufs_hba *hba)
{
	int err;

	err = ufs_mtk_unipro_set_pm(hba, true);
	if (err) {
		/* Resume UniPro state for following error recovery */
		ufs_mtk_unipro_set_pm(hba, false);
		return err;
	}

	return 0;
}

static void ufs_mtk_vreg_set_lpm(struct ufs_hba *hba, bool lpm)
{
	if (!hba->vreg_info.vccq2 || !hba->vreg_info.vcc)
		return;

	if (lpm && !hba->vreg_info.vcc->enabled)
		regulator_set_mode(hba->vreg_info.vccq2->reg,
				   REGULATOR_MODE_IDLE);
	else if (!lpm)
		regulator_set_mode(hba->vreg_info.vccq2->reg,
				   REGULATOR_MODE_NORMAL);
}

/* Level 2: drop both PHY votes so phy-mtk-ufs runs its power-down, then gate
 * XO_UFS. VA09 stays on.
 */
static int ufs_mt6771_phy_ref_off(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int err;

	err = ufs_mtk_wait_link_state(hba, VS_LINK_HIBERN8, 15);
	if (err)
		return err;
	if (host->clk_phy_voted) {
		err = phy_power_off(host->mphy);
		if (err)
			return err;
		host->clk_phy_voted = false;
	}
	if (host->mphy_enabled) {
		err = phy_power_off(host->mphy);
		if (err)
			goto phy_on;
		host->mphy_enabled = false;
		host->mphy_powered_on = false;
	}
	err = ufs_mt6771_setup_ref_clk(hba, false);
	if (!err)
		return 0;
phy_on:
	if (!ufs_mtk_mphy_power_on(hba, true) && !phy_power_on(host->mphy))
		host->clk_phy_voted = true;
	return err;
}

/* ufshcd_suspend() has sent SSU (sleep) and put the link in Hibern8. */
static int ufs_mt6771_suspend(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int err;

	if (!ufshcd_is_link_hibern8(hba))
		return -EINVAL;
	/* Vendor UniPro power-down, as stock; it undoes itself on failure. */
	err = ufs_mtk_link_set_lpm(hba);
	if (err)
		return err;
	if (host->sleep_level < 2)
		return 0;
	err = ufs_mt6771_phy_ref_off(hba);
	if (err)
		ufs_mtk_unipro_set_pm(hba, false);
	return err;
}

static int ufs_mtk_suspend(struct ufs_hba *hba, enum ufs_pm_op pm_op)
{
	int err;
	struct arm_smccc_res res;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	if (host->ram_probe) {
		if (pm_op != UFS_SYSTEM_PM || !host->sleep_level)
			return -EOPNOTSUPP;
		return ufs_mt6771_suspend(hba);
	}
	if (ufshcd_is_link_hibern8(hba)) {
		err = ufs_mtk_link_set_lpm(hba);
		if (err)
			goto fail;
	}

	if (!ufshcd_is_link_active(hba)) {
		if (ufs_mtk_uses_mt6771_fw(hba)) {
			struct ufs_mtk_host *host = ufshcd_get_variant(hba);

			/* Release the clock vote before base PHY/VA09 power. */
			err = ufs_mtk_setup_clocks(hba, false, PRE_CHANGE);
			if (err)
				goto fail;
			if (host->clk_phy_voted)
				goto fail;
		}
		/*
		 * Make sure no error will be returned to prevent
		 * ufshcd_suspend() re-enabling regulators while vreg is still
		 * in low-power mode.
		 */
		ufs_mtk_vreg_set_lpm(hba, true);
		err = ufs_mtk_mphy_power_on(hba, false);
		if (err)
			goto fail;
	}

	if (ufshcd_is_link_off(hba)) {
		err = ufs_mtk_device_reset_ctrl(hba, 0, res);
		if (err)
			goto fail;
	}

	return 0;
fail:
	/*
	 * Set link as off state enforcedly to trigger
	 * ufshcd_host_reset_and_restore() in ufshcd_suspend()
	 * for completed host reset.
	 */
	ufshcd_set_link_off(hba);
	return -EAGAIN;
}

static int ufs_mtk_resume(struct ufs_hba *hba, enum ufs_pm_op pm_op)
{
	int err;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	/* setup_clocks() has already restored XO_UFS and the M-PHY. */
	if (host->ram_probe && (pm_op != UFS_SYSTEM_PM || !host->sleep_level))
		return -EOPNOTSUPP;
	err = ufs_mtk_mphy_power_on(hba, true);
	if (err)
		goto fail;

	ufs_mtk_vreg_set_lpm(hba, false);

	if (ufshcd_is_link_hibern8(hba)) {
		err = ufs_mtk_link_set_hpm(hba);
		if (err)
			goto fail;
	}

	return 0;
fail:
	return ufshcd_link_recovery(hba);
}

static void ufs_mtk_dbg_register_dump(struct ufs_hba *hba)
{
	ufshcd_dump_regs(hba, REG_UFS_REFCLK_CTRL, 0x4, "Ref-Clk Ctrl ");

	ufshcd_dump_regs(hba, REG_UFS_EXTREG, 0x4, "Ext Reg ");

	ufshcd_dump_regs(hba, REG_UFS_MPHYCTRL,
			 REG_UFS_REJECT_MON - REG_UFS_MPHYCTRL + 4,
			 "MPHY Ctrl ");

	/* Direct debugging information to REG_MTK_PROBE */
	ufshcd_writel(hba, 0x20, REG_UFS_DEBUG_SEL);
	ufshcd_dump_regs(hba, REG_UFS_PROBE, 0x4, "Debug Probe ");
}

static int ufs_mtk_apply_dev_quirks(struct ufs_hba *hba)
{
	struct ufs_dev_info *dev_info = &hba->dev_info;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	u16 mid = dev_info->wmanufacturerid;

	/*
	 * The card is known now: take a clock reference on the inherited-on
	 * XO_UFS so a level 2 sleep can gate it. If that fails the buffer is
	 * left as it is and sleep just won't gate it.
	 */
	if (ufs_mtk_uses_mt6771_fw(hba) && !host->ref_clk_enabled) {
		int ret = ufs_mt6771_setup_ref_clk(hba, true);

		if (ret)
			dev_warn(hba->dev, "XO_UFS not owned: %d\n", ret);
	}

	if (mid == UFS_VENDOR_SAMSUNG)
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TACTIVATE), 6);

	/*
	 * Decide waiting time before gating reference clock and
	 * after ungating reference clock according to vendors'
	 * requirements.
	 */
	if (mid == UFS_VENDOR_SAMSUNG)
		ufs_mtk_setup_ref_clk_wait_us(hba, 1, 1);
	else if (mid == UFS_VENDOR_SKHYNIX)
		ufs_mtk_setup_ref_clk_wait_us(hba, 30, 30);
	else if (mid == UFS_VENDOR_TOSHIBA)
		ufs_mtk_setup_ref_clk_wait_us(hba, 100, 32);

	return 0;
}

static void ufs_mtk_fixup_dev_quirks(struct ufs_hba *hba)
{
	ufshcd_fixup_dev_quirks(hba, ufs_mtk_dev_fixups);

	if (ufs_mtk_is_broken_vcc(hba) && hba->vreg_info.vcc &&
	    (hba->dev_quirks & UFS_DEVICE_QUIRK_DELAY_AFTER_LPM)) {
		hba->vreg_info.vcc->always_on = true;
		/*
		 * VCC will be kept always-on thus we don't
		 * need any delay during regulator operations
		 */
		hba->dev_quirks &= ~(UFS_DEVICE_QUIRK_DELAY_BEFORE_LPM |
			UFS_DEVICE_QUIRK_DELAY_AFTER_LPM);
	}
}

static void ufs_mtk_event_notify(struct ufs_hba *hba,
				 enum ufs_event_type evt, void *data)
{
	unsigned int val = *(u32 *)data;

	trace_ufs_mtk_event(evt, val);
}

/*
 * struct ufs_hba_mtk_vops - UFS MTK specific variant operations
 *
 * The variant operations configure the necessary controller and PHY
 * handshake during initialization.
 */
static const struct ufs_hba_variant_ops ufs_hba_mtk_vops = {
	.name                = "mediatek.ufshci",
	.init                = ufs_mtk_init,
	.get_ufs_hci_version = ufs_mtk_get_ufs_hci_version,
	.setup_clocks        = ufs_mtk_setup_clocks,
	.hce_enable_notify   = ufs_mtk_hce_enable_notify,
	.link_startup_notify = ufs_mtk_link_startup_notify,
	.pwr_change_notify   = ufs_mtk_pwr_change_notify,
	.apply_dev_quirks    = ufs_mtk_apply_dev_quirks,
	.fixup_dev_quirks    = ufs_mtk_fixup_dev_quirks,
	.suspend             = ufs_mtk_suspend,
	.resume              = ufs_mtk_resume,
	.dbg_register_dump   = ufs_mtk_dbg_register_dump,
	.device_reset        = ufs_mtk_device_reset,
	.event_notify        = ufs_mtk_event_notify,
};

/**
 * ufs_mtk_probe - probe routine of the driver
 * @pdev: pointer to Platform device handle
 *
 * Return zero for success and non-zero for failure
 */
static void ufs_mt6771_release_pm_vote(void *data)
{
	pm_runtime_put_noidle(data);
}

static int ufs_mtk_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev = &pdev->dev;
	struct device_node *reset_node = NULL;
	struct platform_device *reset_pdev;
	struct device_link *link;

	if (of_device_is_compatible(dev->of_node, "mediatek,mt6771-ufshci")) {
		if (!of_property_read_bool(dev->of_node,
					   "unihertz,ram-storage-probe"))
			return -ENODEV;
		/* Pin runtime PM before async scan can drop its own temporary vote.
		 * Capability flags alone only control SCSI-device autosuspend.
		 * This managed vote neither changes user policy nor schedules idle.
		 */
		pm_runtime_get_noresume(dev);
		err = devm_add_action_or_reset(dev, ufs_mt6771_release_pm_vote, dev);
		if (err)
			return err;
		/* The explicit infracfg reset provider is handled by reset core. */
		goto skip_reset;
	}

	reset_node = of_find_compatible_node(NULL, NULL,
					     "ti,syscon-reset");
	if (!reset_node) {
		dev_notice(dev, "find ti,syscon-reset fail\n");
		goto skip_reset;
	}
	reset_pdev = of_find_device_by_node(reset_node);
	if (!reset_pdev) {
		dev_notice(dev, "find reset_pdev fail\n");
		goto skip_reset;
	}
	link = device_link_add(dev, &reset_pdev->dev,
		DL_FLAG_AUTOPROBE_CONSUMER);
	/* The link keeps its own supplier reference, independent of lookup. */
	put_device(&reset_pdev->dev);
	if (!link) {
		dev_notice(dev, "add reset device_link fail\n");
		goto skip_reset;
	}
	/* supplier is not probed */
	if (link->status == DL_STATE_DORMANT) {
		err = -EPROBE_DEFER;
		goto out;
	}

skip_reset:
	/* perform generic probe */
	err = ufshcd_pltfrm_init(pdev, &ufs_hba_mtk_vops);

out:
	if (err)
		dev_info(dev, "probe failed %d\n", err);

	of_node_put(reset_node);
	return err;
}

/**
 * ufs_mtk_remove - set driver_data of the device to NULL
 * @pdev: pointer to platform device handle
 *
 * Always return 0
 */
static int ufs_mtk_remove(struct platform_device *pdev)
{
	struct ufs_hba *hba =  platform_get_drvdata(pdev);

	pm_runtime_get_sync(&(pdev)->dev);
	ufshcd_remove(hba);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
/*
 * MT6771 binding: with mt6771_sleep at 0, system sleep leaves the host, link
 * and device active, since ufshcd_suspend() would send SSU and enter Hibern8
 * before ufs_mtk_suspend() could refuse. Levels 1 and 2 go through
 * ufshcd_suspend() with VCC held on.
 */
static int ufs_mtk_system_suspend(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_vreg *vcc = hba->vreg_info.vcc;
	int ret;

	if (host->ram_probe) {
		host->sleep_level = min(READ_ONCE(mt6771_sleep), 2U);
		if (!host->sleep_level)
			return 0;
		if (vcc) {
			host->vcc_always_on_saved = vcc->always_on;
			vcc->always_on = true;
		}
	}
	ret = ufshcd_pltfrm_suspend(dev);
	if (ret && host->ram_probe) {
		dev_err(dev, "sleep level %u failed: %d\n", host->sleep_level, ret);
		if (vcc)
			vcc->always_on = host->vcc_always_on_saved;
		host->sleep_level = 0;
	}
	return ret;
}

static int ufs_mtk_system_resume(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int ret;

	if (host->ram_probe && !host->sleep_level)
		return 0;
	ret = ufshcd_pltfrm_resume(dev);
	if (host->ram_probe) {
		if (ret)
			dev_err(dev, "resume from sleep level %u failed: %d\n",
				host->sleep_level, ret);
		if (hba->vreg_info.vcc)
			hba->vreg_info.vcc->always_on = host->vcc_always_on_saved;
		host->sleep_level = 0;
	}
	return ret;
}
#else
#define ufs_mtk_system_suspend	NULL
#define ufs_mtk_system_resume	NULL
#endif

static const struct dev_pm_ops ufs_mtk_pm_ops = {
	.suspend         = ufs_mtk_system_suspend,
	.resume          = ufs_mtk_system_resume,
	.runtime_suspend = ufshcd_pltfrm_runtime_suspend,
	.runtime_resume  = ufshcd_pltfrm_runtime_resume,
	.runtime_idle    = ufshcd_pltfrm_runtime_idle,
};

static struct platform_driver ufs_mtk_pltform = {
	.probe      = ufs_mtk_probe,
	.remove     = ufs_mtk_remove,
	.shutdown   = ufshcd_pltfrm_shutdown,
	.driver = {
		.name   = "ufshcd-mtk",
		.pm     = &ufs_mtk_pm_ops,
		.of_match_table = ufs_mtk_of_match,
	},
};

MODULE_AUTHOR("Stanley Chu <stanley.chu@mediatek.com>");
MODULE_AUTHOR("Peter Wang <peter.wang@mediatek.com>");
MODULE_DESCRIPTION("MediaTek UFS Host Driver");
MODULE_LICENSE("GPL v2");

module_platform_driver(ufs_mtk_pltform);
