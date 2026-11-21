// SPDX-License-Identifier: GPL-2.0
/*
 * The legacy MT6358 PMIC calls the 4.14 codec and accdet make, over 5.10's
 * mt6397 MFD: pmic_get_auxadc_value (the ACCDET and HPOFS_CAL channels only),
 * pmic_Read_Efuse_HPOffset, pmic_register_interrupt_callback and
 * pmic_enable_interrupt (the three ACCDET interrupts only) and
 * pwrap_node_to_regmap. Stock had them in pmic/mt6358/v1 (pmic_auxadc.c,
 * pmic_efuse.c, pmic_irq.c), iio/adc/mt635x-auxadc.c and mtk-pmic-wrap.c; the
 * register sequences here are theirs.
 *
 * Everything goes through the one pwrap regmap that the MFD, its IRQ chip, the
 * regulators and the codec use. Each regmap access is atomic under the
 * regmap's lock; on 5.10 that lock is a mutex (stock's pwrap regmap was
 * fast_io), so every call here sleeps.
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/mfd/mt6358/core.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/pmic_wrap.h>

#include <mt-plat/mt6358_pmic_legacy.h>
#include <mt-plat/mtk_auxadc_intf.h>
#include <mt-plat/upmu_common.h>

/* Stock's legacy interrupt numbers (upmu_sw.h) are the MFD's hwirqs. */
static_assert(INT_ACCDET == MT6358_IRQ_ACCDET);
static_assert(INT_ACCDET_EINT0 == MT6358_IRQ_ACCDET_EINT0);
static_assert(INT_ACCDET_EINT1 == MT6358_IRQ_ACCDET_EINT1);

static DEFINE_MUTEX(legacy_lock);	/* the chip lookup and the IRQ table */
static DEFINE_MUTEX(auxadc_lock);	/* one AUXADC request at a time, as stock */
static DEFINE_MUTEX(otp_lock);		/* OTP_CON0 to OTP_CON13 */

/* The mediatek,mt6358 MFD once its probe has finished. Built in, never unbound. */
static struct mt6397_chip *legacy_chip;

static struct mt6397_chip *mt6358_chip_get(void)
{
	struct mt6397_chip *chip;
	struct platform_device *pdev;
	struct device_node *np;

	mutex_lock(&legacy_lock);
	chip = legacy_chip;
	if (chip)
		goto out;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	if (!np) {
		chip = ERR_PTR(-ENODEV);
		goto out;
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		chip = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}
	chip = device_is_bound(&pdev->dev) ? platform_get_drvdata(pdev) : NULL;
	if (!chip || !chip->regmap || !chip->irq_domain) {
		put_device(&pdev->dev);
		chip = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}
	/* The reference on pdev stays with legacy_chip. */
	legacy_chip = chip;
out:
	mutex_unlock(&legacy_lock);
	return chip;
}

int mt6358_pmic_legacy_ready(void)
{
	return PTR_ERR_OR_ZERO(mt6358_chip_get());
}
EXPORT_SYMBOL_GPL(mt6358_pmic_legacy_ready);

/* The pwrap regmap, from the MT6358 node's parent, as the codec finds it. */
static struct regmap *legacy_regmap(void)
{
	static struct regmap *map;
	struct device_node *pmic, *wrap;
	struct regmap *found;

	found = READ_ONCE(map);
	if (found)
		return found;
	pmic = of_find_compatible_node(NULL, NULL, "mediatek,mt6358");
	if (!pmic)
		return NULL;
	wrap = of_get_parent(pmic);
	of_node_put(pmic);
	if (!wrap)
		return NULL;
	found = pwrap_node_to_regmap(wrap);
	of_node_put(wrap);
	if (IS_ERR(found))
		return NULL;
	WRITE_ONCE(map, found);
	return found;
}

/*
 * Stock (mtk-pmic-wrap.c) returned the wrapper's regmap for its node. Here
 * it's the regmap the 5.10 pwrap driver registers on its device, or
 * -EPROBE_DEFER before that.
 */
struct regmap *pwrap_node_to_regmap(struct device_node *np)
{
	struct platform_device *pdev;
	struct regmap *map;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return ERR_PTR(-ENODEV);
	map = dev_get_regmap(&pdev->dev, NULL);
	put_device(&pdev->dev);
	return map ? map : ERR_PTR(-EPROBE_DEFER);
}
EXPORT_SYMBOL_GPL(pwrap_node_to_regmap);

/*
 * AUXADC, as stock's mt635x-auxadc get_auxadc_out: request the channel in
 * AUXADC_RQST0, wait avg-num * 10 us, poll the ready bit (15) in its output
 * register for up to 160 * 100 to 200 us, keep the low res bits. ACCDET is
 * returned in mV (processed: out * ratio * 1800 >> 12), HPOFS_CAL raw, as
 * stock's legacy pmic_get_auxadc_value.
 */
#define AUXADC_RDY		BIT(15)
#define AUXADC_AVG_TIME_US	10
#define AUXADC_MAX_POLLS	160
#define AUXADC_VOLT_FULL	1800	/* mV */
#define AUXADC_DEF_AVG_NUM	8

struct legacy_adc {
	int list;			/* AUXADC_LIST_* */
	const char *name;
	unsigned int dt_channel;	/* "channel" in the mt635x-auxadc child */
	unsigned int rqst_shift;	/* bit in AUXADC_RQST0, the hardware channel */
	unsigned int out_reg;
	unsigned int res;		/* bits */
	bool raw;
	/* from the tree, defaults as stock */
	unsigned int avg_num;
	unsigned int r_ratio[2];
};

static struct legacy_adc legacy_adcs[] = {
	{ AUXADC_LIST_ACCDET, "ACCDET", 9, 5, MT6358_AUXADC_ADC5, 12, false },
	{ AUXADC_LIST_HPOFS_CAL, "HPOFS_CAL", 12, 9, MT6358_AUXADC_ADC9, 15, true },
};
static bool legacy_adcs_parsed;

/*
 * Stock made IIO channels from the available children of mt635x-auxadc and
 * read avg-num and resistance-ratio from them. No 5.10 driver binds that
 * node, so its children are read here whatever their status.
 */
static void legacy_adc_parse_dt(void)
{
	struct device_node *np, *child;
	unsigned int ch, i, ratio[2];

	for (i = 0; i < ARRAY_SIZE(legacy_adcs); i++) {
		legacy_adcs[i].avg_num = AUXADC_DEF_AVG_NUM;
		legacy_adcs[i].r_ratio[0] = 1;
		legacy_adcs[i].r_ratio[1] = 1;
	}
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6358-auxadc");
	for_each_child_of_node(np, child) {
		if (of_property_read_u32(child, "channel", &ch))
			continue;
		for (i = 0; i < ARRAY_SIZE(legacy_adcs); i++) {
			if (legacy_adcs[i].dt_channel != ch)
				continue;
			of_property_read_u32(child, "avg-num", &legacy_adcs[i].avg_num);
			if (!of_property_read_u32_array(child, "resistance-ratio", ratio, 2) &&
			    ratio[1]) {
				legacy_adcs[i].r_ratio[0] = ratio[0];
				legacy_adcs[i].r_ratio[1] = ratio[1];
			}
		}
	}
	of_node_put(np);
	for (i = 0; i < ARRAY_SIZE(legacy_adcs); i++)
		pr_info("mt6358 legacy AUXADC %s: avg-num %u, ratio %u/%u\n",
			legacy_adcs[i].name, legacy_adcs[i].avg_num,
			legacy_adcs[i].r_ratio[0], legacy_adcs[i].r_ratio[1]);
}

int pmic_get_auxadc_value(int list)
{
	struct regmap *map = legacy_regmap();
	struct legacy_adc *adc = NULL;
	unsigned int i, out = 0, polls = 0;
	int ret, val;

	for (i = 0; i < ARRAY_SIZE(legacy_adcs); i++)
		if (legacy_adcs[i].list == list)
			adc = &legacy_adcs[i];
	if (!adc) {
		pr_notice_ratelimited("%s(): channel %d isn't ported\n", __func__, list);
		return -EINVAL;
	}
	if (!map) {
		pr_notice_ratelimited("%s(): %s before the pwrap regmap\n", __func__, adc->name);
		return -EPROBE_DEFER;
	}

	mutex_lock(&auxadc_lock);
	if (!legacy_adcs_parsed) {
		legacy_adc_parse_dt();
		legacy_adcs_parsed = true;
	}
	ret = regmap_write(map, MT6358_AUXADC_RQST0, BIT(adc->rqst_shift));
	if (!ret) {
		fsleep(adc->avg_num * AUXADC_AVG_TIME_US);
		ret = regmap_read(map, adc->out_reg, &out);
	}
	while (!ret && !(out & AUXADC_RDY) && polls++ <= AUXADC_MAX_POLLS) {
		usleep_range(100, 200);
		ret = regmap_read(map, adc->out_reg, &out);
	}
	mutex_unlock(&auxadc_lock);

	if (ret)
		return ret;
	if (!(out & AUXADC_RDY))
		pr_notice_ratelimited("%s(): %s (ch %u) not ready, out 0x%x\n",
				      __func__, adc->name, adc->rqst_shift, out);
	out &= BIT(adc->res) - 1;
	if (adc->raw)
		return out;
	val = out * adc->r_ratio[0] * AUXADC_VOLT_FULL;
	return (val / adc->r_ratio[1]) >> adc->res;
}
EXPORT_SYMBOL(pmic_get_auxadc_value);

void mt6358_otp_lock(void)
{
	mutex_lock(&otp_lock);
}
EXPORT_SYMBOL_GPL(mt6358_otp_lock);

void mt6358_otp_unlock(void)
{
	mutex_unlock(&otp_lock);
}
EXPORT_SYMBOL_GPL(mt6358_otp_unlock);

#define OTP_BUSY_POLLS	1000	/* 10 us apart; stock waited forever */

static unsigned int otp_field(struct regmap *map, unsigned int reg,
			      unsigned int mask, unsigned int shift)
{
	unsigned int v = 0;

	regmap_read(map, reg, &v);
	return (v >> shift) & mask;
}

static void otp_set(struct regmap *map, unsigned int reg, unsigned int mask,
		    unsigned int shift, unsigned int val)
{
	regmap_update_bits(map, reg, mask << shift, (val & mask) << shift);
}

/* Stock's pmic_read_efuse_nolock: one 16-bit OTP word, row i * 2. */
unsigned int pmic_Read_Efuse_HPOffset(int i)
{
	struct regmap *map = legacy_regmap();
	unsigned int data, polls = 0;

	if (!map) {
		pr_notice("%s(%d): before the pwrap regmap, reads 0\n", __func__, i);
		return 0;
	}

	mt6358_otp_lock();
	/* 1. efuse controller clock on */
	regmap_write(map, PMIC_TOP_CKHWEN_CON0_CLR_ADDR, 1 << PMIC_RG_EFUSE_CK_PDN_HWEN_SHIFT);
	regmap_write(map, PMIC_TOP_CKPDN_CON0_CLR_ADDR, 1 << PMIC_RG_EFUSE_CK_PDN_SHIFT);
	/* 2. software read */
	otp_set(map, PMIC_RG_OTP_RD_SW_ADDR, PMIC_RG_OTP_RD_SW_MASK, PMIC_RG_OTP_RD_SW_SHIFT, 1);
	/* 3. row */
	otp_set(map, PMIC_RG_OTP_PA_ADDR, PMIC_RG_OTP_PA_MASK, PMIC_RG_OTP_PA_SHIFT, i * 2);
	/* 4. toggle RG_OTP_RD_TRIG */
	otp_set(map, PMIC_RG_OTP_RD_TRIG_ADDR, PMIC_RG_OTP_RD_TRIG_MASK, PMIC_RG_OTP_RD_TRIG_SHIFT,
		!otp_field(map, PMIC_RG_OTP_RD_TRIG_ADDR, PMIC_RG_OTP_RD_TRIG_MASK,
			   PMIC_RG_OTP_RD_TRIG_SHIFT));
	/* 5. wait for RG_OTP_RD_BUSY to clear */
	udelay(300);
	while (otp_field(map, PMIC_RG_OTP_RD_BUSY_ADDR, PMIC_RG_OTP_RD_BUSY_MASK,
			 PMIC_RG_OTP_RD_BUSY_SHIFT) == 1 && polls++ < OTP_BUSY_POLLS)
		udelay(10);
	/* 6. read RG_OTP_DOUT_SW */
	udelay(100);
	data = otp_field(map, PMIC_RG_OTP_DOUT_SW_ADDR, PMIC_RG_OTP_DOUT_SW_MASK,
			 PMIC_RG_OTP_DOUT_SW_SHIFT);
	/* 7. efuse controller clock off */
	regmap_write(map, PMIC_TOP_CKHWEN_CON0_SET_ADDR, 1 << PMIC_RG_EFUSE_CK_PDN_HWEN_SHIFT);
	regmap_write(map, PMIC_TOP_CKPDN_CON0_SET_ADDR, 1 << PMIC_RG_EFUSE_CK_PDN_SHIFT);
	mt6358_otp_unlock();

	if (polls > OTP_BUSY_POLLS) {
		pr_notice("%s(%d): OTP still busy, reads 0\n", __func__, i);
		return 0;
	}
	return data;
}
EXPORT_SYMBOL(pmic_Read_Efuse_HPOffset);

/*
 * Interrupts. Stock's pmic_enable_interrupt requested the MFD's virq for the
 * legacy number with a threaded handler that calls the registered callback.
 * Only the ACCDET ones are mapped here; names as the stock tree's
 * interrupt-names.
 */
struct legacy_irq {
	unsigned int hwirq;
	const char *name;
	void (*callback)(void);
	unsigned int virq;
	bool requested;
	bool disabled;
	bool wake;
};

static struct legacy_irq legacy_irqs[] = {
	{ MT6358_IRQ_ACCDET, "accdet" },
	{ MT6358_IRQ_ACCDET_EINT0, "accdet_eint0" },
	{ MT6358_IRQ_ACCDET_EINT1, "accdet_eint1" },
};

static struct legacy_irq *legacy_irq_find(unsigned int intNo)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(legacy_irqs); i++)
		if (legacy_irqs[i].hwirq == intNo)
			return &legacy_irqs[i];
	pr_notice("mt6358 legacy: interrupt %u isn't ported\n", intNo);
	return NULL;
}

static irqreturn_t legacy_irq_thread(int irq, void *data)
{
	struct legacy_irq *li = data;
	void (*callback)(void) = READ_ONCE(li->callback);

	if (callback)
		callback();
	return IRQ_HANDLED;
}

void pmic_register_interrupt_callback(unsigned int intNo, void (EINT_FUNC_PTR)(void))
{
	struct legacy_irq *li = legacy_irq_find(intNo);

	if (!li)
		return;
	mutex_lock(&legacy_lock);
	WRITE_ONCE(li->callback, EINT_FUNC_PTR);
	mutex_unlock(&legacy_lock);
	pr_info("%s(): %s callback %ps\n", __func__, li->name, EINT_FUNC_PTR);
}
EXPORT_SYMBOL(pmic_register_interrupt_callback);

void pmic_enable_interrupt(unsigned int intNo, unsigned int en, char *str)
{
	struct legacy_irq *li = legacy_irq_find(intNo);
	struct mt6397_chip *chip;
	int ret;

	if (!li)
		return;
	chip = mt6358_chip_get();
	if (IS_ERR(chip)) {
		pr_notice("%s(): %s before the MT6358 IRQ domain (%ld)\n",
			  __func__, li->name, PTR_ERR(chip));
		return;
	}

	mutex_lock(&legacy_lock);
	if (!li->callback) {
		pr_notice("%s(): no callback for %s\n", __func__, li->name);
		goto out;
	}
	if (!li->virq)
		li->virq = irq_create_mapping(chip->irq_domain, li->hwirq);
	if (!li->virq) {
		pr_notice("%s(): no virq for %s\n", __func__, li->name);
		goto out;
	}
	if (en == 1) {
		if (!li->requested) {
			/* A nested thread of the MFD's IRQ thread, so it may sleep. */
			ret = request_threaded_irq(li->virq, NULL, legacy_irq_thread,
						   IRQF_ONESHOT, li->name, li);
			if (ret)
				pr_notice("%s(): request %s failed, %d\n", __func__, li->name, ret);
			else
				li->requested = true;
		} else if (li->disabled) {
			enable_irq(li->virq);
			li->disabled = false;
		}
		/*
		 * Wake armed, so a plug or button during s2idle runs the handler on
		 * resume. Unarmed, a nested IRQ is only marked pending in suspend,
		 * the MFD clears the status bit and the resend fails, so the event
		 * is lost. The MFD chip has IRQCHIP_SKIP_SET_WAKE and its parent IRQ
		 * is already a wake source, as for mtk-pmic-keys.
		 */
		if (li->requested && !li->wake) {
			ret = enable_irq_wake(li->virq);
			if (ret)
				pr_notice("%s(): wake for %s failed, %d\n", __func__, li->name, ret);
			else
				li->wake = true;
		}
	} else if (en == 0 && li->requested && !li->disabled) {
		if (li->wake) {
			disable_irq_wake(li->virq);
			li->wake = false;
		}
		disable_irq_nosync(li->virq);
		li->disabled = true;
	}
	pr_info("%s(): %s (%s) virq %u en %u\n", __func__, li->name, str ? str : "",
		li->virq, en);
out:
	mutex_unlock(&legacy_lock);
}
EXPORT_SYMBOL(pmic_enable_interrupt);
