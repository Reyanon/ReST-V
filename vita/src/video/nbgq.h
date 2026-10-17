#ifndef RSTV_NBGQ_H
#define RSTV_NBGQ_H

// NBG cell quads (ADR-014). Builds a quad per visible 8x8 cell from the line values left by vidsoft capture mode (rstv_capture.h).
// Character data is read from the character atlas (an 8x8 block per 32-byte VRAM unit, two 1024x1024 L8 textures).
// Shared by Vita (gvdp2.c) and the PC harness (nbgcheck.c, checks against vidsoft with software rasterization).

#include <stdint.h>

#include "rstv_capture.h"

#define NBGQ_ATLAS 1024 // atlas side (texels)

typedef struct {
	int16_t x0, y0, x1, y1; // screen rect [x0,x1) x [y0,y1)
	int16_t u0, v0, u1, v1; // atlas texel coords, corner (x0,y0) -> corner (x1,y1). u1 < u0 if flipped
	uint16_t palbase;       // CRAM color index base (color offset + palette, 0..2047)
	uint8_t prio;           // priority (not built if 0)
	uint8_t scf;            // special color calc bit (pattern name)
} NbgQuad;

// can this layer be drawn as cell quads (4bpp/8bpp palette color only)
int nbgq_supported(const RstvLayerCapture *c);
#define NBGQ_NO_WINDOW 1 // nbgq_build flags: ignore the window runs (the caller clears outside them, ADR-052)
#define NBGQ_SKIP_ZOOM 2 // skip zoomed lines (zx != 1: the caller draws them with the pixel shader, ADR-052)
#define NBGQ_ZOOM_RUNS 4 // magnified lines (0 <= zx < 0.5) as runs of one texel each (ADR-052)
#define NBGQ_RUN_ZX(zx) ((zx) >= 0.0f && (zx) < 0.5f)

// fills out with quads. Returns count, -1 if over max (caller draws with the pixel shader)
int nbgq_build(const RstvLayerCapture *c, int w, int h, const uint8_t *vram, NbgQuad *out, int max, int flags);
// ADR-058: nbgq_track != 0 -> nbgq_build marks the VRAM 4KB pages it reads (pattern names and characters)
extern int nbgq_track;
extern uint32_t nbgq_track_pages[4];

// character atlas update: if VRAM 4KB page p changed, re-decode the blocks touching that page.
//   a4: 4bpp decode (block = 32 bytes -> 8x8 nibbles), a8: 8bpp (block = 64 bytes from that unit)
//   only pages where dirty(p) is nonzero (all if NULL)
void nbgq_atlas_update(uint8_t *a4, uint8_t *a8, int pitch, const uint8_t *vram, const uint8_t *dirty);

#endif
