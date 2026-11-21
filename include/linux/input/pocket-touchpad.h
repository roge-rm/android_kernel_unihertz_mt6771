/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hooks between the Titan Pocket keyboard (aw9523-key) and its touchpad
 * (pocket-touchpad, input device mtk-pad). The names are the stock ones.
 */
#ifndef _LINUX_INPUT_POCKET_TOUCHPAD_H
#define _LINUX_INPUT_POCKET_TOUCHPAD_H

#include <linux/types.h>

#ifdef CONFIG_INPUT_POCKET_TOUCHPAD
/* 1 once the pad has probed, 0 before. Stock checks it before each call below. */
extern int touchpad_flag;
/*
 * true puts the pad to sleep and drops any touch, false wakes and reinits it.
 * Sleeps (I2C), so call it from process context. Does nothing before probe.
 */
void agold_touchpad_resume_suspend(bool suspend);
/*
 * Stock's pad probe ends by calling the keyboard's agold_key_resume(). The
 * keyboard hands that function over here from its probe; the pad calls it
 * once at the end of its own probe if it's set by then.
 */
void pocket_touchpad_set_key_resume(void (*resume)(void));
#else
#define touchpad_flag 0
static inline void agold_touchpad_resume_suspend(bool suspend) {}
static inline void pocket_touchpad_set_key_resume(void (*resume)(void)) {}
#endif

#endif
