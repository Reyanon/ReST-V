/*
 * c2_ym.h - internal C interface to the YM3438 wrapper (c2_ym.cpp, built on ymfm).
 *
 * Wrapper of ymfm (BSD-3-Clause, copyright Aaron Giles) as used by MAME 0.264
 * src/devices/sound/ymopn.cpp.
 * This file is part of ReST-V. GPL-2.0-or-later.
 *
 * All times here are in XL2 master clocks (YM clock = XL2/7).
 */
#ifndef RESTV_C2_YM_H
#define RESTV_C2_YM_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C2YM_MASTER_DIV     7u                      /* XL2 clocks per YM clock */
#define C2YM_SAMPLE_PERIOD  (7u * 144u)             /* XL2 clocks per YM output sample */

void    c2ym_init(void);
void    c2ym_reset(void);
void    c2ym_write(int a, uint8_t d);
uint8_t c2ym_read(int a);
int     c2ym_irq(void);
int32_t c2ym_sample(void);                 /* clock one YM sample, return channel 0 (int16 range) */
/* ADR-107: second chip that only makes sound (the first keeps timers, IRQ and status on the emulation thread).
   It gets the same register writes, replayed on the sound worker */
void    c2ym_gen_write(int a, uint8_t d);
void    c2ym_count_tick(void);         /* timer chip: one sample period passes (timer B phase), no sound */
int32_t c2ym_gen_sample(void);
void    c2ym_advance_time(uint32_t master); /* run timers / busy time base */
void    c2ym_set_busy_emulation(int on);
int     c2ym_state(FILE *fp, int save);    /* 0 ok, -1 error */

#ifdef __cplusplus
}
#endif

#endif /* RESTV_C2_YM_H */
