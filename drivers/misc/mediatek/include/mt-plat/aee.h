/* SPDX-License-Identifier: GPL-2.0 */
/* Stub for the 4.14 display port: AEE is left out, every call does nothing. */

#ifndef __AEE_H__
#define __AEE_H__

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>

enum AE_DEFECT_ATTR {
	AE_DEFECT_FATAL,
	AE_DEFECT_EXCEPTION,
	AE_DEFECT_WARNING,
	AE_DEFECT_REMINDING,
	AE_DEFECT_ATTR_END
};

/* db_opt bits, same values as T */
#define DB_OPT_DEFAULT                  (0)
#define DB_OPT_FTRACE                   (1<<0)
#define DB_OPT_PRINTK_TOO_MUCH          (1<<1)
#define DB_OPT_NE_JBT_TRACES            (1<<2)
#define DB_OPT_SWT_JBT_TRACES           (1<<3)
#define DB_OPT_VM_TRACES                (1<<4)
#define DB_OPT_DUMPSYS_ACTIVITY         (1<<5)
#define DB_OPT_DUMPSYS_WINDOW           (1<<6)
#define DB_OPT_DUMPSYS_GFXINFO          (1<<7)
#define DB_OPT_DUMPSYS_SURFACEFLINGER   (1<<8)
#define DB_OPT_DISPLAY_HANG_DUMP        (1<<9)
#define DB_OPT_LOW_MEMORY_KILLER        (1<<10)
#define DB_OPT_PROC_MEM                 (1<<11)
#define DB_OPT_FS_IO_LOG                (1<<12)
#define DB_OPT_PROCESS_COREDUMP         (1<<13)
#define DB_OPT_VM_HPROF                 (1<<14)
#define DB_OPT_PROCMEM                  (1<<15)
#define DB_OPT_DUMPSYS_INPUT            (1<<16)
#define DB_OPT_MMPROFILE_BUFFER         (1<<17)
#define DB_OPT_BINDER_INFO              (1<<18)
#define DB_OPT_WCN_ISSUE_INFO           (1<<19)
#define DB_OPT_DUMMY_DUMP               (1<<20)
#define DB_OPT_PID_MEMORY_INFO          (1<<21)
#define DB_OPT_VM_OOME_HPROF            (1<<22)
#define DB_OPT_PID_SMAPS                (1<<23)
#define DB_OPT_PROC_CMDQ_INFO           (1<<24)
#define DB_OPT_PROC_USKTRK              (1<<25)
#define DB_OPT_SF_RTT_DUMP              (1<<26)
#define DB_OPT_PAGETYPE_INFO            (1<<27)
#define DB_OPT_DUMPSYS_PROCSTATS        (1<<28)
#define DB_OPT_DUMP_DISPLAY             (1<<29)
#define DB_OPT_NATIVE_BACKTRACE         (1<<30)
#define DB_OPT_AARCH64                  (1U<<31)

/* powerkey monitor bits */
#define WDT_SETBY_DEFAULT               (0)
#define WDT_SETBY_Backlight             (1<<0)
#define WDT_SETBY_Display               (1<<1)
#define WDT_SETBY_SF                    (1<<2)
#define WDT_SETBY_PM                    (1<<3)

static inline void aee_kernel_exception_api_func(const char *file,
		const int line, const int db_opt, const char *module,
		const char *msg, ...)
{
}

static inline void aee_kernel_warning_api_func(const char *file,
		const int line, const int db_opt, const char *module,
		const char *msg, ...)
{
}

static inline void aee_kernel_reminding_api(const char *file, const int line,
		const int db_opt, const char *module, const char *msg, ...)
{
}

static inline void aee_kernel_dal_api(const char *file, const int line,
		const char *msg)
{
}

static inline int aee_is_printk_too_much(const char *module)
{
	return 0;
}

/* Connsys core dump report; a logging stub in the connectivity adapter, which only builds with MTK_COMBO. */
#ifdef CONFIG_MTK_COMBO
void aed_combo_exception_api(const int *log, int log_size, const int *phy,
			int phy_size, const char *detail, const int db_opt);
#else
static inline void aed_combo_exception_api(const int *log, int log_size,
			const int *phy, int phy_size, const char *detail,
			const int db_opt)
{
}
#endif
/* Modem exception report; a logging stub in drivers/misc/mediatek/ccci_platform. */
#ifdef CONFIG_MTK_CCCI_PLATFORM_DEPS
void aed_md_exception_api(const int *log, int log_size, const int *phy,
			int phy_size, const char *detail, const int db_opt);
#else
static inline void aed_md_exception_api(const int *log, int log_size,
			const int *phy, int phy_size, const char *detail,
			const int db_opt)
{
}
#endif
#define aed_md_exception(log, log_size, phy, phy_size, detail)		\
	aed_md_exception_api(log, log_size, phy, phy_size, detail,	\
			DB_OPT_DEFAULT)
#define aed_combo_exception(log, log_size, phy, phy_size, detail)	\
	aed_combo_exception_api(log, log_size, phy, phy_size, detail,	\
			DB_OPT_DEFAULT)

#define aee_kernel_exception(module, msg...) \
	aee_kernel_exception_api_func(__FILE__, __LINE__, DB_OPT_DEFAULT, \
				      module, msg)
#define aee_kernel_warning(module, msg...) \
	aee_kernel_warning_api_func(__FILE__, __LINE__, DB_OPT_DEFAULT, \
				    module, msg)
#define aee_kernel_exception_api(file, line, db_opt, module, msg...) \
	aee_kernel_exception_api_func(__FILE__, __LINE__, db_opt, module, msg)
#define aee_kernel_warning_api(file, line, db_opt, module, msg...) \
	aee_kernel_warning_api_func(__FILE__, __LINE__, db_opt, module, msg)
#define aee_kernel_reminding(module, msg...) \
	aee_kernel_reminding_api(__FILE__, __LINE__, DB_OPT_DEFAULT, \
				 module, msg)
#define aee_kernel_dal_show(msg) \
	aee_kernel_dal_api(__FILE__, __LINE__, msg)

static inline void aee_sram_printk(const char *fmt, ...)
{
}

static inline int aee_nested_printf(const char *fmt, ...)
{
	return 0;
}

static inline void aee_kernel_wdt_kick_Powkey_api(const char *module, int msg)
{
}

static inline int aee_kernel_wdt_kick_api(int kinterval)
{
	return 0;
}

static inline void aee_powerkey_notify_press(unsigned long pressed)
{
}

static inline int aee_kernel_Powerkey_is_press(void)
{
	return 0;
}

#endif /* __AEE_H__ */
