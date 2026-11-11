// Neon corridor (ADR-236, from ReBOX vita/src/frontend/neon.c, same author, GPL-2.0-or-later): the moving artwork
// beside the game picture for games without their own (Border = Artwork). A corridor seen down its length: its walls'
// depth lines (rails) run from the screen's edges in to the picture's sides, rings on the walls flow out to the screen's
// edges, light pulses run out along the rails and dust drifts by the walls. Nothing shows inside the picture's columns.
// The background and the rails are made into vertices once (again when the picture's width or the theme changes) and
// kept; the rings, pulses and dust are made each frame into gx_temp. Lines are thin quads, added (blend one + one).
// Knobs fixed at ReBOX's defaults (speed 10, rings 2, theme hue, glow 1.0, brightness 0.5, 60 pulses, 200 dust).
#include "video/neon.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "video/gx.h"
#include "platform/log.h"
#include "platform/timer.h"
#include "platform/cpu3.h"

#include <psp2/kernel/threadmgr.h>

// coloured triangles: position in pixels (960x544, top = 0), colour per vertex; alpha blended or added
static const char *s_vs_col =
	"void main(float2 position, float4 color, uniform float4 scale,\n"
	"  float4 out vPosition : POSITION, float4 out vCol : COLOR)\n"
	"{\n"
	"  vPosition = float4(position * scale.xy - 1.0, 0.0, 1.0);\n"
	"  vCol = color;\n"
	"}\n";
static const char *s_fs_col =
	"float4 main(float4 vCol : COLOR) : COLOR\n"
	"{\n"
	"  return vCol;\n"
	"}\n";
static GxProg *s_prog_alpha, *s_prog_add, *s_pr;
static GxParam s_u_alpha_scale, s_u_add_scale, s_scale;
static const float s_sc[4] = { 2.0f / 960.0f, 2.0f / 544.0f, 0, 0 };
static int prog_make(void)
{
	static int tried;
	if (!tried) {
		tried = 1;
		const GxAttr a[2] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 }, { "color", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 8 } };
		s_prog_alpha = gx_prog(s_vs_col, s_fs_col, a, 2, 12, GX_BLEND_ALPHA);
		s_prog_add = gx_prog(s_vs_col, s_fs_col, a, 2, 12, GX_BLEND_ADD);
		if (s_prog_alpha)
			s_u_alpha_scale = gx_param(s_prog_alpha, "scale");
		if (s_prog_add)
			s_u_add_scale = gx_param(s_prog_add, "scale");
		RSTV_LOG("neon: programs %s", s_prog_alpha && s_prog_add ? "ok" : "FAILED");
	}
	return s_prog_alpha && s_prog_add;
}

#define SW 960
#define SH 544
#define ZM 40.0f       // the depth whose walls meet the picture's sides
#define DENSITY 2.0f
#define SPEED 10.0f
#define GLOW 1.0f
#define BRIGHT 0.5f
#define NPULSE_MAX 60
#define NDUST_MAX 300
#define NPULSE 60
#define NDUST 200
#define RAIL_SEG 16
#define STATIC_BYTES (512 * 1024)
#define DYN_MAX 9000   // vertices a frame

typedef struct { float x, y; uint32_t c; } Vtx; // (position, colour ABGR)

static float F, HW, HH, ZN, ZF, RING, cx, cy, edge;
static float s_lay_x = -1, s_lay_w = -1;
static uint32_t s_lay_acc;

// colours: the HTML's hsla(hue + dh, 85%, l%), the hue the theme accent's
static float s_hue, s_sat = 0.85f;   // (saturation 0: a grey theme's lines grey and white)
static uint32_t rgb_hsl(float h, float l)
{
	h = fmodf(h + 360.0f, 360.0f) / 60.0f;
	const float s = s_sat, c = (1.0f - fabsf(2.0f * l - 1.0f)) * s, x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f)), m = l - c / 2;
	float r = 0, g = 0, b = 0;
	switch ((int)h) {
	case 0: r = c; g = x; break;
	case 1: r = x; g = c; break;
	case 2: g = c; b = x; break;
	case 3: g = x; b = c; break;
	case 4: r = x; b = c; break;
	default: r = c; b = x; break;
	}
	return (uint32_t)((r + m) * 255.0f + 0.5f) | (uint32_t)((g + m) * 255.0f + 0.5f) << 8 | (uint32_t)((b + m) * 255.0f + 0.5f) << 16;
}
static int s_add;   // (drawing added: the add blend takes the colour whole, so it is the colour times alpha)
static uint32_t col(uint32_t rgb, float a)
{
	if (s_add)
		a *= BRIGHT;
	if (a > 1.0f)
		a = 1.0f;
	if (a < 0.0f)
		a = 0.0f;
	if (s_add) {
		const unsigned k = (unsigned)(a * 256.0f);
		return ((rgb & 0xFF) * k >> 8) | (((rgb >> 8) & 0xFF) * k >> 8) << 8 | (((rgb >> 16) & 0xFF) * k >> 8) << 16 | 0xFF000000u;
	}
	return rgb | (uint32_t)(a * 255.0f + 0.5f) << 24;
}
static uint32_t C55, C62, C76, C82, C50, P65, P88, D80;   // (lightness, the pulses' hue +25, the dust's -20)

static float hue_of(uint32_t acc)
{
	const float r = (acc & 0xFF) / 255.0f, g = ((acc >> 8) & 0xFF) / 255.0f, b = ((acc >> 16) & 0xFF) / 255.0f;
	const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
	if (d < 0.12f)
		return -1.0f;   // (a grey theme: no hue, grey lines)
	float h = mx == r ? fmodf((g - b) / d, 6.0f) : mx == g ? (b - r) / d + 2.0f : (r - g) / d + 4.0f;
	h *= 60.0f;
	return h < 0 ? h + 360.0f : h;   // (0-360: below 0 means grey)
}

static void proj(float x, float y, float z, float *px, float *py)
{
	*px = cx + x * F / z;
	*py = cy + y * F / z;
}
static float smooth(float a, float b, float v)
{
	float t = (v - a) / (b - a);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}
// fades out into the picture's columns
static float side(float sx) { return smooth(edge * 0.7f, edge * 1.02f, fabsf(sx - cx)); }
static float z_at(float t) { return 1.0f / (1.0f / ZN + (1.0f / ZF - 1.0f / ZN) * t); }

static Vtx *s_v;
static int s_n, s_max;
static void tri(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2)
{
	if (s_n + 3 > s_max)
		return;
	Vtx *v = s_v + s_n;
	v[0] = (Vtx){ x0, y0, c0 };
	v[1] = (Vtx){ x1, y1, c1 };
	v[2] = (Vtx){ x2, y2, c2 };
	s_n += 3;
}
// a line a-b as a quad, its widths and colours at each end (thinner than a pixel: a pixel wide and fainter)
static void seg(float ax, float ay, float bx, float by, float wa, float wb, uint32_t rgb, float aa, float ab)
{
	if (wa < 1.0f) {
		aa *= wa;
		wa = 1.0f;
	}
	if (wb < 1.0f) {
		ab *= wb;
		wb = 1.0f;
	}
	float dx = bx - ax, dy = by - ay;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len < 0.01f)
		return;
	dx /= len;
	dy /= len;
	const float nax = -dy * wa * 0.5f, nay = dx * wa * 0.5f, nbx = -dy * wb * 0.5f, nby = dx * wb * 0.5f;
	const uint32_t ca = col(rgb, aa), cb = col(rgb, ab);
	tri(ax + nax, ay + nay, ca, ax - nax, ay - nay, ca, bx + nbx, by + nby, cb);
	tri(bx + nbx, by + nby, cb, ax - nax, ay - nay, ca, bx - nbx, by - nby, cb);
}
static void rect(float x0, float y0, float x1, float y1, uint32_t c0, uint32_t c1)   // (colours left, right)
{
	tri(x0, y0, c0, x0, y1, c0, x1, y0, c1);
	tri(x1, y0, c1, x0, y1, c0, x1, y1, c1);
}

static uint32_t s_rand = 0x2545F491;
static float rnd(void)
{
	s_rand = s_rand * 1664525u + 1013904223u;
	return (s_rand >> 8) * (1.0f / 16777216.0f);
}

// rails (depth lines): x, y in corridor halves, major
#define NRAIL (33 * 2 + 8 * 4)
static float s_rail[NRAIL][3];
static void rails_make(void)
{
	int n = 0;
	for (int i = 0; i <= 32; i++) {
		const float fy = -1.0f + i / 16.0f;
		for (int s = -1; s <= 1; s += 2) {
			s_rail[n][0] = (float)s;
			s_rail[n][1] = fy;
			s_rail[n++][2] = i % 4 == 0;
		}
	}
	for (int i = 0; i < 8; i++)
		for (int s = -1; s <= 1; s += 2)
			for (int f = -1; f <= 1; f += 2) {
				s_rail[n][0] = s * (1.0f - i * 0.06f);
				s_rail[n][1] = (float)f;
				s_rail[n++][2] = i == 0;
			}
}

static struct { float z, v, len; int r; } s_pulse[NPULSE_MAX];
static struct { float sx, fx, fy, t; } s_dust[NDUST_MAX];
static float s_travel, s_time;
static uint64_t s_last;

static Vtx *s_static;
static int s_nbg, s_nstatic;

static void layout(float gx, float gw)
{
	float c = gw / SW;
	c = c < 0.2f ? 0.2f : c > 0.96f ? 0.96f : c;
	F = (SW < SH ? SW : SH) * 0.9f;
	edge = gw * 0.5f;
	cx = gx + gw * 0.5f;
	cy = SH * 0.5f;
	HW = edge * ZM / F;
	HH = (SH / 2) * 1.05f * ZM * 1.6f / F;
	ZN = ZM * c * 0.9f;
	ZF = ZM * 1.6f;
	RING = (ZM - ZM * c) / DENSITY;
	s_travel = fmodf(s_travel, RING * 5.0f);
}

// the background and the rails into the kept vertices
static void static_make(void)
{
	if (!s_static) {
		s_static = (Vtx *)gx_static(STATIC_BYTES);
		if (!s_static)
			return;
		rails_make();
	} else
		gx_finish(); // (the GPU may be reading them)
	s_v = s_static;
	s_n = 0;
	s_max = STATIC_BYTES / sizeof(Vtx);
	// the ground, the bands darkest at the screen's edges and melting into it towards the picture, a faint glow in each
	const uint32_t ground = 0xFF26211C, deep = 0xFF130F0A, mid = 0xFF1F1A14, clear = 0x0026211C;
	rect(0, 0, SW, SH, ground, ground);
	const float band = SW * 0.5f - edge;
	for (int s = -1; s <= 1; s += 2) {
		const float x0 = s < 0 ? 0 : SW, x1 = cx + s * edge * 0.85f, xm = x0 + (x1 - x0) * 0.7f;
		if (s < 0) {
			rect(x0, 0, xm, SH, deep, mid);
			rect(xm, 0, x1, SH, mid, clear);
		} else {
			rect(xm, 0, x0, SH, mid, deep);
			rect(x1, 0, xm, SH, clear, mid);
		}
		const float gx = cx + s * (edge + band * 0.45f), r = fmaxf(band * 1.6f, SH * 0.45f);
		const uint32_t c0 = col(C50, 0.10f * GLOW), c1 = col(C50, 0);
		for (int k = 0; k < 32; k++) {
			const float a0 = k * (6.2831853f / 32), a1 = (k + 1) * (6.2831853f / 32);
			tri(gx, cy, c0, gx + cosf(a0) * r, cy + sinf(a0) * r, c1, gx + cosf(a1) * r, cy + sinf(a1) * r, c1);
		}
	}
	s_nbg = s_n;
	s_add = 1;
	for (int i = 0; i < NRAIL; i++) {
		const float x = s_rail[i][0] * HW, y = s_rail[i][1] * HH, k8 = s_rail[i][2] ? 1.0f : 0.5f;
		float px[RAIL_SEG + 1], py[RAIL_SEG + 1], a[RAIL_SEG + 1], w[RAIL_SEG + 1];
		for (int k = 0; k <= RAIL_SEG; k++) {
			const float z = z_at((float)k / RAIL_SEG);
			proj(x, y, z, &px[k], &py[k]);
			a[k] = side(px[k]) * k8;
			w[k] = fmaxf(0.6f, F / z * HW * 0.0009f);
		}
		for (int k = 0; k < RAIL_SEG; k++) {
			if (a[k] < 0.01f && a[k + 1] < 0.01f)
				continue;
			if ((py[k] < -8 && py[k + 1] < -8) || (py[k] > SH + 8 && py[k + 1] > SH + 8))
				continue;
			if (GLOW > 0)
				seg(px[k], py[k], px[k + 1], py[k + 1], w[k] * 5, w[k + 1] * 5, C55, a[k] * 0.07f * GLOW, a[k + 1] * 0.07f * GLOW);
			seg(px[k], py[k], px[k + 1], py[k + 1], w[k], w[k + 1], s_rail[i][2] ? C76 : C62, a[k] * 0.7f, a[k + 1] * 0.7f);
		}
	}
	s_nstatic = s_n;
	s_add = 0;
}

static void pulse_new(int i, float z)
{
	s_pulse[i].r = (int)(rnd() * NRAIL) % NRAIL;
	s_pulse[i].z = z;
	s_pulse[i].v = 0.6f + rnd() * 1.2f;
	s_pulse[i].len = 0.8f + rnd() * 2.0f;
}

static void draw_list(const Vtx *v, int n)
{
	for (int i = 0; i < n; i += 60000) { // (the shared index list is 65536 long)
		gx_use(s_pr);
		gx_set4(s_scale, s_sc);
		gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, v + i, gx_seq_indices(0), n - i < 60000 ? n - i : 60000);
	}
}

// ADR-240 (owner: the artwork made the game slower): the rings, pulses and dust are made on a thread of their own (CPU3
// when it is open), a frame ahead, into four vertex buffers in turn; the main thread only draws the last one done. A
// buffer is written again only after three newer ones, when the GPU (at most two frames behind) is done with it
#define NDYN 4
static Vtx *s_dyn[NDYN];
static volatile int s_dyn_n[NDYN], s_ready = -1, s_busy;
static SceUID s_thr = -1, s_sema = -1;
static int dyn_make(Vtx *v)
{
	const uint64_t now = rstv_time_us();
	float dt = s_last ? (now - s_last) / 1e6f : 0;
	s_last = now;
	if (dt > 0.05f)
		dt = 0.05f;
	s_travel = fmodf(s_travel + SPEED * dt * 0.22f, RING * 5.0f);
	s_time += dt;

	s_add = 1;
	s_v = v;
	s_n = 0;
	s_max = DYN_MAX;
	// rings: upright on the walls, flowing out to the screen's edges
	int idx = (int)ceilf((ZF + s_travel) / RING);
	for (float z = idx * RING - s_travel; z > ZN * 0.98f && s_v; z -= RING, idx--) {
		if (z > ZF)
			continue;
		const int major = idx % 5 == 0;
		for (int s = -1; s <= 1; s += 2) {
			float tx, ty, bx, by, ix0, iy0, ix1, iy1;
			proj(s * HW, -HH, z, &tx, &ty);
			proj(s * HW, HH, z, &bx, &by);
			proj(s * HW * 0.8f, -HH, z, &ix0, &iy0);
			proj(s * HW * 0.8f, HH, z, &ix1, &iy1);
			const float w8 = side(tx);
			if (w8 < 0.01f)
				continue;
			const float w = fmaxf(0.6f, F / z * HW * (major ? 0.0018f : 0.0009f));
			const float ga = w8 * (major ? 0.14f : 0.05f) * GLOW, ca = w8 * (major ? 0.95f : 0.5f);
			const uint32_t cc = major ? C82 : C62;
			if (ty > 0) {
				seg(ix0, iy0, tx, ty, w * 6, w * 6, C55, ga, ga);
				seg(ix0, iy0, tx, ty, w, w, cc, ca, ca);
			}
			seg(tx, fmaxf(ty, -8), bx, fminf(by, SH + 8), w * 6, w * 6, C55, ga, ga);
			seg(tx, fmaxf(ty, -8), bx, fminf(by, SH + 8), w, w, cc, ca, ca);
			if (by < SH) {
				seg(bx, by, ix1, iy1, w * 6, w * 6, C55, ga, ga);
				seg(bx, by, ix1, iy1, w, w, cc, ca, ca);
			}
		}
	}
	// pulses: light running out along the rails
	for (int i = 0; i < NPULSE && s_v; i++) {
		s_pulse[i].z -= SPEED * s_pulse[i].v * dt * 0.5f;
		if (s_pulse[i].z < ZN * 0.9f)
			pulse_new(i, ZF);
		const float x = s_rail[s_pulse[i].r][0] * HW, y = s_rail[s_pulse[i].r][1] * HH;
		for (int k = 0; k < 8; k++) {
			const float z0 = s_pulse[i].z + s_pulse[i].len * k / 8, z1 = s_pulse[i].z + s_pulse[i].len * (k + 1) / 8;
			float ax, ay, bx, by;
			proj(x, y, z0, &ax, &ay);
			proj(x, y, z1, &bx, &by);
			const float w8 = side(ax) * (1.0f - k / 8.0f);
			if (w8 < 0.01f || ay < -8 || ay > SH + 8)
				continue;
			const float w = fmaxf(1.0f, F / z0 * HW * 0.0025f);
			seg(ax, ay, bx, by, w * 4, w * 4, P65, w8 * 0.2f * (0.3f + GLOW), w8 * 0.2f * (0.3f + GLOW));
			seg(ax, ay, bx, by, w, w, P88, w8, w8);
		}
	}
	// dust drifting out by the walls
	for (int i = 0; i < NDUST && s_v; i++) {
		s_dust[i].t -= dt * SPEED * 0.012f;
		if (s_dust[i].t < 0) {
			s_dust[i].t = 1;
			s_dust[i].fy = rnd() * 2 - 1;
			s_dust[i].fx = 0.75f + rnd() * 0.25f;
		}
		const float z = ZN + (ZM * 1.15f - ZN) * s_dust[i].t;
		float sx, sy;
		proj(s_dust[i].sx * s_dust[i].fx * HW, s_dust[i].fy * HH + sinf(s_time + s_dust[i].fx * 20) * HH * 0.02f, z, &sx, &sy);
		float a = side(sx) * 0.7f;
		if (a < 0.01f || sy < -4 || sy > SH + 4)
			continue;
		float r = fmaxf(0.5f, F / z * HW * 0.0012f);
		if (r < 0.75f) {
			a *= r / 0.75f;
			r = 0.75f;
		}
		const uint32_t c = col(D80, a);
		rect(sx - r, sy - r, sx + r, sy + r, c, c);
	}
	s_add = 0;
	return s_n;
}
static int neon_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (;;) {
		sceKernelWaitSema(s_sema, 1, NULL);
		const int w = (s_ready + 1) % NDYN;
		s_dyn_n[w] = dyn_make(s_dyn[w]);
		__sync_synchronize();
		s_ready = w;
		s_busy = 0;
	}
	return 0;
}
static int worker_start(void)
{
	if (s_thr >= 0)
		return 1;
	for (int k = 0; k < NDYN; k++)
		if (!(s_dyn[k] = (Vtx *)gx_static(DYN_MAX * sizeof(Vtx))))
			return 0;
	s_sema = sceKernelCreateSema("rstv_neon", 0, 0, 1, NULL);
	s_thr = sceKernelCreateThread("rstv_neon", neon_thread, 120, 0x4000, 0, rstv_cpu3_mask(0), NULL);
	if (s_sema < 0 || s_thr < 0) {
		RSTV_LOG("neon: thread %08x sema %08x", (unsigned)s_thr, (unsigned)s_sema);
		s_thr = -1;
		return 0;
	}
	sceKernelStartThread(s_thr, 0, NULL);
	return 1;
}

// ADR-240: the background and the rails do not move: drawn once into a texture of the screen's size (again when the
// picture's width or the theme changes) that the video output copies beside the picture. Outside a scene
static GxTex *s_stex;
static int s_stex_ok;
GxTex *rstv_neon_prepare(float game_x, float game_w, uint32_t accent)
{
	if (!prog_make() || !worker_start())
		return NULL;
	static int init;
	if (!init) {
		init = 1;
		for (int i = 0; i < NDUST_MAX; i++) {
			s_dust[i].sx = rnd() < 0.5f ? -1.0f : 1.0f;
			s_dust[i].fx = 0.75f + rnd() * 0.25f;
			s_dust[i].fy = rnd() * 2 - 1;
			s_dust[i].t = rnd();
		}
	}
	if (game_x != s_lay_x || game_w != s_lay_w || accent != s_lay_acc || !s_static) {
		const int first = s_lay_w < 0;
		while (s_busy) // (the worker reads the layout)
			sceKernelDelayThread(200);
		s_lay_x = game_x;
		s_lay_w = game_w;
		s_lay_acc = accent;
		s_hue = hue_of(accent); // (the theme's hue)
		s_sat = s_hue < 0 ? 0.0f : 0.85f;
		if (s_hue < 0)
			s_hue = 0;
		C50 = rgb_hsl(s_hue, 0.50f);
		C55 = rgb_hsl(s_hue, 0.55f);
		C62 = rgb_hsl(s_hue, 0.62f);
		C76 = rgb_hsl(s_hue, 0.76f);
		C82 = rgb_hsl(s_hue, 0.82f);
		P65 = rgb_hsl(s_hue + 25, 0.65f);
		P88 = rgb_hsl(s_hue + 25, 0.88f);
		D80 = rgb_hsl(s_hue - 20, 0.80f);
		layout(game_x, game_w);
		if (first)
			for (int i = 0; i < NPULSE_MAX; i++)
				pulse_new(i, ZN + rnd() * (ZF - ZN));
		static_make();
		s_stex_ok = 0;
	}
	if (!s_static)
		return NULL;
	if (!s_stex)
		s_stex = gx_tex(SW, SH, GX_RGBA8 | GX_CDRAM, 1, 30); // (render target slot 30: its own)
	if (s_stex && !s_stex_ok) {
		gx_begin(s_stex);
		s_pr = s_prog_alpha;
		s_scale = s_u_alpha_scale;
		draw_list(s_static, s_nbg);
		s_pr = s_prog_add;
		s_scale = s_u_add_scale;
		draw_list(s_static + s_nbg, s_nstatic - s_nbg);
		gx_end();
		s_stex_ok = 1;
		RSTV_LOG("neon: background drawn (%d + %d vertices)", s_nbg, s_nstatic - s_nbg);
	}
	return s_stex_ok ? s_stex : NULL;
}

// inside the screen's scene, after the background was copied: the moving parts (added)
void rstv_neon_draw(void)
{
	if (!s_stex_ok)
		return;
	s_pr = s_prog_add;
	s_scale = s_u_add_scale;
	// the rings, pulses and dust the worker made last (ADR-240); then it makes the next ones
	const int r = s_ready;
	if (r >= 0 && s_dyn_n[r] > 0)
		draw_list(s_dyn[r], s_dyn_n[r]);
	if (!s_busy) {
		s_busy = 1;
		sceKernelSignalSema(s_sema, 1);
	}
}
