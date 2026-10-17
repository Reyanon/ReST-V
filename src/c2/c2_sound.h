/*
 * c2_sound.h - Sega System C / C2 sound module (YM3438 + SN76489 PSG + uPD7759).
 *
 * Chip wiring follows MAME 0.264 src/mame/sega/segac2.cpp (BSD-3-Clause,
 * copyright-holders: David Haywood, Aaron Giles).
 * This file is part of ReST-V. GPL-2.0-or-later.
 *
 * Clocks: XL2 = 53.693175 MHz master. YM3438 = XL2/7 (native rate XL2/1008),
 * PSG = XL2/15, uPD7759 = 640 kHz. Output: 44100 Hz, stereo int16 (mono in both).
 * Mix (MAME): YM ch0 x0.50 + PSG x0.50 + uPD7759 x0.50, then a ~7 Hz DC blocker
 * (models the output coupling capacitor; removes the PSG's unipolar offset), clamp.
 *
 * Threading: all functions are meant to be called from the emulation thread.
 * c2snd_fetch() may be called from another thread (single-producer /
 * single-consumer ring); everything else must stay on one thread.
 */
#ifndef RESTV_C2_SOUND_H
#define RESTV_C2_SOUND_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C2SND_XL2_CLOCK     53693175u
#define C2SND_XL1_CLOCK     640000u
#define C2SND_OUTPUT_RATE   44100u
#define C2SND_RING_FRAMES   8192        /* power of two; ~186 ms at 44.1 kHz */

void c2snd_init(void);                 /* fixed chip clocks listed above; output rate 44100 Hz */
void c2snd_reset(void);
void c2snd_ym_write(int a, uint8_t d); /* a = 0..3 (A0/A1) */
uint8_t c2snd_ym_read(int a);          /* MAME/ymfm: status on a==0 only, 0 elsewhere */
int  c2snd_ym_irq(void);               /* current YM IRQ line (1 = asserted) -> 68000 IRQ 2 */
void c2snd_sn_write(uint8_t d);
void c2snd_upd_set_rom(const uint8_t *rom, uint32_t size); /* NULL/0 = no uPD7759 (System C) */
void c2snd_upd_set_bank(int bank);     /* 0x20000-byte banks (wrapped to the ROM's bank count) */
void c2snd_upd_set_start_delay(int d); /* extra uPD clocks before first DRQ (MAME ribbit: 250) */
void c2snd_upd_port(uint8_t d);
void c2snd_upd_start(int line);
void c2snd_upd_reset(int line);        /* /RESET line: a 1->0 edge resets the chip */
/* MAME busy_r() semantics exactly: returns the /BUSY pin level.
 *   1 = NOT busy (chip idle), 0 = busy (playing / starting).
 * With no uPD7759 (System C) it returns 1, matching segac2.cpp io_portc_r (0x40). */
int  c2snd_upd_busy(void);

/* Advance all chips by 'xl2_clocks' master clocks (53.693175 MHz units). Generates samples
   into an internal ring resampled to 44100 Hz, and runs YM timers so c2snd_ym_irq() is
   up to date. Called often (e.g. once per scanline, ~3420 clocks). */
void c2snd_advance(uint32_t xl2_clocks);
/* Pop up to max_frames stereo frames (interleaved L,R int16) produced so far; returns count. */
int  c2snd_fetch(int16_t *out, int max_frames);
/* Number of frames currently waiting in the ring. */
int  c2snd_available(void);
/* Save state: write (save=1) or read (save=0) the complete sound state; 0 = ok, -1 = error.
   The output ring is not saved; it is emptied on load. The uPD ROM pointer is not saved
   (call c2snd_upd_set_rom before loading); the bank number is. */
int  c2snd_state(FILE *fp, int save);
/* ADR-107: YM sound on a worker. Call c2snd_frame_end() after each frame; it mixes the previous frame (waiting for the
   worker) and hands this one over. The frontend may set c2snd_kick / c2snd_wait to run c2snd_worker() on another
   thread; left NULL it runs inline (same output). c2snd_sync() finishes pending work (before reset / save / load). */
void c2snd_frame_end(void);
void c2snd_sync(void);
void c2snd_worker(void);
extern void (*c2snd_kick)(void);
extern void (*c2snd_wait)(void);

/* Optional helpers */
/* YM busy flag: ymfm sets busy for 192 YM clocks after each data write. Our time base only
   moves in c2snd_advance(), so a CPU polling busy inside one advance step would spin until the
   next step. Default 0 = busy never reported (fast, safe); 1 = emulate. */
void c2snd_ym_set_busy_emulation(int on);
/* Per-chip gain in 1/256 units (default 128 = MAME route 0.50 each). */
void c2snd_set_gains(int ym, int psg, int upd);

#ifdef __cplusplus
}
#endif

#endif /* RESTV_C2_SOUND_H */
