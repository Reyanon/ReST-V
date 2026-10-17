// VDP1 GPU renderer (ADR-023): turns VDP1 commands recorded by the core (rstv_vdp1rec.h) into triangles.
// CPU decodes sprite patterns once into a texture cache (atlas 1024x1024 RGBA8: r = value low byte, g = high, a = written),
// shader reads the atlas once and writes 16-bit framebuffer values (r, g) to a 512x256 render target.
#ifndef RSTV_V1GPU_H
#define RSTV_V1GPU_H

#include <stdint.h>

#include "rstv_vdp1rec.h"

#define V1GPU_VERTEX_FLOATS 12 // pos 2, uv 2, clip 4, gouraud rgb 3, 0
#define V1GPU_MAX_VERTS 20000
#define V1GPU_ATLAS 1024
#define V1GPU_MAX_BATCH 1024

// shader variant bits: 1 mesh, 2 gouraud, 4 solid (polygon/clear), 8/16 color calc (ADR-115: 1 shadow, 2 half-luminance, 3 half-transparent),
// 32 MSB on (ADR-118: only with mesh/solid, no gouraud or color calc)
#define V1GPU_VARIANTS 64
#define V1GPU_CC(variant) (((variant) >> 3) & 3)
#define V1GPU_MSB(variant) (((variant) >> 5) & 1)
#define V1GPU_MSB_BAD(variant) (V1GPU_MSB(variant) && ((variant) & 0x1A)) // unused combination
// variants that read the framebuffer copy (tSnap): shadow/half-transparent, MSB on
#define V1GPU_READS_FB(variant) (V1GPU_CC(variant) == 1 || V1GPU_CC(variant) == 3 || V1GPU_MSB(variant))

typedef struct {
	int variant, first, count;
} V1Batch;

typedef struct {
	int nv, nb;
	V1Batch batch[V1GPU_MAX_BATCH];
	unsigned decoded_px, cache_resets;
} V1Frame;

// items -> vertices (out, max_verts) + batches. atlas is GPU atlas memory (RGBA8, width V1GPU_ATLAS), vram is VDP1 RAM,
// page_stamp/serial are VDP1 RAM write stamps (re-decode when a pattern changes). -1 on failure
int v1gpu_build(const RstvV1Item *items, int n, float *out, int max_verts, uint8_t *atlas, const uint8_t *vram,
	const uint32_t *page_stamp, uint32_t serial, V1Frame *f);

// ADR-100: list format (rstv_v1list_fmt): 1 = draw x * 0.5, 2 = y * 0.5, 4 = 8-bit pixels (value & 0xFF). Clears use buffer coords as is
extern int v1gpu_fmt;

// 1 = reduce the grid of small distorted sprites (ADR-090)
extern int v1gpu_grid_adapt;

// shader source per variant
const char *v1gpu_vs(void);
const char *v1gpu_fs(int variant);

#endif
