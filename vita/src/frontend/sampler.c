// Sampling profiler (for measurement, only when ux0:data/restv/sampler exists).
// The main thread only writes what it is doing (rstv_act) and the running SH-2 block address (rstv_act_pc) to globals;
// a sampler thread on CPU2 reads them about every 250us and builds a histogram. No clock calls, so almost no cost on the main thread.
#include "sampler.h"

#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#include "platform/log.h"
#include "rstv_prof.h"


#define NACT 32
#define HBITS 12
#define HSIZE (1 << HBITS)

static volatile int s_run;
static unsigned s_act[NACT];
static unsigned s_pc[HSIZE], s_pcn[HSIZE];
static unsigned s_total;
static unsigned s_rax[8]; // RAX board state of the sound thread (rstv_rax_act)
extern volatile int rstv_rax_act;

static int sampler_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (s_run) {
		sceKernelDelayThread(250);
		const int a = rstv_act & (NACT - 1);
		s_act[a]++;
		s_total++;
		s_rax[rstv_rax_act & 7]++;
		if (a == 1 || a == 2) {
			const unsigned pc = rstv_act_pc | (a == 2 ? 1u : 0u); // slave marked with an odd address
			unsigned h = (pc * 2654435761u) >> (32 - HBITS);
			for (int k = 0; k < 16; k++, h = (h + 1) & (HSIZE - 1)) {
				if (s_pc[h] == pc || s_pcn[h] == 0) {
					s_pc[h] = pc;
					s_pcn[h]++;
					break;
				}
			}
		}
	}
	return 0;
}

void rstv_sampler_start(void)
{
	extern int rstv_rax_act_on;
	rstv_rax_act_on = 1;
	s_run = 1;
	SceUID t = sceKernelCreateThread("rstv_sampler", sampler_thread, 64, 0x2000, 0, 0x80000, NULL); // CPU3 (shared with the system):
	// on CPU1 the sound thread starved it (pblbeach), on CPU2 the prep and audio threads (34 samples a window in vfremix)
	if (t >= 0)
		sceKernelStartThread(t, 0, NULL);
	RSTV_LOG("sampler: started (%d)", (int)t);
}

void rstv_sampler_report(char *out, int size)
{
	static const char *names[NACT] = { "?", "msh2", "ssh2", "loop", "hblank", "scsp", "scu", "vblankin",
		"m68ksync", "front", "v2vout", "hbout", "hbin", "scudma", "erase", "v1change", "hbint", "hbdma", "v2start", "dsp",
		"scutimer", "v1draw", "v2screens", "v1end", "v2end", "cfetch", "cemit", "cother", "csync", "ccopy", "sndsync", "cheat" };
	int n = snprintf(out, size, "sample %u:", s_total);
	if (s_total && s_total != s_rax[0]) { // the board's share of wall time: native loops, blocks, interpreter, frame loop, dispatch
		static const char *rn[8] = { "-", "nl", "jit", "int", "frame", "disp", "6", "7" };
		n += snprintf(out + n, size - n, " rax");
		for (int k = 1; k < 8 && n < size; k++)
			if (s_rax[k])
				n += snprintf(out + n, size - n, " %s %.1f%%", rn[k], 100.0 * s_rax[k] / s_total);
		n += snprintf(out + n, size - n, " |");
	}
	memset(s_rax, 0, sizeof(s_rax));
	for (int a = 0; a < NACT && n < size; a++)
		if (s_act[a])
			n += snprintf(out + n, size - n, " %s %.1f%%", names[a], 100.0 * s_act[a] / s_total);
	// top 12 blocks
	for (int t = 0; t < 48 && n < size; t++) {
		unsigned best = 0;
		int bi = -1;
		for (int h = 0; h < HSIZE; h++)
			if (s_pcn[h] > best) {
				best = s_pcn[h];
				bi = h;
			}
		if (bi < 0)
			break;
		n += snprintf(out + n, size - n, " | %c%08x %.1f%%", (s_pc[bi] & 1) ? 'S' : 'M', s_pc[bi] & ~1u, 100.0 * best / s_total);
		s_pcn[bi] = 0;
	}
	if (n < size)
		snprintf(out + n, size - n, "\n");
	memset(s_act, 0, sizeof(s_act));
	memset(s_pc, 0, sizeof(s_pc));
	memset(s_pcn, 0, sizeof(s_pcn));
	s_total = 0;
}
