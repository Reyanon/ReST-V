// In-game menu, start screen game select, notices, FPS display (ADR-094, ADR-126). Same layout, colors, controls and features as
// Reirem (sister emulator, vita/src/frontend/menu.c, settings.c): favorites, list options, per-game settings, scanlines/border, coin/start mapping, one turbo key.
// Drawing is in ui.c (system default PGF font drawn to a CPU canvas and overlaid on the screen).
#include "frontend/menu.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/appmgr.h>
#include <psp2/shellutil.h>
#include <taihen.h>

#include "restv_kctrl.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/cheats.h"
#include "frontend/stv_list.h"
#include "frontend/ui.h"
#include "stv.h"
#include "c2.h"
#include "platform/log.h"
#include "platform/timer.h"
#include "rstv_version.h" // generated at build time (tools/git_version.cmake)
#include "video/video.h"

#define DATA_DIR "ux0:data/restv"
#define SETTINGS_PATH DATA_DIR "/settings.cfg"
#define OLD_SETTINGS_PATH DATA_DIR "/settings.txt" // (pre-ADR-126 format: "key value", migrated once on first read)
#define FAV_PATH DATA_DIR "/favorites.cfg"
#define MENU_OPEN_COMBO (SCE_CTRL_SELECT | SCE_CTRL_START)

#define SCREEN_W 960
#define SCREEN_H 544
#define PANEL_W  640
#define LINE_H   40 // owner: more room between rows (pages taller than the screen scroll)
#define ABOUT_LH 30 // About text lines
#define GL_LINE  44 // game list rows (owner: more room between rows; the list scrolls)
#define TITLE_H  56
#define FOOT_H   48
#define VALUE_X  330 // from panel left edge to the value column

// ADR-194 (owner): blue theme in the manner of RetroArch's default menu: deep blue gradient with light ribbons, white
// text, a light-blue accent and soft translucent highlight bars
#define COL_BG    UI_RGBA(0x00, 0x04, 0x14, 0xC0)
#define COL_PANEL UI_RGBA(0x0A, 0x1C, 0x46, 0xFF) // opaque: filled without reading what is under it (fast)
#define COL_TEXT  UI_RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_DIM   UI_RGBA(0x9C, 0xB0, 0xD4, 0xFF)
#define COL_SEL   UI_RGBA(0x6C, 0xD0, 0xFF, 0xFF)
#define COL_WARN  UI_RGBA(0xFF, 0x80, 0x70, 0xFF)
#define COL_HILITE UI_RGBA(0x8C, 0xC8, 0xFF, 0x30) // selected row bar
#define COL_EDGE  UI_RGBA(0x4C, 0x8C, 0xE0, 0xFF)  // panel outline, separators

// button symbols inside text (drawn by draw_key)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"

#define STATE_SLOTS 10

// ADR-194: rounded rectangle drawn row by row (no overlapping parts, so translucent colours stay even)
static void rrect(UiCanvas *c, int x, int y, int w, int h, int r, uint32_t col)
{
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	for (int yy = 0; yy < h; yy++) {
		int in = 0;
		const int d = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
		if (d > 0) {
			const float dy = (float)d - 0.5f;
			in = r - (int)sqrtf((float)(r * r) - dy * dy);
		}
		ui_rect(c, x + in, y + yy, w - 2 * in, 1, col);
	}
}
// outline of a rounded rectangle, t px thick (rows of the outer shape minus the inner one: no overlap)
static void rrect_outline(UiCanvas *c, int x, int y, int w, int h, int r, int t, uint32_t col)
{
	if (r * 2 > h) r = h / 2;
	const int ri = r - t > 0 ? r - t : 0;
	for (int yy = 0; yy < h; yy++) {
		int in = 0;
		const int d = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
		if (d > 0) {
			const float dy = (float)d - 0.5f;
			in = r - (int)sqrtf((float)(r * r) - dy * dy);
		}
		if (yy < t || yy >= h - t) {
			ui_rect(c, x + in, y + yy, w - 2 * in, 1, col);
			continue;
		}
		int ii = 0; // inner shape inset on this row (inner rect is t smaller on every side)
		const int yi = yy - t, hi = h - 2 * t, di = yi < ri ? ri - yi : (yi >= hi - ri ? yi - (hi - ri - 1) : 0);
		if (di > 0 && ri > 0) {
			const float dy = (float)di - 0.5f;
			ii = ri - (int)sqrtf((float)(ri * ri) - dy * dy);
		}
		const int l0 = x + in, l1 = x + t + ii, r0 = x + w - t - ii, r1 = x + w - in;
		if (l1 > l0) ui_rect(c, l0, y + yy, l1 - l0, 1, col);
		if (r1 > r0) ui_rect(c, r0, y + yy, r1 - r0, 1, col);
	}
}
// ADR-194: the focus outline blinks (RetroArch-like). It is a small layer of its own (slot 1, over the menu canvas),
// drawn once per position; the blink is that layer's opacity, set every frame (no CPU drawing). Redrawing the menu at
// 30 Hz for the blink made input slow, and it still did not blink where nothing redrew (owner). Only the topmost focus
// blinks; a focus under an open window keeps a steady outline (owner: the list under a window need not blink)
#define COL_FOCUS_STILL UI_RGBA(0x6C, 0xD0, 0xFF, 0xB0)
static UiCanvas *s_focus_cv;
static int s_focus_x = -1, s_focus_y, s_focus_w, s_focus_h, s_focus_on, s_focus_want;
static int s_focus_cy0, s_focus_cy1 = SCREEN_H; // rows the focus layer may cover (the list / menu rows area)
static void focus_at(UiCanvas *c, int x, int y, int w, int h)
{
	enum { CW = SCREEN_W, CH = 56 };
	if (y - 2 < s_focus_cy0 || y + h + 2 > s_focus_cy1) {
		// partly past the rows area (while scrolling): a steady outline in the canvas, cut there like the rows
		// (owner: the layer showed over the button help for a moment)
		rrect_outline(c, x, y, w, h, 6, 2, COL_FOCUS_STILL);
		return;
	}
	s_focus_want = 1;
	if (!s_focus_cv)
		s_focus_cv = ui_canvas(1, 0, 0, CW, CH);
	if (!s_focus_cv || w + 4 > CW || h + 4 > CH)
		return;
	if (x == s_focus_x && y == s_focus_y && w == s_focus_w && h == s_focus_h && s_focus_on)
		return;
	if (w != s_focus_w || h != s_focus_h || s_focus_x < 0) {
		ui_clear(s_focus_cv);
		rrect_outline(s_focus_cv, 2, 2, w, h, 6, 2, UI_RGBA(0xD8, 0xF4, 0xFF, 0xFF));
		rrect_outline(s_focus_cv, 0, 0, w + 4, h + 4, 8, 2, UI_RGBA(0x6C, 0xD0, 0xFF, 0x50)); // soft glow outside
	}
	s_focus_x = x;
	s_focus_y = y;
	s_focus_w = w;
	s_focus_h = h;
	ui_canvas_move(s_focus_cv, x - 2, y - 2);
	ui_commit(s_focus_cv);
	s_focus_on = 1;
}
static void focus_off(void)
{
	if (s_focus_on)
		ui_hide(s_focus_cv);
	s_focus_on = 0;
}
static void focus_begin(void)
{
	s_focus_want = 0;
	s_focus_cy0 = 0;
	s_focus_cy1 = SCREEN_H;
}
static void focus_end(void)
{
	if (!s_focus_want)
		focus_off();
}
static void focus_tick(void)
{
	if (!s_focus_on)
		return;
	const float p = 0.5f + 0.5f * sinf((float)(rstv_time_us() % 1000000) * 6.2832f / 1e6f);
	rstv_video_layer_alpha(1, 0.12f + 0.88f * p);
}
// owner: favourites as a star image: five points, edges smoothed (4 x 4 samples a pixel)
static void star(UiCanvas *c, float cx, float cy, float r, uint32_t rgb)
{
	float vx[10], vy[10];
	for (int k = 0; k < 10; k++) {
		const float a = -1.5708f + k * 3.14159f / 5.0f, rr = (k & 1) ? r * 0.42f : r;
		vx[k] = cx + rr * cosf(a);
		vy[k] = cy + rr * sinf(a);
	}
	for (int py = (int)(cy - r) - 1; py <= (int)(cy + r) + 1; py++)
		for (int px = (int)(cx - r) - 1; px <= (int)(cx + r) + 1; px++) {
			int hits = 0;
			for (int s = 0; s < 16; s++) {
				const float sx = px + 0.125f + 0.25f * (s & 3), sy = py + 0.125f + 0.25f * (s >> 2);
				int in = 0;
				for (int a = 0, b = 9; a < 10; b = a++)
					if ((vy[a] > sy) != (vy[b] > sy) && sx < (vx[b] - vx[a]) * (sy - vy[a]) / (vy[b] - vy[a]) + vx[a])
						in = !in;
				hits += in;
			}
			if (hits)
				ui_rect(c, px, py, 1, 1, (rgb & 0x00FFFFFFu) | ((uint32_t)(hits * 255 / 16) << 24));
		}
}
#define COL_STAR UI_RGBA(0xFF, 0xD2, 0x48, 0xFF)
// owner: windows with slightly rounded corners and a soft shadow (layers growing outwards, lower right)
// drop shadow of a window (offset 4, 6, soft over 9 px), only outside it: straight edges as one-colour runs, the
// corners a pixel at a time. Filling whole shadow layers took tens of ms a redraw (owner: the UI felt slow)
enum { SH_OX = 4, SH_OY = 6, SH_B = 9, SH_A = 0x58 };
static uint32_t sh_col(int dx, int dy)
{
	const int d2 = dx * dx + dy * dy;
	return d2 >= SH_B * SH_B ? 0 : UI_RGBA(0x00, 0x00, 0x08, SH_A * (SH_B * SH_B - d2) / (SH_B * SH_B));
}
static void sh_block(UiCanvas *c, int x0, int y0, int x1, int y1, int sx0, int sy0, int sx1, int sy1)
{
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			const uint32_t col = sh_col(xx < sx0 ? sx0 - xx : (xx >= sx1 ? xx - sx1 + 1 : 0), yy < sy0 ? sy0 - yy : (yy >= sy1 ? yy - sy1 + 1 : 0));
			if (col)
				ui_rect(c, xx, yy, 1, 1, col);
		}
}
static void window_box(UiCanvas *c, int x, int y, int w, int h, uint32_t edge, uint32_t fill)
{
	const int sx0 = x + SH_OX, sy0 = y + SH_OY, sx1 = x + w + SH_OX, sy1 = y + h + SH_OY, B = SH_B;
	const int ex = x + w + 1, ey = y + h + 1; // first column / row past the window and its outline
	for (int xx = ex; xx < sx1 + B; xx++) { // right edge (rows beside the window)
		const uint32_t col = sh_col(xx >= sx1 ? xx - sx1 + 1 : 0, 0);
		if (col)
			ui_rect(c, xx, sy0, 1, ey - sy0, col);
	}
	for (int xx = sx0 - B; xx < x - 1; xx++) { // left edge
		const uint32_t col = sh_col(sx0 - xx, 0);
		if (col)
			ui_rect(c, xx, sy0, 1, ey - sy0, col);
	}
	for (int yy = ey; yy < sy1 + B; yy++) { // bottom edge
		const uint32_t col = sh_col(0, yy >= sy1 ? yy - sy1 + 1 : 0);
		if (col)
			ui_rect(c, sx0, yy, ex - sx0, 1, col);
	}
	sh_block(c, ex, sy0 - B, sx1 + B, sy0, sx0, sy0, sx1, sy1); // corners
	sh_block(c, ex, ey, sx1 + B, sy1 + B, sx0, sy0, sx1, sy1);
	sh_block(c, sx0 - B, ey, sx0, sy1 + B, sx0, sy0, sx1, sy1);
	rrect_outline(c, x - 1, y - 1, w + 2, h + 2, 7, 1, edge);
	rrect(c, x, y, w, h, 6, fill);
}
// the menu background: one colour per row (vertical gradient; the list also gets its header and footer bands). Filled
// row by row: copying a whole-screen image took 8 ms on the device (owner: the UI felt slow)
static uint32_t blend_px(uint32_t d, uint32_t c)
{
	const unsigned a = c >> 24;
	uint32_t o = 0xFF000000u;
	for (int k = 0; k < 24; k += 8)
		o |= ((((d >> k) & 0xFF) * (255 - a) + ((c >> k) & 0xFF) * a + 127) / 255) << k;
	return o;
}
#define LIST_FOOT_H 40 // the list's footer band (one line of button help)
static const uint32_t *theme_rows(int bands)
{
	static uint32_t rows[2][SCREEN_H];
	static int made;
	if (!made) {
		made = 1;
		for (int y = 0; y < SCREEN_H; y++) {
			const float t = (float)y / (SCREEN_H - 1);
			rows[0][y] = rows[1][y] = UI_RGBA((int)(6 + t * 14), (int)(22 + t * 64), (int)(70 + t * 110), 0xFF); // deep blue -> blue
			if (y < TITLE_H - 8)
				rows[1][y] = blend_px(rows[1][y], UI_RGBA(0x00, 0x08, 0x20, 0x50));
			else if (y == TITLE_H - 8 || y == SCREEN_H - LIST_FOOT_H)
				rows[1][y] = blend_px(rows[1][y], UI_RGBA(0x6C, 0xA8, 0xF0, 0x60));
			else if (y > SCREEN_H - LIST_FOOT_H)
				rows[1][y] = blend_px(rows[1][y], UI_RGBA(0x00, 0x08, 0x20, 0x60));
		}
	}
	return rows[bands ? 1 : 0];
}

// ================================ settings ================================
rstv_settings g_set;

static const char *const s_keymap_keys[RSTV_ACT_COUNT] = { "key_b1", "key_b2", "key_b3", "key_b4", "key_coin", "key_start" };

void rstv_settings_default_keys(void)
{
	g_set.keymap[RSTV_ACT_B1] = RSTV_KEY_CROSS;
	g_set.keymap[RSTV_ACT_B2] = RSTV_KEY_CIRCLE;
	g_set.keymap[RSTV_ACT_B3] = RSTV_KEY_SQUARE;
	g_set.keymap[RSTV_ACT_B4] = RSTV_KEY_TRIANGLE;
	g_set.keymap[RSTV_ACT_COIN] = RSTV_KEY_SELECT;
	g_set.keymap[RSTV_ACT_START] = RSTV_KEY_START;
	g_set.stick = RSTV_STICK_BOTH;
	g_set.turbo_key = RSTV_KEY_NONE;
	g_set.turbo_btn = 1;
	g_set.turbo_speed = RSTV_TURBO_FAST;
}

void rstv_settings_defaults(void)
{
	g_set.fps = 0; // (owner: off in the release default)
	g_set.aspect = RSTV_ASPECT_4_3;
	g_set.filter = RSTV_FILTER_SMOOTH;
	g_set.scan_type = RSTV_SCAN_OFF;
	g_set.scan_strength = RSTV_SCAN_MEDIUM;
	g_set.border = RSTV_BORDER_ARTWORK; // owner: artwork by default on a first install
	g_set.rotate = RSTV_ROT_AUTO;
	g_set.border_color = RSTV_BCOL_GRAY;
	g_set.confirm = RSTV_CONFIRM_CROSS;
	g_set.list_filter = RSTV_LIST_PLAYABLE;
	rstv_settings_default_keys();
}

uint32_t rstv_key_code(int key)
{
	static const uint32_t codes[RSTV_KEY_COUNT] = { SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE,
		SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_SELECT, SCE_CTRL_START };
	return key >= 0 && key < RSTV_KEY_COUNT ? codes[key] : 0;
}

const char *rstv_key_name(int key)
{
	static const char *const n[RSTV_KEY_COUNT] = { "Cross", "Circle", "Square", "Triangle", "L", "R", "Select", "Start" };
	return key >= 0 && key < RSTV_KEY_COUNT ? n[key] : "None";
}

// ADR-190: orientation of the sets whose monitor is turned (MAME -listxml: only shienryu, ROT270 among ST-V sets)
static int game_rotation(const char *set)
{
	return set && !strcmp(set, "shienryu") ? 270 : 0;
}
int rstv_input_rot;
static char s_game[32]; // current game set name (<set>_game.cfg)
void rstv_settings_apply_video(void)
{
	const int g = game_rotation(s_game);
	int r = 0;
	rstv_input_rot = 0;
	switch (g_set.rotate) {
	case RSTV_ROT_AUTO: r = g; break;
	case RSTV_ROT_VL: r = (g + 90) % 360; rstv_input_rot = 90; break;
	case RSTV_ROT_VR: r = (g + 270) % 360; rstv_input_rot = 270; break;
	default: break;
	}
	rstv_video_set_rotate(r);
	// rstv_video_set_mode: 0 = 4:3, 1 = integer, 2 = full / 0 = sharp, 1 = smooth
	const int scale = g_set.aspect == RSTV_ASPECT_FULL ? 2 : g_set.aspect == RSTV_ASPECT_INT ? 1 : 0;
	rstv_video_set_mode(scale, g_set.filter == RSTV_FILTER_SMOOTH);
	rstv_video_set_style(g_set.scan_type, g_set.scan_strength, g_set.border, g_set.border_color);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// items per-game settings can override (global values in s_global)
typedef struct {
	int fps, aspect, filter, scan_type, scan_strength, border, border_color, rotate;
	int keymap[RSTV_ACT_COUNT], stick, turbo_key, turbo_btn, turbo_speed;
} game_part;
static game_part s_global;
static int s_game_on;   // per-game settings are active for the current game
static void part_get(game_part *g)
{
	g->fps = g_set.fps; g->aspect = g_set.aspect; g->filter = g_set.filter;
	g->scan_type = g_set.scan_type; g->scan_strength = g_set.scan_strength;
	g->border = g_set.border; g->border_color = g_set.border_color; g->rotate = g_set.rotate;
	memcpy(g->keymap, g_set.keymap, sizeof(g->keymap));
	g->stick = g_set.stick; g->turbo_key = g_set.turbo_key; g->turbo_btn = g_set.turbo_btn; g->turbo_speed = g_set.turbo_speed;
}
static void part_set(const game_part *g)
{
	g_set.fps = g->fps; g_set.aspect = g->aspect; g_set.filter = g->filter;
	g_set.scan_type = g->scan_type; g_set.scan_strength = g->scan_strength;
	g_set.border = g->border; g_set.border_color = g->border_color; g_set.rotate = g->rotate;
	memcpy(g_set.keymap, g->keymap, sizeof(g->keymap));
	g_set.stick = g->stick; g_set.turbo_key = g->turbo_key; g_set.turbo_btn = g->turbo_btn; g_set.turbo_speed = g->turbo_speed;
}
// one overridable line (shared by global and game files). 1 if recognized
static int read_part(const char *key, int val)
{
	if (!strcmp(key, "show_fps")) g_set.fps = val != 0;
	else if (!strcmp(key, "aspect")) g_set.aspect = clampi(val, 0, RSTV_ASPECT_COUNT - 1);
	else if (!strcmp(key, "filter")) g_set.filter = clampi(val, 0, RSTV_FILTER_COUNT - 1);
	else if (!strcmp(key, "scan_type")) g_set.scan_type = clampi(val, 0, RSTV_SCAN_COUNT - 1);
	else if (!strcmp(key, "scan_strength")) g_set.scan_strength = clampi(val, 0, RSTV_SCAN_STRENGTH_COUNT - 1);
	else if (!strcmp(key, "border")) g_set.border = clampi(val, 0, RSTV_BORDER_COUNT - 1);
	else if (!strcmp(key, "border_color")) g_set.border_color = clampi(val, 0, RSTV_BCOL_COUNT - 1);
	else if (!strcmp(key, "rotate")) g_set.rotate = clampi(val, 0, RSTV_ROT_COUNT - 1);
	else if (!strcmp(key, "stick")) g_set.stick = clampi(val, 0, RSTV_STICK_COUNT - 1);
	else if (!strcmp(key, "turbo_key")) g_set.turbo_key = clampi(val, 0, RSTV_KEY_NONE);
	else if (!strcmp(key, "turbo_btn")) g_set.turbo_btn = clampi(val, 1, RSTV_GAME_BUTTONS);
	else if (!strcmp(key, "turbo_speed")) g_set.turbo_speed = clampi(val, 0, RSTV_TURBO_SPEED_COUNT - 1);
	else {
		for (int a = 0; a < RSTV_ACT_COUNT; a++)
			if (!strcmp(key, s_keymap_keys[a])) {
				g_set.keymap[a] = clampi(val, 0, (a == RSTV_ACT_B3 || a == RSTV_ACT_B4) ? RSTV_KEY_NONE : RSTV_KEY_COUNT - 1);
				return 1;
			}
		return 0;
	}
	return 1;
}
static void write_part(FILE *f, const game_part *g)
{
	fprintf(f, "show_fps=%d\naspect=%d\nfilter=%d\n", g->fps, g->aspect, g->filter);
	fprintf(f, "scan_type=%d\nscan_strength=%d\n", g->scan_type, g->scan_strength);
	fprintf(f, "border=%d\nborder_color=%d\nrotate=%d\n", g->border, g->border_color, g->rotate);
	for (int a = 0; a < RSTV_ACT_COUNT; a++)
		fprintf(f, "%s=%d\n", s_keymap_keys[a], g->keymap[a]);
	fprintf(f, "stick=%d\nturbo_key=%d\nturbo_btn=%d\nturbo_speed=%d\n", g->stick, g->turbo_key, g->turbo_btn, g->turbo_speed);
}
// one "key=value" line. 0 at end
static int read_kv(FILE *f, char *key, int klen, char *val, int vlen)
{
	char line[128];
	if (!fgets(line, sizeof(line), f))
		return 0;
	char *eq = strchr(line, '=');
	if (!eq) {
		key[0] = val[0] = 0;
		return 1;
	}
	*eq = 0;
	snprintf(key, klen, "%s", line);
	char *v = eq + 1;
	v[strcspn(v, "\r\n")] = 0;
	snprintf(val, vlen, "%s", v);
	return 1;
}

// pre-ADR-126 settings.txt ("key value"): screen 0 4:3 / 1 integer / 2 full, filter 0 sharp / 1 smooth, buttons b1-b4,
// turbo turbo_btn 0 = none
static int load_old_settings(void)
{
	FILE *f = fopen(OLD_SETTINGS_PATH, "r");
	if (!f)
		return 0;
	char k[32];
	int v, tbtn = -1, tkey = -1;
	while (fscanf(f, "%31s %d", k, &v) == 2) {
		if (!strcmp(k, "scale")) g_set.aspect = v == 1 ? RSTV_ASPECT_INT : v == 2 ? RSTV_ASPECT_FULL : RSTV_ASPECT_4_3;
		else if (!strcmp(k, "filter")) g_set.filter = v ? RSTV_FILTER_SMOOTH : RSTV_FILTER_SHARP;
		else if (!strcmp(k, "fps")) g_set.fps = v != 0;
		else if (!strcmp(k, "stick2")) g_set.stick = clampi(v, 0, RSTV_STICK_COUNT - 1);
		else if (!strcmp(k, "confirm")) g_set.confirm = v != 0;
		else if (!strcmp(k, "list_all")) g_set.list_filter = v ? RSTV_LIST_ALL : RSTV_LIST_PLAYABLE;
		else if (!strcmp(k, "turbo_btn")) tbtn = v;
		else if (!strcmp(k, "turbo_key")) tkey = v;
		else if (!strcmp(k, "turbo_speed")) g_set.turbo_speed = clampi(v, 0, 2);
		else if (k[0] == 'b' && k[1] >= '1' && k[1] <= '4' && !k[2] && v >= 0 && v < 6) g_set.keymap[k[1] - '1'] = v;
	}
	fclose(f);
	if (tbtn > 0 && tbtn <= RSTV_GAME_BUTTONS && tkey >= 0 && tkey < 6) {
		g_set.turbo_key = tkey;
		g_set.turbo_btn = tbtn;
	}
	RSTV_LOG("settings: migrated %s", OLD_SETTINGS_PATH);
	return 1;
}

// ---- favorites (favorites.cfg, one set name per line) ----
#define FAV_MAX 256
static char s_fav[FAV_MAX][16];
static int s_fav_n;

int rstv_fav_is(const char *set)
{
	for (int i = 0; i < s_fav_n; i++)
		if (!strcmp(s_fav[i], set))
			return 1;
	return 0;
}

static void fav_load(void)
{
	FILE *f = fopen(FAV_PATH, "r");
	if (!f)
		return;
	char line[64];
	s_fav_n = 0;
	while (s_fav_n < FAV_MAX && fgets(line, sizeof(line), f)) {
		const size_t len = strcspn(line, "\r\n");
		line[len] = 0;
		if (len && len < sizeof(s_fav[0]) && !rstv_fav_is(line))
			memcpy(s_fav[s_fav_n++], line, len + 1);
	}
	fclose(f);
}

void rstv_fav_toggle(const char *set)
{
	int i = 0;
	while (i < s_fav_n && strcmp(s_fav[i], set) != 0)
		i++;
	if (i < s_fav_n) {
		memmove(s_fav[i], s_fav[i + 1], (size_t)(s_fav_n - i - 1) * sizeof(s_fav[0]));
		s_fav_n--;
	} else if (s_fav_n < FAV_MAX && strlen(set) < sizeof(s_fav[0]))
		strcpy(s_fav[s_fav_n++], set);
	FILE *f = fopen(FAV_PATH, "w");
	if (!f) {
		RSTV_LOG("settings: save failed %s", FAV_PATH);
		return;
	}
	for (i = 0; i < s_fav_n; i++)
		fprintf(f, "%s\n", s_fav[i]);
	fclose(f);
}

void rstv_settings_load(void)
{
	rstv_settings_defaults();
	fav_load();
	FILE *f = fopen(SETTINGS_PATH, "r");
	if (f) {
		char key[32], sval[32];
		while (read_kv(f, key, sizeof(key), sval, sizeof(sval))) {
			const int val = atoi(sval);
			if (read_part(key, val))
				continue;
			else if (!strcmp(key, "menu_confirm")) g_set.confirm = clampi(val, 0, RSTV_CONFIRM_COUNT - 1);
			else if (!strcmp(key, "list_filter")) g_set.list_filter = clampi(val, 0, RSTV_LIST_FILTER_COUNT - 1);
		}
		fclose(f);
	} else
		load_old_settings();
	part_get(&s_global);
	RSTV_LOG("settings: fps %d aspect %d filter %d scan %d/%d border %d/%d keys %d %d %d %d coin %d start %d turbo %d/%d",
		g_set.fps, g_set.aspect, g_set.filter, g_set.scan_type, g_set.scan_strength, g_set.border, g_set.border_color,
		g_set.keymap[0], g_set.keymap[1], g_set.keymap[2], g_set.keymap[3], g_set.keymap[4], g_set.keymap[5], g_set.turbo_key,
		g_set.turbo_btn);
}

void rstv_settings_load_game(const char *set)
{
	snprintf(s_game, sizeof(s_game), "%s", set);
	if (s_game_on) { // drop the previous game's per-game settings and return to global values
		part_set(&s_global);
		s_game_on = 0;
	} else
		part_get(&s_global);
	char p[96];
	snprintf(p, sizeof(p), DATA_DIR "/%s_game.cfg", s_game);
	FILE *f = fopen(p, "r");
	if (!f)
		return;
	char key[32], sval[32];
	while (read_kv(f, key, sizeof(key), sval, sizeof(sval)))
		read_part(key, atoi(sval));
	fclose(f);
	s_game_on = 1;
	RSTV_LOG("settings: game settings %s", p);
}

void rstv_settings_save(void)
{
	FILE *f = fopen(SETTINGS_PATH, "w");
	if (!f) {
		RSTV_LOG("settings: save failed %s", SETTINGS_PATH);
		return;
	}
	// with per-game settings on, those items go to the game file (below); the global file keeps the global values
	if (!s_game_on)
		part_get(&s_global);
	write_part(f, &s_global);
	fprintf(f, "menu_confirm=%d\nlist_filter=%d\n", g_set.confirm, g_set.list_filter);
	fclose(f);
	if (s_game[0] && s_game_on)
		rstv_settings_save_game();
}

int rstv_settings_has_game(void) { return s_game_on; }

void rstv_settings_use_global(void)
{
	if (s_game_on) {
		part_set(&s_global);
		s_game_on = 0;
	}
	s_game[0] = 0; // (game file is not written until the next rstv_settings_load_game)
}

int rstv_settings_save_game(void)
{
	if (!s_game[0])
		return 0;
	char p[96];
	snprintf(p, sizeof(p), DATA_DIR "/%s_game.cfg", s_game);
	FILE *f = fopen(p, "w");
	if (!f) {
		RSTV_LOG("settings: game save failed %s", p);
		return 0;
	}
	game_part g;
	part_get(&g);
	write_part(f, &g);
	fclose(f);
	s_game_on = 1;
	return 1;
}

int rstv_settings_delete_game(void)
{
	if (!s_game[0] || !s_game_on)
		return 0;
	char p[96];
	snprintf(p, sizeof(p), DATA_DIR "/%s_game.cfg", s_game);
	if (sceIoRemove(p) < 0) {
		RSTV_LOG("settings: game delete failed %s", p);
		return 0;
	}
	s_game_on = 0;
	part_set(&s_global); // (global values right away)
	rstv_settings_apply_video();
	return 1;
}

// ================================ menu state ================================
static char s_set[32], s_title[96];
static UiCanvas *s_cv, *s_osd_cv, *s_fps_cv;
enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_ABOUT, PAGE_CHEATS, PAGE_TURBO };
static int s_page; // (defined with the other menu state below)
#define CTRL_PAGE (s_page == PAGE_CONTROLS || s_page == PAGE_TURBO) // (turbo rows are a sub page of Controls)
// rows in groups (a gap before M_STATES, M_DISPLAY and M_ABOUT): game, states / cheats, settings, about / leave
// M_REFRESH (read the ROM folder again) only in the game list options (owner: not in the in-game menu)
enum { M_RESUME, M_RESET, M_TEST, M_STATES, M_CHEATS, M_DISPLAY, M_CONTROLS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME, M_EXIT, M_BACK,
	M_REFRESH, M_COUNT };
#define GROUP_GAP 10
// game list options (START in the list): no game. Only Display / Controls / Back, global settings only
static int s_list_mode;
// main menu rows -> items (Delete only if per-game settings exist, else Save only. Cheats only for games that have some.
// Back only in list options)
static int main_list(int *k)
{
	int n = 0;
	if (s_list_mode) {
		k[n++] = M_DISPLAY;
		k[n++] = M_CONTROLS;
		k[n++] = M_REFRESH;
		k[n++] = M_BACK;
		return n;
	}
	for (int m = 0; m < M_BACK; m++) {
		if (m == (rstv_settings_has_game() ? M_GAME_SAVE : M_GAME_DEL))
			continue;
		if (m == M_CHEATS && !rstv_cheats_count())
			continue;
		k[n++] = m;
	}
	return n;
}
static int main_rows(void) { int k[M_COUNT]; return main_list(k); }
static int main_kind(int row)
{
	int k[M_COUNT];
	const int n = main_list(k);
	return row >= 0 && row < n ? k[row] : M_RESUME;
}
enum { S_SLOT, S_SAVE, S_LOAD, S_BACK, S_COUNT };
enum { D_FPS, D_ASPECT, D_ROTATE, D_FILTER, D_SCAN, D_SCAN_STR, D_BORDER, D_BORDER_COL, D_BACK, D_COUNT };
// Controls rows: game buttons (as many as the game uses, 4 in list options), coin, start, Turbo (sub page, owner), analog stick,
// confirm button, defaults, back. Turbo page: turbo key, turbo button, turbo speed, back
enum { CK_ACT, CK_TURBO_KEY, CK_TURBO_BTN, CK_TURBO_SPEED, CK_TURBO, CK_STICK, CK_CONFIRM, CK_DEFAULTS, CK_BACK };
static int s_nbuttons = 4;
static int ctrl_buttons(void) { return s_list_mode ? RSTV_GAME_BUTTONS : s_nbuttons; }
static int ctrl_rows(void) { return s_page == PAGE_TURBO ? 4 : ctrl_buttons() + 2 + 5; }
static int ctrl_kind(int i, int *act)
{
	const int b = ctrl_buttons();
	*act = -1;
	if (s_page == PAGE_TURBO)
		return i < 3 ? CK_TURBO_KEY + i : CK_BACK;
	if (i < b) {
		*act = RSTV_ACT_B1 + i;
		return CK_ACT;
	}
	if (i < b + 2) {
		*act = i == b ? RSTV_ACT_COIN : RSTV_ACT_START;
		return CK_ACT;
	}
	return CK_TURBO + (i - b - 2);
}
static const char *act_name(int act)
{
	static const char *const n[RSTV_ACT_COUNT] = { "Button 1", "Button 2", "Button 3", "Button 4", "Coin", "Start" };
	return n[act];
}
int rstv_menu_buttons(void) { return s_nbuttons; }
static int s_page, s_sel, s_main_sel, s_reset_armed, s_state_slot, s_about_scroll, s_dirty = 1;
static int s_drawn_h; // panel height currently drawn on the menu canvas (0 = other content)
static uint32_t s_prev_buttons;

// About: vertically scrolling text (empty string = blank line, starting with '#' = heading line)
static const char *const s_about[] = {
	"#ReST-V for Vita",
	"Version " RSTV_VERSION " (" RSTV_VERSION_DATE ")",
	"Developed by Reyanon",
	"  https://github.com/Reyanon",
	"",
	"Sega ST-V / System C / C2 arcade board emulator",
	"Games (c) their owners. ROM images are not included.",
	"Not affiliated with or endorsed by Sega.",
	"Free software under the GNU GPL version 3.",
	"",
	"#Open source used",
	"Yabause / YabaSanshiro - SH-2, SCU, VDP1/2, SCSP, SMPC",
	"  https://github.com/libretro/yabause",
	"Kronos (Yabause fork) - ST-V I/O, EEPROM, cartridge",
	"  https://github.com/FCare/Kronos",
	"devmiyax SH-2 dynamic recompiler (via YabaSanshiro)",
	"Musashi - 68000 core (via Yabause)",
	"  https://github.com/kstenerud/Musashi",
	"ymfm - YM3438 sound (System C2)",
	"  https://github.com/aaronsgiles/ymfm",
	"MAME - ST-V ROM definitions, 315-5881 decryption",
	"  https://github.com/mamedev/mame",
	"VitaSDK - toolchain, newlib, system stubs",
	"  https://github.com/vitasdk",
	"vitashark - runtime shader compiler",
	"  https://github.com/Rinnegatamante/vitaShaRK",
	"junzip - ROM zip reading",
	"  https://github.com/jokkebk/JUnzip",
	"zlib - inflate",
	"  https://github.com/madler/zlib",
	"libpng / libjpeg-turbo - border artwork",
	"  https://github.com/pnggroup/libpng",
	"  https://github.com/libjpeg-turbo/libjpeg-turbo",
	"",
	"License texts: LICENSE in the source repository.",
};
#define ABOUT_LINES ((int)(sizeof(s_about) / sizeof(s_about[0])))
#define ABOUT_VISIBLE 12

void rstv_state_slot_path(char *buf, int len, int slot)
{
	if (slot < 0)
		snprintf(buf, len, DATA_DIR "/state/%s.yss", s_set);
	else
		snprintf(buf, len, DATA_DIR "/state/%s_%d.yss", s_set, slot);
}

static int slot_used(int slot)
{
	char p[160];
	rstv_state_slot_path(p, sizeof(p), slot);
	SceIoStat st;
	return sceIoGetstat(p, &st) >= 0;
}

static void ensure_canvases(void)
{
	ui_init();
	if (!s_cv)
		s_cv = ui_canvas(0, 0, 0, SCREEN_W, SCREEN_H);
	if (!s_osd_cv)
		s_osd_cv = ui_canvas(2, 0, 0, SCREEN_W, 36);
	if (!s_fps_cv)
		s_fps_cv = ui_canvas(3, 0, 0, 240, 30);
}

void rstv_menu_init(const char *set, const char *title, int buttons)
{
	s_nbuttons = buttons < 2 ? 2 : buttons > RSTV_GAME_BUTTONS ? RSTV_GAME_BUTTONS : buttons;
	if (g_set.turbo_btn > s_nbuttons)
		g_set.turbo_btn = 1;
	snprintf(s_set, sizeof(s_set), "%s", set);
	snprintf(s_title, sizeof(s_title), "%s", title ? title : set);
	rstv_cheats_init(set);
	ensure_canvases();
}

static int s_keep_place, s_pop, s_pop_sel; // menu closed with SELECT+START (keep its page); choice window open, its row
// owner: the PS button belongs to the app everywhere (locked for the system from the start, ADR-195). One press opens
// the menu (in a game) or the options (in the game list) and closes them again like SELECT+START; nothing while
// loading. Only a double press goes to the home screen: the app lets go of the lock and the kernel helper presses PS
// for the system
#define RSTV_KCTRL_LOADED 0x8002D013u // taiLoadStartKernelModule: already loaded (device)
static int s_kctrl = -1;    // kernel helper usable (module/restv_kctrl.skprx): 1 yes, -1 no
static int s_ps_locked;     // the system's PS button handling locked by the app
static unsigned s_ps_one, s_ps_two; // helper counts already acted on (single, double presses)
static uint64_t s_ps_home_t; // when a double press sent the app home (lock taken again a while later)
static void ps_lock(int on)
{
	if (s_kctrl < 0 || on == s_ps_locked)
		return;
	const int r = on ? sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2) : sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
	RSTV_LOG("menu: PS button %s (%08x)", on ? "locked" : "unlocked", (unsigned)r);
	s_ps_locked = on;
	if (on) { // (presses made while unlocked are not acted on)
		const unsigned st = rstvKCtrlPsState();
		s_ps_one = st & 0x7FFF;
		s_ps_two = (st >> 15) & 0x7FFF;
	}
}
// ADR-195: load the kernel helper at start. Its syscall is linked into this app only if the module was loaded before
// the app started (a call right after a first load crashed on the device), so a first load since boot restarts the
// app once (argument "kctrl"); the module then stays loaded until the Vita reboots
void rstv_kctrl_init(int restarted)
{
	const SceUID m = taiLoadStartKernelModule("ux0:app/RSTV00001/module/restv_kctrl.skprx", 0, NULL, 0); // (the kernel cannot open app0:)
	// (the module name has a version, restv_kctrl_v4: a module of an older build may still be loaded until reboot)
	if (m >= 0 && !restarted) {
		static char *const av[] = { "kctrl", NULL };
		sceAppMgrLoadExec("app0:eboot.bin", av, NULL);
	}
	s_kctrl = (unsigned)m == RSTV_KCTRL_LOADED ? 1 : -1;
	if (s_kctrl > 0) {
		sceShellUtilInitEvents(0);
		ps_lock(1);
	}
}
// call every frame anywhere: 1 once per single PS press; a double press goes home
static int ps_poll(void)
{
	if (s_kctrl < 0)
		return 0;
	if (!s_ps_locked) {
		if (rstv_time_us() - s_ps_home_t > 1500000)
			ps_lock(1); // (back from the home screen)
		return 0;
	}
	const unsigned st = rstvKCtrlPsState(), one = st & 0x7FFF, two = (st >> 15) & 0x7FFF;
	if (two != s_ps_two) {
		s_ps_two = two;
		s_ps_one = one;
		ps_lock(0);
		rstvKCtrlGoHome();
		s_ps_home_t = rstv_time_us();
		return 0;
	}
	if (one == s_ps_one)
		return 0;
	s_ps_one = one;
	return 1;
}

int rstv_menu_should_open(uint32_t buttons)
{
	const int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int ps = ps_poll();
	const int open = (combo && !prev_combo) || ps;
	s_prev_buttons = buttons; // so the same press does not act right away inside the menu
	if (open) {
		if (!s_keep_place) { // (closed with SELECT+START: opens where it was, sub pages too, owner)
			s_page = PAGE_MAIN;
			s_sel = 0;
		}
		s_keep_place = 0;
		s_pop = 0;
		s_reset_armed = 0;
		s_dirty = 1;
	}
	return open;
}

int rstv_menu_slot(void)
{
	return s_state_slot;
}

static int page_items(void)
{
	switch (s_page) {
	case PAGE_DISPLAY: return D_COUNT;
	case PAGE_CONTROLS:
	case PAGE_TURBO: return ctrl_rows();
	case PAGE_STATES: return S_COUNT;
	case PAGE_ABOUT: return ABOUT_VISIBLE;
	case PAGE_CHEATS: return rstv_cheats_count() + 1;
	default: return main_rows();
	}
}

static void go_page(int page)
{
	if (s_page == PAGE_MAIN)
		s_main_sel = s_sel;
	s_page = page;
	s_sel = (page == PAGE_MAIN) ? s_main_sel : 0;
	if (page == PAGE_MAIN && s_sel >= main_rows())
		s_sel = 0;
	s_reset_armed = 0;
}
// back one level: the Turbo page returns to its row in Controls, other pages to the main page
static void page_back(void)
{
	if (s_page == PAGE_TURBO) {
		s_page = PAGE_CONTROLS;
		s_sel = ctrl_buttons() + 2; // the Turbo row
		return;
	}
	go_page(PAGE_MAIN);
}

// repeat while a direction is held (after about 0.4 s, every 0.07 s)
#define REPEAT_KEYS (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)
// held directions repeat after 300 ms, then every 60 ms. Timed by the clock, not by loop passes: a slow redraw made a
// pass-counted repeat late and slow (owner: the UI felt sluggish)
static uint32_t key_repeat(uint32_t held, uint32_t pressed, uint64_t *next)
{
	const uint32_t dir = held & REPEAT_KEYS;
	const uint64_t now = rstv_time_us();
	if (!dir || (pressed & REPEAT_KEYS)) {
		*next = now + 300000;
		return pressed;
	}
	if (now >= *next) {
		*next = now + 60000;
		pressed |= dir;
	}
	return pressed;
}

static int wrap(int v, int n)
{
	return (v % n + n) % n;
}

// value names (items and the choice window)
static const char *const aspect[] = { "4:3", "Full screen", "Integer" };
static const char *const filter[] = { "Smooth", "Sharp" };
static const char *const scan[] = { "Off", "CRT soft", "CRT sharp", "Every 2 px", "Every 3 px", "Every 4 px" };
static const char *const scan_str[] = { "Light", "Medium", "Heavy" };
static const char *const border[] = { "Black", "Artwork", "Color" };
static const char *const rotate[] = { "Auto", "Off", "Vertical L", "Vertical R" };
static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
static const char *const stick[] = { "Off", "Left", "Right", "Both" };
static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };

static void change(int dir);
// owner: an item with several values opens a window listing them all on the confirm button. The values of the selected
// item: names, count (0 = not such an item), current index
#define MAX_CHOICES 16
static int s_choice_keys; // the last item_choices() listed buttons (drawn as button icons)
static int item_choices(const char **out, int *cur, const char **title)
{
	int n = 0;
	*cur = 0;
	*title = "";
	s_choice_keys = 0;
	if (s_page == PAGE_DISPLAY) {
		static const char *const t[] = { "", "Screen", "Rotate", "Filter", "Scanlines", "Scanline strength", "Border", "Border color" };
		const char *const *v = NULL;
		switch (s_sel) {
		case D_ASPECT: v = aspect; n = RSTV_ASPECT_COUNT; *cur = g_set.aspect; break;
		case D_ROTATE: v = rotate; n = RSTV_ROT_COUNT; *cur = g_set.rotate; break;
		case D_FILTER: v = filter; n = RSTV_FILTER_COUNT; *cur = g_set.filter; break;
		case D_SCAN: v = scan; n = RSTV_SCAN_COUNT; *cur = g_set.scan_type; break;
		case D_SCAN_STR: v = scan_str; n = RSTV_SCAN_STRENGTH_COUNT; *cur = g_set.scan_strength; break;
		case D_BORDER: v = border; n = RSTV_BORDER_COUNT; *cur = g_set.border; break;
		case D_BORDER_COL: v = border_col; n = RSTV_BCOL_COUNT; *cur = g_set.border_color; break;
		default: return 0;
		}
		*title = t[s_sel < 8 ? s_sel : 0];
		for (int i = 0; i < n; i++)
			out[i] = v[i];
	} else if (CTRL_PAGE) {
		int act;
		const int k = ctrl_kind(s_sel, &act);
		static char btn[8][12];
		if (k == CK_ACT || k == CK_TURBO_KEY) {
			const int with_none = k == CK_TURBO_KEY || act == RSTV_ACT_B3 || act == RSTV_ACT_B4;
			n = RSTV_KEY_COUNT + with_none;
			for (int i = 0; i < n; i++)
				out[i] = rstv_key_name(i);
			*cur = k == CK_ACT ? g_set.keymap[act] : g_set.turbo_key;
			s_choice_keys = 1;
			*title = k == CK_ACT ? act_name(act) : "Turbo key";
		} else if (k == CK_TURBO_BTN) {
			n = ctrl_buttons();
			for (int i = 0; i < n && i < 8; i++) {
				snprintf(btn[i], sizeof(btn[i]), "Button %d", i + 1);
				out[i] = btn[i];
			}
			*cur = g_set.turbo_btn - 1;
			*title = "Turbo button";
		} else if (k == CK_TURBO_SPEED) {
			n = RSTV_TURBO_SPEED_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = turbo_speed[i];
			*cur = g_set.turbo_speed;
			*title = "Turbo speed";
		} else if (k == CK_STICK) {
			n = RSTV_STICK_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = stick[i];
			*cur = g_set.stick;
			*title = "Analog stick";
		}
	}
	return n >= 3 ? n : 0; // two values: the button just flips them
}
static void set_choice(int idx)
{
	const char *names[MAX_CHOICES], *title;
	int cur, n = item_choices(names, &cur, &title);
	for (int guard = 0; n && cur != idx && guard < MAX_CHOICES + 2; guard++) {
		change(+1); // the same steps as the right button (wraps, applies)
		n = item_choices(names, &cur, &title);
	}
}

static void change(int dir)
{
	switch (s_page) {
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: g_set.fps = !g_set.fps; break;
		case D_ASPECT: g_set.aspect = wrap(g_set.aspect + dir, RSTV_ASPECT_COUNT); break;
		case D_ROTATE: g_set.rotate = wrap(g_set.rotate + dir, RSTV_ROT_COUNT); break;
		case D_FILTER: g_set.filter = wrap(g_set.filter + dir, RSTV_FILTER_COUNT); break;
		case D_SCAN: g_set.scan_type = wrap(g_set.scan_type + dir, RSTV_SCAN_COUNT); break;
		case D_SCAN_STR: g_set.scan_strength = wrap(g_set.scan_strength + dir, RSTV_SCAN_STRENGTH_COUNT); break;
		case D_BORDER: g_set.border = wrap(g_set.border + dir, RSTV_BORDER_COUNT); break;
		case D_BORDER_COL: g_set.border_color = wrap(g_set.border_color + dir, RSTV_BCOL_COUNT); break;
		}
		rstv_settings_apply_video();
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO: {
		int act;
		const int k = ctrl_kind(s_sel, &act);
		if (k == CK_ACT) // buttons 3 and 4 can also be None
			g_set.keymap[act] = wrap(g_set.keymap[act] + dir,
				(act == RSTV_ACT_B3 || act == RSTV_ACT_B4) ? RSTV_KEY_COUNT + 1 : RSTV_KEY_COUNT);
		else if (k == CK_TURBO_KEY)
			g_set.turbo_key = wrap(g_set.turbo_key + dir, RSTV_KEY_COUNT + 1); // + None (off)
		else if (k == CK_TURBO_BTN) // 1..number of game buttons
			g_set.turbo_btn = wrap(g_set.turbo_btn - 1 + dir, ctrl_buttons()) + 1;
		else if (k == CK_TURBO_SPEED)
			g_set.turbo_speed = wrap(g_set.turbo_speed + dir, RSTV_TURBO_SPEED_COUNT);
		else if (k == CK_STICK)
			g_set.stick = wrap(g_set.stick + dir, RSTV_STICK_COUNT);
		else if (k == CK_CONFIRM)
			g_set.confirm = wrap(g_set.confirm + dir, RSTV_CONFIRM_COUNT);
		break;
	}
	case PAGE_STATES:
		if (s_sel == S_SLOT)
			s_state_slot = wrap(s_state_slot + dir, STATE_SLOTS);
		break;
	case PAGE_CHEATS:
		rstv_cheats_change(s_sel, dir);
		break;
	}
}

static rstv_menu_result confirm(void)
{
	switch (s_page) {
	case PAGE_MAIN:
		switch (main_kind(s_sel)) {
		case M_RESUME: return RSTV_MENU_CLOSE;
		case M_RESET:
			if (!s_reset_armed) {
				s_reset_armed = 1;
				return RSTV_MENU_STAY;
			}
			return RSTV_MENU_RESET;
		case M_TEST: return RSTV_MENU_TEST;
		case M_STATES: go_page(PAGE_STATES); break;
		case M_DISPLAY: go_page(PAGE_DISPLAY); break;
		case M_CONTROLS: go_page(PAGE_CONTROLS); break;
		case M_CHEATS: go_page(PAGE_CHEATS); break;
		case M_GAME_SAVE: // this game only (the global settings file is not written)
			if (rstv_settings_save_game())
				rstv_osd_show("Saved settings for %s", s_set);
			else
				rstv_osd_show("Could not save game settings");
			break;
		case M_GAME_DEL:
			if (rstv_settings_delete_game())
				rstv_osd_show("Deleted settings for %s", s_set);
			else
				rstv_osd_show("Could not delete game settings");
			break;
		case M_ABOUT:
			go_page(PAGE_ABOUT);
			s_about_scroll = 0;
			break;
		case M_SELECT_GAME: return RSTV_MENU_SELECT_GAME;
		case M_REFRESH: return RSTV_MENU_REFRESH;
		case M_EXIT: return RSTV_MENU_EXIT;
		case M_BACK: return RSTV_MENU_CLOSE;
		}
		break;
	case PAGE_DISPLAY:
		if (s_sel == D_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO: {
		int act;
		const int k = ctrl_kind(s_sel, &act);
		if (k == CK_BACK) page_back();
		else if (k == CK_TURBO) go_page(PAGE_TURBO);
		else if (k == CK_DEFAULTS) rstv_settings_default_keys();
		else change(+1);
		break;
	}
	case PAGE_ABOUT:
		go_page(PAGE_MAIN);
		break;
	case PAGE_CHEATS:
		if (s_sel == rstv_cheats_count()) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_STATES:
		if (s_sel == S_SAVE) return RSTV_MENU_SAVE;
		if (s_sel == S_LOAD) {
			if (slot_used(s_state_slot))
				return RSTV_MENU_LOAD;
			rstv_osd_show("Slot %d is empty", s_state_slot);
		}
		if (s_sel == S_BACK) go_page(PAGE_MAIN);
		if (s_sel == S_SLOT) change(+1);
		break;
	}
	return RSTV_MENU_STAY;
}

rstv_menu_result rstv_menu_update(uint32_t buttons)
{
	const int ps_closed = ps_poll();
	if (ps_closed)
		s_pop = 0;
	static uint64_t rep_count;
	uint32_t pressed = key_repeat(buttons, buttons & ~s_prev_buttons, &rep_count);
	s_prev_buttons = buttons;
	if (pressed)
		s_dirty = 1;
	rstv_menu_result r = RSTV_MENU_STAY;
	const int n = page_items();
	{
		const int o_ok = g_set.confirm == RSTV_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		const char *names[MAX_CHOICES], *title;
		int cur;
		const int nc = item_choices(names, &cur, &title);
		if (s_pop) { // choice window: up/down, confirm sets, back closes
			if (!nc)
				s_pop = 0;
			else if (pressed & SCE_CTRL_UP)
				s_pop_sel = wrap(s_pop_sel - 1, nc);
			else if (pressed & SCE_CTRL_DOWN)
				s_pop_sel = wrap(s_pop_sel + 1, nc);
			else if (pressed & ok) {
				set_choice(s_pop_sel);
				s_pop = 0;
				rstv_settings_save();
			} else if (pressed & (back | SCE_CTRL_START))
				s_pop = 0;
			if (!s_pop)
				s_drawn_h = 0; // the whole panel again (the window covered part of it)
			return RSTV_MENU_STAY;
		}
		if ((pressed & ok) && nc) {
			s_pop = 1;
			s_pop_sel = cur;
			s_drawn_h = 0;
			return RSTV_MENU_STAY;
		}
	}
	if (s_page == PAGE_ABOUT) { // up/down scroll
		const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
		if ((pressed & SCE_CTRL_UP) && s_about_scroll > 0)
			s_about_scroll--;
		if ((pressed & SCE_CTRL_DOWN) && s_about_scroll < max)
			s_about_scroll++;
		pressed &= ~REPEAT_KEYS;
	}
	if (pressed & SCE_CTRL_UP) {
		s_sel = wrap(s_sel - 1, n);
		s_reset_armed = 0;
	}
	if (pressed & SCE_CTRL_DOWN) {
		s_sel = wrap(s_sel + 1, n);
		s_reset_armed = 0;
	}
	if (pressed & SCE_CTRL_LEFT)
		change(-1);
	if (pressed & SCE_CTRL_RIGHT)
		change(+1);
	// confirm/back follow the setting (×/○). If the setting changes during this input, use the setting at press time
	const int o_ok = g_set.confirm == RSTV_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	if (pressed & ok)
		r = confirm();
	else if (pressed & back) {
		if (s_page == PAGE_MAIN)
			r = RSTV_MENU_CLOSE;
		else
			page_back();
	}
	if (pressed & SCE_CTRL_START) {
		r = RSTV_MENU_CLOSE;
		s_keep_place = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO && !s_list_mode;
	}
	if (ps_closed) { // owner: PS closes the menu as SELECT+START does (opens again where it was)
		r = RSTV_MENU_CLOSE;
		s_keep_place = !s_list_mode;
	}
	if (r != RSTV_MENU_STAY)
		rstv_settings_save();
	return r;
}

// ================================ drawing ================================
#define VAL_ON "on"   // item value: draw a toggle switch (two-state items only)
#define VAL_OFF "off"
static void item(UiCanvas *c, int px, int ty, int i, int sel, const char *name, const char *value)
{
	const uint32_t col = (i == sel) ? COL_SEL : COL_TEXT;
	if (i == sel) { // ADR-194: soft bar with a shadow and a blinking outline instead of a cursor mark
		const int by = ty - 7 - (LINE_H - 6) / 2; // bar centred on the text
		rrect(c, px + 12, by + 3, PANEL_W - 40, LINE_H - 6, 6, UI_RGBA(0x00, 0x00, 0x10, 0x30));
		rrect(c, px + 12, by, PANEL_W - 40, LINE_H - 6, 6, COL_HILITE);
		if (s_pop) // the choice window is over it
			rrect_outline(c, px + 12, by, PANEL_W - 40, LINE_H - 6, 6, 2, COL_FOCUS_STILL);
		else
			focus_at(c, px + 12, by, PANEL_W - 40, LINE_H - 6);
	}
	ui_text(c, px + 40, ty, i == sel ? COL_TEXT : UI_RGBA(0xD6, 0xE2, 0xFA, 0xFF), 1.0f, name);
	if (value && value[0] == 1) {
		// two-state item as a toggle switch: a pill with the knob on the right (on, accent colour) or left (off, grey)
		const int on = value[2] == 'n';
		const float r = 10.0f, x0 = (float)(px + VALUE_X) + r, x1 = x0 + 26.0f, cy = (float)ty - 7.0f;
		const uint32_t track = on ? UI_RGBA(0x38, 0xA8, 0xFF, 0xFF) : UI_RGBA(0x55, 0x5E, 0x78, 0xFF);
		ui_circle(c, x0, cy, r, track);
		ui_circle(c, x1, cy, r, track);
		ui_rect(c, (int)x0, (int)(cy - r), (int)(x1 - x0), (int)(2 * r), track);
		ui_circle(c, on ? x1 : x0, cy, r - 3.0f, i == sel ? COL_SEL : UI_RGBA(0xF0, 0xF0, 0xF0, 0xFF));
	} else if (value) {
		char buf[96];
		snprintf(buf, sizeof(buf), "< %s >", value);
		ui_text(c, px + VALUE_X, ty, col, 1.0f, buf);
	}
}

static void thick_line(UiCanvas *c, float x0, float y0, float x1, float y1, uint32_t col)
{
	ui_line(c, x0, y0, x1, y1, 2.4f, col);
}

// Draw a button symbol starting at (x, cy), vertically centered on cy, and return the width used. L, R, Select, Start, None as text
static int draw_key(UiCanvas *c, int x, int cy, int ty, int key, uint32_t text_col)
{
	const float r = 9.0f;
	const float cx = x + r + 1;
	switch (key) {
	case RSTV_KEY_CROSS: {
		const uint32_t col = UI_RGBA(0x7C, 0xB4, 0xF0, 0xFF);
		thick_line(c, cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f, col);
		thick_line(c, cx - r * 0.8f, cy + r * 0.8f, cx + r * 0.8f, cy - r * 0.8f, col);
		return (int)(r * 2 + 2);
	}
	case RSTV_KEY_CIRCLE:
		ui_circle(c, cx, cy, r, UI_RGBA(0xFF, 0x6E, 0x6E, 0xFF));
		ui_circle(c, cx, cy, r - 2.5f, COL_PANEL);
		return (int)(r * 2 + 2);
	case RSTV_KEY_SQUARE: {
		const uint32_t col = UI_RGBA(0xF0, 0xA0, 0xD8, 0xFF);
		const float h = r * 0.85f;
		ui_rect(c, (int)(cx - h), (int)(cy - h), (int)(h * 2), 2, col);
		ui_rect(c, (int)(cx - h), (int)(cy + h - 2), (int)(h * 2), 2, col);
		ui_rect(c, (int)(cx - h), (int)(cy - h), 2, (int)(h * 2), col);
		ui_rect(c, (int)(cx + h - 2), (int)(cy - h), 2, (int)(h * 2), col);
		return (int)(r * 2 + 2);
	}
	case RSTV_KEY_TRIANGLE: {
		const uint32_t col = UI_RGBA(0x50, 0xE0, 0xC0, 0xFF);
		const float top = cy - r, bot = cy + r * 0.75f, half = r;
		thick_line(c, cx, top, cx - half, bot, col);
		thick_line(c, cx, top, cx + half, bot, col);
		thick_line(c, cx - half, bot, cx + half, bot, col);
		return (int)(r * 2 + 2);
	}
	default: {
		const char *name = rstv_key_name(key);
		ui_text(c, x, ty, text_col, 1.0f, name);
		return ui_text_width(1.0f, name);
	}
	}
}

static void item_key(UiCanvas *c, int px, int ty, int i, int sel, const char *name, int key)
{
	const uint32_t col = (i == sel) ? COL_SEL : COL_TEXT;
	item(c, px, ty, i, sel, name, NULL);
	int x = px + VALUE_X;
	ui_text(c, x, ty, col, 1.0f, "<");
	x += ui_text_width(1.0f, "< ");
	x += draw_key(c, x, ty - 7, ty, key, col);
	ui_text(c, x + 6, ty, col, 1.0f, ">");
}

// one line of text (size 0.8): symbol chars (0x10-0x13) drawn as button icons
static void text_keys(UiCanvas *c, int x, int ty, uint32_t col, const char *s)
{
	char run[128];
	int k = 0;
	for (const char *p = s;; p++) {
		const unsigned char ch = (unsigned char)*p;
		if ((ch >= 0x10 && ch <= 0x13) || !ch || k == (int)sizeof(run) - 1) {
			run[k] = 0;
			if (k) {
				ui_text(c, x, ty, col, 0.8f, run);
				x += ui_text_width(0.8f, run);
			}
			k = 0;
			if (!ch)
				break;
			if (ch >= 0x10 && ch <= 0x13) {
				static const int keys[4] = { RSTV_KEY_TRIANGLE, RSTV_KEY_CIRCLE, RSTV_KEY_CROSS, RSTV_KEY_SQUARE };
				x += draw_key(c, x, ty - 6, ty, keys[ch - 0x10], col) + 2;
				continue;
			}
		}
		run[k++] = (char)ch;
	}
}

// Draw the menu panel to the canvas (caller uploads). overlay = over the game list (dim without clearing the canvas, then the panel)
static int s_paint_x0, s_paint_y0, s_paint_x1, s_paint_y1;
static float s_mscr, s_mtgt; // menu rows scroll (px): drawn, target
static const uint32_t *s_under; // game list options: the dimmed list under the window (NULL: just drawn in full)
static int menu_anim(void) { return s_mscr != s_mtgt; } // (the menu is still scrolling: draw again)
static void menu_paint(UiCanvas *c, int overlay)
{
	const int o_ok = g_set.confirm == RSTV_CONFIRM_CIRCLE;
	const char *ok_name = o_ok ? G_CIR : G_CRO, *back_name = o_ok ? G_CRO : G_CIR;
	char foot_buf[128];
	const int n = page_items();
	// group gaps on the main page (rows y move down by the gaps above them)
	int gap_at[M_COUNT + 1] = { 0 };
	if (s_page == PAGE_MAIN && !s_list_mode)
		for (int i = 1; i <= n; i++) {
			const int k = i < n ? main_kind(i) : -1;
			gap_at[i] = gap_at[i - 1] + ((k == M_STATES || k == M_DISPLAY || k == M_ABOUT) ? GROUP_GAP : 0);
		}
	// ADR-194 (owner): rows need not fit. The row area is at most the screen minus a margin; taller pages scroll
	// smoothly (the selected row kept a row away from the edges), fading at a cut edge, with a scroll bar
	const int lh = s_page == PAGE_ABOUT ? ABOUT_LH : LINE_H;
	const int content = n * lh + gap_at[n > 0 ? n - 1 : 0], maxv = SCREEN_H - 32 - TITLE_H - FOOT_H;
	const int view = content < maxv ? content : maxv;
	const int h = TITLE_H + view + FOOT_H;
	const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
	{
		static int last_page = -1, last_n = -1;
		const float maxs = (float)(content - view);
		const int sy = s_sel * lh + gap_at[s_sel < n ? s_sel : 0];
		if (s_page == PAGE_ABOUT)
			s_mtgt = 0;
		else {
			if (sy - lh < s_mtgt)
				s_mtgt = (float)(sy - lh);
			if (sy + 2 * lh > s_mtgt + view)
				s_mtgt = (float)(sy + 2 * lh - view);
		}
		if (s_mtgt > maxs) s_mtgt = maxs;
		if (s_mtgt < 0) s_mtgt = 0;
		if (s_page != last_page || n != last_n)
			s_mscr = s_mtgt; // another page: no animation
		last_page = s_page;
		last_n = n;
		s_mscr += (s_mtgt - s_mscr) * 0.3f;
		if (fabsf(s_mtgt - s_mscr) < 0.5f)
			s_mscr = s_mtgt;
	}
	const int q0 = s_paint_x0, q1 = s_paint_y0, q2 = s_paint_x1, q3 = s_paint_y1; // what the last paint covered
	s_paint_x0 = px - 8; // area this paint covers (panel and shadow; the choice window adds its own below)
	s_paint_y0 = py - 8;
	s_paint_x1 = px + PANEL_W + 14;
	s_paint_y1 = py + h + 16;
	if (overlay) { // over the game list: it is already dimmed there
		s_drawn_h = 0;
		if (s_under) {
			// the last paint's area outside this window, from the kept list (the opaque window covers its inside;
			// its rounded corners come back too)
			const int ix0 = px + 8, iy0 = py + 8, ix1 = px + PANEL_W - 8, iy1 = py + h - 8;
			if (q1 < iy0) ui_blit_rect(c, s_under, q0, q1, q2 - q0, iy0 - q1);
			if (q3 > iy1) ui_blit_rect(c, s_under, q0, iy1, q2 - q0, q3 - iy1);
			const int m0 = q1 > iy0 ? q1 : iy0, m1 = q3 < iy1 ? q3 : iy1;
			if (m1 > m0) {
				if (q0 < ix0) ui_blit_rect(c, s_under, q0, m0, ix0 - q0, m1 - m0);
				if (q2 > ix1) ui_blit_rect(c, s_under, ix1, m0, q2 - ix1, m1 - m0);
			}
		}
	} else if (s_drawn_h == h) {
		// ADR-095: if the panel size is unchanged, redraw only the panel (do not recompose and copy the whole dimmed screen every time)
		// (with the shadow and the choice window of the last paint: they are outside the panel)
		const int x0 = q0 < s_paint_x0 ? q0 : s_paint_x0, y0 = q1 < s_paint_y0 ? q1 : s_paint_y0;
		const int x1 = q2 > s_paint_x1 ? q2 : s_paint_x1, y1 = q3 > s_paint_y1 ? q3 : s_paint_y1;
		ui_fill(c, x0, y0, x1 - x0, y1 - y0, COL_BG);
	} else {
		ui_fill(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG); // (set, not blended: a blend reads the whole screen, ~10 ms)
		s_drawn_h = h;
	}
	window_box(c, px, py, PANEL_W, h, COL_EDGE, COL_PANEL); // ADR-194: rounded panel, outline and shadow

	const char *title = "OPTIONS";
	snprintf(foot_buf, sizeof(foot_buf), "Up/Down: select   Left/Right/%s: change   %s: back", ok_name, back_name);
	const char *foot = foot_buf;
	switch (s_page) {
	case PAGE_DISPLAY:
		title = "DISPLAY";
		if (s_list_mode)
			foot = "Left/Right: change   (global settings)";
		break;
	case PAGE_TURBO:
		title = "TURBO";
		foot = "Hold the turbo key: the turbo button repeats";
		break;
	case PAGE_CONTROLS:
		title = "CONTROLS";
		foot = s_list_mode ? "Left/Right: choose button   (global settings)" : "Left/Right: choose button   Menu: SELECT+START";
		break;
	case PAGE_CHEATS:
		title = "CHEATS";
		foot = "Applies from the next game start (Reset game to use it now)";
		break;
	case PAGE_STATES:
		title = "SAVE / LOAD STATE";
		foot = "Quick save: SELECT+L   Quick load: SELECT+R";
		break;
	case PAGE_ABOUT:
		title = "ABOUT";
		snprintf(foot_buf, sizeof(foot_buf), "Up/Down: scroll (%d/%d)   %s: back", s_about_scroll + 1,
			ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE + 1 : 1, back_name);
		break;
	default:
		if (!s_list_mode && rstv_settings_has_game())
			foot = "This game uses its own settings (Delete game settings to undo)";
		break;
	}
	{
		// title: cut with "..." when it overflows the panel (ST-V set names are long)
		char tbuf[128];
		snprintf(tbuf, sizeof(tbuf), "%s", s_page == PAGE_MAIN && !s_list_mode ? s_title : title);
		if (s_page == PAGE_MAIN && !s_list_mode) { // the game name without the version in brackets
			char *br = strstr(tbuf, " (");
			if (br && br > tbuf)
				*br = 0;
		}
		int len = (int)strlen(tbuf);
		while (len > 4 && ui_text_width(1.1f, tbuf) > PANEL_W - 48) {
			len--;
			memcpy(tbuf + len - 3, "...", 4);
		}
		ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, tbuf);
	}

	const int ra = py + TITLE_H, rb = ra + view, scr = (int)floorf(s_mscr + 0.5f);
	ui_clip_y(c, ra, rb);
	s_focus_cy0 = ra;
	s_focus_cy1 = rb;
	for (int i = 0; i < n; i++) {
		const int ty = ra + i * lh + gap_at[i] - scr + lh / 2 + 7;
		if (ty + lh < ra || ty - lh > rb)
			continue;
		switch (s_page) {
		case PAGE_MAIN: {
			static const char *const names[M_COUNT] = { "Resume game", "Reset game", "Press TEST switch (BIOS menu)", "Save / Load state  >",
				"Cheats  >", "Display  >", "Controls  >", "Save game settings", "Delete game settings", "About  >", "Select game", "Exit",
				"Back", "Refresh ROM list" };
			const int k = main_kind(i);
			const char *name = names[k];
			char rbuf[64];
			if (k == M_RESET && s_reset_armed) {
				snprintf(rbuf, sizeof(rbuf), "Reset game - press %s again", o_ok ? "Circle" : "Cross");
				name = rbuf;
			}
			item(c, px, ty, i, s_sel, name, NULL);
			break;
		}
		case PAGE_DISPLAY: {
			static const char *const names[D_COUNT] = { "Show FPS", "Screen", "Rotate", "Filter", "Scanlines", "Scanline strength",
				"Border (4:3 / Integer)", "Border color", "Back" };
			const char *v = NULL;
			switch (i) {
			case D_FPS: v = g_set.fps ? VAL_ON : VAL_OFF; break;
			case D_ASPECT: v = aspect[g_set.aspect]; break;
			case D_ROTATE: v = rotate[g_set.rotate]; break;
			case D_FILTER: v = filter[g_set.filter]; break;
			case D_SCAN: v = scan[g_set.scan_type]; break;
			case D_SCAN_STR: v = scan_str[g_set.scan_strength]; break;
			case D_BORDER: v = border[g_set.border]; break;
			case D_BORDER_COL: v = border_col[g_set.border_color]; break;
			}
			item(c, px, ty, i, s_sel, names[i], v);
			break;
		}
		case PAGE_CONTROLS:
		case PAGE_TURBO: {
			int act;
			const int k = ctrl_kind(i, &act);
			if (k == CK_ACT)
				item_key(c, px, ty, i, s_sel, act_name(act), g_set.keymap[act]);
			else if (k == CK_TURBO_KEY)
				item_key(c, px, ty, i, s_sel, "Turbo key", g_set.turbo_key);
			else if (k == CK_TURBO_BTN) {
				char bbuf[16];
				snprintf(bbuf, sizeof(bbuf), "Button %d", g_set.turbo_btn);
				item(c, px, ty, i, s_sel, "Turbo button", bbuf);
			} else if (k == CK_TURBO_SPEED)
				item(c, px, ty, i, s_sel, "Turbo speed", turbo_speed[g_set.turbo_speed]);
			else if (k == CK_TURBO)
				item(c, px, ty, i, s_sel, "Turbo  >", NULL);
			else if (k == CK_STICK)
				item(c, px, ty, i, s_sel, "Analog stick", stick[g_set.stick]);
			else if (k == CK_CONFIRM)
				item_key(c, px, ty, i, s_sel, "Menu confirm button", o_ok ? RSTV_KEY_CIRCLE : RSTV_KEY_CROSS);
			else
				item(c, px, ty, i, s_sel, k == CK_DEFAULTS ? "Restore defaults" : "Back", NULL);
			break;
		}
		case PAGE_STATES:
			if (i == S_SLOT) {
				char sbuf[32];
				snprintf(sbuf, sizeof(sbuf), "%d  (%s)", s_state_slot, slot_used(s_state_slot) ? "used" : "empty");
				item(c, px, ty, i, s_sel, "Slot", sbuf);
			} else {
				static const char *const names[S_COUNT] = { "", "Save state", "Load state", "Back" };
				item(c, px, ty, i, s_sel, names[i], NULL);
			}
			break;
		case PAGE_CHEATS:
			if (i < rstv_cheats_count())
				item(c, px, ty, i, s_sel, rstv_cheats_name(i), rstv_cheats_value(i));
			else
				item(c, px, ty, i, s_sel, "Back", NULL);
			break;
		case PAGE_ABOUT: {
			const int k = s_about_scroll + i;
			if (k < ABOUT_LINES && s_about[k][0]) {
				const int head = s_about[k][0] == '#';
				ui_text(c, px + 24, ty, head ? COL_SEL : (s_about[k][0] == ' ' ? COL_DIM : COL_TEXT), head ? 1.0f : 0.85f,
					s_about[k] + head);
			}
			break;
		}
		}
	}
	if (content > view) { // fades where rows are cut (towards the panel colour)
		enum { FADE = 14 };
		for (int k = 0; k < FADE; k++) {
			const unsigned al = (unsigned)(0xE0 - k * 0xE0 / FADE);
			if (scr > 0)
				ui_rect(c, px + 2, ra + k, PANEL_W - 22, 1, UI_RGBA(0x0A, 0x1C, 0x46, al));
			if (scr < content - view)
				ui_rect(c, px + 2, rb - 1 - k, PANEL_W - 22, 1, UI_RGBA(0x0A, 0x1C, 0x46, al));
		}
	}
	ui_clip_y(c, 0, SCREEN_H);
	s_focus_cy0 = 0;
	s_focus_cy1 = SCREEN_H;
	if (s_page != PAGE_ABOUT) { // scroll bar (display only, owner)
		const int tx = px + PANEL_W - 16, t0 = ra + 4, th = view - 8;
		int bh = content > 0 ? th * view / content : th, by = content > 0 ? t0 + (int)(th * s_mscr / content) : t0;
		if (bh < 16) bh = 16;
		if (by + bh > t0 + th) by = t0 + th - bh;
		rrect(c, tx, t0, 4, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
		rrect(c, tx, by, 4, bh, 2, UI_RGBA(0x8C, 0xD4, 0xFF, 0xB0));
	}
	text_keys(c, px + 24, py + h - 18, COL_DIM, foot);
	if (s_pop) { // choice window over the panel
		const char *names[MAX_CHOICES], *title;
		int cur;
		const int nc = item_choices(names, &cur, &title);
		if (nc) {
			const int ww = 380, wh = 64 + nc * LINE_H + 12, wx = (SCREEN_W - ww) / 2, wy = (SCREEN_H - wh) / 2;
			if (wy - 8 < s_paint_y0) s_paint_y0 = wy - 8;
			if (wy + wh + 16 > s_paint_y1) s_paint_y1 = wy + wh + 16;
			ui_rect(c, px, py, PANEL_W, h, UI_RGBA(0x00, 0x00, 0x00, 0x80)); // dim the panel under it
			window_box(c, wx, wy, ww, wh, COL_SEL, COL_PANEL);
			ui_text(c, wx + 24, wy + 38, COL_TEXT, 1.0f, title);
			ui_rect(c, wx + 20, wy + 50, ww - 40, 1, COL_DIM);
			for (int i = 0; i < nc; i++) {
				const int ty = wy + 60 + i * LINE_H + LINE_H / 2 + 7, by = ty - 7 - (LINE_H - 6) / 2;
				if (i == s_pop_sel) {
					rrect(c, wx + 12, by + 3, ww - 24, LINE_H - 6, 6, UI_RGBA(0x00, 0x00, 0x10, 0x30));
					rrect(c, wx + 12, by, ww - 24, LINE_H - 6, 6, COL_HILITE);
					focus_at(c, wx + 12, by, ww - 24, LINE_H - 6);
				}
				if (s_choice_keys) // owner: button icons as in the item, not their names
					draw_key(c, wx + 44, ty - 7, ty, i, i == s_pop_sel ? COL_SEL : COL_TEXT);
				else
					ui_text(c, wx + 44, ty, i == s_pop_sel ? COL_SEL : COL_TEXT, 1.0f, names[i]);
				if (i == cur)
					ui_circle(c, (float)(wx + 28), (float)(ty - 7), 5.0f, i == s_pop_sel ? COL_SEL : COL_TEXT);
			}
		}
	}
}

void rstv_menu_draw(void)
{
	focus_tick(); // (the blink: the focus layer's opacity only)
	if (menu_anim())
		s_dirty = 1;
	if (!s_dirty || !s_cv)
		return;
	s_dirty = 0;
	focus_begin();
	menu_paint(s_cv, 0);
	focus_end();
	ui_commit(s_cv);
}

void rstv_menu_selftest(void)
{
	ensure_canvases();
	static const struct { int page, sel; const char *name; } shots[] = {
		{ PAGE_MAIN, 0, "main" }, { PAGE_STATES, 0, "states" }, { PAGE_DISPLAY, 3, "display" }, { PAGE_CONTROLS, 6, "controls" },
		{ PAGE_ABOUT, 0, "about" },
	};
	for (unsigned k = 0; k < sizeof(shots) / sizeof(shots[0]); k++) {
		s_page = shots[k].page;
		s_sel = shots[k].sel;
		s_dirty = 1;
		rstv_menu_draw();
		char p[96];
		snprintf(p, sizeof(p), DATA_DIR "/menu_%s.ppm", shots[k].name);
		ui_save_ppm(s_cv, p, UI_RGBA(0x50, 0x70, 0x90, 0xFF));
	}
	rstv_menu_hide();
	rstv_osd_show("Saved: quick");
	ui_save_ppm(s_osd_cv, DATA_DIR "/menu_osd.ppm", UI_RGBA(0x50, 0x70, 0x90, 0xFF));
	rstv_fps_update(59.9f, 1234);
	ui_save_ppm(s_fps_cv, DATA_DIR "/menu_fps.ppm", UI_RGBA(0x50, 0x70, 0x90, 0xFF));
	s_page = PAGE_MAIN;
	s_sel = 0;
}

void rstv_menu_hide(void)
{
	focus_off();
	if (s_cv)
		ui_hide(s_cv);
	s_dirty = 1;
	s_drawn_h = 0;
}

// ---- loading screen (same look as Reirem) ----
static char s_loading_title[96];
void rstv_loading_title(const char *title)
{
	snprintf(s_loading_title, sizeof(s_loading_title), "%s", title ? title : "");
}

static int s_loading_cur;
void rstv_loading_tick(void) { rstv_loading_progress(s_loading_cur); } // ADR-193: redraw (the bar band moves)
void rstv_loading_progress(int permille)
{
	ps_poll(); // (loading: a single PS press does nothing, a double press still goes home)
	static uint64_t last_t;
	static int last_p = -1000;
	const uint64_t now = rstv_time_us();
	if (permille > 0 && permille < 1000 && permille - last_p < 20 && now - last_t < 33000)
		return; // do not draw too often (each draw waits for vblank)
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	s_loading_cur = permille;
#ifdef RSTV_DEBUG_TOOLS
	if (last_t && now - last_t > 150000) // diag: where the loading screen went without a redraw
		RSTV_LOG("loading: no redraw for %u ms before %d", (unsigned)((now - last_t) / 1000), permille);
#endif
	last_p = permille;
	last_t = now;
	ensure_canvases();
	UiCanvas *c = s_cv;
	// bar at the bottom with a small margin left, right and below; the title above it
	const int m = 36, h = 14, w = SCREEN_W - 2 * m, x = m, y = SCREEN_H - m - h;
	s_drawn_h = 0;
	ui_clear(c);
	// ADR-191 (owner): the app's background art 20% transparent over black (80% brightness), loaded once
	static uint8_t *bg;
	static int bg_w, bg_h, bg_tried;
	if (!bg_tried) {
		bg_tried = 1;
		bg = rstv_video_load_bg(&bg_w, &bg_h);
	}
	if (bg)
		ui_image(c, bg, bg_w, bg_h, 205);
	else
		ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0, 0, 0, 0xFF));
	// ADR-193 (owner: livelier): capsule bar with a vertical gradient and gloss, stripes moving inside the filled part,
	// a glowing head, a spinner and the stage with its percentage
	const float r = h * 0.5f;
	const int fw = w * permille / 1000;
	ui_rect(c, x - 3, y - 3, w + 6, h + 6, UI_RGBA(0x00, 0x00, 0x00, 0x70)); // shade under the bar
	for (int yy = 0; yy < h; yy++) { // track and fill row by row (rounded ends)
		const float dy = yy + 0.5f - r, in = r - sqrtf(r * r - dy * dy);
		const int t0 = x + (int)in, t1 = x + w - (int)in;
		ui_rect(c, t0, y + yy, t1 - t0, 1, UI_RGBA(0x10, 0x18, 0x30, 0xE0));
		if (fw > 2) {
			const int f1 = x + fw - (fw > 2 * r ? (int)in : 0);
			const int g = yy * 255 / (h - 1);
			const uint32_t col = UI_RGBA(0x70 - (0x58 * g >> 8), 0xC8 - (0x58 * g >> 8), 0xFF - (0x20 * g >> 8), 0xFF);
			if (f1 > t0)
				ui_rect(c, t0, y + yy, f1 - t0, 1, col);
			if (yy < h / 3 && f1 > t0) // gloss
				ui_rect(c, t0, y + yy, f1 - t0, 1, UI_RGBA(0xFF, 0xFF, 0xFF, 0x38));
		}
	}
	if (fw > 2 * r) { // stripes drifting to the right
		const int step = 22, off = (int)((now / 25000) % step);
		for (int sx = -h + off; sx < fw; sx += step) {
			const float x0 = (float)(x + sx), x1 = x0 + h - 4;
			if (x0 >= x + r && x1 <= x + fw - r)
				ui_line(c, x0, (float)(y + h - 2), x1, (float)(y + 2), 6.0f, UI_RGBA(0xFF, 0xFF, 0xFF, 0x30));
		}
	}
	if (fw > 0) { // glowing head
		const float hx = (float)(x + fw), hy = y + r;
		const float pulse = 0.5f + 0.5f * sinf((float)(now % 1000000) * 6.2832f / 1e6f);
		ui_circle(c, hx, hy, r + 7 + 2 * pulse, UI_RGBA(0x60, 0xC0, 0xFF, 0x28));
		ui_circle(c, hx, hy, r + 3, UI_RGBA(0x90, 0xD8, 0xFF, 0x60));
		ui_circle(c, hx, hy, r - 1, UI_RGBA(0xE8, 0xF6, 0xFF, 0xFF));
	}
	// owner: centred text with room around everything. From the bottom: the bar, 50 px, the stage with its percentage
	// and the loading icon after it (26 px apart, centred as one group), 18 px, the game name (centred, clipped to the bar)
	extern volatile int gx_compiling;
	const char *stg = permille < 700 ? "Loading ROMs" : gx_compiling ? "Compiling shaders (first run only)" :
		permille < 1000 ? "Preparing graphics" : "Starting";
	char stage[80];
	snprintf(stage, sizeof(stage), "%s   %d%%", stg, permille / 10);
	const float icon_r = 11.0f; // dots on this circle, 3 px each: the icon is 28 px wide
	const int sb = y - 50, tw = ui_text_width(0.9f, stage), gap = 26, gw = tw + gap + 28, gx0 = (SCREEN_W - gw) / 2;
	ui_text(c, gx0, sb, COL_DIM, 0.9f, stage);
	const float scx = (float)(gx0 + tw + gap + 14), scy = (float)(sb - 5);
	const int ph = (int)((now / 70000) % 12);
	for (int i = 0; i < 12; i++) {
		const float a = i * 6.2832f / 12.0f;
		const int age = (ph - i + 12) % 12, al = 0xFF - age * 0x14;
		ui_circle(c, scx + icon_r * sinf(a), scy - icon_r * cosf(a), 3.0f, UI_RGBA(0xB0, 0xE0, 0xFF, al < 0x20 ? 0x20 : al));
	}
	const char *title = s_loading_title[0] ? s_loading_title : "Loading...";
	const int ttw = ui_text_width(0.9f, title), tx = ttw < w ? (SCREEN_W - ttw) / 2 : x;
	ui_text_clip(c, tx, sb - 18 - 20, COL_DIM, 0.9f, title, x, x + w);
#ifdef RSTV_DEBUG_TOOLS
	{ // check of the look (ux0:data/restv/loadshot): the canvas at 50% and 85% as menu_load<k>.ppm
		static int shot = -1, taken;
		if (shot < 0) { SceIoStat st; shot = sceIoGetstat(DATA_DIR "/loadshot", &st) >= 0; }
		if (shot && taken < 2 && permille >= (taken ? 850 : 500)) {
			char pth[64];
			snprintf(pth, sizeof(pth), DATA_DIR "/menu_load%d.ppm", taken++);
			ui_save_ppm(c, pth, UI_RGBA(0, 0, 0, 0xFF));
		}
	}
#endif
	ui_commit(c);
	rstv_video_present();
}

void rstv_loading_done(void)
{
	if (s_cv)
		ui_hide(s_cv);
}

// ---- notice (bottom center, 2 s) ----
static uint64_t s_osd_until;
static int s_osd_on;

void rstv_osd_show(const char *fmt, ...)
{
	char msg[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	ensure_canvases();
	UiCanvas *c = s_osd_cv;
	const int w = ui_text_width(1.0f, msg) + 40, h = 36;
	const int x = (SCREEN_W - w) / 2;
	ui_clear(c);
	ui_rect(c, x, 0, w, h, UI_RGBA(0x00, 0x00, 0x00, 0xC8));
	ui_rect(c, x, 0, w, 2, COL_SEL);
	ui_text(c, x + 20, 26, COL_TEXT, 1.0f, msg);
	ui_canvas_move(c, 0, SCREEN_H - h - 16);
	ui_commit(c);
	s_osd_until = rstv_time_us() + 2000000;
	s_osd_on = 1;
}

void rstv_osd_tick(void)
{
	if (s_osd_on && rstv_time_us() > s_osd_until) {
		ui_hide(s_osd_cv);
		s_osd_on = 0;
	}
}

// ---- FPS (top right, same box as Reirem, with frame number) ----
void rstv_fps_update(float fps, unsigned frame)
{
	static unsigned last;
	static int shown;
	if (!g_set.fps) {
		if (shown && s_fps_cv)
			ui_hide(s_fps_cv);
		shown = 0;
		return;
	}
	if (shown && frame - last < 6)
		return;
	last = frame;
	shown = 1;
	ensure_canvases();
	UiCanvas *c = s_fps_cv;
	char buf[48];
#ifdef RSTV_DEBUG_TOOLS
	snprintf(buf, sizeof(buf), "%.1f FPS  F%u", (double)fps, frame);
#else
	snprintf(buf, sizeof(buf), "%d FPS", (int)(fps + 0.5f)); // owner: release builds round (59.8 -> 60), no frame number
#endif
	const int w = ui_text_width(1.0f, buf) + 16;
	const uint32_t col = fps >= 59.0f ? UI_RGBA(0x60, 0xFF, 0x60, 0xFF) : UI_RGBA(0xFF, 0xC0, 0x40, 0xFF);
	ui_clear(c);
	ui_rect(c, 240 - w, 0, w, 30, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
	ui_text(c, 240 - w + 8, 22, col, 1.0f, buf);
	// owner: the FPS box follows the game's direction when the Vita is held upright (Vertical L / R): the reader's
	// top-right corner, turned with the picture (the menu stays as it is)
	if (rstv_input_rot == 90) {
		rstv_video_layer_rot(3, 1);
		ui_canvas_move(c, SCREEN_W - 4 - 30, SCREEN_H - 4 - 240);
	} else if (rstv_input_rot == 270) {
		rstv_video_layer_rot(3, 3);
		ui_canvas_move(c, 4, 4);
	} else {
		rstv_video_layer_rot(3, 0);
		ui_canvas_move(c, SCREEN_W - 4 - 240, 4);
	}
	ui_commit(c);
}

// ================================ start screen game select ================================
// ADR-096·126 (same look and behavior as Reirem rei_game_select): all MAME ST-V and System C/C2 games (stv_list.h). Tab = board, switched with L/R.
// SELECT = filter (Playable: games with ROMs / All: everything, games without ROMs gray, picking one shows a notice / Favorites). □ = favorite on/off (★ mark),
// START = options (Display, Controls, global settings), △ = About. Bright if ROMs exist even when not yet supported (ADR-106, picking shows a notice)
#define COL_GREY     UI_RGBA(0x6A, 0x7C, 0x9C, 0xFF)
#define COL_GREY_SEL UI_RGBA(0xA8, 0xB8, 0xD4, 0xFF)
static int s_gl_present[RSTV_LIST_N], s_gl_supported[RSTV_LIST_N];
static int gl_playable(int i) { return s_gl_present[i]; }
static const char *const s_gl_filter_name[RSTV_LIST_FILTER_COUNT] = { "Playable", "All", "Favorites" };

// games visible under the current filter
static int gl_visible(int i)
{
	if (g_set.list_filter == RSTV_LIST_ALL)
		return 1;
	if (g_set.list_filter == RSTV_LIST_FAVORITES)
		return rstv_fav_is(s_stv_list[i].set);
	return gl_playable(i);
}

#define MAX_TABS 8
static const char *s_gl_tabs[MAX_TABS];
static int s_gl_ntabs, s_gl_tab, s_gl_nvis;

// Rebuild tabs from games matching the filter and move to the tab/position of want (list index, may be -1). Game indices of the current tab go in idx
static int gl_build(int *idx, int want, int *sel)
{
	const char *cur = s_gl_tab < s_gl_ntabs ? s_gl_tabs[s_gl_tab] : NULL;
	s_gl_ntabs = 0;
	s_gl_nvis = 0;
	for (int i = 0; i < RSTV_LIST_N; i++) {
		if (!gl_visible(i))
			continue;
		s_gl_nvis++;
		int t = 0;
		while (t < s_gl_ntabs && strcmp(s_gl_tabs[t], s_stv_list[i].board))
			t++;
		if (t == s_gl_ntabs && s_gl_ntabs < MAX_TABS)
			s_gl_tabs[s_gl_ntabs++] = s_stv_list[i].board;
	}
	s_gl_tab = 0;
	const char *tb = want >= 0 && gl_visible(want) ? s_stv_list[want].board : cur;
	for (int t = 0; tb && t < s_gl_ntabs; t++)
		if (!strcmp(s_gl_tabs[t], tb))
			s_gl_tab = t;
	int n = 0;
	*sel = 0;
	for (int i = 0; i < RSTV_LIST_N && s_gl_ntabs; i++) {
		if (!gl_visible(i) || strcmp(s_stv_list[i].board, s_gl_tabs[s_gl_tab]))
			continue;
		if (i == want)
			*sel = n;
		idx[n++] = i;
	}
	return n;
}

// which ROM zips are in rom_dir (start, and R in the list: owner, ROMs copied while the app runs). Games that can run
static int gl_scan(const char *rom_dir)
{
	int nplay = 0;
	for (int i = 0; i < RSTV_LIST_N; i++) {
		char path[256];
		snprintf(path, sizeof(path), "%s/%s.zip", rom_dir, s_stv_list[i].set);
		SceIoStat st;
		s_gl_present[i] = sceIoGetstat(path, &st) >= 0;
		s_gl_supported[i] = STVFindGame(s_stv_list[i].set) != NULL || c2_is_game(s_stv_list[i].set); // ADR-107
		nplay += gl_playable(i);
	}
	return nplay;
}

// one row of the game list (title at x + 48): the selected one with a soft shadow below, a bar, the blinking focus
// outline and a left accent; favourites get a star; off = how far a long title has scrolled
// selected: 0 no, 1 the focus (blinks), 2 the focus under an open window (steady outline)
static void gl_row(UiCanvas *c, int px, int w, int ty, int g, int selected, int off)
{
	const int okg = gl_playable(g);
	const uint32_t col = selected ? (okg ? COL_TEXT : COL_GREY_SEL) : (okg ? UI_RGBA(0xD6, 0xE2, 0xFA, 0xFF) : COL_GREY);
	if (selected) {
		const int by = ty - 7 - (GL_LINE - 6) / 2; // bar centred on the text
		rrect(c, px + 12, by + 3, w - 24, GL_LINE - 6, 6, UI_RGBA(0x00, 0x00, 0x10, 0x30)); // shadow (below only)
		rrect(c, px + 13, by + 5, w - 26, GL_LINE - 6, 6, UI_RGBA(0x00, 0x00, 0x10, 0x20));
		rrect(c, px + 12, by, w - 24, GL_LINE - 6, 6, COL_HILITE);
		if (selected == 2)
			rrect_outline(c, px + 12, by, w - 24, GL_LINE - 6, 6, 2, COL_FOCUS_STILL);
		else
			focus_at(c, px + 12, by, w - 24, GL_LINE - 6);
	}
	if (rstv_fav_is(s_stv_list[g].set))
		star(c, (float)(px + 36), (float)(ty - 7), 7.0f, COL_STAR);
	ui_text_clip(c, px + 48 - off, ty, col, 1.0f, s_stv_list[g].title, px + 48, px + w - 24);
}

const char *rstv_game_select(const char *initial_set, const char *rom_dir)
{
	ensure_canvases();
	UiCanvas *c = s_cv;
	int want = -1;
	const int nplay = gl_scan(rom_dir);
	for (int i = 0; i < RSTV_LIST_N; i++)
		if (initial_set && !strcmp(initial_set, s_stv_list[i].set))
			want = i;
	int auto_all = 0; // the filter went to All only because no game could run
	if (!nplay && g_set.list_filter == RSTV_LIST_PLAYABLE) {
		g_set.list_filter = RSTV_LIST_ALL; // (show everything if no game can run)
		auto_all = 1;
	}
	static int idx[RSTV_LIST_N];
	int sel = 0, about = 0, dirty = 1;
	uint64_t rep = 0;
	float scr = -1, tgt = 0; // list scroll (rows): drawn position easing to the target
	static uint32_t *under; // the dimmed list under the options window
	int under_ok = 0;
	int mq_sel = -1, mq_t = 0, mq_y = 0, mq_px = 0, mq_w = 0, mq_tw = 0; // scrolling title of the selected row (marquee)
	s_gl_ntabs = s_gl_tab = 0;
	int n = gl_build(idx, want, &sel);
	ui_clear(c); // whole canvas the first time (what loading/menu left there)
	char msg[256] = { 0 };
	uint32_t prev = 0xffffffffu; // ignore until released once, so the press that launched the app does not pick right away
	// test (ux0:data/restv/gstest): first screen → favorite → filter → filter → options → close options, as menu_gs<k>.ppm (simulated presses)
	SceIoStat gst;
	int gstest = sceIoGetstat(DATA_DIR "/gstest", &gst) >= 0 ? 1 : 0, gstep = 0;
	if (gstest)
		sceIoRemove(DATA_DIR "/gstest");
	for (unsigned tick = 0;; tick++) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t b = pad.buttons;
		if ((tick & 63) == 63) {
			// no auto suspend in the list either (a device left here went to sleep and looked switched off)
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
			// exit request of the measuring tools (ADR-090) also in this list: an app left here kept every remote launch out
			SceIoStat est;
			if (sceIoGetstat(DATA_DIR "/exit", &est) >= 0) {
				char xc = 0;
				SceUID xf = sceIoOpen(DATA_DIR "/exit", SCE_O_RDONLY, 0);
				if (xf >= 0) {
					sceIoRead(xf, &xc, 1);
					sceIoClose(xf);
				}
				sceIoRemove(DATA_DIR "/exit");
				RSTV_LOG("exit: request in the game list");
				if (xc == 'n') // "next": relaunch (autostart picks the game)
					sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
				sceKernelExitProcess(0);
			}
		}
		if (gstest) {
			// then (ADR-194) options -> Controls -> up to the Turbo row (the page scrolls) -> Turbo page -> turbo key
			// choice window (button icons) -> back out. A step waits until scrolling has settled
			const uint32_t gok = g_set.confirm == RSTV_CONFIRM_CIRCLE ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
			const uint32_t gbk = gok == SCE_CTRL_CIRCLE ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
			static const int GS_N = 18;
			const uint32_t seq[18] = { 0, SCE_CTRL_SQUARE, SCE_CTRL_SELECT, SCE_CTRL_SELECT, SCE_CTRL_START, SCE_CTRL_DOWN, gok,
				SCE_CTRL_UP, SCE_CTRL_UP, SCE_CTRL_UP, SCE_CTRL_UP, SCE_CTRL_UP, gok, gok, gbk, gbk, gbk, gbk };
			static int gs_rel; // a pass with no button between steps (the options see each press as new)
			if (gstep > 0 && (scr != tgt || menu_anim()))
				; // (still scrolling)
			else if (gs_rel) {
				gs_rel = 0;
				prev = 0;
				b = 0;
			} else {
				gs_rel = 1;
				if (gstep > 0) {
					char gp[64];
					snprintf(gp, sizeof(gp), DATA_DIR "/menu_gs%d.ppm", gstep);
					ui_save_ppm(c, gp, UI_RGBA(0, 0, 0, 0xFF));
				}
				static const char *gs_set; // game the test marked favorite (restored at the end)
				if (gstep == 1 && n)
					gs_set = s_stv_list[idx[sel]].set;
				if (gstep >= GS_N) {
					gstest = 0;
					if (gs_set)
						rstv_fav_toggle(gs_set);
				} else {
					prev = 0;
					b = seq[gstep];
				}
				gstep++;
			}
		}
		if (pad.ly < 128 - 48) b |= SCE_CTRL_UP;
		if (pad.ly > 128 + 48) b |= SCE_CTRL_DOWN;
		if (pad.lx < 128 - 48) b |= SCE_CTRL_LEFT;
		if (pad.lx > 128 + 48) b |= SCE_CTRL_RIGHT;
		if (!s_list_mode && ps_poll()) { // owner: the PS button opens the options here too (rstv_menu_update closes them)
			about = 0;
			msg[0] = 0;
			rstv_settings_use_global();
			s_list_mode = 1;
			s_page = PAGE_MAIN;
			s_sel = s_main_sel = 0;
			s_prev_buttons = b;
			dirty = 1;
		}
		const uint32_t pressed = key_repeat(b, b & ~prev, &rep);
		prev = b;
		if (pressed)
			dirty = 1;
		const int o_ok = g_set.confirm == RSTV_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		if (s_list_mode) { // options (Display / Controls, global settings. Saved by rstv_menu_update on close)
			const rstv_menu_result mr = rstv_menu_update(b);
			if (mr != RSTV_MENU_STAY) {
				s_list_mode = 0;
				s_page = PAGE_MAIN;
				ui_clear(c);
			}
			if (mr == RSTV_MENU_REFRESH) { // owner: ROMs copied while the app runs
				const int cur = n ? idx[sel] : -1;
				const int np = gl_scan(rom_dir);
				if (auto_all && np && g_set.list_filter == RSTV_LIST_ALL) {
					g_set.list_filter = RSTV_LIST_PLAYABLE; // ROMs now there: back to the games that run
					auto_all = 0;
				}
				n = gl_build(idx, cur, &sel);
				scr = -1; // (new list: no scroll animation)
				snprintf(msg, sizeof(msg), "ROM list refreshed: %d game%s ready@in %s", np, np == 1 ? "" : "s", rom_dir);
				for (char *q = msg; *q; q++)
					if (*q == '@')
						*q = 10; // second line of the notice
				dirty = 1;
			}
		} else if (msg[0]) { // notice box: any button closes it
			if (pressed & (ok | back | SCE_CTRL_START | SCE_CTRL_TRIANGLE))
				msg[0] = 0;
		} else if (about) {
			const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
			if ((pressed & SCE_CTRL_UP) && s_about_scroll > 0) s_about_scroll--;
			if ((pressed & SCE_CTRL_DOWN) && s_about_scroll < max) s_about_scroll++;
			if (pressed & (ok | back | SCE_CTRL_TRIANGLE | SCE_CTRL_START))
				about = 0;
		} else {
			if (n && (pressed & SCE_CTRL_UP)) sel = wrap(sel - 1, n);
			if (n && (pressed & SCE_CTRL_DOWN)) sel = wrap(sel + 1, n);
			// left/right: one page, L/R: board tab
			if (n && (pressed & SCE_CTRL_LEFT)) sel = sel >= 12 ? sel - 12 : 0;
			if (n && (pressed & SCE_CTRL_RIGHT)) sel = sel + 12 < n ? sel + 12 : n - 1;
			if (s_gl_ntabs > 1 && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
				s_gl_tab = wrap(s_gl_tab + ((pressed & SCE_CTRL_LTRIGGER) ? -1 : 1), s_gl_ntabs);
				int first = -1; // first game of that tab
				for (int i = 0; i < RSTV_LIST_N && first < 0; i++)
					if (gl_visible(i) && !strcmp(s_stv_list[i].board, s_gl_tabs[s_gl_tab]))
						first = i;
				n = gl_build(idx, first, &sel);
				scr = -1;
			} else if (pressed & SCE_CTRL_SELECT) { // filter: Playable → All → Favorites (current game kept if visible)
				g_set.list_filter = wrap(g_set.list_filter + 1, RSTV_LIST_FILTER_COUNT);
				rstv_settings_save();
				n = gl_build(idx, n ? idx[sel] : -1, &sel);
				scr = -1;
			} else if (n && (pressed & SCE_CTRL_SQUARE)) { // favorite on/off (if it drops out of Favorites the cursor goes to the next game, or the previous one at the end)
				const int cur = idx[sel];
				rstv_fav_toggle(s_stv_list[cur].set);
				int w2 = cur;
				if (g_set.list_filter == RSTV_LIST_FAVORITES && !gl_visible(cur))
					w2 = sel + 1 < n ? idx[sel + 1] : (sel > 0 ? idx[sel - 1] : -1);
				n = gl_build(idx, w2, &sel);
			} else if (pressed & SCE_CTRL_TRIANGLE) {
				about = 1;
				s_about_scroll = 0;
			} else if (pressed & SCE_CTRL_START) { // options: edit and save global settings only
				rstv_settings_use_global();
				s_list_mode = 1;
				s_page = PAGE_MAIN;
				s_sel = s_main_sel = 0;
				s_prev_buttons = b; // (so the START press does not act inside options)
			} else if (n && (pressed & ok)) {
				const int g = idx[sel];
				if (gl_playable(g) && s_gl_supported[g]) {
					s_drawn_h = 0;
					focus_off();
					ui_hide(c);
					return s_stv_list[g].set;
				}
				const char *par = s_stv_list[g].parent;
				if (!s_gl_present[g])
					snprintf(msg, sizeof(msg), "ROM not found: %s.zip%s%s%s\nPut the ROM zip in %s", s_stv_list[g].set,
						par ? "  (clone of " : "", par ? par : "", par ? ")" : "", rom_dir);
				else
					snprintf(msg, sizeof(msg), "Not supported yet: %s\nThis game does not run in this version", s_stv_list[g].set);
			}
		}
		focus_tick();
		if (s_list_mode && menu_anim())
			dirty = 1; // the options window is still scrolling
		if (!s_list_mode && under_ok) { // options closed: the list is drawn again in full
			under_ok = 0;
			dirty = 1;
		}
		// ADR-194 (owner): the list scrolls smoothly and fades out at its top and bottom edges; rows need not fit
		const int tab_h = 40, la = TITLE_H + tab_h, lb = SCREEN_H - LIST_FOOT_H, lpad = 18, fade = 20;
		const float rows_f = (float)(lb - la - 2 * lpad) / GL_LINE;
		{
			float maxt = (float)n - rows_f;
			if (maxt < 0)
				maxt = 0;
			if (sel < tgt + 1)
				tgt = (float)sel - 1;
			if (sel > tgt + rows_f - 2)
				tgt = (float)sel - rows_f + 2;
			if (tgt > maxt) tgt = maxt;
			if (tgt < 0) tgt = 0;
			if (scr < 0)
				scr = tgt;
			else if (scr - tgt > 1.5f * rows_f || tgt - scr > 1.5f * rows_f) // long jumps: ease over the last half page only
				scr = scr < tgt ? tgt - 0.5f * rows_f : tgt + 0.5f * rows_f;
			if (scr != tgt && !s_list_mode) {
				scr += (tgt - scr) * 0.3f;
				if (fabsf(tgt - scr) < 0.01f)
					scr = tgt;
				dirty = 1;
			}
		}
		if (dirty) {
			dirty = 0;
			const int h = SCREEN_H, w = SCREEN_W;
			const int px = 0, py = 0;
			const uint32_t *bgr = theme_rows(1);
			s_drawn_h = 0;
			focus_begin();
			if (s_list_mode && under_ok) {
				// options over the list: menu_paint puts back the dimmed list kept when they opened, around the window
				s_under = under;
			} else if (about) {
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, SCREEN_H);
				ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, "ABOUT");
				for (int i = 0; i < ABOUT_VISIBLE && s_about_scroll + i < ABOUT_LINES; i++) {
					const char *t = s_about[s_about_scroll + i];
					if (!t[0])
						continue;
					const int head = t[0] == '#';
					ui_text(c, px + 24, py + TITLE_H + i * ABOUT_LH + 22, head ? COL_SEL : (t[0] == ' ' ? COL_DIM : COL_TEXT),
						head ? 1.0f : 0.85f, t + head);
				}
				text_keys(c, px + 24, py + h - 13, COL_DIM,
					o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
			} else {
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, SCREEN_H);
				const int rw = w - 16; // (room for the scroll bar)
				const int still = s_list_mode || msg[0];
				s_focus_cy0 = la;
				s_focus_cy1 = lb;
				const int i0 = (int)floorf(scr), i1 = (int)ceilf(scr + rows_f + 2.0f * lpad / GL_LINE);
				for (int i = i0 < 0 ? 0 : i0; i < n && i <= i1; i++) {
					const int ty = la + lpad + (int)floorf(((float)i - scr) * GL_LINE + 0.5f) + GL_LINE / 2 + 7;
					gl_row(c, px, rw, ty, idx[i], i == sel ? (still ? 2 : 1) : 0, 0);
					if (i == sel) { // titles longer than the cell scroll (only this row is redrawn each frame below)
						if (mq_sel != idx[i])
							mq_t = 0;
						mq_sel = idx[i];
						mq_y = ty;
						mq_px = px;
						mq_w = rw;
						mq_tw = ui_text_width(1.0f, s_stv_list[idx[i]].title);
					}
				}
				if (!n)
					mq_sel = -1;
				s_focus_cy0 = 0;
				s_focus_cy1 = SCREEN_H;
				for (int k = 0; k < fade; k++) { // fade bands: the background over the rows, opaque at the edge
					const unsigned al = (unsigned)(255 - (k + 1) * 255 / (fade + 1));
					ui_rect(c, 0, la + k, rw, 1, (bgr[la + k] & 0xFFFFFFu) | (al << 24));
					ui_rect(c, 0, lb - 1 - k, rw, 1, (bgr[lb - 1 - k] & 0xFFFFFFu) | (al << 24));
				}
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, la); // rows that scrolled past the edges
				ui_fill_rows(c, bgr, 0, lb, SCREEN_W, SCREEN_H - lb);
				if (n) { // scroll bar (display only, owner)
					const int tx = SCREEN_W - 14, ty0 = la + 8, th = lb - la - 16;
					const float vis = rows_f < (float)n ? rows_f : (float)n;
					int bh = (int)(th * vis / n), by = ty0 + (int)(th * scr / n);
					if (bh < 16) bh = 16;
					if (by + bh > ty0 + th) by = ty0 + th - bh;
					rrect(c, tx, ty0, 5, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
					rrect(c, tx, by, 5, bh, 2, UI_RGBA(0x8C, 0xD4, 0xFF, 0xB0));
				}
				char title[96];
				snprintf(title, sizeof(title), "ReST-V (" RSTV_VERSION ") - SELECT GAME (%d/%d)", n ? sel + 1 : 0, n);
				ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, title);
				char have[40];
				snprintf(have, sizeof(have), "%s %d", s_gl_filter_name[g_set.list_filter], s_gl_nvis);
				ui_text(c, px + w - 24 - ui_text_width(0.85f, have), py + 34, COL_DIM, 0.85f, have);
				{ // tab row: current tab highlighted; on overflow, scrolled so the current tab stays visible
					int tx = px + 16, first = 0, total = 0;
					for (int t = 0; t < s_gl_ntabs; t++)
						total += ui_text_width(0.95f, s_gl_tabs[t]) + 28;
					while (total > w - 32 && first < s_gl_tab) {
						total -= ui_text_width(0.95f, s_gl_tabs[first]) + 28;
						first++;
					}
					for (int t = first; t < s_gl_ntabs; t++) {
						const int tw = ui_text_width(0.95f, s_gl_tabs[t]) + 20;
						if (tx + tw > px + w - 16)
							break;
						if (t == s_gl_tab) // ADR-194: current tab bright with an accent underline
							rrect(c, tx + 6, py + TITLE_H + 26, tw - 12, 4, 2, COL_SEL);
						ui_text(c, tx + 10, py + TITLE_H + 20, t == s_gl_tab ? COL_TEXT : COL_DIM, 0.95f, s_gl_tabs[t]);
						tx += tw + 8;
					}
				}
				if (!n && g_set.list_filter == RSTV_LIST_FAVORITES)
					text_keys(c, px + 48, la + 40, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
				else if (!n)
					ui_text(c, px + 48, la + 40, COL_DIM, 1.0f, "No playable games. SELECT: show all");
				{ // button help on one line (owner)
					char foot[192];
					snprintf(foot, sizeof(foot), "%s: play   " G_SQU ": favorite   " G_TRI ": about   %sLeft/Right: page   SELECT: %s   START: options",
						o_ok ? G_CIR : G_CRO, s_gl_ntabs > 1 ? "L/R: system   " : "",
						s_gl_filter_name[wrap(g_set.list_filter + 1, RSTV_LIST_FILTER_COUNT)]);
					text_keys(c, px + 24, py + h - 13, COL_DIM, foot);
				}
				if (msg[0]) { // notice box (over the list)
					const int mw = 720, mh = 150, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
					window_box(c, mx, my, mw, mh, COL_EDGE, COL_PANEL);
					char line[256];
					snprintf(line, sizeof(line), "%s", msg);
					char *nl = strchr(line, '\n');
					if (nl)
						*nl = 0;
					ui_text(c, mx + 24, my + 44, COL_WARN, 1.0f, line);
					if (nl)
						ui_text(c, mx + 24, my + 84, COL_TEXT, 0.9f, nl + 1);
					ui_text(c, mx + 24, my + mh - 20, COL_DIM, 0.8f, "Press any button");
				}
				s_under = NULL;
				if (s_list_mode) { // options just opened: dim the list once and keep it (the window redraws over it)
					ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
					if (!under)
						under = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
					if (under) {
						memcpy(under, ui_pixels(c), (size_t)SCREEN_W * SCREEN_H * 4);
						under_ok = 1;
					}
				}
			}
			if (s_list_mode && !about)
				menu_paint(c, 1);
			focus_end();
			ui_commit(c);
		} else if (!about && !msg[0] && !s_list_mode && mq_sel >= 0 && mq_tw > mq_w - 72) {
			// the selected row's title scrolls when longer than the row (ADR-099): 1 s pause → 60 px/s to the end →
			// 1 s pause → from the start. Only this row is drawn again
			const int avail = mq_w - 72, over = mq_tw - avail, hold = 60;
			const int cyc = hold + over + hold;
			const int t = mq_t++ % cyc;
			const int off = t < hold ? 0 : t < hold + over ? t - hold : over;
			const int by = mq_y - 7 - (GL_LINE - 6) / 2;
			ui_fill_rows(c, theme_rows(1), mq_px + 6, by - 2, mq_w - 12, GL_LINE + 4);
			gl_row(c, mq_px, mq_w, mq_y, mq_sel, 1, off);
			ui_commit(c);
		}
		rstv_video_present();
	}
}
