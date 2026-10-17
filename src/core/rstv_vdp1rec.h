/*  ReST-V: VDP1 command recording for the GPU sprite renderer (ADR-023).
    VIDSoftVdp1DrawStart runs Vdp1DrawCommands on a register copy with rstv_v1rec set; drawQuad then records the
    quad instead of drawing it. If any command needs the CPU (colour calculation modes that read the framebuffer,
    MSB on, lines, 8-bit framebuffer, ...), rstv_v1unsup is set and the frame is drawn by the CPU as before. */
#ifndef RSTV_VDP1REC_H
#define RSTV_VDP1REC_H

#include "core.h"

#define RSTV_V1_MAX 4096

enum { RSTV_V1_TEX, RSTV_V1_POLY, RSTV_V1_ERASE, RSTV_V1_LINE }; /* LINE: x/y[0..1] end points, grd[0..1] (ADR-041) */

typedef struct {
	u8 kind;
	u8 axis;           /* axis-aligned (normal/scaled sprite or rectangle) */
	s16 x[4], y[4];    /* framebuffer coordinates: 0 tl, 1 tr, 2 bl, 3 br (VDP1 vertex order A, B, D, C) */
	u16 pmod, colr, srca, size, ctrl;
	s16 clip[4];       /* inclusive clip rectangle x1, y1, x2, y2 (system clip, intersected with user clip) */
	u16 grd[4];        /* gouraud colours at tl, tr, bl, br (RGB555) */
} RstvV1Item;

extern int rstv_v1rec;      /* recording pass is running */
extern int rstv_v1unsup;    /* recording found a command the GPU renderer does not support */
extern int rstv_v1n;
extern RstvV1Item rstv_v1list[RSTV_V1_MAX];

/* GPU renderer state shared with vita/src/video/gvdp2.c */
extern int rstv_vdp1_gpu_on;       /* frontend: GPU VDP1 renderer available */
extern int rstv_v1ready;           /* a recorded list waits for gvdp2 (target buffer below) */
extern int rstv_v1ready_target;
extern int rstv_vdp1_back_idx;     /* which of the two framebuffers is the back (draw) buffer */
extern int rstv_vdp1_gpu_drawn[2]; /* buffer was last drawn by the GPU renderer */
extern unsigned rstv_v1_gpu_frames, rstv_v1_cpu_frames, rstv_v1_why; /* statistics, last fallback reason */
/* ADR-100: format of the recorded list: bit 0 = 8-bit 1024-wide framebuffer read at half width (draw x * 0.5),
   bit 1 = double interlace drawn at half height (draw y * 0.5), bit 2 = 8-bit pixels (values & 0xFF) */
extern int rstv_v1list_fmt;
/* ADR-115: frontend draws shadow / half luminance / half transparency (CMDPMOD & 7 = 1..3) on the GPU; at most
   RSTV_V1_CC_MAX such commands a frame (rstv_v1_ccn counts them while recording) */
#define RSTV_V1_CC_MAX 8
extern int rstv_v1_cc_ok, rstv_v1_ccn;

#endif
