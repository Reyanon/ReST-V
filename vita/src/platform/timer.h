#ifndef RSTV_PLATFORM_TIMER_H
#define RSTV_PLATFORM_TIMER_H

#include <stdint.h>
#include <psp2/kernel/processmgr.h>

// process monotonic time (microseconds)
static inline uint64_t rstv_time_us(void)
{
	return (uint64_t)sceKernelGetProcessTimeWide();
}

#endif
