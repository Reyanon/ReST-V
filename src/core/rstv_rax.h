/* ReST-V (ADR-173): Acclaim RAX sound board (batmanfr): ADSP-2181 + stereo DAC (rstv_rax.cpp) */
#ifndef RSTV_RAX_H
#define RSTV_RAX_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int rstv_rax_active;

/* rom: the board's "rax" region (MAME layout: boot ROM at 0, sample ROMs from 0x400000); kept, not copied */
void rstv_rax_init(const u8 *rom, u32 size);
void rstv_rax_shutdown(void);
void rstv_rax_reset(void);
/* emu thread: SH-2 write to 0x04800000 (upper word) */
void rstv_rax_data_w(u16 data);
/* emu thread: end of an emulated frame (the commands written so far belong to it) */
void rstv_rax_frame_end(void);
/* sound thread: run the DSP for one frame and add its output to the frame's samples */
void rstv_rax_mix(s32 *l, s32 *r, int n);
/* save state (between frames, sound thread at rest): 0 = ok */
#include <stdio.h>
int rstv_rax_state(FILE *fp, int save);

#ifdef __cplusplus
}
#endif

#endif
