// NBG cell quad generation and character atlas (ADR-014). Math follows the same rules as vidsoft Vdp2DrawScroll
// (the same decode as shade() in tools/harness/nbgcheck.c, done per cell).
#include "nbgq.h"

#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define VRAM_MASK 0x7FFFF

static inline int rd16(const uint8_t *vram, uint32_t a)
{
	a &= VRAM_MASK & ~1u;
	return (vram[a] << 8) | vram[a + 1];
}

int nbgq_supported(const RstvLayerCapture *c)
{
	// 16/256 color palette, RGB555 (RGB555 uses the same RGB atlas as RBG0)
	return c->colornumber <= 1 || c->colornumber == 3;
}

typedef struct {
	int ok;          // 0 = cell not visible (priority 0)
	uint32_t unit;   // character start (VRAM 32-byte units)
	int flipx, flipy;
	uint16_t palbase;
	uint8_t prio, scf;
} Cell;

// char, palette, flip and priority of the 8x8 cell containing map coord (xx, yy)
int nbgq_track;
uint32_t nbgq_track_pages[4];
uint16_t nbgq_track_sub[128];
#define TRACK_PAGE(a) do { const unsigned a_ = (unsigned)(a) & 0x7FFFFu, p_ = a_ >> 12; nbgq_track_pages[p_ >> 5] |= 1u << (p_ & 31); 	nbgq_track_sub[p_] |= (uint16_t)(1u << ((a_ >> 8) & 15)); } while (0)

static void cell_at(const RstvLayerCapture *c, const RstvCapRow *r, const uint8_t *vram, int xx, int yy, Cell *o)
{
	// Vita has no divide instruction -> all sizes are powers of 2, so use shifts and masks
	const int supp = c->supplementdata;
	const int ppw_b = c->planepixelwidth_bits, pph_b = c->planepixelheight_bits;
	const int pagewh_b = c->pagewh_bits, pagesize_b = c->pagewh_bits * 2;
	const int planew_b = c->planew_bits, pds_b = c->patterndatasize_bits + 1;
	const int cellwh_b = 2 + c->patternwh;
	const int pr = r->priority & 15, spm = r->priority >> 4;
	const int pn = (yy >> pph_b) * c->mapwh + (xx >> ppw_b);
	const int lx = xx & ((1 << ppw_b) - 1), ly = yy & ((1 << pph_b) - 1);
	const uint32_t pt = r->planetbl[pn & 3] & 0xFFFFFF;
	const uint32_t addr = pt + ((((ly >> 9) << (pagesize_b + planew_b)) + ((lx >> 9) << pagesize_b) +
		(((ly & 511) >> cellwh_b) << pagewh_b) + ((lx & 511) >> cellwh_b)) << pds_b);
	const int cn = c->colornumber;
	int charaddr, paladdr, flip, sf, scf;
	if (c->patterndatasize < 2) {
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
	const int prio = (spm == 1) ? ((pr & ~1) + sf) : pr;
	o->ok = prio != 0;
	if (!o->ok)
		return;
	uint32_t unit = (uint32_t)charaddr; // 32-byte units
	if (c->patternwh >= 2) {
		// 2x2 pattern: 4 chars (top-left, top-right, bottom-left, bottom-right), positions swap when flipped
		int qx = (lx & 15) >= 8, qy = (ly & 15) >= 8;
		if (flip & 1) qx = !qx;
		if (flip & 2) qy = !qy;
		// char size: 4bpp 32 bytes (1 unit), 8bpp 64 (2), RGB555 128 (4)
		unit += (uint32_t)(qy * 2 + qx) * (cn == 0 ? 1 : cn == 1 ? 2 : 4);
	}
	o->unit = unit & 0x3FFF;
	if (nbgq_track) {
		TRACK_PAGE(addr);
		TRACK_PAGE(addr + 3);
		TRACK_PAGE(o->unit * 32);
		TRACK_PAGE(o->unit * 32 + (cn == 0 ? 31 : cn == 1 ? 63 : cn == 4 ? 255 : 127)); // ADR-137: the char's last byte
	}
	o->flipx = flip & 1;
	o->flipy = (flip >> 1) & 1;
	o->palbase = (uint16_t)((c->coloroffset + paladdr) & 2047);
	o->prio = (uint8_t)prio;
	o->scf = (uint8_t)scf;
}

int nbgq_build(const RstvLayerCapture *c, int w, int h, const uint8_t *vram, NbgQuad *out, int max, int flags)
{
	int n = 0;
	const int usewin = !(flags & NBGQ_NO_WINDOW);
	const int xwrap = c->xmask + 1;
	if (h > c->nrows)
		h = c->nrows;
	const int skipzoom = flags & NBGQ_SKIP_ZOOM, zoomruns = flags & NBGQ_ZOOM_RUNS;
#define ROW_ON(rr) ((rr)->enable && (!skipzoom || (rr)->zx == 1.0f))
	for (int y0 = 0; y0 < h;) {
		const RstvCapRow *r = &c->row[y0];
		if (zoomruns && r->enable && r->zx != 1.0f && NBGQ_RUN_ZX(r->zx)) {
			// ADR-052: a magnified line: map x = (int)(x + i * zx) & xmask (+ line scroll), as Vdp2DrawScroll; each run
			// of pixels with the same map x is one texel wide. Identical following lines share the quads
			int y1 = y0 + 1;
			while (y1 < h) {
				const RstvCapRow *q = &c->row[y1];
				if (!q->enable || q->zx != r->zx || q->y != r->y || q->x != r->x || q->lsx != r->lsx || q->priority != r->priority ||
					memcmp(q->planetbl, r->planetbl, sizeof(r->planetbl)) != 0 ||
					(usewin && (q->nwin != r->nwin || (r->nwin && (memcmp(q->wx0, r->wx0, sizeof(r->wx0)) || memcmp(q->wx1, r->wx1, sizeof(r->wx1)))))))
					break;
				y1++;
			}
			const int yy = r->y;
			for (int i = 0; i < w;) {
				int xm = ((int)((float)r->x + (float)i * r->zx)) & c->xmask;
				if (r->lsx)
					xm = (xm + r->lsx) & c->xmask; // ADR-099: map width
				int b = i + 1;
				while (b < w) {
					int xb = ((int)((float)r->x + (float)b * r->zx)) & c->xmask;
					if (r->lsx)
						xb = (xb + r->lsx) & c->xmask;
					if (xb != xm)
						break;
					b++;
				}
				Cell cell;
				cell_at(c, r, vram, xm, yy, &cell);
				if (cell.ok) {
					const int ox = (int)(cell.unit & 127) * 8, oy = (int)(cell.unit >> 7) * 8;
					const int cx = xm & 7, cy = yy & 7;
					const int nrun = (usewin && r->nwin) ? r->nwin : 1;
					for (int k = 0; k < nrun; k++) {
						int a0 = i, b0 = b;
						if (usewin && r->nwin) {
							if (a0 < r->wx0[k]) a0 = r->wx0[k];
							if (b0 > r->wx1[k] + 1) b0 = r->wx1[k] + 1;
							if (a0 >= b0)
								continue;
						}
						if (n >= max)
							return -1;
						NbgQuad *q = &out[n++];
						q->x0 = (int16_t)a0; q->x1 = (int16_t)b0;
						q->y0 = (int16_t)y0; q->y1 = (int16_t)y1;
						if (!cell.flipx) { q->u0 = (int16_t)(ox + cx); q->u1 = (int16_t)(ox + cx + 1); }
						else { q->u0 = (int16_t)(ox + 8 - cx); q->u1 = (int16_t)(ox + 7 - cx); }
						if (!cell.flipy) { q->v0 = (int16_t)(oy + cy); q->v1 = (int16_t)(oy + cy + 1); }
						else { q->v0 = (int16_t)(oy + 8 - cy); q->v1 = (int16_t)(oy + 7 - cy); }
						q->palbase = cell.palbase;
						q->prio = cell.prio;
						q->scf = cell.scf;
					}
				}
				i = b;
			}
			y0 = y1;
			continue;
		}
		if (!ROW_ON(r)) {
			y0++;
			continue;
		}
		// run of lines with the same scroll values where map y increases by 1. ADR-052: lines repeating the same map line (vertical stretch)
		// become one quad of height k and 1 texel row (v at every pixel center falls in the same texel row)
		int repy = 0;
		if (y0 + 1 < h) {
			const RstvCapRow *q = &c->row[y0 + 1];
			repy = ROW_ON(q) && q->y == r->y && q->x == r->x && q->lsx == r->lsx && q->priority == r->priority &&
				!memcmp(q->planetbl, r->planetbl, sizeof(r->planetbl));
		}
		int y1 = y0 + 1;
		while (y1 < h) {
			const RstvCapRow *q = &c->row[y1];
			if (!ROW_ON(q) || q->x != r->x || q->lsx != r->lsx || q->priority != r->priority ||
				memcmp(q->planetbl, r->planetbl, sizeof(r->planetbl)) != 0 || q->y != r->y + (repy ? 0 : (y1 - y0)) ||
				(usewin && (q->nwin != r->nwin || (r->nwin && (memcmp(q->wx0, r->wx0, sizeof(r->wx0)) || memcmp(q->wx1, r->wx1, sizeof(r->wx1)))))))
				break;
			y1++;
		}
		// split per cell row
		for (int sy = y0; sy < y1;) {
			const int yy = c->row[sy].y;
			int rows = repy ? y1 - sy : 8 - (yy & 7);
			if (rows > y1 - sy)
				rows = y1 - sy;
			const int vr = repy ? 1 : rows; // atlas rows the quad covers
			for (int i = 0; i < w;) {
				int xx = (r->x + i) & (xwrap - 1);
				int run = xwrap - xx; // up to the map horizontal wrap
				if (r->lsx) {
					xx = (xx + r->lsx) & (xwrap - 1); // ADR-099: line scroll wraps over the map width too
					if (xwrap - xx < run)
						run = xwrap - xx;
				}
				int seg = 8 - (xx & 7);
				if (seg > run) seg = run;
				if (seg > w - i) seg = w - i;
				Cell cell;
				cell_at(c, r, vram, xx, yy, &cell);
				if (cell.ok) {
					const int ox = (int)(cell.unit & 127) * 8, oy = (int)(cell.unit >> 7) * 8;
					const int cx = xx & 7, cy = yy & 7;
					// window (ADR-040): only the part overlapping the visible span, u clipped at the same ratio
					const int nrun = (usewin && r->nwin) ? r->nwin : 1;
					for (int k = 0; k < nrun; k++) {
						int a = i, b = i + seg;
						if (usewin && r->nwin) {
							if (a < r->wx0[k]) a = r->wx0[k];
							if (b > r->wx1[k] + 1) b = r->wx1[k] + 1;
							if (a >= b)
								continue;
						}
						if (n >= max)
							return -1;
						NbgQuad *q = &out[n++];
						q->x0 = (int16_t)a; q->x1 = (int16_t)b;
						q->y0 = (int16_t)sy; q->y1 = (int16_t)(sy + rows);
						if (!cell.flipx) { q->u0 = (int16_t)(ox + cx + (a - i)); q->u1 = (int16_t)(ox + cx + (b - i)); }
						else { q->u0 = (int16_t)(ox + 8 - cx - (a - i)); q->u1 = (int16_t)(ox + 8 - cx - (b - i)); }
						if (!cell.flipy) { q->v0 = (int16_t)(oy + cy); q->v1 = (int16_t)(oy + cy + vr); }
						else { q->v0 = (int16_t)(oy + 8 - cy); q->v1 = (int16_t)(oy + 8 - cy - vr); }
						q->palbase = cell.palbase;
						q->prio = cell.prio;
						q->scf = cell.scf;
					}
				}
				i += seg;
			}
			sy += rows;
		}
		y0 = y1;
	}
	return n;
}

static void unpack_unit(uint8_t *a4, uint8_t *a8, int pitch, const uint8_t *vram, uint32_t u)
{
	const int ox = (int)(u & 127) * 8, oy = (int)(u >> 7) * 8;
	const uint32_t base = u * 32;
	if (base + 64 <= VRAM_MASK + 1) {
		// atlas is GPU memory (uncached), byte writes are expensive -> two 32-bit writes per row
		const uint8_t *src = vram + base;
		for (int r = 0; r < 8; r++) {
			uint32_t *d4 = (uint32_t *)(a4 + (oy + r) * pitch + ox);
			uint32_t *d8 = (uint32_t *)(a8 + (oy + r) * pitch + ox);
			const uint8_t *s4 = src + r * 4;
			d4[0] = (uint32_t)(s4[0] >> 4) | (uint32_t)(s4[0] & 15) << 8 | (uint32_t)(s4[1] >> 4) << 16 | (uint32_t)(s4[1] & 15) << 24;
			d4[1] = (uint32_t)(s4[2] >> 4) | (uint32_t)(s4[2] & 15) << 8 | (uint32_t)(s4[3] >> 4) << 16 | (uint32_t)(s4[3] & 15) << 24;
			const uint8_t *s8 = src + r * 8;
			d8[0] = (uint32_t)s8[0] | (uint32_t)s8[1] << 8 | (uint32_t)s8[2] << 16 | (uint32_t)s8[3] << 24;
			d8[1] = (uint32_t)s8[4] | (uint32_t)s8[5] << 8 | (uint32_t)s8[6] << 16 | (uint32_t)s8[7] << 24;
		}
		return;
	}
	for (int r = 0; r < 8; r++) {
		uint8_t *d4 = a4 + (oy + r) * pitch + ox;
		uint8_t *d8 = a8 + (oy + r) * pitch + ox;
		for (int b = 0; b < 4; b++) {
			const uint8_t v = vram[(base + r * 4 + b) & VRAM_MASK];
			d4[b * 2] = v >> 4;
			d4[b * 2 + 1] = v & 15;
		}
		for (int b = 0; b < 8; b++)
			d8[b] = vram[(base + r * 8 + b) & VRAM_MASK];
	}
}

// ADR-185: a whole 256-byte block = 8 units side by side on 8 atlas rows. Each 64-byte row is built in cached memory
// and written with four 16-byte stores (unpack_unit: four 32-bit stores per unit row into uncached GPU memory)
static void unpack_block(uint8_t *a4, uint8_t *a8, int pitch, const uint8_t *vram, uint32_t u0)
{
	const int ox = (int)(u0 & 127) * 8, oy = (int)(u0 >> 7) * 8;
	const uint8_t *src = vram + u0 * 32;
	uint8_t r4[64] __attribute__((aligned(16))), r8[64] __attribute__((aligned(16)));
	for (int r = 0; r < 8; r++) {
		for (int k = 0; k < 8; k++) {
			const uint8_t *s4 = src + k * 32 + r * 4;
			uint8_t *d = r4 + k * 8;
			for (int b = 0; b < 4; b++) {
				d[b * 2] = s4[b] >> 4;
				d[b * 2 + 1] = s4[b] & 15;
			}
			memcpy(r8 + k * 8, src + k * 32 + r * 8, 8);
		}
		uint8_t *d4 = a4 + (oy + r) * pitch + ox, *d8 = a8 + (oy + r) * pitch + ox;
#ifdef __ARM_NEON
		for (int q = 0; q < 64; q += 16) {
			vst1q_u8(d4 + q, vld1q_u8(r4 + q));
			vst1q_u8(d8 + q, vld1q_u8(r8 + q));
		}
#else
		memcpy(d4, r4, 64);
		memcpy(d8, r8, 64);
#endif
	}
}

void nbgq_atlas_update(uint8_t *a4, uint8_t *a8, int pitch, const uint8_t *vram, const uint16_t *dirty)
{
	for (int p = 0; p < 128; p++) {
		const unsigned m = dirty ? dirty[p] : 0xFFFFu;
		if (!m)
			continue;
		// ADR-137: per changed 256-byte block, its 8 units + 1 preceding unit whose 8bpp block reaches into it
		for (int b = 0; b < 16; b++) {
			if (!(m & (1u << b)))
				continue;
			const int k0 = (b && (m & (1u << (b - 1)))) ? b * 8 : b * 8 - 1;
			const uint32_t u0 = (uint32_t)(p * 128 + b * 8);
			if (u0 * 32 + 256 + 32 <= VRAM_MASK + 1 && (pitch & 15) == 0) { // the last unit's 8bpp rows read 32 bytes on
				if (k0 < b * 8)
					unpack_unit(a4, a8, pitch, vram, (u0 - 1) & 0x3FFF);
				unpack_block(a4, a8, pitch, vram, u0);
				continue;
			}
			for (int k = k0; k < b * 8 + 8; k++)
				unpack_unit(a4, a8, pitch, vram, (uint32_t)(p * 128 + k) & 0x3FFF);
		}
	}
}
