/*  ReST-V: lightweight per-frame profiling buckets (microseconds).
    The frontend sets rstv_prof_clock to a monotonic microsecond clock; NULL disables it. */
#ifndef RSTV_PROF_H
#define RSTV_PROF_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RSTV_PROF_MSH2, RSTV_PROF_SSH2, RSTV_PROF_VDP, RSTV_PROF_SOUND, RSTV_PROF_OTHER,
       /* sub-buckets inside VDP (not added to the total) */
       RSTV_PROF_VDP1, RSTV_PROF_VDP2L, RSTV_PROF_COMPOSE,
       /* per-layer inside VDP2L */
       RSTV_PROF_L_SPR, RSTV_PROF_L_N0, RSTV_PROF_L_N1, RSTV_PROF_L_N2, RSTV_PROF_L_N3, RSTV_PROF_L_R0,
       /* async SCSP thread: real work (68K + SCSP), excluding its spin-wait */
       RSTV_PROF_SND_THREAD,
       /* VDP1 helper thread: drawing time / main thread waiting for it */
       RSTV_PROF_VDP1_THREAD, RSTV_PROF_VDP1_WAIT,
       /* 68K part of RSTV_PROF_SND_THREAD */
       RSTV_PROF_SND_68K, RSTV_PROF_SND_DSP, RSTV_PROF_SND_SLOTS,
       /* main thread waiting for the SCSP thread at VBlank */
       RSTV_PROF_SND_WAIT,
       RSTV_PROF_COUNT };

extern u64 (*rstv_prof_clock)(void);
extern u64 rstv_prof_us[RSTV_PROF_COUNT];

#define RSTV_PROF_T() (rstv_prof_clock ? rstv_prof_clock() : 0)
#define RSTV_PROF_ADD(b, t0) do { if (rstv_prof_clock) rstv_prof_us[b] += rstv_prof_clock() - (t0); } while (0)

/* Fine buckets inside the per-deciline loop (about 20k clock reads per frame). On the Vita the clock read
   itself cost about 10ms per frame, so these only run when rstv_prof_fine is set. */
extern int rstv_prof_fine;

/* Sampling profiler marks (vita sampler.c): what the main thread is doing and the SH-2 block being run.
   1 msh2, 2 ssh2, 3 emulate loop, 4 hblank, 5 scsp, 6 scu/smpc, 7 vblankin, 8 vblankout, 9 frontend */
extern volatile int rstv_act;
extern volatile unsigned rstv_act_pc;
#define RSTV_PROF_TF() ((rstv_prof_fine && rstv_prof_clock) ? rstv_prof_clock() : 0)
#define RSTV_PROF_ADDF(b, t0) do { if (rstv_prof_fine && rstv_prof_clock) rstv_prof_us[b] += rstv_prof_clock() - (t0); } while (0)

#ifdef __cplusplus
}
#endif

#endif
