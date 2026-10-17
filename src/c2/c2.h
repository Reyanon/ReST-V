/*  Sega System C / C2 arcade board (ADR-107): 68000 + 315-5313 VDP + YM3438 + SN76489 (+ uPD7759 on C2),
    315-5296 I/O, EPM5032 protection / palette banking, external palette RAM.
    Board logic follows MAME src/mame/sega/segac2.cpp (BSD-3-Clause, David Haywood, Aaron Giles).
    This file is part of ReST-V. GPL-2.0-or-later.
*/
#ifndef RSTV_C2_H
#define RSTV_C2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set table (c2_games.h) lookup: 1 if romset is a System C / C2 game
int c2_is_game(const char *romset);
const char *c2_game_title(const char *romset);

// Load <rom_dir>/<set>.zip (+ parent) and reset. 0 = ok, otherwise message in err
int c2_load(const char *rom_dir, const char *romset, char *err, int errlen);
void c2_unload(void);
void c2_reset(void);

// One frame (262 lines, 59.92 Hz). Inputs are pressed-bit masks in the ST-V layout (stvio.h STV_IN_* / STV_SYS_*):
// the C/C2 ports use the same bit order (B1 B2 B3 - DOWN UP RIGHT LEFT; COIN1 COIN2 TEST SERVICE START1 START2)
void c2_run_frame(uint8_t p1, uint8_t p2, uint8_t sys);

// Last frame, RGBA8888 (uint32 = 0xAABBGGRR, like video.h), pitch in pixels
const uint32_t *c2_frame(int *w, int *h, int *pitch);

// Sound produced so far: stereo int16 at 44100 Hz
int c2_audio(int16_t *out, int max_frames);

// Battery-backed work RAM (64 KB)
uint8_t *c2_nvram(int *size);

int c2_state_save(const char *path); // 0 = ok
int c2_state_load(const char *path);

#ifdef __cplusplus
}
#endif
#endif
