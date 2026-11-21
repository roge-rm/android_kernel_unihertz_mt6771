// SPDX-License-Identifier: GPL-2.0
/*
 * Titan Pocket touchscreen: a Goodix GT1151QM on i2c0.
 *
 * Behaves as the stock 4.14 driver (gt1151qm under mtk-tpd) does: talks at
 * 0x14 whatever the DT reg says; GPIO 153 switches the touch supply; the
 * INT and reset lines are driven through the stock pinctrl states; no config
 * is sent to the chip; the IRQ is NO_SUSPEND and a wake source; an ESD check
 * runs every 2 s; touches go out as MT protocol A through an input device
 * named mtk-tpd with fixed 720x720 ranges. Sleep keeps the supply on. The
 * Agold gestures, /dev/touch and the debug and update nodes are left out.
 */

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "pocket-gt1151-report.h"

#define GT_NAME			"mtk-tpd"
#define GT_ADDR			0x14	/* stock overrides the DT reg (0x5d) with this */

/* Stock takes the range from the LCM (DISP_GetScreenWidth/Height). */
#define GT_RES_X		720
#define GT_RES_Y		720
#define GT_MAX_WIDTH		100
#define GT_MAX_ID		10

#define GT_REG_CMD		0x8040
#define GT_REG_RQST		0x8044
#define GT_REG_VERSION		0x8140
#define GT_REG_COOR		0x814e
#define GT_REG_HW_INFO		0x4220
#define GT_REG_MAIN_CHECK	0x41e4
#define GT_REG_SUB_CHECK	0x5095
#define GT_REG_ESD_RESET	0x4226

#define GT_CMD_SLEEP		0x05
#define GT_CMD_ESD		0xaa

#define GT_RQST_CONFIG		0x01
#define GT_RQST_BAK_REF		0x02
#define GT_RQST_RESET		0x03
#define GT_RQST_MAIN_CLOCK	0x04

#define GT_HW_GT1151QM		0x00900802
#define GT_HW_GT2X		0x00901003

/* Stock error codes: bit 31 plus one bit for the kind. */
#define GT_ERROR		(BIT(31) | BIT(1))
#define GT_ERROR_IIC		(BIT(31) | BIT(2))
#define GT_ERROR_VALUE		(BIT(31) | BIT(16))

/* Stock queues the ESD check every 500 jiffies at HZ=250. */
#define GT_ESD_PERIOD		(2 * HZ)

enum gt_pin {
	GT_PIN_INT,
	GT_PIN_RST,
};

struct gt1151 {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *power;
	struct pinctrl *pinctrl;
	struct pinctrl_state *int_as_int;
	struct pinctrl_state *out[2][2];	/* [pin][level] */

	/* Chip access from the IRQ thread, ESD work, sleep and wake. */
	struct mutex lock;
	/*
	 * The ESD check, sleep and wake, as stock's ESD mutex: the ESD work
	 * holds it through its 50 ms sleeps, without lock, so touch reports
	 * go on. Taken before lock.
	 */
	struct mutex cmd_lock;
	/* Stock irq_flag: true while this driver holds the IRQ disabled. */
	spinlock_t irq_lock;
	bool irq_off;
	bool wake_enabled;

	bool powered;
	bool halted;		/* asleep; interrupts are ignored */
	bool resetting;
	u8 int_type;		/* 0 rising, 1 falling */
	u8 wakeup_level;
	struct pocket_gt_state report;

	char product_id[5];
	u32 patch_id;
	u32 mask_id;

	struct workqueue_struct *wq;
	struct delayed_work esd_work;
	struct mutex esd_lock;
	bool esd_running;

	struct notifier_block fb_notifier;
	struct workqueue_struct *resume_wq;
	struct work_struct resume_work;
	bool lcd_off;
};

static void gt_pin(struct gt1151 *ts, enum gt_pin pin, int level)
{
	int ret = pinctrl_select_state(ts->pinctrl, ts->out[pin][!!level]);

	if (ret)
		dev_err(&ts->client->dev, "pin state %d/%d: %d\n", pin, level, ret);
}

static void gt_pin_as_int(struct gt1151 *ts)
{
	int ret = pinctrl_select_state(ts->pinctrl, ts->int_as_int);

	if (ret)
		dev_err(&ts->client->dev, "int pin state: %d\n", ret);
}

/* Register address big-endian, then the data, in one transfer each way. */
static int gt_read(struct gt1151 *ts, u16 reg, u8 *buf, int len)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = GT_ADDR, .flags = 0, .len = 2, .buf = addr },
		{ .addr = GT_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret = i2c_transfer(ts->client->adapter, msgs, 2);

	if (ret != 2) {
		dev_dbg(&ts->client->dev, "read 0x%04x: %d\n", reg, ret);
		return GT_ERROR_IIC;
	}
	return 0;
}

static int gt_write(struct gt1151 *ts, u16 reg, const u8 *data, int len)
{
	u8 buf[8];
	struct i2c_msg msg = { .addr = GT_ADDR, .flags = 0, .len = len + 2, .buf = buf };
	int ret;

	if (len > sizeof(buf) - 2)
		return GT_ERROR;
	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	memcpy(buf + 2, data, len);
	ret = i2c_transfer(ts->client->adapter, &msg, 1);
	if (ret != 1) {
		dev_dbg(&ts->client->dev, "write 0x%04x: %d\n", reg, ret);
		return GT_ERROR_IIC;
	}
	return 0;
}

/* Stock reads twice, 5 ms apart, and wants the same bytes. */
static int gt_read_dbl_check(struct gt1151 *ts, u16 reg, u8 *buf, int len)
{
	u8 first[16], second[16];
	int ret;

	if (len > sizeof(first))
		return GT_ERROR;
	memset(first, 0xaa, sizeof(first));
	ret = gt_read(ts, reg, first, len);
	if (ret)
		return ret;
	msleep(5);
	memset(second, 0, sizeof(second));
	ret = gt_read(ts, reg, second, len);
	if (ret)
		return ret;
	if (memcmp(first, second, len)) {
		dev_err(&ts->client->dev, "read 0x%04x, %d bytes, double check failed\n", reg, len);
		return GT_ERROR;
	}
	memcpy(buf, second, len);
	return 0;
}

/* Data and checksum to 0x8041, then the command to 0x8040. */
static int gt_write_cmd(struct gt1151 *ts, u8 cmd, u8 data)
{
	u8 buf[3] = { cmd, data, (u8)(0 - cmd - data) };
	int ret;

	ret = gt_write(ts, GT_REG_CMD + 1, buf + 1, 2);
	ret |= gt_write(ts, GT_REG_CMD, buf, 1);
	return ret;
}

/* The command, then 50 ms. */
static int gt_send_cmd(struct gt1151 *ts, u8 cmd, u8 data)
{
	int ret = gt_write_cmd(ts, cmd, data);

	msleep(50);
	return ret;
}

static void gt_irq_enable(struct gt1151 *ts)
{
	unsigned long flags;
	bool enable;

	spin_lock_irqsave(&ts->irq_lock, flags);
	enable = ts->irq_off;
	ts->irq_off = false;
	spin_unlock_irqrestore(&ts->irq_lock, flags);
	if (enable)
		enable_irq(ts->client->irq);
}

/* No wait for a running thread: callers may hold ts->lock, which it takes. */
static void gt_irq_disable(struct gt1151 *ts)
{
	unsigned long flags;
	bool disable;

	spin_lock_irqsave(&ts->irq_lock, flags);
	disable = !ts->irq_off;
	ts->irq_off = true;
	spin_unlock_irqrestore(&ts->irq_lock, flags);
	if (disable)
		disable_irq_nosync(ts->client->irq);
}

/* Stock gt1151qm_power_switch: both lines low, 10 ms, then GPIO 153. */
static void gt_power_switch(struct gt1151 *ts, bool on)
{
	gt_pin(ts, GT_PIN_RST, 0);
	gt_pin(ts, GT_PIN_INT, 0);
	msleep(10);
	if (on == ts->powered)
		return;
	gpiod_direction_output(ts->power, on);
	ts->powered = on;
}

/*
 * Stock gt1151qm_reset_guitar: INT high during the reset pulse selects
 * address 0x14, then INT low 50 ms to sync, then INT back to the EINT and
 * the reset status written. Returns the status write result.
 */
static int gt_reset_guitar(struct gt1151 *ts)
{
	static const u8 status[4] = { 0xaa, 0x00, 0x56, 0xaa };
	int ret;

	dev_dbg(&ts->client->dev, "reset\n");
	gt_pin(ts, GT_PIN_RST, 0);
	gt_pin(ts, GT_PIN_INT, 1);
	msleep(2);
	gt_pin(ts, GT_PIN_RST, 1);
	msleep(2);
	msleep(8);
	gt_pin(ts, GT_PIN_INT, 0);
	msleep(50);
	gt_pin_as_int(ts);

	ret = gt_write(ts, GT_REG_CMD + 1, status + 1, 3);
	if (ret)
		return ret;
	return gt_write(ts, GT_REG_CMD, status, 1);
}

/* Stock gt1151qm_power_reset: supply off 30 ms, on, up to 5 resets. No config. */
static void gt_power_reset(struct gt1151 *ts)
{
	int i;

	if (ts->resetting)
		return;
	dev_info(&ts->client->dev, "force reset\n");
	ts->resetting = true;
	gt_irq_disable(ts);
	gt_power_switch(ts, false);
	msleep(30);
	gt_power_switch(ts, true);
	msleep(30);
	for (i = 0; i < 5; i++)
		if (!gt_reset_guitar(ts))
			break;
	gt_irq_enable(ts);
	ts->resetting = false;
}

static int gt_get_chip_type(struct gt1151 *ts)
{
	u8 buf[4];
	u32 hw;
	int ret;

	ret = gt_read_dbl_check(ts, GT_REG_HW_INFO, buf, sizeof(buf));
	if (ret) {
		dev_err(&ts->client->dev, "chip type read failed\n");
		return -EIO;
	}
	hw = buf[0] | buf[1] << 8 | buf[2] << 16 | buf[3] << 24;
	if (hw == GT_HW_GT1151QM)
		dev_info(&ts->client->dev, "chip type GT1151QM\n");
	else if (hw == GT_HW_GT2X)
		dev_info(&ts->client->dev, "chip type GT2X\n");
	else {
		dev_err(&ts->client->dev, "unknown chip type 0x%08x\n", hw);
		return -ENODEV;
	}
	return 0;
}

static bool gt_num_or_char(u8 c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
}

/*
 * Stock gt1151qm_read_version: 12 bytes from 0x8140, three tries 100 ms
 * apart. As stock, a good read on the third try still counts as a failure.
 */
static int gt_read_version(struct gt1151 *ts)
{
	u8 buf[12];
	int retry, ret;

	for (retry = 0; retry < 3; retry++) {
		ret = gt_read_dbl_check(ts, GT_REG_VERSION, buf, sizeof(buf));
		if (!ret && !pocket_gt_checksum(buf, sizeof(buf)) && gt_num_or_char(buf[0]) &&
		    gt_num_or_char(buf[1]) && gt_num_or_char(buf[2]) &&
		    !(isdigit(buf[2]) && buf[10] == 0xff)) {
			if (retry == 2)
				break;
			memcpy(ts->product_id, buf, 4);
			ts->product_id[4] = 0;
			ts->patch_id = buf[4] << 16 | buf[5] << 8 | buf[6];
			ts->mask_id = buf[7] << 16 | buf[8] << 8 | buf[9];
			dev_info(&ts->client->dev,
				 "IC version GT%s_%06X(Patch)_%04X(Mask)_%02X(SensorID)\n",
				 ts->product_id, ts->patch_id, ts->mask_id >> 8, buf[10] & 0x0f);
			return 0;
		}
		dev_err(&ts->client->dev, "read version failed%s\n", ret ? "" : " (checksum)");
		msleep(100);
	}
	return -EIO;
}

/*
 * Stock gt1151qm_init: power on, up to 5 resets each followed by a check of
 * 0x41e4 (0xbe), chip type, version. Stock never calls init_panel here, so
 * no config is read or sent and the trigger stays rising.
 */
static int gt_init(struct gt1151 *ts)
{
	struct device *dev = &ts->client->dev;
	bool failed = false;
	int attempt, ret = -EIO;
	u8 val;

	gt_power_switch(ts, true);
	for (attempt = 1; attempt <= 5; attempt++) {
		failed = false;
		ret = gt_reset_guitar(ts);
		if (ret) {
			dev_err(dev, "reset failed\n");
			continue;
		}
		ret = gt_read_dbl_check(ts, GT_REG_MAIN_CHECK, &val, 1);
		if (ret)
			continue;
		if (val != 0xbe) {
			dev_err(dev, "main system check failed (0x%02x)\n", val);
			failed = true;
		}
		/* Stock keeps this read's result: if it fails, the defaults below apply. */
		ret = gt_read_dbl_check(ts, GT_REG_SUB_CHECK, &val, 1);
		if (!ret && val == 0xaa)
			dev_err(dev, "subsystem check (0x%02x)\n", val);
		break;
	}
	/* Stock also gives up when only the fifth try worked. */
	if (failed || attempt == 5) {
		dev_err(dev, "not a GT1151QM\n");
		return -ENODEV;
	}
	if (ret) {
		/* Stock carries on with these defaults, falling edge among them. */
		dev_err(dev, "init failed, using default setting\n");
		ts->int_type = 1;
		ts->wakeup_level = 1;
	}

	ret = gt_get_chip_type(ts);
	if (ret)
		return ret;
	return gt_read_version(ts);
}

/* Stock gt1151qm_request_event_handler. */
static void gt_request_event(struct gt1151 *ts)
{
	struct device *dev = &ts->client->dev;
	u8 rqst = 0;

	if (gt_read(ts, GT_REG_RQST, &rqst, 1)) {
		dev_err(dev, "request read failed\n");
		return;
	}
	switch (rqst & 0x0f) {
	case GT_RQST_CONFIG:
		/*
		 * Stock answers with its config buffer, which stays all zero since
		 * init_panel never runs, so its send_cfg refuses and writes nothing.
		 */
		dev_info(dev, "request config, not sent\n");
		break;
	case GT_RQST_RESET:
		dev_info(dev, "request reset\n");
		gt_reset_guitar(ts);
		rqst = 0;
		gt_write(ts, GT_REG_RQST, &rqst, 1);
		break;
	case GT_RQST_BAK_REF:
		dev_info(dev, "request ref\n");
		break;
	case GT_RQST_MAIN_CLOCK:
		dev_info(dev, "request main clock\n");
		break;
	}
}

static void gt_input_event(void *ctx, unsigned int type, unsigned int code, int value)
{
	input_event(ctx, type, code, value);
}

/* Stock gt1151qm_touch_event_handler: 0 or a stock error code. */
static int gt_touch_event(struct gt1151 *ts, u8 *data)
{
	struct pocket_gt_sink sink = { gt_input_event, ts->input };
	unsigned int touch_num = data[0] & 0x0f;
	unsigned int len;
	int ret;

	if (touch_num > POCKET_GT_MAX_TOUCH) {
		dev_err(&ts->client->dev, "illegal finger number %u\n", touch_num);
		return GT_ERROR_VALUE;
	}
	len = POCKET_GT_PACKET_LEN(touch_num);
	if (touch_num > 1) {
		ret = gt_read(ts, GT_REG_COOR + POCKET_GT_FIRST_READ,
			      data + POCKET_GT_FIRST_READ, len - POCKET_GT_FIRST_READ);
		if (ret)
			return ret;
	}
	if (pocket_gt_checksum(data, len)) {
		ret = gt_read(ts, GT_REG_COOR, data, len);
		if (ret)
			return ret;
		if (pocket_gt_checksum(data, len)) {
			dev_err(&ts->client->dev, "checksum error\n");
			return GT_ERROR_VALUE;
		}
	}
	pocket_gt_report(&sink, data, touch_num, &ts->report);
	return 0;
}

/*
 * Stock tpd_event_handler, one pass per interrupt. The IRQ stays off after
 * an interrupt while asleep until wake turns it back on.
 */
static irqreturn_t gt_irq(int irq, void *dev_id)
{
	struct gt1151 *ts = dev_id;
	u8 data[POCKET_GT_PACKET_MAX] = { 0 };
	u8 end = 0;
	int ret;

	mutex_lock(&ts->lock);
	if (ts->halted) {
		gt_irq_disable(ts);
		mutex_unlock(&ts->lock);
		dev_dbg(&ts->client->dev, "interrupt while asleep ignored\n");
		return IRQ_HANDLED;
	}
	ret = gt_read(ts, GT_REG_COOR, data, POCKET_GT_FIRST_READ);
	if (ret) {
		dev_err(&ts->client->dev, "coordinate read failed\n");
		goto out;
	}
	if (!data[0]) {
		gt_request_event(ts);
		goto out;
	}
	if (!(data[0] & 0x80))
		goto out;
	ret = gt_touch_event(ts, data);
	if (ret == 0 || ret == GT_ERROR_VALUE)
		gt_write(ts, GT_REG_COOR, &end, 1);
out:
	mutex_unlock(&ts->lock);
	return IRQ_HANDLED;
}

static void gt_esd_switch(struct gt1151 *ts, bool on)
{
	mutex_lock(&ts->esd_lock);
	if (on && !ts->esd_running) {
		ts->esd_running = true;
		queue_delayed_work(ts->wq, &ts->esd_work, GT_ESD_PERIOD);
	} else if (!on && ts->esd_running) {
		ts->esd_running = false;
		cancel_delayed_work(&ts->esd_work);
	}
	mutex_unlock(&ts->esd_lock);
}

/*
 * Stock gt1151qm_esd_check_func: up to 3 reads of 0x8040, 50 ms apart; a
 * live chip shows 0x8040 != 0xaa and 0x8043 == 0xaa and gets fed 0xaa.
 * Otherwise 0x4226 gets 01 01 01 01 and the supply is cycled.
 * Only the reset holds lock across its sleeps, to keep the IRQ thread out.
 */
static void gt_esd_work(struct work_struct *work)
{
	struct gt1151 *ts = container_of(to_delayed_work(work), struct gt1151, esd_work);
	u8 buf[4] = { 0 };
	static const u8 reset[4] = { 1, 1, 1, 1 };
	int i, ret;

	mutex_lock(&ts->cmd_lock);
	if (!READ_ONCE(ts->esd_running))
		goto out;
	for (i = 0; i < 3; i++) {
		mutex_lock(&ts->lock);
		ret = gt_read(ts, GT_REG_CMD, buf, sizeof(buf));
		mutex_unlock(&ts->lock);
		if (!ret && buf[0] != 0xaa && buf[3] == 0xaa)
			break;
		msleep(50);
	}
	if (i < 3) {
		mutex_lock(&ts->lock);
		gt_write_cmd(ts, GT_CMD_ESD, 0);
		mutex_unlock(&ts->lock);
		msleep(50);
	} else if (READ_ONCE(ts->esd_running)) {
		dev_err(&ts->client->dev, "chip not answering ESD check, resetting\n");
		mutex_lock(&ts->lock);
		gt_write(ts, GT_REG_ESD_RESET, reset, sizeof(reset));
		msleep(50);
		gt_power_reset(ts);
		mutex_unlock(&ts->lock);
	}
out:
	mutex_unlock(&ts->cmd_lock);
	mutex_lock(&ts->esd_lock);
	if (ts->esd_running)
		queue_delayed_work(ts->wq, &ts->esd_work, GT_ESD_PERIOD);
	mutex_unlock(&ts->esd_lock);
}

/*
 * Stock gt1151qm_suspend with the Agold gesture flag off: ESD off, then the
 * sleep command, three tries. The supply stays on and the IRQ stays on.
 */
static void gt_suspend(struct gt1151 *ts)
{
	int i, ret = -EIO;

	dev_info(&ts->client->dev, "suspend\n");
	gt_esd_switch(ts, false);
	mutex_lock(&ts->cmd_lock);
	mutex_lock(&ts->lock);
	ts->halted = true;
	if (ts->wakeup_level == 1)
		gt_pin(ts, GT_PIN_INT, 0);
	msleep(5);
	for (i = 0; i < 3; i++) {
		ret = gt_send_cmd(ts, GT_CMD_SLEEP, 0);
		if (!ret)
			break;
		msleep(10);
	}
	mutex_unlock(&ts->lock);
	mutex_unlock(&ts->cmd_lock);
	if (ret)
		dev_err(&ts->client->dev, "enter sleep failed\n");
	msleep(58);
}

/*
 * Stock gt1151qm_resume. Its gesture_enabled byte is 1 from probe on, so it
 * wakes the chip by reset (two tries), not by an INT pulse.
 */
static void gt_resume(struct gt1151 *ts)
{
	int ret;

	dev_info(&ts->client->dev, "resume\n");
	mutex_lock(&ts->cmd_lock);
	mutex_lock(&ts->lock);
	gt_irq_disable(ts);
	ret = gt_reset_guitar(ts);
	if (ret)
		ret = gt_reset_guitar(ts);
	if (ret)
		dev_err(&ts->client->dev, "wake up failed\n");
	ts->halted = false;
	gt_irq_enable(ts);
	mutex_unlock(&ts->lock);
	mutex_unlock(&ts->cmd_lock);
	gt_esd_switch(ts, true);
}

static void gt_resume_work(struct work_struct *work)
{
	struct gt1151 *ts = container_of(work, struct gt1151, resume_work);

	gt_resume(ts);
	ts->lcd_off = false;
}

/* Stock tpd_fb_notifier_callback: sleep on powerdown, wake on unblank after one. */
static int gt_fb_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
	struct gt1151 *ts = container_of(nb, struct gt1151, fb_notifier);
	struct fb_event *evdata = data;
	int blank;

	if (event != FB_EVENT_BLANK || !evdata || !evdata->data)
		return 0;
	blank = *(int *)evdata->data;
	if (blank == FB_BLANK_POWERDOWN) {
		if (!ts->lcd_off) {
			cancel_work_sync(&ts->resume_work);
			gt_suspend(ts);
		}
		ts->lcd_off = true;
	} else if (blank == FB_BLANK_UNBLANK && ts->lcd_off) {
		queue_work(ts->resume_wq, &ts->resume_work);
	}
	return 0;
}

/* Stock convert_productname: four digits as nibbles, anything else adds '*'. */
static u16 gt_product(const char *name)
{
	u16 product = 0;
	int i;

	for (i = 0; i < 4; i++) {
		product <<= 4;
		if (name[i] < '0' || name[i] > '9')
			product += '*';
		else
			product += name[i] - '0';
	}
	return product;
}

static int gt_get_pins(struct gt1151 *ts)
{
	static const char *const names[2][2] = {
		{ "state_eint_output0", "state_eint_output1" },
		{ "state_rst_output0", "state_rst_output1" },
	};
	struct device *dev = &ts->client->dev;
	int pin, level;

	ts->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(ts->pinctrl))
		return dev_err_probe(dev, PTR_ERR(ts->pinctrl), "no pinctrl\n");
	ts->int_as_int = pinctrl_lookup_state(ts->pinctrl, "state_eint_as_int");
	if (IS_ERR(ts->int_as_int))
		return dev_err_probe(dev, PTR_ERR(ts->int_as_int), "no state_eint_as_int\n");
	for (pin = 0; pin < 2; pin++)
		for (level = 0; level < 2; level++) {
			ts->out[pin][level] = pinctrl_lookup_state(ts->pinctrl, names[pin][level]);
			if (IS_ERR(ts->out[pin][level]))
				return dev_err_probe(dev, PTR_ERR(ts->out[pin][level]),
						     "no %s\n", names[pin][level]);
		}
	return 0;
}

static void gt_destroy_wq(void *wq)
{
	destroy_workqueue(wq);
}

static int gt_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct input_dev *input;
	struct gt1151 *ts;
	unsigned long trigger;
	int ret;

	if (!client->irq)
		return dev_err_probe(dev, -EINVAL, "no IRQ\n");
	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	mutex_init(&ts->lock);
	mutex_init(&ts->cmd_lock);
	mutex_init(&ts->esd_lock);
	spin_lock_init(&ts->irq_lock);
	INIT_DELAYED_WORK(&ts->esd_work, gt_esd_work);
	INIT_WORK(&ts->resume_work, gt_resume_work);
	i2c_set_clientdata(client, ts);

	ts->power = devm_gpiod_get(dev, "power", GPIOD_ASIS);
	if (IS_ERR(ts->power))
		return dev_err_probe(dev, PTR_ERR(ts->power), "no power GPIO\n");
	ret = gt_get_pins(ts);
	if (ret)
		return ret;

	if (client->addr != GT_ADDR)
		dev_info(dev, "talking at 0x%02x, not the DT 0x%02x, as stock\n", GT_ADDR,
			 client->addr);

	ret = gt_init(ts);
	if (ret) {
		/* Stock switches the supply off when init fails. */
		gt_power_switch(ts, false);
		return dev_err_probe(dev, ret, "init failed\n");
	}

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	ts->input = input;
	input->name = GT_NAME;
	input->id.vendor = 0;
	input->id.product = gt_product(ts->product_id);
	input->id.version = ts->patch_id >> 8;
	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	/* Stock sets these for the gestures; they're kept for the same capabilities. */
	input_set_capability(input, EV_KEY, KEY_F3);
	input_set_capability(input, EV_KEY, KEY_F2);
	input_set_capability(input, EV_KEY, KEY_POWER);
	input_set_capability(input, EV_KEY, KEY_TV);
	input_set_abs_params(input, ABS_X, 0, GT_RES_X, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, GT_RES_Y, 0, 0);
	input_abs_set_res(input, ABS_X, GT_RES_X);
	input_abs_set_res(input, ABS_Y, GT_RES_Y);
	input_set_abs_params(input, ABS_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, GT_RES_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, GT_RES_Y, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, GT_MAX_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, GT_MAX_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_TRACKING_ID, 0, GT_MAX_ID, 0, 0);
	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(dev, ret, "input register failed\n");

	gt_pin_as_int(ts);
	msleep(50);

	ts->wq = alloc_ordered_workqueue("gt1151qm_workthread", 0);
	if (!ts->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, gt_destroy_wq, ts->wq);
	if (ret)
		return ret;
	ts->resume_wq = alloc_ordered_workqueue("touch_resume", 0);
	if (!ts->resume_wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, gt_destroy_wq, ts->resume_wq);
	if (ret)
		return ret;

	/* Stock: rising unless init fell back to its defaults. */
	trigger = ts->int_type ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
	ret = devm_request_threaded_irq(dev, client->irq, NULL, gt_irq,
					trigger | IRQF_ONESHOT | IRQF_NO_SUSPEND,
					"TOUCH_PANEL-eint", ts);
	if (ret)
		return dev_err_probe(dev, ret, "irq %d request failed\n", client->irq);
	ts->wake_enabled = !enable_irq_wake(client->irq);
	if (!ts->wake_enabled)
		dev_warn(dev, "irq %d wake not enabled\n", client->irq);

	gt_esd_switch(ts, true);

	ts->fb_notifier.notifier_call = gt_fb_notifier;
	if (fb_register_client(&ts->fb_notifier))
		dev_warn(dev, "fb notifier not registered\n");

	dev_info(dev, "GT%s ready, irq %d %s\n", ts->product_id, client->irq,
		 ts->int_type ? "falling" : "rising");
	return 0;
}

static int gt_remove(struct i2c_client *client)
{
	struct gt1151 *ts = i2c_get_clientdata(client);

	fb_unregister_client(&ts->fb_notifier);
	if (ts->wake_enabled)
		disable_irq_wake(client->irq);
	gt_esd_switch(ts, false);
	cancel_delayed_work_sync(&ts->esd_work);
	cancel_work_sync(&ts->resume_work);
	return 0;
}

static const struct of_device_id gt_of_match[] = {
	{ .compatible = "mediatek,cap_touch" },
	{ }
};
MODULE_DEVICE_TABLE(of, gt_of_match);

static const struct i2c_device_id gt_id[] = {
	{ "gt1151qm", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gt_id);

static struct i2c_driver gt_driver = {
	.driver = {
		.name = "gt1151qm",
		.of_match_table = gt_of_match,
	},
	.probe_new = gt_probe,
	.remove = gt_remove,
	.id_table = gt_id,
};
module_i2c_driver(gt_driver);

MODULE_DESCRIPTION("Titan Pocket GT1151QM touchscreen (mtk-tpd)");
MODULE_LICENSE("GPL");
