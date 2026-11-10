// CPU3 (the Vita's fourth core) for app threads (ADR-218). A stock system refuses user threads there
// (sceKernelCreateThread: 0x80028025); a plugin such as CoreUnlocker, or the kernel helper where its code is the
// known one, opens it. Threads meant for CPU3 ask rstv_cpu3_mask for their mask.
#ifndef RSTV_PLATFORM_CPU3_H
#define RSTV_PLATFORM_CPU3_H

int rstv_cpu3_usable(void);          // 1 if a thread really ran on CPU3 (asked once, cached)
void rstv_cpu3_recheck(void);        // ask again (the kernel helper has just opened it)
int rstv_cpu3_mask(int fallback);    // CPU3's mask when it is open, else fallback (0 = any user core)

#endif
