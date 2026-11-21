// SPDX-License-Identifier: GPL-2.0
/*
 * Awinic AW9523 keyboard matrix on the Unihertz Titan Pocket (i2c4 0x58).
 *
 * Based on some00's aw9523_key.c for the 4.14 kernel, with the behaviour of
 * the stock 4.14 driver as read from the shipped Image.
 *
 * Port 0 is the 8 rows (inputs, low = pressed), port 1 bits 1 to 6 are the
 * 6 columns. An interrupt on port 0 starts a scan; while any key is down the
 * matrix is scanned again every 1.5 ms, then interrupts are turned back on.
 */
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/pocket-touchpad.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/workqueue.h>

#define AW9523_REG_P0		0x00
#define AW9523_REG_P1		0x01
#define AW9523_REG_P1_LED	0x03
#define AW9523_REG_P0_CONFIG	0x04
#define AW9523_REG_P1_CONFIG	0x05
#define AW9523_REG_P0_INT	0x06
#define AW9523_REG_P1_INT	0x07
#define AW9523_REG_CHIPID	0x10
#define AW9523_REG_P0_LED_MODE	0x12
#define AW9523_REG_P1_LED_MODE	0x13
#define AW9523_REG_LAST		0x2f

#define AW9523_CHIPID		0x23
#define AW9523_COL_MASK		0x7e
#define AW9523_ALL_RELEASED	0xff
#define AW9523_NUM_COLS		8
/* Rescan period while a key is down; stock's only debounce. */
#define AW9523_SCAN_NS		1500000
/* Scans in a row with a failed transfer (about 150 ms) before giving up on the press. */
#define AW9523_MAX_FAILED_SCANS	100

/*
 * Stock leaves the keyboard suspended after probe until the pad's probe
 * resumes it or the first FB_EVENT_BLANK unblank. Set
 * aw9523_key.start_suspended=0 to start it live.
 */
static bool start_suspended = true;
module_param(start_suspended, bool, 0444);
MODULE_PARM_DESC(start_suspended, "Stay suspended until the pad probes or the first fb unblank, as stock does");

struct aw9523_key {
	const char *name;
	unsigned short code;
	u8 row;
	u8 col;
};

/* Same 35 entries, codes, rows and columns as the stock Image's table. */
static const struct aw9523_key aw9523_keymap[] = {
	{ "Q", KEY_Q, 4, 2 },		{ "W", KEY_W, 3, 2 },
	{ "E", KEY_E, 2, 2 },		{ "R", KEY_R, 1, 2 },
	{ "T", KEY_T, 0, 2 },		{ "P", KEY_P, 4, 1 },
	{ "O", KEY_O, 3, 1 },		{ "U", KEY_U, 1, 1 },
	{ "I", KEY_I, 2, 1 },		{ "J", KEY_J, 3, 4 },
	{ "K", KEY_K, 2, 4 },		{ "L", KEY_L, 1, 4 },
	{ "DEL", KEY_BACKSPACE, 0, 4 },	{ "A", KEY_A, 4, 3 },
	{ "S", KEY_S, 3, 3 },		{ "D", KEY_D, 2, 3 },
	{ "F", KEY_F, 1, 3 },		{ "G", KEY_G, 0, 3 },
	{ "Y", KEY_Y, 0, 1 },		{ "H", KEY_H, 4, 4 },
	{ "SPACE", KEY_SPACE, 4, 5 },	{ "Z", KEY_Z, 3, 5 },
	{ "X", KEY_X, 2, 5 },		{ "C", KEY_C, 1, 5 },
	{ "V", KEY_V, 0, 5 },		{ "ENTER", KEY_ENTER, 3, 6 },
	{ "M", KEY_M, 2, 6 },		{ "B", KEY_B, 1, 6 },
	{ "N", KEY_N, 0, 6 },		{ "SYM", KEY_COMPOSE, 5, 2 },
	{ "BACK", KEY_BACK, 5, 3 },	{ "MENU", KEY_APPSELECT, 5, 4 },
	{ "SHIFT-L", KEY_LEFTSHIFT, 5, 1 }, { "F13", KEY_F13, 6, 1 },
	{ "ALT", KEY_RIGHTALT, 7, 1 },
};

struct aw9523_kbd {
	struct i2c_client *client;
	struct input_dev *input;
	struct mutex lock;		/* serialises register access */
	struct work_struct work;	/* runs on system_wq, as stock */
	struct hrtimer timer;
	struct notifier_block fb_notifier;
	int irq;
	struct mutex pm_lock;		/* serialises suspend and resume */
	bool suspended;			/* under pm_lock */
	bool wake_enabled;
	bool stopping;			/* set by remove; stops the timer and work re-arming */
	unsigned int io_errors;		/* failed transfers, counted under lock */
	unsigned int failed_scans;	/* only touched by the work */
	u8 keyst_old[AW9523_NUM_COLS];
	u8 keyst_new[AW9523_NUM_COLS];
};

static int aw9523_read(struct aw9523_kbd *kbd, u8 reg)
{
	int ret;

	mutex_lock(&kbd->lock);
	ret = i2c_smbus_read_byte_data(kbd->client, reg);
	if (ret < 0)
		kbd->io_errors++;
	mutex_unlock(&kbd->lock);
	if (ret < 0)
		dev_err_ratelimited(&kbd->client->dev, "read reg 0x%02x failed: %d\n", reg, ret);
	return ret;
}

static void aw9523_write(struct aw9523_kbd *kbd, u8 reg, u8 val)
{
	int ret;

	mutex_lock(&kbd->lock);
	ret = i2c_smbus_write_byte_data(kbd->client, reg, val);
	if (ret < 0)
		kbd->io_errors++;
	mutex_unlock(&kbd->lock);
	if (ret < 0)
		dev_err_ratelimited(&kbd->client->dev, "write 0x%02x to reg 0x%02x failed: %d\n", val, reg, ret);
}

static int aw9523_read_chipid(struct aw9523_kbd *kbd)
{
	int tries, ret;

	for (tries = 0; tries < 3; tries++) {
		ret = aw9523_read(kbd, AW9523_REG_CHIPID);
		if (ret == AW9523_CHIPID)
			return 0;
		msleep(5);
	}
	dev_err(&kbd->client->dev, "chip id 0x%02x, expected 0x%02x\n", ret & 0xff, AW9523_CHIPID);
	return -ENODEV;
}

static unsigned int aw9523_io_errors(struct aw9523_kbd *kbd)
{
	unsigned int errors;

	mutex_lock(&kbd->lock);
	errors = kbd->io_errors;
	mutex_unlock(&kbd->lock);
	return errors;
}

/* Stock's init writes, in stock's order. A failed read skips its write. */
static int aw9523_key_init(struct aw9523_kbd *kbd)
{
	unsigned int errors = aw9523_io_errors(kbd);
	int val;

	if (aw9523_read(kbd, AW9523_REG_P0_LED_MODE) >= 0)
		aw9523_write(kbd, AW9523_REG_P0_LED_MODE, 0xff);
	val = aw9523_read(kbd, AW9523_REG_P1_LED_MODE);
	if (val >= 0)
		aw9523_write(kbd, AW9523_REG_P1_LED_MODE, val | AW9523_COL_MASK);
	if (aw9523_read(kbd, AW9523_REG_P0_INT) >= 0)
		aw9523_write(kbd, AW9523_REG_P0_INT, 0xff);
	val = aw9523_read(kbd, AW9523_REG_P1_INT);
	if (val >= 0)
		aw9523_write(kbd, AW9523_REG_P1_INT, val | AW9523_COL_MASK);
	if (aw9523_read(kbd, AW9523_REG_P0_CONFIG) >= 0)
		aw9523_write(kbd, AW9523_REG_P0_CONFIG, 0xff);
	val = aw9523_read(kbd, AW9523_REG_P1_CONFIG);
	if (val >= 0)
		aw9523_write(kbd, AW9523_REG_P1_CONFIG, val & 0x81);
	val = aw9523_read(kbd, AW9523_REG_P1_LED);
	if (val >= 0)
		aw9523_write(kbd, AW9523_REG_P1_LED, val & 0x81);
	aw9523_read(kbd, AW9523_REG_P0);
	aw9523_read(kbd, AW9523_REG_P1);
	aw9523_read(kbd, AW9523_REG_P0_INT);
	aw9523_write(kbd, AW9523_REG_P0_INT, 0);
	return aw9523_io_errors(kbd) == errors ? 0 : -EIO;
}

/* Mask port 0 interrupts and the IRQ. No reinit and no GPIO writes, as stock. */
static void aw9523_key_suspend(struct aw9523_kbd *kbd)
{
	mutex_lock(&kbd->pm_lock);
	if (!kbd->suspended) {
		if (aw9523_read(kbd, AW9523_REG_P0_INT) >= 0)
			aw9523_write(kbd, AW9523_REG_P0_INT, 0xff);
		disable_irq_nosync(kbd->irq);
		kbd->suspended = true;
	}
	mutex_unlock(&kbd->pm_lock);
}

static void aw9523_key_resume(struct aw9523_kbd *kbd)
{
	mutex_lock(&kbd->pm_lock);
	if (kbd->suspended) {
		aw9523_read(kbd, AW9523_REG_P0);
		aw9523_read(kbd, AW9523_REG_P1);
		aw9523_read(kbd, AW9523_REG_P0_INT);
		aw9523_write(kbd, AW9523_REG_P0_INT, 0);
		kbd->suspended = false;
		enable_irq(kbd->irq);
	}
	mutex_unlock(&kbd->pm_lock);
}

/* The one keyboard, for the pad's probe. Stock's agold_key_resume() uses a global too. */
static struct aw9523_kbd *aw9523_kbd_dev;

/* Called once from the pad's probe, as stock's agold_key_resume(). */
static void aw9523_pad_key_resume(void)
{
	struct aw9523_kbd *kbd = READ_ONCE(aw9523_kbd_dev);

	if (kbd)
		aw9523_key_resume(kbd);
}

static int aw9523_fb_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
	struct aw9523_kbd *kbd = container_of(nb, struct aw9523_kbd, fb_notifier);
	struct fb_event *evdata = data;
	int blank;

	if (event != FB_EVENT_BLANK || !evdata || !evdata->data)
		return 0;
	blank = *(int *)evdata->data;
	if (blank == FB_BLANK_UNBLANK)
		aw9523_key_resume(kbd);
	else if (blank == FB_BLANK_POWERDOWN)
		aw9523_key_suspend(kbd);
	return 0;
}

/* Threaded handler on a level-low, oneshot IRQ: the work turns it back on. */
static irqreturn_t aw9523_irq(int irq, void *data)
{
	struct aw9523_kbd *kbd = data;

	aw9523_write(kbd, AW9523_REG_P0_INT, 0xff);
	disable_irq_nosync(kbd->irq);
	queue_work(system_wq, &kbd->work);
	return IRQ_HANDLED;
}

static const struct aw9523_key *aw9523_lookup(int row, int col)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(aw9523_keymap); i++)
		if (aw9523_keymap[i].row == row && aw9523_keymap[i].col == col)
			return &aw9523_keymap[i];
	return NULL;
}

static void aw9523_key_work(struct work_struct *work)
{
	struct aw9523_kbd *kbd = container_of(work, struct aw9523_kbd, work);
	unsigned int errors = aw9523_io_errors(kbd);
	bool all_released = true;
	int col, row, val, i;

	if (READ_ONCE(kbd->stopping))
		return;

	/* Columns 1 to 6: drive one low, read the rows. Columns 0 and 7 aren't wired. */
	for (col = 0; col < AW9523_NUM_COLS; col++) {
		u8 bit = BIT(col);

		if (bit & AW9523_COL_MASK) {
			aw9523_write(kbd, AW9523_REG_P1_CONFIG, AW9523_COL_MASK ^ bit);
			val = aw9523_read(kbd, AW9523_REG_P0);
			kbd->keyst_new[col] = val >= 0 ? val : kbd->keyst_old[col];
		} else {
			kbd->keyst_new[col] = AW9523_ALL_RELEASED;
		}
		if (kbd->keyst_new[col] != AW9523_ALL_RELEASED)
			all_released = false;
	}

	for (col = 0; col < AW9523_NUM_COLS; col++) {
		u8 changed = kbd->keyst_old[col] ^ kbd->keyst_new[col];

		for (row = 0; changed && row < 8; row++) {
			const struct aw9523_key *key;
			int down;

			if (!(changed & BIT(row)))
				continue;
			key = aw9523_lookup(row, col);
			if (!key)
				continue;
			down = !(kbd->keyst_new[col] & BIT(row));
			/* Stock puts the pad to sleep on each key down, before the report. */
			if (down && touchpad_flag == 1)
				agold_touchpad_resume_suspend(true);
			input_report_key(kbd->input, key->code, down);
			input_sync(kbd->input);
			dev_dbg(&kbd->client->dev, "%s row %d col %d code %d val %d\n",
				key->name, row, col, key->code, down);
		}
		kbd->keyst_old[col] = kbd->keyst_new[col];
	}

	if (aw9523_io_errors(kbd) == errors)
		kbd->failed_scans = 0;
	else if (++kbd->failed_scans >= AW9523_MAX_FAILED_SCANS)
		goto give_up;

	if (!all_released) {
		hrtimer_start(&kbd->timer, ns_to_ktime(AW9523_SCAN_NS), HRTIMER_MODE_REL);
		return;
	}

	/* Columns back to inputs; if no row reads low, turn interrupts back on. */
	aw9523_write(kbd, AW9523_REG_P1_CONFIG, 0);
	aw9523_read(kbd, AW9523_REG_P1);
	val = aw9523_read(kbd, AW9523_REG_P0);
	if (val == AW9523_ALL_RELEASED) {
		aw9523_write(kbd, AW9523_REG_P0_INT, 0);
		/* Every key is up: wake the pad, as stock. */
		if (touchpad_flag == 1)
			agold_touchpad_resume_suspend(false);
		enable_irq(kbd->irq);
		return;
	}
	hrtimer_start(&kbd->timer, ns_to_ktime(AW9523_SCAN_NS), HRTIMER_MODE_REL);
	return;

give_up:
	/* The chip stopped answering: release every key and wait for the next interrupt. */
	dev_err(&kbd->client->dev, "%u scans failed, releasing all keys\n", kbd->failed_scans);
	kbd->failed_scans = 0;
	for (i = 0; i < ARRAY_SIZE(aw9523_keymap); i++)
		input_report_key(kbd->input, aw9523_keymap[i].code, 0);
	input_sync(kbd->input);
	memset(kbd->keyst_old, AW9523_ALL_RELEASED, sizeof(kbd->keyst_old));
	aw9523_write(kbd, AW9523_REG_P1_CONFIG, 0);
	aw9523_write(kbd, AW9523_REG_P0_INT, 0);
	enable_irq(kbd->irq);
}

static enum hrtimer_restart aw9523_timer(struct hrtimer *timer)
{
	struct aw9523_kbd *kbd = container_of(timer, struct aw9523_kbd, timer);

	if (!READ_ONCE(kbd->stopping))
		queue_work(system_wq, &kbd->work);
	return HRTIMER_NORESTART;
}

static ssize_t aw9523_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9523_kbd *kbd = i2c_get_clientdata(to_i2c_client(dev));
	ssize_t len = 0;
	int reg;

	for (reg = 0; reg <= AW9523_REG_LAST; reg++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "reg[0x%02x] = 0x%02x\n",
				 reg, aw9523_read(kbd, reg) & 0xff);
	return len;
}

static ssize_t aw9523_reg_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct aw9523_kbd *kbd = i2c_get_clientdata(to_i2c_client(dev));
	unsigned int reg, val;

	if (sscanf(buf, "%x %x", &reg, &val) != 2 || reg > 0xff || val > 0xff)
		return -EINVAL;
	aw9523_write(kbd, reg, val);
	return count;
}

static DEVICE_ATTR(aw9523_reg, 0660, aw9523_reg_show, aw9523_reg_store);

static void aw9523_select_state(struct device *dev, struct pinctrl *pinctrl, const char *name)
{
	struct pinctrl_state *state = pinctrl_lookup_state(pinctrl, name);

	if (IS_ERR(state) || pinctrl_select_state(pinctrl, state))
		dev_warn(dev, "pinctrl state %s not set\n", name);
}

static int aw9523_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct aw9523_kbd *kbd;
	struct pinctrl *pinctrl;
	int reset_gpio, irq_gpio, i, ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	kbd = devm_kzalloc(dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;
	kbd->client = client;
	mutex_init(&kbd->lock);
	mutex_init(&kbd->pm_lock);
	INIT_WORK(&kbd->work, aw9523_key_work);
	hrtimer_init(&kbd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	kbd->timer.function = aw9523_timer;
	memset(kbd->keyst_old, AW9523_ALL_RELEASED, sizeof(kbd->keyst_old));
	memset(kbd->keyst_new, AW9523_ALL_RELEASED, sizeof(kbd->keyst_new));
	i2c_set_clientdata(client, kbd);

	/* Stock selects reset_high then eint before touching the GPIOs. */
	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(dev, PTR_ERR(pinctrl), "no pinctrl\n");
	aw9523_select_state(dev, pinctrl, "aw9523_reset_high");
	aw9523_select_state(dev, pinctrl, "aw9523_eint");

	/* GPIO 178 comes out of the bootloader low, holding the chip in reset. */
	reset_gpio = of_get_named_gpio(np, "awinic,reset-gpio", 0);
	if (reset_gpio < 0)
		return dev_err_probe(dev, reset_gpio, "no reset gpio\n");
	ret = devm_gpio_request_one(dev, reset_gpio, GPIOF_OUT_INIT_HIGH, "reset-gpio");
	if (ret)
		return dev_err_probe(dev, ret, "reset gpio %d busy\n", reset_gpio);
	gpio_set_value(reset_gpio, 0);
	msleep(1);
	gpio_set_value(reset_gpio, 1);
	msleep(1);

	irq_gpio = of_get_named_gpio(np, "awinic,irq-gpio", 0);
	if (irq_gpio < 0)
		return dev_err_probe(dev, irq_gpio, "no irq gpio\n");
	ret = devm_gpio_request_one(dev, irq_gpio, GPIOF_IN, "irq-gpio");
	if (ret)
		return dev_err_probe(dev, ret, "irq gpio %d busy\n", irq_gpio);
	kbd->irq = gpio_to_irq(irq_gpio);
	if (kbd->irq < 0)
		return dev_err_probe(dev, kbd->irq, "no irq for gpio %d\n", irq_gpio);

	ret = aw9523_read_chipid(kbd);
	if (ret)
		return ret;

	kbd->input = devm_input_allocate_device(dev);
	if (!kbd->input)
		return -ENOMEM;
	/* No EV_REP and bustype left 0, as stock. */
	kbd->input->name = "aw9523-key";
	kbd->input->phys = "aw9523-keys/input0";
	__set_bit(EV_KEY, kbd->input->evbit);
	for (i = 0; i < ARRAY_SIZE(aw9523_keymap); i++)
		__set_bit(aw9523_keymap[i].code, kbd->input->keybit);
	ret = input_register_device(kbd->input);
	if (ret)
		return ret;

	ret = aw9523_key_init(kbd);
	if (ret)
		return dev_err_probe(dev, ret, "init writes failed\n");

	ret = devm_request_threaded_irq(dev, kbd->irq, NULL, aw9523_irq,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT, "aw9523_irq", kbd);
	if (ret)
		return dev_err_probe(dev, ret, "irq %d request failed\n", kbd->irq);
	kbd->wake_enabled = !irq_set_irq_wake(kbd->irq, 1);
	if (!kbd->wake_enabled)
		dev_warn(dev, "irq %d can't wake\n", kbd->irq);
	device_init_wakeup(dev, true);

	kbd->fb_notifier.notifier_call = aw9523_fb_notifier;
	if (fb_register_client(&kbd->fb_notifier))
		dev_warn(dev, "fb notifier not registered\n");

	ret = device_create_file(dev, &dev_attr_aw9523_reg);
	if (ret)
		dev_warn(dev, "aw9523_reg not created: %d\n", ret);

	if (start_suspended)
		aw9523_key_suspend(kbd);

	/*
	 * Stock's pad probe resumes the keyboard. Hand it our resume, and do it
	 * here if the pad has already probed. A double resume does nothing.
	 */
	WRITE_ONCE(aw9523_kbd_dev, kbd);
	pocket_touchpad_set_key_resume(aw9523_pad_key_resume);
	if (touchpad_flag == 1)
		aw9523_key_resume(kbd);

	dev_info(dev, "keypad, irq %d%s\n", kbd->irq,
		 READ_ONCE(kbd->suspended) ? ", suspended until pad probe or fb unblank" : "");
	return 0;
}

static int aw9523_remove(struct i2c_client *client)
{
	struct aw9523_kbd *kbd = i2c_get_clientdata(client);

	pocket_touchpad_set_key_resume(NULL);
	WRITE_ONCE(aw9523_kbd_dev, NULL);
	fb_unregister_client(&kbd->fb_notifier);
	device_remove_file(&client->dev, &dev_attr_aw9523_reg);
	WRITE_ONCE(kbd->stopping, true);
	disable_irq(kbd->irq);
	hrtimer_cancel(&kbd->timer);
	cancel_work_sync(&kbd->work);
	hrtimer_cancel(&kbd->timer);
	if (kbd->wake_enabled)
		irq_set_irq_wake(kbd->irq, 0);
	device_init_wakeup(&client->dev, false);
	aw9523_write(kbd, AW9523_REG_P0, 0);
	devm_free_irq(&client->dev, kbd->irq, kbd);
	return 0;
}

static const struct i2c_device_id aw9523_id[] = {
	{ "aw9523-key", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw9523_id);

static const struct of_device_id aw9523_of_match[] = {
	{ .compatible = "awinic,aw9523-key" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9523_of_match);

static struct i2c_driver aw9523_driver = {
	.driver = {
		.name = "aw9523-key",
		.of_match_table = aw9523_of_match,
	},
	.probe = aw9523_probe,
	.remove = aw9523_remove,
	.id_table = aw9523_id,
};
module_i2c_driver(aw9523_driver);

MODULE_DESCRIPTION("AW9523 keyboard matrix for the Titan Pocket");
MODULE_LICENSE("GPL");
