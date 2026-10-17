/*  Sega System C / C2 arcade board (ADR-107).
    Memory map, I/O chip, protection, palette banking and interrupt wiring follow MAME src/mame/sega/segac2.cpp
    and src/mame/sega/315_5296.cpp (BSD-3-Clause, David Haywood, Aaron Giles). The 68000 is the Musashi core
    already used for the ST-V sound CPU.
    This file is part of ReST-V. GPL-2.0-or-later.
*/
#include "c2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "m68kcore.h"
#include "m68kmusashi.h"
#include "musashi/m68k.h"
#include "rstv_zip.h"
#include "c2_vdp.h"
#include "c2_sound.h"
#include "c2_prot.h"

extern void (*rstv_m68k_int_hook)(int level); // musashi/m68kcpu.h

// ---- set table ----
enum { C2_END, C2_BYTE16, C2_PLAIN, C2_UPD, C2_COPY };
#define C2F_NOPROTW   1 // tfrceacb: protection chip writes ignored
#define C2F_ICHIRJBL  2 // ichirjbl: 0x840108 reads 0x00F5
#define C2F_PCLUB     4 // Print Club camera / printer ports
#define C2F_DDR_F     8 // tfrceacjpb: I/O direction override 0x0F
#define C2F_UPD_DELAY 16 // ribbit: uPD7759 start delay 250
typedef struct { int kind; const char *file; uint32_t offset, length, crc; } C2Rom;
typedef struct {
	const char *set, *parent, *title;
	int is_c2, prot, flags, dsw;
	C2Rom roms[8];
} C2Game;
static const C2Game s_games[] = {
#include "c2_games.h"
};
#define NGAMES ((int)(sizeof(s_games) / sizeof(s_games[0])))

static const C2Game *find(const char *set)
{
	for (int i = 0; i < NGAMES; i++)
		if (!strcmp(s_games[i].set, set))
			return &s_games[i];
	return NULL;
}
int c2_is_game(const char *romset) { return romset && find(romset) != NULL; }
const char *c2_game_title(const char *romset) { const C2Game *g = find(romset); return g ? g->title : NULL; }

// ---- board state ----
#define LINES 262
#define CYC_LINE 570      // 68000 at XL2/6, line = 3420 XL2 clocks
#define XL2_LINE 3420
static const C2Game *s_g;
static uint8_t *s_rom;    // 2 MB, big-endian bytes
static uint8_t *s_upd;
static uint32_t s_upd_size;
static uint8_t s_ram[0x10000]; // work RAM / NVRAM, big-endian bytes
static uint16_t s_palram[0x800];
static uint32_t s_pen[0x1800]; // normal, shadow (+0x800), highlight (+0x1000)
static uint32_t s_lut[C2VDP_IDX_N]; // VDP line index (c2_vdp.h C2VDP_IDX_*) -> RGBA for the current palette banking
static int s_lut_dirty = 1;
static int s_bg_lookup[4], s_sp_lookup[4];
static int s_palbank, s_bg_palbase, s_sp_palbase, s_alt_pal, s_display;
static int s_prot_write, s_prot_read;
static uint8_t s_io_latch[8], s_io_dir, s_io_cnt;
static uint16_t s_cam;
static uint8_t s_in_p1 = 0xFF, s_in_p2 = 0xFF, s_in_sys = 0xFF;
static int s_line;
static uint32_t s_fb[320 * 240];
static int s_fb_w = 320, s_fb_h = 224;
static int s_ym_irq;
static int s_debt; // 68000 cycles run past the end of the previous line
uint64_t (*c2_clock_us)(void);  // frontend clock for the section times (NULL = off)
unsigned c2_prof_us[4];         // accumulated: 68000, VDP render, sound, rest

// ---- palette (MAME palette_w / recompute_palette_tables) ----
static uint32_t rgba(int r5, int g5, int b5)
{
	const int r = (r5 << 3) | (r5 >> 2), g = (g5 << 3) | (g5 >> 2), b = (b5 << 3) | (b5 >> 2);
	return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void pen_update(int off)
{
	const int w = s_palram[off];
	const int r = ((w << 1) & 0x1e) | ((w >> 12) & 1), g = ((w >> 3) & 0x1e) | ((w >> 13) & 1), b = ((w >> 7) & 0x1e) | ((w >> 14) & 1);
	s_pen[off] = rgba(r, g, b);
	s_pen[off + 0x800] = rgba(r >> 1, g >> 1, b >> 1);
	s_pen[off + 0x1000] = rgba((r >> 1) | 0x10, (g >> 1) | 0x10, (b >> 1) | 0x10);
	s_lut_dirty = 1;
}

static void lut_build(void)
{
	uint32_t *raw = s_lut + C2VDP_IDX_RAW;
	for (int v = 0; v < 256; v++) {
		const int c = v & 0x0F, p = (v >> 4) & 3;
		switch (v & 0xC0) {
		case C2VDP_SHADOW: raw[v] = s_pen[(c | s_bg_lookup[p]) + 0x800]; break;
		case C2VDP_BG: raw[v] = s_pen[c | s_bg_lookup[p]]; break;
		case C2VDP_SPRITE: raw[v] = s_pen[c | s_sp_lookup[p]]; break;
		default: raw[v] = s_pen[(c | s_bg_lookup[p]) + 0x1000]; break;
		}
	}
	for (int r = 0; r < 8; r++) // priority ranks 3 and 6 are sprites, the rest plane / backdrop pixels
		for (int c = 0; c < 64; c++)
			s_lut[r * 64 + c] = raw[((r == 3 || r == 6) ? C2VDP_SPRITE : C2VDP_BG) | c];
	s_lut_dirty = 0;
}

static void recompute_palette_tables(void)
{
	s_lut_dirty = 1;
	for (int i = 0; i < 4; i++) {
		const int bgpal = 0x000 + s_bg_palbase * 0x40 + i * 0x10;
		const int sppal = 0x100 + s_sp_palbase * 0x40 + i * 0x10;
		if (!s_alt_pal) {
			s_bg_lookup[i] = 0x200 * s_palbank + bgpal;
			s_sp_lookup[i] = 0x200 * s_palbank + sppal;
		} else {
			s_bg_lookup[i] = 0x200 * s_palbank + ((bgpal << 1) & 0x180) + ((~bgpal >> 2) & 0x40) + (bgpal & 0x30);
			s_sp_lookup[i] = 0x200 * s_palbank + ((~sppal << 2) & 0x100) + ((sppal << 2) & 0x80) + ((~sppal >> 2) & 0x40) +
				((sppal >> 2) & 0x20) + (sppal & 0x10);
		}
	}
}

static int pal_offset(int offset)
{
	offset &= 0x1ff;
	if (s_alt_pal)
		offset = ((offset << 1) & 0x100) | ((offset << 2) & 0x80) | ((~offset >> 2) & 0x40) | ((offset >> 1) & 0x20) | (offset & 0x1f);
	return offset + s_palbank * 0x200;
}

static uint16_t palette_r(int offset) { return s_palram[pal_offset(offset)]; }

static void palette_w(int offset, uint16_t data, uint16_t mask)
{
	const int o = pal_offset(offset);
	s_palram[o] = (uint16_t)((s_palram[o] & ~mask) | (data & mask));
	pen_update(o);
}

// ---- protection / control (MAME prot_r / prot_w / control_w) ----
static uint8_t prot_r(void) { return (uint8_t)(s_prot_read | 0xF0); }

static void prot_w(uint8_t data)
{
	if (s_g->flags & C2F_NOPROTW)
		return;
	const int new_sp = (data >> 2) & 3, new_bg = data & 3;
	const int index = (s_prot_write << 4) | s_prot_read;
	s_prot_write = data & 0x0F;
	s_prot_read = c2prot_funcs[s_g->prot](index);
	if (new_sp != s_sp_palbase || new_bg != s_bg_palbase) {
		s_sp_palbase = new_sp;
		s_bg_palbase = new_bg;
		recompute_palette_tables();
	}
}

static void control_w(uint8_t data)
{
	data &= 0x0F;
	s_display = ~data & 1;
	if (!(data & 2))
		s_prot_write = s_prot_read = 0;
	s_alt_pal = (~data & 4) >> 2;
	recompute_palette_tables();
}

// ---- 315-5296 I/O ----
static uint8_t io_port_in(int port)
{
	switch (port) {
	case 0: return s_in_p1;
	case 1: return s_in_p2;
	case 2: return (uint8_t)(0xBF | (s_upd ? (c2snd_upd_busy() ? 0x40 : 0) : 0x40)); // D6 = uPD7759 /BUSY
	case 4: return s_in_sys;
	case 5: return 0xFF;                       // coinage: 1 coin 1 credit
	case 6: return (uint8_t)s_g->dsw;
	default: return 0xFF;
	}
}

static void io_port_out(int port, uint8_t data)
{
	if (port == 7) { // H: palette bank, uPD7759 sample bank
		const int nb = data & 3;
		if (nb != s_palbank) {
			s_palbank = nb;
			recompute_palette_tables();
		}
		const int banks = (int)(s_upd_size / 0x20000);
		if (banks > 1)
			c2snd_upd_set_bank((data >> 2) & (banks - 1));
	}
}

static uint8_t io_r(int offset)
{
	offset &= 0x3F;
	const uint8_t dir = (uint8_t)(s_io_dir & ((s_g->flags & C2F_DDR_F) ? 0x0F : 0xFF));
	if (offset < 8)
		return (dir >> offset) & 1 ? s_io_latch[offset] : io_port_in(offset);
	switch (offset) {
	case 0x8: return 'S';
	case 0x9: return 'E';
	case 0xA: return 'G';
	case 0xB: return 'A';
	case 0xC: case 0xE: return s_io_cnt;
	case 0xD: case 0xF: return s_io_dir;
	default: return 0xFF;
	}
}

static void io_w(int offset, uint8_t data)
{
	offset &= 0x3F;
	if (offset < 8) {
		if ((s_io_dir >> offset) & 1)
			io_port_out(offset, data);
		s_io_latch[offset] = data;
	} else if (offset == 0xE) {
		if (s_g->is_c2)
			c2snd_upd_reset((data >> 1) & 1); // CNT1 -> uPD7759 /RESET
		s_io_cnt = data;
	} else if (offset == 0xF) {
		for (int i = 0; i < 8; i++)
			if (((s_io_dir ^ data) >> i) & 1)
				io_port_out(i, ((data >> i) & 1) ? s_io_latch[i] : 0);
		s_io_dir = data;
	}
}

// ---- 68000 bus ----
static void update_irq(void)
{
	int level = 0;
	if (c2vdp_irq6())
		level = 6;
	else if (c2vdp_irq4())
		level = 4;
	else if (s_ym_irq)
		level = 2;
	m68k_set_irq((unsigned)level);
}

static void int_taken(int level) // Musashi cleared its level: drop the HOLD source, re-raise what is still pending
{
	c2vdp_ack(level);
	update_irq();
}

static uint8_t board_r8(uint32_t a)
{
	const int sel = (a >> 18) & 3;
	switch (sel) {
	case 0: return (a & 0x200) ? 0xFF : prot_r();
	case 1:
		if ((s_g->flags & C2F_ICHIRJBL) && (a & 0xFFFFFE) == 0x840108)
			return 0xF5;
		return (a & 0x100) ? c2snd_ym_read((a >> 1) & 3) : io_r((a >> 1) & 0x0F);
	case 2:
		if ((s_g->flags & C2F_PCLUB) && ((a & 0xFFFFFE) == 0x880120 || (a & 0xFFFFFE) == 0x880124))
			return (uint8_t)s_cam;
		return 0xFF;
	default: return (uint8_t)palette_r((a >> 1) & 0x7FF);
	}
}

static void board_w8(uint32_t a, uint8_t d)
{
	const int sel = (a >> 18) & 3;
	switch (sel) {
	case 0:
		if (a & 0x200)
			control_w(d);
		else
			prot_w(d);
		break;
	case 1:
		if (a & 0x100)
			c2snd_ym_write((a >> 1) & 3, d);
		else
			io_w((a >> 1) & 0x0F, d);
		break;
	case 2:
		if (a & 0x100) {
			if ((s_g->flags & C2F_PCLUB) && (a & 0xFFFFFE) == 0x880124)
				s_cam = d;
			// counter / timer: not needed
		} else if (s_g->is_c2) {
			c2snd_upd_port(d); // MAME segac2_upd7759_w: port, then a start pulse
			c2snd_upd_start(0);
			c2snd_upd_start(1);
		}
		break;
	default: palette_w((a >> 1) & 0x7FF, d, 0x00FF); break;
	}
}

#define IS_VDP(a) (((a) & 0xE700E0) == 0xC00000)

static void vdp_sync(void) { c2vdp_set_line_cycle(s_debt + m68k_cycles_run(), CYC_LINE); }

static u32 FASTCALL bus_r16(const u32 adr)
{
	const uint32_t a = adr & 0xFFFFFE;
	if (a < 0x200000)
		return (u32)(s_rom[a] << 8 | s_rom[a + 1]);
	if (a >= 0xE00000)
		return (u32)(s_ram[a & 0xFFFF] << 8 | s_ram[(a & 0xFFFF) + 1]);
	if (IS_VDP(a)) {
		vdp_sync();
		return c2vdp_read(a & 0x1F);
	}
	if (a >= 0x800000 && a < 0xA00000) {
		if (((a >> 18) & 3) == 3)
			return palette_r((a >> 1) & 0x7FF);
		return 0xFF00u | board_r8(a | 1);
	}
	return 0xFFFF;
}

static u32 FASTCALL bus_r8(const u32 adr)
{
	const uint32_t a = adr & 0xFFFFFF;
	if (a < 0x200000)
		return s_rom[a];
	if (a >= 0xE00000)
		return s_ram[a & 0xFFFF];
	if (IS_VDP(a)) {
		const u32 w = bus_r16(a);
		return (a & 1) ? (w & 0xFF) : (w >> 8);
	}
	if (a >= 0x800000 && a < 0xA00000) {
		if (((a >> 18) & 3) == 3) {
			const uint16_t w = palette_r((a >> 1) & 0x7FF);
			return (a & 1) ? (w & 0xFF) : (w >> 8);
		}
		return (a & 1) ? board_r8(a) : 0xFF;
	}
	return 0xFF;
}

static void FASTCALL bus_w16(const u32 adr, u32 data)
{
	const uint32_t a = adr & 0xFFFFFE;
	if (a >= 0xE00000) {
		s_ram[a & 0xFFFF] = (uint8_t)(data >> 8);
		s_ram[(a & 0xFFFF) + 1] = (uint8_t)data;
		return;
	}
	if (IS_VDP(a)) {
		vdp_sync();
		c2vdp_write(a & 0x1F, (uint16_t)data);
		return;
	}
	if (a >= 0x800000 && a < 0xA00000) {
		if (((a >> 18) & 3) == 3)
			palette_w((a >> 1) & 0x7FF, (uint16_t)data, 0xFFFF);
		else
			board_w8(a | 1, (uint8_t)data);
	}
}

static void FASTCALL bus_w8(const u32 adr, u32 data)
{
	const uint32_t a = adr & 0xFFFFFF;
	data &= 0xFF;
	if (a >= 0xE00000) {
		s_ram[a & 0xFFFF] = (uint8_t)data;
		return;
	}
	if (IS_VDP(a)) { // byte writes reach both halves of the VDP bus
		vdp_sync();
		c2vdp_write(a & 0x1F, (uint16_t)(data | (data << 8)));
		return;
	}
	if (a >= 0x800000 && a < 0xA00000) {
		if (((a >> 18) & 3) == 3)
			palette_w((a >> 1) & 0x7FF, (uint16_t)(data | (data << 8)), (a & 1) ? 0x00FF : 0xFF00);
		else if (a & 1)
			board_w8(a, (uint8_t)data);
	}
}

static uint16_t vdp_dma_read(uint32_t a) { return (uint16_t)bus_r16(a); }

// ---- loading ----
int c2_load(const char *rom_dir, const char *romset, char *err, int errlen)
{
	const C2Game *g = find(romset);
	if (!g) {
		snprintf(err, errlen, "unsupported set %s", romset);
		return -1;
	}
	c2_unload();
	s_rom = (uint8_t *)malloc(0x200000);
	if (!s_rom) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	memset(s_rom, 0xFF, 0x200000);
	char own[512], par[512];
	snprintf(own, sizeof(own), "%s/%s.zip", rom_dir, g->set);
	snprintf(par, sizeof(par), "%s/%s.zip", rom_dir, g->parent ? g->parent : g->set);
	for (const C2Rom *r = g->roms; r->kind != C2_END; r++) {
		if (r->kind == C2_COPY) { // ROM_COPY("maincpu", src = crc field, dst = offset, length)
			memmove(s_rom + r->offset, s_rom + r->crc, r->length);
			continue;
		}
		u32 size = 0;
		uint8_t *d = rstv_zip_load(own, r->file, r->crc, &size);
		if (!d && g->parent)
			d = rstv_zip_load(par, r->file, r->crc, &size);
		if (!d) {
			snprintf(err, errlen, "%s: missing %s", romset, r->file);
			c2_unload();
			return -1;
		}
		const uint32_t n = size < r->length ? size : r->length;
		if (r->kind == C2_BYTE16) {
			for (uint32_t j = 0; j < n && r->offset + 2 * j < 0x200000; j++)
				s_rom[r->offset + 2 * j] = d[j];
		} else if (r->kind == C2_PLAIN) {
			for (uint32_t j = 0; j < n && r->offset + j < 0x200000; j++)
				s_rom[r->offset + j] = d[j];
		} else { // C2_UPD
			const uint32_t end = r->offset + r->length;
			if (end > s_upd_size) {
				uint8_t *nu = (uint8_t *)realloc(s_upd, end);
				if (!nu) { free(d); c2_unload(); snprintf(err, errlen, "out of memory"); return -1; }
				memset(nu + s_upd_size, 0, end - s_upd_size);
				s_upd = nu;
				s_upd_size = end;
			}
			memcpy(s_upd + r->offset, d, n);
		}
		free(d);
	}
	s_g = g;
	static const C2VdpBus bus = { vdp_dma_read, c2snd_sn_write };
	c2vdp_init(&bus);
	c2snd_init();
	c2snd_upd_set_rom(g->is_c2 ? s_upd : NULL, g->is_c2 ? s_upd_size : 0);
	if (g->flags & C2F_UPD_DELAY)
		c2snd_upd_set_start_delay(250);
	M68KMusashi.Init();
	M68KMusashi.SetReadB(bus_r8);
	M68KMusashi.SetReadW(bus_r16);
	M68KMusashi.SetWriteB(bus_w8);
	M68KMusashi.SetWriteW(bus_w16);
	rstv_m68k_int_hook = int_taken;
	memset(s_ram, 0xFF, sizeof(s_ram)); // NVRAM DEFAULT_ALL_1 (borencha needs the 0xFF fill)
	c2_reset();
	return 0;
}

void c2_unload(void)
{
	if (s_g)
		c2snd_sync(); // the sound worker must be idle
	free(s_rom);
	free(s_upd);
	s_rom = s_upd = NULL;
	s_upd_size = 0;
	s_g = NULL;
	rstv_m68k_int_hook = NULL;
}

void c2_reset(void)
{
	memset(s_palram, 0, sizeof(s_palram));
	for (int i = 0; i < 0x800; i++)
		pen_update(i);
	s_palbank = s_bg_palbase = s_sp_palbase = s_alt_pal = 0;
	s_display = 1;
	s_prot_write = s_prot_read = 0;
	memset(s_io_latch, 0, sizeof(s_io_latch));
	s_io_dir = s_io_cnt = 0;
	s_cam = 0;
	s_ym_irq = 0;
	s_debt = 0;
	recompute_palette_tables();
	c2vdp_reset();
	c2snd_reset();
	m68k_set_irq(0);
	m68k_pulse_reset();
	s_line = 0;
}

void c2_run_frame(uint8_t p1, uint8_t p2, uint8_t sys)
{
	s_in_p1 = (uint8_t)~p1;
	s_in_p2 = (uint8_t)~p2;
	s_in_sys = (uint8_t)~sys;
	for (int line = 0; line < LINES; line++) {
		s_line = line;
		c2vdp_line_start(line);
		update_irq();
		const int vis = c2vdp_visible_lines();
		const uint64_t ta = c2_clock_us ? c2_clock_us() : 0;
		if (line < vis && line < 240) {
			uint16_t idx[320];
			const int w = c2vdp_width();
			c2vdp_render_line(line, idx);
			uint32_t *dst = s_fb + line * 320;
			if (!s_display) {
				for (int x = 0; x < w; x++)
					dst[x] = 0xFF000000u;
			} else {
				if (s_lut_dirty)
					lut_build();
				for (int x = 0; x < w; x++)
					dst[x] = s_lut[idx[x]];
			}
			s_fb_w = w;
			s_fb_h = vis < 240 ? vis : 240;
		}
		const uint64_t tb = c2_clock_us ? c2_clock_us() : 0;
		const int want = CYC_LINE - s_debt;
		s_debt = want > 0 ? m68k_execute(want) - want : -want;
		const uint64_t tc = c2_clock_us ? c2_clock_us() : 0;
		c2snd_advance(XL2_LINE);
		if (c2_clock_us) {
			const uint64_t td = c2_clock_us();
			c2_prof_us[1] += (unsigned)(tb - ta);
			c2_prof_us[0] += (unsigned)(tc - tb);
			c2_prof_us[2] += (unsigned)(td - tc);
		}
		const int yi = c2snd_ym_irq();
		if (yi != s_ym_irq) {
			s_ym_irq = yi;
			update_irq();
		}
	}
	c2snd_frame_end(); // ADR-107: YM sound of this frame goes to the worker, the previous frame is mixed
}

void c2_sound_sync(void) { c2snd_sync(); }

const uint32_t *c2_frame(int *w, int *h, int *pitch)
{
	*w = s_fb_w;
	*h = s_fb_h;
	*pitch = 320;
	return s_fb;
}

int c2_audio(int16_t *out, int max_frames) { return c2snd_fetch(out, max_frames); }

uint8_t *c2_nvram(int *size)
{
	*size = (int)sizeof(s_ram);
	return s_ram;
}

// ---- save state ----
#define C2_STATE_MAGIC 0x32435352u // "RSC2"
#define ST_(x) do { if (save) { if (fwrite(&(x), sizeof(x), 1, fp) != 1) return -1; } \
	else if (fread(&(x), sizeof(x), 1, fp) != 1) return -1; } while (0)
static int board_state(FILE *fp, int save)
{
	ST_(s_ram); ST_(s_palram); ST_(s_palbank); ST_(s_bg_palbase); ST_(s_sp_palbase); ST_(s_alt_pal); ST_(s_display);
	ST_(s_prot_write); ST_(s_prot_read); ST_(s_io_latch); ST_(s_io_dir); ST_(s_io_cnt); ST_(s_cam); ST_(s_ym_irq); ST_(s_debt);
	return 0;
}

static int state_io(const char *path, int save)
{
	if (!s_g)
		return -1;
	FILE *fp = fopen(path, save ? "wb" : "rb");
	if (!fp)
		return -1;
	uint32_t magic = C2_STATE_MAGIC, ctx_size = m68k_context_size();
	char set[16] = { 0 };
	strncpy(set, s_g->set, sizeof(set) - 1);
	int rc = 0;
	void *ctx = malloc(ctx_size);
	if (!ctx)
		rc = -1;
	if (!rc && save) {
		m68k_get_context(ctx);
		rc = (fwrite(&magic, 4, 1, fp) != 1 || fwrite(set, sizeof(set), 1, fp) != 1 || fwrite(&ctx_size, 4, 1, fp) != 1 ||
			fwrite(ctx, ctx_size, 1, fp) != 1) ? -1 : 0;
	} else if (!rc) {
		uint32_t m = 0, cs = 0;
		char s2[16];
		rc = (fread(&m, 4, 1, fp) != 1 || m != magic || fread(s2, sizeof(s2), 1, fp) != 1 || memcmp(s2, set, sizeof(set)) ||
			fread(&cs, 4, 1, fp) != 1 || cs != ctx_size || fread(ctx, ctx_size, 1, fp) != 1) ? -1 : 0;
		if (!rc)
			m68k_set_context(ctx);
	}
	free(ctx);
	if (!rc)
		rc = board_state(fp, save);
	if (!rc)
		rc = c2vdp_state(fp, save);
	if (!rc)
		rc = c2snd_state(fp, save);
	fclose(fp);
	if (!rc && !save) {
		for (int i = 0; i < 0x800; i++)
			pen_update(i);
		recompute_palette_tables();
		M68KMusashi.SetReadB(bus_r8); // the context restores Musashi's own state; our bus stays ours
		M68KMusashi.SetReadW(bus_r16);
		M68KMusashi.SetWriteB(bus_w8);
		M68KMusashi.SetWriteW(bus_w16);
		update_irq();
	}
	return rc;
}
#undef ST_

int c2_state_save(const char *path) { return state_io(path, 1); }
int c2_state_load(const char *path) { return state_io(path, 0); }
