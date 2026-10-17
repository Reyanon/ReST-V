/*  Sega 315-5313 (Mega Drive VDP) for the System C / C2 core (ADR-107).
    Line renderer written for ReST-V from the documented hardware behaviour. Register/port handling, interrupt
    timing and the raw output format (what the C2 colour circuit sees) follow MAME
    src/devices/video/315_5313.cpp (BSD-3-Clause, David Haywood).
    This file is part of ReST-V. GPL-2.0-or-later.
*/
#include "c2_vdp.h"

#include <string.h>
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define C2VDP_NEON 1
#endif

static C2VdpBus s_bus;

static uint8_t s_vram[0x10000];   // big-endian bytes (word n at 2n: high byte first)
static uint16_t s_vsram[40];
static uint16_t s_cram[64];       // kept for reads; C2 colours come from its own palette RAM
static uint8_t s_reg[32];
static int s_pending, s_code;
static uint32_t s_addr;
static int s_fill_pending;        // DMA fill waits for the next data port write
static int s_line, s_cyc, s_cyc_line = 570;
static int s_irq4_counter, s_irq4_pending, s_irq6_pending, s_vblank;
static int s_hold4, s_hold6;      // C2: both lines are HOLD_LINE (cleared when the 68000 acknowledges)
static int s_collision, s_imode, s_odd;
static int s_visible = 224;
// Decoded tiles (ADR-107 speed): 2048 tiles x 8 rows x 8 pixels (colour 0-15), rebuilt when VRAM under the tile changes
static uint8_t s_tdec[2048 * 64];
static uint32_t s_tdirty[2048 / 32];
#define TILE_DIRTY(a) (s_tdirty[((a) & 0xFFFF) >> 10] |= 1u << ((((a) & 0xFFFF) >> 5) & 31))

#define R(n) s_reg[n]
#define H40 ((R(12) & 0x81) != 0)
#define AUTOINC R(15)

void c2vdp_init(const C2VdpBus *bus)
{
	s_bus = *bus;
	c2vdp_reset();
}

void c2vdp_reset(void)
{
	memset(s_vram, 0, sizeof(s_vram));
	memset(s_vsram, 0, sizeof(s_vsram));
	memset(s_cram, 0, sizeof(s_cram));
	memset(s_reg, 0, sizeof(s_reg));
	s_pending = s_code = 0;
	s_addr = 0;
	s_fill_pending = 0;
	s_line = 0;
	s_cyc = 0;
	s_irq4_counter = 0;
	s_irq4_pending = s_irq6_pending = s_vblank = 0;
	s_hold4 = s_hold6 = 0;
	s_collision = s_imode = s_odd = 0;
	s_visible = 224;
	memset(s_tdirty, 0xFF, sizeof(s_tdirty));
}

int c2vdp_visible_lines(void) { return s_visible; }
int c2vdp_width(void) { return H40 ? 320 : 256; }
int c2vdp_display_enabled(void) { return (R(1) & 0x40) != 0; }
int c2vdp_irq6(void) { return s_hold6; }
int c2vdp_irq4(void) { return s_hold4; }

void c2vdp_ack(int level)
{
	if (level == 6)
		s_hold6 = 0;
	if (level == 4) {
		s_hold4 = 0;
		s_irq4_pending = 0;
	}
}

// ---- memory ----
static void vram_write_word(uint32_t a, uint16_t d)
{
	a &= 0xFFFF;
	if (a & 1) // odd address: bytes swapped (MAME vdp_vram_write)
		d = (uint16_t)((d >> 8) | (d << 8));
	a &= 0xFFFE;
	s_vram[a] = (uint8_t)(d >> 8);
	s_vram[a + 1] = (uint8_t)d;
	TILE_DIRTY(a);
}

static uint16_t vram_word(uint32_t a)
{
	a &= 0xFFFE;
	return (uint16_t)(s_vram[a] << 8 | s_vram[a + 1]);
}

static void target_write(uint16_t d)
{
	switch (s_code & 0x0F) {
	case 1: vram_write_word(s_addr, d); break;
	case 3: s_cram[(s_addr >> 1) & 0x3F] = d; break;
	case 5: if (((s_addr >> 1) & 0x3F) < 40) s_vsram[(s_addr >> 1) & 0x3F] = d & 0x7FF; break;
	default: break;
	}
	s_addr = (s_addr + AUTOINC) & 0xFFFF;
}

static uint32_t dma_len(void)
{
	uint32_t n = R(19) | (R(20) << 8);
	return n ? n : 0x10000;
}

static void dma_68k(void)
{
	uint32_t src = (R(21) | (R(22) << 8) | ((R(23) & 0x7F) << 16)) << 1;
	uint32_t n = dma_len();
	for (uint32_t i = 0; i < n; i++) {
		target_write(s_bus.read68k(src));
		src = (src & 0xFE0000) | ((src + 2) & 0x1FFFF); // the source wraps inside a 128 KB window
	}
	R(19) = R(20) = 0;
	R(21) = (uint8_t)(src >> 1);
	R(22) = (uint8_t)(src >> 9);
}

static void dma_fill(uint16_t d)
{
	uint32_t n = dma_len();
	if ((s_code & 0x0F) == 1) {
		for (uint32_t i = 0; i < n; i++) {
			s_vram[(s_addr ^ 1) & 0xFFFF] = (uint8_t)(d >> 8);
			TILE_DIRTY(s_addr);
			s_addr = (s_addr + AUTOINC) & 0xFFFF;
		}
	} else {
		for (uint32_t i = 0; i < n; i++)
			target_write(d);
	}
	R(19) = R(20) = 0;
}

static void dma_copy(void)
{
	uint32_t src = R(21) | (R(22) << 8);
	uint32_t n = dma_len();
	for (uint32_t i = 0; i < n; i++) {
		s_vram[s_addr & 0xFFFF] = s_vram[src & 0xFFFF];
		TILE_DIRTY(s_addr);
		src = (src + 1) & 0xFFFF;
		s_addr = (s_addr + AUTOINC) & 0xFFFF;
	}
	R(19) = R(20) = 0;
	R(21) = (uint8_t)src;
	R(22) = (uint8_t)(src >> 8);
}

static void set_register(int n, uint8_t v)
{
	if (n >= 24)
		return;
	s_reg[n] = v;
	if (n == 0 && s_irq4_pending)
		s_hold4 = (v & 0x10) != 0; // MAME vdp_set_register: re-raise or drop a pending HINT
}

static void ctrl_write(uint16_t d)
{
	if (!s_pending) {
		if ((d & 0xC000) == 0x8000) { // register write
			set_register((d >> 8) & 0x1F, (uint8_t)d);
			return;
		}
		s_code = (s_code & 0x3C) | (d >> 14);
		s_addr = (s_addr & 0xC000) | (d & 0x3FFF);
		s_pending = 1;
		return;
	}
	s_pending = 0;
	s_code = (s_code & 0x03) | ((d >> 2) & 0x3C);
	s_addr = (s_addr & 0x3FFF) | ((d & 3) << 14);
	if ((s_code & 0x20) && (R(1) & 0x10)) {
		switch (R(23) >> 6) {
		case 0: case 1: dma_68k(); break;
		case 2: s_fill_pending = 1; break;
		case 3: dma_copy(); break;
		}
	}
}

static void data_write(uint16_t d)
{
	s_pending = 0;
	if (s_fill_pending) {
		s_fill_pending = 0;
		target_write(d);
		s_addr = (s_addr - AUTOINC) & 0xFFFF; // the fill starts at the same address
		dma_fill(d);
		return;
	}
	target_write(d);
}

static uint16_t data_read(void)
{
	uint16_t v = 0;
	s_pending = 0;
	switch (s_code & 0x0F) {
	case 0: v = vram_word(s_addr); break;
	case 4: v = ((s_addr >> 1) & 0x3F) < 40 ? s_vsram[(s_addr >> 1) & 0x3F] : 0; break;
	case 8: v = s_cram[(s_addr >> 1) & 0x3F]; break;
	default: break;
	}
	s_addr = (s_addr + AUTOINC) & 0xFFFF;
	return v;
}

static int hpos480(void) { return s_cyc_line ? s_cyc * 480 / s_cyc_line : 0; }

static uint16_t status_read(void)
{
	s_pending = 0;
	const int hp = hpos480();
	const int hblank = hp > 400 && hp <= 460;
	const int vblank = s_vblank || !(R(1) & 0x40);
	return (uint16_t)(0x3400 | 0x0200 | (s_irq6_pending << 7) | (s_collision << 5) | ((s_imode & 1) ? (s_odd ^ 1) << 4 : 0) |
		(vblank << 3) | (hblank << 2));
}

static uint16_t hv_read(void)
{
	int v = s_line, h = hpos480();
	if (h > 460)
		v++;
	const int total = 262;
	v %= total;
	if (!(R(1) & 0x08)) // 224 lines: 00-EA then E5-FF (MAME vc_ntsc_224)
		v = v <= 0xEA ? v : v - 0xEB + 0xE5;
	else
		v = v <= 0xFF ? v : v - 0x100 + 0x00; // 240 lines are PAL only; keep it simple
	if (h > 0xF7)
		h -= 0x49;
	return (uint16_t)(((v & 0xFF) << 8) | (h & 0xFF));
}

uint16_t c2vdp_read(uint32_t offset)
{
	switch (offset & 0x1E) {
	case 0x00: case 0x02: return data_read();
	case 0x04: case 0x06: return status_read();
	case 0x08: case 0x0A: case 0x0C: case 0x0E: return hv_read();
	default: return 0xFFFF;
	}
}

void c2vdp_write(uint32_t offset, uint16_t data)
{
	switch (offset & 0x1E) {
	case 0x00: case 0x02: data_write(data); break;
	case 0x04: case 0x06: ctrl_write(data); break;
	case 0x10: case 0x12: case 0x14: case 0x16: if (s_bus.psg_write) s_bus.psg_write((uint8_t)data); break;
	default: break;
	}
}

void c2vdp_set_line_cycle(int cyc, int cycles_per_line)
{
	s_cyc = cyc;
	s_cyc_line = cycles_per_line;
}

// MAME megadriv_scanline_timer_callback_alt_timing + vdp_handle_scanline_callback (C2 has 262 lines, NTSC)
void c2vdp_line_start(int line)
{
	s_line = line;
	s_cyc = 0;
	if (line == 0) { // vdp_handle_eof
		s_vblank = 0;
		s_collision = 0;
		s_imode = (R(12) >> 1) & 3;
		s_odd ^= 1;
		s_visible = (R(1) & 0x08) ? 240 : 224;
	}
	if (line == s_visible) {
		s_irq6_pending = 1;
		s_vblank = 1;
		s_hold6 = 1; // C2: the VDP's Z80 interrupt line drives 68000 IRQ 6 (HOLD), whatever R1 says
	}
	if (line <= 224) {
		if (--s_irq4_counter == -1) {
			s_irq4_counter = R(10);
			s_irq4_pending = 1;
			if (R(0) & 0x10)
				s_hold4 = 1;
		}
	} else
		s_irq4_counter = R(10);
}

// ---- rendering ----
// Per-pixel layer words: bits 8-10 = priority rank (0 = transparent; B low 1, A low 2, S low 3, B high 4, A high 5,
// S high 6), bit 7 = the tile's priority bit (kept on transparent pixels for shadow / highlight), bits 0-5 = colour.
// The winning pixel is then just the largest word.
static uint16_t s_la[320 + 16], s_lb[320 + 16], s_ls[320 + 16];

static int plane_cells(int bits) { return bits == 0 ? 32 : bits == 1 ? 64 : bits == 3 ? 128 : 32; }

static const uint8_t *tile_pixels(int t)
{
	uint32_t *d = &s_tdirty[t >> 5];
	const uint32_t bit = 1u << (t & 31);
	uint8_t *o = &s_tdec[t * 64];
	if (*d & bit) {
		*d &= ~bit;
		const uint8_t *p = &s_vram[t << 5];
		for (int k = 0; k < 32; k++) {
			o[2 * k] = p[k] >> 4;
			o[2 * k + 1] = p[k] & 0x0F;
		}
	}
	return o;
}

// Decode one tile row (8 pixels) of name table entry e into out[0..7]
static inline void tile_row(uint16_t e, int row, uint16_t *out, int rlo, int rhi)
{
	if (e & 0x1000)
		row = 7 - row;
	const uint8_t *src = tile_pixels(e & 0x7FF) + (row << 3);
	const int pri = (e & 0x8000) != 0;
	const uint16_t clear = (uint16_t)(pri ? 0x80 : 0);
	const uint16_t opaque = (uint16_t)(((pri ? rhi : rlo) << 8) | clear | ((e >> 9) & 0x30));
#ifdef C2VDP_NEON
	uint8x8_t v = vld1_u8(src);
	if (e & 0x0800)
		v = vrev64_u8(v);
	const uint16x8_t w = vmovl_u8(v);
	const uint16x8_t m = vceqq_u16(w, vdupq_n_u16(0));
	vst1q_u16(out, vbslq_u16(m, vdupq_n_u16(clear), vorrq_u16(w, vdupq_n_u16(opaque))));
#else
	if (e & 0x0800)
		for (int k = 0; k < 8; k++) {
			const int c = src[7 - k];
			out[k] = c ? (uint16_t)(opaque | c) : clear;
		}
	else
		for (int k = 0; k < 8; k++) {
			const int c = src[k];
			out[k] = c ? (uint16_t)(opaque | c) : clear;
		}
#endif
}

// Plane line, a tile (or a 16-pixel vertical scroll column) at a time
static void draw_plane(uint16_t *out, uint32_t base, int hscroll, const int *vscroll_col, int w, int line, int pw, int ph,
	int rlo, int rhi)
{
	const int wmask = pw * 8 - 1, hmask = ph * 8 - 1;
	int x = 0;
	while (x < w) {
		const int py = (line + vscroll_col[x >> 4]) & hmask;
		const int px = (x - hscroll) & wmask;
		int n = 8 - (px & 7);
		const int to_col = 16 - (x & 15);
		if (n > to_col) n = to_col;
		if (n > w - x) n = w - x;
		const uint16_t e = vram_word(base + (((py >> 3) * pw + (px >> 3)) << 1));
		if (n == 8) {
			tile_row(e, py & 7, out + x, rlo, rhi);
		} else {
			uint16_t t[8];
			tile_row(e, py & 7, t, rlo, rhi);
			const int o = px & 7;
			for (int k = 0; k < n; k++)
				out[x + k] = t[o + k];
		}
		x += n;
	}
}

static void draw_window(uint16_t *out, int x0, int x1, int line)
{
	const int pw = H40 ? 64 : 32;
	const uint32_t base = H40 ? ((R(3) & 0x3C) << 10) : ((R(3) & 0x3E) << 10);
	int x = x0;
	while (x < x1) {
		uint16_t t[8];
		tile_row(vram_word(base + (((line >> 3) * pw + (x >> 3)) << 1)), line & 7, t, 2, 5);
		const int o = x & 7;
		int n = 8 - o;
		if (n > x1 - x) n = x1 - x;
		for (int k = 0; k < n; k++)
			out[x + k] = t[o + k];
		x += n;
	}
}

// returns 0 when no sprite pixel was drawn on the line (s_ls is then not cleared)
static int draw_sprites(int line, int w)
{
	const int h40 = H40;
	const uint32_t base = h40 ? ((R(5) & 0x7E) << 9) : ((R(5) & 0x7F) << 9);
	const int max_total = h40 ? 80 : 64, max_line = h40 ? 20 : 16, max_px = h40 ? 320 : 256;
	int link = 0, n = 0, on_line = 0, px_used = 0, masked = 0, nonzero_seen = 0, drawn = 0;
	do {
		const uint32_t a = base + ((uint32_t)link << 3);
		const int y = (vram_word(a) & 0x3FF) - 128;
		const int size = s_vram[(a + 2) & 0xFFFF];
		link = s_vram[(a + 3) & 0xFFFF] & 0x7F;
		const int hs = ((size >> 2) & 3) + 1, vs = (size & 3) + 1;
		if (line >= y && line < y + vs * 8) {
			if (++on_line > max_line)
				break;
			const uint16_t attr = vram_word(a + 4);
			const int xraw = vram_word(a + 6) & 0x1FF;
			if (xraw == 0) {
				if (nonzero_seen)
					masked = 1; // x = 0 masks the sprites after it on this line
			} else
				nonzero_seen = 1;
			int row = line - y;
			if (attr & 0x1000) row = vs * 8 - 1 - row;
			const int x0 = xraw - 128;
			const uint16_t hi = (uint16_t)((((attr & 0x8000) ? 6 : 3) << 8) | ((attr >> 9) & 0x30));
			const int hflip = (attr & 0x0800) != 0;
			for (int cx = 0; cx < hs; cx++) {
				// the line's pixel budget counts every sprite pixel, drawn or not
				int np = max_px - px_used;
				if (np <= 0)
					goto done;
				if (np > 8)
					np = 8;
				px_used += np;
				const int sx0 = x0 + cx * 8;
				if (masked || sx0 >= w || sx0 + 8 <= 0)
					continue;
				const int tcx = hflip ? hs - 1 - cx : cx;
				const uint8_t *src = tile_pixels(((attr & 0x7FF) + tcx * vs + (row >> 3)) & 0x7FF) + ((row & 7) << 3);
				for (int p = 0; p < np; p++) {
					const int c = src[hflip ? 7 - p : p];
					const int sx = sx0 + p;
					if (!c || sx < 0 || sx >= w)
						continue;
					if (!drawn) {
						memset(s_ls, 0, sizeof(uint16_t) * (size_t)w);
						drawn = 1;
					}
					if (s_ls[sx] & 0x3F) {
						s_collision = 1;
						continue;
					}
					s_ls[sx] = (uint16_t)(hi | c);
				}
			}
		}
	} while (link && ++n < max_total);
done:
	return drawn;
}

extern uint64_t (*c2_clock_us)(void);
unsigned c2vdp_prof_us[4]; // planes, window, sprites, compose (only when c2vdp_prof_on)
int c2vdp_prof_on;
void c2vdp_render_line(int line, uint16_t *idx)
{
	uint64_t pt0 = 0, pt1 = 0, pt2 = 0, pt3 = 0;
	const int w = c2vdp_width();
	const uint8_t bd = R(7) & 0x3F;
	if (!(R(1) & 0x40) || line >= s_visible) {
		for (int x = 0; x < w; x++)
			idx[x] = (uint16_t)(C2VDP_IDX_RAW | C2VDP_BG | bd);
		return;
	}
	const int pw = plane_cells(R(16) & 3), ph = plane_cells((R(16) >> 4) & 3);
	// horizontal scroll
	const uint32_t hsbase = (R(13) & 0x3F) << 10;
	int hoff = 0;
	switch (R(11) & 3) {
	case 2: hoff = (line & ~7) * 4; break;
	case 3: hoff = line * 4; break;
	default: break;
	}
	const int hsa = vram_word(hsbase + hoff) & 0x3FF, hsb = vram_word(hsbase + hoff + 2) & 0x3FF;
	// vertical scroll (full or per 2-cell column)
	int vsa[21], vsb[21];
	for (int c = 0; c < 21; c++) {
		if (R(11) & 4) {
			const int k = c < 20 ? c : 19;
			vsa[c] = s_vsram[k * 2] & 0x3FF;
			vsb[c] = s_vsram[k * 2 + 1] & 0x3FF;
		} else {
			vsa[c] = s_vsram[0] & 0x3FF;
			vsb[c] = s_vsram[1] & 0x3FF;
		}
	}
	if (c2vdp_prof_on) pt0 = c2_clock_us();
	draw_plane(s_lb, (R(4) & 7) << 13, hsb, vsb, w, line, pw, ph, 1, 4);
	draw_plane(s_la, (R(2) & 0x38) << 10, hsa, vsa, w, line, pw, ph, 2, 5);
	if (c2vdp_prof_on) pt1 = c2_clock_us();
	// window replaces plane A where it covers the line / columns
	{
		const int wv = (R(18) & 0x1F) * 8, down = R(18) & 0x80;
		const int wh = (R(17) & 0x1F) * 16, right = R(17) & 0x80;
		int x0 = 0, x1 = 0;
		if (down ? line >= wv : line < wv) {
			x0 = 0;
			x1 = w;
		} else if (right) {
			x0 = wh < w ? wh : w;
			x1 = w;
		} else {
			x0 = 0;
			x1 = wh < w ? wh : w;
		}
		if (x1 > x0)
			draw_window(s_la, x0, x1, line);
	}
	if (c2vdp_prof_on) pt2 = c2_clock_us();
	const int spr = draw_sprites(line, w);
	if (c2vdp_prof_on) {
		pt3 = c2_clock_us();
		c2vdp_prof_us[0] += (unsigned)(pt1 - pt0);
		c2vdp_prof_us[1] += (unsigned)(pt2 - pt1);
		c2vdp_prof_us[2] += (unsigned)(pt3 - pt2);
	}
	if (!(R(12) & 0x08)) { // no shadow / highlight: the largest layer word wins -> index rank * 64 + colour
		// the backdrop word sits above the transparent words (0 / 0x80) and below every opaque one (rank >= 1)
		const uint16_t back = (uint16_t)(0xC0 | bd);
		uint16_t *restrict o = idx;
		const uint16_t *restrict la = s_la, *restrict lb = s_lb, *restrict ls = s_ls;
		if (!spr) {
			for (int x = 0; x < w; x++) {
				uint16_t m = la[x] > lb[x] ? la[x] : lb[x];
				m = m > back ? m : back;
				o[x] = (uint16_t)(((m >> 2) & 0x1C0) | (m & 0x3F));
			}
		} else {
			for (int x = 0; x < w; x++) {
				uint16_t m = la[x] > lb[x] ? la[x] : lb[x];
				const uint16_t t = ls[x] > back ? ls[x] : back;
				m = m > t ? m : t;
				o[x] = (uint16_t)(((m >> 2) & 0x1C0) | (m & 0x3F));
			}
		}
		return;
	}
	if (!spr)
		memset(s_ls, 0, sizeof(uint16_t) * (size_t)w);
	for (int x = 0; x < w; x++) {
		const uint16_t a = s_la[x], b = s_lb[x], s = s_ls[x];
		const uint16_t ab = a > b ? a : b;
		int c = (ab >> 8) ? (ab & 0x3F) : bd, kind = C2VDP_BG;
		// shadow / highlight (Mega Drive rules): planes without a high-priority tile are shadowed
		int level = ((a | b) & 0x80) ? 1 : 0; // 0 shadow, 1 normal, 2 highlight
		if ((s >> 8) && s > ab) {
			const int sc = s & 0x3F;
			if (sc == 0x3E) // palette 3 colour 14: highlight operator
				level++;
			else if (sc == 0x3F) // palette 3 colour 15: shadow operator
				level = 0;
			else {
				c = sc;
				kind = C2VDP_SPRITE;
				if ((s >> 8) == 6 || (sc & 0x0F) == 0x0E)
					level = 1;
			}
		}
		if (level > 2) level = 2;
		if (level == 0) kind = C2VDP_SHADOW;
		else if (level == 2) kind = C2VDP_HIGHLIGHT;
		else if (kind != C2VDP_SPRITE) kind = C2VDP_BG;
		idx[x] = (uint16_t)(C2VDP_IDX_RAW | kind | c);
	}
}

#define ST_(x) do { if (save) { if (fwrite(&(x), sizeof(x), 1, fp) != 1) return -1; } \
	else if (fread(&(x), sizeof(x), 1, fp) != 1) return -1; } while (0)
int c2vdp_state(FILE *fp, int save)
{
	ST_(s_vram); ST_(s_vsram); ST_(s_cram); ST_(s_reg);
	ST_(s_pending); ST_(s_code); ST_(s_addr); ST_(s_fill_pending);
	ST_(s_line); ST_(s_irq4_counter); ST_(s_irq4_pending); ST_(s_irq6_pending); ST_(s_vblank);
	ST_(s_hold4); ST_(s_hold6); ST_(s_collision); ST_(s_imode); ST_(s_odd); ST_(s_visible);
	if (!save)
		memset(s_tdirty, 0xFF, sizeof(s_tdirty));
	return 0;
}
#undef ST_
