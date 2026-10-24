// VDP1 GPU renderer (ADR-023). Sprite patterns -> texture cache (atlas), commands -> triangles.
// Pattern decode (color modes 0~5, SPD, end codes: rest of line transparent after the second end code) follows vidsoft drawAxisAlignedFast.
// Distorted sprites: 2 triangles if a parallelogram, else a 4x4 grid approximating VDP1's bilinear mapping.
#include "video/v1gpu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define VRAM_MASK 0x7FFFF

int v1gpu_grid_adapt = 1; // ADR-090 (ux0:data/restv/v1grid4 = always 4x4 as before)
int v1gpu_fmt;            // ADR-100 (v1gpu.h)
static int s_fb8;         // format the atlas was decoded in (8-bit pixels: only low 8 bits of the value)

// ---- texture cache ----
#define CACHE_SIZE 4096 // power of 2
typedef struct {
	uint32_t addr;       // pattern byte address
	uint32_t valid_as_of; // reflects only write stamps below this value (= serial + 1 at decode)
	uint16_t size, colr;
	uint8_t cmode, flags; // flags: 1 SPD, 2 end codes enabled
	uint8_t used;
	uint16_t ax, ay, w, h;
	uint32_t hash;        // hash of source bytes (+ LUT): not re-decoded if content is the same despite a write stamp
} Entry;
static Entry s_cache[CACHE_SIZE];
static int s_shelf_n, s_next_y;
static struct { int y, h, x; } s_shelf[128];
static unsigned s_resets;

static void cache_reset(void)
{
	memset(s_cache, 0, sizeof(s_cache));
	s_shelf_n = 0;
	s_next_y = 0;
	s_resets++;
}

static int atlas_alloc(int w, int h, int *ax, int *ay)
{
	for (int i = 0; i < s_shelf_n; i++)
		if (s_shelf[i].h >= h && s_shelf[i].h <= h + h / 2 + 8 && s_shelf[i].x + w <= V1GPU_ATLAS) {
			*ax = s_shelf[i].x;
			*ay = s_shelf[i].y;
			s_shelf[i].x += w;
			return 0;
		}
	if (s_shelf_n >= 128 || s_next_y + h > V1GPU_ATLAS || w > V1GPU_ATLAS)
		return -1;
	s_shelf[s_shelf_n].y = s_next_y;
	s_shelf[s_shelf_n].h = h;
	s_shelf[s_shelf_n].x = w;
	s_shelf_n++;
	*ax = 0;
	*ay = s_next_y;
	s_next_y += h;
	return 0;
}

static inline int rd16(const uint8_t *vram, uint32_t a)
{
	a &= VRAM_MASK & ~1u;
	return (vram[a] << 8) | vram[a + 1];
}

static int row_bytes(int cmode, int w) { return cmode <= 1 ? w >> 1 : cmode == 5 ? w * 2 : w; }

// decode a pattern into the atlas (r = value low, g = high, a = 0xFF written)
static void decode(const Entry *e, const uint8_t *vram, uint8_t *atlas)
{
	const int w = e->w, h = e->h, cm = e->cmode, spd = e->flags & 1, ends = (e->flags & 2) && (cm == 0 || cm == 1 || cm == 4 || cm == 5);
	const int ecd = (e->flags & 2) != 0;
	uint16_t lut[16];
	if (cm == 1)
		for (int k = 0; k < 16; k++)
			lut[k] = (uint16_t)rd16(vram, ((uint32_t)e->colr << 3) + (uint32_t)k * 2);
	const int rb = row_bytes(cm, w);
	for (int y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)(atlas + ((size_t)(e->ay + y) * V1GPU_ATLAS + e->ax) * 4);
		const uint32_t row = e->addr + (uint32_t)(y * rb);
		int endcodes = 0;
		for (int x = 0; x < w; x++) {
			uint32_t dot, pix = 0;
			int wr = 0, is_end = 0;
			switch (cm) {
			case 0:
			case 1:
				dot = vram[(row + ((uint32_t)x >> 1)) & VRAM_MASK];
				dot = (x & 1) ? (dot & 0xF) : (dot >> 4);
				if (ends && dot == 0xF) { is_end = 1; break; }
				if (cm == 0) { pix = (e->colr & 0xfff0) | dot; wr = spd || dot != 0; }
				else { pix = lut[dot]; wr = spd || (dot != 0 && pix != 0); }
				break;
			case 2:
				dot = vram[(row + (uint32_t)x) & VRAM_MASK] & 0x3F;
				if (ecd && dot == 63) dot = 0;
				pix = (e->colr & 0xffc0) | dot;
				wr = spd || dot != 0;
				break;
			case 3:
				dot = vram[(row + (uint32_t)x) & VRAM_MASK] & 0x7F;
				pix = (e->colr & 0xff80) | dot;
				wr = spd || dot != 0;
				break;
			case 4:
				dot = vram[(row + (uint32_t)x) & VRAM_MASK];
				if (ends && dot == 0xFF) { is_end = 1; break; }
				pix = (e->colr & 0xff00) | dot;
				wr = spd || dot != 0;
				break;
			default:
				dot = (uint32_t)rd16(vram, row + (uint32_t)x * 2);
				if (ends && dot == 0x7FFF) { is_end = 1; break; }
				pix = dot;
				wr = spd || (dot & 0x8000);
				break;
			}
			if (is_end) {
				d[x] = 0;
				if (++endcodes == 2) {
					for (x++; x < w; x++)
						d[x] = 0;
					break;
				}
				continue;
			}
			d[x] = wr ? (pix & (s_fb8 ? 0xFF : 0xFFFF)) | 0xFF000000u : 0;
		}
	}
	// 1-texel border copied from the pattern's own edge (clamp to edge): a texture coordinate landing exactly on the
	// pattern edge (distorted sprites: vertices on pixel centres, coordinates on texel edges) may round to the texel
	// outside. That texel held the last row of the pattern above or stale data (thuntk hats: dark lines along the top);
	// a transparent border instead opened gaps between the polygons of ffrevng's characters
	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(atlas + ((size_t)(e->ay + y) * V1GPU_ATLAS + e->ax) * 4);
		row[-1] = row[0];
		row[w] = row[w - 1];
	}
	memcpy(atlas + ((size_t)(e->ay - 1) * V1GPU_ATLAS + e->ax - 1) * 4, atlas + ((size_t)e->ay * V1GPU_ATLAS + e->ax - 1) * 4, (size_t)(w + 2) * 4);
	memcpy(atlas + ((size_t)(e->ay + h) * V1GPU_ATLAS + e->ax - 1) * 4, atlas + ((size_t)(e->ay + h - 1) * V1GPU_ATLAS + e->ax - 1) * 4, (size_t)(w + 2) * 4);
}

static uint32_t src_hash(const Entry *e, const uint8_t *vram)
{
	const uint32_t bytes = (uint32_t)(row_bytes(e->cmode, e->w) * e->h);
	uint32_t h = 2166136261u, a = e->addr & VRAM_MASK;
	if (a + bytes <= VRAM_MASK + 1 && !(a & 3)) {
		const uint32_t *p = (const uint32_t *)(vram + a);
		for (uint32_t i = 0; i < bytes / 4; i++)
			h = (h ^ p[i]) * 16777619u;
		for (uint32_t i = bytes & ~3u; i < bytes; i++)
			h = (h ^ vram[a + i]) * 16777619u;
	} else {
		for (uint32_t i = 0; i < bytes; i++)
			h = (h ^ vram[(a + i) & VRAM_MASK]) * 16777619u;
	}
	if (e->cmode == 1)
		for (int k = 0; k < 32; k++)
			h = (h ^ vram[(((uint32_t)e->colr << 3) + (uint32_t)k) & VRAM_MASK]) * 16777619u;
	return h;
}

static int stale(const Entry *e, const uint32_t *stamp)
{
	if (!stamp)
		return 1;
	const uint32_t bytes = (uint32_t)(row_bytes(e->cmode, e->w) * e->h);
	const uint32_t p0 = (e->addr & VRAM_MASK) >> 12, p1 = ((e->addr + (bytes ? bytes - 1 : 0)) & VRAM_MASK) >> 12;
	for (uint32_t p = p0; p <= p1 && p < 128; p++)
		if (stamp[p] >= e->valid_as_of)
			return 1;
	if (e->cmode == 1 && stamp[(((uint32_t)e->colr << 3) & VRAM_MASK) >> 12] >= e->valid_as_of)
		return 1;
	return 0;
}

// atlas position of a pattern. -1 on failure (atlas full)
static int lookup(uint32_t addr, uint16_t size, int cmode, uint16_t colr, int flags, const uint8_t *vram, uint8_t *atlas,
	const uint32_t *stamp, uint32_t serial, int *ax, int *ay, unsigned *decoded)
{
	const int w = ((size >> 8) & 0x3F) * 8, h = size & 0xFF;
	if (w == 0 || h == 0)
		return -1;
	if (cmode != 1)
		colr = (uint16_t)(colr & (cmode == 0 ? 0xfff0 : cmode == 2 ? 0xffc0 : cmode == 3 ? 0xff80 : cmode == 4 ? 0xff00 : 0));
	uint32_t hsh = (addr * 2654435761u) ^ ((uint32_t)size * 40503u) ^ ((uint32_t)colr << 7) ^ ((uint32_t)cmode << 3) ^ (uint32_t)flags;
	hsh = (hsh ^ (hsh >> 15)) & (CACHE_SIZE - 1);
	for (int probe = 0; probe < 32; probe++, hsh = (hsh + 1) & (CACHE_SIZE - 1)) {
		Entry *e = &s_cache[hsh];
		if (e->used && e->addr == addr && e->size == size && e->colr == colr && e->cmode == cmode && e->flags == flags) {
			if (stale(e, stamp)) {
				e->valid_as_of = serial + 1;
				const uint32_t hs = src_hash(e, vram);
				if (hs != e->hash) {
					e->hash = hs;
					decode(e, vram, atlas);
					*decoded += (unsigned)(e->w * e->h);
				}
			}
			*ax = e->ax;
			*ay = e->ay;
			return 0;
		}
		if (!e->used) {
			int x, y;
			if (atlas_alloc(w + 2, h + 2, &x, &y) < 0) // 1-texel transparent border (see decode)
				return -1;
			x++;
			y++;
			e->used = 1;
			e->addr = addr; e->size = size; e->colr = colr; e->cmode = (uint8_t)cmode; e->flags = (uint8_t)flags;
			e->ax = (uint16_t)x; e->ay = (uint16_t)y; e->w = (uint16_t)w; e->h = (uint16_t)h;
			e->valid_as_of = serial + 1;
			e->hash = src_hash(e, vram);
			decode(e, vram, atlas);
			*decoded += (unsigned)(w * h);
			*ax = x;
			*ay = y;
			return 0;
		}
	}
	return -1;
}

// ---- vertices ----
typedef struct {
	float x, y, u, v, r, g, b;
} Corner;

static Corner lerp(const Corner *a, const Corner *b, float t)
{
	Corner c;
	c.x = a->x + (b->x - a->x) * t; c.y = a->y + (b->y - a->y) * t;
	c.u = a->u + (b->u - a->u) * t; c.v = a->v + (b->v - a->v) * t;
	c.r = a->r + (b->r - a->r) * t; c.g = a->g + (b->g - a->g) * t; c.b = a->b + (b->b - a->b) * t;
	return c;
}

static float *put(float *o, const Corner *c, const float *clip)
{
	o[0] = c->x; o[1] = c->y; o[2] = c->u; o[3] = c->v;
	o[4] = clip[0]; o[5] = clip[1]; o[6] = clip[2]; o[7] = clip[3];
	o[8] = c->r; o[9] = c->g; o[10] = c->b; o[11] = 0;
	return o + V1GPU_VERTEX_FLOATS;
}

static int emit(float *out, int nv, int max, const Corner *c00, const Corner *c10, const Corner *c01, const Corner *c11, int g,
	const float *clip)
{
	if (nv + g * g * 6 > max)
		return -1;
	float *o = out + nv * V1GPU_VERTEX_FLOATS;
	if (g == 1) {
		o = put(o, c00, clip); o = put(o, c10, clip); o = put(o, c01, clip);
		o = put(o, c10, clip); o = put(o, c11, clip); o = put(o, c01, clip);
		return nv + 6;
	}
	// ADR-090: interpolate the (g+1)^2 grid points once (previously 8 corner interpolations per cell)
	Corner p[5][5];
	for (int i = 0; i <= g; i++) {
		const float s0 = (float)i / g;
		const Corner top = lerp(c00, c10, s0), bot = lerp(c01, c11, s0);
		for (int j = 0; j <= g; j++)
			p[j][i] = lerp(&top, &bot, (float)j / g);
	}
	for (int j = 0; j < g; j++)
		for (int i = 0; i < g; i++) {
			const Corner *a = &p[j][i], *b = &p[j][i + 1], *c = &p[j + 1][i], *d = &p[j + 1][i + 1];
			o = put(o, a, clip); o = put(o, b, clip); o = put(o, c, clip);
			o = put(o, b, clip); o = put(o, d, clip); o = put(o, c, clip);
		}
	return nv + g * g * 6;
}

static void add_batch(V1Frame *f, int variant, int first, int count)
{
	if (f->nb > 0 && f->batch[f->nb - 1].variant == variant && f->batch[f->nb - 1].first + f->batch[f->nb - 1].count == first) {
		f->batch[f->nb - 1].count += count;
		return;
	}
	if (f->nb < V1GPU_MAX_BATCH) {
		f->batch[f->nb].variant = variant;
		f->batch[f->nb].first = first;
		f->batch[f->nb].count = count;
		f->nb++;
	}
}

extern int v1gpu_skip_kinds;
int v1gpu_build(const RstvV1Item *items, int n, float *out, int max, uint8_t *atlas, const uint8_t *vram,
	const uint32_t *page_stamp, uint32_t serial, V1Frame *f)
{
	int nv = 0, restarted = 0;
	f->nb = 0;
	f->decoded_px = 0;
	if (((v1gpu_fmt & 4) != 0) != s_fb8) { // re-decode the atlas when the 8-bit / 16-bit format changes
		s_fb8 = (v1gpu_fmt & 4) != 0;
		cache_reset();
	}
	const float hx = (v1gpu_fmt & 1) ? 0.5f : 1.0f, hy = (v1gpu_fmt & 2) ? 0.5f : 1.0f;
	const uint16_t cmask = s_fb8 ? 0xFF : 0xFFFF;
	for (int k = 0; k < n; k++) {
		const RstvV1Item *it = &items[k];
		if ((v1gpu_skip_kinds >> it->kind) & 1) // diag (ux0:data/restv/v1skip = kind bit mask)
			continue;
		Corner c[4];
		memset(c, 0, sizeof(c));
		float clip[4];
		int variant, g = 1;
		if (it->kind == RSTV_V1_ERASE) {
			const float x0 = it->x[0], y0 = it->y[0], x1 = it->x[3], y1 = it->y[3];
			if (x1 <= x0 || y1 <= y0)
				continue;
			clip[0] = 0; clip[1] = 0; clip[2] = 511; clip[3] = 255;
			c[0].x = x0; c[0].y = y0; c[1].x = x1; c[1].y = y0; c[2].x = x0; c[2].y = y1; c[3].x = x1; c[3].y = y1;
			for (int i = 0; i < 4; i++) { c[i].u = (float)(it->colr & cmask & 0xFF); c[i].v = (float)((it->colr & cmask) >> 8); }
			variant = 4;
		} else if (it->kind == RSTV_V1_LINE) {
			// line (ADR-041): 1-pixel-wide quad stretched along the major axis to the end pixel, +-0.5 on the minor axis
			const int pmod = it->pmod;
			variant = 4 | ((pmod & 0x100) ? 1 : 0) | (((pmod & 7) == 4) ? 2 : 0);
			clip[0] = it->clip[0]; clip[1] = it->clip[1]; clip[2] = it->clip[2]; clip[3] = it->clip[3];
			const float ax = it->x[0] + 0.5f, ay = it->y[0] + 0.5f, bx = it->x[1] + 0.5f, by = it->y[1] + 0.5f;
			const int dx = it->x[1] - it->x[0], dy = it->y[1] - it->y[0];
			const int xmaj = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
			float ex = 0, ey = 0, px = 0, py = 0;
			if (xmaj) { ex = dx >= 0 ? 0.5f : -0.5f; py = 0.5f; }
			else { ey = dy >= 0 ? 0.5f : -0.5f; px = 0.5f; }
			c[0].x = ax - ex - px; c[0].y = ay - ey - py;
			c[1].x = bx + ex - px; c[1].y = by + ey - py;
			c[2].x = ax - ex + px; c[2].y = ay - ey + py;
			c[3].x = bx + ex + px; c[3].y = by + ey + py;
			for (int i = 0; i < 4; i++) {
				const uint16_t gc = it->grd[i & 1];
				c[i].u = (float)(it->colr & cmask & 0xFF);
				c[i].v = (float)((it->colr & cmask) >> 8);
				c[i].r = (float)(gc & 31); c[i].g = (float)((gc >> 5) & 31); c[i].b = (float)((gc >> 10) & 31);
			}
		} else {
			const int pmod = it->pmod, cmode = (pmod >> 3) & 7;
			variant = ((pmod & 0x100) ? 1 : 0) | (it->kind == RSTV_V1_POLY ? 4 : 0);
			if (pmod & 0x8000)
				variant |= 32; // ADR-118: MSB on (vidsoft returns before gouraud and colour calculation)
			else
				variant |= (((pmod & 7) == 4) ? 2 : 0) | (((pmod & 7) >= 1 && (pmod & 7) <= 3) ? (pmod & 7) << 3 : 0);
			clip[0] = it->clip[0]; clip[1] = it->clip[1]; clip[2] = it->clip[2]; clip[3] = it->clip[3];
			for (int i = 0; i < 4; i++) {
				c[i].x = it->x[i];
				c[i].y = it->y[i];
				c[i].r = (float)(it->grd[i] & 31); c[i].g = (float)((it->grd[i] >> 5) & 31); c[i].b = (float)((it->grd[i] >> 10) & 31);
			}
			if (it->kind == RSTV_V1_POLY) {
				for (int i = 0; i < 4; i++) { c[i].u = (float)(it->colr & cmask & 0xFF); c[i].v = (float)((it->colr & cmask) >> 8); }
			} else {
				int ax, ay;
				const int flags = ((pmod & 0x40) ? 1 : 0) | ((pmod & 0x80) ? 0 : 2);
				if (lookup((uint32_t)(it->srca << 3) & VRAM_MASK, it->size, cmode, it->colr, flags, vram, atlas, page_stamp, serial,
						&ax, &ay, &f->decoded_px) < 0) {
					// atlas full: flush and rebuild this frame from the start. Flushing midway lets a new pattern overwrite one that already got a slot,
					// so earlier sprites come out black or broken (thuntk stage start)
					// if it still overflows after rebuild (one frame's patterns exceed the atlas), only that sprite is dropped
					if (restarted)
						continue;
					cache_reset();
					restarted = 1;
					nv = 0;
					f->nb = 0;
					k = -1;
					continue;
				}
				const int w = ((it->size >> 8) & 0x3F) * 8, h = it->size & 0xFF;
				const int flip = (it->ctrl >> 4) & 3;
				const float u0 = (flip & 1) ? (float)(ax + w) : (float)ax, u1 = (flip & 1) ? (float)ax : (float)(ax + w);
				const float v0 = (flip & 2) ? (float)(ay + h) : (float)ay, v1 = (flip & 2) ? (float)ay : (float)(ay + h);
				c[0].u = u0; c[0].v = v0; c[1].u = u1; c[1].v = v0; c[2].u = u0; c[2].v = v1; c[3].u = u1; c[3].v = v1;
			}
			if (it->axis) {
				// pixel rect including the end point: right/bottom edge + 1 (start side + 1 on a flipped axis)
				const int xr = it->x[1] >= it->x[0], yd = it->y[2] >= it->y[0];
				const float xa = xr ? it->x[0] : it->x[0] + 1.0f, xb = xr ? it->x[1] + 1.0f : it->x[1];
				const float ya = yd ? it->y[0] : it->y[0] + 1.0f, yb = yd ? it->y[2] + 1.0f : it->y[2];
				c[0].x = c[2].x = xa; c[1].x = c[3].x = xb;
				c[0].y = c[1].y = ya; c[2].y = c[3].y = yb;
			} else {
				for (int i = 0; i < 4; i++) {
					c[i].x += 0.5f;
					c[i].y += 0.5f;
				}
				const int par = it->x[0] + it->x[3] == it->x[1] + it->x[2] && it->y[0] + it->y[3] == it->y[1] + it->y[2];
				g = par ? 1 : 4;
				if (!par && v1gpu_grid_adapt) {
					// ADR-090: reduce the grid for small distorted sprites (bilinear vs triangle approximation error scales with size, around 1 texel)
					int x0 = it->x[0], x1 = it->x[0], y0 = it->y[0], y1 = it->y[0];
					for (int i = 1; i < 4; i++) {
						if (it->x[i] < x0) x0 = it->x[i];
						if (it->x[i] > x1) x1 = it->x[i];
						if (it->y[i] < y0) y0 = it->y[i];
						if (it->y[i] > y1) y1 = it->y[i];
					}
					const int ext = (x1 - x0 > y1 - y0) ? x1 - x0 : y1 - y0;
					g = ext <= 16 ? 1 : ext <= 48 ? 2 : 4;
				}
			}
		}
		if (it->kind != RSTV_V1_ERASE && (hx != 1.0f || hy != 1.0f)) {
			// ADR-100: hi-res / double interlace list drawn at half size (pixel edges scale with the corners)
			for (int i = 0; i < 4; i++) {
				c[i].x *= hx;
				c[i].y *= hy;
			}
			clip[0] = floorf(clip[0] * hx);
			clip[1] = floorf(clip[1] * hy);
			clip[2] = floorf((clip[2] + 1) * hx) - 1;
			clip[3] = floorf((clip[3] + 1) * hy) - 1;
		}
		int r = emit(out, nv, max, &c[0], &c[1], &c[2], &c[3], g, clip);
		if (r < 0 && g > 1)
			r = emit(out, nv, max, &c[0], &c[1], &c[2], &c[3], 1, clip);
		if (r < 0)
			return -1;
		add_batch(f, variant, nv, r - nv);
		nv = r;
	}
	f->nv = nv;
	f->cache_resets = s_resets;
	return nv;
}

int v1gpu_skip_kinds;

// ---- shaders ----
#define NL "\n"
const char *v1gpu_vs(void)
{
	return "void main(float2 pos, float2 uv, float4 clip, float4 grd," NL
		"  float4 out vPosition : POSITION, float2 out vPos : TEXCOORD0, float2 out vUV : TEXCOORD1," NL
		"  float4 out vClip : TEXCOORD2, float4 out vGrd : TEXCOORD3)" NL
		"{" NL
		"  vPosition = float4(pos.x / 256.0 - 1.0, pos.y / 128.0 - 1.0, 0.0, 1.0);" NL
		"  vPos = pos; vUV = uv / 1024.0; vClip = clip; vGrd = grd;" NL
		"}" NL;
}

const char *v1gpu_fs(int variant)
{
	static char buf[V1GPU_VARIANTS][3072];
	char *b = buf[variant & (V1GPU_VARIANTS - 1)];
	const int mesh = variant & 1, gour = variant & 2, solid = variant & 4, cc = V1GPU_CC(variant), msb = V1GPU_MSB(variant);
	int n = snprintf(b, 3072,
		"float4 main(float2 vPos : TEXCOORD0, float2 vUV : TEXCOORD1, float4 vClip : TEXCOORD2, float4 vGrd : TEXCOORD3," NL
		"  uniform sampler2D tAtlas%s) : COLOR" NL, V1GPU_READS_FB(variant) ? ", uniform sampler2D tSnap" : "");
	n += snprintf(b + n, 3072 - n,
		"{" NL
		"  float2 fp = floor(vPos);" NL
		"  float4 cl = floor(vClip + 0.5);" NL
		"  if (fp.x < cl.x || fp.y < cl.y || fp.x > cl.z || fp.y > cl.w) discard;" NL);
	if (mesh)
		n += snprintf(b + n, 3072 - n, "  if (fmod(fp.x + fp.y, 2.0) > 0.5) discard;" NL);
	if (solid)
		// solid colour as two bytes in u / v: a 16-bit value in one coordinate lost its low bits in the interpolator
		// (sanjeon RGB polygons came out as yellow / blue patterns)
		n += snprintf(b + n, 3072 - n, "  float pix = floor(vUV.x * 1024.0 + 0.5) + floor(vUV.y * 1024.0 + 0.5) * 256.0;" NL);
	else if (!gour && !cc && !msb)
		n += snprintf(b + n, 3072 - n, "  float4 c = tex2D(tAtlas, vUV);" NL "  if (c.w < 0.5) discard;" NL
			"  return float4(c.x, c.y, 0.0, 0.0);" NL "}" NL);
	else
		n += snprintf(b + n, 3072 - n, "  float4 c = tex2D(tAtlas, vUV);" NL "  if (c.w < 0.5) discard;" NL
			"  float pix = floor(c.x * 255.0 + 0.5) + floor(c.y * 255.0 + 0.5) * 256.0;" NL);
	if (msb) {
		// ADR-118: vidsoft putpixel "if (currentPixel) *iPix |= 0x8000"
		n += snprintf(b + n, 3072 - n,
			"  if (pix < 0.5) discard;" NL
			"  float4 d = tex2D(tSnap, (fp + 0.5) / float2(512.0, 256.0));" NL
			"  float dp = floor(d.x * 255.0 + 0.5) + floor(d.y * 255.0 + 0.5) * 256.0;" NL
			"  pix = dp < 32768.0 ? dp + 32768.0 : dp;" NL);
	}
	if (cc) {
		// ADR-115: vidsoft.c Vdp1 putpixel modes 1..3 (alphablend16 level 128 = per channel floor((s + d) / 2))
		n += snprintf(b + n, 3072 - n,
			"  float sr = fmod(pix, 32.0), sg = fmod(floor(pix / 32.0), 32.0), sb = fmod(floor(pix / 1024.0), 32.0);" NL);
		if (cc == 2)
			n += snprintf(b + n, 3072 - n, "  pix = 32768.0 + floor(sr * 0.5) + floor(sg * 0.5) * 32.0 + floor(sb * 0.5) * 1024.0;" NL);
		else
			n += snprintf(b + n, 3072 - n,
				"  float4 d = tex2D(tSnap, (fp + 0.5) / float2(512.0, 256.0));" NL
				"  float dp = floor(d.x * 255.0 + 0.5) + floor(d.y * 255.0 + 0.5) * 256.0;" NL
				"  %s" NL
				"  if (dp >= 32768.0) {" NL
				"    float dr = fmod(dp, 32.0), dg = fmod(floor(dp / 32.0), 32.0), db = fmod(floor(dp / 1024.0), 32.0);" NL
				"    pix = 32768.0 + floor((sr + dr) * 0.5) + floor((sg + dg) * 0.5) * 32.0 + floor((sb + db) * 0.5) * 1024.0;" NL
				"  }" NL,
				cc == 1 ? "if (dp < 32768.0) discard; sr = 0.0; sg = 0.0; sb = 0.0;" : "");
	}
	if (solid || gour || cc || msb) {
		if (gour)
			n += snprintf(b + n, 3072 - n,
				"  float r = fmod(pix, 32.0), g = fmod(floor(pix / 32.0), 32.0), bl = fmod(floor(pix / 1024.0), 32.0);" NL
				"  r = clamp(r + floor(vGrd.x) - 16.0, 0.0, 31.0);" NL
				"  g = clamp(g + floor(vGrd.y) - 16.0, 0.0, 31.0);" NL
				"  bl = clamp(bl + floor(vGrd.z) - 16.0, 0.0, 31.0);" NL
				"  pix = 32768.0 + r + g * 32.0 + bl * 1024.0;" NL);
		n += snprintf(b + n, 3072 - n, "  return float4(fmod(pix, 256.0) / 255.0, floor(pix / 256.0) / 255.0, 0.0, 0.0);" NL "}" NL);
	}
	return b;
}
