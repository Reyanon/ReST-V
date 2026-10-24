/*  ReST-V: capture of VDP2 scroll-layer parameters for the GPU layer renderer (ADR-011, stage 2).
    vidsoft's Vdp2DrawScroll fills this instead of drawing pixels when rstv_capture_mode is set.
    GPL-2.0-or-later. */
#ifndef RSTV_CAPTURE_H
#define RSTV_CAPTURE_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSTV_CAP_ROWS 256

typedef struct {
   u8 enable;      /* 0 = row empty */
   u8 priority;
   u16 x;          /* info->x */
   u16 lsx;        /* line scroll x (0 = none) */
   u16 y;          /* Y after line/vertical scroll and ymask */
   u32 planetbl[4];
   float zx;       /* horizontal coordinate increment of this line (zoom / line zoom, ADR-037) */
   u8 nwin;        /* ADR-040 windows: number of visible runs [wx0, wx1] on this line (0 = the whole line) */
   u16 wx0[2], wx1[2];
} RstvCapRow;

typedef struct {
   int captured;   /* Vdp2DrawScroll ran in capture mode for this layer this frame */
   int ok;         /* every row is supported by the GPU layer shader */
   int why;        /* !ok reasons: 1 bitmap, 2 colornumber, 4 cellw, 8 mapwh, 16 window/zoom/bad cycle/per-pixel prio/cc window, 32 too many rows */
   /* static layer configuration (vdp2draw_struct / screeninfo_struct) */
   int colornumber, transparencyenable, cellw;
   int patterndatasize, patternwh, auxmode, supplementdata, specialprimode;
   int pagewh_bits, planew_bits, patterndatasize_bits, mapwh;
   int planepixelwidth_bits, planepixelheight_bits, xmask, ymask;
   int charaddr_mask_3fff; /* !(VRSIZE & 0x8000) */
   int coloroffset, alpha, specialcolormode, specialcode;
   int has_win;    /* some row has window runs (ADR-040) */
   int isbitmap, bm_charaddr, bm_paladdr, bm_scf; /* bitmap layer (ADR-039): VRAM address, palette, special colour function */
   float zx;       /* != 1 when some line is zoomed (the cell-quad path cannot draw it; pixel path uses row zx) */
   int mosx;       /* horizontal mosaic size (> 1 = on): drawn without it, the GPU compose snaps x (ADR-129) */
   int swin;       /* sprite window alone (ADR-161): 1 = hidden where the window bit is set, 2 = where it is clear */
   int nrows;
   RstvCapRow row[RSTV_CAP_ROWS];
} RstvLayerCapture;

extern int rstv_capture_mode;
extern RstvLayerCapture rstv_layer_capture[6]; /* Titan layer index */

/* RBG0 rotation capture (GPU rotation renderer, ADR-016). Per line the map position of screen pixel i is
   (x0 + i*dx, y0 + i*dy) before integer truncation; line coefficients with the MSB set make the line transparent. */
typedef struct {
   float x0, dx, y0, dy;
   u8 enable;
   /* ADR-037 RPMD 3: the rotation parameter window splits the line into nseg runs [sx, ex] using parameter A (sp 0)
      or B (sp 1, coordinates xb0..). nseg 0 = the whole line uses A */
   u8 nseg, enb;
   u8 sp[4];
   u16 sx[4], ex[4];
   float xb0, dxb, yb0, dyb;
} RstvRotLine;

typedef struct {
   int captured;   /* RBG0 was recorded this frame (not drawn by the CPU) */
   int colornumber, transparencyenable, patterndatasize, patternwh, auxmode, supplementdata;
   int specialprimode, priority, charaddr_mask_3fff, coloroffset, alpha, specialcolormode, specialcode;
   int planew, planeh;        /* plane size in pages (1 or 2) */
   int mapw, maph;            /* map size in pixels (xmask + 1, ymask + 1) */
   int screenover;            /* 0/1 = repeat, 2 = transparent outside */
   /* bitmap RBG0 (ADR-037): the map is a bmw x bmh bitmap at charaddr (VRAM bytes), palette paladdr, special colour scf */
   int isbitmap, charaddr, paladdr, scf;
   u32 planetbl[16];
   /* ADR-046: parameter B uses another map (same size): its plane table; runs with sp 1 / B lines use it */
   int two_maps;
   int screenover_b; /* parameter B screen-over mode (ADR-071: drawn as a separate B list when it differs) */
   u32 planetbl_b[16];
   int nrows;
   RstvRotLine row[RSTV_CAP_ROWS];
} RstvRotCapture;

extern RstvRotCapture rstv_rot_capture;
/* last RBG0 capture refusal: 1 bitmap, 2 RPMD>=2, 4 window, 8 line colour/coef, 16 per-dot prio, 32 hires,
   64 screenover 3, 128 colour mode, 256 per-dot coef, 512 coef mode 3, 1024 no rotation */
extern int rstv_rot_why;

/* back/line screen drawing into Titan (VIDSoftVdp2DrawStart); the GPU compositor sets rstv_skip_back_line and
   calls rstv_vidsoft_draw_back_line() itself for CPU-composed frames */
extern int rstv_skip_back_line;
void rstv_vidsoft_draw_back_line(void);

/* Draw one Titan background layer (0..4) on the CPU with the current registers (capture off) */
void rstv_vidsoft_draw_layer(int titan_layer);

#ifdef __cplusplus
}
#endif

#endif
