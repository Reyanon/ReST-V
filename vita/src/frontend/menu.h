// In-game menu, start screen game select, notices, FPS display (ADR-094, ADR-126). Same look, controls and features as the Reirem (sister emulator) menu.
#ifndef RSTV_MENU_H
#define RSTV_MENU_H

#include <stdint.h>

// ---- settings (ux0:data/restv/settings.cfg, per-game <set>_game.cfg, favorites favorites.cfg; same key=value as Reirem) ----
// Vita buttons that can be picked (cycled with left/right on the options screen). RSTV_KEY_NONE = none
enum {
	RSTV_KEY_CROSS, RSTV_KEY_CIRCLE, RSTV_KEY_SQUARE, RSTV_KEY_TRIANGLE,
	RSTV_KEY_L, RSTV_KEY_R, RSTV_KEY_SELECT, RSTV_KEY_START,
	RSTV_KEY_COUNT
};
#define RSTV_KEY_NONE RSTV_KEY_COUNT
// game inputs to map: buttons 1-4, coin, start
enum { RSTV_ACT_B1, RSTV_ACT_B2, RSTV_ACT_B3, RSTV_ACT_B4, RSTV_ACT_COIN, RSTV_ACT_START, RSTV_ACT_COUNT };
#define RSTV_GAME_BUTTONS 4
enum { RSTV_STICK_OFF, RSTV_STICK_LEFT, RSTV_STICK_RIGHT, RSTV_STICK_BOTH, RSTV_STICK_COUNT };
// screen (Reirem order): 4:3, full, integer
enum { RSTV_ASPECT_4_3, RSTV_ASPECT_FULL, RSTV_ASPECT_INT, RSTV_ASPECT_COUNT };
enum { RSTV_FILTER_SMOOTH, RSTV_FILTER_SHARP, RSTV_FILTER_COUNT };
// scanlines: CRT types follow game lines, PX types use screen pixel spacing
enum { RSTV_SCAN_OFF, RSTV_SCAN_CRT_SOFT, RSTV_SCAN_CRT_SHARP, RSTV_SCAN_PX2, RSTV_SCAN_PX3, RSTV_SCAN_PX4, RSTV_SCAN_COUNT };
enum { RSTV_SCAN_LIGHT, RSTV_SCAN_MEDIUM, RSTV_SCAN_HEAVY, RSTV_SCAN_STRENGTH_COUNT };
// outside the game screen (4:3 / integer): black, artwork (ux0:data/restv/artwork/<set>.png|jpg|bmp), solid color
enum { RSTV_BORDER_BLACK, RSTV_BORDER_ARTWORK, RSTV_BORDER_COLOR, RSTV_BORDER_COUNT };
// screen rotation (ADR-190): Auto = vertical games upright on the landscape screen; Vertical L / R = upright with the Vita
// turned left (right edge up) / right (left edge up), directions turned with it
enum { RSTV_ROT_AUTO, RSTV_ROT_OFF, RSTV_ROT_VL, RSTV_ROT_VR, RSTV_ROT_COUNT };
enum { RSTV_BCOL_NAVY, RSTV_BCOL_GRAY, RSTV_BCOL_MAROON, RSTV_BCOL_GREEN, RSTV_BCOL_PURPLE, RSTV_BCOL_BRICK, RSTV_BCOL_COUNT };
// menu confirm button (the other one is back)
enum { RSTV_CONFIRM_CROSS, RSTV_CONFIRM_CIRCLE, RSTV_CONFIRM_COUNT };
// game list filter (SELECT cycles in this order)
enum { RSTV_LIST_PLAYABLE, RSTV_LIST_ALL, RSTV_LIST_FAVORITES, RSTV_LIST_FILTER_COUNT };
enum { RSTV_TURBO_FAST, RSTV_TURBO_MEDIUM, RSTV_TURBO_SLOW, RSTV_TURBO_SPEED_COUNT };

typedef struct {
	int fps;           // FPS display
	int aspect;        // RSTV_ASPECT_*
	int filter;        // RSTV_FILTER_*
	int scan_type;     // RSTV_SCAN_*
	int scan_strength; // RSTV_SCAN_LIGHT..HEAVY
	int border;        // RSTV_BORDER_*
	int border_color;  // RSTV_BCOL_*
	int rotate;        // RSTV_ROT_*
	int keymap[RSTV_ACT_COUNT]; // RSTV_KEY_* (buttons 3 and 4 may be RSTV_KEY_NONE)
	int stick;         // RSTV_STICK_* (ReST-V: analog stick used as lever)
	int turbo_key;     // Vita button that triggers turbo, RSTV_KEY_NONE = off
	int turbo_btn;     // game button to turbo, 1..4
	int turbo_speed;   // RSTV_TURBO_*
	int confirm;       // RSTV_CONFIRM_*
	int list_filter;   // RSTV_LIST_*
} rstv_settings;
extern rstv_settings g_set;
// ADR-190: game direction = screen direction turned back by this (0, 90, 270 degrees clockwise); set by apply_video
extern int rstv_input_rot;

// frames per turbo period (press + release): Fast 4, Medium 6, Slow 8 (same as Reirem)
static inline int rstv_turbo_period(int speed) { return speed == 0 ? 4 : speed == 1 ? 6 : 8; }
void rstv_settings_defaults(void);
void rstv_settings_default_keys(void);
void rstv_settings_load(void);
void rstv_settings_save(void);
uint32_t rstv_key_code(int key);       // RSTV_KEY_* -> SCE_CTRL_* (0 for RSTV_KEY_NONE)
const char *rstv_key_name(int key);    // "Cross" … "None"
// per-game settings (<set>_game.cfg: screen, filter, scanlines, border, FPS, buttons, stick, turbo). If present, rstv_settings_load_game overrides global.
// While active, rstv_settings_save writes those items only to the game file and keeps global values in the global file
void rstv_settings_load_game(const char *set);
int rstv_settings_has_game(void);
int rstv_settings_save_game(void);     // current values as this game's settings (1 on success)
int rstv_settings_delete_game(void);   // delete game settings and fall back to global (1 on success)
void rstv_settings_use_global(void);   // game list options: drop game settings and edit global values only
// favorites (favorites.cfg, one set name per line)
int rstv_fav_is(const char *set);
void rstv_fav_toggle(const char *set);
// apply current settings to video output (screen, filter, scanlines, border)
void rstv_settings_apply_video(void);

typedef enum {
	RSTV_MENU_STAY,
	RSTV_MENU_CLOSE,
	RSTV_MENU_RESET,
	RSTV_MENU_SAVE,        // save to current slot (menu stays open)
	RSTV_MENU_LOAD,        // load current slot, then close
	RSTV_MENU_SELECT_GAME, // relaunch the app to game select
	RSTV_MENU_REFRESH,     // game list options: read the ROM folder again
	RSTV_MENU_EXIT,
	RSTV_MENU_TEST,        // close and press the board's TEST switch (ST-V BIOS test menu)
} rstv_menu_result;

void rstv_menu_init(const char *set, const char *title, int buttons); // after the game is known (buttons = number of game buttons)
int rstv_menu_buttons(void); // button count of the current game (2-4)
void rstv_kctrl_init(int restarted); // kernel helper for the PS button (may restart the app once after boot)
int rstv_menu_should_open(uint32_t buttons);              // 1 at the moment SELECT+START or PS is pressed
rstv_menu_result rstv_menu_update(uint32_t buttons);
void rstv_menu_draw(void);  // redraws only when changed
void rstv_menu_hide(void);
int rstv_menu_slot(void);
void rstv_state_slot_path(char *buf, int len, int slot); // slot -1 = quick (state/<set>.yss)

// loading screen (same look as Reirem rei_loading_progress): game name + bar (0-1000)
void rstv_loading_title(const char *title);
void rstv_loading_tick(void); // redraw the loading screen at its current value (ADR-193)
void rstv_loading_progress(int permille);
void rstv_loading_done(void);

void rstv_osd_show(const char *fmt, ...);
void rstv_osd_tick(void);                   // every frame (hides after 2 s)
void rstv_fps_update(float fps, unsigned frame); // every frame (display refreshed every 0.1 s)

// test (ux0:data/restv/menutest): draw each menu page, notice and FPS box to ux0:data/restv/menu_<name>.ppm
void rstv_menu_selftest(void);

// start screen game select (ADR-096·126, Reirem list): board tabs, SELECT filter (Playable/All/Favorites), □ favorite, START options,
// △ About. Returns the picked set name
const char *rstv_game_select(const char *initial_set, const char *rom_dir);

#endif
