/*
 * sn76489.c - Sega VDP PSG (SN76489 variant), compact C port.
 *
 * Ported from MAME 0.264 src/devices/sound/sn76496.cpp (BSD-3-Clause,
 * copyright-holders: Nicola Salmoria), SEGAPSG configuration:
 *   feedback mask 0x8000 (16-bit LFSR), white-noise taps 0x01 | 0x08, output negated,
 *   mono, clock divider 8 (on a clock/2 stream => one tick per 16 input clocks),
 *   not NCR-style, m_sega_style_psg = false (as MAME passes it: period 0 stays 0).
 * The READY line and Game Gear stereo are not needed on System C/C2 and are omitted.
 * This file is part of ReST-V. GPL-2.0-or-later.
 */
#include "sn76489.h"

#include <string.h>

#define MAX_OUTPUT     0x7fff
#define FEEDBACK_MASK  0x8000
#define WN_TAP1        0x01
#define WN_TAP2        0x08

void sn_init(sn_psg *p)
{
	int i;
	double out = MAX_OUTPUT / 4;     /* four channels, each gets 1/4 of the total range */

	memset(p, 0, sizeof(*p));
	p->last_reg = 0;                 /* m_sega_style_psg is false for SEGAPSG in MAME */
	for (i = 0; i < 8; i += 2) { p->reg[i] = 0; p->reg[i + 1] = 0; } /* volume 0 = max */
	p->rng = FEEDBACK_MASK;
	p->output[3] = p->rng & 1;

	for (i = 0; i < 15; i++) {       /* 2 dB steps */
		p->vol_table[i] = (int32_t)out;
		out /= 1.258925412;
	}
	p->vol_table[15] = 0;
	/* MAME starts m_volume[] at 0 (silent) even though the volume registers hold 0 */
	for (i = 0; i < 4; i++) p->volume[i] = 0;
	p->out = 0;
}

void sn_write(sn_psg *p, uint8_t data)
{
	int r, c, n;

	if (data & 0x80) {
		r = (data & 0x70) >> 4;
		p->last_reg = r;
		p->reg[r] = (p->reg[r] & 0x3f0) | (data & 0x0f);
	} else {
		r = p->last_reg;
	}

	c = r >> 1;
	switch (r) {
	case 0: case 2: case 4:          /* tone frequency */
		if ((data & 0x80) == 0) p->reg[r] = (p->reg[r] & 0x0f) | ((data & 0x3f) << 4);
		p->period[c] = p->reg[r];        /* Sega PSG: 0 stays 0 (not 0x400) */
		if (r == 4 && (p->reg[6] & 0x03) == 0x03) p->period[3] = p->period[2] << 1;
		break;
	case 1: case 3: case 5: case 7:  /* volume */
		p->volume[c] = p->vol_table[data & 0x0f];
		if ((data & 0x80) == 0) p->reg[r] = (p->reg[r] & 0x3f0) | (data & 0x0f);
		break;
	case 6:                          /* noise: frequency, mode */
		if ((data & 0x80) == 0) p->reg[r] = (p->reg[r] & 0x3f0) | (data & 0x0f);
		n = p->reg[6];
		p->period[3] = ((n & 3) == 3) ? (p->period[2] << 1) : (1 << (5 + (n & 3)));
		p->rng = FEEDBACK_MASK;
		break;
	}
}

int32_t sn_tick(sn_psg *p)
{
	int i;
	for (i = 0; i < 3; i++) {
		if (--p->count[i] <= 0) {
			p->output[i] ^= 1;
			p->count[i] = p->period[i];
		}
	}
	if (--p->count[3] <= 0) {
		/* noise mode (reg6 bit 2): both taps; periodic mode: tap2 held at 0 */
		int t1 = (p->rng & WN_TAP1) != 0;
		int t2 = ((p->rng & WN_TAP2) != 0) && (p->reg[6] & 4);
		p->rng >>= 1;
		if (t1 != t2) p->rng |= FEEDBACK_MASK;
		p->output[3] = p->rng & 1;
		p->count[3] = p->period[3];
	}
	p->out = -((p->output[0] ? p->volume[0] : 0) + (p->output[1] ? p->volume[1] : 0)
	         + (p->output[2] ? p->volume[2] : 0) + (p->output[3] ? p->volume[3] : 0));
	return p->out;
}
