#ifndef RSTV_GX_H
#define RSTV_GX_H

// Direct sceGxm backend (ADR-015). Thin wrapper: textures, render targets, programs, uniforms, draw, display.
// - Shaders: Cg source compiled with SceShaccCg (vitashark), GXP cached in ux0:data/restv/shaders/.
// - 3 display buffers + display queue. CPU runs at most 2 frames ahead of GPU (frame fence).
// - All drawing happens on the main thread only.

#include <stdint.h>
#include <psp2/gxm.h>

typedef struct GxTex {
	SceGxmTexture tex;
	uint8_t *data;     // memory written directly by CPU (LPDDR, uncached)
	int w, h, stride;  // stride: bytes
	int bpp;
	int rt;            // also used as render target?
	SceGxmColorSurface surf;
	SceGxmRenderTarget *target; // dedicated render target (render targets wider than 256 shared between scenes lose the earlier scene's result)
} GxTex;

typedef struct GxAttr {
	const char *name;
	SceGxmAttributeFormat fmt;
	int count;
	int offset;
} GxAttr;

typedef struct GxProg {
	const SceGxmProgram *vgxp, *fgxp;
	SceGxmVertexProgram *vp;
	SceGxmFragmentProgram *fp;
	int vbuf, fbuf; // default uniform buffer in use
} GxProg;

// uniform: also remembers which program (vertex/fragment) the parameter belongs to
typedef struct GxParam {
	const SceGxmProgramParameter *p;
	int vertex;
	unsigned index; // texture unit if sampler
} GxParam;

enum { GX_RGBA8, GX_L8 };
#define GX_CDRAM 0x100 // add to fmt to place the texture in CDRAM (video memory)
enum { GX_BLEND_NONE, GX_BLEND_ALPHA, GX_BLEND_PREMUL }; // PREMUL: premultiplied alpha (src + dst * (1 - a))

int gx_init(int vsync);
void gx_shutdown(void);

// texture (w rounded to a multiple of 8). linear: bilinear filter, rt: nonzero = also a render target (value = render target slot,
// textures in the same slot share a render target -> only 1 scene per slot per frame)
GxTex *gx_tex(int w, int h, int fmt, int linear, int rt);
void gx_tex_filter(GxTex *t, int linear);
unsigned gx_gpu_wait_us(void); // diag: accumulated GPU wait (us) // point (0) or bilinear (1) sampling
// horizontal repeat addressing (palette etc.)
void gx_tex_repeat_u(GxTex *t);

// program. attrs is a single stream 0 (stride bytes). NULL on failure
GxProg *gx_prog(const char *vs_src, const char *fs_src, const GxAttr *attrs, int nattr, int stride, int blend);
GxParam gx_param(const GxProg *p, const char *name);
// Cg source -> GXP binary (uses disk cache, compiles if missing). Does not touch the shader patcher, so safe from other threads
const void *gx_compile(const char *src, int fragment);
// program from an existing binary (main thread)
GxProg *gx_prog_bin(const void *vgxp, const void *fgxp, const GxAttr *attrs, int nattr, int stride, int blend);

// frame: begin (fence wait) -> scenes -> display
void gx_frame_begin(void);
void gx_begin(GxTex *target);  // NULL = back buffer of the screen (960x544)
void gx_end(void);
void gx_swap(void);
void gx_finish(void);
// serialize per scene when main and worker threads share GXM (ADR-029)
void gx_lock(void);
void gx_unlock(void);

// draw state
void gx_use(const GxProg *p);  // set program + reserve default uniform buffer (call again for every draw)
void gx_set4(GxParam u, const float *v);
void gx_set_tex(GxParam sampler, const GxTex *t);
void *gx_temp(int bytes);      // temp memory the GPU reads during this frame (vertices, indices)
void *gx_static(int bytes);    // persistent GPU memory (fixed indices etc.)
void gx_draw(SceGxmPrimitiveType prim, const void *vertices, const uint16_t *indices, int count);
// sequential indices 0..count-1 (fixed buffer that can be passed to gx_draw)
const uint16_t *gx_seq_indices(int count);

int gx_frame_number(void);

#endif
