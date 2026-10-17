/*
 * upd7759.c - NEC uPD7759 ADPCM speech processor, stand-alone (ROM) mode, compact C port.
 *
 * Ported from MAME 0.264 src/devices/sound/upd7759.cpp (BSD-3-Clause,
 * copyright-holders: Juergen Buchmueller, Mike Balfour, Howie Cohen, Olivier Galibert,
 * Aaron Giles). Only the stand-alone mode (/MD high, sample ROM attached) used by Sega
 * System C2 is kept; slave mode / DRQ callbacks are dropped. The state machine, ADPCM
 * tables, timing constants and start delay are unchanged. MAME's 160 kHz stream loop is
 * replaced by running whole chip clocks; a negative post-DRQ clock count is carried as
 * 'debt' exactly like MAME's fractional 'pos' carry.
 * This file is part of ReST-V. GPL-2.0-or-later.
 */
#include "upd7759.h"

#include <string.h>

enum {
	STATE_IDLE, STATE_DROP_DRQ, STATE_START, STATE_FIRST_REQ, STATE_LAST_SAMPLE,
	STATE_DUMMY1, STATE_ADDR_MSB, STATE_ADDR_LSB, STATE_DUMMY2, STATE_BLOCK_HEADER,
	STATE_NIBBLE_COUNT, STATE_NIBBLE_MSN, STATE_NIBBLE_LSN
};

#define SAMPLE_OFFSET_SHIFT 1   /* uPD7759 (not 7756) */
#define BANK_SIZE           0x20000u

static const int16_t upd775x_step[16][16] =
{
	{ 0,  0,  1,  2,  3,   5,   7,  10,  0,   0,  -1,  -2,  -3,   -5,   -7,  -10 },
	{ 0,  1,  2,  3,  4,   6,   8,  13,  0,  -1,  -2,  -3,  -4,   -6,   -8,  -13 },
	{ 0,  1,  2,  4,  5,   7,  10,  15,  0,  -1,  -2,  -4,  -5,   -7,  -10,  -15 },
	{ 0,  1,  3,  4,  6,   9,  13,  19,  0,  -1,  -3,  -4,  -6,   -9,  -13,  -19 },
	{ 0,  2,  3,  5,  8,  11,  15,  23,  0,  -2,  -3,  -5,  -8,  -11,  -15,  -23 },
	{ 0,  2,  4,  7, 10,  14,  19,  29,  0,  -2,  -4,  -7, -10,  -14,  -19,  -29 },
	{ 0,  3,  5,  8, 12,  16,  22,  33,  0,  -3,  -5,  -8, -12,  -16,  -22,  -33 },
	{ 1,  4,  7, 10, 15,  20,  29,  43, -1,  -4,  -7, -10, -15,  -20,  -29,  -43 },
	{ 1,  4,  8, 13, 18,  25,  35,  53, -1,  -4,  -8, -13, -18,  -25,  -35,  -53 },
	{ 1,  6, 10, 16, 22,  31,  43,  64, -1,  -6, -10, -16, -22,  -31,  -43,  -64 },
	{ 2,  7, 12, 19, 27,  37,  51,  76, -2,  -7, -12, -19, -27,  -37,  -51,  -76 },
	{ 2,  9, 16, 24, 34,  46,  64,  96, -2,  -9, -16, -24, -34,  -46,  -64,  -96 },
	{ 3, 11, 19, 29, 41,  57,  79, 117, -3, -11, -19, -29, -41,  -57,  -79, -117 },
	{ 4, 13, 24, 36, 50,  69,  96, 143, -4, -13, -24, -36, -50,  -69,  -96, -143 },
	{ 4, 16, 29, 44, 62,  85, 118, 175, -4, -16, -29, -44, -62,  -85, -118, -175 },
	{ 6, 20, 36, 54, 76, 104, 144, 214, -6, -20, -36, -54, -76, -104, -144, -214 },
};

static const int8_t upd775x_state_table[16] = { -1, -1, 0, 0, 1, 2, 2, 3, -1, -1, 0, 0, 1, 2, 2, 3 };

static uint8_t read_byte(const upd_chip *u, uint32_t offs)
{
	uint32_t a;
	if (!u->rom || !u->rom_size) return 0;
	a = (uint32_t)u->s.bank * BANK_SIZE + (offs & (BANK_SIZE - 1));
	if (a >= u->rom_size) a %= u->rom_size;
	return u->rom[a];
}

static void update_adpcm(upd_state *s, int data)
{
	s->sample = (int16_t)(s->sample + upd775x_step[s->adpcm_state][data]);
	s->adpcm_state = (int8_t)(s->adpcm_state + upd775x_state_table[data]);
	if (s->adpcm_state < 0) s->adpcm_state = 0;
	else if (s->adpcm_state > 15) s->adpcm_state = 15;
}

/* MAME upd775x_device::advance_state(), stand-alone mode */
static void advance_state(upd_chip *u)
{
	upd_state *s = &u->s;
	switch (s->state) {
	case STATE_IDLE:
		s->clocks_left = 4;
		break;
	case STATE_DROP_DRQ:
		s->drq = 0;
		s->clocks_left = s->post_drq_clocks;
		s->state = s->post_drq_state;
		break;
	case STATE_START:
		s->req_sample = s->fifo_in;
		s->clocks_left = 70 + (int32_t)s->start_delay;
		s->state = STATE_FIRST_REQ;
		break;
	case STATE_FIRST_REQ:
		s->drq = 1;
		s->clocks_left = 44;
		s->state = STATE_LAST_SAMPLE;
		break;
	case STATE_LAST_SAMPLE:
		s->last_sample = read_byte(u, 0);
		s->drq = 1;
		s->clocks_left = 28;
		s->state = (s->req_sample > s->last_sample) ? STATE_IDLE : STATE_DUMMY1;
		break;
	case STATE_DUMMY1:
		s->drq = 1;
		s->clocks_left = 32;
		s->state = STATE_ADDR_MSB;
		break;
	case STATE_ADDR_MSB:
		s->offset = (uint32_t)read_byte(u, s->req_sample * 2 + 5) << (8 + SAMPLE_OFFSET_SHIFT);
		s->drq = 1;
		s->clocks_left = 44;
		s->state = STATE_ADDR_LSB;
		break;
	case STATE_ADDR_LSB:
		s->offset |= (uint32_t)read_byte(u, s->req_sample * 2 + 6) << SAMPLE_OFFSET_SHIFT;
		s->drq = 1;
		s->clocks_left = 36;
		s->state = STATE_DUMMY2;
		break;
	case STATE_DUMMY2:
		s->offset++;
		s->first_valid_header = 0;
		s->drq = 1;
		s->clocks_left = 36;
		s->state = STATE_BLOCK_HEADER;
		break;
	case STATE_BLOCK_HEADER:
		if (s->repeat_count) {
			s->repeat_count--;
			s->offset = s->repeat_offset;
		}
		s->block_header = read_byte(u, s->offset++);
		s->drq = 1;
		switch (s->block_header & 0xc0) {
		case 0x00:  /* silence */
			s->clocks_left = 1024 * ((s->block_header & 0x3f) + 1);
			s->state = (s->block_header == 0 && s->first_valid_header) ? STATE_IDLE : STATE_BLOCK_HEADER;
			s->sample = 0;
			s->adpcm_state = 0;
			break;
		case 0x40:  /* 256 nibbles */
			s->sample_rate = (uint8_t)((s->block_header & 0x3f) + 1);
			s->nibbles_left = 256;
			s->clocks_left = 36;
			s->state = STATE_NIBBLE_MSN;
			break;
		case 0x80:  /* n nibbles */
			s->sample_rate = (uint8_t)((s->block_header & 0x3f) + 1);
			s->clocks_left = 36;
			s->state = STATE_NIBBLE_COUNT;
			break;
		case 0xc0:  /* repeat loop */
			s->repeat_count = (uint8_t)((s->block_header & 7) + 1);
			s->repeat_offset = s->offset;
			s->clocks_left = 36;
			s->state = STATE_BLOCK_HEADER;
			break;
		}
		if (s->block_header != 0) s->first_valid_header = 1;
		break;
	case STATE_NIBBLE_COUNT:
		s->nibbles_left = (uint16_t)(read_byte(u, s->offset++) + 1);
		s->drq = 1;
		s->clocks_left = 36;
		s->state = STATE_NIBBLE_MSN;
		break;
	case STATE_NIBBLE_MSN:
		s->adpcm_data = read_byte(u, s->offset++);
		update_adpcm(s, s->adpcm_data >> 4);
		s->drq = 1;
		s->clocks_left = s->sample_rate * 4;
		s->state = (--s->nibbles_left == 0) ? STATE_BLOCK_HEADER : STATE_NIBBLE_LSN;
		break;
	case STATE_NIBBLE_LSN:
		update_adpcm(s, s->adpcm_data & 15);
		s->clocks_left = s->sample_rate * 4;
		s->state = (--s->nibbles_left == 0) ? STATE_BLOCK_HEADER : STATE_NIBBLE_MSN;
		break;
	}

	if (s->drq) {
		s->post_drq_state = s->state;
		s->post_drq_clocks = s->clocks_left - 21;
		s->state = STATE_DROP_DRQ;
		s->clocks_left = 21;
	}
}

void upd_device_reset(upd_chip *u)
{
	upd_state *s = &u->s;
	s->state = STATE_IDLE;
	s->clocks_left = 0;
	s->debt = 0;
	s->nibbles_left = 0;
	s->repeat_count = 0;
	s->post_drq_state = STATE_IDLE;
	s->post_drq_clocks = 0;
	s->req_sample = 0;
	s->last_sample = 0;
	s->block_header = 0;
	s->sample_rate = 0;
	s->first_valid_header = 0;
	s->offset = 0;
	s->repeat_offset = 0;
	s->adpcm_state = 0;
	s->adpcm_data = 0;
	s->sample = 0;
	s->drq = 0;                 /* upd7759_device::device_reset */
}

void upd_init(upd_chip *u)
{
	const uint8_t *rom = u->rom;
	uint32_t size = u->rom_size;
	memset(u, 0, sizeof(*u));
	u->rom = rom;
	u->rom_size = size;
	u->s.reset = 1;
	u->s.start = 1;
	upd_device_reset(u);
}

void upd_set_rom(upd_chip *u, const uint8_t *rom, uint32_t size)
{
	u->rom = (rom && size) ? rom : 0;
	u->rom_size = (rom && size) ? size : 0;
	upd_set_bank(u, u->s.bank);
}

void upd_set_bank(upd_chip *u, int bank)
{
	uint32_t nb = u->rom_size / BANK_SIZE;
	if (bank < 0) bank = 0;
	if (nb == 0) nb = 1;
	u->s.bank = (int32_t)((uint32_t)bank % nb);
}

void upd_reset_w(upd_chip *u, int line)
{
	uint8_t old = u->s.reset;
	u->s.reset = (line != 0);
	if (old && !u->s.reset) upd_device_reset(u);
}

void upd_start_w(upd_chip *u, int line)
{
	uint8_t old = u->s.start;
	u->s.start = (line != 0);
	if (u->s.state == STATE_IDLE && old && !u->s.start && u->s.reset)
		u->s.state = STATE_START;
}

void upd_port_w(upd_chip *u, uint8_t data) { u->s.fifo_in = data; }

int upd_busy_r(const upd_chip *u) { return u->s.state == STATE_IDLE; }

int64_t upd_run(upd_chip *u, uint32_t clocks)
{
	upd_state *s = &u->s;
	int64_t sum = 0;

	for (;;) {
		if (s->state == STATE_IDLE) {      /* MAME leaves m_clocks_left at 0 when going idle */
			s->clocks_left = 0;
			s->debt = 0;
			break;
		}
		if (s->clocks_left <= 0) {
			if (s->clocks_left < 0) s->debt += -s->clocks_left;
			s->clocks_left = 0;
			advance_state(u);
			continue;
		}
		if (s->debt > 0) {                 /* borrowed time: consumed without output */
			int32_t t = s->debt < s->clocks_left ? s->debt : s->clocks_left;
			s->debt -= t;
			s->clocks_left -= t;
			continue;
		}
		if (clocks == 0) break;
		{
			uint32_t t = (uint32_t)s->clocks_left < clocks ? (uint32_t)s->clocks_left : clocks;
			sum += (int64_t)s->sample * t;
			s->clocks_left -= (int32_t)t;
			clocks -= t;
		}
	}
	return sum;
}
