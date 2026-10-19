// In-game menu, start screen game select, notices, FPS display (ADR-094, ADR-126). Same layout, colors, controls and features as
// Reirem (sister emulator, vita/src/frontend/menu.c, settings.c): favorites, list options, per-game settings, scanlines/border, coin/start mapping, one turbo key.
// Drawing is in ui.c (system default PGF font drawn to a CPU canvas and overlaid on the screen).
#include "frontend/menu.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
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
#define LINE_H   30
#define TITLE_H  56
#define FOOT_H   48
#define VALUE_X  330 // from panel left edge to the value column

#define COL_BG    UI_RGBA(0x00, 0x00, 0x00, 0xC0)
#define COL_PANEL UI_RGBA(0x18, 0x20, 0x48, 0xF0)
#define COL_TEXT  UI_RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_DIM   UI_RGBA(0xA0, 0xA8, 0xC0, 0xFF)
#define COL_SEL   UI_RGBA(0xF0, 0x90, 0x20, 0xFF)
#define COL_WARN  UI_RGBA(0xFF, 0x70, 0x60, 0xFF)

// button symbols inside text (drawn by draw_key)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"

#define STATE_SLOTS 10

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
	g_set.fps = 1;
	g_set.aspect = RSTV_ASPECT_4_3;
	g_set.filter = RSTV_FILTER_SMOOTH;
	g_set.scan_type = RSTV_SCAN_OFF;
	g_set.scan_strength = RSTV_SCAN_MEDIUM;
	g_set.border = RSTV_BORDER_BLACK;
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

void rstv_settings_apply_video(void)
{
	// rstv_video_set_mode: 0 = 4:3, 1 = integer, 2 = full / 0 = sharp, 1 = smooth
	const int scale = g_set.aspect == RSTV_ASPECT_FULL ? 2 : g_set.aspect == RSTV_ASPECT_INT ? 1 : 0;
	rstv_video_set_mode(scale, g_set.filter == RSTV_FILTER_SMOOTH);
	rstv_video_set_style(g_set.scan_type, g_set.scan_strength, g_set.border, g_set.border_color);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// items per-game settings can override (global values in s_global)
typedef struct {
	int fps, aspect, filter, scan_type, scan_strength, border, border_color;
	int keymap[RSTV_ACT_COUNT], stick, turbo_key, turbo_btn, turbo_speed;
} game_part;
static game_part s_global;
static int s_game_on;   // per-game settings are active for the current game
static char s_game[32]; // current game set name (<set>_game.cfg)
static void part_get(game_part *g)
{
	g->fps = g_set.fps; g->aspect = g_set.aspect; g->filter = g_set.filter;
	g->scan_type = g_set.scan_type; g->scan_strength = g_set.scan_strength;
	g->border = g_set.border; g->border_color = g_set.border_color;
	memcpy(g->keymap, g_set.keymap, sizeof(g->keymap));
	g->stick = g_set.stick; g->turbo_key = g_set.turbo_key; g->turbo_btn = g_set.turbo_btn; g->turbo_speed = g_set.turbo_speed;
}
static void part_set(const game_part *g)
{
	g_set.fps = g->fps; g_set.aspect = g->aspect; g_set.filter = g->filter;
	g_set.scan_type = g->scan_type; g_set.scan_strength = g->scan_strength;
	g_set.border = g->border; g_set.border_color = g->border_color;
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
	fprintf(f, "border=%d\nborder_color=%d\n", g->border, g->border_color);
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
enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_ABOUT, PAGE_CHEATS };
enum { M_RESUME, M_RESET, M_STATES, M_DISPLAY, M_CONTROLS, M_CHEATS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME, M_EXIT, M_BACK, M_COUNT };
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
enum { D_FPS, D_ASPECT, D_FILTER, D_SCAN, D_SCAN_STR, D_BORDER, D_BORDER_COL, D_BACK, D_COUNT };
// Controls rows: game buttons (as many as the game uses, 4 in list options), coin, start, turbo key, turbo button, turbo speed, analog stick,
// confirm button, defaults, back
enum { CK_ACT, CK_TURBO_KEY, CK_TURBO_BTN, CK_TURBO_SPEED, CK_STICK, CK_CONFIRM, CK_DEFAULTS, CK_BACK };
static int s_nbuttons = 4;
static int ctrl_buttons(void) { return s_list_mode ? RSTV_GAME_BUTTONS : s_nbuttons; }
static int ctrl_rows(void) { return ctrl_buttons() + 2 + 7; }
static int ctrl_kind(int i, int *act)
{
	const int b = ctrl_buttons();
	*act = -1;
	if (i < b) {
		*act = RSTV_ACT_B1 + i;
		return CK_ACT;
	}
	if (i < b + 2) {
		*act = i == b ? RSTV_ACT_COIN : RSTV_ACT_START;
		return CK_ACT;
	}
	return CK_TURBO_KEY + (i - b - 2);
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

int rstv_menu_should_open(uint32_t buttons)
{
	const int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int open = combo && !prev_combo;
	s_prev_buttons = buttons; // so the same press does not act right away inside the menu
	if (open) {
		s_page = PAGE_MAIN;
		s_sel = 0;
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
	case PAGE_CONTROLS: return ctrl_rows();
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

// repeat while a direction is held (after about 0.4 s, every 0.07 s)
#define REPEAT_KEYS (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)
static uint32_t key_repeat(uint32_t held, uint32_t pressed, int *count)
{
	const uint32_t dir = held & REPEAT_KEYS;
	if (!dir || (pressed & REPEAT_KEYS)) {
		*count = 0;
		return pressed;
	}
	++*count;
	if (*count >= 24 && (*count - 24) % 4 == 0)
		pressed |= dir;
	return pressed;
}

static int wrap(int v, int n)
{
	return (v % n + n) % n;
}

static void change(int dir)
{
	switch (s_page) {
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: g_set.fps = !g_set.fps; break;
		case D_ASPECT: g_set.aspect = wrap(g_set.aspect + dir, RSTV_ASPECT_COUNT); break;
		case D_FILTER: g_set.filter = wrap(g_set.filter + dir, RSTV_FILTER_COUNT); break;
		case D_SCAN: g_set.scan_type = wrap(g_set.scan_type + dir, RSTV_SCAN_COUNT); break;
		case D_SCAN_STR: g_set.scan_strength = wrap(g_set.scan_strength + dir, RSTV_SCAN_STRENGTH_COUNT); break;
		case D_BORDER: g_set.border = wrap(g_set.border + dir, RSTV_BORDER_COUNT); break;
		case D_BORDER_COL: g_set.border_color = wrap(g_set.border_color + dir, RSTV_BCOL_COUNT); break;
		}
		rstv_settings_apply_video();
		break;
	case PAGE_CONTROLS: {
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
		case M_EXIT: return RSTV_MENU_EXIT;
		case M_BACK: return RSTV_MENU_CLOSE;
		}
		break;
	case PAGE_DISPLAY:
		if (s_sel == D_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_CONTROLS: {
		int act;
		const int k = ctrl_kind(s_sel, &act);
		if (k == CK_BACK) go_page(PAGE_MAIN);
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
	static int rep_count;
	uint32_t pressed = key_repeat(buttons, buttons & ~s_prev_buttons, &rep_count);
	s_prev_buttons = buttons;
	if (pressed)
		s_dirty = 1;
	rstv_menu_result r = RSTV_MENU_STAY;
	const int n = page_items();
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
			go_page(PAGE_MAIN);
	}
	if (pressed & SCE_CTRL_START)
		r = RSTV_MENU_CLOSE;
	if (r != RSTV_MENU_STAY)
		rstv_settings_save();
	return r;
}

// ================================ drawing ================================
static void item(UiCanvas *c, int px, int ty, int i, int sel, const char *name, const char *value)
{
	const uint32_t col = (i == sel) ? COL_SEL : COL_TEXT;
	if (i == sel)
		ui_text(c, px + 16, ty, COL_SEL, 1.0f, ">");
	ui_text(c, px + 40, ty, col, 1.0f, name);
	if (value) {
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
static void menu_paint(UiCanvas *c, int overlay)
{
	static const char *const aspect[] = { "4:3", "Full screen", "Integer" };
	static const char *const filter[] = { "Smooth", "Sharp" };
	static const char *const scan[] = { "Off", "CRT soft", "CRT sharp", "Every 2 px", "Every 3 px", "Every 4 px" };
	static const char *const scan_str[] = { "Light", "Medium", "Heavy" };
	static const char *const border[] = { "Black", "Artwork", "Color" };
	static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
	static const char *const stick[] = { "Off", "Left", "Right", "Both" };
	static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };
	const int o_ok = g_set.confirm == RSTV_CONFIRM_CIRCLE;
	const char *ok_name = o_ok ? G_CIR : G_CRO, *back_name = o_ok ? G_CRO : G_CIR;
	char foot_buf[128];
	const int n = page_items();
	const int h = TITLE_H + n * LINE_H + FOOT_H;
	const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
	if (overlay) {
		ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
		s_drawn_h = 0;
	} else if (s_drawn_h == h) {
		// ADR-095: if the panel size is unchanged, redraw only the panel (do not recompose and copy the whole dimmed screen every time)
		ui_clear_rect(c, px, py, PANEL_W, h);
		ui_rect(c, px, py, PANEL_W, h, COL_BG);
	} else {
		ui_clear(c);
		ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
		s_drawn_h = h;
	}
	ui_rect(c, px, py, PANEL_W, h, COL_PANEL);

	const char *title = "OPTIONS";
	snprintf(foot_buf, sizeof(foot_buf), "Up/Down: select   Left/Right/%s: change   %s: back", ok_name, back_name);
	const char *foot = foot_buf;
	switch (s_page) {
	case PAGE_DISPLAY:
		title = "DISPLAY";
		if (s_list_mode)
			foot = "Left/Right: change   (global settings)";
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
		int len = (int)strlen(tbuf);
		while (len > 4 && ui_text_width(1.1f, tbuf) > PANEL_W - 48) {
			len--;
			memcpy(tbuf + len - 3, "...", 4);
		}
		ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, tbuf);
	}

	for (int i = 0; i < n; i++) {
		const int ty = py + TITLE_H + i * LINE_H + 22;
		switch (s_page) {
		case PAGE_MAIN: {
			static const char *const names[M_COUNT] = { "Resume game", "Reset game", "Save / Load state  >", "Display  >",
				"Controls  >", "Cheats  >", "Save game settings", "Delete game settings", "About  >", "Select game", "Exit", "Back" };
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
			static const char *const names[D_COUNT] = { "Show FPS", "Screen", "Filter", "Scanlines", "Scanline strength",
				"Border (4:3 / Integer)", "Border color", "Back" };
			const char *v = NULL;
			switch (i) {
			case D_FPS: v = g_set.fps ? "On" : "Off"; break;
			case D_ASPECT: v = aspect[g_set.aspect]; break;
			case D_FILTER: v = filter[g_set.filter]; break;
			case D_SCAN: v = scan[g_set.scan_type]; break;
			case D_SCAN_STR: v = scan_str[g_set.scan_strength]; break;
			case D_BORDER: v = border[g_set.border]; break;
			case D_BORDER_COL: v = border_col[g_set.border_color]; break;
			}
			item(c, px, ty, i, s_sel, names[i], v);
			break;
		}
		case PAGE_CONTROLS: {
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
	text_keys(c, px + 24, py + h - 18, COL_DIM, foot);
}

void rstv_menu_draw(void)
{
	if (!s_dirty || !s_cv)
		return;
	s_dirty = 0;
	menu_paint(s_cv, 0);
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

void rstv_loading_progress(int permille)
{
	static uint64_t last_t;
	static int last_p = -1000;
	const uint64_t now = rstv_time_us();
	if (permille > 0 && permille < 1000 && permille - last_p < 20 && now - last_t < 100000)
		return; // do not draw too often (each draw waits for vblank)
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	last_p = permille;
	last_t = now;
	ensure_canvases();
	UiCanvas *c = s_cv;
	const int w = 600, h = 18, x = (SCREEN_W - w) / 2, y = SCREEN_H / 2;
	s_drawn_h = 0;
	ui_clear(c);
	ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0, 0, 0, 0xFF));
	ui_text(c, x, y - 20, COL_TEXT, 1.0f, s_loading_title[0] ? s_loading_title : "Loading...");
	ui_rect(c, x - 2, y - 2, w + 4, h + 4, COL_DIM);
	ui_rect(c, x, y, w, h, COL_PANEL);
	ui_rect(c, x, y, w * permille / 1000, h, UI_RGBA(0x38, 0xA8, 0xFF, 0xFF)); // blue, the app art accent
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
	snprintf(buf, sizeof(buf), "%.1f FPS  F%u", (double)fps, frame);
	const int w = ui_text_width(1.0f, buf) + 16;
	const uint32_t col = fps >= 59.0f ? UI_RGBA(0x60, 0xFF, 0x60, 0xFF) : UI_RGBA(0xFF, 0xC0, 0x40, 0xFF);
	ui_clear(c);
	ui_rect(c, 240 - w, 0, w, 30, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
	ui_text(c, 240 - w + 8, 22, col, 1.0f, buf);
	ui_canvas_move(c, SCREEN_W - 4 - 240, 4);
	ui_commit(c);
}

// ================================ start screen game select ================================
// ADR-096·126 (same look and behavior as Reirem rei_game_select): all MAME ST-V and System C/C2 games (stv_list.h). Tab = board, switched with L/R.
// SELECT = filter (Playable: games with ROMs / All: everything, games without ROMs gray, picking one shows a notice / Favorites). □ = favorite on/off (★ mark),
// START = options (Display, Controls, global settings), △ = About. Bright if ROMs exist even when not yet supported (ADR-106, picking shows a notice)
#define COL_GREY     UI_RGBA(0x70, 0x74, 0x84, 0xFF)
#define COL_GREY_SEL UI_RGBA(0xB0, 0x80, 0x50, 0xFF)
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

const char *rstv_game_select(const char *initial_set, const char *rom_dir)
{
	ensure_canvases();
	UiCanvas *c = s_cv;
	int nplay = 0, want = -1;
	for (int i = 0; i < RSTV_LIST_N; i++) {
		char path[256];
		snprintf(path, sizeof(path), "%s/%s.zip", rom_dir, s_stv_list[i].set);
		SceIoStat st;
		s_gl_present[i] = sceIoGetstat(path, &st) >= 0;
		s_gl_supported[i] = STVFindGame(s_stv_list[i].set) != NULL || c2_is_game(s_stv_list[i].set); // ADR-107
		nplay += gl_playable(i);
		if (initial_set && !strcmp(initial_set, s_stv_list[i].set))
			want = i;
	}
	if (!nplay && g_set.list_filter == RSTV_LIST_PLAYABLE)
		g_set.list_filter = RSTV_LIST_ALL; // (show everything if no game can run)
	static int idx[RSTV_LIST_N];
	int sel = 0, top = 0, about = 0, dirty = 1, rep = 0;
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
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t b = pad.buttons;
		if (gstest) {
			static const uint32_t seq[7] = { 0, SCE_CTRL_SQUARE, SCE_CTRL_SELECT, SCE_CTRL_SELECT, SCE_CTRL_START, SCE_CTRL_DOWN,
				SCE_CTRL_START };
			if (gstep > 0 && !dirty) {
				char gp[64];
				snprintf(gp, sizeof(gp), DATA_DIR "/menu_gs%d.ppm", gstep);
				ui_save_ppm(c, gp, UI_RGBA(0, 0, 0, 0xFF));
			}
			static const char *gs_set; // game the test marked favorite (restored at the end)
			if (gstep == 1 && n)
				gs_set = s_stv_list[idx[sel]].set;
			if (gstep >= 7) {
				gstest = 0;
				if (gs_set)
					rstv_fav_toggle(gs_set);
			} else {
				prev = 0;
				b = seq[gstep];
			}
			gstep++;
		}
		if (pad.ly < 128 - 48) b |= SCE_CTRL_UP;
		if (pad.ly > 128 + 48) b |= SCE_CTRL_DOWN;
		if (pad.lx < 128 - 48) b |= SCE_CTRL_LEFT;
		if (pad.lx > 128 + 48) b |= SCE_CTRL_RIGHT;
		const uint32_t pressed = key_repeat(b, b & ~prev, &rep);
		prev = b;
		if (pressed)
			dirty = 1;
		const int o_ok = g_set.confirm == RSTV_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		if (s_list_mode) { // options (Display / Controls, global settings. Saved by rstv_menu_update on close)
			if (rstv_menu_update(b) != RSTV_MENU_STAY) {
				s_list_mode = 0;
				s_page = PAGE_MAIN;
				ui_clear(c);
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
				top = 0;
			} else if (pressed & SCE_CTRL_SELECT) { // filter: Playable → All → Favorites (current game kept if visible)
				g_set.list_filter = wrap(g_set.list_filter + 1, RSTV_LIST_FILTER_COUNT);
				rstv_settings_save();
				n = gl_build(idx, n ? idx[sel] : -1, &sel);
				top = 0;
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
		if (dirty) {
			dirty = 0;
			const int rows = 12, vis = n < rows ? n : rows;
			if (sel < top) top = sel;
			if (sel >= top + vis) top = sel - vis + 1;
			if (top > n - vis) top = n - vis;
			if (top < 0) top = 0;
			const int tab_h = 40;
			const int h = TITLE_H + tab_h + rows * LINE_H + FOOT_H, w = 800;
			const int px = (SCREEN_W - w) / 2, py = (SCREEN_H - h) / 2;
			s_drawn_h = 0;
			if (s_list_mode)
				ui_clear(c); // (including the dim layer options drew)
			else
				ui_clear_rect(c, px - 2, py - 2, w + 4, h + 4);
			ui_rect(c, px, py, w, h, COL_PANEL);
			if (about) {
				ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, "ABOUT");
				for (int i = 0; i < ABOUT_VISIBLE && s_about_scroll + i < ABOUT_LINES; i++) {
					const char *t = s_about[s_about_scroll + i];
					if (!t[0])
						continue;
					const int head = t[0] == '#';
					ui_text(c, px + 24, py + TITLE_H + i * LINE_H + 22, head ? COL_SEL : (t[0] == ' ' ? COL_DIM : COL_TEXT),
						head ? 1.0f : 0.85f, t + head);
				}
				text_keys(c, px + 24, py + h - 18, COL_DIM,
					o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
			} else {
				char title[64];
				snprintf(title, sizeof(title), "ReST-V - SELECT GAME (%d/%d)", n ? sel + 1 : 0, n);
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
						if (t == s_gl_tab)
							ui_rect(c, tx, py + TITLE_H - 4, tw, 32, COL_SEL);
						ui_text(c, tx + 10, py + TITLE_H + 20, t == s_gl_tab ? COL_PANEL : COL_DIM, 0.95f, s_gl_tabs[t]);
						tx += tw + 8;
					}
				}
				if (!n && g_set.list_filter == RSTV_LIST_FAVORITES)
					text_keys(c, px + 48, py + TITLE_H + tab_h + 22, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
				else if (!n)
					ui_text(c, px + 48, py + TITLE_H + tab_h + 22, COL_DIM, 1.0f, "No playable games. SELECT: show all");
				for (int i = top; i < top + vis; i++) {
					const int ty = py + TITLE_H + tab_h + (i - top) * LINE_H + 22;
					const int okg = gl_playable(idx[i]);
					const uint32_t col = (i == sel) ? (okg ? COL_SEL : COL_GREY_SEL) : (okg ? COL_TEXT : COL_GREY);
					if (i == sel)
						ui_text(c, px + 16, ty, col, 1.0f, ">");
					if (rstv_fav_is(s_stv_list[idx[i]].set)) // favorite mark
						ui_text(c, px + 32, ty, COL_WARN, 1.0f, "*");
					ui_text_clip(c, px + 48, ty, col, 1.0f, s_stv_list[idx[i]].title, px + 48, px + w - 24);
					if (i == sel) { // titles longer than the cell scroll (only this row is redrawn each frame below)
						if (mq_sel != idx[i])
							mq_t = 0;
						mq_sel = idx[i];
						mq_y = ty;
						mq_px = px;
						mq_w = w;
						mq_tw = ui_text_width(1.0f, s_stv_list[idx[i]].title);
					}
				}
				if (!n)
					mq_sel = -1;
				char foot[128];
				snprintf(foot, sizeof(foot), "%sLeft/Right: page   %s: play   " G_TRI ": about", s_gl_ntabs > 1 ? "L/R: system   " : "",
					o_ok ? G_CIR : G_CRO);
				text_keys(c, px + 24, py + h - 28, COL_DIM, foot);
				snprintf(foot, sizeof(foot), "START: options   " G_SQU ": favorite   SELECT: %s",
					s_gl_filter_name[wrap(g_set.list_filter + 1, RSTV_LIST_FILTER_COUNT)]);
				text_keys(c, px + 24, py + h - 8, COL_DIM, foot);
				if (msg[0]) { // notice box (over the list)
					const int mw = 720, mh = 150, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
					ui_rect(c, mx - 2, my - 2, mw + 4, mh + 4, COL_WARN);
					ui_rect(c, mx, my, mw, mh, COL_PANEL);
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
				if (s_list_mode) // options (over the list)
					menu_paint(c, 1);
			}
			ui_commit(c);
		} else if (!about && !msg[0] && !s_list_mode && mq_sel >= 0 && mq_tw > mq_w - 72) {
			// scrolling title (ADR-099): 1 s pause → 60 px/s to the end → 1 s pause → from the start
			const int avail = mq_w - 72, over = mq_tw - avail, hold = 60;
			const int cyc = hold + over + hold;
			const int t = mq_t++ % cyc;
			const int off = t < hold ? 0 : t < hold + over ? t - hold : over;
			const int x0 = mq_px + 48, x1 = mq_px + mq_w - 24;
			ui_clear_rect(c, x0, mq_y - 22, x1 - x0, LINE_H - 2);
			ui_rect(c, x0, mq_y - 22, x1 - x0, LINE_H - 2, COL_PANEL);
			ui_text_clip(c, x0 - off, mq_y, gl_playable(mq_sel) ? COL_SEL : COL_GREY_SEL, 1.0f, s_stv_list[mq_sel].title, x0, x1);
			ui_commit(c);
		}
		rstv_video_present();
	}
}
