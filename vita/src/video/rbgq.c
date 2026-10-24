// CPU side of the RBG0 GPU rotation renderer (ADR-016): map page decode cache, RGB555 character atlas, reference shading.
// Pattern name decode follows the same rules as vidsoft PatternAddr / nbgq.c cell_at.
#include "rbgq.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_MASK 0x7FFFF

static inline int rd16(const uint8_t *vram, uint32_t a)
{
	a &= VRAM_MASK & ~1u;
	return (vram[a] << 8) | vram[a + 1];
}

static uint32_t decode_key(const RstvRotCapture *c)
{
	return (uint32_t)c->colornumber | (uint32_t)c->patterndatasize << 3 | (uint32_t)c->patternwh << 5 |
		(uint32_t)c->auxmode << 7 | (uint32_t)(c->supplementdata & 0x3FF) << 8 | (uint32_t)c->charaddr_mask_3fff << 18 |
		(uint32_t)(c->specialprimode & 3) << 19 | (uint32_t)(c->priority & 7) << 21 | 1u << 31;
}

// decode one page (64x64 cells) into page atlas slot pi
static void decode_page(const RstvRotCapture *c, const uint8_t *vram, uint32_t base, uint8_t *pages, int pi, uint32_t *bank)
{
	bank[0] = bank[1] = bank[2] = bank[3] = 0;
	const int supp = c->supplementdata, cn = c->colornumber;
	// cell size 8 or 16: shifts (the Cortex-A9 has no divide instruction: two divisions a cell were most of the decode)
	const int csh = c->patternwh >= 2 ? 4 : 3;
	const int pagewh_sh = 9 - csh; // 512 / cellwh = 64 or 32
	const int pds = c->patterndatasize >= 2 ? 4 : 2;
	const int pr = c->priority & 7;
	const int ox = (pi & 7) * 64, oy = (pi >> 3) * 64;
	for (int cy = 0; cy < 64; cy++) {
		uint8_t *dst = pages + ((oy + cy) * 512 + ox) * 4;
		const uint32_t rowbase = base + (uint32_t)((((cy * 8) >> csh) << pagewh_sh) * pds);
		for (int cx = 0; cx < 64; cx++, dst += 4) {
			const int lx = cx * 8, ly = cy * 8;
			const uint32_t addr = rowbase + (uint32_t)((lx >> csh) * pds);
			int charaddr, paladdr, flip, sf, scf;
			if (pds == 2) {
				const int tmp = rd16(vram, addr);
				sf = (supp >> 9) & 1;
				scf = (supp >> 8) & 1;
				paladdr = cn == 0 ? (((tmp >> 12) & 15) * 16 + ((supp & 0xE0) << 3)) : (((tmp >> 12) & 7) * 256);
				if (!c->auxmode) {
					flip = (tmp >> 10) & 3;
					charaddr = c->patternwh < 2 ? ((tmp & 0x3FF) + ((supp & 0x1F) << 10))
					                            : ((tmp & 0x3FF) * 4 + ((supp & 0x3) | ((supp & 0x1C) << 10)));
				} else {
					flip = 0;
					charaddr = c->patternwh < 2 ? ((tmp & 0xFFF) + ((supp & 0x1C) << 10))
					                            : ((tmp & 0xFFF) * 4 + ((supp & 0x3) | ((supp & 0x10) << 10)));
				}
			} else {
				const int t1 = rd16(vram, addr), t2 = rd16(vram, addr + 2);
				charaddr = t2 & 0x7FFF;
				flip = t1 >> 14;
				paladdr = cn == 0 ? ((t1 & 0x7F) * 16) : (((t1 >> 4) & 7) * 256);
				sf = (t1 >> 13) & 1;
				scf = (t1 >> 12) & 1;
			}
			if (c->charaddr_mask_3fff)
				charaddr &= 0x3FFF;
			const int prio = (c->specialprimode == 1) ? ((pr & ~1) + sf) : pr;
			if (!prio) {
				dst[0] = dst[1] = dst[2] = dst[3] = 0;
				continue;
			}
			uint32_t unit = (uint32_t)charaddr;
			if (c->patternwh >= 2) {
				int qx = (lx & 15) >= 8, qy = (ly & 15) >= 8;
				if (flip & 1) qx = !qx;
				if (flip & 2) qy = !qy;
				// char size: 4bpp 32 bytes (1 unit), 8bpp 64 (2), RGB555 128 (4)
				unit += (uint32_t)(qy * 2 + qx) * (cn == 0 ? 1 : cn == 1 ? 2 : 4);
			}
			unit &= 0x3FFF;
			paladdr &= 2047;
			bank[(paladdr >> 4) >> 5] |= 1u << ((paladdr >> 4) & 31);
			dst[0] = (uint8_t)(unit & 0xFF);
			dst[1] = (uint8_t)(((unit >> 8) & 0x3F) | ((flip & 1) << 6) | ((flip & 2) << 6));
			dst[2] = (uint8_t)(paladdr & 0xFF);
			dst[3] = (uint8_t)(((paladdr >> 8) & 7) | (prio << 3) | (scf << 6) | 0x80);
		}
	}
}

int rbgq_prepare(RbgqState *st, const RstvRotCapture *c, const uint8_t *vram, const uint32_t *page_stamp, uint32_t serial,
	uint32_t frame, uint8_t *pages, uint8_t *table, int flat)
{
	if (flat && !st->cpu_pages && !(st->cpu_pages = (uint8_t *)calloc(512 * 512, 4)))
		flat = 0;
	if (flat != st->layout) {
		// drop the slot cache when the layout changes (decoded into different places)
		for (int k = 0; k < RBGQ_PAGES; k++)
			st->valid[k] = 0;
		for (int k = 0; k < 64; k++)
			st->flat_slot[k] = -1;
		st->layout = flat;
	}
	uint8_t *dpages = flat ? st->cpu_pages : pages;
	const uint32_t key = decode_key(c);
	const int names = c->patternwh >= 2 ? 1024 : 4096;
	const uint32_t bytes = (uint32_t)names * (c->patterndatasize >= 2 ? 4 : 2);
	const int mpw = c->mapw / 512, mph = c->maph / 512; // map size (pages)
	if (mpw < 1 || mph < 1 || mpw > 8 || mph > 8)
		return -1;
	memset(table, 0, 8 * 8 * 4);
	memset(st->bank_union, 0, sizeof(st->bank_union));
	for (int py = 0; py < mph; py++)
		for (int px = 0; px < mpw; px++) {
			const int plane = (py / c->planeh) * 4 + (px / c->planew);
			const int pin = (py % c->planeh) * c->planew + (px % c->planew);
			const uint32_t base = ((c->planetbl[plane & 15] & 0xFFFFFF) + (uint32_t)pin * bytes) & VRAM_MASK;
			// look up in cache
			int pi = -1;
			for (int k = 0; k < RBGQ_PAGES; k++)
				if (st->valid[k] && st->addr[k] == base && st->key[k] == key) {
					pi = k;
					break;
				}
			if (pi >= 0 && page_stamp) {
				// re-decode if this page's name area was written after decode (gvdp2 passes the time the content changed, ADR-057)
				for (uint32_t a = base >> 12; a <= ((base + bytes - 1) & VRAM_MASK) >> 12 && a < 128; a++)
					if (page_stamp[a] >= st->serial[pi]) {
						st->valid[pi] = 0;
						pi = -1;
						break;
					}
			} else if (pi >= 0 && !page_stamp) {
				st->valid[pi] = 0; // no write stamps: decode every time
				pi = -1;
			}
			if (pi < 0) {
				// empty slot, or the oldest slot not used this frame
				uint32_t best = 0xFFFFFFFFu;
				for (int k = 0; k < RBGQ_PAGES; k++) {
					if (!st->valid[k]) {
						pi = k;
						break;
					}
					if (st->used[k] != frame && st->used[k] < best) {
						best = st->used[k];
						pi = k;
					}
				}
				if (pi < 0)
					return -1;
				decode_page(c, vram, base, dpages, pi, st->pbank[pi]);
				st->slot_gen[pi]++;
				st->changed++;
				st->addr[pi] = base;
				st->key[pi] = key;
				st->serial[pi] = serial;
				st->valid[pi] = 1;
				st->decoded++;
			}
			st->used[pi] = frame;
			for (int w = 0; w < 4; w++)
				st->bank_union[w] |= st->pbank[pi][w];
			if (flat) {
				const int pos = py * 8 + px;
				if (st->flat_slot[pos] != pi || st->flat_gen[pos] != st->slot_gen[pi]) {
					// slot pi (64x64 cells) -> 64x64 cells at the map position (GPU memory: copy 256 bytes per row)
					for (int cy = 0; cy < 64; cy++)
						memcpy(pages + ((py * 64 + cy) * 512 + px * 64) * 4,
							st->cpu_pages + (((pi >> 3) * 64 + cy) * 512 + (pi & 7) * 64) * 4, 256);
					st->flat_slot[pos] = pi;
					st->flat_gen[pos] = st->slot_gen[pi];
					st->changed++; // texture content changed (layer reuse key, ADR-057)
				}
				continue;
			}
			uint8_t *t = table + (py * 8 + px) * 4;
			t[0] = (uint8_t)pi;
			t[1] = 255;
		}
	return 0;
}

void rbgq_atlas_rgb_update(uint8_t *argb, int pitch_px, const uint8_t *vram, const uint16_t *dirty)
{
	for (int p = 0; p < 128; p++) {
		const unsigned m = dirty ? dirty[p] : 0xFFFFu;
		if (!m)
			continue;
		// units touching each changed 256-byte block (ADR-137): a 128-byte char can start up to 3 units earlier
		for (int k = -3; k < 128; k++) {
			const int b = k < 0 ? 0 : k >> 3, bn = (k + 3) >> 3;
			if (!(m & (1u << b)) && !(bn < 16 && (m & (1u << bn))))
				continue;
			const uint32_t u = (uint32_t)(p * 128 + k) & 0x3FFF;
			const int ox = (int)(u & 127) * 8, oy = (int)(u >> 7) * 8;
			for (int r = 0; r < 8; r++) {
				// GPU memory (uncached): one 32-bit write per pixel
				uint32_t *d = (uint32_t *)(argb + ((oy + r) * pitch_px + ox) * 4);
				for (int x = 0; x < 8; x++) {
					const int v = rd16(vram, u * 32 + (uint32_t)(r * 8 + x) * 2);
					d[x] = (uint32_t)((v & 31) << 3) | (uint32_t)(((v >> 5) & 31) << 3) << 8 | (uint32_t)(((v >> 10) & 31) << 3) << 16 |
						((v & 0x8000) ? 0xFF000000u : 0);
				}
			}
		}
	}
}

uint32_t rbgq_shade(const RstvRotCapture *c, const uint8_t *pages, const uint8_t *table, const uint8_t *table_b, const uint8_t *a4,
	const uint8_t *a8, const uint8_t *argb, const uint8_t (*pal)[4], int i, int j, int flat)
{
	if (j >= c->nrows || !c->row[j].enable)
		return 0;
	const RstvRotLine *r = &c->row[j];
	float x0 = r->x0, dx = r->dx, y0 = r->y0, dy = r->dy;
	int so = c->screenover;
	if (r->nseg) {
		// line spans (ADR-037 rotation parameter window, ADR-063 layer window): nothing drawn outside spans, B spans use B coords
		int k = 0;
		while (k < r->nseg && !(r->sx[k] <= i && i <= r->ex[k]))
			k++;
		if (k == r->nseg)
			return 0;
		if (r->sp[k]) {
			if (!r->enb)
				return 0;
			if (c->two_maps) {
				// B list: B off-screen mode and B map table (ADR-046/071)
				if (!table_b)
					return 0;
				table = table_b;
				so = c->screenover_b;
			}
			x0 = r->xb0; dx = r->dxb; y0 = r->yb0; dy = r->dyb;
		}
	}
	// GPU: interpolate from line quad vertex value (x0 - dx/2) to pixel center
	const float fx = (x0 - 0.5f * dx) + ((float)i + 0.5f) * dx;
	const float fy = (y0 - 0.5f * dy) + ((float)i + 0.5f) * dy;
	int x = (int)floorf(fx), y = (int)floorf(fy);
	if (so == 3) {
		if (x < 0 || y < 0 || x >= 512 || y >= 512)
			return 0;
	} else if (so == 2) {
		if (x < 0 || y < 0 || x >= c->mapw || y >= c->maph)
			return 0;
	} else {
		x &= c->mapw - 1;
		y &= c->maph - 1;
	}
	const uint8_t *at;
	if (flat)
		at = pages + ((y >> 3) * 512 + (x >> 3)) * 4;
	else {
		const uint8_t *t = table + ((y >> 9) * 8 + (x >> 9)) * 4;
		if (!t[1])
			return 0;
		const int pi = t[0];
		at = pages + (((pi >> 3) * 64 + ((y & 511) >> 3)) * 512 + (pi & 7) * 64 + ((x & 511) >> 3)) * 4;
	}
	if (!(at[3] & 0x80))
		return 0;
	const int unit = at[0] | (at[1] & 0x3F) << 8;
	int lx = x & 7, ly = y & 7;
	if (at[1] & 0x40) lx = 7 - lx;
	if (at[1] & 0x80) ly = 7 - ly;
	const int au = (unit & 127) * 8 + lx, av = (unit >> 7) * 8 + ly;
	const int prio = (at[3] >> 3) & 7, scf = (at[3] >> 6) & 1;
	int dotv, msb = 0;
	uint8_t col[3];
	if (c->colornumber == 3) {
		const uint8_t *px = argb + (av * 1024 + au) * 4;
		if (!px[3] && c->transparencyenable)
			return 0;
		col[0] = px[0]; col[1] = px[1]; col[2] = px[2];
		dotv = px[0] >> 3;
	} else {
		dotv = (c->colornumber == 0 ? a4 : a8)[av * 1024 + au];
		if (dotv == 0 && c->transparencyenable)
			return 0;
		const int paladdr = at[2] | (at[3] & 7) << 8;
		const uint8_t *p = pal[(c->coloroffset + paladdr + dotv) & 2047];
		col[0] = p[0]; col[1] = p[1]; col[2] = p[2];
		msb = p[3];
	}
	const int scm = c->specialcolormode;
	int use = 1;
	if ((scm == 1 || scm == 2) && !scf) use = 0;
	else if (scm == 2) { if (!((c->specialcode >> ((dotv & 15) / 2)) & 1)) use = 0; }
	else if (scm == 3 && msb < 128) use = 0;
	const int flag = (use && (c->alpha & 0xFF) != 63) ? 8 : 0;
	return (uint32_t)col[0] | (uint32_t)col[1] << 8 | (uint32_t)col[2] << 16 | (uint32_t)(prio + flag) << 24;
}
