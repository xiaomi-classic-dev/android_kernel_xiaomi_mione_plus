/* MiOne power-path breadcrumbs. No allocation or printk in trace calls. */
#ifndef __MACH_MIONE_POWER_DIAG_H
#define __MACH_MIONE_POWER_DIAG_H

#include <linux/init.h>
#include <linux/types.h>

enum mione_power_stage {
	MIONE_FREQ_TARGET = 1, MIONE_FREQ_CANCEL, MIONE_FREQ_QUEUE,
	MIONE_FREQ_WORK, MIONE_FREQ_PRE, MIONE_FREQ_CLOCK,
	MIONE_FREQ_POST, MIONE_FREQ_DONE, MIONE_FREQ_WAIT_DONE,
	MIONE_ACPU_ENTER = 20, MIONE_ACPU_LOCK, MIONE_ACPU_LOCKED,
	MIONE_VDD_MEM_UP, MIONE_VDD_DIG_UP, MIONE_VDD_SC_UP,
	MIONE_CPU_SWITCH, MIONE_L2_LOCK, MIONE_L2_LOCKED,
	MIONE_L2_DONE, MIONE_BUS_VOTE, MIONE_VDD_SC_DOWN,
	MIONE_VDD_DIG_DOWN, MIONE_VDD_MEM_DOWN, MIONE_ACPU_DONE,
	MIONE_PLL_START = 40, MIONE_PLL_WAIT_START,
	MIONE_PLL_WAIT_DONE, MIONE_PLL_DONE,
	MIONE_SAW_ENTER = 50, MIONE_SAW_DONE, MIONE_SAW_ERROR,
	MIONE_WDOG_INIT = 60, MIONE_WDOG_PET, MIONE_WDOG_SUSPEND,
	MIONE_WDOG_RESUME, MIONE_WDOG_BARK, MIONE_WDOG_PANIC,
	MIONE_REBOOT,
};

#if defined(CONFIG_MACH_MIONE) && defined(CONFIG_ANDROID_RAM_CONSOLE)
void __init mione_power_diag_reserve(phys_addr_t start);
void mione_power_trace(unsigned int stage, int target, u32 a, u32 b,
		       u32 c, u32 d);
void *mione_power_diag_tz_buffer(phys_addr_t *phys);
void mione_power_diag_tz_registered(void);
#else
static inline void mione_power_trace(unsigned int stage, int target,
		u32 a, u32 b, u32 c, u32 d) { }
static inline void *mione_power_diag_tz_buffer(phys_addr_t *phys)
{
	return NULL;
}
static inline void mione_power_diag_tz_registered(void) { }
#endif

#endif
