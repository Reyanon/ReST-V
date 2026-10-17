/*
 * sn76489.h - Sega VDP PSG (SN76489 variant), compact C port.
 *
 * Ported from MAME 0.264 src/devices/sound/sn76496.cpp/.h (BSD-3-Clause,
 * copyright-holders: Nicola Salmoria).
 * This file is part of ReST-V. GPL-2.0-or-later.
 */
#ifndef RESTV_C2_SN76489_H
#define RESTV_C2_SN76489_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Saved verbatim; fixed-width fields only. */
typedef struct sn_psg {
	int32_t  vol_table[16];
	int32_t  reg[8];
	int32_t  last_reg;
	int32_t  volume[4];
	int32_t  period[4];
	int32_t  count[4];
	int32_t  output[4];
	uint32_t rng;
	int32_t  out;        /* current mixed output (negated, -32767..0) */
} sn_psg;

void sn_init(sn_psg *p);
void sn_write(sn_psg *p, uint8_t data);
/* One divided clock (chip clock / 16). Returns the current output. */
int32_t sn_tick(sn_psg *p);

#ifdef __cplusplus
}
#endif

#endif
