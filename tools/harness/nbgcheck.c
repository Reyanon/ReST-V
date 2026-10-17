#include <math.h>
// --gpucheck: port the Vita NBG layer shader math (vita/src/video/gvdp2.c s_fs_nbg) to C as is,
// and compare per pixel against the layer vidsoft draws on the CPU. Catches shader logic bugs without hardware.
// Output format matches gvdp2.c fill_layer: (rgb) | (priority | ratio_used<<3) << 24, priority 0 is 0.
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "core.h"
#include "rstv_capture.h"
#include "titan/titan.h"
#include "vdp2.h"
#include "vidshared.h"
#include "vidsoft.h"
#include "nbgq.h"
#include "rbgq.h"

extern int rstv_gpu_compose;
extern int vdp2width, vdp2height;

static u8 s_pal[2048][4];
static unsigned long long s_checked, s_bad, s_frames, s_layers, s_fallback;
static int s_reported;
static unsigned long long s_why[16];
static void (*s_orig)(void);

static void build_palette(void)
{
	for (int i = 0; i < 2048; i++) {
		u8 *o = s_pal[i];
		if (Vdp2Internal.ColorMode == 2) {
			u32 v = T2ReadLong(Vdp2ColorRam, (i << 2) & 0xFFF);
			o[0] = v & 0xFF; o[1] = (v >> 8) & 0xFF; o[2] = (v >> 16) & 0xFF;
			o[3] = (v & 0x80000000) ? 255 : 0;
		} else {
			u16 v = T2ReadWord(Vdp2ColorRam, (i << 1) & 0xFFF);
			o[0] = (v & 0x1F) << 3; o[1] = ((v >> 5) & 0x1F) << 3; o[2] = ((v >> 10) & 0x1F) << 3;
			o[3] = (v & 0x8000) ? 255 : 0;
		}
	}
}

static int rd8(u32 a) { return Vdp2Ram[a & 0x7FFFF]; }
static int rd16(u32 a)
{
	a &= 0x7FFFF;
	const u8 *t = Vdp2Ram + (a & ~3u);
	return (a & 3) < 1 ? (t[0] << 8 | t[1]) : (t[2] << 8 | t[3]);
}

// same math as s_fs_nbg (integer)
static u32 shade(const RstvLayerCapture *c, int i, int row)
{
	const RstvCapRow *r = &c->row[row];
	if (row >= c->nrows || !r->enable)
		return 0;
	if (r->nwin) { /* window runs (ADR-040) */
		int in = 0;
		for (int k = 0; k < r->nwin; k++)
			if (i >= r->wx0[k] && i <= r->wx1[k]) in = 1;
		if (!in)
			return 0;
	}
	const int supp = c->supplementdata;
	const int ppw = 1 << c->planepixelwidth_bits, pph = 1 << c->planepixelheight_bits;
	const int pagewh = 1 << c->pagewh_bits, pagesize = 1 << (c->pagewh_bits * 2);
	const int planew = 1 << c->planew_bits, pds = 1 << (c->patterndatasize_bits + 1);
	const int cellwh = 1 << (2 + c->patternwh);
	const float zx = r->zx >= 0.0f ? r->zx : 1.0f;
	int xx = (r->x + (int)(i * zx)) % (c->xmask + 1);
	if (r->lsx)
		xx = (xx + r->lsx) % (c->xmask + 1); // ADR-099
	const int yy = r->y;
	const int pr = r->priority & 15, spm = r->priority >> 4;
	if (c->isbitmap) {
		/* bitmap layer (ADR-039, same as the GPU shader / Vdp2FetchPixel) */
		const int bidx = yy * (c->xmask + 1) + xx, cnb = c->colornumber, ca = c->bm_charaddr;
		int dotb, msbb = 0;
		u8 cb[3];
		if (cnb <= 1) {
			if (cnb == 0) { dotb = rd8(ca + bidx / 2); if (xx % 2 == 0) dotb >>= 4; dotb &= 15; }
			else dotb = rd8(ca + bidx);
			if (dotb == 0 && c->transparencyenable) return 0;
			const u8 *pp = s_pal[(c->coloroffset + c->bm_paladdr + dotb) % 2048];
			memcpy(cb, pp, 3); msbb = pp[3];
		} else if (cnb == 2) {
			dotb = rd16(ca + bidx * 2);
			if (dotb == 0 && c->transparencyenable) return 0;
			const u8 *pp = s_pal[(c->coloroffset + dotb) % 2048];
			memcpy(cb, pp, 3); msbb = pp[3];
		} else if (cnb == 3) {
			dotb = rd16(ca + bidx * 2);
			if (dotb < 0x8000 && c->transparencyenable) return 0;
			cb[0] = (dotb & 31) * 8; cb[1] = ((dotb >> 5) & 31) * 8; cb[2] = ((dotb >> 10) & 31) * 8;
		} else {
			const int hi = rd16(ca + bidx * 4), lo = rd16(ca + bidx * 4 + 2);
			dotb = lo;
			if (hi < 0x8000 && c->transparencyenable) return 0;
			cb[0] = lo & 0xFF; cb[1] = lo >> 8; cb[2] = hi & 0xFF;
		}
		if (pr == 0)
			return 0;
		const int scm = c->specialcolormode;
		int use = 1;
		if ((scm == 1 || scm == 2) && !c->bm_scf) use = 0;
		else if (scm == 2) { if (!((c->specialcode >> ((dotb & 15) / 2)) & 1)) use = 0; }
		else if (scm == 3 && msbb < 128) use = 0;
		const int flag = (use && (c->alpha & 0xFF) != 63) ? 8 : 0;
		return (u32)cb[0] | (u32)cb[1] << 8 | (u32)cb[2] << 16 | (u32)(pr + flag) << 24;
	}
	const int pn = (yy / pph) * c->mapwh + xx / ppw;
	const int lx = xx % ppw, ly = yy % pph;
	const u32 pt = r->planetbl[pn] & 0xFFFFFF;
	const u32 addr = pt + ((ly / 512) * pagesize * planew + (lx / 512) * pagesize + ((ly % 512) / cellwh) * pagewh +
		(lx % 512) / cellwh) * pds;
	const int cn = c->colornumber;
	int charaddr, paladdr, flip, sf, scf;
	if (c->patterndatasize < 2) {
		const int tmp = rd16(addr);
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
		const int t1 = rd16(addr), t2 = rd16(addr + 2);
		charaddr = t2 & 0x7FFF;
		flip = t1 >> 14;
		paladdr = cn == 0 ? ((t1 & 0x7F) * 16) : (((t1 >> 4) & 7) * 256);
		sf = (t1 >> 13) & 1;
		scf = (t1 >> 12) & 1;
	}
	if (c->charaddr_mask_3fff)
		charaddr &= 0x3FFF;
	charaddr *= 32;
	const int prio = (spm == 1) ? ((pr / 2) * 2 + sf) : pr;
	int cx, cy;
	if (c->patternwh < 2) {
		cx = lx % 8; cy = ly % 8;
		if (flip & 1) cx = 7 - cx;
		if (flip & 2) cy = 7 - cy;
	} else {
		int y16 = ly % 16, x16 = lx % 16;
		if (flip) {
			if (flip & 2) y16 = y16 < 8 ? (7 - y16 + 16) : (15 - y16);
			else if (y16 >= 8) y16 += 8;
			if (flip & 1) { if (x16 < 8) y16 += 8; cx = 7 - x16 % 8; }
			else if (x16 >= 8) { y16 += 8; cx = x16 % 8; }
			else cx = x16 % 8;
		} else {
			if (y16 >= 8) y16 += 8;
			if (x16 >= 8) y16 += 8;
			cx = x16 % 8;
		}
		cy = y16;
	}
	const int off = cy * 8 + cx;
	int dotv, msb = 0;
	u8 col[3];
	const int transp = c->transparencyenable;
	if (cn == 0) {
		dotv = rd8(charaddr + off / 2);
		if (cx % 2 == 0) dotv >>= 4;
		if ((dotv & 15) == 0 && transp) return 0;
		const u8 *p = s_pal[(c->coloroffset + paladdr + (dotv & 15)) % 2048];
		memcpy(col, p, 3); msb = p[3];
	} else if (cn == 1) {
		dotv = rd8(charaddr + off);
		if (dotv == 0 && transp) return 0;
		const u8 *p = s_pal[(c->coloroffset + paladdr + dotv) % 2048];
		memcpy(col, p, 3); msb = p[3];
	} else if (cn == 2) {
		dotv = rd16(charaddr + off * 2);
		if (dotv == 0 && transp) return 0;
		const u8 *p = s_pal[(c->coloroffset + dotv) % 2048];
		memcpy(col, p, 3); msb = p[3];
	} else if (cn == 3) {
		dotv = rd16(charaddr + off * 2);
		if (dotv < 0x8000 && transp) return 0;
		col[0] = (dotv & 31) * 8; col[1] = ((dotv >> 5) & 31) * 8; col[2] = ((dotv >> 10) & 31) * 8;
	} else {
		const int hi = rd16(charaddr + off * 4), lo = rd16(charaddr + off * 4 + 2);
		dotv = lo;
		if (hi < 0x8000 && transp) return 0;
		col[0] = lo & 0xFF; col[1] = lo >> 8; col[2] = hi & 0xFF;
	}
	if (prio == 0)
		return 0;
	const int scm = c->specialcolormode;
	int use = 1;
	if ((scm == 1 || scm == 2) && !scf) use = 0;
	else if (scm == 2) { if (!((c->specialcode >> ((dotv & 15) / 2)) & 1)) use = 0; }
	else if (scm == 3 && msb < 128) use = 0;
	const int flag = (use && (c->alpha & 0xFF) != 63) ? 8 : 0;
	return (u32)col[0] | (u32)col[1] << 8 | (u32)col[2] << 16 | (u32)(prio + flag) << 24;
}

static u32 titan_px(const struct PixelData *p)
{
	if (!p->priority)
		return 0;
	const u32 ab = p->pixel >> 24;
	return (p->pixel & 0x00FFFFFFu) | ((p->priority | ((ab != 0x3F) ? 8u : 0u)) << 24);
}

// ---- cell quad path (ADR-014, vita/src/video/nbgq.c): software rasterize with the same rules as the shader ----
static uint8_t s_a4[NBGQ_ATLAS * NBGQ_ATLAS], s_a8[NBGQ_ATLAS * NBGQ_ATLAS];
static NbgQuad s_quads[16384];
static u32 s_qbuf[256][704];
static unsigned long long s_q_layers, s_q_bad, s_q_over, s_q_unsup, s_q_count;
static unsigned long long s_q_unsup_cn[8];
static int s_q_reported;

static uint8_t s_argb[1024 * 1024 * 4];

static u32 quad_px(const RstvLayerCapture *c, const NbgQuad *q, int x, int y)
{
	/* GPU: texel = floor(interpolated coordinate at the pixel centre) (ADR-052: a quad may stretch one atlas row) */
	const double fu = q->u0 + (x - q->x0 + 0.5) * (double)(q->u1 - q->u0) / (q->x1 - q->x0);
	const double fv = q->v0 + (y - q->y0 + 0.5) * (double)(q->v1 - q->v0) / (q->y1 - q->y0);
	const int u = (int)floor(fu), v = (int)floor(fv);
	if (c->colornumber == 3) {
		// RGB555 (vita s_fs_qrgb): RGB atlas
		const uint8_t *px = s_argb + (v * 1024 + u) * 4;
		if (!px[3] && c->transparencyenable)
			return 0;
		const int dotv = px[0] >> 3, scm = c->specialcolormode;
		int use = 1;
		if ((scm == 1 || scm == 2) && !q->scf) use = 0;
		else if (scm == 2) { if (!((c->specialcode >> ((dotv & 15) / 2)) & 1)) use = 0; }
		else if (scm == 3) use = 0;
		const int flag = (use && (c->alpha & 0xFF) != 63) ? 8 : 0;
		return (u32)px[0] | (u32)px[1] << 8 | (u32)px[2] << 16 | (u32)(q->prio + flag) << 24;
	}
	const int idx = (c->colornumber == 0 ? s_a4 : s_a8)[v * NBGQ_ATLAS + u];
	if (idx == 0 && c->transparencyenable)
		return 0;
	const u8 *p = s_pal[(q->palbase + idx) & 2047];
	const int scm = c->specialcolormode;
	int use = 1;
	if ((scm == 1 || scm == 2) && !q->scf) use = 0;
	else if (scm == 2) { if (!((c->specialcode >> ((idx & 15) / 2)) & 1)) use = 0; }
	else if (scm == 3 && p[3] < 128) use = 0;
	const int flag = (use && (c->alpha & 0xFF) != 63) ? 8 : 0;
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)(q->prio + flag) << 24;
}

static unsigned long long s_q_zoomrows;
static void check_quads(int L, const RstvLayerCapture *c, const struct PixelData *buf, int tw)
{
	if (!nbgq_supported(c) || c->isbitmap) {
		s_q_unsup++;
		s_q_unsup_cn[c->colornumber & 7]++;
		return;
	}
	const int w = vdp2width, h = vdp2height < 256 ? vdp2height : 256;
	if (c->colornumber == 3)
		rbgq_atlas_rgb_update(s_argb, 1024, Vdp2Ram, NULL);
	const int qflags = c->zx != 0.0f ? NBGQ_SKIP_ZOOM | NBGQ_ZOOM_RUNS : 0; /* as gvdp2 (ADR-052) */
	const int n = nbgq_build(c, w, h, Vdp2Ram, s_quads, 16384, qflags);
	if (n < 0) {
		s_q_over++;
		return;
	}
	s_q_layers++;
	s_q_count += n;
	memset(s_qbuf, 0, sizeof(s_qbuf));
	for (int k = 0; k < n; k++) {
		const NbgQuad *q = &s_quads[k];
		for (int y = q->y0; y < q->y1; y++)
			for (int x = q->x0; x < q->x1; x++)
				s_qbuf[y][x] = quad_px(c, q, x, y);
	}
	for (int y = 0; y < h; y++) {
		if (qflags && y < c->nrows && c->row[y].enable && c->row[y].zx != 1.0f && !NBGQ_RUN_ZX(c->row[y].zx))
			continue; /* drawn by the pixel shader on the Vita */
		if (qflags && y < c->nrows && c->row[y].enable && NBGQ_RUN_ZX(c->row[y].zx))
			s_q_zoomrows++;
		for (int x = 0; x < w; x++) {
			const u32 want = titan_px(buf + y * tw + x), got = s_qbuf[y][x];
			if (want != got) {
				s_q_bad++;
				if (s_q_reported < 20) {
					s_q_reported++;
					printf("quadcheck: frame %llu L%d (%d,%d) cpu %08x quad %08x cn %d pds %d pwh %d aux %d scm %d\n",
						s_frames, L, x, y, want, got, c->colornumber, c->patterndatasize, c->patternwh, c->auxmode,
						c->specialcolormode);
				}
			}
		}
	}
}

// ---- RBG0 rotation path (ADR-016, vita/src/video/rbgq.c) ----
static uint8_t s_pages[512 * 512 * 4], s_table[8 * 8 * 4], s_table_b[8 * 8 * 4];
static RbgqState s_rbgq;
static unsigned long long s_r_frames, s_r_bad, s_r_pixels, s_r_fail, s_r_win;
static int s_r_reported;

static void check_rbg(void)
{
	const RstvRotCapture *c = &rstv_rot_capture;
	rbgq_atlas_rgb_update(s_argb, 1024, Vdp2Ram, NULL);
	static int flat = -1;
	if (flat < 0)
		flat = getenv("RSTV_RBGFLAT") ? 1 : 0;
	const int fl = flat && !c->two_maps;
	int have_b = 0;
	if (c->two_maps && !c->isbitmap) {
		// B map (same page cell, own table): B first, same order as gvdp2
		static RstvRotCapture cb;
		memcpy(&cb, c, offsetof(RstvRotCapture, nrows));
		memcpy(cb.planetbl, c->planetbl_b, sizeof(cb.planetbl));
		cb.screenover = c->screenover_b;
		have_b = rbgq_prepare(&s_rbgq, &cb, Vdp2Ram, NULL, 0, (uint32_t)s_frames, s_pages, s_table_b, 0) == 0;
	}
	{
		static int shown;
		if (!shown++)
			printf("rbgcheck: layout %s (two_maps %d)%c", fl ? "flat" : "pages", c->two_maps, 10);
	}
	if (rbgq_prepare(&s_rbgq, c, Vdp2Ram, NULL, 0, (uint32_t)s_frames, s_pages, s_table, fl) < 0) {
		s_r_fail++;
		if (getenv("RSTV_ROTLOG"))
			printf("rbgcheck: frame %llu prepare failed (map %dx%d cn %d bmp %d so %d)%c", s_frames, c->mapw, c->maph, c->colornumber,
				c->isbitmap, c->screenover, 10);
		return;
	}
	s_r_frames++;
	for (int y = 0; y < c->nrows; y++)
		if (c->row[y].nseg) {
			if (!s_r_win++)
				printf("rbgcheck: first frame with line pieces %llu\n", s_frames);
			break;
		}
	rstv_vidsoft_draw_layer(4);
	const unsigned long long bad0 = s_r_bad;
	int tw = 0;
	const struct PixelData *buf = TitanGetLayerBuffer(4, &tw);
	const int w = vdp2width, h = vdp2height < 256 ? vdp2height : 256;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const u32 want = titan_px(buf + y * tw + x);
			const u32 got = rbgq_shade(c, s_pages, s_table, have_b ? s_table_b : NULL, s_a4, s_a8, s_argb, (const uint8_t (*)[4])s_pal, x, y, fl);
			s_r_pixels++;
			if (want != got) {
				s_r_bad++;
				if (s_r_reported < 20) {
					s_r_reported++;
					printf("rbgcheck: frame %llu (%d,%d) cpu %08x gpu-ref %08x cn %d so %d map %dx%d\n", s_frames, x, y, want, got,
						c->colornumber, c->screenover, c->mapw, c->maph);
					{
						const RstvRotLine *rr = &c->row[y];
						printf("   row: en %d x0 %.2f dx %.4f y0 %.2f dy %.4f nseg %d sp0 %d sx0 %d ex0 %d enb %d xb0 %.2f yb0 %.2f so_b %d\n", rr->enable, rr->x0,
							rr->dx, rr->y0, rr->dy, rr->nseg, rr->sp[0], rr->sx[0], rr->ex[0], rr->enb, rr->xb0, rr->yb0, c->screenover_b);
					}
				}
			}
		}
	if (getenv("RSTV_ROTLOG") && c->row[0].dy == 0.0f && c->row[0].dx != 0.0f) // ADR-083: unrotated frames
		printf("rbgcheck: frame %llu unrotated x0 %.2f dx %.4f y0 %.2f mismatches %llu%c", s_frames, c->row[0].x0, c->row[0].dx,
			c->row[0].y0, s_r_bad - bad0, 10);
}

static void check_draw_screens(void)
{
	for (int L = 0; L < 6; L++)
		rstv_layer_capture[L].captured = 0;
	rstv_rot_capture.captured = 0;
	rstv_gpu_compose = 1;
	rstv_capture_mode = 1;
	VIDSoftVdp2DrawScreens();
	rstv_capture_mode = 0;
	{
		extern int rstv_rot_why;
		static int shown;
		if (rstv_rot_why && shown < 3) {
			shown++;
			printf("rbgcheck: frame %llu rot why %x RPMD %x WCTLC %x WCTLD %x\n", s_frames, rstv_rot_why, Vdp2Regs->RPMD, Vdp2Regs->WCTLC, Vdp2Regs->WCTLD);
		}
	}
	if (getenv("RSTV_LINEDIFF") && (s_frames % 300) == 0) {
		// which Vdp2 register words differ between consecutive lines (ADR-031 diagnosis)
		static unsigned cnt[sizeof(Vdp2) / 2];
		memset(cnt, 0, sizeof(cnt));
		int same = 0;
		for (int y = 1; y < 224; y++) {
			const u16 *a = (const u16 *)&Vdp2Lines[y], *b = (const u16 *)&Vdp2Lines[y - 1];
			int d = 0;
			for (unsigned k = 0; k < sizeof(Vdp2) / 2; k++)
				if (a[k] != b[k]) { cnt[k]++; d = 1; }
			same += !d;
		}
		printf("linediff frame %llu: same lines %d/223:", s_frames, same);
		for (unsigned k = 0; k < sizeof(Vdp2) / 2; k++)
			if (cnt[k]) printf(" %03x:%u", k * 2, cnt[k]);
		printf("\n");
	}
	if (getenv("RSTV_CAPCHK")) {
		// ADR-031 check: capture again recomputing every line and compare
		static RstvLayerCapture saved[6];
		static unsigned long long nchk, nbad;
		extern int rstv_cap_same;
		memcpy(saved, rstv_layer_capture, sizeof(saved));
		for (int L = 0; L < 6; L++)
			rstv_layer_capture[L].captured = 0;
		rstv_cap_same = 0;
		rstv_capture_mode = 1;
		VIDSoftVdp2DrawScreens();
		rstv_capture_mode = 0;
		rstv_cap_same = 1;
		for (int L = 0; L < 6; L++) {
			nchk++;
			if (memcmp(&saved[L], &rstv_layer_capture[L], sizeof(saved[L]))) {
				if (nbad++ < 8) {
					const RstvLayerCapture *a = &saved[L], *b = &rstv_layer_capture[L];
					printf("capchk: frame %llu layer %d differs: ok %d/%d why %x/%x nrows %d/%d\n", s_frames, L, a->ok, b->ok, a->why,
						b->why, a->nrows, b->nrows);
					{
						extern unsigned char rstv_line_same[512];
						for (int y = 0; y < 5; y++)
							printf("  line %d same %d BGON %04x cmp %d\n", y, rstv_line_same[y], Vdp2Lines[y].BGON,
								y ? memcmp(&Vdp2Lines[y], &Vdp2Lines[y - 1], sizeof(Vdp2)) : -9);
					}
					for (int r = 0; r < RSTV_CAP_ROWS; r++)
						if (memcmp(&a->row[r], &b->row[r], sizeof(a->row[r]))) {
							printf("  row %d: en %d/%d pr %d/%d x %d/%d lsx %d/%d y %d/%d pt0 %x/%x\n", r, a->row[r].enable,
								b->row[r].enable, a->row[r].priority, b->row[r].priority, a->row[r].x, b->row[r].x, a->row[r].lsx,
								b->row[r].lsx, a->row[r].y, b->row[r].y, (unsigned)a->row[r].planetbl[0], (unsigned)b->row[r].planetbl[0]);
							break;
						}
				}
			}
		}
		if ((s_frames % 600) == 0)
			printf("capchk: %llu checks, %llu differ\n", nchk, nbad);
	}
	build_palette();
	nbgq_atlas_update(s_a4, s_a8, NBGQ_ATLAS, Vdp2Ram, NULL);
	s_frames++;
	for (int L = 0; L < 4; L++) {
		const RstvLayerCapture *c = &rstv_layer_capture[L];
		if (!c->captured)
			continue;
		if (!c->ok) {
			s_fallback++;
			for (int b = 0; b < 16; b++)
				if (c->why & (1 << b))
					s_why[b]++;
			continue;
		}
		s_layers++;
		rstv_vidsoft_draw_layer(L);
		int tw = 0;
		const struct PixelData *buf = TitanGetLayerBuffer(L, &tw);
		check_quads(L, c, buf, tw);
		for (int y = 0; y < vdp2height && y < 256; y++)
			for (int x = 0; x < vdp2width; x++) {
				const u32 want = titan_px(buf + y * tw + x), got = shade(c, x, y);
				s_checked++;
				if (want != got) {
					s_bad++;
					if (s_reported < 20) {
						s_reported++;
						printf("gpucheck: frame %llu L%d (%d,%d) cpu %08x ref %08x cn %d pds %d pwh %d aux %d scm %d\n",
							s_frames, L, x, y, want, got, c->colornumber, c->patterndatasize, c->patternwh, c->auxmode,
							c->specialcolormode);
					}
				}
			}
	}
	if (rstv_rot_capture.captured)
		check_rbg();
	rstv_gpu_compose = 0;
	s_orig(); // harness hash uses the original path result
}

void nbgcheck_install(void)
{
	s_orig = VIDSoft.Vdp2DrawScreens;
	VIDSoft.Vdp2DrawScreens = check_draw_screens;
}

void nbgcheck_report(void)
{
	printf("gpucheck: frames %llu gpu layers %llu cpu-fallback layers %llu pixels %llu mismatches %llu\n", s_frames,
		s_layers, s_fallback, s_checked, s_bad);
	printf("quadcheck: layers %llu (quads/layer %.0f) mismatches %llu, over-limit %llu, unsupported %llu\n", s_q_layers,
		s_q_layers ? (double)s_q_count / s_q_layers : 0.0, s_q_bad, s_q_over, s_q_unsup);
	printf("quadcheck: unsupported by colour mode: cn2 %llu cn3 %llu cn4 %llu\n", s_q_unsup_cn[2], s_q_unsup_cn[3], s_q_unsup_cn[4]);
	printf("quadcheck: magnified lines drawn as texel runs %llu\n", s_q_zoomrows);
	printf("rbgcheck: frames with line pieces %llu\n", s_r_win);
	printf("rbgcheck: frames %llu pixels %llu mismatches %llu (%.4f%%), prepare failed %llu\n", s_r_frames, s_r_pixels, s_r_bad,
		s_r_pixels ? 100.0 * s_r_bad / s_r_pixels : 0.0, s_r_fail);
	printf("gpucheck: fallback why bits:");
	for (int b = 0; b < 16; b++)
		if (s_why[b])
			printf(" [%x]=%llu", 1 << b, s_why[b]);
	printf("\n");
}
