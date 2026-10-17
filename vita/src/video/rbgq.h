#ifndef RSTV_RBGQ_H
#define RSTV_RBGQ_H

// CPU side of the RBG0 GPU rotation renderer (ADR-016). Shared by Vita (gvdp2.c) and the PC harness (nbgcheck.c).
// - Pattern names decoded per map page (64x64 cells = 512x512 pixels) into the "page atlas" (RGBA 512x512 = 8x8 pages).
//   cell attr RGBA: R = char unit low 8 bits, G = unit high 6 bits | hflip << 6 | vflip << 7,
//                 B = palette base low 8 bits, A = palette high 3 bits | priority << 3 | scf << 6 | valid << 7
// - page table (RGBA 8x8): map page slot -> R = page atlas index, G = 255 (present)
// - character atlas: 4bpp/8bpp use nbgq's, RGB555 uses rbgq_atlas_rgb_update (RGBA 1024x1024, 8x8 per unit)

#include <stdint.h>

#include "rstv_capture.h"

#define RBGQ_PAGES 64

typedef struct {
	uint32_t addr[RBGQ_PAGES];   // page pattern name base address
	uint32_t key[RBGQ_PAGES];    // decode config key
	uint32_t serial[RBGQ_PAGES]; // decode time (compared against VRAM write stamps)
	uint32_t used[RBGQ_PAGES];   // frame last used
	int valid[RBGQ_PAGES];
	unsigned decoded;            // stats: pages re-decoded
	// ADR-057: decode count (for generation), palette banks used per slot, bank union of pages used by the last rbgq_prepare (128 bits of 16-color banks)
	unsigned changed;
	uint32_t pbank[RBGQ_PAGES][4];
	uint32_t bank_union[4];
	// ADR-067 plane layout: pages are decoded into a CPU cache (cpu_pages, slot layout), and only changed ones are copied to the GPU texture in map order (map page position (px, py) ->
	// 64x64 cells at (px * 64, py * 64)). Shader reads by cell coords directly, no page table
	int layout;                  // current GPU texture layout: 0 = slots (page table), 1 = map order
	uint8_t *cpu_pages;          // 512x512 RGBA (page slots for plane mode)
	unsigned slot_gen[RBGQ_PAGES];
	int flat_slot[64];           // slot copied for each map position (-1 = none)
	unsigned flat_gen[64];
} RbgqState;

// update page table and page atlas for this frame. 0 = success, -1 = too many pages (draw on CPU)
//   page_stamp/serial: VRAM 4KB page write stamps (vdp2.cpp). 0 = no stamps, decode every time
// flat = 1: fill pages in map-order cell layout (table unused; do not use for two maps (two_maps))
int rbgq_prepare(RbgqState *st, const RstvRotCapture *c, const uint8_t *vram, const uint32_t *page_stamp, uint32_t serial,
	uint32_t frame, uint8_t *pages, uint8_t *table, int flat);

// RGB555 character atlas (RGBA, 8x8 per unit, a = MSB): only units touching 4KB pages with dirty[p] (NULL = all)
void rbgq_atlas_rgb_update(uint8_t *argb, int pitch_px, const uint8_t *vram, const uint8_t *dirty);

// same math as the shader (for harness comparison). Layer texture format: rgb | (priority | ratio used << 3) << 24, transparent 0
//   flat = 1: pages is map-order cell layout (table ignored)
//   table_b: B map page table for two maps (B range is 0 if NULL)
uint32_t rbgq_shade(const RstvRotCapture *c, const uint8_t *pages, const uint8_t *table, const uint8_t *table_b, const uint8_t *a4,
	const uint8_t *a8, const uint8_t *argb, const uint8_t (*pal)[4], int i, int j, int flat);

#endif
