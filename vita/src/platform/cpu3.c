// CPU3 for app threads (ADR-218, as ReSys12 ADR-069). See cpu3.h
#include "platform/cpu3.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#define CPU3_MASK 0x80000 // (no SDK constant)

static int cpu3_probe(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	return 0;
}

static int s_cpu3_ok = -1;

void rstv_cpu3_recheck(void) { s_cpu3_ok = -1; }

int rstv_cpu3_usable(void)
{
	int ok = s_cpu3_ok;
	if (ok < 0) {
		ok = 0;
#ifdef RSTV_DEBUG_TOOLS
		SceIoStat st; // test: ux0:data/restv/cpu3off = act as a stock Vita (CPU3 closed) even where a plugin opened it
		if (sceIoGetstat("ux0:data/restv/cpu3off", &st) >= 0) {
			s_cpu3_ok = 0;
			return 0;
		}
#endif
		const SceUID t = sceKernelCreateThread("rstv_cpu3", cpu3_probe, 0x60, 0x1000, 0, CPU3_MASK, NULL);
		if (t >= 0) {
			SceUInt timeout = 200000; // (created but never run there: not usable)
			if (sceKernelStartThread(t, 0, NULL) >= 0 && sceKernelWaitThreadEnd(t, NULL, &timeout) >= 0)
				ok = 1;
			sceKernelDeleteThread(t);
		}
		s_cpu3_ok = ok;
	}
	return ok;
}

int rstv_cpu3_mask(int fallback) { return rstv_cpu3_usable() ? CPU3_MASK : fallback; }
