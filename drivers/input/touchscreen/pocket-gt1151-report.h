/* SPDX-License-Identifier: GPL-2.0 */
/*
 * GT1151QM coordinate packet to MT protocol A events, in the order and form
 * the stock mtk-tpd driver (gt1151qm_touch_event_handler) sends them. Kept
 * free of kernel calls so tools/check-pocket-touchscreen-report.py can build
 * it on the host. The includer provides u8, u16 and the input event codes.
 */
#ifndef _POCKET_GT1151_REPORT_H
#define _POCKET_GT1151_REPORT_H

#define POCKET_GT_MAX_TOUCH	10
/* Status byte, 8 bytes per finger, key byte and checksum. */
#define POCKET_GT_PACKET_LEN(n)	(1 + 8 * (n) + 2)
#define POCKET_GT_PACKET_MAX	POCKET_GT_PACKET_LEN(POCKET_GT_MAX_TOUCH)
/* The first read is this long; more fingers need a second read. */
#define POCKET_GT_FIRST_READ	11

/* pre_event bits, as stock */
#define POCKET_GT_EV_TOUCH	0x01
#define POCKET_GT_EV_KEY	0x02
#define POCKET_GT_EV_STYLUS_KEY	0x08

/* Chip keys are sent as touches at these points, as stock does. */
#define POCKET_GT_KEY_Y		765

struct pocket_gt_sink {
	void (*event)(void *ctx, unsigned int type, unsigned int code, int value);
	void *ctx;
};

struct pocket_gt_state {
	u16 pre_event;
	u16 pre_index;
};

static inline void pocket_gt_emit(const struct pocket_gt_sink *s, unsigned int type,
				  unsigned int code, int value)
{
	s->event(s->ctx, type, code, value);
}

/* Bytes of a packet must sum to 0. */
static inline u8 pocket_gt_checksum(const u8 *data, unsigned int len)
{
	u8 sum = 0;

	while (len--)
		sum += *data++;
	return sum;
}

/*
 * Stock gt1151qm_touch_down. Size 0 with id 0 is taken as a key: TOUCH_MAJOR
 * 100 and no TRACKING_ID. Stock also sends ABS_MT_PRESSURE, which the input
 * core drops since mtk-tpd doesn't set that bit.
 */
static inline void pocket_gt_touch_down(const struct pocket_gt_sink *s, int x, int y,
					int size, int id)
{
	pocket_gt_emit(s, EV_KEY, BTN_TOUCH, 1);
	if (!size && !id) {
		pocket_gt_emit(s, EV_ABS, ABS_MT_TOUCH_MAJOR, 100);
	} else {
		pocket_gt_emit(s, EV_ABS, ABS_MT_TOUCH_MAJOR, size);
		pocket_gt_emit(s, EV_ABS, ABS_MT_TRACKING_ID, id);
	}
	pocket_gt_emit(s, EV_ABS, ABS_MT_POSITION_X, x);
	pocket_gt_emit(s, EV_ABS, ABS_MT_POSITION_Y, y);
	pocket_gt_emit(s, EV_SYN, SYN_MT_REPORT, 0);
}

/* Stock gt1151qm_touch_up */
static inline void pocket_gt_touch_up(const struct pocket_gt_sink *s)
{
	pocket_gt_emit(s, EV_KEY, BTN_TOUCH, 0);
	pocket_gt_emit(s, EV_SYN, SYN_MT_REPORT, 0);
}

/* Key bit to the point stock reports it at */
static inline int pocket_gt_key_x(u8 key)
{
	if (key & 0x01)
		return 60;
	if (key & 0x02)
		return 300;
	if (key & 0x04)
		return 180;
	return 420;
}

/*
 * One checked packet of @touch_num fingers (at most POCKET_GT_MAX_TOUCH).
 * Coordinates are sent raw, with no swap or warp, as stock does.
 */
static inline void pocket_gt_report(const struct pocket_gt_sink *s, const u8 *data,
				    unsigned int touch_num, struct pocket_gt_state *st)
{
	u8 key = data[1 + 8 * touch_num];
	unsigned int key_event = 0, touch = touch_num ? POCKET_GT_EV_TOUCH : 0;
	unsigned int i, reported = 0, next_id;
	const u8 *coor;

	if ((data[0] & 0x10) && key) {
		if (key & 0xf0)
			key_event = POCKET_GT_EV_STYLUS_KEY;
		else if ((key & 0x0f) && !touch_num)
			key_event = POCKET_GT_EV_KEY;
	}

	if (key_event & POCKET_GT_EV_KEY)
		pocket_gt_touch_down(s, pocket_gt_key_x(key), POCKET_GT_KEY_Y, 0, 0);
	else if (st->pre_event & POCKET_GT_EV_KEY)
		pocket_gt_touch_up(s);

	if (touch) {
		coor = data + 1;
		next_id = coor[0] & 0x0f;
		for (i = 0; i < POCKET_GT_MAX_TOUCH; i++) {
			if (i == next_id) {
				if (reported++ < touch_num) {
					pocket_gt_touch_down(s, coor[1] | (coor[2] << 8),
							     coor[3] | (coor[4] << 8),
							     coor[5] | (coor[6] << 8), i);
					coor += 8;
					next_id = coor[0] & 0x0f;
				}
				st->pre_index |= 1 << i;
			} else {
				st->pre_index &= ~(1 << i);
			}
		}
	} else if (st->pre_event & POCKET_GT_EV_TOUCH) {
		pocket_gt_touch_up(s);
		st->pre_index = 0;
	}

	/* Stock also syncs a pen device here for stylus keys; there is none. */
	if ((key_event & (POCKET_GT_EV_TOUCH | POCKET_GT_EV_KEY)) || touch ||
	    (st->pre_event & (POCKET_GT_EV_TOUCH | POCKET_GT_EV_KEY)))
		pocket_gt_emit(s, EV_SYN, SYN_REPORT, 0);

	if (st->pre_event || key_event || touch)
		st->pre_event = key_event | touch;
}

#endif
