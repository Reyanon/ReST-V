/*
 * upd7759.h - NEC uPD7759 ADPCM speech processor, stand-alone (ROM) mode, compact C port.
 *
 * Ported from MAME 0.264 src/devices/sound/upd7759.cpp/.h (BSD-3-Clause,
 * copyright-holders: Juergen Buchmueller, Mike Balfour, Howie Cohen, Olivier Galibert,
 * Aaron Giles).
 * This file is part of ReST-V. GPL-2.0-or-later.
 */
#ifndef RESTV_C2_UPD7759_H
#define RESTV_C2_UPD7759_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Saved state: fixed-width fields only (ROM pointer lives outside). */
typedef struct upd_state {
	int32_t  state;
	int32_t  clocks_left;
	int32_t  debt;            /* clocks owed by a negative post-DRQ count (MAME's 'pos' carry) */
	int32_t  post_drq_state;
	int32_t  post_drq_clocks;
	uint32_t offset;
	uint32_t repeat_offset;
	uint32_t start_delay;
	int32_t  bank;
	uint16_t nibbles_left;
	int16_t  sample;
	uint8_t  fifo_in;
	uint8_t  reset;
	uint8_t  start;
	uint8_t  drq;
	uint8_t  repeat_count;
	uint8_t  req_sample;
	uint8_t  last_sample;
	uint8_t  block_header;
	uint8_t  sample_rate;
	uint8_t  first_valid_header;
	int8_t   adpcm_state;
	uint8_t  adpcm_data;
} upd_state;

typedef struct upd_chip {
	upd_state s;
	const uint8_t *rom;
	uint32_t rom_size;
} upd_chip;

void upd_init(upd_chip *u);          /* power-on (constructor + device_reset) */
void upd_device_reset(upd_chip *u);  /* MAME device_reset() */
void upd_set_rom(upd_chip *u, const uint8_t *rom, uint32_t size);
void upd_set_bank(upd_chip *u, int bank);
void upd_reset_w(upd_chip *u, int line);
void upd_start_w(upd_chip *u, int line);
void upd_port_w(upd_chip *u, uint8_t data);
int  upd_busy_r(const upd_chip *u);  /* /BUSY: 1 = idle (not busy) */
/* Run 'clocks' chip clocks (640 kHz). Returns the sum of (sample * clocks) over the span,
   in raw ADPCM units (MAME scales by 128/32768 to get +-1.0 => x128 for int16). Output is 0
   while idle. */
int64_t upd_run(upd_chip *u, uint32_t clocks);

#ifdef __cplusplus
}
#endif

#endif
