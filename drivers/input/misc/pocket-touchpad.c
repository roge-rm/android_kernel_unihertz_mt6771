// SPDX-License-Identifier: GPL-2.0
/*
 * Titan Pocket keyboard touchpad: a Synaptics RMI4 sensor on i2c6.
 *
 * Behaves as the stock 4.14 driver (synaptics_dsx_pad under mtk-pad) does:
 * talks at 0x20 whatever the DT reg says, swapping with 0x2c on the third
 * failed try; pulses GPIO 9 for reset; scans the PDT for F01, F11, F12 and
 * F1A; reports one finger as MT protocol A through an input device named
 * mtk-pad with fixed 720x360 ranges and no INPUT_PROP_POINTER. Cursor and
 * scroll handling are left to userspace. The keyboard puts the pad to
 * sleep while a key is down through agold_touchpad_resume_suspend().
 */

#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/pocket-touchpad.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "pocket-touchpad-report.h"

#define PAD_NAME		"mtk-pad"
#define PAD_ADDR		0x20	/* stock overrides the DT reg (0x21) with this */
#define PAD_ALT_ADDR		0x2c	/* stock swaps to this on the third failed try */
#define PAD_TRIES		5
#define PAD_RETRY_MS		20

/* Stock CONFIG_AGOLD_PAD_RES_X/Y; the sensor's own range isn't used. */
#define PAD_RES_X		720
#define PAD_RES_Y		360
#define PAD_MAX_WIDTH		100
#define PAD_MAX_ID		10

/* Stock reports one object, whatever the sensor supports. */
#define PAD_POINTS		1

#define RMI_PAGE_SELECT		0xff
#define RMI_PDT_START		0xe9
#define RMI_PDT_END		0xd0	/* entries at 0xe9 down to 0xd1 */
#define RMI_PDT_ENTRY		6
#define RMI_PAGES		10
#define RMI_MAX_INT_REGS	4

#define F01_CMD_RESET		0x01
#define F01_STATUS_CODE		0x0f
#define F01_STATUS_CRC_BUSY	0x06
#define F01_STATUS_FLASH_PROG	BIT(6)
#define F01_STATUS_UNCONFIGURED	BIT(7)
#define F01_CTRL_SLEEP_MASK	0x07
#define F01_CTRL_SLEEP		0x01
#define F01_CTRL_NO_SLEEP	BIT(2)
#define F01_CTRL_CONFIGURED	BIT(7)
#define F01_BUILD_ID_OFFSET	18
#define F01_QUERY_BYTES		21

#define F12_REPORT_ENABLE	0xdf	/* everything but Z, as stock writes */

#define F1A_MAX_BUTTONS		3

struct pad_fn {
	bool present;
	u8 number;
	u8 intr_reg;
	u8 intr_mask;
	u16 query;
	u16 cmd;
	u16 ctrl;
	u16 data;
};

struct pocket_pad {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset;
	int irq;

	struct mutex io_lock;		/* page cache and each register access */
	struct mutex state_lock;	/* sleep state, reporting and reinit */
	u16 addr;
	int page;

	struct pad_fn f01, f11, f12, f1a;
	u8 num_int_regs;
	u8 intr_mask[RMI_MAX_INT_REGS];
	u8 no_sleep;

	u8 f11_status_bytes;
	u8 f12_data1_offset;
	bool f12_has_data15;
	u8 f12_data15_offset;
	u8 f12_data15_bytes;
	u16 f12_ctrl28;

	u8 f1a_buttons;
	u8 f1a_data_bytes;
	u8 f1a_state;

	u8 fingers_present;		/* stock keeps this across frames */
	bool touch_stopped;
	bool sensor_sleep;

	bool lcd_off;
	struct workqueue_struct *resume_wq;
	struct work_struct resume_work;
	struct notifier_block fb_notifier;
};

static const unsigned int pad_f1a_keys[F1A_MAX_BUTTONS] = {
	KEY_APPSELECT, KEY_HOMEPAGE, KEY_BACK,	/* stock F1A map */
};

static struct pocket_pad *pocket_pad;
int touchpad_flag;
EXPORT_SYMBOL_GPL(touchpad_flag);

static void (*pad_key_resume)(void);
static DEFINE_MUTEX(pad_key_resume_lock);

void pocket_touchpad_set_key_resume(void (*resume)(void))
{
	mutex_lock(&pad_key_resume_lock);
	pad_key_resume = resume;
	mutex_unlock(&pad_key_resume_lock);
}
EXPORT_SYMBOL_GPL(pocket_touchpad_set_key_resume);

static void pad_swap_addr(struct pocket_pad *pad)
{
	pad->addr = pad->addr != PAD_ADDR ? PAD_ADDR : PAD_ALT_ADDR;
	dev_info(&pad->client->dev, "trying address 0x%02x\n", pad->addr);
}

static int pad_xfer(struct pocket_pad *pad, struct i2c_msg *msgs, int num)
{
	int i, ret;

	for (i = 0; i < num; i++)
		msgs[i].addr = pad->addr;
	ret = i2c_transfer(pad->client->adapter, msgs, num);
	if (ret == num)
		return 0;
	return ret < 0 ? ret : -EIO;
}

static int pad_set_page(struct pocket_pad *pad, u16 reg)
{
	u8 buf[2] = { RMI_PAGE_SELECT, reg >> 8 };
	struct i2c_msg msg = { .len = sizeof(buf), .buf = buf };
	int try, ret = 0;

	if (pad->page == buf[1])
		return 0;
	for (try = 1; try <= PAD_TRIES; try++) {
		ret = pad_xfer(pad, &msg, 1);
		if (!ret) {
			pad->page = buf[1];
			return 0;
		}
		dev_err_ratelimited(&pad->client->dev, "page select retry %d: %d\n", try, ret);
		msleep(PAD_RETRY_MS);
		if (try == 3)
			pad_swap_addr(pad);
	}
	return ret;
}

static int pad_read(struct pocket_pad *pad, u16 reg, u8 *buf, u16 len)
{
	u8 lo = reg & 0xff;
	struct i2c_msg msgs[2] = {
		{ .len = 1, .buf = &lo },
		{ .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int try, ret;

	mutex_lock(&pad->io_lock);
	ret = pad_set_page(pad, reg);
	for (try = 1; !ret && try <= PAD_TRIES; try++) {
		ret = pad_xfer(pad, msgs, 2);
		if (!ret)
			break;
		dev_err_ratelimited(&pad->client->dev, "read 0x%04x retry %d: %d\n", reg, try, ret);
		msleep(PAD_RETRY_MS);
		if (try == 3)
			pad_swap_addr(pad);
		if (try < PAD_TRIES)
			ret = 0;
	}
	mutex_unlock(&pad->io_lock);
	return ret;
}

static int pad_write(struct pocket_pad *pad, u16 reg, const u8 *data, u16 len)
{
	u8 buf[8];
	struct i2c_msg msg = { .len = len + 1, .buf = buf };
	int try, ret;

	if (len + 1 > sizeof(buf))
		return -EINVAL;
	buf[0] = reg & 0xff;
	memcpy(buf + 1, data, len);

	/*
	 * Stock writes go to whatever address the reads last swapped to and
	 * don't swap themselves; only the page select can.
	 */
	mutex_lock(&pad->io_lock);
	ret = pad_set_page(pad, reg);
	for (try = 1; !ret && try <= PAD_TRIES; try++) {
		ret = pad_xfer(pad, &msg, 1);
		if (!ret)
			break;
		dev_err_ratelimited(&pad->client->dev, "write 0x%04x retry %d: %d\n", reg, try, ret);
		msleep(PAD_RETRY_MS);
		if (try < PAD_TRIES)
			ret = 0;
	}
	mutex_unlock(&pad->io_lock);
	return ret;
}

static int pad_write_byte(struct pocket_pad *pad, u16 reg, u8 val)
{
	return pad_write(pad, reg, &val, 1);
}

static void pad_input_event(void *ctx, unsigned int type, unsigned int code, int value)
{
	input_event(ctx, type, code, value);
}

/* Lift everything: the release stock sends on suspend, reset and reinit. */
static void pad_release(struct pocket_pad *pad)
{
	struct pocket_pad_sink sink = { pad_input_event, pad->input };

	pocket_pad_report_none(&sink);
}

static int pad_write_intr_masks(struct pocket_pad *pad, bool clear)
{
	int i, ret;

	for (i = 0; i < pad->num_int_regs; i++) {
		if (!pad->intr_mask[i])
			continue;
		ret = pad_write_byte(pad, pad->f01.ctrl + 1 + i, clear ? 0 : pad->intr_mask[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int pad_ack_irq(struct pocket_pad *pad)
{
	u8 status[RMI_MAX_INT_REGS];

	return pad_read(pad, pad->f01.data + 1, status, pad->num_int_regs);
}

static void pad_set_configured(struct pocket_pad *pad)
{
	u8 ctrl;

	if (pad_read(pad, pad->f01.ctrl, &ctrl, 1)) {
		dev_err(&pad->client->dev, "set configured failed\n");
		return;
	}
	pad->no_sleep = ctrl & F01_CTRL_NO_SLEEP;
	if (pad_write_byte(pad, pad->f01.ctrl, ctrl | F01_CTRL_CONFIGURED))
		dev_err(&pad->client->dev, "set configured failed\n");
}

static void pad_sleep_enable(struct pocket_pad *pad, bool sleep)
{
	u8 ctrl;

	if (pad_read(pad, pad->f01.ctrl, &ctrl, 1)) {
		dev_err(&pad->client->dev, "device control read failed\n");
		return;
	}
	ctrl = (ctrl & ~F01_CTRL_SLEEP_MASK) | (sleep ? F01_CTRL_SLEEP : pad->no_sleep);
	if (pad_write_byte(pad, pad->f01.ctrl, ctrl)) {
		dev_err(&pad->client->dev, "device control write failed\n");
		return;
	}
	pad->sensor_sleep = sleep;
}

/* After a sensor reset or wake: report enable, interrupt masks, configured bit. */
static int pad_reinit(struct pocket_pad *pad)
{
	int ret;

	/* The sensor may have reset, which puts its page back to 0. */
	mutex_lock(&pad->io_lock);
	pad->page = -1;
	mutex_unlock(&pad->io_lock);
	pad_release(pad);
	if (pad->f12.present && pad->f12_ctrl28)
		pad_write_byte(pad, pad->f12_ctrl28, F12_REPORT_ENABLE);
	ret = pad_write_intr_masks(pad, false);
	if (ret)
		return ret;
	pad_set_configured(pad);
	return 0;
}

static void pad_report_f12(struct pocket_pad *pad)
{
	struct pocket_pad_sink sink = { pad_input_event, pad->input };
	u8 data[PAD_POINTS * POCKET_PAD_F12_OBJECT_BYTES];
	unsigned int count = PAD_POINTS;

	if (pad->f12_has_data15) {
		u8 bits[(PAD_POINTS + 7) / 8];
		int i;

		if (pad_read(pad, pad->f12.data + pad->f12_data15_offset, bits, sizeof(bits)))
			return;
		count = 0;
		for (i = PAD_POINTS - 1; i >= 0; i--) {
			if (bits[i / 8] & BIT(i % 8)) {
				count = i + 1;
				break;
			}
		}
	}
	count = max_t(unsigned int, count, pad->fingers_present);
	if (!count) {
		pocket_pad_report_none(&sink);
		return;
	}
	if (pad_read(pad, pad->f12.data + pad->f12_data1_offset, data,
		     count * POCKET_PAD_F12_OBJECT_BYTES))
		return;
	pocket_pad_report_f12(&sink, data, count, &pad->fingers_present);
}

static void pad_report_f11(struct pocket_pad *pad)
{
	struct pocket_pad_sink sink = { pad_input_event, pad->input };
	u8 status[(PAD_POINTS + 3) / 4];
	u8 finger[POCKET_PAD_F11_FINGER_BYTES];
	int i, down = 0;

	if (pad_read(pad, pad->f11.data, status, pad->f11_status_bytes))
		return;
	for (i = 0; i < PAD_POINTS; i++) {
		if (!pocket_pad_f11_state(status, i))
			continue;
		if (pad_read(pad, pad->f11.data + pad->f11_status_bytes +
			     i * POCKET_PAD_F11_FINGER_BYTES, finger, sizeof(finger)))
			return;
		pocket_pad_report_f11_finger(&sink, finger);
		down++;
	}
	pocket_pad_report_f11_end(&sink, down);
}

static void pad_report_f1a(struct pocket_pad *pad)
{
	u8 data[1];
	bool changed = false;
	int i;

	if (pad_read(pad, pad->f1a.data, data, pad->f1a_data_bytes))
		return;
	for (i = 0; i < pad->f1a_buttons; i++) {
		u8 bit = BIT(i);

		if ((data[0] ^ pad->f1a_state) & bit) {
			input_report_key(pad->input, pad_f1a_keys[i], data[0] & bit);
			changed = true;
		}
	}
	pad->f1a_state = data[0];
	if (changed)
		input_sync(pad->input);
}

static bool pad_fn_fired(struct pocket_pad *pad, struct pad_fn *fn, const u8 *irq_status)
{
	return fn->present && fn->intr_mask && (irq_status[fn->intr_reg] & fn->intr_mask);
}

/* Level-low and oneshot, so the line stays masked until this returns. */
static irqreturn_t pad_irq(int irq, void *data)
{
	struct pocket_pad *pad = data;
	u8 status[1 + RMI_MAX_INT_REGS];

	mutex_lock(&pad->state_lock);
	/* Read even while stopped so a pending source can't hold the line low. */
	if (pad_read(pad, pad->f01.data, status, 1 + pad->num_int_regs)) {
		dev_err_ratelimited(&pad->client->dev, "interrupt status read failed\n");
		goto out;
	}
	if (pad->touch_stopped)
		goto out;
	if ((status[0] & (F01_STATUS_UNCONFIGURED | F01_STATUS_FLASH_PROG)) ==
	    F01_STATUS_UNCONFIGURED) {
		dev_info_ratelimited(&pad->client->dev, "sensor reset itself, reinit\n");
		if (pad_reinit(pad))
			dev_err_ratelimited(&pad->client->dev, "reinit failed\n");
		goto out;
	}
	if (pad_fn_fired(pad, &pad->f12, status + 1))
		pad_report_f12(pad);
	if (pad_fn_fired(pad, &pad->f11, status + 1))
		pad_report_f11(pad);
	if (pad_fn_fired(pad, &pad->f1a, status + 1))
		pad_report_f1a(pad);
out:
	mutex_unlock(&pad->state_lock);
	return IRQ_HANDLED;
}

/* Stock synaptics_rmi4_suspend with no wake gesture and no firmware update. */
static void pad_suspend_locked(struct pocket_pad *pad)
{
	if (!pad->sensor_sleep) {
		pad->touch_stopped = true;
		pad_write_intr_masks(pad, true);
		pad_sleep_enable(pad, true);
		pad_release(pad);
	}
	pad->sensor_sleep = true;
}

/* Stock synaptics_rmi4_resume: wakes and reinits even if it wasn't asleep. */
static void pad_resume_locked(struct pocket_pad *pad)
{
	pad_sleep_enable(pad, false);
	pad_ack_irq(pad);
	pad_write_intr_masks(pad, false);
	/* As stock: on failure the pad stays stopped until the next resume. */
	if (pad_reinit(pad)) {
		dev_err_ratelimited(&pad->client->dev, "resume reinit failed, pad stays stopped\n");
		return;
	}
	pad->sensor_sleep = false;
	pad->touch_stopped = false;
}

void agold_touchpad_resume_suspend(bool suspend)
{
	struct pocket_pad *pad = READ_ONCE(pocket_pad);

	if (!pad)
		return;
	mutex_lock(&pad->state_lock);
	if (suspend)
		pad_suspend_locked(pad);
	else
		pad_resume_locked(pad);
	mutex_unlock(&pad->state_lock);
}
EXPORT_SYMBOL_GPL(agold_touchpad_resume_suspend);

static void pad_resume_work(struct work_struct *work)
{
	struct pocket_pad *pad = container_of(work, struct pocket_pad, resume_work);

	agold_touchpad_resume_suspend(false);
	pad->lcd_off = false;
}

/* Stock pad_fb_notifier_callback: sleep on powerdown, wake on unblank after one. */
static int pad_fb_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
	struct pocket_pad *pad = container_of(nb, struct pocket_pad, fb_notifier);
	struct fb_event *evdata = data;
	int blank;

	if (event != FB_EVENT_BLANK || !evdata || !evdata->data)
		return 0;
	blank = *(int *)evdata->data;
	if (blank == FB_BLANK_POWERDOWN) {
		if (!pad->lcd_off) {
			cancel_work_sync(&pad->resume_work);
			agold_touchpad_resume_suspend(true);
		}
		pad->lcd_off = true;
	} else if (blank == FB_BLANK_UNBLANK && pad->lcd_off) {
		queue_work(pad->resume_wq, &pad->resume_work);
	}
	return 0;
}

static void pad_irq_bits(struct pad_fn *fn, unsigned int first, unsigned int count)
{
	unsigned int i;

	fn->intr_reg = first / 8;
	fn->intr_mask = 0;
	for (i = first % 8; i < first % 8 + count && i < 8; i++)
		fn->intr_mask |= BIT(i);
}

/* F01 in the PDT scan: stock resets the sensor here and waits out the CRC check. */
static int pad_f01_init(struct pocket_pad *pad, bool *rescan, bool *flash_prog)
{
	unsigned int waited = 0;
	u8 status;
	int ret;

	ret = pad_write_byte(pad, pad->f01.cmd, F01_CMD_RESET);
	if (ret)
		return ret;
	msleep(200);
	pad->page = -1;
	ret = pad_read(pad, pad->f01.data, &status, 1);
	while (!ret && (status & F01_STATUS_CODE) == F01_STATUS_CRC_BUSY) {
		if (waited >= 100)
			return -ETIMEDOUT;
		msleep(20);
		waited += 20;
		ret = pad_read(pad, pad->f01.data, &status, 1);
	}
	if (ret)
		return ret;
	*rescan = waited;
	*flash_prog = status & F01_STATUS_FLASH_PROG;
	/* num_int_regs isn't known yet; stock reads one status byte here. */
	return pad_read(pad, pad->f01.data + 1, &status, 1);
}

static int pad_f11_init(struct pocket_pad *pad)
{
	u8 ctrl[4];

	pad->f11_status_bytes = (PAD_POINTS + 3) / 4;
	if (!pad_read(pad, pad->f11.ctrl + 6, ctrl, sizeof(ctrl)))
		dev_info(&pad->client->dev, "F11 sensor max x %u y %u, reporting 0-%u 0-%u\n",
			 (ctrl[0] | ctrl[1] << 8) & 0xfff, (ctrl[2] | ctrl[3] << 8) & 0xfff,
			 PAD_RES_X, PAD_RES_Y);
	return 0;
}

static int pad_f12_init(struct pocket_pad *pad)
{
	struct device *dev = &pad->client->dev;
	u8 q5[5] = {}, q8[3] = {}, size, ctrl8[14];
	unsigned int ctrl8_off, ctrl23_off, ctrl28_off;
	int ret;

	/* A size of 0 means the register isn't there; a zero-length read isn't allowed. */
	ret = pad_read(pad, pad->f12.query + 4, &size, 1);
	if (!ret && size)
		ret = pad_read(pad, pad->f12.query + 5, q5, min_t(u8, size, sizeof(q5)));
	if (ret)
		return ret;
	/* query 5: byte 0 is the size of query 6, then one presence bit per control register */
	ctrl8_off = hweight8(q5[1]);
	ctrl23_off = hweight8(q5[1]) + hweight8(q5[2]) + hweight8(q5[3] & 0x7f);
	ctrl28_off = ctrl23_off + !!(q5[3] & 0x80) + hweight8(q5[4] & 0x0f);

	ret = pad_read(pad, pad->f12.query + 7, &size, 1);
	if (!ret && size)
		ret = pad_read(pad, pad->f12.query + 8, q8, min_t(u8, size, sizeof(q8)));
	if (ret)
		return ret;
	/* query 8: byte 0 is the size of query 9, then one presence bit per data register */
	pad->f12_data1_offset = q8[1] & 1;
	pad->f12_has_data15 = size >= 3 && (q8[2] & 0x80);
	pad->f12_data15_offset = hweight8(q8[1]) + hweight8(q8[2] & 0x7f);
	pad->f12_data15_bytes = (PAD_POINTS + 7) / 8;

	pad->f12_ctrl28 = pad->f12.ctrl + ctrl28_off;
	ret = pad_write_byte(pad, pad->f12_ctrl28, F12_REPORT_ENABLE);
	if (ret)
		return ret;
	if (!pad_read(pad, pad->f12.ctrl + ctrl8_off, ctrl8, sizeof(ctrl8)))
		dev_info(dev, "F12 sensor max x %u y %u, rx %u tx %u, reporting 0-%u 0-%u\n",
			 ctrl8[0] | ctrl8[1] << 8, ctrl8[2] | ctrl8[3] << 8, ctrl8[12], ctrl8[13],
			 PAD_RES_X, PAD_RES_Y);
	dev_info(dev, "F12 data1 at +%u, data15 %s at +%u, ctrl28 0x%04x\n",
		 pad->f12_data1_offset, pad->f12_has_data15 ? "present" : "absent",
		 pad->f12_data15_offset, pad->f12_ctrl28);
	return 0;
}

static int pad_f1a_init(struct pocket_pad *pad)
{
	u8 query[2];
	unsigned int count, i;
	int ret;

	ret = pad_read(pad, pad->f1a.query, query, sizeof(query));
	if (ret)
		return ret;
	count = (query[0] & 7) + 1;
	pad->f1a_buttons = min_t(unsigned int, count, F1A_MAX_BUTTONS);
	pad->f1a_data_bytes = 1;	/* 8 buttons at most with a 3-bit count */
	for (i = 0; i < pad->f1a_buttons; i++)
		input_set_capability(pad->input, EV_KEY, pad_f1a_keys[i]);
	dev_info(&pad->client->dev, "F1A with %u buttons\n", count);
	return 0;
}

static int pad_query_device(struct pocket_pad *pad)
{
	struct device *dev = &pad->client->dev;
	struct pad_fn *fns[] = { &pad->f01, &pad->f11, &pad->f12, &pad->f1a };
	u8 entry[RMI_PDT_ENTRY], query[F01_QUERY_BYTES], build[3];
	unsigned int intr_count, page, rescans = 0;
	bool rescan, flash_prog = false;
	int addr, ret, i;

restart:
	memset(&pad->f01, 0, sizeof(pad->f01));
	memset(&pad->f11, 0, sizeof(pad->f11));
	memset(&pad->f12, 0, sizeof(pad->f12));
	memset(&pad->f1a, 0, sizeof(pad->f1a));
	intr_count = 0;
	rescan = false;

	for (page = 0; page < RMI_PAGES && !flash_prog; page++) {
		for (addr = RMI_PDT_START; addr > RMI_PDT_END; addr -= RMI_PDT_ENTRY) {
			u16 base = page << 8;
			struct pad_fn *fn = NULL;
			u8 irqs;

			ret = pad_read(pad, base | addr, entry, sizeof(entry));
			if (ret)
				return ret;
			if (!entry[5] || entry[5] == 0xff)
				break;
			irqs = entry[4] & 7;
			dev_info(dev, "F%02x on page %u, %u interrupts\n", entry[5], page, irqs);
			switch (entry[5]) {
			case 0x01:
				fn = &pad->f01;
				break;
			case 0x11:
				fn = &pad->f11;
				break;
			case 0x12:
				fn = &pad->f12;
				break;
			case 0x1a:
				fn = &pad->f1a;
				break;
			}
			if (fn && irqs) {
				fn->present = true;
				fn->number = entry[5];
				fn->query = base | entry[0];
				fn->cmd = base | entry[1];
				fn->ctrl = base | entry[2];
				fn->data = base | entry[3];
				pad_irq_bits(fn, intr_count, irqs);
				if (fn == &pad->f01)
					ret = pad_f01_init(pad, &rescan, &flash_prog);
				else if (fn == &pad->f11)
					ret = pad_f11_init(pad);
				else if (fn == &pad->f12)
					ret = pad_f12_init(pad);
				else
					ret = pad_f1a_init(pad);
				if (ret)
					return ret;
				if (rescan) {
					if (++rescans > 3)
						return -EIO;
					goto restart;
				}
				if (flash_prog) {
					/* Stock stops the scan here and leaves F01 off the interrupt list. */
					dev_warn(dev, "sensor in flash programming mode\n");
					fn->intr_mask = 0;
					break;
				}
			}
			intr_count += irqs;
		}
	}

	if (!pad->f01.present)
		return -ENODEV;
	pad->num_int_regs = DIV_ROUND_UP(intr_count, 8);
	if (pad->num_int_regs > RMI_MAX_INT_REGS)
		return -EINVAL;

	ret = pad_read(pad, pad->f01.query, query, sizeof(query));
	if (!ret)
		ret = pad_read(pad, pad->f01.query + F01_BUILD_ID_OFFSET, build, sizeof(build));
	if (ret)
		return ret;
	if (query[0] != 1)
		dev_err(dev, "not a Synaptics device, manufacturer %u\n", query[0]);
	dev_info(dev, "product %.10s, build %u, %u interrupt registers\n", query + 11,
		 build[0] | build[1] << 8 | build[2] << 16, pad->num_int_regs);

	memset(pad->intr_mask, 0, sizeof(pad->intr_mask));
	for (i = 0; i < ARRAY_SIZE(fns); i++)
		if (fns[i]->present)
			pad->intr_mask[fns[i]->intr_reg] |= fns[i]->intr_mask;
	ret = pad_write_intr_masks(pad, false);
	if (ret)
		return ret;
	pad_set_configured(pad);
	return 0;
}

static void pad_reset_pulse(struct pocket_pad *pad)
{
	/* Stock values; the bootloader leaves GPIO 9 low, holding the pad in reset. */
	gpiod_set_value_cansleep(pad->reset, 1);
	msleep(200);
	gpiod_set_value_cansleep(pad->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(pad->reset, 1);
	msleep(200);
}

static int pad_setup_input(struct pocket_pad *pad)
{
	struct input_dev *input = pad->input;

	input->name = PAD_NAME;
	/* No INPUT_PROP_POINTER: Android 16 takes that as a laptop touchpad. */
	input_set_capability(input, EV_KEY, BTN_TOOL_FINGER);
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_X, 0, PAD_RES_X, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, PAD_RES_Y, 0, 0);
	input_abs_set_res(input, ABS_X, PAD_RES_X);
	input_abs_set_res(input, ABS_Y, PAD_RES_Y);
	input_set_abs_params(input, ABS_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, PAD_MAX_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, PAD_MAX_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, PAD_RES_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, PAD_RES_Y, 0, 0);
	input_set_abs_params(input, ABS_MT_TRACKING_ID, 0, PAD_MAX_ID, 0, 0);
	return input_register_device(input);
}

static int pad_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pocket_pad *pad;
	int ret;

	if (pocket_pad)
		return -EBUSY;
	pad = devm_kzalloc(dev, sizeof(*pad), GFP_KERNEL);
	if (!pad)
		return -ENOMEM;
	pad->client = client;
	pad->addr = PAD_ADDR;
	pad->page = -1;
	mutex_init(&pad->io_lock);
	mutex_init(&pad->state_lock);
	INIT_WORK(&pad->resume_work, pad_resume_work);
	i2c_set_clientdata(client, pad);

	pad->irq = client->irq;
	if (pad->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");
	pad->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pad->reset))
		return dev_err_probe(dev, PTR_ERR(pad->reset), "reset gpio\n");
	pad->input = devm_input_allocate_device(dev);
	if (!pad->input)
		return -ENOMEM;

	pad_reset_pulse(pad);
	ret = pad_query_device(pad);
	if (ret)
		return dev_err_probe(dev, ret, "query failed at 0x%02x\n", pad->addr);
	if (!pad->f11.present && !pad->f12.present)
		dev_warn(dev, "no F11 or F12, the pad won't report touches\n");

	ret = pad_setup_input(pad);
	if (ret)
		return ret;

	pad->resume_wq = create_singlethread_workqueue("touchpad_resume");
	if (!pad->resume_wq)
		return -ENOMEM;

	ret = devm_request_threaded_irq(dev, pad->irq, NULL, pad_irq,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT, "TOUCH_PANEL-eint", pad);
	if (ret) {
		destroy_workqueue(pad->resume_wq);
		return dev_err_probe(dev, ret, "irq %d\n", pad->irq);
	}
	mutex_lock(&pad->state_lock);
	pad_ack_irq(pad);
	mutex_unlock(&pad->state_lock);

	pad->fb_notifier.notifier_call = pad_fb_notifier;
	if (fb_register_client(&pad->fb_notifier))
		dev_warn(dev, "fb notifier not registered\n");

	WRITE_ONCE(pocket_pad, pad);
	touchpad_flag = 1;
	dev_info(dev, "mtk-pad ready at 0x%02x, irq %d\n", pad->addr, pad->irq);

	/* Stock ends its probe this way, and that's where it first resumes the keyboard. */
	agold_touchpad_resume_suspend(true);
	msleep(200);
	mutex_lock(&pad_key_resume_lock);
	if (pad_key_resume)
		pad_key_resume();
	mutex_unlock(&pad_key_resume_lock);
	msleep(200);
	agold_touchpad_resume_suspend(false);
	return 0;
}

static int pad_remove(struct i2c_client *client)
{
	struct pocket_pad *pad = i2c_get_clientdata(client);

	touchpad_flag = 0;
	WRITE_ONCE(pocket_pad, NULL);
	fb_unregister_client(&pad->fb_notifier);
	disable_irq(pad->irq);
	cancel_work_sync(&pad->resume_work);
	destroy_workqueue(pad->resume_wq);
	return 0;
}

static const struct of_device_id pad_of_match[] = {
	{ .compatible = "mediatek,cap_tkpd" },
	{ }
};
MODULE_DEVICE_TABLE(of, pad_of_match);

static const struct i2c_device_id pad_id[] = {
	{ "synaptics_dsx_pad", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pad_id);

static struct i2c_driver pad_driver = {
	.driver = {
		.name = "synaptics_dsx_pad",
		.of_match_table = pad_of_match,
	},
	.probe_new = pad_probe,
	.remove = pad_remove,
	.id_table = pad_id,
};
module_i2c_driver(pad_driver);

MODULE_DESCRIPTION("Titan Pocket keyboard touchpad (mtk-pad)");
MODULE_LICENSE("GPL");
