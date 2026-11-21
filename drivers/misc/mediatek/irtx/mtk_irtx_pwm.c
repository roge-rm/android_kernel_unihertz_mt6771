/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

/*
 * /dev/irtx for the stock consumerir HAL: the same ioctls, write format and
 * blocking write as MediaTek's driver. The LED is powered from irtx-supply
 * (the MT6370 LDO) from open to close.
 */

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <mt-plat/mtk_pwm.h>
#include <mach/mtk_pwm_hal.h>

#include "mtk_irtx.h"

#define irtx_driver_name "mt_irtx"
#define IRTX_GPIO_MODE_LED_DEFAULT 0
#define IRTX_GPIO_MODE_LED_SET 1
#define IRTX_PWM_CLOCK (26000000)
/* BUF0_SIZE is 16 bits and holds the word count less one */
#define IRTX_MAX_WORDS (0x10000 + 1)
//#define IRTX_DEBUG

static atomic_t ir_usage_cnt;
static atomic_t ir_transmit;
static const char * const irtx_gpio_cfg[] = {
	"irtx_gpio_led_default", "irtx_gpio_led_set"
};

static struct mt_irtx mt_irtx_dev;
static struct pwm_spec_config irtx_pwm_config = {
	.pwm_no = 0,
	.mode = PWM_MODE_MEMORY,
	.clk_div = CLK_DIV1,
	.clk_src = PWM_CLK_NEW_MODE_BLOCK,
	.pmic_pad = 0,
	.PWM_MODE_MEMORY_REGS.IDLE_VALUE = IDLE_FALSE,
	.PWM_MODE_MEMORY_REGS.GUARD_VALUE = GUARD_FALSE,
	.PWM_MODE_MEMORY_REGS.STOP_BITPOS_VALUE = 31,
	/* 1 microseconds, assume clock source is 26M */
	.PWM_MODE_MEMORY_REGS.HDURATION = 229,
	.PWM_MODE_MEMORY_REGS.LDURATION = 229,
	.PWM_MODE_MEMORY_REGS.GDURATION = 0,
	.PWM_MODE_MEMORY_REGS.WAVE_NUM = 1,
};

struct pwm_ir_t {
	unsigned int carrier;
	unsigned int cycle;
	unsigned int duty_cycle;
};

static struct pwm_ir_t pwm_ir = {
	.carrier = 38009,
	.cycle = 3,
	.duty_cycle = 1
};

static int get_ir_device(void)
{
	if (atomic_cmpxchg(&ir_usage_cnt, 0, 1) != 0)
		return -EBUSY;
	return 0;
}

static int put_ir_device(void)
{
	if (atomic_cmpxchg(&ir_usage_cnt, 1, 0) != 1)
		return -EFAULT;
	return 0;
}

static int get_ir_transmit(void)
{
	if (atomic_cmpxchg(&ir_transmit, 0, 1) != 0)
		return -EBUSY;
	return 0;
}

static int put_ir_transmit(void)
{
	if (atomic_cmpxchg(&ir_transmit, 1, 0) != 1)
		return -EFAULT;
	return 0;
}

static void switch_irtx_gpio(int mode)
{
	struct pinctrl *ppinctrl_irtx = mt_irtx_dev.ppinctrl_irtx;
	struct pinctrl_state *pins_irtx = NULL;

	if (mode < 0 || mode >= (ARRAY_SIZE(irtx_gpio_cfg))) {
		pr_notice("%s() [PinC](%d) fail!! - invalid parameter!\n",
			__func__, mode);
		return;
	}

	if (IS_ERR_OR_NULL(ppinctrl_irtx)) {
		pr_notice("%s() [PinC] ppinctrl_irtx:%p Error! err:%ld\n",
		       __func__, ppinctrl_irtx, PTR_ERR(ppinctrl_irtx));
		return;
	}

	pins_irtx = pinctrl_lookup_state(ppinctrl_irtx, irtx_gpio_cfg[mode]);
	if (IS_ERR(pins_irtx)) {
		pr_notice("%s() [PinC] pinctrl_lockup(%p, %s) fail!\n",
			__func__, ppinctrl_irtx, irtx_gpio_cfg[mode]);
		pr_notice("%s() [PinC] ppinctrl:%p, err:%ld\n",
			__func__, pins_irtx, PTR_ERR(pins_irtx));
		return;
	}

	pinctrl_select_state(ppinctrl_irtx, pins_irtx);
	pr_info("%s() [PinC] to mode:%d done.\n", __func__, mode);
}

static int dev_char_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	ret = get_ir_device();
	if (ret) {
		pr_notice("%s() IRTX device busy.\n", __func__);
		goto exit;
	}

	/* LED power on for as long as the device is open */
	ret = regulator_enable(mt_irtx_dev.buck);
	if (ret) {
		pr_notice("%s() irtx regulator_enable fail %d\n", __func__, ret);
		put_ir_device();
		goto exit;
	}

	pr_info("%s() IRTX open by %s\n", __func__, current->comm);
	nonseekable_open(inode, file);
exit:
	return ret;
}

static int dev_char_close(struct inode *inode, struct file *file)
{
	int ret = 0;

	ret = put_ir_device();
	if (ret) {
		pr_notice("%s() IRTX device close without open\n", __func__);
		goto exit;
	}

	regulator_disable(mt_irtx_dev.buck);
	pr_info("%s() IRTX close by %s\n", __func__, current->comm);
exit:
	return ret;
}

static ssize_t dev_char_read(struct file *file, char __user *buf, size_t count,
	loff_t *ppos)
{
	return 0;
}

/*
 * The buffer is a bitstream of u32 words, one bit per PWM step, and a last
 * word with the total send time in microseconds. The PWM sends every word
 * but the last from a DMA buffer; this blocks until the frame is out.
 */
static ssize_t dev_char_write(struct file *file, const char __user *buf,
	size_t count, loff_t *ppos)
{
	dma_addr_t wave_phy;
	void *wave_vir;
	ssize_t ret;
	int i;
	int buf_size = (count + 3) / 4;
	size_t alloc_size = round_up(count, 4);
	unsigned char *data_ptr;
	int h_l_period = 0;
	unsigned long total_time = 0;
	int *buf_ptr;

	ret = get_ir_transmit();
	if (ret < 0) {
		pr_info("%s(), another ir is transmitting\n", __func__);
		goto exit_0;
	}

	pr_info("%s() irtx write len=0x%x, pwm=%d\n", __func__,
		(unsigned int)count, (unsigned int)irtx_pwm_config.pwm_no);
	if (count == 0) {
		ret = 0;
		goto exit_1;
	}

	/* at least one data word and the time word, within BUF0_SIZE */
	if (count < 5 || count > IRTX_MAX_WORDS * 4) {
		pr_notice("%s() IRTX bad length %zu\n", __func__, count);
		ret = -EINVAL;
		goto exit_1;
	}

	// pwm_ir.cycle: whole cycle,  pwm_ir.duty_cycle: high period
	if (!pwm_ir.carrier || !pwm_ir.cycle) {
		pr_notice("%s() IRTX carrier %u cycle %u\n", __func__,
			pwm_ir.carrier, pwm_ir.cycle);
		ret = -EINVAL;
		goto exit_1;
	}
	h_l_period = DIV_ROUND_CLOSEST_ULL((u64)IRTX_PWM_CLOCK * pwm_ir.duty_cycle,
			(u64)pwm_ir.carrier * pwm_ir.cycle);
	/* HDURATION and LDURATION are 16 bits and hold the period less one */
	if (h_l_period < 1 || h_l_period > 0x10000) {
		pr_notice("%s() IRTX period %d out of range\n", __func__,
			h_l_period);
		ret = -EINVAL;
		goto exit_1;
	}

	wave_vir = dma_alloc_coherent(&mt_irtx_dev.plat_dev->dev, alloc_size,
		&wave_phy, GFP_KERNEL);
	if (!wave_vir) {
		pr_notice("%s() IRTX alloc memory fail\n", __func__);
		ret = -ENOMEM;
		goto exit_1;
	}

	if (copy_from_user(wave_vir, buf, count)) {
		pr_notice("%s() IRTX copy from user fail\n", __func__);
		ret = -EFAULT;
		goto exit_2;
	}

	/* invert bit */
	if (mt_irtx_dev.pwm_data_invert) {
		pr_notice("%s() IRTX invert data\n", __func__);
		for (i = 0; i < count; i++) {
			data_ptr = (unsigned char *)wave_vir + i;
			*data_ptr = ~(*data_ptr);
		}
	}

	irtx_pwm_config.PWM_MODE_MEMORY_REGS.HDURATION = h_l_period-1;
	irtx_pwm_config.PWM_MODE_MEMORY_REGS.LDURATION = h_l_period-1;
	irtx_pwm_config.PWM_MODE_MEMORY_REGS.BUF0_BASE_ADDR = wave_phy;
	irtx_pwm_config.PWM_MODE_MEMORY_REGS.BUF0_SIZE = buf_size - 2;

	switch_irtx_gpio(IRTX_GPIO_MODE_LED_SET);

	ret = pwm_set_spec_config(&irtx_pwm_config);
	buf_ptr = (int *) wave_vir;
	pr_info("irtx ret:%zd, period:%d, count:%zu, total:%d, duty:%d/%d\n",
		ret, h_l_period, count, buf_ptr[buf_size - 1],
		pwm_ir.duty_cycle, pwm_ir.cycle);
#ifdef IRTX_DEBUG
	pr_info("%s() irtx pwm buf size = %d\n", __func__, buf_size);
	for (i = 0; i < buf_size; i++) {
		if (i && i % 16 == 0)
			pr_info("\n");
		pr_info("[%d]0x%x, ", i, buf_ptr[i]);
	}
	pr_info("\n");
#endif
	if (buf_ptr[buf_size - 1] > 0) {
		total_time = buf_ptr[buf_size - 1];
	} else {
		/* steps sent times the step length in 26 MHz ticks, in us */
		total_time = div_u64((u64)(count - 4) * 8 * h_l_period *
			USEC_PER_SEC, IRTX_PWM_CLOCK);
	}
	usleep_range(total_time, total_time + 100);

	ret = count;
	pr_info("[IRTX] done, clean up\n");
	mt_pwm_disable(irtx_pwm_config.pwm_no, irtx_pwm_config.pmic_pad);
	switch_irtx_gpio(IRTX_GPIO_MODE_LED_DEFAULT);

exit_2:
	dma_free_coherent(&mt_irtx_dev.plat_dev->dev, alloc_size, wave_vir,
			wave_phy);
exit_1:
	if (put_ir_transmit() < 0)
		pr_info("%s, irtx is not transmitting\n", __func__);
exit_0:
	return ret;
}

static long dev_char_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	int ret = 0;
	unsigned int para = 0, en = 0;

	switch (cmd) {
	case IRTX_IOC_GET_SOLUTTION_TYPE:
		ret = put_user(1, (unsigned int __user *)arg);
		break;
	case IRTX_IOC_SET_IRTX_LED_EN:
		ret = copy_from_user(&para, (void __user *)arg,
			sizeof(unsigned int));
		if (ret) {
			pr_notice("%s() IRTX SET LED EN,copy_from_user fail\n",
				__func__);
			ret = -EFAULT;
		} else {
			/* en: bits 0-3 */
			en = (para & 0xF);
			pr_info("%s IRTX SET LED EN:0x%x,en:%u\n",
				__func__, para, en);

			if (en)
				switch_irtx_gpio(IRTX_GPIO_MODE_LED_SET);
			else
				switch_irtx_gpio(IRTX_GPIO_MODE_LED_DEFAULT);
		}
		break;
	case IRTX_IOC_SET_CARRIER_FREQ:
		ret = copy_from_user(&para, (void __user *)arg,
			sizeof(unsigned int));
		if (ret) {
			pr_notice("IRTX_IOC_SET_CARRIER_FREQ fail\n");
			ret = -EFAULT;
		} else {
			pwm_ir.carrier = para;
			pr_info("irtx carrier freq = %d\n", pwm_ir.carrier);
		}
		break;
	case IRTX_IOC_SET_DUTY_CYCLE:
		ret = copy_from_user(&para, (void __user *)arg,
			sizeof(unsigned int));
		if (ret) {
			pr_notice("IRTX_IOC_SET_DUTY_CYCLE fail\n");
			ret = -EFAULT;
		} else {
			pwm_ir.cycle = para & 0xFFFF;
			pwm_ir.duty_cycle = (para >> 16) & 0xFFFF;
			pr_info("irtx duty cycle = %d/%d\n",
				pwm_ir.duty_cycle, pwm_ir.cycle);
		}
		break;
	default:
		pr_notice("%s() IRTX invalid ioctl cmd 0x%x\n", __func__, cmd);
		ret = -ENOTTY;
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long compat_dev_char_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	if (!file->f_op || !file->f_op->unlocked_ioctl) {
		pr_info("irtx file has no f_op or no unlocked_ioctl.\n");
		return -ENOTTY;
	}

	switch (cmd) {
	case COMPAT_IRTX_IOC_GET_SOLUTTION_TYPE:
	case COMPAT_IRTX_IOC_SET_IRTX_LED_EN:
	case COMPAT_IRTX_IOC_SET_DUTY_CYCLE:
	case COMPAT_IRTX_IOC_SET_CARRIER_FREQ:
		pr_debug("irtx compat_ioctl : command: 0x%x\n", cmd);
		return file->f_op->unlocked_ioctl(
			file, cmd, (unsigned long)compat_ptr(arg));
	default:
		pr_info("irtx compat_ioctl : No such command!! 0x%x\n", cmd);
		return -ENOIOCTLCMD;
	}
}
#endif


static const struct file_operations char_dev_fops = {
	.owner = THIS_MODULE,
	.open = &dev_char_open,
	.release = &dev_char_close,
	.read = &dev_char_read,
	.write = &dev_char_write,
	.unlocked_ioctl = &dev_char_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = &compat_dev_char_ioctl,
#endif
	.llseek = no_llseek,
};

static int irtx_probe(struct platform_device *plat_dev)
{
	static struct cdev c_dev;
	dev_t dev_t_irtx;
	struct device *dev = NULL;
	struct class *dev_class;
	u32 major = 0, minor = 0;
	int ret = 0;

	if (plat_dev->dev.of_node == NULL) {
		pr_notice("%s() irtx OF node is NULL!\n", __func__);
		ret = -ENODEV;
		goto exit;
	}

	of_property_read_u32(plat_dev->dev.of_node, "major", &major);
	of_property_read_u32(plat_dev->dev.of_node, "pwm_ch",
		&mt_irtx_dev.pwm_ch);
	of_property_read_u32(plat_dev->dev.of_node, "pwm_data_invert",
		&mt_irtx_dev.pwm_data_invert);
	pr_info("%s() device tree info: major=%d pwm=%d invert=%d\n", __func__,
		major, mt_irtx_dev.pwm_ch, mt_irtx_dev.pwm_data_invert);

	mt_irtx_dev.ppinctrl_irtx = devm_pinctrl_get(&plat_dev->dev);
	if (IS_ERR(mt_irtx_dev.ppinctrl_irtx)) {
		pr_notice("%s() [PinC]cannot find pinctrl! ptr_err:%ld.\n",
			__func__, PTR_ERR(mt_irtx_dev.ppinctrl_irtx));
		ret = PTR_ERR(mt_irtx_dev.ppinctrl_irtx);
		goto exit;
	}

	/* the IR LED supply, on only while /dev/irtx is open */
	mt_irtx_dev.buck = devm_regulator_get(&plat_dev->dev, "irtx");
	if (IS_ERR(mt_irtx_dev.buck)) {
		ret = PTR_ERR(mt_irtx_dev.buck);
		if (ret != -EPROBE_DEFER)
			pr_notice("%s() irtx regulator not found %d\n",
				__func__, ret);
		goto exit;
	}

	ret = dma_set_mask_and_coherent(&plat_dev->dev, DMA_BIT_MASK(32));
	if (ret) {
		pr_notice("%s() dma mask fail %d\n", __func__, ret);
		goto exit;
	}

	switch_irtx_gpio(IRTX_GPIO_MODE_LED_DEFAULT);

	if (!major) {
		ret = alloc_chrdev_region(&dev_t_irtx, 0, 1, irtx_driver_name);
		if (ret) {
			pr_notice("%s() alloc_chrdev_region fail! ret=%d.\n",
				__func__, ret);
			goto exit;
		} else {
			major = MAJOR(dev_t_irtx);
			minor = MINOR(dev_t_irtx);
		}
	} else {
		dev_t_irtx = MKDEV(major, minor);
		ret = register_chrdev_region(dev_t_irtx, 1, irtx_driver_name);
		if (ret) {
			pr_notice("%s() register_chrdev_region fail! ret=%d.\n",
				__func__, ret);
			goto exit;
		}
	}

	irtx_pwm_config.pwm_no = mt_irtx_dev.pwm_ch;
	mt_irtx_dev.plat_dev = plat_dev;

	cdev_init(&c_dev, &char_dev_fops);
	c_dev.owner = THIS_MODULE;
	ret = cdev_add(&c_dev, dev_t_irtx, 1);
	if (ret) {
		pr_notice("%s() cdev_add fail! ret=%d\n", __func__, ret);
		goto exit_region;
	}

	dev_class = class_create(THIS_MODULE, irtx_driver_name);
	if (IS_ERR(dev_class)) {
		ret = PTR_ERR(dev_class);
		pr_notice("%s() class_create fail! ret=%d\n", __func__, ret);
		goto exit_cdev;
	}
	dev = device_create(dev_class, NULL, dev_t_irtx, NULL, "irtx");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		pr_notice("%s() device_create fail! ret=%d\n", __func__, ret);
		class_destroy(dev_class);
		goto exit_cdev;
	}
	pr_info("%s() Done.\n", __func__);
	return 0;

exit_cdev:
	cdev_del(&c_dev);
exit_region:
	unregister_chrdev_region(dev_t_irtx, 1);
exit:
	pr_info("%s() fail! ret=%d\n", __func__, ret);
	return ret;
}

static const struct of_device_id irtx_of_ids[] = {
	{.compatible = "mediatek,irtx-pwm",},
	{}
};

static struct platform_driver irtx_driver = {
	.driver = {
		.name = "mt_irtx",
		.of_match_table = irtx_of_ids,
		.suppress_bind_attrs = true,
	},
	.probe = irtx_probe,
};

static int __init irtx_init(void)
{
	int ret = 0;

	pr_info("%s()\n", __func__);

	ret = platform_driver_register(&irtx_driver);
	if (ret) {
		pr_notice("%s() platform driver register fail ret:%d.\n",
			__func__, ret);
		goto exit;
	}

 exit:
	return ret;
}

late_initcall(irtx_init);

MODULE_AUTHOR("Chun-Hung Wu <chun-hung.wu@mediatek.com>");
MODULE_DESCRIPTION("Consumer IR transmitter driver v0.1");
MODULE_LICENSE("GPL");
