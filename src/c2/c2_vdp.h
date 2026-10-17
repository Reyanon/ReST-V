/*  Sega 315-5313 (Mega Drive VDP) for the System C / C2 core (ADR-107).
    Written for ReST-V from the documented hardware behaviour; output format and C2 wiring follow
    MAME src/devices/video/315_5313.cpp (BSD-3-Clause, David Haywood).
    This file is part of ReST-V. GPL-2.0-or-later.
*/
#ifndef RSTV_C2_VDP_H
#define RSTV_C2_VDP_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Raw pixel (MAME m_render_line_raw): bits 0-5 colour (palette*16 + index), bits 6-7 kind
#define C2VDP_SHADOW    0x00
#define C2VDP_BG        0x40
#define C2VDP_SPRITE    0x80
#define C2VDP_HIGHLIGHT 0xC0
// Rendered line = colour table indices: 0..511 = priority rank (0 backdrop, 3 / 6 sprite, others plane) * 64 + colour,
// 512 + raw pixel (kind | colour) when shadow / highlight is on or the display is off
#define C2VDP_IDX_RAW 0x200
#define C2VDP_IDX_N 0x300

typedef struct {
	uint16_t (*read68k)(uint32_t addr); // DMA source reads (68000 bus, word)
	void (*psg_write)(uint8_t d);       // SN76489 inside the VDP
} C2VdpBus;

void c2vdp_init(const C2VdpBus *bus);
void c2vdp_reset(void);

// 68000 side, offset = address & 0x1F (word access; byte writes are passed as a word with both bytes)
uint16_t c2vdp_read(uint32_t offset);
void c2vdp_write(uint32_t offset, uint16_t data);

// Timing, called by the machine. Lines 0..261; the machine passes the 68000 cycle inside the line for the HV counter
void c2vdp_line_start(int line);           // counters, interrupt flags (call before running the line)
void c2vdp_set_line_cycle(int cyc, int cycles_per_line);
int c2vdp_visible_lines(void);             // 224 or 240
int c2vdp_width(void);                     // 256 or 320
void c2vdp_render_line(int line, uint16_t *idx); // idx[0..width-1], see C2VDP_IDX_*
int c2vdp_display_enabled(void);

// Interrupt lines for the 68000 (C2 wiring): VBLANK -> level 6 (held until acknowledged), HINT -> level 4
int c2vdp_irq6(void);
int c2vdp_irq4(void);
void c2vdp_ack(int level);

int c2vdp_state(FILE *fp, int save);

#ifdef __cplusplus
}
#endif
#endif
