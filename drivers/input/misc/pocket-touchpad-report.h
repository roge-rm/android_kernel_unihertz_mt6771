/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RMI4 F11 and F12 finger data to MT protocol A events, in the order and
 * form the stock mtk-pad driver sends them. Kept free of kernel calls so
 * tools/check-pocket-touchpad-report.py can build it on the host. The
 * includer provides u8, u16, bool and the input event codes.
 */
#ifndef _POCKET_TOUCHPAD_REPORT_H
#define _POCKET_TOUCHPAD_REPORT_H

/* F12 object data with report enable 0xdf: type, x lo, x hi, y lo, y hi, wx, wy. */
#define POCKET_PAD_F12_OBJECT_BYTES	7
/* F11 per-finger absolute data: x hi, y hi, y lo << 4 | x lo, wy << 4 | wx, z. */
#define POCKET_PAD_F11_FINGER_BYTES	5

struct pocket_pad_sink {
	void (*event)(void *ctx, unsigned int type, unsigned int code, int value);
	void *ctx;
};

static inline void pocket_pad_emit(const struct pocket_pad_sink *s, unsigned int type,
				   unsigned int code, int value)
{
	s->event(s->ctx, type, code, value);
}

/* No finger to look at: what stock sends when an F12 frame has nothing to process. */
static inline void pocket_pad_report_none(const struct pocket_pad_sink *s)
{
	pocket_pad_emit(s, EV_KEY, BTN_TOUCH, 0);
	pocket_pad_emit(s, EV_KEY, BTN_TOOL_FINGER, 0);
	pocket_pad_emit(s, EV_SYN, SYN_MT_REPORT, 0);
	pocket_pad_emit(s, EV_SYN, SYN_REPORT, 0);
}

/*
 * One F12 frame of @count objects. Updates *@present to one past the last
 * object seen down, which stock keeps for later frames. Returns the number
 * of objects reported down.
 */
static inline int pocket_pad_report_f12(const struct pocket_pad_sink *s, const u8 *data,
					unsigned int count, u8 *present)
{
	unsigned int i;
	int down = 0;

	for (i = 0; i < count; i++) {
		const u8 *o = data + i * POCKET_PAD_F12_OBJECT_BYTES;
		u16 x = o[1] | (o[2] << 8);
		u16 y = o[3] | (o[4] << 8);
		u8 wx = o[5], wy = o[6];

		if (!(o[0] & 1))
			continue;
		*present = i + 1;
		pocket_pad_emit(s, EV_ABS, ABS_MT_TRACKING_ID, i);
		pocket_pad_emit(s, EV_KEY, BTN_TOUCH, 1);
		pocket_pad_emit(s, EV_KEY, BTN_TOOL_FINGER, 1);
		pocket_pad_emit(s, EV_ABS, ABS_MT_POSITION_X, x);
		pocket_pad_emit(s, EV_ABS, ABS_MT_POSITION_Y, y);
		pocket_pad_emit(s, EV_ABS, ABS_MT_TOUCH_MAJOR, wx > wy ? wx : wy);
		pocket_pad_emit(s, EV_ABS, ABS_MT_TOUCH_MINOR, wx < wy ? wx : wy);
		pocket_pad_emit(s, EV_SYN, SYN_MT_REPORT, 0);
		down++;
	}
	if (down) {
		pocket_pad_emit(s, EV_SYN, SYN_REPORT, 0);
		return down;
	}
	pocket_pad_emit(s, EV_KEY, BTN_TOUCH, 0);
	pocket_pad_emit(s, EV_KEY, BTN_TOOL_FINGER, 0);
	pocket_pad_emit(s, EV_ABS, ABS_MT_TRACKING_ID, -1);
	pocket_pad_emit(s, EV_SYN, SYN_MT_REPORT, 0);
	pocket_pad_emit(s, EV_SYN, SYN_REPORT, 0);
	return 0;
}

/* F11 finger state for finger @i: 2 bits per finger, 4 fingers per byte. */
static inline unsigned int pocket_pad_f11_state(const u8 *status, unsigned int i)
{
	return (status[i / 4] >> ((i % 4) * 2)) & 3;
}

/* One F11 finger that is down. Stock sends no tracking ID or BTN_TOUCH here. */
static inline void pocket_pad_report_f11_finger(const struct pocket_pad_sink *s, const u8 *f)
{
	u16 x = (f[0] << 4) | (f[2] & 0xf);
	u16 y = (f[1] << 4) | (f[2] >> 4);
	u8 wx = f[3] & 0xf, wy = f[3] >> 4;

	pocket_pad_emit(s, EV_KEY, BTN_TOOL_FINGER, 1);
	pocket_pad_emit(s, EV_ABS, ABS_MT_POSITION_X, x);
	pocket_pad_emit(s, EV_ABS, ABS_MT_POSITION_Y, y);
	pocket_pad_emit(s, EV_ABS, ABS_MT_TOUCH_MAJOR, wx > wy ? wx : wy);
	pocket_pad_emit(s, EV_ABS, ABS_MT_TOUCH_MINOR, wx < wy ? wx : wy);
	pocket_pad_emit(s, EV_SYN, SYN_MT_REPORT, 0);
}

/* Close an F11 frame after @down fingers were sent. */
static inline void pocket_pad_report_f11_end(const struct pocket_pad_sink *s, int down)
{
	if (!down) {
		pocket_pad_emit(s, EV_KEY, BTN_TOUCH, 0);
		pocket_pad_emit(s, EV_KEY, BTN_TOOL_FINGER, 0);
		pocket_pad_emit(s, EV_SYN, SYN_MT_REPORT, 0);
	}
	pocket_pad_emit(s, EV_SYN, SYN_REPORT, 0);
}

#endif
