// Screen output (ADR-015: direct sceGxm). Scales one emulated screen texture to 4:3 and puts FPS text at top-left.
#include "video/video.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

#include "platform/log.h"
#include "video/gx.h"

#define NUM_TEX 3

static GxTex *s_tex[NUM_TEX];
static int s_w[NUM_TEX], s_h[NUM_TEX];
static int s_draw;
static int s_ready = -1;
// GPU composite frame (gvdp2)
static GxTex *s_gpu_tex;
static int s_gpu_w, s_gpu_h, s_gpu_last;

static GxProg *s_prog_img, *s_prog_osd, *s_prog_fill, *s_prog_layer; // layer: premultiplied alpha (ui.c, ADR-095)
static GxParam s_u_layer_scale, s_u_layer_tex;
static GxParam s_u_img_scale, s_u_img_tex, s_u_osd_scale, s_u_osd_tex, s_u_fill_scale, s_u_fill_color;

// pixel coords (960x544, top = 0) -> NDC. Texture coords unchanged
static const char *s_vs_img =
	"void main(float2 position, float2 texcoord, uniform float4 scale,\n"
	"  float4 out vPosition : POSITION, float2 out vTex : TEXCOORD0)\n"
	"{\n"
	"  vPosition = float4(position * scale.xy - 1.0, 0.0, 1.0);\n"
	"  vTex = texcoord;\n"
	"}\n";
static const char *s_fs_img =
	"float4 main(float2 vTex : TEXCOORD0, uniform sampler2D tex) : COLOR\n"
	"{\n"
	"  return tex2D(tex, vTex);\n"
	"}\n";
static const char *s_vs_fill =
	"void main(float2 position, uniform float4 scale, float4 out vPosition : POSITION)\n"
	"{\n"
	"  vPosition = float4(position * scale.xy - 1.0, 0.0, 1.0);\n"
	"}\n";
static const char *s_fs_fill =
	"float4 main(uniform float4 color) : COLOR\n"
	"{\n"
	"  return color;\n"
	"}\n";

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f)
		fclose(f);
	return f != NULL;
}

void rstv_video_init(void)
{
	// instrumentation: disable vsync if ux0:data/restv/novsync exists
	if (gx_init(!file_exists("ux0:data/restv/novsync")) < 0) {
		RSTV_LOG("video: gx_init failed");
		return;
	}
	for (int i = 0; i < NUM_TEX; i++) {
		s_tex[i] = gx_tex(RSTV_VIDEO_TEX_W, RSTV_VIDEO_TEX_H, GX_RGBA8, 1, 0);
		s_w[i] = 320;
		s_h[i] = 224;
	}
	const GxAttr img_attr[2] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 }, { "texcoord", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 8 } };
	s_prog_img = gx_prog(s_vs_img, s_fs_img, img_attr, 2, 16, GX_BLEND_NONE);
	s_prog_osd = gx_prog(s_vs_img, s_fs_img, img_attr, 2, 16, GX_BLEND_ALPHA);
	s_prog_layer = gx_prog(s_vs_img, s_fs_img, img_attr, 2, 16, GX_BLEND_PREMUL);
	const GxAttr fill_attr[1] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 } };
	s_prog_fill = gx_prog(s_vs_fill, s_fs_fill, fill_attr, 1, 8, GX_BLEND_NONE);
	if (s_prog_img) {
		s_u_img_scale = gx_param(s_prog_img, "scale");
		s_u_img_tex = gx_param(s_prog_img, "tex");
	}
	if (s_prog_osd) {
		s_u_osd_scale = gx_param(s_prog_osd, "scale");
		s_u_osd_tex = gx_param(s_prog_osd, "tex");
	}
	if (s_prog_layer) {
		s_u_layer_scale = gx_param(s_prog_layer, "scale");
		s_u_layer_tex = gx_param(s_prog_layer, "tex");
	}
	if (s_prog_fill) {
		s_u_fill_scale = gx_param(s_prog_fill, "scale");
		s_u_fill_color = gx_param(s_prog_fill, "color");
	}
	RSTV_LOG("video: gxm init, programs %s", (s_prog_img && s_prog_osd && s_prog_fill) ? "ok" : "FAILED");
}

void rstv_video_shutdown(void)
{
	gx_shutdown();
}

uint32_t *rstv_video_frame_begin(int w, int h)
{
	gx_frame_begin(); // until the GPU has finished reading this texture (triple buffering + frame fence)
	s_w[s_draw] = w;
	s_h[s_draw] = h;
	return (uint32_t *)s_tex[s_draw]->data;
}

void rstv_video_frame_end(void)
{
	s_ready = s_draw;
	s_draw = (s_draw + 1) % NUM_TEX;
	s_gpu_last = 0;
}

void rstv_video_frame_gpu(GxTex *tex, int w, int h)
{
	s_gpu_tex = tex;
	s_gpu_w = w;
	s_gpu_h = h;
	s_gpu_last = 1;
}

// ---- on-screen text (FPS) ----
#define OSD_N 3
#define OSD_W 256
#define OSD_H 9
#define OSD_SCALE 3
static GxTex *s_osd_tex[OSD_N];
static int s_osd_cur = -1, s_osd_len;

static const uint8_t *osd_glyph(char c)
{
	// 5x7 font: capitals, digits, a few signs (small letters use the capitals)
	static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-/()>!+=_";
	static const uint8_t glyph[][7] = {
		{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
		{ 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
		{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
		{ 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
		{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
		{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
		{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
		{ 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
		{ 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
		{ 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
		{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
		{ 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
		{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
		{ 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
		{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
		{ 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
		{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
		{ 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
		{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
		{ 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
		{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
		{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
		{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
		{ 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
		{ 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 },
		{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
		{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
		{ 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
		{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
		{ 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
		{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
		{ 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
		{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
		{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
		{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
		{ 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
		{ 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },
		{ 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
		{ 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 },
		{ 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 },
		{ 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 },
		{ 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 },
		{ 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },
		{ 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },
		{ 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F }
	};
	static const uint8_t space[7] = { 0 };
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	for (int i = 0; chars[i]; i++)
		if (chars[i] == c)
			return glyph[i];
	return space;
}

void rstv_video_set_osd(const char *text)
{
	if (!text) {
		s_osd_len = 0;
		return;
	}
	if (!s_osd_tex[0])
		for (int i = 0; i < OSD_N; i++)
			s_osd_tex[i] = gx_tex(OSD_W, OSD_H, GX_RGBA8, 0, 0);
	if (!s_osd_tex[0])
		return;
	const int next = (s_osd_cur + 1) % OSD_N;
	uint32_t *px = (uint32_t *)s_osd_tex[next]->data;
	int len = 0;
	while (text[len] && len < (OSD_W - 2) / 6)
		len++;
	// translucent black background + white text (ABGR)
	for (int i = 0; i < OSD_W * OSD_H; i++)
		px[i] = 0x80000000u;
	for (int n = 0; n < len; n++) {
		const uint8_t *g = osd_glyph(text[n]);
		for (int y = 0; y < 7; y++)
			for (int x = 0; x < 5; x++)
				if (g[y] & (0x10 >> x))
					px[(y + 1) * OSD_W + 1 + n * 6 + x] = 0xFFFFFFFFu;
	}
	s_osd_len = len;
	s_osd_cur = next;
}

// ---- text list (game select menu) ----
#define MENU_W 256
#define MENU_LINES 16
#define MENU_LH 10
static GxTex *s_menu_tex[2];
static int s_menu_cur = -1, s_menu_rows;

void rstv_video_menu(const char *const *lines, int n, int sel)
{
	if (!lines) {
		s_menu_rows = 0;
		return;
	}
	if (!s_menu_tex[0])
		for (int i = 0; i < 2; i++)
			s_menu_tex[i] = gx_tex(MENU_W, MENU_LINES * MENU_LH, GX_RGBA8, 0, 0);
	if (!s_menu_tex[0] || !s_menu_tex[1])
		return;
	if (n > MENU_LINES)
		n = MENU_LINES;
	const int next = (s_menu_cur + 1) & 1; // the GPU may still read the other one
	uint32_t *px = (uint32_t *)s_menu_tex[next]->data;
	for (int y = 0; y < MENU_LINES * MENU_LH; y++) {
		const uint32_t bg = (y / MENU_LH == sel) ? 0xFF905020u : 0xD0000000u; // ABGR: selected line blue
		for (int x = 0; x < MENU_W; x++)
			px[y * MENU_W + x] = bg;
	}
	for (int r = 0; r < n; r++) {
		const char *t = lines[r];
		for (int c = 0; t && t[c] && c < (MENU_W - 4) / 6; c++) {
			const uint8_t *g = osd_glyph(t[c]);
			for (int y = 0; y < 7; y++)
				for (int x = 0; x < 5; x++)
					if (g[y] & (0x10 >> x))
						px[(r * MENU_LH + 2 + y) * MENU_W + 2 + c * 6 + x] = 0xFFFFFFFFu;
		}
	}
	s_menu_rows = n;
	s_menu_cur = next;
}

// one quad (triangle strip): pixel coords x0,y0 - x1,y1, texture coords u0,v0 - u1,v1
static void quad(const GxProg *p, GxParam scale, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
	float *v = (float *)gx_temp(4 * 4 * sizeof(float));
	if (!v)
		return;
	const float d[16] = { x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1 };
	memcpy(v, d, sizeof(d));
	const float sc[4] = { 2.0f / 960.0f, 2.0f / 544.0f, 0, 0 };
	(void)p;
	gx_set4(scale, sc);
	gx_draw(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, gx_seq_indices(4), 4);
}

static struct { GxTex *t; int x, y, w, h; } s_layer[4];
void rstv_video_layer(int slot, struct GxTex *t, int x, int y, int w, int h)
{
	if (slot < 0 || slot >= 4)
		return;
	s_layer[slot].t = t;
	s_layer[slot].x = x;
	s_layer[slot].y = y;
	s_layer[slot].w = w;
	s_layer[slot].h = h;
}

static int s_scale_mode, s_filter = 1;
void rstv_video_set_mode(int scale, int filter)
{
	s_scale_mode = scale;
	s_filter = filter;
}

// ---- ADR-126: scanlines and border (same math as Reirem frontend/video.c) ----
static int s_scan_type, s_scan_strength, s_border, s_border_color;
void rstv_video_set_style(int scan_type, int scan_strength, int border, int border_color)
{
	s_scan_type = scan_type;
	s_scan_strength = scan_strength;
	s_border = border;
	s_border_color = border_color;
}

// scanlines: stretch an 8-wide translucent black texture over the game screen (height = drawn height dh, one row = one screen line).
// CRT smooth: average of a cos curve (darkest at game line boundaries) over the game line span covered by one screen line.
// CRT sharp: darken if the screen line falls in the lower half of its game line. 2/3/4 pixels: screen pixel spacing (independent of game lines)
#define SCAN_MAX_H 544
static GxTex *s_scan_tex[2];
static int s_scan_cur = -1, s_scan_k_type = -1, s_scan_k_str = -1, s_scan_k_dh = -1, s_scan_k_lines = -1;
static GxTex *scan_texture(int dh, int lines)
{
	if (s_scan_type <= 0 || dh <= 0 || dh > SCAN_MAX_H || lines <= 0)
		return NULL;
	if (s_scan_cur >= 0 && s_scan_type == s_scan_k_type && s_scan_strength == s_scan_k_str && dh == s_scan_k_dh &&
		lines == s_scan_k_lines)
		return s_scan_tex[s_scan_cur];
	if (!s_scan_tex[0])
		for (int i = 0; i < 2; i++)
			s_scan_tex[i] = gx_tex(8, SCAN_MAX_H, GX_RGBA8, 0, 0);
	if (!s_scan_tex[0] || !s_scan_tex[1])
		return NULL;
	const int next = (s_scan_cur + 1) & 1; // the other one may still be read by the GPU
	static const float alpha[3] = { 70.0f, 120.0f, 175.0f };
	const float a = alpha[s_scan_strength < 0 ? 0 : s_scan_strength > 2 ? 2 : s_scan_strength];
	const float pi2 = 6.2831853f;
	uint32_t *p = (uint32_t *)s_scan_tex[next]->data;
	const int stride = s_scan_tex[next]->stride / 4;
	for (int y = 0; y < dh; y++) {
		float dark = 0.0f;
		switch (s_scan_type) {
		case 1: {
			const float u0 = (float)y * lines / dh, u1 = (float)(y + 1) * lines / dh;
			const float F0 = u0 * 0.5f + sinf(pi2 * u0) / (2.0f * pi2), F1 = u1 * 0.5f + sinf(pi2 * u1) / (2.0f * pi2);
			dark = (F1 - F0) / (u1 - u0);
			break;
		}
		case 2: {
			const float pos = (y + 0.5f) * lines / dh;
			dark = (pos - (int)pos) >= 0.5f ? 1.0f : 0.0f;
			break;
		}
		case 3: dark = (y % 2) == 1; break;
		case 4: dark = (y % 3) == 2; break;
		default: dark = (y % 4) == 3; break;
		}
		const uint32_t c = (uint32_t)(a * dark + 0.5f) << 24; // ABGR: black, alpha = darkness
		for (int x = 0; x < 8; x++)
			p[y * stride + x] = c;
	}
	s_scan_cur = next;
	s_scan_k_type = s_scan_type;
	s_scan_k_str = s_scan_strength;
	s_scan_k_dh = dh;
	s_scan_k_lines = lines;
	return s_scan_tex[next];
}

// border solid colors (same 6 colors as Reirem)
static void border_rgba(float *c)
{
	static const uint8_t col[6][3] = { { 0x14, 0x18, 0x3C }, { 0x28, 0x28, 0x2C }, { 0x48, 0x10, 0x14 },
		{ 0x10, 0x34, 0x1C }, { 0x2C, 0x14, 0x48 }, { 0x5A, 0x24, 0x18 } };
	const int k = s_border_color < 0 || s_border_color > 5 ? 0 : s_border_color;
	c[0] = col[k][0] / 255.0f;
	c[1] = col[k][1] / 255.0f;
	c[2] = col[k][2] / 255.0f;
	c[3] = 1.0f;
}

// artwork: full-screen background supplied by the owner, ux0:data/restv/artwork/<set>.png (.jpg, .bmp), stretched to the screen.
// set name, else parent set name. Not shipped with the app (copyright). Solid border if missing
static GxTex *s_art;
static int s_art_w, s_art_h;
static uint8_t *load_png(const char *path, int *w, int *h)
{
	png_image img;
	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, path))
		return NULL;
	img.format = PNG_FORMAT_RGBA;
	uint8_t *buf = (uint8_t *)malloc(PNG_IMAGE_SIZE(img));
	if (!buf || !png_image_finish_read(&img, NULL, buf, 0, NULL)) {
		free(buf);
		png_image_free(&img);
		return NULL;
	}
	*w = (int)img.width;
	*h = (int)img.height;
	return buf;
}
struct jerr {
	struct jpeg_error_mgr mgr;
	jmp_buf jb;
};
static void jpeg_fail(j_common_ptr ci) { longjmp(((struct jerr *)ci->err)->jb, 1); }
static uint8_t *load_jpg(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	struct jpeg_decompress_struct ci;
	struct jerr je;
	uint8_t *volatile buf = NULL;
	uint8_t *volatile row = NULL;
	ci.err = jpeg_std_error(&je.mgr);
	je.mgr.error_exit = jpeg_fail;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		fclose(f);
		free(buf);
		free(row);
		return NULL;
	}
	jpeg_create_decompress(&ci);
	jpeg_stdio_src(&ci, f);
	jpeg_read_header(&ci, TRUE);
	ci.out_color_space = JCS_RGB;
	jpeg_start_decompress(&ci);
	const int iw = (int)ci.output_width, ih = (int)ci.output_height;
	buf = (uint8_t *)malloc((size_t)iw * ih * 4);
	row = (uint8_t *)malloc((size_t)iw * 3);
	if (!buf || !row)
		longjmp(je.jb, 1);
	while (ci.output_scanline < ci.output_height) {
		const int y = (int)ci.output_scanline;
		uint8_t *rp = row;
		jpeg_read_scanlines(&ci, &rp, 1);
		for (int x = 0; x < iw; x++) {
			uint8_t *o = buf + ((size_t)y * iw + x) * 4;
			o[0] = row[x * 3];
			o[1] = row[x * 3 + 1];
			o[2] = row[x * 3 + 2];
			o[3] = 255;
		}
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	fclose(f);
	free(row);
	*w = iw;
	*h = ih;
	return buf;
}
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint8_t *load_bmp(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t hd[54];
	uint8_t *buf = NULL;
	if (fread(hd, 1, 54, f) == 54 && hd[0] == 0x42 && hd[1] == 0x4D) { // "BM"
		const uint32_t off = le32(hd + 10);
		const int32_t bw = (int32_t)le32(hd + 18), bh = (int32_t)le32(hd + 22);
		const int bpp = hd[28] | hd[29] << 8, comp = hd[30];
		const int ah = bh < 0 ? -bh : bh;
		if ((bpp == 24 || bpp == 32) && (comp == 0 || comp == 3) && bw > 0 && ah > 0 && bw <= 4096 && ah <= 4096) {
			const int rs = ((bw * (bpp / 8)) + 3) & ~3;
			uint8_t *row = (uint8_t *)malloc((size_t)rs);
			buf = (uint8_t *)malloc((size_t)bw * ah * 4);
			int ok = row && buf && fseek(f, (long)off, SEEK_SET) == 0;
			for (int r = 0; ok && r < ah; r++) {
				if (fread(row, 1, (size_t)rs, f) != (size_t)rs) {
					ok = 0;
					break;
				}
				const int y = bh < 0 ? r : ah - 1 - r; // usually bottom-up
				for (int x = 0; x < bw; x++) {
					const uint8_t *sp = row + x * (bpp / 8);
					uint8_t *o = buf + ((size_t)y * bw + x) * 4;
					o[0] = sp[2];
					o[1] = sp[1];
					o[2] = sp[0];
					o[3] = 255;
				}
			}
			free(row);
			if (ok) {
				*w = bw;
				*h = ah;
			} else {
				free(buf);
				buf = NULL;
			}
		}
	}
	fclose(f);
	return buf;
}
static uint8_t *load_art_file(const char *name, int *w, int *h)
{
	char path[160];
	snprintf(path, sizeof(path), "ux0:data/restv/artwork/%s.png", name);
	uint8_t *b = load_png(path, w, h);
	if (!b) {
		snprintf(path, sizeof(path), "ux0:data/restv/artwork/%s.jpg", name);
		b = load_jpg(path, w, h);
	}
	if (!b) {
		snprintf(path, sizeof(path), "ux0:data/restv/artwork/%s.bmp", name);
		b = load_bmp(path, w, h);
	}
	return b;
}
void rstv_video_set_art(const char *set, const char *parent)
{
	s_art_w = s_art_h = 0; // (one texture: one game per run)
	if (!set)
		return;
	int w = 0, h = 0;
	uint8_t *b = load_art_file(set, &w, &h);
	if (!b && parent)
		b = load_art_file(parent, &w, &h);
	if (!b) {
		RSTV_LOG("video: %s artwork missing (Border Artwork falls back to the border color)", set);
		return;
	}
	if (w > 2048 || h > 2048 || (s_art && (w > s_art->w || h > s_art->h))) {
		RSTV_LOG("video: artwork %dx%d too large", w, h);
		free(b);
		return;
	}
	if (!s_art)
		s_art = gx_tex(w, h, GX_RGBA8, 1, 0);
	if (s_art) {
		for (int y = 0; y < h; y++)
			memcpy(s_art->data + (size_t)y * s_art->stride, b + (size_t)y * w * 4, (size_t)w * 4);
		s_art_w = w;
		s_art_h = h;
		RSTV_LOG("video: artwork %s %dx%d", set, w, h);
	}
	free(b);
}

void rstv_video_present(void)
{
	if (!s_prog_img || !s_prog_fill)
		return;
	gx_lock(); // keep clear of the worker thread's GPU submits (ADR-029)
	gx_begin(NULL);
	// black background
	{
		gx_use(s_prog_fill);
		const float black[4] = { 0, 0, 0, 1 };
		gx_set4(s_u_fill_color, black);
		float *v = (float *)gx_temp(4 * 2 * sizeof(float));
		if (v) {
			const float d[8] = { 0, 0, 960, 0, 0, 544, 960, 544 };
			memcpy(v, d, sizeof(d));
			const float sc[4] = { 2.0f / 960.0f, 2.0f / 544.0f, 0, 0 };
			gx_set4(s_u_fill_scale, sc);
			gx_draw(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, gx_seq_indices(4), 4);
		}
	}
	if (s_ready >= 0 || s_gpu_last) {
		const GxTex *t;
		int w, h;
		if (s_gpu_last) {
			t = s_gpu_tex;
			w = s_gpu_w;
			h = s_gpu_h;
		} else {
			t = s_tex[s_ready];
			w = s_w[s_ready];
			h = s_h[s_ready];
		}
		// screen size: 4:3 (fit to 544 height), integer scale, full
		float dw, dh;
		if (s_scale_mode == 1 && w > 0 && h > 0) {
			int k = 960 / w < 544 / h ? 960 / w : 544 / h;
			if (k < 1)
				k = 1;
			dw = (float)(w * k);
			dh = (float)(h * k);
		} else if (s_scale_mode == 2) {
			dw = 960.0f;
			dh = 544.0f;
		} else {
			dh = 544.0f;
			dw = dh * 4.0f / 3.0f;
		}
		const float x0 = (960.0f - dw) * 0.5f, y0 = (544.0f - dh) * 0.5f;
		if (s_border && s_scale_mode != 2) {
			// ADR-126 border: fill the whole screen with artwork (if any) or solid color, then the game screen
			if (s_border == 1 && s_art && s_art_w > 0) {
				gx_tex_filter(s_art, 1);
				gx_use(s_prog_img);
				gx_set_tex(s_u_img_tex, s_art);
				quad(s_prog_img, s_u_img_scale, 0, 0, 960, 544, 0, 0, (float)s_art_w / s_art->w, (float)s_art_h / s_art->h);
			} else {
				float col[4];
				border_rgba(col);
				gx_use(s_prog_fill);
				gx_set4(s_u_fill_color, col);
				float *v = (float *)gx_temp(4 * 2 * sizeof(float));
				if (v) {
					const float d[8] = { 0, 0, 960, 0, 0, 544, 960, 544 };
					memcpy(v, d, sizeof(d));
					const float sc[4] = { 2.0f / 960.0f, 2.0f / 544.0f, 0, 0 };
					gx_set4(s_u_fill_scale, sc);
					gx_draw(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, gx_seq_indices(4), 4);
				}
			}
		}
		gx_tex_filter((GxTex *)t, s_filter);
		gx_use(s_prog_img);
		gx_set_tex(s_u_img_tex, t);
		quad(s_prog_img, s_u_img_scale, x0, y0, x0 + dw, y0 + dh, 0, 0, (float)w / t->w, (float)h / t->h);
		// ADR-126 scanlines: game line count (half for double-line screens)
		GxTex *sc = s_prog_osd ? scan_texture((int)(dh + 0.5f), h > 256 ? h / 2 : h) : NULL;
		if (sc) {
			gx_use(s_prog_osd);
			gx_set_tex(s_u_osd_tex, sc);
			quad(s_prog_osd, s_u_osd_scale, x0, y0, x0 + dw, y0 + dh, 0, 0, 1, dh / (float)SCAN_MAX_H);
		}
	}
	if (s_prog_osd && s_menu_rows > 0 && s_menu_cur >= 0) {
		const float w = (float)MENU_W, h = (float)(s_menu_rows * MENU_LH), sc = 3.5f;
		const float x0 = (960.0f - w * sc) * 0.5f, y0 = (544.0f - h * sc) * 0.5f;
		gx_use(s_prog_osd);
		gx_set_tex(s_u_osd_tex, s_menu_tex[s_menu_cur]);
		quad(s_prog_osd, s_u_osd_scale, x0, y0, x0 + w * sc, y0 + h * sc, 0, 0, 1, h / (MENU_LINES * MENU_LH));
	}
	if (s_prog_osd && s_osd_len > 0 && s_osd_cur >= 0) {
		const float w = (float)(s_osd_len * 6 + 1), h = (float)OSD_H;
		gx_use(s_prog_osd);
		gx_set_tex(s_u_osd_tex, s_osd_tex[s_osd_cur]);
		quad(s_prog_osd, s_u_osd_scale, 8, 8, 8 + w * OSD_SCALE, 8 + h * OSD_SCALE, 0, 0, w / OSD_W, 1);
	}
	for (int k = 0; k < 4 && s_prog_layer; k++) {
		GxTex *t = s_layer[k].t;
		if (!t)
			continue;
		static int logged[4];
		if (!logged[k]) {
			logged[k] = 1;
			RSTV_LOG("video: layer %d drawn %dx%d at %d,%d", k, s_layer[k].w, s_layer[k].h, s_layer[k].x, s_layer[k].y);
		}
		gx_tex_filter(t, 0);
		gx_use(s_prog_layer);
		gx_set_tex(s_u_layer_tex, t);
		const float x0 = (float)s_layer[k].x, y0 = (float)s_layer[k].y, w = (float)s_layer[k].w, h = (float)s_layer[k].h;
		quad(s_prog_layer, s_u_layer_scale, x0, y0, x0 + w, y0 + h, 0, 0, w / t->w, h / t->h);
	}
	gx_end();
	gx_swap();
	gx_unlock();
}
