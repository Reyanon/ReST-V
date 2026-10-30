#ifndef RSTV_VIDEO_H
#define RSTV_VIDEO_H

#include <stdint.h>

// Output layer (ADR-003, PLAN.md 5-1). CPU writes directly into GPU textures (sceGxm), shader scales them up.
// 3 textures in rotation. Pixel format RGBA8888 (memory order R,G,B,A -> uint32 little-endian ABGR)
#define RSTV_VIDEO_TEX_W 1024 // for hi-res mode (704)
#define RSTV_VIDEO_TEX_H 512
#define RSTV_VIDEO_PITCH RSTV_VIDEO_TEX_W

void rstv_video_init(void);
void rstv_video_shutdown(void);
void rstv_video_game_end(void);
void rstv_video_preload(void); // the loading screen's background and the blank artwork, decoded on a thread at start
void rstv_video_picture_rect(int *x, int *y, int *w, int *h); // the game picture on the screen (last present) // (back to the game list in the same process, ADR-211)

// pixel pointer of the next texture to draw. w x h is this frame's game screen size
uint32_t *rstv_video_frame_begin(int w, int h);
void rstv_video_frame_end(void);

// make the GPU-drawn frame (top-left w x h of texture tex, texture size tw x th) the next display target
struct GxTex;
void rstv_video_frame_gpu(struct GxTex *tex, int w, int h);

// draw the last completed frame to the screen and flip on vsync (main thread only)
void rstv_video_present(void);

// text at top-left of screen (uppercase, digits, some symbols, max 42 chars). NULL turns it off
void rstv_video_set_osd(const char *text);

// text list at screen center (n lines, max 16 lines x 42 chars, line sel highlighted). lines NULL turns it off
void rstv_video_menu(const char *const *lines, int n, int sel);

// overlay layer (ADR-094 menu/notice/FPS): in layer slot (0..3, higher is on top), top-left w x h of texture t at screen (x, y) 1:1,
// alpha blended. t NULL removes that layer
void rstv_video_layer(int slot, struct GxTex *t, int x, int y, int w, int h);

// strips drawn right over slot 0 (the game list rows: drawn once each, moved by the GPU while the list scrolls; ReiREM's
// way): the top-left w x h of texture t at (x, y), cut to rows y0..y1 of the screen. n 0 removes them
#define RSTV_NSTRIP 24
typedef struct { const struct GxTex *t; int x, y, w, h; } RstvStrip;
void rstv_video_strips(const RstvStrip *s, int n, int y0, int y1);

// screen size: 0 = 4:3 (fit height), 1 = integer scale (pixel exact), 2 = full screen. filter: 0 = sharp (point), 1 = smooth (bilinear)
void rstv_video_set_mode(int scale, int filter);
// ADR-190: the game picture turned clockwise by 0, 90, 180 or 270 degrees
void rstv_video_set_rotate(int deg);
// layer slot turned clockwise by rot * 90 degrees (drawn at x, y as an h x w rectangle when rot is odd)
void rstv_video_layer_rot(int slot, int rot);
// layer slot opacity 0..1 (1 = as drawn)
void rstv_video_layer_alpha(int slot, float a);
// the app's LiveArea background as RGBA (malloc'd, NULL if missing)
uint8_t *rstv_video_load_bg(int *w, int *h);

// ADR-126 (Reirem Display): scanline type 0~5 (off, CRT smooth, CRT sharp, every 2/3/4 pixels), strength 0~2,
// border 0 = black, 1 = artwork (solid if missing), 2 = solid, solid color index 0~5
void rstv_video_set_style(int scan_type, int scan_strength, int border, int border_color);
// border artwork: ux0:data/restv/artwork/<set>.png (.jpg, .bmp), else parent. set NULL removes it
void rstv_video_set_art(const char *set, const char *parent);
void rstv_video_art_retint(void); // (the blank artwork follows a theme change, ADR-210)

#endif
