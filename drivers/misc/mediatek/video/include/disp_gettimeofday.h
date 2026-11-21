/* SPDX-License-Identifier: GPL-2.0 */
/*
 * do_gettimeofday for the 4.14 display sources, which 5.10 no longer has.
 * Wall-clock time in a struct __kernel_old_timeval, the layout of 4.14's
 * struct timeval. Only used for log timestamps.
 */
#ifndef __DISP_GETTIMEOFDAY_H__
#define __DISP_GETTIMEOFDAY_H__

#include <linux/time64.h>
#include <linux/timekeeping.h>

static inline void disp_do_gettimeofday(struct __kernel_old_timeval *tv)
{
	struct timespec64 now;

	ktime_get_real_ts64(&now);
	tv->tv_sec = now.tv_sec;
	tv->tv_usec = now.tv_nsec / NSEC_PER_USEC;
}

#endif
