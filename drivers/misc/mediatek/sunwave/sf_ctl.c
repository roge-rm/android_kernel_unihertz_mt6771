// SPDX-License-Identifier: GPL-2.0
/*
 * Sunwave fingerprint sensor control for the stock Sunwave HAL, as stock's
 * sunwave-fp v2.2.38: misc device sunwave_fp and the fingerprint_key input
 * device, on the mediatek,fingerprint node. Rewritten from the Sunwave
 * sf_ctl.c reference (v2.2.36) with the stock behaviour.
 *
 * The TrustKernel TA reads the sensor over SPI0, which is secure only, so
 * Linux has no SPI device here. This driver powers and resets the sensor,
 * keeps the SPI0 clock on while the HAL asks, reads the chip ID once through
 * tee_spi_transfer, sends SPI_STATE=finger on each finger IRQ and
 * SCREEN_STATUS on blank and after the HAL opens it, and reports the keys
 * the HAL decides on.
 */

#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/tee_fp.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define SF_DRV_VERSION		"v2.2.38-2018-12-25"
#define SF_DRV_VERSION_LEN	32
#define SF_LIB_VERSION_LEN	0x160	/* sf_version_info_t, packed */
#define SF_WAKEUP_MS		1250
#define SF_SCREEN_SYNC_MS	1000	/* after open, for the HAL's uevent thread to start */
#define SF_SCREEN_RESYNC_MS	3000	/* between the syncs, in case the thread starts later */
#define SF_SCREEN_SYNCS		2

/*
 * The stock ioctl numbers. The 's' ones are Sunwave's, the 'k' ones the spidev
 * names it also takes. 0x40087309 and 0x80087320 encode a 64-bit pointer size,
 * as the stock 64-bit HAL uses them, so they are written out.
 */
#define SF_IOC_INIT_DRIVER	0x7300
#define SF_IOC_DEINIT_DRIVER	0x7301
#define SF_IOC_RESET_DEVICE	0x7302
#define SF_IOC_ENABLE_IRQ	0x7303
#define SF_IOC_DISABLE_IRQ	0x7304
#define SF_IOC_ENABLE_SPI_CLK	0x7305
#define SF_IOC_DISABLE_SPI_CLK	0x7306
#define SF_IOC_ENABLE_POWER	0x7307
#define SF_IOC_DISABLE_POWER	0x7308
#define SF_IOC_REPORT_KEY_EVENT	0x40087309	/* struct sf_key_event */
#define SF_IOC_SYNC_CONFIG	0xc008730a
#define SF_IOC_SPI_SPEED	0x730b
#define SF_IOC_ATTRIBUTE	0x730d
#define SF_IOC_GET_VERSION	0x80087320	/* 32 bytes out */
#define SF_IOC_REQUEST_IRQ	0x7321
#define SF_IOC_SET_LIB_VERSION	0x7330		/* 0x160 bytes in */
#define SF_IOC_GET_LIB_VERSION	0x7331		/* 0x160 bytes out */
#define SPI_IOC_RD_SPEED	0x80046b04
#define SPI_IOC_WR_SPEED	0x40046b04
#define SPI_IOC_RESET		0x6b05
#define SF_IOC_ATTRIBUTE_K	0x40046b11

struct sf_key_event {
	s32 key;	/* 1 to 10, see sf_keys */
	s32 value;	/* 0 up, else down */
};

/* HAL key number to key code, index 1 to 10 as stock. */
static const unsigned short sf_keys[] = {
	KEY_UNKNOWN, KEY_HOME, KEY_MENU, KEY_BACK, KEY_HOMEPAGE, KEY_ENTER,
	KEY_UP, KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_WAKEUP,
};

/* The keys stock's fingerprint_key declares. */
static const unsigned short sf_caps[] = {
	KEY_HOME, KEY_HOMEPAGE, KEY_BACK, KEY_MENU, KEY_UP, KEY_LEFT,
	KEY_RIGHT, KEY_DOWN, KEY_ENTER, KEY_F11, KEY_WAKEUP,
};

/*
 * MediaTek mt_chip_conf for the TA's SPI0 setup, 20 u32 as the 4.14 spi
 * driver. Stock values: about 0.9 MHz from the 109.2 MHz syspll_d5_d2 source,
 * mode 0, MSB first, FIFO mode.
 */
struct sf_chip_conf {
	u32 setuptime, holdtime, high_time, low_time, cs_idletime, ulthgh_thrsh;
	u32 sample_sel, cs_pol, cpol, cpha, tx_mlsb, rx_mlsb, tx_endian, rx_endian;
	u32 com_mod, pause, finish_intr, deassert, ulthigh, tckdly;
};

static const struct sf_chip_conf sf_stock_conf = {
	.setuptime = 15, .holdtime = 15, .high_time = 60, .low_time = 60,
	.cs_idletime = 20, .tx_mlsb = 1, .rx_mlsb = 1, .finish_intr = 1,
};

struct sf_dev {
	struct device *dev;
	struct miscdevice misc;
	struct input_dev *input;
	struct wakeup_source *ws;
	struct work_struct event_work;
	struct delayed_work screen_work;
	struct notifier_block fb_notifier;
	struct mutex screen_lock;	/* screen_on and the order of SCREEN_STATUS uevents */
	bool screen_on;
	int screen_syncs;		/* syncs still to send, under screen_lock */
	struct pinctrl *pinctrl;
	struct pinctrl_state *power_high, *rst_low, *rst_high;
	struct clk *spi_clk;
	struct mutex lock;		/* clk_on, lib_version and the TEE buffers */
	bool clk_on;
	int irq;
	u16 chip_id;
	struct sf_chip_conf conf;	/* tee_spi_transfer buffers, off the stack */
	u8 tx[16], rx[16];
	u8 lib_version[SF_LIB_VERSION_LEN];
};

static void sf_reset(struct sf_dev *sf)
{
	pinctrl_select_state(sf->pinctrl, sf->rst_high);
	usleep_range(1000, 1500);
	pinctrl_select_state(sf->pinctrl, sf->rst_low);
	msleep(10);
	pinctrl_select_state(sf->pinctrl, sf->rst_high);
}

/* Under sf->lock. */
static void sf_spi_clk(struct sf_dev *sf, bool on)
{
	if (on == sf->clk_on)
		return;
	if (on) {
		if (clk_prepare_enable(sf->spi_clk)) {
			dev_err(sf->dev, "spi clock on failed\n");
			return;
		}
	} else {
		clk_disable_unprepare(sf->spi_clk);
	}
	sf->clk_on = on;
}

/*
 * One TA transfer of len bytes into rx; 0 on success. The command is cmd_len
 * bytes and the rest of the write buffer is zero. Under sf->lock.
 */
static int sf_xfer_cmd(struct sf_dev *sf, const u8 *tx, int cmd_len, int len)
{
	memset(sf->rx, 0, sizeof(sf->rx));
	memset(sf->tx, 0, sizeof(sf->tx));
	memcpy(sf->tx, tx, cmd_len);
	sf->conf = sf_stock_conf;
	return tee_spi_transfer(&sf->conf, sizeof(sf->conf), sf->tx, sf->rx, len);
}

#define sf_xfer(sf, cmd, len) sf_xfer_cmd(sf, cmd, sizeof(cmd), len)

/*
 * The stock probe's ID read, in the stock order: the 8205/8231/8241/8271,
 * 8202 and 8221 checks, then the 8201 sequence after a reset, retried with a
 * longer wait. The Titan Pocket answers the 8201 sequence (stock log 0x8201).
 * Under sf->lock with the SPI clock on.
 */
static int sf_read_chip_id(struct sf_dev *sf)
{
	static const u8 id_a0[] = { 0xa0, 0x5f };
	static const u8 id_60[] = { 0x60, 0x9f, 0x28, 0x02, 0x00 };
	static const u8 id_8221[] = { 0x60, 0x28, 0x02, 0x00 };
	static const u8 w84[] = { 0x32, 0xcd, 0x00, 0x00, 0x84 };
	static const u8 w81[] = { 0x32, 0xcd, 0x00, 0x00, 0x81 };
	static const u8 r21[] = { 0x81, 0x7e, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00 };
	static const u8 w83[] = { 0x32, 0xcd, 0x00, 0x00, 0x83 };
	int tries, retry;

	for (tries = 0; tries < 3; tries++) {
		if (sf_xfer(sf, id_a0, 6))
			continue;
		if (!memcmp(&sf->rx[2], "SunW", 4)) {
			sf->chip_id = 0;
			return 0;
		}
		if (sf_xfer(sf, id_60, 7))
			continue;
		if (sf->rx[5] == 0x82) {
			sf->chip_id = 0x8200 | sf->rx[6];
			return 0;
		}
		if (sf_xfer(sf, id_8221, 6))
			continue;
		if (sf->rx[4] == 0x82) {
			sf->chip_id = 0x8200 | sf->rx[5];
			return 0;
		}
		for (retry = 0; retry <= 10; retry++) {
			sf_reset(sf);
			msleep(1 + retry * 2);
			if (sf_xfer(sf, w84, 5))
				continue;
			msleep(1);
			if (sf_xfer(sf, w81, 5))
				continue;
			msleep(5);
			if (sf_xfer(sf, r21, 10))
				continue;
			if (sf->rx[8] == 0x82) {
				sf->chip_id = sf->rx[8] << 8 | sf->rx[9];
				sf_xfer(sf, w83, 5);
				return 0;
			}
		}
	}
	return -ENODEV;
}

static int sf_report_key(struct sf_dev *sf, const struct sf_key_event *ev)
{
	unsigned int code = KEY_UNKNOWN;

	if (ev->key > 0 && ev->key < ARRAY_SIZE(sf_keys))
		code = sf_keys[ev->key];
	input_report_key(sf->input, code, ev->value);
	input_sync(sf->input);
	return 0;
}

static long sf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sf_dev *sf = container_of(file->private_data, struct sf_dev, misc);
	void __user *uarg = (void __user *)arg;
	struct sf_key_event ev;
	char version[SF_DRV_VERSION_LEN] = SF_DRV_VERSION;
	long ret = 0;

	switch (cmd) {
	case SF_IOC_RESET_DEVICE:
	case SPI_IOC_RESET:
		sf_reset(sf);
		break;
	case SF_IOC_ENABLE_SPI_CLK:
	case SF_IOC_DISABLE_SPI_CLK:
		mutex_lock(&sf->lock);
		sf_spi_clk(sf, cmd == SF_IOC_ENABLE_SPI_CLK);
		mutex_unlock(&sf->lock);
		break;
	case SF_IOC_REPORT_KEY_EVENT:
		if (copy_from_user(&ev, uarg, sizeof(ev)))
			return -EFAULT;
		ret = sf_report_key(sf, &ev);
		break;
	case SF_IOC_ATTRIBUTE:
	case SF_IOC_ATTRIBUTE_K:
		ret = put_user(0U, (u32 __user *)uarg);
		break;
	case SF_IOC_GET_VERSION:
		if (copy_to_user(uarg, version, sizeof(version)))
			return -EFAULT;
		break;
	case SF_IOC_SET_LIB_VERSION:
		mutex_lock(&sf->lock);
		if (copy_from_user(sf->lib_version, uarg, SF_LIB_VERSION_LEN))
			ret = -EFAULT;
		mutex_unlock(&sf->lock);
		break;
	case SF_IOC_GET_LIB_VERSION:
		mutex_lock(&sf->lock);
		if (copy_to_user(uarg, sf->lib_version, SF_LIB_VERSION_LEN))
			ret = -EFAULT;
		mutex_unlock(&sf->lock);
		break;
	/* The SPI speed is the TA's; stock takes these and does nothing. */
	case SF_IOC_SPI_SPEED:
	case SPI_IOC_WR_SPEED:
	case SPI_IOC_RD_SPEED:
	/* Power stays on from probe, as stock. */
	case SF_IOC_ENABLE_POWER:
	case SF_IOC_DISABLE_POWER:
	case SF_IOC_INIT_DRIVER:
	case SF_IOC_DEINIT_DRIVER:
	case SF_IOC_ENABLE_IRQ:
	case SF_IOC_DISABLE_IRQ:
	case SF_IOC_REQUEST_IRQ:
	case SF_IOC_SYNC_CONFIG:
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long sf_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return sf_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

/*
 * The HAL only tells the TA about the screen when its flag changes on a
 * SCREEN_STATUS uevent, and it starts after SurfaceFlinger's first unblank,
 * so it would miss the boot state. Once it has opened the device and started
 * listening, send the other state then the current one, which makes it sync
 * the TA whatever its flag holds. This is sent twice, 1 s and 4 s after open,
 * in case the HAL's thread starts late.
 */
static void sf_screen_work(struct work_struct *work)
{
	struct sf_dev *sf = container_of(to_delayed_work(work), struct sf_dev, screen_work);
	char *on[] = { "SCREEN_STATUS=ON", NULL };
	char *off[] = { "SCREEN_STATUS=OFF", NULL };
	struct kobject *kobj = &sf->misc.this_device->kobj;

	mutex_lock(&sf->screen_lock);
	kobject_uevent_env(kobj, KOBJ_CHANGE, sf->screen_on ? off : on);
	kobject_uevent_env(kobj, KOBJ_CHANGE, sf->screen_on ? on : off);
	if (--sf->screen_syncs > 0)
		schedule_delayed_work(&sf->screen_work, msecs_to_jiffies(SF_SCREEN_RESYNC_MS));
	mutex_unlock(&sf->screen_lock);
}

static int sf_open(struct inode *inode, struct file *file)
{
	struct sf_dev *sf = container_of(file->private_data, struct sf_dev, misc);

	mutex_lock(&sf->screen_lock);
	sf->screen_syncs = SF_SCREEN_SYNCS;
	mutex_unlock(&sf->screen_lock);
	mod_delayed_work(system_wq, &sf->screen_work, msecs_to_jiffies(SF_SCREEN_SYNC_MS));
	return nonseekable_open(inode, file);
}

static const struct file_operations sf_fops = {
	.owner = THIS_MODULE,
	.open = sf_open,
	.unlocked_ioctl = sf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = sf_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", SF_DRV_VERSION);
}
static DEVICE_ATTR_RO(version);

static ssize_t chip_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct miscdevice *misc = dev_get_drvdata(dev);
	struct sf_dev *sf = container_of(misc, struct sf_dev, misc);

	return sprintf(buf, "0x%04x\n", sf->chip_id);
}
static DEVICE_ATTR_RO(chip_info);

static struct attribute *sf_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_chip_info.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sf);

static void sf_event_work(struct work_struct *work)
{
	struct sf_dev *sf = container_of(work, struct sf_dev, event_work);
	char *env[] = { "SPI_STATE=finger", NULL };

	kobject_uevent_env(&sf->misc.this_device->kobj, KOBJ_CHANGE, env);
}

static irqreturn_t sf_irq(int irq, void *data)
{
	struct sf_dev *sf = data;

	pm_wakeup_ws_event(sf->ws, SF_WAKEUP_MS, false);
	schedule_work(&sf->event_work);
	return IRQ_HANDLED;
}

/* The HAL tells the TA about the screen from these. */
static int sf_fb_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
	struct sf_dev *sf = container_of(nb, struct sf_dev, fb_notifier);
	struct fb_event *evdata = data;
	char *env[] = { NULL, NULL };
	bool on;
	int blank;

	if (event != FB_EVENT_BLANK || !evdata || !evdata->data)
		return 0;
	blank = *(int *)evdata->data;
	if (blank == FB_BLANK_UNBLANK)
		on = true;
	else if (blank == FB_BLANK_POWERDOWN)
		on = false;
	else
		return 0;
	env[0] = on ? "SCREEN_STATUS=ON" : "SCREEN_STATUS=OFF";
	mutex_lock(&sf->screen_lock);
	sf->screen_on = on;
	kobject_uevent_env(&sf->misc.this_device->kobj, KOBJ_CHANGE, env);
	mutex_unlock(&sf->screen_lock);
	return 0;
}

static int sf_state(struct sf_dev *sf, const char *name, struct pinctrl_state **state)
{
	*state = pinctrl_lookup_state(sf->pinctrl, name);
	if (IS_ERR(*state)) {
		dev_err(sf->dev, "no pin state %s\n", name);
		return PTR_ERR(*state);
	}
	return 0;
}

static int sf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk *sel, *parent;
	struct sf_dev *sf;
	unsigned int i;
	int ret;

	sf = devm_kzalloc(dev, sizeof(*sf), GFP_KERNEL);
	if (!sf)
		return -ENOMEM;
	sf->dev = dev;
	mutex_init(&sf->lock);
	INIT_WORK(&sf->event_work, sf_event_work);
	INIT_DELAYED_WORK(&sf->screen_work, sf_screen_work);
	mutex_init(&sf->screen_lock);
	sf->screen_on = true;	/* LK leaves the panel on and nothing has blanked yet */

	sf->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(sf->pinctrl))
		return PTR_ERR(sf->pinctrl);
	ret = sf_state(sf, "finger_power_high", &sf->power_high);
	ret = ret ?: sf_state(sf, "finger_rst_low", &sf->rst_low);
	ret = ret ?: sf_state(sf, "finger_rst_high", &sf->rst_high);
	if (ret)
		return ret;

	/* SPI0's clocks; stock's spi driver sets the mux to syspll_d5_d2 the same way. */
	sf->spi_clk = devm_clk_get(dev, "spi-clk");
	if (IS_ERR(sf->spi_clk))
		return dev_err_probe(dev, PTR_ERR(sf->spi_clk), "no spi-clk\n");
	sel = devm_clk_get(dev, "sel-clk");
	parent = devm_clk_get(dev, "parent-clk");
	if (IS_ERR(sel) || IS_ERR(parent) || clk_set_parent(sel, parent))
		dev_warn(dev, "SPI0 source clock not set\n");

	sf->irq = platform_get_irq(pdev, 0);
	if (sf->irq < 0)
		return sf->irq;

	pinctrl_select_state(sf->pinctrl, sf->power_high);
	msleep(10);
	sf_reset(sf);

	mutex_lock(&sf->lock);
	sf_spi_clk(sf, true);
	ret = sf_read_chip_id(sf);
	sf_spi_clk(sf, false);
	mutex_unlock(&sf->lock);
	if (ret) {
		dev_err(dev, "no chip ID through the TEE\n");
		return ret;
	}
	dev_info(dev, "read chip is ok: 0x%04x\n", sf->chip_id);

	sf->ws = wakeup_source_register(dev, "sf_wakelock");
	if (!sf->ws)
		return -ENOMEM;

	/* No parent, as stock's /devices/virtual/input one. */
	sf->input = input_allocate_device();
	if (!sf->input) {
		ret = -ENOMEM;
		goto err_ws;
	}
	sf->input->name = "fingerprint_key";
	for (i = 0; i < ARRAY_SIZE(sf_caps); i++)
		input_set_capability(sf->input, EV_KEY, sf_caps[i]);
	ret = input_register_device(sf->input);
	if (ret) {
		input_free_device(sf->input);
		goto err_ws;
	}

	sf->misc.minor = MISC_DYNAMIC_MINOR;
	sf->misc.name = "sunwave_fp";
	sf->misc.fops = &sf_fops;
	sf->misc.groups = sf_groups;
	ret = misc_register(&sf->misc);
	if (ret)
		goto err_input;

	ret = request_irq(sf->irq, sf_irq, IRQF_TRIGGER_FALLING, "sf-irq", sf);
	if (ret)
		goto err_misc;
	enable_irq_wake(sf->irq);

	sf->fb_notifier.notifier_call = sf_fb_notifier;
	if (fb_register_client(&sf->fb_notifier))
		dev_warn(dev, "fb notifier not registered\n");

	dev_info(dev, "driver version: '%s'\n", SF_DRV_VERSION);
	return 0;

err_misc:
	misc_deregister(&sf->misc);
	cancel_delayed_work_sync(&sf->screen_work);
err_input:
	input_unregister_device(sf->input);
err_ws:
	wakeup_source_unregister(sf->ws);
	return ret;
}

static const struct of_device_id sf_of_match[] = {
	{ .compatible = "mediatek,fingerprint" },
	{ }
};

static struct platform_driver sf_driver = {
	.probe = sf_probe,
	.driver = {
		.name = "sunwave-fp",
		.of_match_table = sf_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(sf_driver);
