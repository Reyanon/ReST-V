// GPU VDP2 composite (ADR-011, stage 1). Layer decode on CPU (vidsoft -> Titan buffers), composite on GPU.
//
// frame flow (main thread):
//   VBlankOUT  Vdp2DrawScreens -> gvdp2_draw_screens: decide support, vidsoft draws NBG/RBG layers into the Titan buffers
//              (without color offset), then layers, palette, sprite framebuffer and per-line values go into the texture set.
//   VBlankIN   Vdp2DrawEnd -> gvdp2_draw_end: composite shader draws into the output texture and requests display.
// texture sets rotate in 3 (CPU writes the next set while the GPU reads the previous frame).
#include "video/gvdp2.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>


#include "core.h"
#include "rstv_capture.h"
#include "rstv_prof.h"
#include "titan/titan.h"
#include "vdp1.h"
#include "vdp2.h"
#include "vidshared.h"
#include "vidsoft.h"
#include "yui.h"
#include "stv.h"

#include "platform/log.h"
#include "video/video.h"
#include "video/gx.h"
#include "video/nbgq.h"
#include "video/nbgq_shaders.h"
#include "video/rbgq.h"
#include "video/rbgq_shaders.h"
#include "video/v1gpu.h"

// vidsoft.c (ReST-V additions)
extern int rstv_gpu_compose;
extern int vdp2width, vdp2height, vdp2_x_hires, vdp2_interlace;
extern u32 rstv_vdp2_serial, rstv_vdp2_page_stamp[128];
extern u32 rstv_cram_serial;
void rstv_vidsoft_vdp1_info(int *width, int *height, int *pixelsize, u8 **front_framebuffer);
void VIDSoftVdp2DrawScreens(void);
// ADR-079: the layer pass on the render worker with the snapshot as input
void rstv_vidsoft_set_inputs(Vdp2 *lines, Vdp2 *regs, u8 *ram, u8 *cram, struct CellScrollData *cells);
void rstv_vidsoft_screens_begin(void);
void rstv_vidsoft_screens_layers(void);
void VIDSoftVdp2DrawEnd(void);
void VIDSoftVdp1SwapFrameBuffer(void);
void VidsoftDrawSprite(Vdp2 *vdp2_regs, u8 *spr_window_mask, u8 *vdp1_front_framebuffer, u8 *vdp2_ram, Vdp1 *vdp1_regs,
	Vdp2 *vdp2_lines, u8 *color_ram);
extern u8 sprite_window_mask[704 * 512];

#define NSETS 3
#define LAYER_W 512
#define LAYER_H 256
#define LINE_W 16
#define NUM_BG 5 // Titan order: NBG3=0, NBG2=1, NBG1=2, NBG0=3, RBG0=4

typedef struct {
	GxTex *layer[NUM_BG]; // RGBA8: rgb = color before offset, a = priority | (color calc ratio used << 3)
	GxTex *spr;           // 256x256 RGBA8: VDP1 framebuffer 512x256 u16 raw
	GxTex *pal;           // 2048x1 RGBA8: converted CRAM colors, a = MSB
	GxTex *line;          // 16x256 RGBA8: per-line values
	uint8_t *layer_px[NUM_BG], *spr_px, *pal_px, *line_px;
	// frame constants (composite shader uniforms)
	float layerOn[4], layerAlpha[4], layerShad[4], layerLs[4];
	float mosaic[4];   // ADR-129: horizontal mosaic size per NBG (N3..N0), 1 = off (runtime value)
	float mosaicOn[4]; // 1 where mosaic > 1 (specialized: one compose variant for every size)
	float layerSw[4];  // ADR-161: sprite window only (N3..N0): 1 = hidden where the sprite MSB is set, 2 = where it is clear
	float ccwA[4], ccwB[4], ccw0[4], ccw1[4]; // ADR-112: colour calculation window (rectangles W0/W1)
	float misc[4];
	float sprA[4], sprB[4], sprC[4], sprD[4], sprE[4];
	float prioLo[4], prioHi[4], ccLo[4], ccHi[4];
	float mode[4];
	int w, h;
	int gpu; // composite this set on the GPU?
	// stage 2: NBG layers drawn by GPU
	GxTex *vram;               // 512x256 RGBA8: raw VDP2 VRAM bytes
	GxTex *rows;               // 64x256 RGBA8: per-layer line values (8 texels per layer)
	uint8_t *vram_px, *rows_px;
	uint32_t vram_copied;      // rstv_vdp2_serial when VRAM was copied into this set (0 = not yet)
	int gpu_layer[NUM_BG];     // draw this layer with the GPU layer shader?
	float nbg[NUM_BG][9][4];   // layer shader uniforms (cfgA..cfgI)
	void *nbgw_v[NUM_BG];      // layers with a window (ADR-040): visible span quads (position NDC 2, pixel 2), else full screen
	void *qclr_v[NUM_BG];      // ADR-052: strips outside the window to clear after window-ignoring cell quads (position NDC 2)
	int qclr_n[NUM_BG];
	void *nbgh_v[NUM_BG];      // ADR-052: scaled lines of a cell quad layer (pixel shader line quads, same format as nbgw)
	int nbgh_n[NUM_BG];
	int nbgw_n[NUM_BG];
	// cell quads (ADR-014): if quad_n[L] >= 0 this layer is drawn as quads
	int quad_n[NUM_BG];
	// ADR-123: bitmap NBG as an RGBA texture (colour, alpha = opaque 128 | palette MSB 64 | dot & 15), rebuilt per changed row
	GxTex *bmp_tex[NUM_BG];
	uint32_t bmp_copied[NUM_BG], bmp_key[NUM_BG][8];
	int bq_on[NUM_BG], bq_ln[NUM_BG], bq_cn[NUM_BG]; // the layer is drawn as line runs over bmp_tex (final pixels)
	void *bq_lv[NUM_BG];
	void *quad_v[NUM_BG];     // (frame temp GPU memory) 4 vertices per quad x (x, y, u, v, palette, priority, scf, 0)
	float quad_cfg[NUM_BG][3][4];
	int quad_cn[NUM_BG];
	// RBG0 rotation (ADR-016): drawn on GPU if rbg_n > 0 (line quad count)
	GxTex *rbg_table;          // 8x8 page table
	float *rbg_v;              // (frame temp GPU memory) line quad vertices (position NDC 2, map coords 2)
	int rbg_n;
	GxTex *rbg_table_b;        // ADR-046: B page table and B span quads when parameter B uses a different map
	float *rbg_vb;
	int rbg_nb;
	float rbg_cfg[4][4];
	float rbg_cfg2[4];         // bitmap RBG0 (ADR-037): VRAM address, palette, priority, special color calc
	int rbg_bmp;
	int rbg_so_b;        // ADR-071: off-screen mode of the B list
	int rbg_flat;        // ADR-067: RBG0 is in map-order cell layout this frame (variant key + 256)
	float offu[7][4];          // color offset uniforms for fast composite (back, N3, N2, N1, N0, R0, sprite)
	int off_ok;                // is each layer's color offset the same on every line (if so, fast composite shader)
	uint32_t pal_serial;       // rstv_cram_serial when the palette was converted
	int pal_mode;
	// VDP1 GPU renderer (ADR-023)
	void *v1_v;                // VDP1 triangles to draw this frame (frame temp GPU memory)
	int v1_nv, v1_target, v1_pending;
	int v1_clear;              // ADR-149: VDP1 target to clear before this frame (-1 none)
	V1Frame v1f;               // batches per shader variant
	int spr_front;             // VDP1 buffer read by the composite, s_v1rt[spr_front] if spr_gpu
	int spr_gpu;
	int swin_n;       // ADR-100: sprite layer window 0 as clear quads on the sprite texture (outside / inside a rectangle)
	float *swin_cv;
	int swin_x0, swin_y0, swin_x1, swin_y1, swin_in; // the rectangle (screen pixels, inclusive), 1 = visible inside
	int swin_lw;                        // ADR-117: line window, x span per line in swin_lx0/1 ([x0, x1))
	int16_t swin_lx0[256], swin_lx1[256];
	int spr8; // ADR-100: VDP1 framebuffer is 8-bit 1024 wide (hi-res): read every 2nd pixel into the 16-bit sprite texture
	int submitted;             // worker thread finished up to GPU submit (ADR-029)
	// sprite decode table (ADR-027): 512x256 RGBA8, (lo, hi) -> left (color, key), right (alpha byte, shadow type)
	GxTex *slut;
	uint8_t *slut_px;
	float slut_key[48];
	int slut_valid;
	int slut_idx; // ADR-089: slot in shared pool (s_slut_pool), -1 = none
	// ADR-057: layer reuse
	uint32_t lvh[NUM_BG];      // vertex hash of the cell quads (built on the CPU side)
	uint32_t lbank[NUM_BG][4]; // 16-colour palette banks the cell quads use (128 bits; all set = whole palette)
	uint32_t rbg_vh;           // vertex hash of the RBG0 line quads
	uint32_t rbg_bank[4];      // palette banks of the RBG0 map pages (before the colour offset)
	uint8_t lok[NUM_BG];       // this layer may come from the cache texture (cell quads without pixel shader lines, RBG0)
	GxTex *lsrc[NUM_BG];       // texture the compose reads for the layer
	// ADR-058: plane cache layers: pass 1 (cell quads into the plane, -1 = plane kept), pass 2 (line copy quads)
	int pl_on[NUM_BG], pl_n[NUM_BG], pl_ln[NUM_BG], pl_cn[NUM_BG];
	void *pl_v[NUM_BG], *pl_lv[NUM_BG], *pl_cv[NUM_BG]; // pl_cv: clear bands (NDC) of the plane rows drawn again
	int pl_idx[NUM_BG]; // ADR-089: the plane holds palette indices (pass 2 looks the colours up)
} FrameSet;

// ADR-057: GPU layers whose inputs did not change are not drawn again. Eligible layers draw into one cache texture per
// layer (not the per-set texture the CPU may write); the key covers their vertices, settings and palette, and a generation
// that moves whenever the character atlas, the RBG page cache or the VRAM copy changes. ux0:data/restv/noreuse: off
static GxTex *s_lcache[NUM_BG];
static uint32_t s_lkey[NUM_BG], s_cgen = 1, s_rbgq_dec_seen;
static int s_lvalid[NUM_BG], s_reuse_on;
static uint32_t s_vcstamp[128]; // VRAM 4KB page: serial of the snapshot where its bytes last changed (+0), for the RBG page cache
static uint32_t s_vcsub[128][16]; // ADR-137: the same for each 256-byte block of the page (plane row dependencies)
static uint32_t mixw(uint32_t h, const void *p, size_t bytes)
{
	const uint32_t *w = (const uint32_t *)p;
	for (size_t i = 0; i < bytes / 4; i++)
		h = (h ^ w[i]) * 16777619u;
	return h;
}
#define MIXV(h, v) ((h) = ((h) ^ (uint32_t)(v)) * 16777619u)

// ADR-058: layers whose lines scroll / zoom differently (perspective floors, waves) would need one-line cell quads
// (5000+ tiny quads, 9 ms of GPU in sasissu / sanjeon 3D scenes). Their map rows are drawn once as plain cell quads
// into a plane texture (kept while the VRAM pages it read and its palette banks do not change), and each screen line
// copies its run of the plane (map x = x + i * zx, + line scroll) with one quad per wrap-free segment.
#define PLANE_W 1024
#define PLANE_WMAX 2048 // ADR-100: maps 2048 wide (2x1 planes, twcup98) get a wider plane texture for that layer
static int s_plane_w[4] = { PLANE_W, PLANE_W, PLANE_W, PLANE_W }; // plane texture width per NBG
#define PLANE_H 512
#define PLANE_HMAX 1024 // ADR-125: a layer whose lines need two map rows on one plane row gets a 1024-row plane
static int s_plane_h[4] = { PLANE_H, PLANE_H, PLANE_H, PLANE_H }; // plane texture height per NBG
static GxTex *s_plane[NUM_BG];
static int s_plane_on, s_plane_valid[NUM_BG];
static uint32_t s_plane_key[NUM_BG], s_plane_stamp[NUM_BG], s_plane_pages[NUM_BG][4], s_plane_bank[NUM_BG][4], s_plane_bankh[NUM_BG];
static int s_plane_tag[NUM_BG][PLANE_HMAX];  // map row held by each plane row (-1 = none): plane row = map y & (PLANE_H - 1)
static short s_plane_c0[NUM_BG][PLANE_HMAX], s_plane_c1[NUM_BG][PLANE_HMAX]; // columns drawn on that row [c0, c1)
static uint32_t s_plane_rpg[NUM_BG][PLANE_HMAX][4]; // ADR-100: VRAM pages (4KB) each plane row was drawn from
#define PLANE_GROUP_ROWS 16 // ADR-100: plane rows drawn per group at most (tracking granularity; ADR-137: 32 -> 16)
// ADR-137: the 256-byte blocks each plane row read, for up to PL_SUBN pages (n = 255: more pages, page granularity).
// vfkids rewrites a few NBG1 map cells every frame: a 4KB map page holds 256 plane rows, which were all drawn again
#define PL_SUBN 14
typedef struct { uint8_t n, pg[PL_SUBN]; uint16_t m[PL_SUBN]; } PlSub;
static PlSub s_plane_sub[NUM_BG][PLANE_HMAX];
static uint32_t s_plane_gen[NUM_BG];      // moves whenever plane rows are drawn (layer reuse key)
static GxProg *s_prog_pcopy;
static GxParam s_pc_scale, s_pc_tex;
static RstvLayerCapture s_plane_cap;
static const char *s_vs_pc =
	"void main(float2 position, float2 uv, uniform float4 scale, float4 out vPosition : POSITION, float2 out vUV : TEXCOORD0)\n"
	"{\n"
	"  vPosition = float4(position * scale.xy - 1.0, 0.0, 1.0);\n"
	"  vUV = uv * scale.zw;\n"
	"}\n";
static const char *s_fs_pc =
	"float4 main(float2 vUV : TEXCOORD0, uniform sampler2D tPlane) : COLOR\n"
	"{\n"
	"  return tex2D(tPlane, vUV);\n"
	"}\n";
// ADR-089: palette-index plane. Pass 1 writes r = palette address low 8 bits, g = high 3 bits + 8 (ratio flag fixed on)
// + 16 (ratio flag = palette MSB), a = key without the ratio flag (0 = transparent). Pass 2 looks the colour up in tPal, so a
// CRAM change (fades) no longer redraws the plane (g8 round end: 38 rebuilds of 240 rows, 5ms each)
static GxProg *s_prog_qi, *s_prog_pci;
static GxParam s_qi_scale, s_qi_atlas, s_qi_cfg, s_qi_lo, s_qi_hi, s_pci_scale, s_pci_tex, s_pci_pal;
static const char *s_fs_qi =
	"float lut8(float i, float4 lo, float4 hi) {\n"
	"  float4 a = step(abs(float4(i, i, i, i) - float4(0.0, 1.0, 2.0, 3.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  float4 b = step(abs(float4(i, i, i, i) - float4(4.0, 5.0, 6.0, 7.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  return dot(a, lo) + dot(b, hi);\n"
	"}\n"
	"float4 main(float2 vUV : TEXCOORD0, float4 vAttr : TEXCOORD1, uniform sampler2D tAtlas,\n"
	"  uniform float4 qcfg, uniform float4 qcodeLo, uniform float4 qcodeHi) : COLOR\n"
	"{\n"
	"  float idx = floor(tex2D(tAtlas, vUV).x * 255.0 + 0.5);\n"
	"  float prio = floor(vAttr.y + 0.5);\n"
	"  if ((idx < 0.5 && qcfg.x > 0.5) || prio < 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"  float pa = floor(vAttr.x + 0.5) + idx;\n"
	"  if (pa >= 2048.0) pa -= 2048.0;\n"
	"  float hi = floor(pa / 256.0);\n"
	"  float scm = qcfg.y;\n"
	"  float fl = 8.0;\n"
	"  if ((scm > 0.5 && scm < 2.5) && vAttr.z < 0.5) fl = 0.0;\n"
	"  else if (scm > 1.5 && scm < 2.5) { if (lut8(floor(fmod(idx, 16.0) / 2.0), qcodeLo, qcodeHi) < 0.5) fl = 0.0; }\n"
	"  else if (scm > 2.5) fl = 16.0;\n"
	"  if (qcfg.z < 0.5) fl = 0.0;\n"
	"  return float4((pa - hi * 256.0) / 255.0, (hi + fl) / 255.0, 0.0, (prio * 8.0 + qcfg.w) / 255.0);\n"
	"}\n";
static const char *s_fs_pci =
	"float4 main(float2 vUV : TEXCOORD0, uniform sampler2D tPlane, uniform sampler2D tPal) : COLOR\n"
	"{\n"
	"  float4 t = tex2D(tPlane, vUV);\n"
	"  float a = floor(t.w * 255.0 + 0.5);\n"
	"  if (a < 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"  float g = floor(t.y * 255.0 + 0.5);\n"
	"  float fl = floor(g / 8.0);\n"
	"  float pa = floor(t.x * 255.0 + 0.5) + (g - fl * 8.0) * 256.0;\n"
	"  float4 pc = tex2D(tPal, float2((pa + 0.5) / 2048.0, 0.5));\n"
	"  float flag = (fl > 0.5 && fl < 1.5) ? 128.0 : (fl > 1.5 && pc.w > 0.5) ? 128.0 : 0.0;\n"
	"  return float4(pc.xyz, (a + flag) / 255.0);\n"
	"}\n";

#define ROWS_W 64

// comparison capture: at the frame numbers in ux0:data/restv/shot.txt (one per line), save both GPU and CPU composite as PPM
#define MAX_SHOTS 16
static unsigned s_shot_frames[MAX_SHOTS];
static int s_shots;
static unsigned s_frame;
static int s_shot_now;
void TitanRender(pixel_t *dispbuffer);
extern pixel_t *dispbuffer;

static void load_shot_list(void)
{
	FILE *f = fopen("ux0:data/restv/shot.txt", "r");
	if (!f)
		return;
	unsigned v;
	while (s_shots < MAX_SHOTS && fscanf(f, "%u", &v) == 1)
		s_shot_frames[s_shots++] = v;
	fclose(f);
}

static void save_ppm(const char *path, const uint8_t *rgba, int w, int h, int stride)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			fwrite(rgba + (y * stride + x) * 4, 1, 3, f);
	fclose(f);
}

static FrameSet s_set[NSETS];
void (*gvdp2_progress)(int permille); // loading screen during gvdp2_init (main thread, outside any scene)
static int s_cur = -1; // set filled by draw_screens
static GxTex *s_out_tex[NSETS];
static int file_exists_q(const char *path);
static GxProg *s_prog, *s_prog_fast;
static GxProg *s_prog_ovr;   // experiment: composite everything with the source in ux0:data/restv/cmpoverride.cg (GPU time measurement)
static GxParam s_loc_ovr[64];
static GxProg *s_prog_exp; // for measurement
static int s_cmp_exp;
static GxProg *s_prog_v1;      // VDP1 GPU renderer (ADR-023): variant 0 (others in s_v1prog)
static GxProg *s_v1prog[V1GPU_VARIANTS];
static GxParam s_v1loc[V1GPU_VARIANTS];
int rstv_v1_fullsnap; // diag (ux0:data/restv/v1full): copy all VDP1 RAM a frame, recheck every texture
static GxTex *s_v1rt[2];       // 2 VDP1 framebuffers (front/back by rstv_vdp1_back_idx)
static GxTex *s_v1atlas;       // sprite pattern cache (1024x1024 RGBA8)
static GxTex *s_v1snap;        // ADR-115: VDP1 framebuffer copy read by shadow/half-transparency (512x256)
static GxParam s_v1snaploc[V1GPU_VARIANTS];
static int v1_cc_programs_ok;
static float s_v1buf[V1GPU_MAX_VERTS * V1GPU_VERTEX_FLOATS];
static GxParam s_loc[40], s_loc_fast[40]; // >= U_COUNT (35)
static int s_ok;

// ---- layer prep worker thread (ADR-025): VDP2 state snapshot ----
// main only does capture (vidsoft) and snapshot at VBlankOUT; the rest of the prep (prep_b) runs on the CPU2 thread. draw_end waits for it.
static Vdp2 s_snap_regs;
static Vdp2 s_snap_lines[270];
static u8 s_snap_cram[0x1000];
static u8 *s_snap_vram;                 // 512KB
static struct CellScrollData s_snap_cells[270]; // ADR-079: vertical cell scroll table of the snapshot
static volatile int s_prep_capture;     // ADR-079: the worker runs the layer pass first
static int s_capture_main;              // ADR-079: 1 = layer pass on the main thread (ux0:data/restv/capmain)
static void capture_layers(void);
static u32 s_snap_vram_serial, s_snap_cram_serial = 0xFFFFFFFFu, s_snap_serial;
static int s_snap_colormode, s_snap_valid;
static SceUID s_prep_go = -1, s_prep_done = -1;
static volatile int s_prep_pending;
static FrameSet *s_prep_s;
static u8 *s_prep_front;
static u64 s_prep_tp;
static int s_prep_thread_ok;
static int s_submit_thr; // worker thread goes up to GPU submit (ADR-029, disabled by ux0:data/restv/nosubmitthr)
static void gpu_submit(FrameSet *s, int oi, int on_worker);
// VDP1 RAM / command list snapshot (ADR-025): s_v1snap_stamp is the serial when a page was copied (for the cache's 'changed' check)
static u8 *s_v1snap_ram;
static u32 s_v1snap_stamp[128], s_v1snap_serial;
static int s_v1snap_valid, s_v1snap_n, s_v1snap_fmt; // fmt: ADR-100 (rstv_v1list_fmt)
static RstvV1Item s_v1snap_list[RSTV_V1_MAX];
static int s_shot_async; // test: with ux0:data/restv/shotasync, capture frames also go to the worker thread (no CPU comparison image)
static gvdp2_stats s_stats;

enum {
	U_tN3, U_tN2, U_tN1, U_tN0, U_tR0, U_tSpr, U_tPal, U_tLine,
	U_layerOn, U_layerAlpha, U_layerShad, U_layerLs, U_misc,
	U_sprA, U_sprB, U_sprC, U_sprD, U_sprE, U_prioLo, U_prioHi, U_ccLo, U_ccHi, U_mode,
	U_offB, U_off3, U_off2, U_off1, U_off0, U_offr, U_offs, U_tSLut,
	U_ccwA, U_ccwB, U_ccw0, U_ccw1, U_mosaic, U_mosaicOn, U_layerSw,
	U_COUNT
};
_Static_assert(U_COUNT <= 40, "s_loc size");
static const char *s_uname[U_COUNT] = {
	"tN3", "tN2", "tN1", "tN0", "tR0", "tSpr", "tPal", "tLine",
	"layerOn", "layerAlpha", "layerShad", "layerLs", "misc",
	"sprA", "sprB", "sprC", "sprD", "sprE", "prioLo", "prioHi", "ccLo", "ccHi", "mode",
	"offB", "off3", "off2", "off1", "off0", "offr", "offs_", "tSLut",
	"ccwA", "ccwB", "ccw0", "ccw1", "mosaic", "mosaicOn", "layerSw",
};

static const char *s_vs =
	"void main(float2 position, float2 pix, float4 out vPosition : POSITION, float2 out vPix : TEXCOORD0)\n"
	"{\n"
	"  vPosition = float4(position, 0.0, 1.0);\n"
	"  vPix = pix;\n"
	"}\n";

// fast composite: layer (512x256) and line value (16x256) coords are passed from the vertex shader. Reading with coords computed in the fragment
// is a dependent read and defeats texture prefetch (most of the 10ms composite GPU time)
static const char *s_vs_fast =
	"void main(float2 position, float2 pix, float4 out vPosition : POSITION, float2 out vPix : TEXCOORD0,\n"
	"  float2 out vUV : TEXCOORD1, float2 out vL0 : TEXCOORD2, float2 out vL1 : TEXCOORD3)\n"
	"{\n"
	"  vPosition = float4(position, 0.0, 1.0);\n"
	"  vPix = pix;\n"
	"  vUV = pix / float2(512.0, 256.0);\n"
	"  vL0 = float2(0.5 / 16.0, pix.y / 256.0);\n"
	"  vL1 = float2(1.5 / 16.0, pix.y / 256.0);\n"
	"}\n";

// composite shader: same integer result as Titan's TitanDigPixel (top 2 pixels by priority + line screen/shadow/color calc)
static const char *s_fs =
	"float lut8(float i, float4 lo, float4 hi) {\n"
	"  float4 a = step(abs(float4(i, i, i, i) - float4(0.0, 1.0, 2.0, 3.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  float4 b = step(abs(float4(i, i, i, i) - float4(4.0, 5.0, 6.0, 7.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  return dot(a, lo) + dot(b, hi);\n"
	"}\n"
	"float4 fetch(sampler2D t, float2 p, float2 sz) { return floor(tex2D(t, (p + 0.5) / sz) * 255.0 + 0.5); }\n"
	"float4 lay(float4 c) { float f = (c.w >= 128.0) ? 1.0 : 0.0; return float4(c.xyz, floor((c.w - 128.0 * f) / 8.0) + 8.0 * f); }\n"
	"float3 offs(float3 c, float4 lo, float4 hi) {\n"
	"  if (lo.w < 0.5) return c;\n"
	"  float3 o = lo.xyz + hi.xyz * 256.0 - 256.0;\n"
	"  return clamp(c + o, 0.0, 255.0);\n"
	"}\n"
	"float3 btop(float3 t, float ta, float3 b) {\n" // TitanBlendPixelsTop (a = low 6 bits)
	"  float a = fmod(ta, 64.0) * 4.0 + 3.0;\n"
	"  return floor(t * a / 255.0) + floor(b * (255.0 - a) / 255.0);\n"
	"}\n"
	"float3 blend(float m, float3 t, float ta, float3 b, float ba) {\n"
	"  if (m < 0.5) return btop(t, ta, b);\n"
	"  if (m < 1.5) {\n" // bottom: unchanged if the top pixel has no bit7
	"    if (ta < 128.0) return t;\n"
	"    float a = fmod(ba, 64.0) * 4.0 + 3.0;\n"
	"    return floor(t * a / 255.0) + floor(b * (255.0 - a) / 255.0);\n"
	"  }\n"
	"  return min(t + b, 255.0);\n"
	"}\n"
	"float trans(float m, float a) { return (m < 0.5) ? ((fmod(a, 64.0) < 63.0) ? 1.0 : 0.0) : ((a >= 128.0) ? 1.0 : 0.0); }\n"
	"float mx(float x, float on, float m) { return (on > 0.5) ? floor(x / m) * m : x; }\n" // ADR-129 horizontal mosaic
	"float4 main(float2 vPix : TEXCOORD0,\n"
	"  uniform sampler2D tN3, uniform sampler2D tN2, uniform sampler2D tN1, uniform sampler2D tN0, uniform sampler2D tR0,\n"
	"  uniform sampler2D tSpr, uniform sampler2D tPal, uniform sampler2D tLine,\n"
	"  uniform float4 layerOn, uniform float4 layerAlpha, uniform float4 layerShad, uniform float4 layerLs, uniform float4 misc,\n"
	"  uniform float4 sprA, uniform float4 sprB, uniform float4 sprC, uniform float4 sprD, uniform float4 sprE,\n"
	"  uniform float4 prioLo, uniform float4 prioHi, uniform float4 ccLo, uniform float4 ccHi, uniform float4 mode,\n"
	"  uniform float4 ccwA, uniform float4 ccwB, uniform float4 ccw0, uniform float4 ccw1, uniform float4 mosaic, uniform float4 mosaicOn,\n"
	"  uniform float4 layerSw) : COLOR\n"
	"{\n"
	"  float x = floor(vPix.x);\n"
	"  float y = floor(vPix.y);\n"
	"  float ccok = 1.0;\n"
	"  if (ccwA.x > 0.5) {\n"
	"    float h0 = step(ccw0.x, x) * step(x, ccw0.z), h1 = step(ccw1.x, x) * step(x, ccw1.z);\n"
	"    float i0 = h0 * step(ccw0.y, y) * step(y, ccw0.w), i1 = h1 * step(ccw1.y, y) * step(y, ccw1.w);\n"
	"    float v0 = (ccwA.z > 0.5) ? i0 : 1.0 - max(i0, h0 * ccwB.z);\n"
	"    float v1 = (ccwA.w > 0.5) ? i1 : 1.0 - max(i1, h1 * ccwB.w);\n"
	"    if (ccwB.x < 0.5) ccok = v0;\n"
	"    else if (ccwA.y < 0.5) ccok = v1;\n"
	"    else ccok = (ccwB.y > 0.5) ? max(v0, v1) : min(v0, v1);\n"
	"  }\n"
	"  float2 lsz = float2(16.0, 256.0);\n"
	"  float2 bsz = float2(512.0, 256.0);\n"
	// per-line values
	"  float4 lBack = fetch(tLine, float2(0.0, y), lsz);\n"
	"  float4 lLs = fetch(tLine, float2(1.0, y), lsz);\n"
	"  float3 back = offs(lBack.xyz, fetch(tLine, float2(2.0, y), lsz), fetch(tLine, float2(3.0, y), lsz));\n"
	// candidates of the 6 background layers: key = priority * 8 + order (sprite 5, R0 4, N0 3, N1 2, N2 1, N3 0), -1 if none
	"  float4 c3 = lay(fetch(tN3, float2(mx(x, mosaicOn.x, mosaic.x), y), bsz));\n"
	"  float4 c2 = lay(fetch(tN2, float2(mx(x, mosaicOn.y, mosaic.y), y), bsz));\n"
	"  float4 c1 = lay(fetch(tN1, float2(mx(x, mosaicOn.z, mosaic.z), y), bsz));\n"
	"  float4 c0 = lay(fetch(tN0, float2(mx(x, mosaicOn.w, mosaic.w), y), bsz));\n"
	"  float4 cr = lay(fetch(tR0, float2(x, y), bsz));\n"
	"  if (layerSw.x + layerSw.y + layerSw.z + layerSw.w > 0.5) {\n" // ADR-161: sprite window (sprite MSB) per layer
	"    float4 sw = (sprE.z > 0.5) ? fetch(tSpr, float2(x, y), float2(512.0, 256.0)) : fetch(tSpr, float2(floor(x / 2.0), y), float2(256.0, 256.0));\n"
	"    float b = step(127.5, (sprE.z > 0.5 || fmod(x, 2.0) < 0.5) ? sw.y : sw.w);\n"
	"    float4 hd = step(abs(layerSw - 1.0), 0.5) * b + step(abs(layerSw - 2.0), 0.5) * (1.0 - b);\n"
	"    c3 *= 1.0 - hd.x; c2 *= 1.0 - hd.y; c1 *= 1.0 - hd.z; c0 *= 1.0 - hd.w;\n"
	"  }\n"
	"  float p3 = fmod(c3.w, 8.0) * layerOn.x, p2 = fmod(c2.w, 8.0) * layerOn.y;\n"
	"  float p1 = fmod(c1.w, 8.0) * layerOn.z, p0 = fmod(c0.w, 8.0) * layerOn.w, pr = fmod(cr.w, 8.0) * misc.x;\n"
	"  float k3 = (p3 > 0.5) ? p3 * 8.0 + 0.0 : -1.0;\n"
	"  float k2 = (p2 > 0.5) ? p2 * 8.0 + 1.0 : -1.0;\n"
	"  float k1 = (p1 > 0.5) ? p1 * 8.0 + 2.0 : -1.0;\n"
	"  float k0 = (p0 > 0.5) ? p0 * 8.0 + 3.0 : -1.0;\n"
	"  float kr = (pr > 0.5) ? pr * 8.0 + 4.0 : -1.0;\n"
	"  float a3 = (c3.w >= 8.0) ? layerAlpha.x : 63.0, a2 = (c2.w >= 8.0) ? layerAlpha.y : 63.0;\n"
	"  float a1 = (c1.w >= 8.0) ? layerAlpha.z : 63.0, a0 = (c0.w >= 8.0) ? layerAlpha.w : 63.0;\n"
	"  float ar = (cr.w >= 8.0) ? misc.y : 63.0;\n"
	"  if (ccok < 0.5) { a3 = 63.0; a2 = 63.0; a1 = 63.0; a0 = 63.0; ar = 63.0; }\n"
	"  float3 col3 = offs(c3.xyz, fetch(tLine, float2(4.0, y), lsz), fetch(tLine, float2(5.0, y), lsz));\n"
	"  float3 col2 = offs(c2.xyz, fetch(tLine, float2(6.0, y), lsz), fetch(tLine, float2(7.0, y), lsz));\n"
	"  float3 col1 = offs(c1.xyz, fetch(tLine, float2(8.0, y), lsz), fetch(tLine, float2(9.0, y), lsz));\n"
	"  float3 col0 = offs(c0.xyz, fetch(tLine, float2(10.0, y), lsz), fetch(tLine, float2(11.0, y), lsz));\n"
	"  float3 colr = offs(cr.xyz, fetch(tLine, float2(12.0, y), lsz), fetch(tLine, float2(13.0, y), lsz));\n"
	// sprite (VidsoftDrawSprite 16-bit path)
	"  float ks = -1.0; float3 cols = float3(0.0, 0.0, 0.0); float as_ = 63.0; float shs = 0.0;\n"
	"  if (sprB.w > 0.5) {\n"
	"    float4 st = (sprE.z > 0.5) ? fetch(tSpr, float2(x, y), float2(512.0, 256.0)) : fetch(tSpr, float2(floor(x / 2.0), y), float2(256.0, 256.0));\n"
	"    float pix = (sprE.z > 0.5 || fmod(x, 2.0) < 0.5) ? (st.x + st.y * 256.0) : (st.z + st.w * 256.0);\n"
	"    if (pix > 0.5) {\n"
	"      float sprio = 0.0; float doOff = 1.0;\n"
	"      if (pix >= 32768.0 && sprB.z > 0.5) {\n" // RGB direct color
	"        float a = 63.0;\n"
	"        if (sprC.x > 2.5 && sprC.z > 0.5) { a = ccLo.x; if (sprD.y > 0.5) a += 128.0; }\n"
	"        float draw = (pix != 32768.0 || sprD.x < 2.0 || (sprD.x < 8.0 && sprE.y < 0.5)) ? 1.0 : 0.0;\n"
	"        if (draw > 0.5) {\n"
	"          float v = pix - 32768.0;\n"
	"          cols = float3(fmod(v, 32.0), fmod(floor(v / 32.0), 32.0), fmod(floor(v / 1024.0), 32.0)) * 8.0;\n"
	"          sprio = prioLo.x; as_ = a;\n"
	"        }\n"
	"      } else {\n" // color bank
	"        float pi = fmod(floor(pix / sprA.x), sprA.y);\n"
	"        float ci = fmod(floor(pix / sprA.z), sprA.w);\n"
	"        float code = fmod(pix, sprB.x);\n"
	"        float msb = (sprB.y > 0.5 && pix >= 32768.0) ? 1.0 : 0.0;\n"
	"        float pv = lut8(pi, prioLo, prioHi);\n"
	"        if (code == sprB.x - 2.0) {\n" // normal shadow
	"          sprio = pv; shs = 1.0; doOff = 0.0;\n"
	"        } else {\n"
	"          float pa = sprC.w + code;\n"
	"          float4 pc = fetch(tPal, float2(fmod(pa, 2048.0), 0.0), float2(2048.0, 1.0));\n"
	"          float a = 63.0;\n"
	"          if (sprC.z > 0.5) {\n"
	"            float tr = 0.0;\n"
	"            if (sprC.x < 0.5) tr = (pv <= sprC.y) ? 1.0 : 0.0;\n"
	"            else if (sprC.x < 1.5) tr = (pv == sprC.y) ? 1.0 : 0.0;\n"
	"            else if (sprC.x < 2.5) tr = (pv >= sprC.y) ? 1.0 : 0.0;\n"
	"            else tr = (pc.w > 127.0) ? 1.0 : 0.0;\n"
	"            float cv = lut8(ci, ccLo, ccHi);\n"
	"            if (sprD.z > 0.5) { a = cv; if (tr > 0.5) a += 128.0; }\n"
	"            else if (tr > 0.5) { a = cv; if (sprD.w > 0.5) a += 128.0; }\n"
	"          }\n"
	"          sprio = pv; as_ = a;\n"
	"          if (msb > 0.5) shs = 2.0;\n"
	"          cols = (msb > 0.5 && code < 0.5) ? float3(0.0, 0.0, 0.0) : pc.xyz;\n"
	"        }\n"
	"      }\n"
	"      if (ccok < 0.5) as_ = 63.0;\n"
	"      if (sprio > 0.5) {\n"
	"        ks = sprio * 8.0 + 5.0;\n"
	"        if (doOff > 0.5) cols = offs(cols, fetch(tLine, float2(14.0, y), lsz), fetch(tLine, float2(15.0, y), lsz));\n"
	"      }\n"
	"    }\n"
	"  }\n"
	// pick top 2
	"  float m1 = max(max(max(k3, k2), max(k1, k0)), max(kr, ks));\n"
	"  float m2 = -1.0;\n"
	"  m2 = (k3 < m1) ? max(m2, k3) : m2; m2 = (k2 < m1) ? max(m2, k2) : m2;\n"
	"  m2 = (k1 < m1) ? max(m2, k1) : m2; m2 = (k0 < m1) ? max(m2, k0) : m2;\n"
	"  m2 = (kr < m1) ? max(m2, kr) : m2; m2 = (ks < m1) ? max(m2, ks) : m2;\n"
	// candidate attrs: rgb, alpha byte, shadow type, shadow target, line screen
	"  float3 t0c = back; float t0a = 63.0, t0s = 0.0, t0e = 0.0, t0l = 0.0;\n"
	"  float3 t1c = float3(0.0, 0.0, 0.0); float t1a = 0.0, t1e = 0.0;\n"
	"  if (m1 >= 0.0) {\n"
	"    float s = fmod(m1, 8.0);\n"
	"    if (s < 0.5) { t0c = col3; t0a = a3; t0e = layerShad.x; t0l = layerLs.x; }\n"
	"    else if (s < 1.5) { t0c = col2; t0a = a2; t0e = layerShad.y; t0l = layerLs.y; }\n"
	"    else if (s < 2.5) { t0c = col1; t0a = a1; t0e = layerShad.z; t0l = layerLs.z; }\n"
	"    else if (s < 3.5) { t0c = col0; t0a = a0; t0e = layerShad.w; t0l = layerLs.w; }\n"
	"    else if (s < 4.5) { t0c = colr; t0a = ar; t0e = misc.z; t0l = misc.w; }\n"
	"    else { t0c = cols; t0a = as_; t0s = shs; t0e = 0.0; t0l = sprE.x; }\n"
	"    t1c = back; t1a = 63.0; t1e = 0.0;\n"
	"    if (m2 >= 0.0) {\n"
	"      float s2 = fmod(m2, 8.0);\n"
	"      if (s2 < 0.5) { t1c = col3; t1a = a3; t1e = layerShad.x; }\n"
	"      else if (s2 < 1.5) { t1c = col2; t1a = a2; t1e = layerShad.y; }\n"
	"      else if (s2 < 2.5) { t1c = col1; t1a = a1; t1e = layerShad.z; }\n"
	"      else if (s2 < 3.5) { t1c = col0; t1a = a0; t1e = layerShad.w; }\n"
	"      else if (s2 < 4.5) { t1c = colr; t1a = ar; t1e = misc.z; }\n"
	"      else { t1c = cols; t1a = as_; t1e = 0.0; }\n"
	"    }\n"
	"  }\n"
	// TitanDigPixel
	"  float m = mode.x;\n"
	"  if (t0l > 0.5) { t0c = blend(m, t0c, t0a, lLs.xyz, lLs.w); if (m < 0.5 || m > 1.5) t0a = 63.0; }\n"
	"  float3 shadowed = btop(float3(0.0, 0.0, 0.0), 32.0, t1c);\n"
	"  float zero = (t0c.x + t0c.y + t0c.z < 0.5) ? 1.0 : 0.0;\n"
	"  if (t0s > 1.5 && zero > 0.5) {\n"
	"    t0c = (t1e > 0.5) ? shadowed : t1c;\n"
	"  } else if (t0s > 1.5) {\n"
	"    if (trans(m, t0a) > 0.5) t0c = blend(m, t0c, t0a, t1c, t1a);\n"
	"    if (mode.z < 0.5) t0c = btop(float3(0.0, 0.0, 0.0), 32.0, t0c);\n"
	"  } else if (t0s > 0.5) {\n"
	"    t0c = (t1e > 0.5) ? shadowed : t1c;\n"
	"  } else if (trans(m, t0a) > 0.5) {\n"
	"    t0c = blend(m, t0c, t0a, t1c, t1a);\n"
	"  }\n"
	"  return float4(t0c / 255.0, 1.0);\n"
	"}\n";

// fast composite shader: when color offset is the same on every line (uniform). Line value reads 16 -> 2 (back, line screen)
// compares layer texture a = key (priority * 8 + order, +128 if ratio used) as normalized values and picks the top 2 one-hot.
// colors mixed as 0..1 (float blend instead of Titan's integer floor, at most 1 off per channel: ADR-027). GXP 372 -> instruction counts in PERF.md
static const char *s_fs_fast =
	"half lut8(half i, half4 lo, half4 hi) {\n"
	"  half4 a = step(abs(half4(i, i, i, i) - half4(0.0, 1.0, 2.0, 3.0)), half4(0.5, 0.5, 0.5, 0.5));\n"
	"  half4 b = step(abs(half4(i, i, i, i) - half4(4.0, 5.0, 6.0, 7.0)), half4(0.5, 0.5, 0.5, 0.5));\n"
	"  return dot(a, lo) + dot(b, hi);\n"
	"}\n"
	"float4 fetch(sampler2D t, float2 p, float2 sz) { return floor(tex2D(t, (p + 0.5) / sz) * 255.0 + 0.5); }\n"
	"float4 fetchv(sampler2D t, float2 uv) { return floor(tex2D(t, uv) * 255.0 + 0.5); }\n"
	"half3 mixa(half3 t, half r, half3 b) { half w = (r * 4.0 + 3.0) / 255.0; return t * w + b * (1.0 - w); }\n"
	"half3 blend(half m, half3 t, half tr, half tq, half3 b, half br) {\n"
	"  if (m < 0.5) return mixa(t, tr, b);\n"
	"  if (m < 1.5) return (tq < 0.5) ? t : mixa(t, br, b);\n"
	"  return min(t + b, 1.0);\n"
	"}\n"
	"half trans(half m, half r, half q) { return (m < 0.5) ? ((r < 62.5) ? 1.0 : 0.0) : q; }\n"
	"float2 mu(float2 uv, float x, float on, float m) { return (on > 0.5) ? float2((floor(x / m) * m + 0.5) / 512.0, uv.y) : uv; }\n" // ADR-129
	"float4 main(float2 vPix : TEXCOORD0, float2 vUV : TEXCOORD1, float2 vL0 : TEXCOORD2, float2 vL1 : TEXCOORD3,\n"
	"  uniform sampler2D tN3, uniform sampler2D tN2, uniform sampler2D tN1, uniform sampler2D tN0, uniform sampler2D tR0,\n"
	"  uniform sampler2D tSpr, uniform sampler2D tPal, uniform sampler2D tLine, uniform sampler2D tSLut,\n"
	"  uniform float4 offB, uniform float4 off3, uniform float4 off2, uniform float4 off1, uniform float4 off0,\n"
	"  uniform float4 offr, uniform float4 offs_,\n"
	"  uniform float4 layerOn, uniform float4 layerAlpha, uniform float4 layerShad, uniform float4 layerLs, uniform float4 misc,\n"
	"  uniform float4 sprA, uniform float4 sprB, uniform float4 sprC, uniform float4 sprD, uniform float4 sprE,\n"
	"  uniform float4 prioLo, uniform float4 prioHi, uniform float4 ccLo, uniform float4 ccHi, uniform float4 mode,\n"
	"  uniform float4 ccwA, uniform float4 ccwB, uniform float4 ccw0, uniform float4 ccw1, uniform float4 mosaic, uniform float4 mosaicOn) : COLOR\n"
	"{\n"
	"  float x = floor(vPix.x);\n"
	"  float y = floor(vPix.y);\n"
	"  float ccok = 1.0;\n"
	"  if (ccwA.x > 0.5) {\n"
	"    float h0 = step(ccw0.x, x) * step(x, ccw0.z), h1 = step(ccw1.x, x) * step(x, ccw1.z);\n"
	"    float i0 = h0 * step(ccw0.y, y) * step(y, ccw0.w), i1 = h1 * step(ccw1.y, y) * step(y, ccw1.w);\n"
	"    float v0 = (ccwA.z > 0.5) ? i0 : 1.0 - max(i0, h0 * ccwB.z);\n"
	"    float v1 = (ccwA.w > 0.5) ? i1 : 1.0 - max(i1, h1 * ccwB.w);\n"
	"    if (ccwB.x < 0.5) ccok = v0;\n"
	"    else if (ccwA.y < 0.5) ccok = v1;\n"
	"    else ccok = (ccwB.y > 0.5) ? max(v0, v1) : min(v0, v1);\n"
	"  }\n"
	"  half4 lB = tex2D(tLine, vL0);\n"
	"  half4 lL = tex2D(tLine, vL1);\n"
	"  half3 back = lB.xyz;\n"
	"  if (offB.w > 0.5) back = saturate(back + offB.xyz / 255.0);\n"
	"  half4 c3 = half4(0.0, 0.0, 0.0, 0.0), c2 = c3, c1 = c3, c0 = c3, cr = c3;\n"
	"  if (layerOn.x > 0.5) c3 = tex2D(tN3, mu(vUV, x, mosaicOn.x, mosaic.x));\n"
	"  if (layerOn.y > 0.5) c2 = tex2D(tN2, mu(vUV, x, mosaicOn.y, mosaic.y));\n"
	"  if (layerOn.z > 0.5) c1 = tex2D(tN1, mu(vUV, x, mosaicOn.z, mosaic.z));\n"
	"  if (layerOn.w > 0.5) c0 = tex2D(tN0, mu(vUV, x, mosaicOn.w, mosaic.w));\n"
	"  if (misc.x > 0.5) cr = tex2D(tR0, vUV);\n"
	"  half4 W = half4(c3.w, c2.w, c1.w, c0.w);\n"
	"  half4 F = step(half4(0.5, 0.5, 0.5, 0.5), W);\n"
	"  half4 K = W - F * (128.0 / 255.0);\n"
	"  half fr = step(0.5, cr.w);\n"
	"  half kr = cr.w - fr * (128.0 / 255.0);\n"
	"  half4 hA = layerAlpha;\n"
	"  half4 LQ = step(half4(128.0, 128.0, 128.0, 128.0), hA);\n"
	"  half4 FC = F * ccok;\n"
	"  half4 R4 = 63.0 + FC * (hA - 128.0 * LQ - 63.0);\n"
	"  half4 Q4 = FC * LQ;\n"
	"  half hmy = misc.y;\n"
	"  half mq = step(128.0, hmy);\n"
	"  half frc = fr * ccok;\n"
	"  half rR = 63.0 + frc * (hmy - 128.0 * mq - 63.0), rQ = frc * mq;\n"
	"  half ks = 0.0; half3 cols = half3(0.0, 0.0, 0.0); half sa = 63.0; half shs = 0.0;\n"
	"  if (sprB.w > 0.5 && sprE.z > 0.5) {\n"
	"    float4 st = tex2D(tSpr, vUV);\n"
	"    if (st.x + st.y > 0.0) {\n"
	"    float2 lu = st.xy * float2(255.0 / 512.0, 255.0 / 256.0) + float2(0.5 / 512.0, 0.5 / 256.0);\n"
	"    float4 d = tex2D(tSLut, lu);\n"
	"    float4 d2 = tex2D(tSLut, lu + float2(0.5, 0.0));\n"
	"    float pu = dot(d.xy, float2(255.0 / 2048.0, 65280.0 / 2048.0)) + 0.5 / 2048.0;\n"
	"    float4 pc = tex2D(tPal, float2(pu, 0.5));\n"
	"    cols = (d2.w > 0.5) ? pc.xyz : d.xyz;\n"
	"    ks = d.w;\n"
	"    sa = floor(((pc.w > 0.5) ? d2.y : d2.x) * 255.0 + 0.5);\n"
	"    shs = floor(d2.z * 255.0 + 0.5);\n"
	"    if (offs_.w > 0.5 && (shs < 0.5 || shs > 1.5)) cols = saturate(cols + offs_.xyz / 255.0);\n"
	"    }\n"
	"  } else if (sprB.w > 0.5) {\n"
	"    float4 st = fetch(tSpr, float2(floor(x / 2.0), y), float2(256.0, 256.0));\n"
	"    float pix = (fmod(x, 2.0) < 0.5) ? (st.x + st.y * 256.0) : (st.z + st.w * 256.0);\n"
	"    if (pix > 0.5) {\n"
	"      float sprio = 0.0; float doOff = 1.0;\n"
	"      if (pix >= 32768.0 && sprB.z > 0.5) {\n"
	"        float a = 63.0;\n"
	"        if (sprC.x > 2.5 && sprC.z > 0.5) { a = ccLo.x; if (sprD.y > 0.5) a += 128.0; }\n"
	"        float draw = (pix != 32768.0 || sprD.x < 2.0 || (sprD.x < 8.0 && sprE.y < 0.5)) ? 1.0 : 0.0;\n"
	"        if (draw > 0.5) {\n"
	"          float v = pix - 32768.0;\n"
	"          cols = float3(fmod(v, 32.0), fmod(floor(v / 32.0), 32.0), floor(v / 1024.0)) * (8.0 / 255.0);\n"
	"          sprio = prioLo.x; sa = a;\n"
	"        }\n"
	"      } else {\n"
	"        float pi = fmod(floor(pix / sprA.x), sprA.y);\n"
	"        float ci = fmod(floor(pix / sprA.z), sprA.w);\n"
	"        float code = fmod(pix, sprB.x);\n"
	"        float msb = (sprB.y > 0.5 && pix >= 32768.0) ? 1.0 : 0.0;\n"
	"        float pv = lut8(pi, prioLo, prioHi);\n"
	"        if (code == sprB.x - 2.0) {\n"
	"          sprio = pv; shs = 1.0; doOff = 0.0;\n"
	"        } else {\n"
	"          float pa = fmod(sprC.w + code, 2048.0);\n"
	"          float4 pc = tex2D(tPal, float2((pa + 0.5) / 2048.0, 0.5));\n"
	"          float a = 63.0;\n"
	"          if (sprC.z > 0.5) {\n"
	"            float tr = 0.0;\n"
	"            if (sprC.x < 0.5) tr = (pv <= sprC.y) ? 1.0 : 0.0;\n"
	"            else if (sprC.x < 1.5) tr = (pv == sprC.y) ? 1.0 : 0.0;\n"
	"            else if (sprC.x < 2.5) tr = (pv >= sprC.y) ? 1.0 : 0.0;\n"
	"            else tr = (pc.w > 0.5) ? 1.0 : 0.0;\n"
	"            float cv = lut8(ci, ccLo, ccHi);\n"
	"            if (sprD.z > 0.5) { a = cv; if (tr > 0.5) a += 128.0; }\n"
	"            else if (tr > 0.5) { a = cv; if (sprD.w > 0.5) a += 128.0; }\n"
	"          }\n"
	"          sprio = pv; sa = a;\n"
	"          if (msb > 0.5) shs = 2.0;\n"
	"          cols = (msb > 0.5 && code < 0.5) ? float3(0.0, 0.0, 0.0) : pc.xyz;\n"
	"        }\n"
	"      }\n"
	"      if (sprio > 0.5) {\n"
	"        ks = (sprio * 8.0 + 5.0) / 255.0;\n"
	"        if (doOff > 0.5 && offs_.w > 0.5) cols = saturate(cols + offs_.xyz / 255.0);\n"
	"      }\n"
	"    }\n"
	"  }\n"
	"  if (ccok < 0.5) sa = 63.0;\n"
	"  half sq = step(128.0, sa);\n"
	"  half sR = sa - 128.0 * sq;\n"
	"  half e = 0.5 / 255.0;\n"
	"  half m1 = max(max(max(K.x, K.y), max(K.z, K.w)), max(kr, ks));\n"
	"  half4 E1 = step(abs(K - m1), half4(e, e, e, e)) * step(half4(e, e, e, e), K);\n"
	"  half er1 = step(abs(kr - m1), e) * step(e, kr), es1 = step(abs(ks - m1), e) * step(e, ks), eb1 = 1.0 - step(e, m1);\n"
	"  half3 n1 = E1.x * c3.xyz + E1.y * c2.xyz + E1.z * c1.xyz + E1.w * c0.xyz + er1 * cr.xyz;\n"
	"  half4 o1 = E1.x * off3 + E1.y * off2 + E1.z * off1 + E1.w * off0 + er1 * offr;\n"
	"  if (o1.w > 0.5) n1 = saturate(n1 + o1.xyz / 255.0);\n"
	"  half3 t0c = n1 + es1 * cols + eb1 * back;\n"
	"  half t0r = dot(E1, R4) + er1 * rR + es1 * sR + eb1 * 63.0;\n"
	"  half t0q = dot(E1, Q4) + er1 * rQ + es1 * sq;\n"
	"  half t0s = es1 * shs;\n"
	"  half t0l = E1.x * layerLs.x + E1.y * layerLs.y + E1.z * layerLs.z + E1.w * layerLs.w + er1 * misc.w + es1 * sprE.x;\n"
	"  half m = mode.x;\n"
	"  if (t0l > 0.5) {\n"
	"    half lq = step(128.0, lL.w * 255.0 + 0.5);\n"
	"    t0c = blend(m, t0c, t0r, t0q, lL.xyz, lL.w * 255.0 - 128.0 * lq);\n"
	"    if (m < 0.5 || m > 1.5) { t0r = 63.0; t0q = 0.0; }\n"
	"  }\n"
	"  half3 r = t0c;\n"
	"  // ADR-051: the second layer only where the top one blends or is a shadow (most pixels skip it)\n"
	"  if (trans(m, t0r, t0q) > 0.5 || t0s > 0.5) {\n"
	"    half4 K2 = K * step(K, half4(m1 - e, m1 - e, m1 - e, m1 - e));\n"
	"    half m2 = max(max(max(K2.x, K2.y), max(K2.z, K2.w)), max(kr * step(kr, m1 - e), ks * step(ks, m1 - e)));\n"
	"    half4 E2 = step(abs(K - m2), half4(e, e, e, e)) * step(half4(e, e, e, e), K);\n"
	"    half er2 = step(abs(kr - m2), e) * step(e, kr), es2 = step(abs(ks - m2), e) * step(e, ks), eb2 = 1.0 - step(e, m2);\n"
	"    half3 n2 = E2.x * c3.xyz + E2.y * c2.xyz + E2.z * c1.xyz + E2.w * c0.xyz + er2 * cr.xyz;\n"
	"    half4 o2 = E2.x * off3 + E2.y * off2 + E2.z * off1 + E2.w * off0 + er2 * offr;\n"
	"    if (o2.w > 0.5) n2 = saturate(n2 + o2.xyz / 255.0);\n"
	"    half3 t1c = n2 + es2 * cols + eb2 * back;\n"
	"    half t1r = dot(E2, R4) + er2 * rR + es2 * sR + eb2 * 63.0;\n"
	"    half t1e = E2.x * layerShad.x + E2.y * layerShad.y + E2.z * layerShad.z + E2.w * layerShad.w + er2 * misc.z;\n"
	"    half3 sh = (t1e > 0.5) ? t1c * (124.0 / 255.0) : t1c;\n"
	"    r = (trans(m, t0r, t0q) > 0.5) ? blend(m, t0c, t0r, t0q, t1c, t1r) : t0c;\n"
	"    if (sprB.y > 0.5) {\n"
	"      half zero = (t0c.x + t0c.y + t0c.z < 0.5 / 255.0) ? 1.0 : 0.0;\n"
	"      half3 r2 = (mode.z < 0.5) ? r * (124.0 / 255.0) : r;\n"
	"      r = (t0s > 1.5) ? ((zero > 0.5) ? sh : r2) : r;\n"
	"    }\n"
	"    r = (t0s > 0.5 && t0s < 1.5) ? sh : r;\n"
	"  }\n"
	"  return float4(r, 1.0);\n"
	"}\n";

// ADR-161: s_fs_fast plus the per-layer sprite window test; made at init so s_fs_fast (its hash names the cached
// variants) and the variants of frames without a sprite window stay as before
static char *s_fs_fast_sw;
static char *make_fs_fast_sw(void)
{
	static const char sig_old[] = "uniform float4 mosaic, uniform float4 mosaicOn) : COLOR\n";
	static const char sig_new[] = "uniform float4 mosaic, uniform float4 mosaicOn,\n  uniform float4 layerSw) : COLOR\n";
	static const char at[] = "  if (misc.x > 0.5) cr = tex2D(tR0, vUV);\n";
	static const char test[] =
		"  if (layerSw.x + layerSw.y + layerSw.z + layerSw.w > 0.5) {\n"
		"    float4 sw = (sprE.z > 0.5) ? tex2D(tSpr, vUV) : tex2D(tSpr, float2((floor(x / 2.0) + 0.5) / 256.0, vUV.y));\n"
		"    half b = step(127.5 / 255.0, (sprE.z > 0.5 || fmod(x, 2.0) < 0.5) ? sw.y : sw.w);\n"
		"    half4 hd = step(abs(layerSw - 1.0), 0.5) * b + step(abs(layerSw - 2.0), 0.5) * (1.0 - b);\n"
		"    c3 *= 1.0 - hd.x; c2 *= 1.0 - hd.y; c1 *= 1.0 - hd.z; c0 *= 1.0 - hd.w;\n"
		"  }\n";
	const char *p1 = strstr(s_fs_fast, sig_old), *p2 = strstr(s_fs_fast, at);
	if (!p1 || !p2 || p2 < p1)
		return NULL;
	char *o = (char *)malloc(strlen(s_fs_fast) + sizeof(sig_new) + sizeof(test));
	if (!o)
		return NULL;
	size_t n = 0;
	memcpy(o + n, s_fs_fast, (size_t)(p1 - s_fs_fast)); n += (size_t)(p1 - s_fs_fast);
	memcpy(o + n, sig_new, sizeof(sig_new) - 1); n += sizeof(sig_new) - 1;
	const char *q = p1 + sizeof(sig_old) - 1;
	const size_t m = (size_t)(p2 - q) + sizeof(at) - 1;
	memcpy(o + n, q, m); n += m;
	memcpy(o + n, test, sizeof(test) - 1); n += sizeof(test) - 1;
	strcpy(o + n, p2 + sizeof(at) - 1);
	return o;
}

// ---- fast composite shader specialization (ADR-019) ----
// compiles a source where the uniform components that pick branches (enabled layers, shadow/line screen, color calc mode, sprite format ...) are
// replaced by constants of their current values. The shader compiler drops unused paths. Cached in the GXP cache (ux0:data/restv/shaders) by source hash, so
// a config seen once loads directly on the next run. Not used if ux0:data/restv/nocmpspec exists
typedef struct {
	char key[256];
	volatile int state; // 0 waiting to compile, 1 binary ready (worker thread), 2 usable, 3 failed
	char *src;
	const void *fbin;
	GxProg *prog;
	GxParam loc[U_COUNT];
	int tries;          // compile failure count (the compiler sometimes fails in game: retry later)
	unsigned retry_at;  // frame to retry at
	unsigned instr;     // GXP instruction count (header +60)
	int listed;         // confirmed in the per-game list this run
	volatile int need_save, need_list; // file writes for the worker thread (source cache, one line of per-game list); no card writes from main
	uint32_t list_hash;
	float vals[64], sw[4]; // ADR-185: a variant found in a submit gets its source built by the worker (src NULL until then)
	int has_sw;
} CmpVariant;
// sanjeon goes over 24 (ADR-042). At 96 the table filled with variants of several games and a new thuntk scene was composited with the
// generic shader (100 instructions) -> 256; preregistration only for entries in the per-game list (cl_<game>.txt) (ADR-064)
#define MAX_VARIANTS 256
static CmpVariant s_var[MAX_VARIANTS];
static int s_nvar, s_cmp_spec, s_lab_on;
static SceUID s_cmp_sema = -1;

static void rbg_compile_pending(void);
static void nbg_compile_pending(void);
static void variant_files(void);
static void cmp_variant_save(const char *key, const char *src);
static char *cmp_variant_src(const float *vals, const float *sw);
static void mig_scan(void);
static volatile int s_mig_pending;
// worker thread (CPU1, lowest priority): variant source -> GXP (just reads if cached)
static int cmp_worker(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (;;) {
		if (sceKernelWaitSema(s_cmp_sema, 1, NULL) < 0)
			break;
		if (s_mig_pending) {
			s_mig_pending = 0;
			mig_scan();
		}
		variant_files();
		for (int i = 0; i < s_nvar; i++) {
			CmpVariant *v = &s_var[i];
			if (v->state != 0)
				continue;
			if (!v->src) {
				v->src = cmp_variant_src(v->vals, v->has_sw ? v->sw : NULL);
				if (!v->src) {
					v->need_save = 0;
					v->state = 3;
					continue;
				}
				RSTV_LOG("gvdp2: compose variant %d source built, key %s", i + 1, v->key);
			}
			if (v->need_save) {
				cmp_variant_save(v->key, v->src);
				v->need_save = 0;
			}
			const u64 t0 = RSTV_PROF_T();
			v->fbin = gx_compile(v->src, 1);
			if (v->fbin)
				v->instr = *(const uint32_t *)((const uint8_t *)v->fbin + 60);
			{
				uint32_t h = 2166136261u;
				size_t ln = 0;
				for (const char *q = v->src; *q; q++, ln++)
					h = (h ^ (uint8_t)*q) * 16777619u;
				RSTV_LOG("gvdp2: compose variant %d compiled %s (%.0f ms) len %u fnv %08x", i + 1, v->fbin ? "ok" : "failed",
					(RSTV_PROF_T() - t0) / 1000.0, (unsigned)ln, (unsigned)h);
			}
			__sync_synchronize();
			if (!v->fbin)
				v->tries++;
			v->state = v->fbin ? 1 : 3;
		}
		rbg_compile_pending();
		nbg_compile_pending();
	}
	return 0;
}
static GxAttr s_pix_attr[2];
static const struct { int u; const char *name; const char *comps; } s_spec_list[] = {
	{ U_layerOn, "layerOn", "xyzw" }, { U_layerShad, "layerShad", "xyzw" }, { U_layerLs, "layerLs", "xyzw" },
	{ U_misc, "misc", "xzw" }, { U_mode, "mode", "xz" }, { U_sprA, "sprA", "xyzw" }, { U_sprB, "sprB", "xyzw" },
	{ U_sprC, "sprC", "xz" }, { U_sprD, "sprD", "xyzw" }, { U_sprE, "sprE", "xyz" },
	{ U_offB, "offB", "w" }, { U_off3, "off3", "w" }, { U_off2, "off2", "w" }, { U_off1, "off1", "w" },
	{ U_off0, "off0", "w" }, { U_offr, "offr", "w" }, { U_offs, "offs_", "w" },
	{ U_mosaicOn, "mosaicOn", "xyzw" },
};

static const float *uniform_values(const FrameSet *s, int u)
{
	switch (u) {
	case U_layerOn: return s->layerOn;
	case U_layerShad: return s->layerShad;
	case U_layerLs: return s->layerLs;
	case U_misc: return s->misc;
	case U_mode: return s->mode;
	case U_sprA: return s->sprA;
	case U_sprB: return s->sprB;
	case U_sprC: return s->sprC;
	case U_sprD: return s->sprD;
	case U_sprE: return s->sprE;
	case U_mosaicOn: return s->mosaicOn;
	default: return s->offu[u - U_offB];
	}
}

static int is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static uint32_t fnv_str(const char *p)
{
	uint32_t h = 2166136261u;
	for (; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	return h;
}

// remember seen variants and preregister them at the start of the next run (ADR-035: first registration in game held the GXM lock for tens of ms, stutter)
#define VARLIST_DIR "ux0:data/restv/shaders"
static void cmp_variant_save(const char *key, const char *src)
{
	char path[128];
	snprintf(path, sizeof(path), VARLIST_DIR "/cv_%08x_%08x.txt", (unsigned)fnv_str(s_fs_fast), (unsigned)fnv_str(key));
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "%s\n", key);
	fwrite(src, 1, strlen(src), f);
	fclose(f);
}

// ADR-064: per-game variant list (key hashes). Preregistration only for variants in this list
static char s_game[32];
static uint32_t s_cl_set[MAX_VARIANTS];
static int s_cl_n, s_cl_loaded;
void gvdp2_set_game(const char *game) { snprintf(s_game, sizeof(s_game), "%s", game ? game : ""); }
static int cl_has(uint32_t h)
{
	for (int i = 0; i < s_cl_n; i++)
		if (s_cl_set[i] == h)
			return 1;
	return 0;
}
static void cl_load(void)
{
	char path[128];
	snprintf(path, sizeof(path), VARLIST_DIR "/cl_%s.txt", s_game);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	unsigned h;
	while (s_cl_n < MAX_VARIANTS && fscanf(f, "%x", &h) == 1)
		if (!cl_has(h))
			s_cl_set[s_cl_n++] = h;
	fclose(f);
	s_cl_loaded = 1;
}
static void cl_note(CmpVariant *v)
{
	v->listed = 1;
	if (!s_game[0])
		return;
	const uint32_t h = fnv_str(v->key);
	if (cl_has(h) || s_cl_n >= MAX_VARIANTS)
		return;
	s_cl_set[s_cl_n++] = h;
	v->list_hash = h;
	__sync_synchronize();
	v->need_list = 1;
	sceKernelSignalSema(s_cmp_sema, 1);
}

// specialized source: replace "name.component" in s_fs_fast with value literals (vals in key order)
static int spec_pos(unsigned k, char comp)
{
	int p = 0;
	for (unsigned i = 0; i < k; i++)
		p += (int)strlen(s_spec_list[i].comps);
	return p + (int)(strchr(s_spec_list[k].comps, comp) - s_spec_list[k].comps);
}
static char *cmp_variant_src(const float *vals, const float *sw)
{
	const char *base = sw ? s_fs_fast_sw : s_fs_fast;
	if (!base)
		return NULL;
	const size_t n = strlen(base);
	char *src = (char *)malloc(n * 2 + 1024);
	if (!src)
		return NULL;
	size_t o = 0;
	for (size_t i = 0; i < n;) {
		int done = 0;
		if (!i || !is_ident(base[i - 1])) {
			for (unsigned k = 0; k < sizeof(s_spec_list) / sizeof(s_spec_list[0]) && !done; k++) {
				const size_t ln = strlen(s_spec_list[k].name);
				if (strncmp(base + i, s_spec_list[k].name, ln) || base[i + ln] != '.')
					continue;
				const char comp = base[i + ln + 1];
				if (!strchr(s_spec_list[k].comps, comp) || is_ident(base[i + ln + 2]))
					continue;
				char lit[32];
				snprintf(lit, sizeof(lit), "%.6f", (double)vals[spec_pos(k, comp)]);
				o += (size_t)sprintf(src + o, "(%s)", lit);
				i += ln + 2;
				done = 1;
			}
			if (!done && sw && !strncmp(base + i, "layerSw.", 8) && strchr("xyzw", base[i + 8]) && !is_ident(base[i + 9])) {
				o += (size_t)sprintf(src + o, "(%.6f)", (double)sw[base[i + 8] == 'w' ? 3 : base[i + 8] - 'x']);
				i += 9;
				done = 1;
			}
		}
		if (!done)
			src[o++] = base[i++];
	}
	src[o] = 0;
	return src;
}

// ADR-066 migration: worker thread reads keys from the cv files of the previous source hash, queues them and deletes the files (this game's list entries, or all if no list).
// main (cmp_variant) registers up to 4 per frame as new-source variants -> no startup time, no file work on main thread
#define MIG_MAX 256
static char s_mig_key[MIG_MAX][256];
static volatile int s_mig_n;  // count filled by worker thread
static int s_mig_taken;       // count taken by main
static void mig_scan(void)
{
	char pre_c[16];
	snprintf(pre_c, sizeof(pre_c), "cv_%08x_", (unsigned)fnv_str(s_fs_fast));
	SceUID d = sceIoDopen(VARLIST_DIR);
	if (d < 0)
		return;
	SceIoDirent e;
	int n = 0;
	while (sceIoDread(d, &e) > 0 && n < MIG_MAX) {
		if (strncmp(e.d_name, "cv_", 3) || !strncmp(e.d_name, pre_c, 12))
			continue;
		char path[160];
		snprintf(path, sizeof(path), VARLIST_DIR "/%s", e.d_name);
		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		const int ok = fgets(s_mig_key[n], sizeof(s_mig_key[0]), f) != NULL;
		fclose(f);
		if (!ok)
			continue;
		s_mig_key[n][strcspn(s_mig_key[n], "\r\n")] = 0;
		if (s_cl_loaded && !cl_has(fnv_str(s_mig_key[n])))
			continue; // another game's variant: migrated when that game runs
		sceIoRemove(path);
		__sync_synchronize();
		s_mig_n = ++n;
	}
	sceIoDclose(d);
	RSTV_LOG("gvdp2: %d compose variants to rebuild for the new shader", n);
}
// ADR-185: a new variant from the submit path: building the source (16 ms) and the queue log (20-70 ms) ran inside the
// GPU submit; the worker builds the source now (src stays NULL until then)
static void cmp_variant_defer(const char *key, const float *vals, const float *sw)
{
	CmpVariant *vv = &s_var[s_nvar];
	snprintf(vv->key, sizeof(vv->key), "%s", key);
	memcpy(vv->vals, vals, sizeof(vv->vals));
	vv->has_sw = sw != NULL;
	if (sw)
		memcpy(vv->sw, sw, sizeof(vv->sw));
	vv->src = NULL;
	vv->need_save = 1;
	vv->fbin = NULL;
	vv->prog = NULL;
	vv->state = 0;
	__sync_synchronize();
	s_nvar++;
	sceKernelSignalSema(s_cmp_sema, 1);
}

static void mig_take(void)
{
	for (int k = 0; k < 4 && s_mig_taken < s_mig_n && s_nvar < MAX_VARIANTS; k++) {
		const char *key = s_mig_key[s_mig_taken++];
		int dup = 0;
		for (int i = 0; i < s_nvar && !dup; i++)
			dup = !strcmp(s_var[i].key, key);
		if (dup)
			continue;
		float vals[64];
		int nv = 0;
		const char *q = key;
		while (*q && nv < 64) {
			char *end;
			vals[nv++] = strtof(q, &end);
			if (end == q || *end != ',')
				break;
			q = end + 1;
		}
		if (nv != spec_pos(sizeof(s_spec_list) / sizeof(s_spec_list[0]) - 1, 'w') + 1)
			continue; // different key format (build where the specialization list changed)
		float sw[4];
		const char *ks = strstr(key, "sw");
		const int has_sw = ks && sscanf(ks, "sw%f,%f,%f,%f,", &sw[0], &sw[1], &sw[2], &sw[3]) == 4;
		cmp_variant_defer(key, vals, has_sw ? sw : NULL);
	}
}

static GxProg *cmp_variant(const FrameSet *s, GxParam **loc)
{
	if (s_mig_taken < s_mig_n)
		mig_take();
	char key[256];
	float vals[64];
	int kn = 0, nv = 0;
	for (unsigned i = 0; i < sizeof(s_spec_list) / sizeof(s_spec_list[0]); i++) {
		const float *v = uniform_values(s, s_spec_list[i].u);
		for (const char *c = s_spec_list[i].comps; *c; c++) {
			const float x = v[*c == 'w' ? 3 : *c - 'x'];
			kn += snprintf(key + kn, sizeof(key) - kn, "%g,", (double)x);
			vals[nv++] = x;
		}
	}
	// ADR-161: the sprite window values only when a layer uses it (other keys and sources stay as before ADR-161)
	const float *sw = NULL;
	if (s->layerSw[0] + s->layerSw[1] + s->layerSw[2] + s->layerSw[3] > 0.5f) {
		sw = s->layerSw;
		kn += snprintf(key + kn, sizeof(key) - kn, "sw%g,%g,%g,%g,", (double)sw[0], (double)sw[1], (double)sw[2], (double)sw[3]);
	}
	for (int i = 0; i < s_nvar; i++) {
		CmpVariant *v = &s_var[i];
		if (strcmp(v->key, key))
			continue;
		if (v->state == 1) {
			// worker thread built the binary -> register program (main thread)
			const u64 tr = RSTV_PROF_T();
			v->prog = gx_prog_bin(gx_compile(s_vs_fast, 0), v->fbin, s_pix_attr, 2, 16, GX_BLEND_NONE);
			if (RSTV_PROF_T() - tr > 10000)
				RSTV_LOG("gvdp2: slow variant register %u us", (unsigned)(RSTV_PROF_T() - tr));
			if (v->prog)
				for (int k = 0; k < U_COUNT; k++)
					v->loc[k] = gx_param(v->prog, s_uname[k]);
			v->state = v->prog ? 2 : 3;
			free(v->src);
			v->src = NULL;
			RSTV_LOG("gvdp2: compose variant %d %s", i + 1, v->prog ? "ready" : "failed");
		}
		if (v->state == 3 && v->tries < 6 && v->src) {
			// failed: recompile after 120 frames
			if (!v->retry_at)
				v->retry_at = s_frame + 120;
			else if (s_frame >= v->retry_at) {
				v->retry_at = 0;
				v->state = 0;
				sceKernelSignalSema(s_cmp_sema, 1);
			}
		}
		if (v->state != 2)
			return NULL; // generic shader until ready
		if (!v->listed)
			cl_note(v);
		*loc = v->loc;
		return v->prog;
	}
	if (s_nvar >= MAX_VARIANTS || s_cmp_sema < 0)
		return NULL;
	cmp_variant_defer(key, vals, sw);
	return NULL;
}


// ---- stage 2: NBG layer shader (vidsoft Vdp2DrawScroll's MapCalcXY + PatternAddr + FetchPixel + GetAlpha) ----
// ---- cell quad path (ADR-014) ----
#define MAXQ 16384 // quad cap per layer (pixel shader if exceeded). 16384 x 4 vertices = u16 index limit (ADR-052)
static GxProg *s_prog_q, *s_prog_clear, *s_prog_qrgb;
static GxParam s_qloc[8], s_qrloc[8];
enum { QR_scale, QR_tRgb, QR_qcfg, QR_qcodeLo, QR_qcodeHi, QR_COUNT };
static const char *s_qrname[QR_COUNT] = { "scale", "tRgb", "qcfg", "qcodeLo", "qcodeHi" };
enum { Q_scale, Q_tAtlas, Q_tPal, Q_qcfg, Q_qcodeLo, Q_qcodeHi, Q_COUNT };
static const char *s_qname[Q_COUNT] = { "scale", "tAtlas", "tPal", "qcfg", "qcodeLo", "qcodeHi" };
static GxTex *s_atlas[2];       // 0 = 4bpp decode, 1 = 8bpp (1024x1024 L8)
// RBG0 rotation path (ADR-016)
static GxProg *s_prog_rbg;
enum { R_tTable, R_tPages, R_tAtlas, R_tRgb, R_tPal, R_rcfg0, R_rcfg1, R_qcodeLo, R_qcodeHi, R_rdbg, R_tVram, R_rcfg2, R_COUNT };
static const char *s_rname[R_COUNT] = { "tTable", "tPages", "tAtlas", "tRgb", "tPal", "rcfg0", "rcfg1", "qcodeLo", "qcodeHi", "rdbg",
	"tVram", "rcfg2" };
static GxParam s_rloc[R_COUNT];
// RBG0 specialized variants (ADR-034): key = off-screen | RGB << 1 | transparency << 2 | special color calc << 3 | ratio << 5
typedef struct {
	int key;
	volatile int state; // 0 waiting to compile, 1 binary ready, 2 usable, 3 failed
	char *src;
	const void *fbin;
	GxProg *prog;
	GxParam loc[R_COUNT];
	volatile int need_file; // worker thread creates the marker file
} RbgVariant;
static RbgVariant s_rvar[32];
static int rbg_flat_ok(const RstvRotCapture *rc);
static volatile int s_nrvar;
static int s_rbg_spec;
static GxAttr s_rbg_attr[2];
static char *rbg_variant_src(int key);
static void rbg_compile_pending(void)
{
	for (int i = 0; i < s_nrvar; i++) {
		RbgVariant *v = &s_rvar[i];
		if (v->state != 0)
			continue;
		if (!v->src && !(v->src = rbg_variant_src(v->key))) { // ADR-193: built here, not in the GPU submit
			v->state = 3;
			continue;
		}
		v->fbin = gx_compile(v->src, 1);
		RSTV_LOG("gvdp2: rbg variant %02x compiled %s", v->key, v->fbin ? "ok" : "failed");
		__sync_synchronize();
		v->state = v->fbin ? 1 : 3;
	}
}
static GxTex *s_rbg_pages, *s_argb;
static uint16_t s_argb_dirty[128]; // ADR-185: 256-byte VRAM blocks not yet in the RGB atlas
static int s_argb_pend;
// convert the pending blocks into the RGB atlas if frame s samples it (s = NULL: always). Prep worker (or main thread
// without the submit thread), from the snapshot the frame was prepared from
static void argb_flush(const FrameSet *s)
{
	if (s) {
		int need = s->gpu_layer[4] && s->rbg_cfg[0][3] >= 3.0f;
		for (int L = 0; L < 4 && !need; L++)
			need = s->quad_cn[L] == 3;
		if (!need) {
			// ADR-193: a few blocks a frame meanwhile, so the first frame that samples it does not convert everything
			// changed since boot at once (thuntk: 53 ms in one submit)
			static uint16_t part[128];
			int left = 32, any = 0;
			for (int p = 0; p < 128; p++) {
				part[p] = 0;
				for (int b = 0; b < 16 && left > 0; b++)
					if (s_argb_dirty[p] & (1u << b)) {
						part[p] |= (uint16_t)(1u << b);
						s_argb_dirty[p] &= (uint16_t)~(1u << b);
						left--;
					}
				any |= s_argb_dirty[p];
			}
			if (left < 32) {
				const u64 ta = RSTV_PROF_T();
				rbgq_atlas_rgb_update(s_argb->data, 1024, s_snap_vram, part);
				s_stats.t_atlas += RSTV_PROF_T() - ta;
			}
			if (!any)
				s_argb_pend = 0;
			return;
		}
	}
	const u64 ta = RSTV_PROF_T();
	rbgq_atlas_rgb_update(s_argb->data, 1024, s_snap_vram, s_argb_dirty);
	memset(s_argb_dirty, 0, sizeof(s_argb_dirty));
	s_argb_pend = 0;
	s_stats.t_atlas += RSTV_PROF_T() - ta;
}
static RbgqState s_rbgq;
static GxTex *s_rbg_pages_b;  // ADR-067: map-order cell layout of the B map when there are two maps
// ADR-077: draw RBG0 at half horizontal resolution (256 x 256, render target slot 20) and stretch-copy into the layer texture (half the rotation shader pixels)
int rstv_rbg_half; // 0: game table (RSTV_HACK_RBG_HALF), 1: on (ux0:data/restv/rbghalf), 2: off (norbghalf)
static GxTex *s_rbg_halftex;
static RbgqState s_rbgq_b;
static int s_rbg_dbg; // test: ux0:data/restv/rbgdbg (1 constant, 2 up to page table, 3 up to cell attrs)
static uint8_t *s_atlas_px[2];
static uint32_t s_atlas_copied; // rstv_vdp2_serial when the atlas was updated (0 = not yet)
static uint16_t *s_qidx;        // GPU memory (quad indices, fixed)
#define PLQ (MAXQ * 2) // ADR-100: plane cell quads (a 2048-wide plane), drawn in MAXQ chunks (u16 index limit)
static NbgQuad s_qtmp[PLQ];
static int16_t s_qvtx[PLQ * 4 * 8]; // vertex staging buffer (cached memory, copied to GPU temp memory in one go when done)

static int file_exists_q(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f)
		fclose(f);
	return f != NULL;
}

static GxProg *s_prog_nbg;
static GxParam s_nloc[16];
// NBG pixel path specialized variants (ADR-038): key = color count | transparency << 3 | pds2 << 4 | pwh2 << 5 | aux << 6 | 3FFF << 7 | scm << 8 | ratio << 10
typedef struct {
	int key;
	volatile int state; // 0 waiting to compile, 1 binary ready, 2 usable, 3 failed
	char *src;
	const void *fbin;
	GxProg *prog;
	GxParam loc[16];
	volatile int need_file; // worker thread creates the marker file
} NbgVariant;
static NbgVariant s_nvar_tab[24];
static volatile int s_nnvar;
static int s_nbg_spec;
static char *nbg_variant_src_alloc(int key);
static void nbg_compile_pending(void)
{
	for (int i = 0; i < s_nnvar; i++) {
		NbgVariant *v = &s_nvar_tab[i];
		if (v->state != 0)
			continue;
		if (!v->src && !(v->src = nbg_variant_src_alloc(v->key))) { // ADR-193: built here, not in the GPU submit
			v->state = 3;
			continue;
		}
		v->fbin = gx_compile(v->src, 1);
		RSTV_LOG("gvdp2: nbg variant %03x compiled %s", v->key, v->fbin ? "ok" : "failed");
		__sync_synchronize();
		v->state = v->fbin ? 1 : 3;
	}
}
enum { N_tVram, N_tPal, N_tRows, N_cfgA, N_cfgB, N_cfgC, N_cfgD, N_cfgE, N_cfgF, N_cfgG, N_cfgH, N_cfgI, N_tBmp, N_COUNT };
static const char *s_nname[N_COUNT] = { "tVram", "tPal", "tRows", "cfgA", "cfgB", "cfgC", "cfgD", "cfgE", "cfgF",
	"cfgG", "cfgH", "cfgI", "tBmp" };

// ADR-038: config components are NBG_* macros. Generic version uses the definitions below (uniforms), variants prepend constant #defines
static const char *s_fs_nbg_gen =
	"#define NBG_CN (cfgA.x)\n"
	"#define NBG_TR (cfgA.y)\n"
	"#define NBG_PDS (cfgA.z)\n"
	"#define NBG_PWH (cfgA.w)\n"
	"#define NBG_AUX (cfgB.x)\n"
	"#define NBG_M3FFF (cfgB.y)\n"
	"#define NBG_SCM (cfgE.y)\n"
	"#define NBG_ALPHA (cfgB.w)\n"
	"#define NBG_BMP (cfgG.w)\n"
	"#define NBG_BTEX (cfgG.w - 1.0)\n"
	;
static const char *s_fs_nbg =
	"float4 lout(float3 c, float prio, float flag, float idx) { return (prio < 0.5) ? float4(0.0, 0.0, 0.0, 0.0) : float4(c, (prio * 8.0 + idx + ((flag > 0.5) ? 128.0 : 0.0)) / 255.0); }\n"
	"float4 fetch(sampler2D t, float2 p, float2 sz) { return floor(tex2D(t, (p + 0.5) / sz) * 255.0 + 0.5); }\n"
	"float4 vt(sampler2D v, float a) { float ti = floor(a / 4.0); return fetch(v, float2(fmod(ti, 512.0), floor(ti / 512.0)), float2(512.0, 256.0)); }\n"
	"float pick(float4 t, float c) { return dot(t, step(abs(float4(c, c, c, c) - float4(0.0, 1.0, 2.0, 3.0)), float4(0.5, 0.5, 0.5, 0.5))); }\n"
	"float rd8(sampler2D v, float a) { a = fmod(a, 524288.0); return pick(vt(v, a), fmod(a, 4.0)); }\n"
	"float rd16(sampler2D v, float a) { a = fmod(a, 524288.0); float4 t = vt(v, a); return (fmod(a, 4.0) < 1.0) ? (t.x * 256.0 + t.y) : (t.z * 256.0 + t.w); }\n"
	"float bits(float v, float d, float m) { return fmod(floor(v / d), m); }\n"
	"float lut8(float i, float4 lo, float4 hi) {\n"
	"  float4 a = step(abs(float4(i, i, i, i) - float4(0.0, 1.0, 2.0, 3.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  float4 b = step(abs(float4(i, i, i, i) - float4(4.0, 5.0, 6.0, 7.0)), float4(0.5, 0.5, 0.5, 0.5));\n"
	"  return dot(a, lo) + dot(b, hi);\n"
	"}\n"
	"float4 nbg_tail(float3 col, float dotv, float msb, float prio, float scf, float scm, float alpha, float4 h, float4 h2, float idx) {\n"
	"  if (prio < 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"  float use = 1.0;\n"
	"  if ((scm > 0.5 && scm < 2.5) && scf < 0.5) use = 0.0;\n"
	"  else if (scm > 1.5 && scm < 2.5) { if (lut8(floor(fmod(dotv, 16.0) / 2.0), h, h2) < 0.5) use = 0.0; }\n"
	"  else if (scm > 2.5 && msb < 127.5) use = 0.0;\n"
	"  float flag = (use > 0.5 && abs(alpha - 63.0) > 0.5) ? 1.0 : 0.0;\n"
	"  return lout(col / 255.0, prio, flag, idx);\n"
	"}\n"
	"float4 main(float2 vPix : TEXCOORD0, uniform sampler2D tVram, uniform sampler2D tPal, uniform sampler2D tRows,\n"
	"  uniform sampler2D tBmp, uniform float4 cfgA, uniform float4 cfgB, uniform float4 cfgC, uniform float4 cfgD, uniform float4 cfgE,\n"
	"  uniform float4 cfgF, uniform float4 cfgG, uniform float4 cfgH, uniform float4 cfgI) : COLOR\n"
	"{\n"
	"  float i = floor(vPix.x);\n"
	"  float yrow = floor(vPix.y);\n"
	"  float2 rsz = float2(64.0, 256.0);\n"
	"  float base = cfgE.z;\n"
	"  float4 r0 = fetch(tRows, float2(base + 0.0, yrow), rsz);\n"
	"  float4 r1 = fetch(tRows, float2(base + 1.0, yrow), rsz);\n"
	"  if (r1.z < 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"  float xo = r0.x + r0.y * 256.0;\n"
	"  float lsx = r0.z + r0.w * 256.0;\n"
	"  float yy = r1.x + r1.y * 256.0;\n"
	"  float pr = fmod(r1.w, 16.0);\n"
	"  float spm = floor(r1.w / 16.0);\n"
	"  float4 rz = fetch(tRows, float2(base + 6.0, yrow), rsz);\n"
	"  float xx = fmod(xo + floor(i * ((rz.x + rz.y * 256.0 + rz.z * 65536.0) * (1.0 / 65536.0))), cfgC.w);\n"
	"  if (lsx > 0.5) xx = fmod(xx + lsx, cfgC.w);\n"
	"  if (NBG_BMP > 0.5) {\n"
	"    if (NBG_BTEX > 0.5) {\n"
	"      // ADR-123: the bitmap converted on the CPU (alpha = opaque 128 | palette MSB 64 | dot & 15)\n"
	"      float4 bt = floor(tex2D(tBmp, (float2(xx, yy) + 0.5) / float2(1024.0, 512.0)) * 255.0 + 0.5);\n"
	"      if (bt.w < 127.5 && NBG_TR > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"      float bl = fmod(bt.w, 128.0);\n"
	"      return nbg_tail(bt.xyz, fmod(bl, 16.0), (bl > 63.5) ? 255.0 : 0.0, pr, cfgF.w, NBG_SCM, NBG_ALPHA, cfgH, cfgI, cfgG.z);\n"
	"    }\n"
	"    // bitmap (ADR-039): cfgF = (VRAM address, palette address, 0, special colour function), width = cfgC.w\n"
	"    float bidx = yy * cfgC.w + xx;\n"
	"    float cnb = NBG_CN;\n"
	"    float3 colb = float3(0.0, 0.0, 0.0); float dotb; float msbb = 0.0;\n"
	"    if (cnb < 0.5) {\n"
	"      dotb = rd8(tVram, cfgF.x + floor(bidx * 0.5));\n"
	"      if (fmod(xx, 2.0) < 0.5) dotb = floor(dotb / 16.0);\n"
	"      dotb = fmod(dotb, 16.0);\n"
	"      if (dotb < 0.5 && NBG_TR > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"      float4 pc = fetch(tPal, float2(fmod(cfgB.z + cfgF.y + dotb, 2048.0), 0.0), float2(2048.0, 1.0));\n"
	"      colb = pc.xyz; msbb = pc.w;\n"
	"    } else if (cnb < 1.5) {\n"
	"      dotb = rd8(tVram, cfgF.x + bidx);\n"
	"      if (dotb < 0.5 && NBG_TR > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"      float4 pc = fetch(tPal, float2(fmod(cfgB.z + cfgF.y + dotb, 2048.0), 0.0), float2(2048.0, 1.0));\n"
	"      colb = pc.xyz; msbb = pc.w;\n"
	"    } else if (cnb < 2.5) {\n"
	"      dotb = rd16(tVram, cfgF.x + bidx * 2.0);\n"
	"      if (dotb < 0.5 && NBG_TR > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"      float4 pc = fetch(tPal, float2(fmod(cfgB.z + dotb, 2048.0), 0.0), float2(2048.0, 1.0));\n"
	"      colb = pc.xyz; msbb = pc.w;\n"
	"    } else if (cnb < 3.5) {\n"
	"      dotb = rd16(tVram, cfgF.x + bidx * 2.0);\n"
	"      if (dotb < 32768.0 && NBG_TR > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"      colb = float3(fmod(dotb, 32.0), bits(dotb, 32.0, 32.0), bits(dotb, 1024.0, 32.0)) * 8.0;\n"
	"    } else {\n"
	"      float hi = rd16(tVram, cfgF.x + bidx * 4.0);\n"
	"      float lo = rd16(tVram, cfgF.x + bidx * 4.0 + 2.0);\n"
	"      dotb = lo;\n"
	"      if (hi < 32768.0 && NBG_TR > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"      colb = float3(fmod(lo, 256.0), floor(lo / 256.0), fmod(hi, 256.0));\n"
	"    }\n"
	"    return nbg_tail(colb, dotb, msbb, pr, cfgF.w, NBG_SCM, NBG_ALPHA, cfgH, cfgI, cfgG.z);\n"
	"  }\n"
	"  float pn = floor(yy / cfgC.y) * cfgC.z + floor(xx / cfgC.x);\n"
	"  float lx = fmod(xx, cfgC.x);\n"
	"  float ly = fmod(yy, cfgC.y);\n"
	"  float4 pt4 = fetch(tRows, float2(base + 2.0 + pn, yrow), rsz);\n"
	"  float pt = pt4.x + pt4.y * 256.0 + pt4.z * 65536.0;\n"
	"  float addr = pt + (floor(ly / 512.0) * cfgD.y * cfgD.z + floor(lx / 512.0) * cfgD.y\n"
	"             + floor(fmod(ly, 512.0) / cfgE.x) * cfgD.x + floor(fmod(lx, 512.0) / cfgE.x)) * cfgD.w;\n"
	"  float cn = NBG_CN;\n"
	"  float charaddr, paladdr, flip, sf, scf;\n"
	"  if (NBG_PDS < 1.5) {\n"
	"    float tmp = rd16(tVram, addr);\n"
	"    sf = cfgG.x; scf = cfgG.y;\n"
	"    paladdr = (cn < 0.5) ? (bits(tmp, 4096.0, 16.0) * 16.0 + cfgE.w) : (bits(tmp, 4096.0, 8.0) * 256.0);\n"
	"    if (NBG_AUX < 0.5) {\n"
	"      flip = bits(tmp, 1024.0, 4.0);\n"
	"      charaddr = (NBG_PWH < 1.5) ? (fmod(tmp, 1024.0) + cfgF.x) : (fmod(tmp, 1024.0) * 4.0 + cfgF.y);\n"
	"    } else {\n"
	"      flip = 0.0;\n"
	"      charaddr = (NBG_PWH < 1.5) ? (fmod(tmp, 4096.0) + cfgF.z) : (fmod(tmp, 4096.0) * 4.0 + cfgF.w);\n"
	"    }\n"
	"  } else {\n"
	"    float t1 = rd16(tVram, addr);\n"
	"    float t2 = rd16(tVram, addr + 2.0);\n"
	"    charaddr = fmod(t2, 32768.0);\n"
	"    flip = floor(t1 / 16384.0);\n"
	"    paladdr = (cn < 0.5) ? (fmod(t1, 128.0) * 16.0) : (bits(t1, 16.0, 8.0) * 256.0);\n"
	"    sf = bits(t1, 8192.0, 2.0);\n"
	"    scf = bits(t1, 4096.0, 2.0);\n"
	"  }\n"
	"  if (NBG_M3FFF > 0.5) charaddr = fmod(charaddr, 16384.0);\n"
	"  charaddr = charaddr * 32.0;\n"
	"  float prio = (spm > 0.5 && spm < 1.5) ? (floor(pr / 2.0) * 2.0 + sf) : pr;\n"
	"  float cx, cy;\n"
	"  if (NBG_PWH < 1.5) {\n"
	"    cx = fmod(lx, 8.0); cy = fmod(ly, 8.0);\n"
	"    if (fmod(flip, 2.0) > 0.5) cx = 7.0 - cx;\n"
	"    if (flip > 1.5) cy = 7.0 - cy;\n"
	"  } else {\n"
	"    float y16 = fmod(ly, 16.0);\n"
	"    float x16 = fmod(lx, 16.0);\n"
	"    if (flip > 0.5) {\n"
	"      if (flip > 1.5) y16 = (y16 < 8.0) ? (7.0 - y16 + 16.0) : (15.0 - y16);\n"
	"      else if (y16 >= 8.0) y16 += 8.0;\n"
	"      if (fmod(flip, 2.0) > 0.5) { if (x16 < 8.0) y16 += 8.0; cx = 7.0 - fmod(x16, 8.0); }\n"
	"      else if (x16 >= 8.0) { y16 += 8.0; cx = fmod(x16, 8.0); }\n"
	"      else cx = fmod(x16, 8.0);\n"
	"    } else {\n"
	"      if (y16 >= 8.0) y16 += 8.0;\n"
	"      if (x16 >= 8.0) y16 += 8.0;\n"
	"      cx = fmod(x16, 8.0);\n"
	"    }\n"
	"    cy = y16;\n"
	"  }\n"
	"  float off = cy * 8.0 + cx;\n"
	"  float dotv; float3 col; float msb = 0.0;\n"
	"  float transp = NBG_TR;\n"
	"  if (cn < 0.5) {\n"
	"    dotv = rd8(tVram, charaddr + floor(off / 2.0));\n"
	"    if (fmod(cx, 2.0) < 0.5) dotv = floor(dotv / 16.0);\n"
	"    if (fmod(dotv, 16.0) < 0.5 && transp > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"    float4 pc = fetch(tPal, float2(fmod(cfgB.z + paladdr + fmod(dotv, 16.0), 2048.0), 0.0), float2(2048.0, 1.0));\n"
	"    col = pc.xyz; msb = pc.w;\n"
	"  } else if (cn < 1.5) {\n"
	"    dotv = rd8(tVram, charaddr + off);\n"
	"    if (dotv < 0.5 && transp > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"    float4 pc = fetch(tPal, float2(fmod(cfgB.z + paladdr + dotv, 2048.0), 0.0), float2(2048.0, 1.0));\n"
	"    col = pc.xyz; msb = pc.w;\n"
	"  } else if (cn < 2.5) {\n"
	"    dotv = rd16(tVram, charaddr + off * 2.0);\n"
	"    if (dotv < 0.5 && transp > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"    float4 pc = fetch(tPal, float2(fmod(cfgB.z + dotv, 2048.0), 0.0), float2(2048.0, 1.0));\n"
	"    col = pc.xyz; msb = pc.w;\n"
	"  } else if (cn < 3.5) {\n"
	"    dotv = rd16(tVram, charaddr + off * 2.0);\n"
	"    if (dotv < 32768.0 && transp > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"    col = float3(fmod(dotv, 32.0), bits(dotv, 32.0, 32.0), bits(dotv, 1024.0, 32.0)) * 8.0;\n"
	"  } else {\n"
	"    float hi = rd16(tVram, charaddr + off * 4.0);\n"
	"    float lo = rd16(tVram, charaddr + off * 4.0 + 2.0);\n"
	"    dotv = lo;\n"
	"    if (hi < 32768.0 && transp > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"    col = float3(fmod(lo, 256.0), floor(lo / 256.0), fmod(hi, 256.0));\n"
	"  }\n"
	"  if (prio < 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
	"  float scm = NBG_SCM;\n"
	"  float use = 1.0;\n"
	"  if ((scm > 0.5 && scm < 2.5) && scf < 0.5) use = 0.0;\n"
	"  else if (scm > 1.5 && scm < 2.5) { if (lut8(floor(fmod(dotv, 16.0) / 2.0), cfgH, cfgI) < 0.5) use = 0.0; }\n"
	"  else if (scm > 2.5 && msb < 127.5) use = 0.0;\n"
	"  float flag = (use > 0.5 && abs(NBG_ALPHA - 63.0) > 0.5) ? 1.0 : 0.0;\n"
	"  return lout(col / 255.0, prio, flag, cfgG.z);\n"
	"}\n";

static unsigned s_gpu_finish_from; // frame to enable gpufinish at (0 = from the start)
static int s_gpu_finish; // instrumentation: measure GPU time with gx_finish after composite (ux0:data/restv/gpufinish)
static int s_gpu_floor;  // experiment: minimal shader work (all NBG lines off, composite layers and sprites off) -> measure fixed cost

// clear layer texture (quad path): full NDC quad with transparent 0
static const char *s_vs_clear =
	"void main(float2 position, float4 out vPosition : POSITION)\n"
	"{\n"
	"  vPosition = float4(position, 0.0, 1.0);\n"
	"}\n";
static const char *s_fs_clear =
	"float4 main() : COLOR\n"
	"{\n"
	"  return float4(0.0, 0.0, 0.0, 0.0);\n"
	"}\n";

// ---- frame prep (CPU) ----------------------------------------------------

static inline void put_off(uint8_t *lo, uint8_t *hi, int en, int r, int g, int b)
{
	lo[0] = (uint8_t)((r + 256) & 0xFF);
	lo[1] = (uint8_t)((g + 256) & 0xFF);
	lo[2] = (uint8_t)((b + 256) & 0xFF);
	lo[3] = en ? 255 : 0;
	hi[0] = (uint8_t)((r + 256) >> 8);
	hi[1] = (uint8_t)((g + 256) >> 8);
	hi[2] = (uint8_t)((b + 256) >> 8);
	hi[3] = 0;
}

static inline int sext9(int v) { return (v & 0x100) ? (v | ~0xFF) : (v & 0xFF); }

// same rules as ReadVdp2ColorOffset
static void color_offset(const Vdp2 *regs, int mask, int *en, int *r, int *g, int *b)
{
	*en = (regs->CLOFEN & mask) != 0;
	if (!*en) {
		*r = *g = *b = 0;
		return;
	}
	if (regs->CLOFSL & mask) {
		*r = sext9(regs->COBR); *g = sext9(regs->COBG); *b = sext9(regs->COBB);
	} else {
		*r = sext9(regs->COAR); *g = sext9(regs->COAG); *b = sext9(regs->COAB);
	}
}

static inline void rgb555(uint8_t *o, u16 v)
{
	o[0] = (uint8_t)((v & 0x1F) << 3);
	o[1] = (uint8_t)(((v >> 5) & 0x1F) << 3);
	o[2] = (uint8_t)(((v >> 10) & 0x1F) << 3);
}

// can this frame be composited on the GPU (otherwise a nonzero reason number)
// ADR-100: sprite layer window made of one rectangle window, W0 or W1 alone (ADR-112: W1 too, myfairld).
// ADR-117: a line window too (winterht), its x span per line comes from the VRAM table (sprite_window_lwta).
// n = the window, 0 if it is not that shape
static int sprite_window_one(const Vdp2 *r)
{
	const int w = (r->WCTLC >> 8) & 0x0a; // the sprite window bit does not apply to the sprite layer (see unsupported_reason)
	if (w == 0x02)
		return 1;
	if (w == 0x08)
		return 2;
	return 0;
}
// ADR-117: window n's line window table, VRAM byte address + 1 (0 = no line window)
static u32 sprite_window_lwta(const Vdp2 *r, int n)
{
	const u32 a = n == 2 ? r->LWTA1.all : n == 1 ? r->LWTA0.all : 0;
	return (a & 0x80000000u) ? ((a & 0x7FFFE) << 1) + 1 : 0;
}
// that window as a rectangle in screen pixels (Yabause ReadWindowCoordinates, half size screens included)
static int sprite_window_rect(const Vdp2 *r, int *x0, int *y0, int *x1, int *y1, int *in)
{
	const int n = sprite_window_one(r);
	if (!n)
		return 0;
	const int w1 = n == 2;
	int xs = w1 ? r->WPSX1 : r->WPSX0, xe = w1 ? r->WPEX1 : r->WPEX0;
	switch ((r->TVMD >> 1) & 3) {
	case 0: xs = (xs >> 1) & 0x1FF; xe = (xe >> 1) & 0x1FF; break;
	case 1: xs &= 0x3FF; xe &= 0x3FF; break;
	case 2: xs &= 0x1FF; xe &= 0x1FF; break;
	default: xs = (xs & 0x3FF) >> 1; xe = (xe & 0x3FF) >> 1; break;
	}
	int ys = (w1 ? r->WPSY1 : r->WPSY0) & 0x1FF, ye = (w1 ? r->WPEY1 : r->WPEY0) & 0x1FF;
	extern int rstv_half_x, rstv_half_y;
	if (rstv_half_x) { xs >>= 1; xe >>= 1; }
	if (rstv_half_y) { ys >>= 1; ye >>= 1; }
	*x0 = xs; *x1 = xe; *y0 = ys; *y1 = ye;
	*in = ((r->WCTLC >> 8) >> (w1 ? 2 : 0)) & 1;
	return 1;
}
// a visible-inside window covering the whole screen: no effect (twcup98 W0, myfairld W1)
static int sprite_window_whole(const Vdp2 *r)
{
	int x0, y0, x1, y1, in;
	extern int vdp2width, vdp2height;
	if (!sprite_window_rect(r, &x0, &y0, &x1, &y1, &in) || !in || sprite_window_lwta(r, sprite_window_one(r)))
		return 0;
	return x0 <= 0 && y0 <= 0 && x1 >= vdp2width - 1 && y1 >= vdp2height - 2;
}

static int unsupported_reason(int vdp1w, int vdp1ps)
{
	const Vdp2 *r = Vdp2Regs;
	if (vdp2_interlace) return 1;
	if (vdp2_x_hires || vdp2width > LAYER_W || vdp2height > LAYER_H) return 2;
	// ADR-100: the 8-bit 1024-wide framebuffer (hi-res VDP1) is read at half width like vidsoft does for a low-res screen
	if (!(vdp1ps == 2 && vdp1w == 512) && !(vdp1ps == 1 && vdp1w == 1024)) return 3;
	if (Vdp1Regs->TVMR & 2) return 4;                       // VDP1 rotation framebuffer
	int wx0, wy0, wx1, wy1, win;
	// the sprite layer's own sprite window bit (0x20) is not applied to it (vidsoft TestBothWindow uses W0/W1 only;
	// kiwames sets WCTLC 0x2000 in play and fell to the CPU)
	if (((r->WCTLC >> 8) & 0x0a) && !sprite_window_whole(r) && !sprite_window_rect(r, &wx0, &wy0, &wx1, &wy1, &win)) {
		// sprite layer window (window 0 covering the full screen is ignored, rectangle window 0 is cleared from the sprite texture)
		static u16 lw[6];
		const u16 cw[6] = { r->WCTLC, r->WCTLD, r->WPSX0, r->WPEX0, r->WPSY0, r->WPEY0 };
		if (memcmp(lw, cw, sizeof(cw))) { // diag: only on change
			memcpy(lw, cw, sizeof(cw));
			RSTV_LOG("gvdp2: sprite window WCTLC %04x WCTLD %04x W0 %d..%d x %d..%d W1 %d..%d x %d..%d LWTA0 %08x", r->WCTLC, r->WCTLD,
				r->WPSX0, r->WPEX0, r->WPSY0, r->WPEY0, r->WPSX1, r->WPEX1, r->WPSY1, r->WPEY1, (unsigned)r->LWTA0.all);
		}
		return 6;
	}
	if ((r->WCTLD >> 8) & 0x20) return 7;                    // color calc window (sprite window; rectangles in shader, ADR-112)
	if ((r->LNCLEN & 0x10) && (r->KTCTL & 0x1010)) return 8; // RBG line screen 2/3
	return 0;
}

// ADR-112: the colour calculation window as vidsoft uses it (ReadWindowData: rectangles W0/W1, no line window; the sprite
// window is not supported here, unsupported_reason sends those frames to the CPU). Where the test fails the pixel gets no
// colour calculation (vidsoft: alpha 0x3F)
static void cc_window_rect(const Vdp2 *r, int n, float *q)
{
	int xs = n ? r->WPSX1 : r->WPSX0, xe = n ? r->WPEX1 : r->WPEX0;
	switch ((r->TVMD >> 1) & 3) {
	case 0: xs = (xs >> 1) & 0x1FF; xe = (xe >> 1) & 0x1FF; break;
	case 1: xs &= 0x3FF; xe &= 0x3FF; break;
	case 2: xs &= 0x1FF; xe &= 0x1FF; break;
	default: xs = (xs & 0x3FF) >> 1; xe = (xe & 0x3FF) >> 1; break;
	}
	int ys = (n ? r->WPSY1 : r->WPSY0) & 0x1FF, ye = (n ? r->WPEY1 : r->WPEY0) & 0x1FF;
	extern int rstv_half_x, rstv_half_y;
	if (rstv_half_x) { xs >>= 1; xe >>= 1; }
	if (rstv_half_y) { ys >>= 1; ye >>= 1; }
	q[0] = (float)xs; q[1] = (float)ys; q[2] = (float)xe; q[3] = (float)ye;
}
static void cc_window(const Vdp2 *r, FrameSet *s)
{
	const int w = r->WCTLD >> 8;
	memset(s->ccwA, 0, sizeof(s->ccwA));
	memset(s->ccwB, 0, sizeof(s->ccwB));
	if (!(w & 0x2a))
		return;
	cc_window_rect(r, 0, s->ccw0);
	cc_window_rect(r, 1, s->ccw1);
	s->ccwA[0] = 1.0f;
	s->ccwA[1] = (w & 0x02) ? 1.0f : 0.0f; // W0 on
	s->ccwA[2] = (w & 0x01) ? 1.0f : 0.0f; // W0 inside
	s->ccwA[3] = (w & 0x04) ? 1.0f : 0.0f; // W1 inside
	s->ccwB[0] = (w & 0x08) ? 1.0f : 0.0f; // W1 on
	s->ccwB[1] = (w & 0x80) ? 1.0f : 0.0f; // AND logic
	extern int vdp2height;
	s->ccwB[2] = s->ccw0[3] > (float)vdp2height ? 1.0f : 0.0f; // "overflows vertically" (vidsoft TestWindow, outside area)
	s->ccwB[3] = s->ccw1[3] > (float)vdp2height ? 1.0f : 0.0f;
}

static float layer_alpha(const Vdp2 *regs, int ccbit, int ccr)
{
	// info.alpha of Vdp2DrawNBGx
	int a;
	if (regs->CCCTL & (0x200 | ccbit))
		a = ((~ccr & 0x1F) << 1) + 1;
	else
		a = 0x3F;
	if ((regs->CCCTL & (0x200 | ccbit)) == (0x200 | ccbit)) a |= 0x80;
	else if ((regs->CCCTL & (0x100 | ccbit)) == (0x100 | ccbit)) a |= 0x80;
	return (float)a;
}

static void fill_sprite_params(FrameSet *s)
{
	const Vdp2 *r = &s_snap_regs;
	static const struct { int ps, pn, cs, cn, colbits, msb; } T[16] = {
		{ 14, 2, 11, 3, 11, 0 }, { 13, 3, 11, 2, 11, 0 }, { 14, 1, 11, 3, 11, 1 }, { 13, 2, 11, 2, 11, 1 },
		{ 13, 2, 10, 3, 10, 1 }, { 12, 3, 11, 1, 11, 1 }, { 12, 3, 10, 2, 10, 1 }, { 12, 3, 9, 3, 9, 1 },
		{ 7, 1, 0, 0, 7, 0 }, { 7, 1, 6, 1, 6, 0 }, { 6, 2, 0, 0, 6, 0 }, { 0, 0, 6, 2, 6, 0 },
		{ 7, 1, 0, 0, 8, 0 }, { 7, 1, 6, 1, 8, 0 }, { 6, 2, 0, 0, 8, 0 }, { 0, 0, 6, 2, 8, 0 },
	};
	const int type = r->SPCTL & 0xF;
	s->sprA[0] = (float)(1 << T[type].ps);
	s->sprA[1] = (float)(1 << T[type].pn);
	s->sprA[2] = (float)(1 << T[type].cs);
	s->sprA[3] = (float)(1 << T[type].cn);
	s->sprB[0] = (float)(1 << T[type].colbits);
	s->sprB[1] = (float)T[type].msb;
	s->sprB[2] = (r->SPCTL & 0x20) ? 1.0f : 0.0f;
	s->sprB[3] = (Vdp1External.disptoggle && (r->TVMD & 0x8000)) ? 1.0f : 0.0f;
	s->sprC[0] = (float)((r->SPCTL >> 12) & 0x3);
	s->sprC[1] = (float)((r->SPCTL >> 8) & 0x7);
	s->sprC[2] = (r->CCCTL & 0x40) ? 1.0f : 0.0f;
	s->sprC[3] = (float)((r->CRAOFB & 0x70) << 4);
	s->sprD[0] = (float)type;
	s->sprD[1] = (r->CCCTL & 0x300) ? 1.0f : 0.0f;
	s->sprD[2] = (r->CCCTL & 0x200) ? 1.0f : 0.0f;
	s->sprD[3] = (r->CCCTL & 0x100) ? 1.0f : 0.0f;
	s->sprE[0] = (float)((r->LNCLEN >> 5) & 1);
	s->sprE[1] = (r->SPCTL & 0x10) ? 1.0f : 0.0f;
	s->sprE[2] = s->sprE[3] = 0;
	const int pt[8] = { r->PRISA & 7, (r->PRISA >> 8) & 7, r->PRISB & 7, (r->PRISB >> 8) & 7,
		r->PRISC & 7, (r->PRISC >> 8) & 7, r->PRISD & 7, (r->PRISD >> 8) & 7 };
	const int ct[8] = { ((~r->CCRSA & 0x1F) << 1) + 1, ((~r->CCRSA >> 7) & 0x3E) + 1,
		((~r->CCRSB & 0x1F) << 1) + 1, ((~r->CCRSB >> 7) & 0x3E) + 1,
		((~r->CCRSC & 0x1F) << 1) + 1, ((~r->CCRSC >> 7) & 0x3E) + 1,
		((~r->CCRSD & 0x1F) << 1) + 1, ((~r->CCRSD >> 7) & 0x3E) + 1 };
	for (int i = 0; i < 4; i++) {
		s->prioLo[i] = (float)pt[i];
		s->prioHi[i] = (float)pt[4 + i];
		s->ccLo[i] = (float)(ct[i] & 0xFF);
		s->ccHi[i] = (float)(ct[4 + i] & 0xFF);
	}
}

// ADR-088: palette/line textures are GPU memory (uncached), byte reads/writes are very slow (7ms of render prep in games that change
// the palette every frame). Build in a cached CPU copy and copy in one go
static uint8_t s_pal_cpu[2048 * 4] __attribute__((aligned(16)));
static uint8_t s_line_cpu[256 * LINE_W * 4] __attribute__((aligned(16)));
static void fill_palette(uint8_t *dst)
{
	uint8_t *o = s_pal_cpu;
	for (int i = 0; i < 2048; i++, o += 4) {
		if (s_snap_colormode == 2) {
			u32 v = T2ReadLong(s_snap_cram, (i << 2) & 0xFFF);
			o[0] = v & 0xFF; o[1] = (v >> 8) & 0xFF; o[2] = (v >> 16) & 0xFF;
			o[3] = (v & 0x80000000) ? 255 : 0;
		} else {
			u16 v = T2ReadWord(s_snap_cram, (i << 1) & 0xFFF);
			rgb555(o, v);
			o[3] = (v & 0x8000) ? 255 : 0;
		}
	}
	memcpy(dst, s_pal_cpu, sizeof(s_pal_cpu));
}

// sprite 16-bit value -> left (RGB direct color or palette index lo/hi, key = priority * 8 + 5), right (alpha byte when palette MSB is 0,
// when 1, shadow type, palette used). The shader reads colors from the palette, so no rebuild on CRAM changes (ADR-027).
// rebuilt only for sets where sprite format, priority or ratio registers changed (worker thread)
static int log2i(int v) { int n = 0; while (v > 1) { v >>= 1; n++; } return n; }
// ADR-088: table built in a CPU copy and copied in one go.
// ADR-089: table textures are not per set but in a shared pool keyed by config (SLUT_POOL slots). Copying 512KB per set into uncached memory
// cost 6.6 / 2.9 / 2.7ms over 3 frames on scene changes. Slots used by other sets (GPU may still be reading) are not overwritten
#define SLUT_POOL 4 // the other two of the 3 sets use at most 2 slots, so a free slot always exists
static GxTex *s_slut_pool[SLUT_POOL];
static float s_slut_pool_key[SLUT_POOL][48];
static int s_slut_pool_valid[SLUT_POOL];
static void fill_sprite_lut(FrameSet *s)
{
	float key[48];
	memset(key, 0, sizeof(key));
	memcpy(key, s->sprA, 16); memcpy(key + 4, s->sprB, 16); memcpy(key + 8, s->sprC, 16); memcpy(key + 12, s->sprD, 16);
	memcpy(key + 16, s->sprE, 16); memcpy(key + 20, s->prioLo, 16); memcpy(key + 24, s->prioHi, 16);
	memcpy(key + 28, s->ccLo, 16); memcpy(key + 32, s->ccHi, 16);
	if (s->slut_valid && s->slut_idx >= 0 && !memcmp(key, s->slut_key, sizeof(key)))
		return;
	memcpy(s->slut_key, key, sizeof(key));
	s->slut_valid = 1;
	for (int i = 0; i < SLUT_POOL; i++)
		if (s_slut_pool_valid[i] && !memcmp(key, s_slut_pool_key[i], sizeof(key))) {
			s->slut_idx = i;
			s->slut = s_slut_pool[i];
			return;
		}
	// slot not used by other sets (empty slots first)
	int pick = -1;
	for (int i = 0; i < SLUT_POOL; i++) {
		int used = 0;
		for (int j = 0; j < NSETS; j++)
			if (&s_set[j] != s && s_set[j].slut_idx == i)
				used = 1;
		if (used)
			continue;
		if (pick < 0 || !s_slut_pool_valid[i])
			pick = i;
		if (!s_slut_pool_valid[i])
			break;
	}
	s->slut_idx = pick;
	s->slut = s_slut_pool[pick];
	memcpy(s_slut_pool_key[pick], key, sizeof(key));
	s_slut_pool_valid[pick] = 1;
	const int ps = log2i((int)s->sprA[0]), pm = (int)s->sprA[1] - 1, cs = log2i((int)s->sprA[2]), cm = (int)s->sprA[3] - 1;
	const int B0 = (int)s->sprB[0], cmask = B0 - 1, B1 = s->sprB[1] > 0.5f, B2 = s->sprB[2] > 0.5f;
	const int C0 = (int)(s->sprC[0] + 0.5f), C1 = (int)s->sprC[1], C2 = s->sprC[2] > 0.5f, C3 = (int)s->sprC[3];
	const int D0 = (int)s->sprD[0], D1 = s->sprD[1] > 0.5f, D2 = s->sprD[2] > 0.5f, D3 = s->sprD[3] > 0.5f;
	const int E1 = s->sprE[1] > 0.5f;
	int prio[8], cc[8];
	for (int k = 0; k < 4; k++) {
		prio[k] = (int)s->prioLo[k]; prio[4 + k] = (int)s->prioHi[k];
		cc[k] = (int)s->ccLo[k]; cc[4 + k] = (int)s->ccHi[k];
	}
	// common alpha for RGB direct color
	int rgb_a = 63;
	if (C0 > 2 && C2) { rgb_a = cc[0]; if (D1) rgb_a += 128; }
	// priority and color calc ratio depend only on two 3-bit indices (pi, ci) of the pixel value; precompute the 8x8 combos to keep the 65536-entry loop light
	uint32_t aFT[8][8];
	for (int pi = 0; pi < 8; pi++)
		for (int ci = 0; ci < 8; ci++) {
			const int pv = prio[pi];
			int aF = 63, aT = 63;
			if (C2) {
				const int cv = cc[ci];
				int t0, t1;
				if (C0 == 0) t0 = t1 = pv <= C1;
				else if (C0 == 1) t0 = t1 = pv == C1;
				else if (C0 == 2) t0 = t1 = pv >= C1;
				else { t0 = 0; t1 = 1; }
				if (D2) { aF = cv + (t0 ? 128 : 0); aT = cv + (t1 ? 128 : 0); }
				else { aF = t0 ? cv + (D3 ? 128 : 0) : 63; aT = t1 ? cv + (D3 ? 128 : 0) : 63; }
			}
			aFT[pi][ci] = (uint32_t)aF | (uint32_t)aT << 8;
		}
	static uint32_t row[512];
	for (int hi = 0; hi < 256; hi++) {
		for (int lo = 0; lo < 256; lo++) {
			const int pix = hi << 8 | lo;
			uint32_t L = 0, R = 63u | (63u << 8); // transparent: key 0
			if (pix) {
				if (pix >= 0x8000 && B2) {
					const int draw = pix != 0x8000 || D0 < 2 || (D0 < 8 && !E1);
					if (draw && prio[0] > 0) {
						const int v = pix - 0x8000;
						L = (uint32_t)((v & 31) * 8) | (uint32_t)(((v >> 5) & 31) * 8) << 8 | (uint32_t)(((v >> 10) & 31) * 8) << 16 |
							(uint32_t)(prio[0] * 8 + 5) << 24;
						R = (uint32_t)rgb_a | (uint32_t)rgb_a << 8;
					}
				} else {
					const int pi = (pix >> ps) & pm & 7, pv = prio[pi], code = pix & cmask;
					const int msb = B1 && pix >= 0x8000;
					if (pv > 0) {
						if (code == B0 - 2) {
							L = (uint32_t)(pv * 8 + 5) << 24; // normal shadow: color 0, no offset
							R = 63u | 63u << 8 | 1u << 16;
						} else {
							const uint32_t aft = aFT[pi][(pix >> cs) & cm & 7]; // alpha byte for palette MSB 0 / 1
							const int usepal = !(msb && code == 0);
							const int pa = (C3 + code) & 2047;
							L = (usepal ? (uint32_t)(pa & 0xFF) | (uint32_t)(pa >> 8) << 8 : 0u) | (uint32_t)(pv * 8 + 5) << 24;
							R = aft | (uint32_t)(msb ? 2 : 0) << 16 | (usepal ? 255u : 0u) << 24;
						}
					}
				}
			}
			row[lo] = L;
			row[256 + lo] = R;
		}
		memcpy(s->slut->data + hi * 512 * 4, row, sizeof(row)); // row (2KB) built in cache and copied right away
	}
}

static void fill_lines(FrameSet *s)
{
	const Vdp2 *fr = &s_snap_regs;
	// back screen (Vdp2DrawBackScreen, before offset)
	u32 back_addr = 0;
	int back_black = ((fr->TVMD & 0x8000) == 0 && (fr->TVMD & 0x100) == 0);
	int back_perline = (fr->BKTAU & 0x8000) != 0;
	if (fr->VRSIZE & 0x8000)
		back_addr = (((fr->BKTAU & 0x7) << 16) | fr->BKTAL) * 2;
	else
		back_addr = (((fr->BKTAU & 0x3) << 16) | fr->BKTAL) * 2;
	int ben, br, bg, bb;
	color_offset(fr, 0x20, &ben, &br, &bg, &bb);
	// line screen (Vdp2DrawLineScreen)
	u32 ls_addr = (fr->VRSIZE & 0x8000) ? ((fr->LCTA.all & 0x7FFFF) << 1) : ((fr->LCTA.all & 0x3FFFF) << 1);
	int ls_perline = (fr->LCTA.part.U & 0x8000) != 0;
	int ls_alpha = (fr->CCRLB & 0x1F) << 1;

	static const int mask[6] = { 0x8, 0x4, 0x2, 0x1, 0x10, 0x40 }; // N3, N2, N1, N0, R0, sprite
	int first[6][4], second[6][4], nfirst[6], nsecond[6], nvals[6];
	memset(first, 0, sizeof(first));
	memset(nfirst, 0, sizeof(nfirst));
	memset(nsecond, 0, sizeof(nsecond));
	memset(nvals, 0, sizeof(nvals));
	s->off_ok = 1;
	s->offu[0][0] = (float)br; s->offu[0][1] = (float)bg; s->offu[0][2] = (float)bb; s->offu[0][3] = ben ? 1.0f : 0.0f;
	for (int y = 0; y < s->h && y < 256; y++) { // line texture has 256 lines (beyond that is outside the buffer)
		uint8_t *row = s_line_cpu + y * LINE_W * 4; // ADR-088: CPU copy (copied in one go at the end)
		if (back_black) {
			row[0] = row[1] = row[2] = 0;
		} else {
			rgb555(row, T1ReadWord(s_snap_vram, back_perline ? back_addr + y * 2 : back_addr));
		}
		row[3] = 0;
		put_off(row + 8, row + 12, ben, br, bg, bb);
		if (fr->LNCLEN) {
			u16 idx = T1ReadWord(s_snap_vram, ls_perline ? ls_addr + y * 2 : ls_addr) & 0x7FF;
			memcpy(row + 4, s_pal_cpu + idx * 4, 3);
			row[7] = (uint8_t)ls_alpha;
		} else {
			row[4] = row[5] = row[6] = row[7] = 0;
		}
		const Vdp2 *lr = Vdp2RestoreRegs(y, s_snap_lines);
		if (!lr) lr = fr;
		for (int k = 0; k < 6; k++) {
			int en, r, g, b;
			color_offset(lr, mask[k], &en, &r, &g, &b);
			put_off(row + (4 + 2 * k) * 4, row + (5 + 2 * k) * 4, en, r, g, b);
			if (y == 0) {
				first[k][0] = en; first[k][1] = r; first[k][2] = g; first[k][3] = b;
				nfirst[k] = 1;
				nsecond[k] = 0;
				nvals[k] = 1;
			} else if (first[k][0] == en && first[k][1] == r && first[k][2] == g && first[k][3] == b) {
				nfirst[k]++;
			} else if (nvals[k] == 1) {
				second[k][0] = en; second[k][1] = r; second[k][2] = g; second[k][3] = b;
				nsecond[k] = 1;
				nvals[k] = 2;
			} else if (second[k][0] == en && second[k][1] == r && second[k][2] == g && second[k][3] == b) {
				nsecond[k]++;
			} else {
				nvals[k] = 3;
			}
		}
	}
	memcpy(s->line_px, s_line_cpu, (size_t)(s->h < 256 ? s->h : 256) * LINE_W * 4); // ADR-088
	// fast composite has one offset per layer. When it changes once mid-frame (2 values, e.g. fade), approximate with the value used by more lines
	// (brightness off for at most 1 frame, ADR-035). 3 or more values (per-line raster effects) use the slow composite
	for (int k = 0; k < 6; k++) {
		if (nvals[k] > 2)
			s->off_ok = 0;
		const int *v = (nvals[k] == 2 && nsecond[k] > nfirst[k]) ? second[k] : first[k];
		float *u = s->offu[1 + k];
		u[0] = (float)v[1]; u[1] = (float)v[2]; u[2] = (float)v[3]; u[3] = v[0] ? 1.0f : 0.0f;
	}
}

// Titan layer buffer -> RGBA8 (rgb color before offset, a = key: priority * 8 + L, +128 if ratio used, 0 if none)
static void fill_layer(FrameSet *s, int L)
{
	int tw = 0;
	struct PixelData *src = TitanGetLayerBuffer(L, &tw);
	uint32_t *dst = (uint32_t *)s->layer_px[L];
	for (int y = 0; y < s->h; y++) {
		const struct PixelData *p = src + y * tw;
		uint32_t *d = dst + y * LAYER_W;
		for (int x = 0; x < s->w; x++) {
			const u32 px = p[x].pixel;
			const u32 prio = p[x].priority;
			if (!prio) {
				d[x] = 0;
				continue;
			}
			const u32 ab = px >> 24;
			const u32 key = prio * 8u + (u32)L + ((ab != 0x3F) ? 128u : 0u);
			d[x] = (px & 0x00FFFFFFu) | (key << 24);
		}
	}
}

// stage 2: captured NBG layers -> line value texture and shader uniforms
static void fill_nbg(FrameSet *s, int L, const RstvLayerCapture *c)
{
	for (int y = 0; y < 256; y++) {
		uint8_t *t = s->rows_px + (y * ROWS_W + L * 8) * 4;
		const RstvCapRow *r = &c->row[y];
		if (y >= c->nrows || y >= s->h || !r->enable || (s_gpu_floor & 1)) {
			t[4 + 2] = 0; // t1.z = enable
			continue;
		}
		t[0] = r->x & 0xFF; t[1] = r->x >> 8; t[2] = r->lsx & 0xFF; t[3] = r->lsx >> 8;
		t[4] = r->y & 0xFF; t[5] = r->y >> 8; t[6] = 1; t[7] = r->priority;
		for (int k = 0; k < 4; k++) {
			uint8_t *p = t + (2 + k) * 4;
			p[0] = r->planetbl[k] & 0xFF; p[1] = (r->planetbl[k] >> 8) & 0xFF; p[2] = (r->planetbl[k] >> 16) & 0xFF; p[3] = 0;
		}
		{
			// per-line horizontal scale (ADR-037): scale * 65536 (line scale table values have 8 fraction bits, so exact)
			const float z = r->zx >= 0.0f ? r->zx : 1.0f;
			const uint32_t zf = (uint32_t)(z * 65536.0f + 0.5f);
			uint8_t *p = t + 6 * 4;
			p[0] = zf & 0xFF; p[1] = (zf >> 8) & 0xFF; p[2] = (zf >> 16) & 0xFF; p[3] = 0;
		}
	}
	float (*u)[4] = s->nbg[L];
	const int supp = c->supplementdata;
	const int pagesize_bits = c->pagewh_bits * 2;
	const int cellwh = 2 + c->patternwh;
	u[0][0] = c->colornumber; u[0][1] = c->transparencyenable ? 1 : 0; u[0][2] = c->patterndatasize; u[0][3] = c->patternwh;
	u[1][0] = c->auxmode; u[1][1] = c->charaddr_mask_3fff; u[1][2] = c->coloroffset; u[1][3] = c->alpha & 0xFF;
	u[2][0] = (float)(1 << c->planepixelwidth_bits); u[2][1] = (float)(1 << c->planepixelheight_bits);
	u[2][2] = c->mapwh; u[2][3] = (float)(c->xmask + 1);
	u[3][0] = (float)(1 << c->pagewh_bits); u[3][1] = (float)(1 << pagesize_bits);
	u[3][2] = (float)(1 << c->planew_bits); u[3][3] = (float)(1 << (c->patterndatasize_bits + 1));
	u[4][0] = (float)(1 << cellwh); u[4][1] = c->specialcolormode; u[4][2] = L * 8; u[4][3] = (float)((supp & 0xE0) << 3);
	u[5][0] = (float)((supp & 0x1F) << 10);
	u[5][1] = (float)((supp & 0x3) | ((supp & 0x1C) << 10));
	u[5][2] = (float)((supp & 0x1C) << 10);
	u[5][3] = (float)((supp & 0x3) | ((supp & 0x10) << 10));
	u[6][0] = (supp >> 9) & 1; u[6][1] = (supp >> 8) & 1; u[6][2] = (float)L; u[6][3] = c->isbitmap ? 1.0f : 0.0f;
	if (c->isbitmap) {
		// bitmap (ADR-039): cfgF = (VRAM address, palette address, 0, special color calc)
		u[5][0] = (float)(c->bm_charaddr & 0x7FFFF);
		u[5][1] = (float)c->bm_paladdr;
		u[5][2] = 0;
		u[5][3] = (float)c->bm_scf;
	}
	for (int b = 0; b < 4; b++) {
		u[7][b] = (c->specialcode >> b) & 1;
		u[8][b] = (c->specialcode >> (4 + b)) & 1;
	}
}

// ADR-123: bitmap NBG -> RGBA texture, only the rows whose VRAM pages changed since this set last converted (or all of
// it when the bitmap, its colours or the palette changed).
// - lines (all enabled lines with the same priority): the texel is the final layer pixel (colour, key = priority * 8 +
//   layer + 128 if colour calculation, 0 = transparent; nbg_tail of the NBG shader done here) and the layer is drawn as
//   line runs with interpolated texture coordinates (bmp_runs, pcopy shader): no dependent reads
// - otherwise the texel keeps the dot (alpha = opaque 128 | palette MSB 64 | dot & 15) and the NBG pixel shader reads it
//   (NBG_BTEX) instead of row data + VRAM + palette (myfairld, vmahjong: two bitmap layers took 6~8 ms of GPU each)
static int s_bmp_tex_on = 1;
static int bmp_texture(FrameSet *s, int L, const RstvLayerCapture *c, int prio)
{
	const int w = c->xmask + 1, h = c->ymask + 1, cn = c->colornumber;
	if (!s_bmp_tex_on || w > 1024 || h > 512 || cn > 4)
		return 0;
	// palette dots: only as palette addresses for the pci shader (ADR-089 format), so a CRAM change (palette animation:
	// kiwames, every frame) costs no conversion. Baking colours there took 42 ms a frame
	const int pidx = cn < 3;
	if (pidx && (prio < 0 || !s_prog_pci))
		return 0;
	if (!s->bmp_tex[L]) {
		s->bmp_tex[L] = gx_tex(1024, 512, GX_RGBA8, 0, 0);
		s->bmp_copied[L] = 0;
		if (!s->bmp_tex[L]) {
			s_bmp_tex_on = 0;
			RSTV_LOG("gvdp2: bitmap texture: out of memory");
			return 0;
		}
	}
	const u32 base = (u32)c->bm_charaddr & 0x7FFFF;
	// palette index = colour offset (+ bitmap palette for 4 / 8 bit dots) + dot, as the shader's bitmap branch
	const u32 pal = (u32)c->coloroffset + (cn < 2 ? (u32)c->bm_paladdr : 0);
	// final pixels (prio > 0): the colour calculation inputs of nbg_tail
	const int fin = prio >= 0, tr = c->transparencyenable != 0, scm = c->specialcolormode, scf = c->bm_scf != 0;
	const int ccr = (c->alpha & 0xFF) != 63;
	const uint32_t key[8] = { base, (uint32_t)w, (uint32_t)h, (uint32_t)cn, pal, 0,
		fin ? (1u | (uint32_t)prio << 1 | (uint32_t)tr << 5 | (uint32_t)scm << 6 | (uint32_t)scf << 8 | (uint32_t)ccr << 9 |
			(uint32_t)L << 10) : 0,
		fin ? (uint32_t)c->specialcode : 0 };
	const int all = !s->bmp_copied[L] || memcmp(key, s->bmp_key[L], sizeof(key)) != 0;
	const u32 rb = cn == 0 ? (u32)w / 2 : cn == 1 ? (u32)w : cn <= 3 ? (u32)w * 2 : (u32)w * 4;
	static uint8_t row[1024 * 4] __attribute__((aligned(16)));
	uint8_t *tex = (uint8_t *)s->bmp_tex[L]->data;
	u8 *v = s_snap_vram;
	for (int y = 0; y < h; y++) {
		const u32 a0 = (base + (u32)y * rb) & 0x7FFFF;
		if (!all) {
			int dirty = 0;
			for (u32 p = a0 >> 12; p <= (a0 + rb - 1) >> 12 && !dirty; p++)
				dirty = rstv_vdp2_page_stamp[p & 127] > s->bmp_copied[L];
			if (!dirty)
				continue;
		}
		uint8_t *o = row;
		for (int x = 0; x < w; x++, o += 4) {
			u32 dot, opaque, msb = 0;
			if (cn <= 2) {
				if (cn == 0) {
					const u8 b = v[(a0 + (u32)x / 2) & 0x7FFFF];
					dot = (x & 1) ? (b & 15u) : (u32)(b >> 4);
				} else if (cn == 1)
					dot = v[(a0 + (u32)x) & 0x7FFFF];
				else
					dot = T1ReadWord(v, (a0 + (u32)x * 2) & 0x7FFFE);
				const uint8_t *pc = s_pal_cpu + ((pal + dot) & 2047) * 4;
				o[0] = pc[0]; o[1] = pc[1]; o[2] = pc[2];
				msb = pc[3] > 127;
				opaque = dot != 0;
			} else if (cn == 3) {
				dot = T1ReadWord(v, (a0 + (u32)x * 2) & 0x7FFFE);
				o[0] = (uint8_t)((dot & 31) * 8); o[1] = (uint8_t)(((dot >> 5) & 31) * 8); o[2] = (uint8_t)(((dot >> 10) & 31) * 8);
				opaque = (dot & 0x8000) != 0;
			} else {
				const u32 hi = T1ReadWord(v, (a0 + (u32)x * 4) & 0x7FFFE), lo = T1ReadWord(v, (a0 + (u32)x * 4 + 2) & 0x7FFFE);
				dot = lo;
				o[0] = (uint8_t)(lo & 0xFF); o[1] = (uint8_t)(lo >> 8); o[2] = (uint8_t)(hi & 0xFF);
				opaque = (hi & 0x8000) != 0;
			}
			if (!fin) {
				o[3] = (uint8_t)((opaque ? 128 : 0) | (msb ? 64 : 0) | (dot & 15));
				continue;
			}
			if (pidx) {
				// r = palette address low 8 bits, g = high 3 bits + 8 (flag on) / 16 (flag = palette MSB), a = key, 0 transparent
				if (prio == 0 || (!opaque && tr)) {
					o[0] = o[1] = o[2] = o[3] = 0;
					continue;
				}
				const u32 pa = (pal + dot) & 2047;
				int fl = 0;
				if (ccr) {
					if (scm == 0)
						fl = 1;
					else if (scm == 1)
						fl = scf ? 1 : 0;
					else if (scm == 2)
						fl = (scf && ((c->specialcode >> ((dot & 15) >> 1)) & 1)) ? 1 : 0;
					else
						fl = 2;
				}
				o[0] = (uint8_t)(pa & 0xFF);
				o[1] = (uint8_t)((pa >> 8) + fl * 8);
				o[2] = 0;
				o[3] = (uint8_t)(prio * 8 + L);
				continue;
			}
			// nbg_tail: priority 0 or a transparent dot -> 0, else key with the colour calculation flag
			if (prio == 0 || (!opaque && tr)) {
				o[0] = o[1] = o[2] = o[3] = 0;
				continue;
			}
			int use = 1;
			if ((scm == 1 || scm == 2) && !scf)
				use = 0;
			else if (scm == 2) {
				if (!((c->specialcode >> ((dot & 15) >> 1)) & 1))
					use = 0;
			} else if (scm == 3 && !msb)
				use = 0;
			o[3] = (uint8_t)(prio * 8 + L + ((use && ccr) ? 128 : 0));
		}
		memcpy(tex + (size_t)y * 1024 * 4, row, (size_t)w * 4);
	}
	memcpy(s->bmp_key[L], key, sizeof(key));
	s->bmp_copied[L] = s_snap_serial;
	if (!fin)
		s->nbg[L][6][3] = 2.0f; // the shader reads the texture (NBG_BTEX)
	return 1;
}

// ADR-123: line runs over the bitmap texture (same split at the bitmap's wrap as the plane runs of ADR-058: u at pixel
// centres = x + i * zx + C, C an integer taking the wrap and line scroll). 0 if a line needs too many runs
static int bmp_runs(FrameSet *s, int L, const RstvLayerCapture *c)
{
	enum { BR_SEG = 16 };
	const int xwrap = c->xmask + 1, w = s->w, h = c->nrows < s->h ? c->nrows : s->h;
	float *lv = (float *)gx_temp(h * BR_SEG * 2 * 16 * (int)sizeof(float));
	if (!lv)
		return 0;
	int ln = 0;
	for (int y = 0; y < h; y++) {
		const RstvCapRow *r = &c->row[y];
		if (!r->enable)
			continue;
		const float zx = r->zx >= 0.0f ? r->zx : 1.0f;
		const int lsx = r->lsx & (xwrap - 1);
		const float pv = (float)(r->y & c->ymask);
		int i0 = 0, nseg = 0;
		while (i0 < w) {
			int i = w;
			if (zx > 0.0f) {
				const int f0i = (int)((float)r->x + (float)i0 * zx);
				const int fm = f0i & (xwrap - 1);
				int bnd = f0i - fm + xwrap;
				if (lsx && fm < xwrap - lsx && f0i - fm + (xwrap - lsx) < bnd)
					bnd = f0i - fm + (xwrap - lsx);
				int ib = (int)(((float)bnd - (float)r->x) / zx);
				if (ib <= i0)
					ib = i0 + 1;
				while (ib > i0 + 1 && (int)((float)r->x + (float)(ib - 1) * zx) >= bnd)
					ib--;
				while (ib < w && (int)((float)r->x + (float)ib * zx) < bnd)
					ib++;
				if (ib < i)
					i = ib;
			}
			const float f0 = (float)r->x + (float)i0 * zx;
			int g0 = ((int)f0) & (xwrap - 1);
			if (lsx)
				g0 = (g0 + lsx) & (xwrap - 1);
			const float ul = f0 + (float)(g0 - (int)f0) + (1.0f / 256.0f) - 0.5f * zx;
			if (++nseg > BR_SEG)
				return 0;
			const int nr = r->nwin ? r->nwin : 1;
			for (int k = 0; k < nr; k++) {
				int a = i0, b = i;
				if (r->nwin) {
					if (a < r->wx0[k]) a = r->wx0[k];
					if (b > r->wx1[k] + 1) b = r->wx1[k] + 1;
					if (a >= b)
						continue;
				}
				if (ln >= h * BR_SEG * 2)
					return 0;
				const float ua = ul + (float)(a - i0) * zx, ub = ul + (float)(b - i0) * zx;
				const float d[16] = { (float)a, (float)y, ua, pv, (float)b, (float)y, ub, pv,
					(float)a, (float)(y + 1), ua, pv + 1.0f, (float)b, (float)(y + 1), ub, pv + 1.0f };
				memcpy(lv + ln * 16, d, sizeof(d));
				ln++;
			}
			i0 = i;
		}
	}
	s->bq_lv[L] = lv;
	s->bq_ln[L] = ln;
	return 1;
}

// screen-sized quad (NDC + pixel coords), program and uniforms set by caller
static void draw_quad(int w, int h)
{
	const float W = (float)w, H = (float)h;
	const float nx = -1.0f + 2.0f * W / LAYER_W, ny = -1.0f + 2.0f * H / LAYER_H;
	float *v = (float *)gx_temp(16 * sizeof(float));
	if (!v)
		return;
	const float d[16] = { -1, -1, 0, 0, nx, -1, W, 0, -1, ny, 0, H, nx, ny, W, H };
	memcpy(v, d, sizeof(d));
	gx_draw(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, gx_seq_indices(4), 4);
}

// ADR-052: rectangles outside the window runs (lines with the same runs merged into bands), as clear-program quads
static int build_win_clear(const RstvLayerCapture *c, int w, int h, float *v, int max)
{
	int n = 0;
	int prev_n = 0, prev_a[3], prev_b[3], band_y0 = 0;
	if (h > c->nrows)
		h = c->nrows;
	for (int y = 0; y <= h; y++) {
		int cn = 0, ca[3], cb[3];
		if (y < h) {
			const RstvCapRow *r = &c->row[y];
			if (r->enable && r->nwin) {
				int st = 0;
				for (int k = 0; k < r->nwin && k < 2; k++) {
					if (r->wx0[k] > st) { ca[cn] = st; cb[cn] = r->wx0[k]; cn++; }
					st = r->wx1[k] + 1;
				}
				if (st < w) { ca[cn] = st; cb[cn] = w; cn++; }
			}
		}
		int same = y < h && cn == prev_n;
		for (int k = 0; same && k < cn; k++)
			same = ca[k] == prev_a[k] && cb[k] == prev_b[k];
		if (same)
			continue;
		// emit the band [band_y0, y) of the previous runs
		for (int k = 0; k < prev_n; k++) {
			if (n >= max)
				return -1;
			const float x0 = -1.0f + 2.0f * prev_a[k] / LAYER_W, x1 = -1.0f + 2.0f * prev_b[k] / LAYER_W;
			const float y0 = -1.0f + 2.0f * band_y0 / LAYER_H, y1 = -1.0f + 2.0f * y / LAYER_H;
			const float d[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
			memcpy(v + n * 8, d, sizeof(d));
			n++;
		}
		prev_n = cn;
		memcpy(prev_a, ca, sizeof(ca));
		memcpy(prev_b, cb, sizeof(cb));
		band_y0 = y;
	}
	return n;
}

// ADR-057: what the layer's GPU drawing depends on
static uint32_t layer_key(const FrameSet *s, int L)
{
	uint32_t k = 2166136261u;
	if (L == 4) {
		MIXV(k, s->rbg_vh); MIXV(k, s->rbg_n); MIXV(k, s->rbg_nb); MIXV(k, s->rbg_bmp);
		k = mixw(k, s->rbg_cfg, sizeof(s->rbg_cfg));
		k = mixw(k, s->rbg_cfg2, sizeof(s->rbg_cfg2));
		k = mixw(k, s->rbg_table->data, 8 * 8 * 4);
		if (s->rbg_nb > 0)
			k = mixw(k, s->rbg_table_b->data, 8 * 8 * 4);
	} else {
		MIXV(k, s->lvh[L]); MIXV(k, s->quad_n[L]); MIXV(k, s->quad_cn[L]); MIXV(k, s->qclr_n[L]);
		// only the palette banks the cells use (the rest of CRAM may animate); RGB layers use none
		if (s->quad_cn[L] <= 2) {
			const int esz = s_snap_colormode == 2 ? 4 : 2;
			for (unsigned bank = 0; bank < 128; bank++)
				if (s->lbank[L][bank >> 5] & (1u << (bank & 31)))
					k = mixw(k, s_snap_cram + ((bank * 16 * esz) & 0xFFF), (size_t)16 * esz);
		}
		k = mixw(k, s->quad_cfg[L], sizeof(s->quad_cfg[L]));
		if (s->qclr_n[L] > 0)
			k = mixw(k, s->qclr_v[L], (size_t)s->qclr_n[L] * 8 * sizeof(float));
	}
	MIXV(k, s_cgen); MIXV(k, s->pal_mode); MIXV(k, s->w); MIXV(k, s->h);
	if (L == 4 && s->rbg_cfg[0][3] <= 2.5f) {
		// RBG0: the banks its map pages use, moved by the colour offset (16 or 256 colours per cell; 2048: all)
		const int cn = (int)s->rbg_cfg[0][3], nb16 = cn == 0 ? 1 : cn == 1 ? 16 : 128;
		const unsigned off = ((unsigned)s->rbg_cfg[1][1] >> 4) & 127;
		const int esz = s_snap_colormode == 2 ? 4 : 2;
		uint32_t used[4] = { 0, 0, 0, 0 };
		for (unsigned b = 0; b < 128; b++)
			if (s->rbg_bank[b >> 5] & (1u << (b & 31)))
				for (int j = 0; j <= nb16; j++) { // one more: the colour offset need not be a multiple of 16
					const unsigned bank = (b + off + (unsigned)j) & 127;
					used[bank >> 5] |= 1u << (bank & 31);
				}
		for (unsigned bank = 0; bank < 128; bank++)
			if (used[bank >> 5] & (1u << (bank & 31)))
				k = mixw(k, s_snap_cram + ((bank * 16 * esz) & 0xFFF), (size_t)16 * esz);
	}
	return k;
}

static GxProg *nbg_variant(const float (*u)[4], GxParam **loc);
// NBG pixel shader over the given line quads (nbgw format), inside the current gx_begin (ADR-052)
static void draw_nbg_lines(FrameSet *s, int L, void *v, int n)
{
	GxProg *np = s_prog_nbg;
	const GxParam *nl = s_nloc;
	if (s_nbg_spec) {
		GxParam *vl;
		GxProg *vp = nbg_variant((const float (*)[4])s->nbg[L], &vl);
		if (vp) {
			np = vp;
			nl = vl;
		}
	}
	gx_use(np);
	const GxTex *nt[3] = { s->vram, s->pal, s->rows };
	for (int i = 0; i < 3; i++)
		gx_set_tex(nl[N_tVram + i], nt[i]);
	if (s->nbg[L][6][3] > 1.5f)
		gx_set_tex(nl[N_tBmp], s->bmp_tex[L]); // ADR-123
	for (int k = 0; k < 9; k++)
		gx_set4(nl[N_cfgA + k], s->nbg[L][k]);
	gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, v, s_qidx, n * 6);
}

// ADR-058: cell quads (NbgQuad) -> packed vertices in s_qvtx, palette banks they reach
static void quads_to_vtx(const NbgQuad *qs, int n, int cn, uint32_t *bk)
{
	int16_t *d = s_qvtx;
	uint32_t base_bk[4] = { 0, 0, 0, 0 };
	for (int k = 0; k < n; k++) {
		const NbgQuad *q = &qs[k];
		if (cn <= 2) {
			const unsigned bank = (unsigned)(q->palbase >> 4) & 127;
			base_bk[bank >> 5] |= 1u << (bank & 31);
		}
		const int16_t xs[4] = { q->x0, q->x1, q->x0, q->x1 }, ys[4] = { q->y0, q->y0, q->y1, q->y1 };
		const int16_t us[4] = { q->u0, q->u1, q->u0, q->u1 }, vs[4] = { q->v0, q->v0, q->v1, q->v1 };
		for (int j = 0; j < 4; j++, d += 8) {
			d[0] = xs[j]; d[1] = ys[j]; d[2] = us[j]; d[3] = vs[j];
			d[4] = (int16_t)q->palbase; d[5] = q->prio; d[6] = q->scf; d[7] = 0;
		}
	}
	bk[0] = bk[1] = bk[2] = bk[3] = 0;
	if (cn <= 2) {
		const int nb16 = cn == 0 ? 1 : cn == 1 ? 16 : 128;
		for (unsigned b = 0; b < 128; b++)
			if (base_bk[b >> 5] & (1u << (b & 31)))
				for (int j = 0; j < nb16; j++) {
					const unsigned bank = (b + (unsigned)j) & 127;
					bk[bank >> 5] |= 1u << (bank & 31);
				}
	}
}

static uint32_t bank_hash(const uint32_t *bk)
{
	uint32_t k = 2166136261u;
	const int esz = s_snap_colormode == 2 ? 4 : 2;
	for (unsigned bank = 0; bank < 128; bank++)
		if (bk[bank >> 5] & (1u << (bank & 31)))
			k = mixw(k, s_snap_cram + ((bank * 16 * esz) & 0xFFF), (size_t)16 * esz);
	return k;
}

// ADR-058: draw layer L through its plane cache. 0 = not suitable (the caller builds cell quads as before)
static int s_plane_rej[4]; // diag: last reject point (perf line prej)
#define PLANE_QN_MANY 3000
static int s_plane_many = 1; // ADR-152 (ux0:data/restv/noplanemany = off)
static int s_last_qn[4];   // ADR-152: cell quads the layer needed when it last went that way
static int s_plane_fullw = 1; // ADR-080 (ux0:data/restv/noplanefull = per-row column ranges)
static int s_plane_mag = 1;   // ADR-080 (ux0:data/restv/noplanemag = magnified lines as texel-run quads)
static int s_plane_all; // experiment: ux0:data/restv/planeall = use the plane cache even for layers with identical lines
static int s_plane_noidx;     // ADR-089 (ux0:data/restv/noplaneidx = colours in the plane, redrawn when CRAM changes)
static int plane_prepare(FrameSet *s, int L, const RstvLayerCapture *c)
{
	const u64 pp0 = RSTV_PROF_T();
	u64 pp1 = pp0, pp2 = pp0, pp3 = pp0;
	int pp_groups = 0, pp_quads = 0, pp_fresh = 0, pp_tag = 0, pp_cols = 0, pp_dirty = 0;
	if (c->xmask + 1 > PLANE_WMAX) // ADR-099/100: the plane holds at most 2048 columns
		return (s_plane_rej[L] = 10, 0);
	if (c->xmask + 1 > s_plane_w[L]) {
		// ADR-100: a wider map: this layer's plane becomes 2048 x 512 (own render target slot 24 + L, made once)
		static GxTex *wide[4];
		if (!wide[L])
			wide[L] = gx_tex(PLANE_WMAX, PLANE_H, GX_RGBA8, 0, 24 + L); // 24..27 (20 is the RBG0 half texture)
		if (!wide[L])
			return (s_plane_rej[L] = 11, 0);
		s_plane[L] = wide[L];
		s_plane_w[L] = PLANE_WMAX;
		s_plane_h[L] = PLANE_H;
		s_plane_valid[L] = 0;
	}
	const int PW = s_plane_w[L];
	int PH = s_plane_h[L];
	int fullw = s_plane_fullw;
	int h = c->nrows < s->h ? c->nrows : s->h;
	if (h > 256)
		h = 256;
	int first = -1, nvar = 0, nzoom = 0;
	const RstvCapRow *prev = NULL;
	for (int y = 0; y < h; y++) {
		const RstvCapRow *r = &c->row[y];
		if (!r->enable)
			continue;
		if (!(r->zx >= 0.0f && r->zx <= 16.0f))
			return (s_plane_rej[L] = 1, 0);
		// ADR-080: magnified lines too (zx < 1, NBGQ_RUN_ZX): as cell quads they are one quad per texel run, 7000-9000
		// quads a frame on sanjeon's dance stage (prep 7-15ms); from the plane they are one copy run per line
		nzoom += r->zx != 1.0f && (s_plane_mag || !NBGQ_RUN_ZX(r->zx));
		if (first < 0)
			first = y;
		else if (r->priority != c->row[first].priority || memcmp(r->planetbl, c->row[first].planetbl, sizeof(r->planetbl)))
			return (s_plane_rej[L] = 2, 0);
		if (prev && (r->x != prev->x || r->lsx != prev->lsx || r->zx != prev->zx))
			nvar++;
		prev = r;
	}
	// lines alike: the cell quads merge well already — unless lines are zoomed by a factor the cell quads cannot draw
	// (those would go to the NBG pixel shader line by line, 251 instructions a pixel: sanjeon f3300 4.6ms)
	// ADR-152: ... unless they did not merge last time (astrass' hi-res NBG2: 8880 quads, 11 ms of prep a frame)
	if (first < 0 || (nvar < 16 && nzoom == 0 && !s_plane_all && (!s_plane_many || s_last_qn[L] < PLANE_QN_MANY)))
		return (s_plane_rej[L] = 3, 0);
	// the map rows this frame needs, at plane row y & (PH - 1); two different rows on one plane row: not here
	static int need[PLANE_HMAX];
	for (int pass = 0;; pass++) {
		int clash = 0;
		for (int j = 0; j < PH; j++)
			need[j] = -1;
		for (int y = first; y < h && !clash; y++) {
			const RstvCapRow *r = &c->row[y];
			if (!r->enable)
				continue;
			const int pr = r->y & (PH - 1);
			if (need[pr] >= 0 && need[pr] != r->y)
				clash = 1;
			need[pr] = r->y;
		}
		if (!clash)
			break;
		// ADR-125: a map taller than the plane, lines on two map rows that share a plane row (puyosun's NBG1, a 1024-row
		// map scrolled per line: 8559 cell quads, 4.1 ms of GPU) -> this layer gets a 1024 x 1024 plane (own slot)
		if (pass || PH == PLANE_HMAX || PW > PLANE_W || c->ymask + 1 <= PH)
			return (s_plane_rej[L] = 4, 0);
		static GxTex *tall[4];
		static const int tall_slot[4] = { 18, 19, 21, 22 };
		if (!tall[L])
			tall[L] = gx_tex(PLANE_W, PLANE_HMAX, GX_RGBA8, 0, tall_slot[L]);
		if (!tall[L])
			return (s_plane_rej[L] = 4, 0);
		s_plane[L] = tall[L];
		s_plane_h[L] = PH = PLANE_HMAX;
		s_plane_valid[L] = 0;
	}
	if (c->row[first].priority == 0) {
		// ADR-078: priority 0 on every line = not displayed. An empty layer, and the plane (drawn for the other priority)
		// stays valid: sanjeon's dance stage blinks NBG3 between priority 0 and 4 every 5 frames, which rebuilt the whole plane
		// each time (prep 9 -> 22ms)
		s->pl_n[L] = -1;
		s->pl_cn[L] = 0;
		s->pl_lv[L] = NULL;
		s->pl_ln[L] = 0;
		s->pl_on[L] = 1;
		s->quad_n[L] = 0;
		s->quad_cn[L] = c->colornumber;
		s->qclr_n[L] = 0;
		s->lvh[L] = 0x9E3779B9u;
		memset(s->lbank[L], 0, sizeof(s->lbank[L]));
		s->lok[L] = 1;
		return 1;
	}
	if (!s_plane[L]) {
		s_plane[L] = gx_tex(PW, PLANE_H, GX_RGBA8, 0, 14 + L); // own render target slot (gx.c)
		if (!s_plane[L])
			return (s_plane_rej[L] = 5, 0);
	}
	// what the plane rows depend on besides VRAM and CRAM contents
	uint32_t key = 2166136261u;
	MIXV(key, c->colornumber); MIXV(key, c->transparencyenable); MIXV(key, c->patterndatasize); MIXV(key, c->patternwh);
	MIXV(key, c->auxmode); MIXV(key, c->supplementdata); MIXV(key, c->specialprimode); MIXV(key, c->pagewh_bits);
	MIXV(key, c->planew_bits); MIXV(key, c->patterndatasize_bits); MIXV(key, c->mapwh); MIXV(key, c->planepixelwidth_bits);
	MIXV(key, c->planepixelheight_bits); MIXV(key, c->charaddr_mask_3fff); MIXV(key, c->coloroffset);
	// only "alpha != 63" reaches the plane texels (nbg_tail flag); the ratio is applied in the compose. rsgun fades NBG2
	// alpha every 2~3 frames: the whole plane was drawn again each time (prep 16ms, dropped frames)
	MIXV(key, (c->alpha & 0xFF) != 63);
	MIXV(key, c->specialcolormode); MIXV(key, c->specialcode); MIXV(key, c->ymask); MIXV(key, L);
	MIXV(key, c->row[first].priority); key = mixw(key, c->row[first].planetbl, sizeof(c->row[first].planetbl));
	MIXV(key, s->pal_mode);
	const int pidx = s_prog_qi && s_prog_pci && c->colornumber <= 2 && !s_plane_noidx;
	MIXV(key, pidx);
	s->pl_idx[L] = pidx;
	int dirty = !s_plane_valid[L] ? 1 : key != s_plane_key[L] ? 2 : 0;
	if (!dirty) {
		// ADR-100: VRAM pages written since the plane was drawn: only the rows drawn from them are stale (twcup98 rewrote
		// a few map cells every 16 frames: the whole 2048-wide plane was drawn again, 16ms)
		uint32_t chg[4] = { 0, 0, 0, 0 };
		int any = 0;
		for (int pg = 0; pg < 128; pg++)
			if ((s_plane_pages[L][pg >> 5] & (1u << (pg & 31))) && s_vcstamp[pg] > s_plane_stamp[L]) {
				chg[pg >> 5] |= 1u << (pg & 31);
				any = 1;
			}
		if (any) {
			s_stats.plane_why[2]++;
			uint16_t chgsub[128];
			for (int pg = 0; pg < 128; pg++) {
				chgsub[pg] = 0;
				if (chg[pg >> 5] & (1u << (pg & 31)))
					for (int b = 0; b < 16; b++)
						if (s_vcsub[pg][b] > s_plane_stamp[L])
							chgsub[pg] |= (uint16_t)(1u << b);
			}
			uint32_t keep[4] = { 0, 0, 0, 0 };
			for (int j = 0; j < PH; j++) {
				if (s_plane_tag[L][j] < 0)
					continue;
				const uint32_t *rp = s_plane_rpg[L][j];
				int stale = ((rp[0] & chg[0]) | (rp[1] & chg[1]) | (rp[2] & chg[2]) | (rp[3] & chg[3])) != 0;
				if (stale && s_plane_sub[L][j].n != 255) {
					// ADR-137: only if a block it read changed (every page it read has an entry)
					const PlSub *ps = &s_plane_sub[L][j];
					stale = 0;
					for (int k = 0; k < ps->n && !stale; k++)
						stale = (ps->m[k] & chgsub[ps->pg[k]]) != 0;
				}
				if (stale)
					s_plane_tag[L][j] = -1;
				else
					for (int k = 0; k < 4; k++)
						keep[k] |= rp[k];
			}
			memcpy(s_plane_pages[L], keep, sizeof(keep));
			s_plane_stamp[L] = s_snap_serial;
			fullw = 0; // only the columns lines show (twcup98's blinking text: 112 rows x 2048 columns every 16 frames, 8ms)
		}
	}
	if (!dirty && !pidx && bank_hash(s_plane_bank[L]) != s_plane_bankh[L])
		dirty = 8;
	pp_dirty = dirty;
	if (dirty) {
		// everything drawn so far is stale
		s_stats.plane_why[dirty == 1 ? 0 : dirty == 2 ? 1 : dirty == 4 ? 2 : 3]++;
		for (int j = 0; j < PH; j++)
			s_plane_tag[L][j] = -1;
		memset(s_plane_pages[L], 0, sizeof(s_plane_pages[L]));
		memset(s_plane_bank[L], 0, sizeof(s_plane_bank[L]));
		s_plane_key[L] = key;
		s_plane_valid[L] = 1;
	}
	s->pl_n[L] = -1;
	s->pl_cn[L] = 0;
	// pass 2 runs first (per line, the runs of the plane it shows: u at pixel centres = x + i * zx + C, C an integer that
	// takes the map wrap and the line scroll; a tiny bias keeps exact integers inside their texel). Their columns tell
	// pass 1 what each plane row needs
	// up to PL_SEG wrap runs a line (a zoomed-out line crosses the map width several times: sanjeon f3300)
	enum { PL_SEG = 16 };
	static short sg_i0[256][PL_SEG], sg_i1[256][PL_SEG];
	static float sg_ul[256][PL_SEG];
	static unsigned char sg_n[256];
	static short need_a[PLANE_HMAX], need_b[PLANE_HMAX], ra[PLANE_HMAX], rb[PLANE_HMAX];
	static unsigned char miss[PLANE_HMAX];
	for (int j = 0; j < PH; j++) {
		need_a[j] = PW;
		need_b[j] = 0;
	}
	const int xwrap = c->xmask + 1, w = s->w;
	for (int y = 0; y < h; y++) {
		const RstvCapRow *r = &c->row[y];
		sg_n[y] = 0;
		if (!r->enable)
			continue;
		const float zx = r->zx;
		const int lsx = r->lsx & (xwrap - 1); // ADR-099: wraps over the map width
		const int pr = r->y & (PH - 1);
		int i0 = 0;
		while (i0 < w) {
			// next wrap: the integer map x reaches the next multiple of xwrap, or (x & (xwrap - 1)) + lsx reaches xwrap.
			// Estimate the pixel from the line equation, then settle it with the exact per-pixel formula
			int i = w;
			if (zx > 0.0f) {
				const int f0i = (int)((float)r->x + (float)i0 * zx);
				const int fm = f0i & (xwrap - 1);
				int bnd = f0i - fm + xwrap; // next multiple of xwrap
				if (lsx && fm < xwrap - lsx && f0i - fm + (xwrap - lsx) < bnd)
					bnd = f0i - fm + (xwrap - lsx);
				int ib = (int)(((float)bnd - (float)r->x) / zx);
				if (ib <= i0)
					ib = i0 + 1;
				while (ib > i0 + 1 && (int)((float)r->x + (float)(ib - 1) * zx) >= bnd)
					ib--;
				while (ib < w && (int)((float)r->x + (float)ib * zx) < bnd)
					ib++;
				if (ib < i)
					i = ib;
			}
			const float f0 = (float)r->x + (float)i0 * zx;
			int g0 = ((int)f0) & (xwrap - 1);
			if (lsx)
				g0 = (g0 + lsx) & (xwrap - 1);
			const float ul = f0 + (float)(g0 - (int)f0) + (1.0f / 256.0f) - 0.5f * zx;
			if (sg_n[y] >= PL_SEG)
				return (s_plane_rej[L] = 6, 0);
			sg_i0[y][sg_n[y]] = (short)i0;
			sg_i1[y][sg_n[y]] = (short)i;
			sg_ul[y][sg_n[y]] = ul;
			sg_n[y]++;
			int lo = (int)ul - 1, hi = (int)(ul + (float)(i - i0) * zx) + 2;
			if (lo < 0) lo = 0;
			if (hi > PW) hi = PW;
			if (lo < need_a[pr]) need_a[pr] = (short)lo;
			if (hi > need_b[pr]) need_b[pr] = (short)hi;
			i0 = i;
		}
	}
	pp1 = RSTV_PROF_T();
	// pass 1: plane rows that are missing or lack columns; ranges in 64 pixel steps so neighbouring rows share them
	// (and their cell quads merge)
	int nmiss = 0;
	for (int j = 0; j < PH; j++) {
		miss[j] = 0;
		if (need[j] < 0 || need_b[j] <= need_a[j])
			continue;
		// ADR-080: the whole plane width. Per-row ranges (64-pixel steps) differ from row to row on a zoomed layer, so
		// each row was its own group of one-row quads, and every frame that needed a few more columns drew it again
		// (sanjeon's dance stage: about 100 rows a frame, prep 9 -> 22ms)
		int a = fullw ? 0 : need_a[j] & ~63, b = fullw ? PW : (need_b[j] + 63) & ~63;
		if (b > PW)
			b = PW;
		const int same = s_plane_tag[L][j] == need[j];
		// ADR-100: a row holding the columns the lines need is kept (rows drawn narrow after a page change are not drawn
		// again at full width the next frame)
		if (same && s_plane_c0[L][j] <= need_a[j] && s_plane_c1[L][j] >= need_b[j])
			continue;
		if (s_plane_tag[L][j] < 0) pp_fresh++; else if (!same) pp_tag++; else pp_cols++;
		if (same) {
			if (s_plane_c0[L][j] < a) a = s_plane_c0[L][j];
			if (s_plane_c1[L][j] > b) b = s_plane_c1[L][j];
		}
		ra[j] = (short)a;
		rb[j] = (short)b;
		miss[j] = 1;
		nmiss++;
	}
	if (nmiss) {
		// ADR-080: a cell row (8 plane rows) with a missing row is drawn whole, so the rows join into one group of
		// 8-row cell quads. A zoomed-out line pattern needs every 2nd or 3rd map row: each was a group of one-row quads.
		// ADR-100: without fullw (2048-wide plane) the cell row gets the union of its rows' column ranges (twcup98's
		// halved hi-res lines need every 2nd map row, 704 columns: one-row quads passed the 16384 quad limit)
		for (int b8 = 0; b8 < PH; b8 += 8) {
			int jm = -1, ua = PW, ub = 0;
			for (int k = b8; k < b8 + 8; k++)
				if (miss[k]) {
					if (jm < 0)
						jm = k;
					if (ra[k] < ua) ua = ra[k];
					if (rb[k] > ub) ub = rb[k];
				}
			if (jm < 0)
				continue;
			if (fullw)
				ua = 0, ub = PW;
			for (int k = b8; k < b8 + 8; k++) {
				if (!miss[k]) {
					if (need[k] < 0)
						need[k] = (need[jm] & ~7) | (k & 7); // the map row of the same cell row
					miss[k] = 1;
					nmiss++;
				} else if (need[k] != ((need[jm] & ~7) | (k & 7)))
					continue; // a different cell row of the map on this plane cell row: keep its own range
				ra[k] = (short)ua;
				rb[k] = (short)ub;
			}
		}
	}
	if (nmiss) {
		float *cv = (float *)gx_temp(PH * 8 * (int)sizeof(float));
		if (!cv)
			return (s_plane_rej[L] = 7, 0);
		int nb = 0, n1 = 0;
		nbgq_track = 1;
		memset(nbgq_track_pages, 0, sizeof(nbgq_track_pages));
		for (int j = 0; j < PH;) {
			if (!miss[j]) {
				j++;
				continue;
			}
			int e = j + 1;
			// ADR-100: at most RSTV_CAP_ROWS rows a group (s_plane_cap.row; a whole 512-row plane overran it)
			while (e < PH && e - j < PLANE_GROUP_ROWS && miss[e] && ra[e] == ra[j] && rb[e] == rb[j])
				e++;
			// rows j .. e - 1, columns [ra, rb): a capture whose rows start at the group
			memcpy(&s_plane_cap, c, offsetof(RstvLayerCapture, row));
			s_plane_cap.xmask = PW - 1; // columns past the map width are what line scroll reaches (cell_at reads them)
			s_plane_cap.has_win = 0;
			s_plane_cap.zx = 0.0f;
			s_plane_cap.nrows = e - j;
			for (int k = 0; k < e - j; k++) {
				RstvCapRow *q = &s_plane_cap.row[k];
				*q = c->row[first];
				q->enable = 1;
				q->x = (u16)ra[j];
				q->lsx = 0;
				q->y = (u16)need[j + k];
				q->zx = 1.0f;
				q->nwin = 0;
			}
			uint32_t gpg[4];
			memcpy(gpg, nbgq_track_pages, sizeof(gpg));
			memset(nbgq_track_pages, 0, sizeof(nbgq_track_pages));
			memset(nbgq_track_sub, 0, sizeof(nbgq_track_sub));
			const int n = nbgq_build(&s_plane_cap, rb[j] - ra[j], e - j, s_snap_vram, s_qtmp + n1, PLQ - n1, NBGQ_NO_WINDOW);
			if (n < 0) {
				nbgq_track = 0;
				s_plane_valid[L] = 0;
				return (s_plane_rej[L] = 8, 0);
			}
			for (int k = n1; k < n1 + n; k++) {
				s_qtmp[k].x0 = (int16_t)(s_qtmp[k].x0 + ra[j]);
				s_qtmp[k].x1 = (int16_t)(s_qtmp[k].x1 + ra[j]);
				s_qtmp[k].y0 = (int16_t)(s_qtmp[k].y0 + j);
				s_qtmp[k].y1 = (int16_t)(s_qtmp[k].y1 + j);
			}
			n1 += n;
			const float x0 = -1.0f + 2.0f * ra[j] / PW, x1 = -1.0f + 2.0f * rb[j] / PW;
			const float y0 = -1.0f + 2.0f * j / PH, y1 = -1.0f + 2.0f * e / PH;
			const float d[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
			memcpy(cv + nb * 8, d, sizeof(d));
			nb++;
			PlSub gs;
			gs.n = 0;
			for (int pg = 0; pg < 128 && gs.n != 255; pg++)
				if (nbgq_track_pages[pg >> 5] & (1u << (pg & 31))) {
					if (gs.n == PL_SUBN)
						gs.n = 255;
					else {
						gs.pg[gs.n] = (uint8_t)pg;
						gs.m[gs.n++] = nbgq_track_sub[pg];
					}
				}
			for (int k = j; k < e; k++) {
				s_plane_tag[L][k] = need[k];
				s_plane_sub[L][k] = gs;
				memcpy(s_plane_rpg[L][k], nbgq_track_pages, sizeof(s_plane_rpg[L][k]));
				s_plane_c0[L][k] = ra[j];
				s_plane_c1[L][k] = rb[j];
			}
			for (int k = 0; k < 4; k++) // the aggregate of every group (s_plane_pages below)
				nbgq_track_pages[k] |= gpg[k];
			j = e;
		}
		nbgq_track = 0;
		pp2 = RSTV_PROF_T();
		pp_groups = nb;
		pp_quads = n1;
		void *v = n1 > 0 ? gx_temp(n1 * 4 * 8 * (int)sizeof(int16_t)) : NULL;
		if (n1 > 0 && !v) {
			s_plane_valid[L] = 0;
			return (s_plane_rej[L] = 9, 0);
		}
		uint32_t bk[4];
		quads_to_vtx(s_qtmp, n1, c->colornumber, bk);
		if (n1 > 0)
			memcpy(v, s_qvtx, (size_t)n1 * 4 * 8 * sizeof(int16_t));
		for (int k = 0; k < 4; k++) {
			s_plane_pages[L][k] |= nbgq_track_pages[k];
			s_plane_bank[L][k] |= bk[k];
		}
		s->pl_v[L] = v;
		s->pl_n[L] = n1;
		s->pl_cv[L] = cv;
		s->pl_cn[L] = nb;
		s_plane_bankh[L] = bank_hash(s_plane_bank[L]);
		s_plane_stamp[L] = s_snap_serial;
		s_plane_gen[L]++;
		s_stats.plane_draws++;
		s_stats.plane_rows += (unsigned)nmiss;
		pp3 = RSTV_PROF_T();
	}
	// pass 2 quads
	float *lv = (float *)gx_temp(h * 2 * PL_SEG * 16 * (int)sizeof(float));
	if (!lv)
		return (s_plane_rej[L] = 10, 0);
	int ln = 0;
	uint32_t vh = 2166136261u;
	for (int y = 0; y < h; y++) {
		const RstvCapRow *r = &c->row[y];
		if (!r->enable)
			continue;
		const float pv = (float)(r->y & (PH - 1));
		const float zx = r->zx;
		for (int sgi = 0; sgi < sg_n[y]; sgi++) {
			const int i0 = sg_i0[y][sgi], i = sg_i1[y][sgi];
			const float ul = sg_ul[y][sgi];
			const int nr = r->nwin ? r->nwin : 1;
			if (ln + nr > h * 2 * PL_SEG)
				return (s_plane_rej[L] = 11, 0);
			for (int k = 0; k < nr; k++) {
				int a = i0, b = i;
				if (r->nwin) {
					if (a < r->wx0[k]) a = r->wx0[k];
					if (b > r->wx1[k] + 1) b = r->wx1[k] + 1;
					if (a >= b)
						continue;
				}
				const float ua = ul + (float)(a - i0) * zx, ub = ul + (float)(b - i0) * zx;
				const float d[16] = { (float)a, (float)y, ua, pv, (float)b, (float)y, ub, pv,
					(float)a, (float)(y + 1), ua, pv + 1.0f, (float)b, (float)(y + 1), ub, pv + 1.0f };
				memcpy(lv + ln * 16, d, sizeof(d));
				vh = mixw(vh, d, sizeof(d));
				ln++;
			}
		}
	}
	s->pl_lv[L] = lv;
	s->pl_ln[L] = ln;
	s->pl_on[L] = 1;
	s->quad_n[L] = ln;
	s->quad_cn[L] = c->colornumber;
	s->qclr_n[L] = 0;
	float (*u)[4] = s->quad_cfg[L];
	u[0][0] = c->transparencyenable ? 1.0f : 0.0f;
	u[0][1] = (float)c->specialcolormode;
	u[0][2] = ((c->alpha & 0xFF) != 63) ? 1.0f : 0.0f;
	u[0][3] = (float)L;
	for (int b = 0; b < 4; b++) {
		u[1][b] = (float)((c->specialcode >> b) & 1);
		u[2][b] = (float)((c->specialcode >> (4 + b)) & 1);
	}
	// ADR-057: the layer is the same while the copy quads and the plane are
	MIXV(vh, s_plane_key[L]);
	MIXV(vh, s_plane_gen[L]);
	s->lvh[L] = vh;
	memcpy(s->lbank[L], s_plane_bank[L], sizeof(s->lbank[L]));
	s->lok[L] = 1;
	{
		const u64 pp4 = RSTV_PROF_T();
		if (pp4 - pp0 > 6000) // diag: slow plane prep (if logged often, the logging itself costs)
			RSTV_LOG("plane L%d f%u: dirty %d rows fresh %d tag %d cols %d | need %u miss %u (groups %d quads %d) build %u vtx %u us",
				L, s_frame, pp_dirty, pp_fresh, pp_tag, pp_cols, (unsigned)(pp1 - pp0), (unsigned)(pp2 - pp1), pp_groups, pp_quads,
				(unsigned)(pp3 - pp2), (unsigned)(pp4 - (pp3 > pp1 ? pp3 : pp1)));
	}
	return 1;
}

// ADR-077: the half-width RBG0 drawing -> the layer texture (each texel to two pixels, point sampling)
static void rbg_half_copy(const FrameSet *s, GxTex *tgt)
{
	float *v = (float *)gx_temp(16 * sizeof(float));
	gx_begin(tgt);
	if (v) {
		const float W = (float)s->w, H = (float)(s->h < LAYER_H ? s->h : LAYER_H);
		const float d[16] = { 0, 0, 0, 0, W, 0, W * 0.5f, 0, 0, H, 0, H, W, H, W * 0.5f, H };
		memcpy(v, d, sizeof(d));
		const float cscale[4] = { 2.0f / LAYER_W, 2.0f / LAYER_H, 2.0f / LAYER_W, 1.0f / LAYER_H };
		gx_use(s_prog_pcopy);
		gx_set4(s_pc_scale, cscale);
		gx_set_tex(s_pc_tex, s_rbg_halftex);
		gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, v, s_qidx, 6);
	}
	gx_end();
}

static void clear_target(void)
{
	float *v = (float *)gx_temp(8 * sizeof(float));
	if (!v)
		return;
	const float d[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	memcpy(v, d, sizeof(d));
	gx_use(s_prog_clear);
	gx_draw(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, gx_seq_indices(4), 4);
}

static void prep_b(FrameSet *s, u8 *front, u64 tp);
static void prep_wait(void);
static void draw_end_work(int cur);
static int s_end_defer, s_end_pend = -1; // ADR-186


static void gvdp2_draw_screens(void)
{
	{ // diag (ux0:data/restv/v1log "from to"): which frames composite and whether they take a VDP1 list
		extern int rstv_v1log_n;
		extern unsigned rstv_v1log_from, rstv_v1log_to;
		if (rstv_v1log_n > 0 && (unsigned)yabsys.frame_count >= rstv_v1log_from && (unsigned)yabsys.frame_count <= rstv_v1log_to)
			RSTV_LOG("v1take f%u ready %d tgt %d back %d drawn %d %d TVMD %04x BGON %04x", (unsigned)yabsys.frame_count, rstv_v1ready,
				rstv_v1ready_target, rstv_vdp1_back_idx, rstv_vdp1_gpu_drawn[0], rstv_vdp1_gpu_drawn[1], Vdp2Regs->TVMD, Vdp2Regs->BGON);
	}
	if (s_end_pend >= 0) { // ADR-186: finish the previous frame (its prep is long done)
		draw_end_work(s_end_pend);
		s_end_pend = -1;
	}
	prep_wait(); // if the previous frame's prep is still pending
#ifdef RSTV_DEBUG_TOOLS
	{ // debug fast boot (main.c): no compose
		extern int rstv_fastboot_now;
		if (rstv_fastboot_now) {
			rstv_vdp1_gpu_on = s_prog_v1 != NULL; // VDP1 lists are recorded (and dropped), not drawn on the CPU
			return;
		}
	}
#endif
	if (s_gpu_finish_from && s_frame >= s_gpu_finish_from)
		s_gpu_finish = 1;
	int vdp1w, vdp1h, vdp1ps;
	u8 *front;
	rstv_vidsoft_vdp1_info(&vdp1w, &vdp1h, &vdp1ps, &front);

	gx_frame_begin(); // until the GPU has finished reading this set (frame fence)
	int reason = s_ok ? unsupported_reason(vdp1w, vdp1ps) : 99;
	FrameSet *s = &s_set[(s_cur + 1) % NSETS];
	s_cur = (s_cur + 1) % NSETS;
	s->gpu = (reason == 0);
	// VDP1 GPU renderer (ADR-023): recorded commands to triangles, VDP1 RAM copied into this set's texture for changed 4KB pages only
	s->v1_nv = 0;
	s->v1_pending = 0;
	if (rstv_v1ready && s_prog_v1) {
		// recorded list and VDP1 RAM copy (changed 4KB pages only). Vertices/cache in prep_b (ADR-025)
		rstv_v1ready = 0;
		extern u32 rstv_vdp1_serial, rstv_vdp1_page_stamp[128];
		extern int rstv_v1_fullsnap;
		for (int p = 0; p < 128; p++)
			if (!s_v1snap_valid || rstv_v1_fullsnap || rstv_vdp1_page_stamp[p] > s_v1snap_serial) {
				memcpy(s_v1snap_ram + p * 4096, Vdp1Ram + p * 4096, 4096);
				s_v1snap_stamp[p] = rstv_vdp1_serial;
			}
		s_v1snap_valid = 1;
		s_v1snap_serial = rstv_vdp1_serial++;
		memcpy(s_v1snap_list, rstv_v1list, (size_t)rstv_v1n * sizeof(RstvV1Item));
		s_v1snap_n = rstv_v1n;
		s_v1snap_fmt = rstv_v1list_fmt;
		s->v1_pending = 1;
		s->v1_target = rstv_v1ready_target;
	}
	s->spr_front = rstv_vdp1_back_idx ^ 1;
	s->spr_gpu = s_prog_v1 && rstv_vdp1_gpu_drawn[s->spr_front];
	s->v1_clear = -1;
	{
		// ADR-149: lists drawn while the compose did not run (VDP2 display off at a scene change) were dropped, but their
		// erase is real: VDP1 kept erasing. The buffer shown now still held the old scene's sprites over the new scene
		// (vfremix after the character select). Clear it before this frame's compose
		// ADR-154: only for a dropped list's erase (rstv_v1_dropped_erase), not for manual erase requests: vfremix makes
		// one every frame and this cleared the fighters away
		extern u8 rstv_v1_dropped_erase[2];
		if (s_prog_v1 && s->spr_gpu && rstv_v1_dropped_erase[s->spr_front] && !(s->v1_pending && s->v1_target == s->spr_front)) {
			s->v1_clear = s->spr_front;
			rstv_v1_dropped_erase[s->spr_front] = 0;
		}
	}
	s->spr8 = vdp1ps == 1;
	// ADR-100: sprite layer window 0 rectangle -> the parts of the sprite texture it hides are cleared before the compose
	s->swin_n = 0;
	s->swin_lw = 0;
	if (s->gpu && ((Vdp2Regs->WCTLC >> 8) & 0x0a) && !sprite_window_whole(Vdp2Regs) &&
		sprite_window_rect(Vdp2Regs, &s->swin_x0, &s->swin_y0, &s->swin_x1, &s->swin_y1, &s->swin_in)) {
		const int W = s->w < 512 ? s->w : 512, H = s->h < 256 ? s->h : 256;
		int x0 = s->swin_x0, x1 = s->swin_x1 + 1, y0 = s->swin_y0, y1 = s->swin_y1 + 1; // [x0, x1) x [y0, y1)
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > W) x1 = W;
		if (y1 > H) y1 = H;
		int rc[4][4], n = 0;
		const u32 lwa = sprite_window_lwta(Vdp2Regs, sprite_window_one(Vdp2Regs));
		s->swin_lw = lwa != 0;
		if (lwa) {
			// ADR-117: line window. x span per line from the table (vidsoft ReadLineWindowClip from line 0), y from the
			// rectangle; clear quads per run of lines with the same span
			u32 a = lwa - 1;
			extern int rstv_half_y;
			for (int y = 0; y < H; y++) {
				clipping_struct c = { 0 };
				ReadOneLineWindowClip(&c, &a, Vdp2Ram, Vdp2Regs);
				if (rstv_half_y)
					a += 4; // one table entry per full-height line (vidsoft ReadLineWindowClip)
				int lx0 = c.xstart, lx1 = c.xend + 1;
				if (y < y0 || y >= y1 || lx0 >= lx1) lx0 = lx1 = 0; // empty on this line
				if (lx0 < 0) lx0 = 0;
				if (lx1 > W) lx1 = W;
				if (lx0 > lx1) lx0 = lx1;
				s->swin_lx0[y] = (int16_t)lx0;
				s->swin_lx1[y] = (int16_t)lx1;
			}
			float *cv = (float *)gx_temp(2 * H * 8 * (int)sizeof(float));
			int m = 0;
			for (int y = 0; cv && y < H;) {
				int e = y + 1;
				while (e < H && s->swin_lx0[e] == s->swin_lx0[y] && s->swin_lx1[e] == s->swin_lx1[y])
					e++;
				int b[2][2], nb = 0;
				const int lx0 = s->swin_lx0[y], lx1 = s->swin_lx1[y];
				if (s->swin_in) { // visible inside: clear left and right of the span (the whole line if empty)
					if (lx0 >= lx1) { b[nb][0] = 0; b[nb][1] = W; nb++; }
					else {
						if (lx0 > 0) { b[nb][0] = 0; b[nb][1] = lx0; nb++; }
						if (lx1 < W) { b[nb][0] = lx1; b[nb][1] = W; nb++; }
					}
				} else if (lx0 < lx1) { b[nb][0] = lx0; b[nb][1] = lx1; nb++; }
				for (int k = 0; k < nb; k++) {
					const float qa = -1.0f + 2.0f * b[k][0] / 512.0f, qb = -1.0f + 2.0f * b[k][1] / 512.0f;
					const float qc = -1.0f + 2.0f * y / 256.0f, qd = -1.0f + 2.0f * e / 256.0f;
					const float q[8] = { qa, qc, qb, qc, qa, qd, qb, qd };
					memcpy(cv + m * 8, q, sizeof(q));
					m++;
				}
				y = e;
			}
			for (int y = H; y < 256; y++)
				s->swin_lx0[y] = s->swin_lx1[y] = 0;
			s->swin_cv = cv;
			s->swin_n = cv ? m : 0;
		} else if (x0 >= x1 || y0 >= y1) {
			if (!s->swin_in) n = -1;                         // empty window, hidden inside: nothing hidden
			else { rc[0][0] = 0; rc[0][1] = 0; rc[0][2] = W; rc[0][3] = H; n = 1; }
		} else if (s->swin_in) {                             // visible inside: clear the 4 bands around it
			if (y0 > 0) { rc[n][0] = 0; rc[n][1] = 0; rc[n][2] = W; rc[n][3] = y0; n++; }
			if (y1 < H) { rc[n][0] = 0; rc[n][1] = y1; rc[n][2] = W; rc[n][3] = H; n++; }
			if (x0 > 0) { rc[n][0] = 0; rc[n][1] = y0; rc[n][2] = x0; rc[n][3] = y1; n++; }
			if (x1 < W) { rc[n][0] = x1; rc[n][1] = y0; rc[n][2] = W; rc[n][3] = y1; n++; }
		} else {
			rc[0][0] = x0; rc[0][1] = y0; rc[0][2] = x1; rc[0][3] = y1; n = 1;
		}
		if (n > 0 && (s->swin_cv = (float *)gx_temp(n * 8 * (int)sizeof(float))) != NULL) {
			for (int k = 0; k < n; k++) {
				const float a = -1.0f + 2.0f * rc[k][0] / 512.0f, b = -1.0f + 2.0f * rc[k][2] / 512.0f;
				const float c = -1.0f + 2.0f * rc[k][1] / 256.0f, d = -1.0f + 2.0f * rc[k][3] / 256.0f;
				const float q[8] = { a, c, b, c, a, d, b, d };
				memcpy(s->swin_cv + k * 8, q, sizeof(q));
			}
			s->swin_n = n;
		}
		if (lwa)
			n = s->swin_n;
		s->swin_x0 = x0; s->swin_x1 = x1; s->swin_y0 = y0; s->swin_y1 = y1;
		if (n < 0)
			s->swin_n = 0;
	}
	// no capture after a CPU-composited frame (the CPU composite reads the CPU framebuffer)
	rstv_vdp1_gpu_on = s_prog_v1 && s->gpu;
	if (!s->gpu) {
		if (s->v1_pending) {
			v1gpu_fmt = s_v1snap_fmt;
			const int nv = v1gpu_build(s_v1snap_list, s_v1snap_n, s_v1buf, V1GPU_MAX_VERTS, s_v1atlas->data, s_v1snap_ram, rstv_v1_fullsnap ? NULL : s_v1snap_stamp,
				s_v1snap_serial, &s->v1f);
			void *v = nv > 0 ? gx_temp(nv * V1GPU_VERTEX_FLOATS * (int)sizeof(float)) : NULL;
			if (v) {
				memcpy(v, s_v1buf, (size_t)nv * V1GPU_VERTEX_FLOATS * sizeof(float));
				s->v1_v = v;
				s->v1_nv = nv;
			}
			s->v1_pending = 0;
		}
		s_stats.last_fallback_reason = reason;
		rstv_gpu_compose = 0;
		rstv_vidsoft_draw_back_line();
		VIDSoftVdp2DrawScreens();
		return;
	}

	// sprite window: if a background layer uses it as a window, CPU layer drawing needs a mask (same rule as VidsoftDrawSprite)
	{
		const Vdp2 *rr = Vdp2Regs;
		const int uses = ((rr->WCTLA | (rr->WCTLA >> 8) | rr->WCTLB | (rr->WCTLB >> 8) | rr->WCTLC) & 0x20) != 0;
		if ((rr->SPCTL & 0x10) && uses) {
			const int type = rr->SPCTL & 0xF;
			const int colormode = rr->SPCTL & 0x20;
			memset(sprite_window_mask, 0, 704 * 512);
			if (type >= 2 && type <= 7) {
				const u16 *fb = (const u16 *)front;
				for (int y = 0; y < vdp2height; y++)
					for (int x = 0; x < vdp2width; x++) {
						u16 p = fb[y * 512 + x];
						if ((p & 0x8000) && !colormode)
							sprite_window_mask[y * vdp2width + x] = 1;
					}
			}
		}
	}

	s_shot_now = 0;
	for (int i = 0; i < s_shots; i++)
		if (s_shot_frames[i] == s_frame)
			s_shot_now = 1;
	{
		// diag (ADR-083): with ux0:data/restv/shotunrot, capture GPU and CPU 2 frames after a non-rotated non-coefficient RBG0 first appears (up to 3 times)
		extern volatile int rstv_rbg_unrot_seen;
		static int on = -1, taken, last_seen, wait;
		if (on < 0)
			on = file_exists_q("ux0:data/restv/shotunrot");
		if (on && taken < 3) {
			if (rstv_rbg_unrot_seen != last_seen && !wait)
				wait = 2;
			last_seen = rstv_rbg_unrot_seen;
			if (wait && --wait == 0) {
				s_shot_now = 1;
				taken++;
				RSTV_LOG("gvdp2: unrotated RBG0 picture at f%u", s_frame);
			}
		}
	}
	// ADR-079: the layer pass (capture, CPU layers) runs on the render worker from the snapshot below; on the main
	// thread (live state) only when the whole preparation is done here
	const int main_prep = (s_shot_now && !s_shot_async) || !s_prep_thread_ok || s_capture_main;
	u64 tp = RSTV_PROF_T();
	rstv_vidsoft_screens_begin();
	if (main_prep) {
		rstv_vidsoft_set_inputs(NULL, NULL, NULL, NULL, NULL);
		capture_layers();
	}

	s->w = vdp2width;
	s->h = vdp2height;
	// snapshot for the worker thread (ADR-025). Write stamp base: this snapshot's serial = S, later writes start at S + 1
	s_snap_regs = *Vdp2Regs;
	memcpy(s_snap_lines, Vdp2Lines, sizeof(s_snap_lines));
	// ADR-057: a CRAM write that leaves the same colours does not count as a change (games rewrite it every frame)
	if (s_snap_cram_serial != rstv_cram_serial && memcmp(s_snap_cram, Vdp2ColorRam, sizeof(s_snap_cram)) != 0) {
		memcpy(s_snap_cram, Vdp2ColorRam, sizeof(s_snap_cram));
		s_snap_cram_serial = rstv_cram_serial;
	}
	s_snap_colormode = Vdp2Internal.ColorMode;
	memcpy(s_snap_cells, cell_scroll_data, sizeof(s_snap_cells));
	for (int p = 0; p < 128; p++)
		if (!s_snap_valid || rstv_vdp2_page_stamp[p] > s_snap_vram_serial)
			memcpy(s_snap_vram + p * 4096, Vdp2Ram + p * 4096, 4096);
	s_snap_valid = 1;
	s_snap_serial = s_snap_vram_serial = rstv_vdp2_serial;
	rstv_vdp2_serial++;
	s_stats.prep_sync += main_prep;
	if (main_prep) {
		prep_b(s, front, tp);
		return;
	}
	s_prep_s = s;
	s_prep_front = front;
	s_prep_tp = tp;
	s_prep_capture = 1;
	s_prep_pending = 1;
	sceKernelSignalSema(s_prep_go, 1);
}

// ADR-079: the vidsoft layer pass: NBG layers are recorded, RBG0 captured (or drawn into its Titan buffer on the CPU)
static void capture_layers(void)
{
	for (int L = 0; L < NUM_BG; L++)
		rstv_layer_capture[L].captured = 0;
	rstv_rot_capture.captured = 0;
	rstv_rot_why = 0;
	s_stats.cpu_mask = 0;
	const u64 tp = RSTV_PROF_T();
	rstv_gpu_compose = 1;
	rstv_capture_mode = s_prog_nbg != 0;
	rstv_vidsoft_screens_layers();
	rstv_capture_mode = 0;
	s_stats.rbg_why = (unsigned)rstv_rot_why;
	s_stats.t_core += RSTV_PROF_T() - tp;
}

// second half of layer prep (ADR-025): reads only the snapshot. On the worker thread (CPU2) or main
static u64 s_pm[4]; // diag: section timestamps in prep_b (after VDP1 vertices, after copy, after layers)
static u64 s_pc[3]; // diag: inside the copy section (after palette, after lines, after sprite vars)
static void prep_b(FrameSet *s, u8 *front, u64 tp)
{
	u64 tn;
	const Vdp2 *r = &s_snap_regs;
	if (s->v1_pending) {
		const u64 tb = RSTV_PROF_T();
		v1gpu_fmt = s_v1snap_fmt;
		const int nv = v1gpu_build(s_v1snap_list, s_v1snap_n, s_v1buf, V1GPU_MAX_VERTS, s_v1atlas->data, s_v1snap_ram, rstv_v1_fullsnap ? NULL : s_v1snap_stamp,
			s_v1snap_serial, &s->v1f);
		void *v = nv > 0 ? gx_temp(nv * V1GPU_VERTEX_FLOATS * (int)sizeof(float)) : NULL;
		if (v) {
			memcpy(v, s_v1buf, (size_t)nv * V1GPU_VERTEX_FLOATS * sizeof(float));
			s->v1_v = v;
			s->v1_nv = nv;
		}
		s->v1_pending = 0;
		s_stats.v1_gpu++;
		s_stats.v1_decoded += s->v1f.decoded_px;
		s_stats.t_v1build += RSTV_PROF_T() - tb;
	}
	s_pm[0] = RSTV_PROF_T();

	// palette re-converted only when CRAM or color mode changed (tracked per set)
	if (s->pal_serial != s_snap_cram_serial || s->pal_mode != s_snap_colormode) {
		fill_palette(s->pal_px);
		s->pal_serial = s_snap_cram_serial;
		s->pal_mode = s_snap_colormode;
	}
	s_pc[0] = RSTV_PROF_T();
	fill_lines(s);
	s_pc[1] = RSTV_PROF_T();
	// sprite framebuffer 512x256 u16 -> 256x256 RGBA8 (same byte layout)
	// copy only the lines and width used on screen (sprite framebuffer line = 512 pixels x 16 bits)
	if (!s->spr_gpu && s->spr8) {
		// ADR-100: 8-bit 1024-wide framebuffer -> every 2nd pixel (as vidsoft reads a hi-res VDP1 on a low-res screen)
		const int rows = s->h < 256 ? s->h : 256, w = s->w < 512 ? s->w : 512;
		static uint16_t row[512]; // cached row (the texture is uncached GPU memory)
		for (int y = 0; y < rows; y++) {
			const u8 *src = front + y * 1024;
			for (int x = 0; x < w; x++)
				row[x] = src[2 * x];
			memcpy(s->spr_px + y * 1024, row, (size_t)w * 2);
		}
	} else if (!s->spr_gpu) {
		const int rows = s->h < 256 ? s->h : 256;
		const int bytes = (s->w < 512 ? s->w : 512) * 2;
		for (int y = 0; y < rows; y++)
			memcpy(s->spr_px + y * 1024, front + y * 1024, bytes);
	}
	if (!s->spr_gpu && s->swin_n > 0) {
		// ADR-100: the sprite window on the CPU copy (u16 pixels, 512 a row)
		const int rows = s->h < 256 ? s->h : 256, w = s->w < 512 ? s->w : 512;
		for (int y = 0; y < rows; y++) {
			uint16_t *d = (uint16_t *)(s->spr_px + y * 1024);
			const int yin = y >= s->swin_y0 && y < s->swin_y1;
			const int sx0 = s->swin_lw ? s->swin_lx0[y] : s->swin_x0, sx1 = s->swin_lw ? s->swin_lx1[y] : s->swin_x1;
			for (int x = 0; x < w; x++) {
				const int in = yin && x >= sx0 && x < sx1;
				if (in != s->swin_in)
					d[x] = 0;
			}
		}
	}
	fill_sprite_params(s);
	s->sprE[2] = s->spr_gpu ? 1.0f : 0.0f;
	s_pc[2] = RSTV_PROF_T();
	if (s->spr_gpu && s->slut_px && s->off_ok) {
		const u64 tl = RSTV_PROF_T();
		fill_sprite_lut(s);
		s_stats.t_slut += RSTV_PROF_T() - tl;
	}
	tn = RSTV_PROF_T();
	s_stats.t_copy += tn - tp;
	tp = tn;
	s_pm[1] = tn;

	// layers: convert only those enabled this frame
	const int on[NUM_BG] = { (r->BGON & 0x8) != 0, (r->BGON & 0x4) != 0, (r->BGON & 0x2) != 0,
		(r->BGON & 0x21) != 0, (r->BGON & 0x10) != 0 };
	int any_gpu = 0;
	if (s_prog_q) {
		// character atlas: re-decode only VRAM pages written since the last update
		uint8_t dirty[128];
		uint16_t dsub[128]; // ADR-137: changed 256-byte blocks
		int nd = 0;
		// ADR-057: a written page whose bytes are the same as at the last decode is not decoded again (and does not move
		// the layer cache generation): tables in VRAM are rewritten every frame with the same values
		static uint8_t *vshadow;
		if (!vshadow)
			vshadow = (uint8_t *)calloc(1, 0x80000);
		for (int p = 0; p < 128; p++) {
			dirty[p] = !s_atlas_copied || rstv_vdp2_page_stamp[p] > s_atlas_copied;
			dsub[p] = 0;
			if (dirty[p] && vshadow && s_atlas_copied) {
				for (int b = 0; b < 16; b++)
					if (memcmp(vshadow + p * 4096 + b * 256, s_snap_vram + p * 4096 + b * 256, 256)) {
						s_vcsub[p][b] = s_snap_serial;
						dsub[p] |= (uint16_t)(1u << b);
					}
				if (!dsub[p])
					dirty[p] = 0;
			} else if (dirty[p]) {
				for (int b = 0; b < 16; b++)
					s_vcsub[p][b] = s_snap_serial;
				dsub[p] = 0xFFFF;
			}
			if (dirty[p] && vshadow) {
				memcpy(vshadow + p * 4096, s_snap_vram + p * 4096, 4096);
				s_vcstamp[p] = s_snap_serial;
			}
			nd += dirty[p];
		}
		if (nd) {
			const u64 ta = RSTV_PROF_T();
			nbgq_atlas_update(s_atlas_px[0], s_atlas_px[1], NBGQ_ATLAS, s_snap_vram, dsub);
			if (s_argb) {
				// ADR-185: the RGB atlas (4 bytes a pixel into GPU memory, 25-40 ms for a whole VRAM change) only for frames
				// that sample it: blocks pile up here, gpu_submit converts them when the frame uses RGB characters
				for (int p = 0; p < 128; p++)
					s_argb_dirty[p] |= dsub[p];
				s_argb_pend = 1;
				if (!s_submit_thr)
					argb_flush(NULL); // the submit runs later on the main thread: convert from this snapshot now
			}
			s_stats.t_atlas += RSTV_PROF_T() - ta;
			s_stats.atlas_pages += nd;
			s_cgen++;
		}
		s_atlas_copied = s_snap_serial;
	}
	for (int L = 0; L < NUM_BG; L++) {
		s->gpu_layer[L] = 0;
		s->lok[L] = 0;
		s->pl_on[L] = 0;
		s_stats.qplane[L] = 0;
		s->quad_n[L] = -1;
		s->bq_on[L] = 0;
		s_stats.qn[L] = -1;
		s_stats.qclr[L] = s_stats.qhyb[L] = 0;
		if (L == 4)
			s->rbg_n = 0, s->rbg_nb = 0;
		if (!on[L])
			continue;
		const RstvLayerCapture *c = &rstv_layer_capture[L];
		if (L == 4 && rstv_rot_capture.captured) {
			const u64 trb = RSTV_PROF_T();
			// RBG0 rotation: draw on GPU, or on CPU if not possible (not drawn in capture mode)
			const RstvRotCapture *rc = &rstv_rot_capture;
			const int h = s->h < rc->nrows ? s->h : rc->nrows;
			float *v = (s_prog_rbg && h > 0) ? (float *)gx_temp(h * 4 * 4 * 4 * (int)sizeof(float)) : NULL; // up to 4 spans per line
			float *vb = NULL;
			int okb = 1;
			uint32_t bank_b[4] = { 0, 0, 0, 0 };
			// ADR-057: with the content check (s_prog_q) the page cache sees only real changes; a page decoded now is valid
			// until a snapshot after this one changes its bytes
			const uint32_t *rbg_stamp = s_prog_q ? s_vcstamp : rstv_vdp2_page_stamp;
			const uint32_t rbg_serial = s_prog_q ? s_snap_serial + 1 : s_snap_serial;
			const int flat = rbg_flat_ok(rc);
			s->rbg_flat = flat;
			s->rbg_so_b = rc->two_maps ? rc->screenover_b : rc->screenover;
			s_stats.rbg_flat_frames += (unsigned)flat;
			s_stats.rbg_two_maps += (unsigned)(rc->two_maps != 0);
			if (v && rc->two_maps && !rc->isbitmap && s->rbg_table_b) {
				// B map page table (same page cache)
				static RstvRotCapture cb;
				memcpy(&cb, rc, offsetof(RstvRotCapture, nrows));
				memcpy(cb.planetbl, rc->planetbl_b, sizeof(cb.planetbl));
				cb.screenover = rc->screenover_b;
				vb = (float *)gx_temp(h * 4 * 4 * 4 * (int)sizeof(float));
				// in plane mode the B map is separate (own decode cache and texture), otherwise it shares page slots with A
				RbgqState *qb = flat ? &s_rbgq_b : &s_rbgq;
				okb = vb && rbgq_prepare(qb, &cb, s_snap_vram, rbg_stamp, rbg_serial, s_frame,
					flat ? s_rbg_pages_b->data : s_rbg_pages->data, s->rbg_table_b->data, flat) == 0;
				memcpy(bank_b, qb->bank_union, sizeof(bank_b));
			} else if (rc->two_maps)
				okb = 0;
			if (v && okb && (rc->isbitmap ? (s_rbg_spec != 0) : rbgq_prepare(&s_rbgq, rc, s_snap_vram, rbg_stamp, rbg_serial,
					s_frame, s_rbg_pages->data, s->rbg_table->data, flat) == 0)) {
				const float W = (float)s->w, nx = -1.0f + 2.0f * W / LAYER_W;
				int n = 0, nb = 0;
				uint32_t rvh = 2166136261u;
				for (int y = 0; y < h; y++) {
					const RstvRotLine *rl = &rc->row[y];
					if (rl->nseg) {
						// rotation parameter window (ADR-037): A or B coords per span
						const float n0 = -1.0f + 2.0f * y / LAYER_H, n1 = -1.0f + 2.0f * (y + 1) / LAYER_H;
						for (int k = 0; k < rl->nseg; k++) {
							const int b = rl->sp[k];
							if (b ? !rl->enb : !rl->enable)
								continue;
							const float x0 = b ? rl->xb0 : rl->x0, dx = b ? rl->dxb : rl->dx;
							const float y0 = b ? rl->yb0 : rl->y0, dy = b ? rl->dyb : rl->dy;
							const float xs = (float)rl->sx[k], xe = (float)rl->ex[k] + 1.0f;
							const float ns = -1.0f + 2.0f * xs / LAYER_W, ne = -1.0f + 2.0f * xe / LAYER_W;
							const float ma = x0 + (xs - 0.5f) * dx, mb = y0 + (xs - 0.5f) * dy;
							const float mc = x0 + (xe - 0.5f) * dx, md = y0 + (xe - 0.5f) * dy;
							const float d[16] = { ns, n0, ma, mb, ne, n0, mc, md, ns, n1, ma, mb, ne, n1, mc, md };
							rvh = mixw(rvh, d, sizeof(d));
							if (b && vb) {
								MIXV(rvh, 1);
								memcpy(vb + nb * 16, d, sizeof(d));
								nb++;
							} else {
								memcpy(v + n * 16, d, sizeof(d));
								n++;
							}
						}
						continue;
					}
					if (!rl->enable)
						continue;
					const float n0 = -1.0f + 2.0f * y / LAYER_H, n1 = -1.0f + 2.0f * (y + 1) / LAYER_H;
					const float mx0 = rl->x0 - 0.5f * rl->dx, my0 = rl->y0 - 0.5f * rl->dy;
					const float mx1 = mx0 + W * rl->dx, my1 = my0 + W * rl->dy;
					const float d[16] = { -1, n0, mx0, my0, nx, n0, mx1, my1, -1, n1, mx0, my0, nx, n1, mx1, my1 };
					rvh = mixw(rvh, d, sizeof(d));
					memcpy(v + n * 16, d, sizeof(d));
					n++;
				}
				if (s_rbgq.changed + s_rbgq_b.changed != s_rbgq_dec_seen) {
					s_rbgq_dec_seen = s_rbgq.changed + s_rbgq_b.changed;
					s_cgen++;
				}
				for (int w = 0; w < 4; w++)
					s->rbg_bank[w] = (rc->isbitmap ? 0xFFFFFFFFu : s_rbgq.bank_union[w]) | bank_b[w];
				s->rbg_vh = rvh;
				s->lok[4] = 1;
				s->rbg_v = v;
				s->rbg_n = n;
				s->rbg_vb = vb;
				s->rbg_nb = nb;
				s->gpu_layer[4] = 1;
				float (*u)[4] = s->rbg_cfg;
				u[0][0] = (float)rc->mapw; u[0][1] = (float)rc->maph; u[0][2] = (float)rc->screenover; u[0][3] = (float)rc->colornumber;
				u[1][0] = rc->transparencyenable ? 1.0f : 0.0f; u[1][1] = (float)(rc->coloroffset & 2047);
				u[1][2] = (float)rc->specialcolormode; u[1][3] = ((rc->alpha & 0xFF) != 63) ? 1.0f : 0.0f;
				for (int b = 0; b < 4; b++) {
					u[2][b] = (float)((rc->specialcode >> b) & 1);
					u[3][b] = (float)((rc->specialcode >> (4 + b)) & 1);
				}
				s->rbg_bmp = rc->isbitmap;
				s->rbg_cfg2[0] = (float)(rc->charaddr & 0x7FFFF);
				s->rbg_cfg2[1] = (float)rc->paladdr;
				s->rbg_cfg2[2] = (float)(rc->priority & 7);
				s->rbg_cfg2[3] = (float)rc->scf;
				if (rc->isbitmap)
					any_gpu = 1; // VRAM texture needed
				s_stats.rbg_layers++;
			} else {
				rstv_vidsoft_draw_layer(4);
				fill_layer(s, 4);
				s_stats.cpu_layers++;
				s_stats.cpu_mask |= 1u << 4;
			}
			s_stats.t_rbgprep += RSTV_PROF_T() - trb;
			tn = RSTV_PROF_T();
			s_stats.t_quad += tn - tp;
			s_stats.t_qlayer[L] += tn - tp;
			tp = tn;
			continue;
		}
		if (c->captured && c->ok && L < 4) {
			s->gpu_layer[L] = 1;
			s_stats.gpu_layers++;
			s_stats.qtry[L] = -9 - (s_prog_q ? 0 : 1) - (c->isbitmap ? 2 : 0) - (nbgq_supported(c) ? 0 : 4);
			s->nbgh_n[L] = 0;
			const int qok = s_prog_q && !c->isbitmap && nbgq_supported(c) && (c->colornumber != 3 || s_prog_qrgb);
			const int pl_ok = qok && s_plane_on && plane_prepare(s, L, c);
			if (qok && s_plane_on && !pl_ok) { // diag: log when the reject point changes
				static int last_rej[4];
				if (s_plane_rej[L] != last_rej[L]) {
					last_rej[L] = s_plane_rej[L];
					RSTV_LOG("gvdp2: plane L%d rejected at %d", L, s_plane_rej[L]);
				}
			}
			if (pl_ok) {
				s_stats.quad_layers++;
				s_stats.qn[L] = s->quad_n[L];
				s_stats.qplane[L] = 1;
			} else if (qok) {
				const u64 tqb0 = RSTV_PROF_T();
				// ADR-052: zoomed lines (zx != 1) are left to the pixel shader, the rest become cell quads
				const int qflags = c->zx != 0.0f ? NBGQ_SKIP_ZOOM | NBGQ_ZOOM_RUNS : 0;
				int n = nbgq_build(c, s->w, s->h < 256 ? s->h : 256, s_snap_vram, s_qtmp, MAXQ, qflags);
				s_last_qn[L] = n < 0 ? 1 << 30 : n; // -1: more than MAXQ
				s_stats.qtry[L] = n;
				s->qclr_n[L] = 0;
				if (n < 0 && c->has_win) {
					// ADR-052: windows that change every line need too many clipped quads: draw the cells whole, then clear
					// outside the window runs
					float *cv = (float *)gx_temp(1024 * 8 * (int)sizeof(float));
					const int nc = cv ? build_win_clear(c, s->w, s->h < 256 ? s->h : 256, cv, 1024) : -1;
					if (nc >= 0) {
						n = nbgq_build(c, s->w, s->h < 256 ? s->h : 256, s_snap_vram, s_qtmp, MAXQ, NBGQ_NO_WINDOW | qflags);
						s->qclr_v[L] = cv;
						s->qclr_n[L] = nc;
					}
				}
				const u64 tqb1 = RSTV_PROF_T();
				s_stats.t_qbuild += tqb1 - tqb0;
				void *v = n > 0 ? gx_temp(n * 4 * 8 * (int)sizeof(int16_t)) : NULL;
				if (n == 0 || (n > 0 && v)) {
					s->quad_v[L] = v;
					int16_t *d = s_qvtx;
					uint32_t *bk = s->lbank[L];
					bk[0] = bk[1] = bk[2] = bk[3] = 0;
					const int pal_cells = c->colornumber <= 2; // RGB cells use no palette
					uint32_t base_bk[4] = { 0, 0, 0, 0 };
					for (int k = 0; k < n; k++) {
						const NbgQuad *q = &s_qtmp[k];
						if (pal_cells) {
							const unsigned bank = (unsigned)(q->palbase >> 4) & 127;
							base_bk[bank >> 5] |= 1u << (bank & 31);
						}
						const int16_t xs[4] = { q->x0, q->x1, q->x0, q->x1 }, ys[4] = { q->y0, q->y0, q->y1, q->y1 };
						const int16_t us[4] = { q->u0, q->u1, q->u0, q->u1 }, vs[4] = { q->v0, q->v0, q->v1, q->v1 };
						for (int j = 0; j < 4; j++, d += 8) {
							d[0] = xs[j]; d[1] = ys[j]; d[2] = us[j]; d[3] = vs[j];
							d[4] = (int16_t)q->palbase; d[5] = q->prio; d[6] = q->scf; d[7] = 0;
						}
					}
					if (pal_cells) {
						// banks one cell can reach from its start: 16 colours 1, 256 colours 16, 2048 colours all
						const int nb16 = c->colornumber == 0 ? 1 : c->colornumber == 1 ? 16 : 128;
						for (unsigned b = 0; b < 128; b++)
							if (base_bk[b >> 5] & (1u << (b & 31)))
								for (int j = 0; j < nb16; j++) {
									const unsigned bank = (b + (unsigned)j) & 127;
									bk[bank >> 5] |= 1u << (bank & 31);
								}
					}
					const u64 tqb2 = RSTV_PROF_T();
					s_stats.t_qfill += tqb2 - tqb1;
					if (n > 0)
						memcpy(v, s_qvtx, (size_t)n * 4 * 8 * sizeof(int16_t));
					s_stats.t_qcopy += RSTV_PROF_T() - tqb2;
					s->quad_n[L] = n;
					s->lvh[L] = mixw(2166136261u, s_qvtx, (size_t)n * 4 * 8 * sizeof(int16_t));
					s->lok[L] = 1;
					s_stats.qn[L] = n;
					s_stats.qclr[L] = s->qclr_n[L];
					s->quad_cn[L] = c->colornumber;
					if (qflags & NBGQ_SKIP_ZOOM) {
						// zoomed lines: pixel shader line quads (window runs kept), drawn after the cells
						fill_nbg(s, L, c);
						any_gpu = 1;
						const int hh = c->nrows < s->h ? c->nrows : s->h;
						float *hv = (float *)gx_temp(hh * 2 * 16 * (int)sizeof(float));
						int hn = 0;
						for (int y = 0; hv && y < hh; y++) {
							const RstvCapRow *r = &c->row[y];
							if (!r->enable || r->zx == 1.0f || NBGQ_RUN_ZX(r->zx))
								continue;
							const int nr = r->nwin ? r->nwin : 1;
							for (int k = 0; k < nr; k++) {
								const float x0 = r->nwin ? (float)r->wx0[k] : 0.0f, x1 = r->nwin ? (float)r->wx1[k] + 1.0f : (float)s->w;
								const float n0 = -1.0f + 2.0f * x0 / LAYER_W, n1 = -1.0f + 2.0f * x1 / LAYER_W;
								const float m0 = -1.0f + 2.0f * y / LAYER_H, m1 = -1.0f + 2.0f * (y + 1) / LAYER_H;
								const float d[16] = { n0, m0, x0, (float)y, n1, m0, x1, (float)y, n0, m1, x0, (float)(y + 1), n1, m1, x1, (float)(y + 1) };
								memcpy(hv + hn * 16, d, sizeof(d));
								hn++;
							}
						}
						s->nbgh_v[L] = hv;
						s->nbgh_n[L] = hv ? hn : 0;
						s_stats.qhyb[L] = s->nbgh_n[L];
						if (s->nbgh_n[L] > 0)
							s->lok[L] = 0;
					}
					float (*u)[4] = s->quad_cfg[L];
					u[0][0] = c->transparencyenable ? 1.0f : 0.0f;
					u[0][1] = (float)c->specialcolormode;
					u[0][2] = ((c->alpha & 0xFF) != 63) ? 1.0f : 0.0f;
					u[0][3] = (float)L; // layer order (key)
					for (int b = 0; b < 4; b++) {
						u[1][b] = (float)((c->specialcode >> b) & 1);
						u[2][b] = (float)((c->specialcode >> (4 + b)) & 1);
					}
					s_stats.quad_layers++;
				}
			}
			if (s->quad_n[L] < 0) {
				any_gpu = 1;
				fill_nbg(s, L, c);
				if (c->isbitmap) {
					// ADR-123: one priority on every drawn line -> final pixels and line runs, else the dots for the shader
					int prio = -1;
					const int hh = c->nrows < s->h ? c->nrows : s->h;
					for (int y = 0; y < hh && prio != -2; y++)
						if (c->row[y].enable) {
							const int p = c->row[y].priority & 15;
							prio = prio < 0 ? p : (p == prio ? prio : -2);
						}
					if (prio >= 0 && s_prog_pcopy && bmp_texture(s, L, c, prio) && bmp_runs(s, L, c)) {
						s->bq_on[L] = 1;
						s->bq_cn[L] = c->colornumber;
					}
					else if (prio != -1)
						bmp_texture(s, L, c, -1);
				}
				s->nbgw_v[L] = NULL;
				s->nbgw_n[L] = 0;
				if (c->has_win) {
					// window (ADR-040): quads only for the visible span of each line
					const int hh = c->nrows < s->h ? c->nrows : s->h;
					float *wv = (float *)gx_temp(hh * 2 * 16 * (int)sizeof(float));
					int wn = 0;
					for (int y = 0; wv && y < hh; y++) {
						const RstvCapRow *r = &c->row[y];
						if (!r->enable)
							continue;
						const int nr = r->nwin ? r->nwin : 1;
						for (int k = 0; k < nr; k++) {
							const float x0 = r->nwin ? (float)r->wx0[k] : 0.0f, x1 = r->nwin ? (float)r->wx1[k] + 1.0f : (float)s->w;
							const float n0 = -1.0f + 2.0f * x0 / LAYER_W, n1 = -1.0f + 2.0f * x1 / LAYER_W;
							const float m0 = -1.0f + 2.0f * y / LAYER_H, m1 = -1.0f + 2.0f * (y + 1) / LAYER_H;
							const float d[16] = { n0, m0, x0, (float)y, n1, m0, x1, (float)y, n0, m1, x0, (float)(y + 1), n1, m1, x1, (float)(y + 1) };
							memcpy(wv + wn * 16, d, sizeof(d));
							wn++;
						}
					}
					if (wv) {
						s->nbgw_v[L] = wv;
						s->nbgw_n[L] = wn;
					}
				}
			}
			tn = RSTV_PROF_T();
			s_stats.t_quad += tn - tp;
			s_stats.t_qlayer[L] += tn - tp;
			if (tn - tp > 6000) // diag: slow layer prep
				RSTV_LOG("layer L%d f%u: %u us plane %d qn %d hyb %d nz %d rej %d xm %d pw %d", L, s_frame, (unsigned)(tn - tp), s_stats.qplane[L], s_stats.qn[L], s_stats.qhyb[L], (int)(c->zx * 100), s_plane_rej[L], c->xmask, s_plane_w[L]);
			tp = tn;
		} else {
			if (c->captured) {
				rstv_vidsoft_draw_layer(L); // layers the GPU cannot draw are redrawn on CPU
				s_stats.cpu_layers++;
				s_stats.cpu_mask |= 1u << L;
				s_stats.nbg_why = (unsigned)c->why;
				tn = RSTV_PROF_T();
				s_stats.t_core += tn - tp;
				tp = tn;
			}
			fill_layer(s, L);
			tn = RSTV_PROF_T();
			s_stats.t_fill += tn - tp;
			tp = tn;
		}
	}
	rstv_gpu_compose = 0;
	s_pm[2] = RSTV_PROF_T();
	if (any_gpu) {
		// copy only 4KB pages written since the last copy (vdp2.cpp write stamps)
		for (int p = 0; p < 128; p++)
			if (!s->vram_copied || rstv_vdp2_page_stamp[p] > s->vram_copied)
				memcpy(s->vram_px + p * 4096, s_snap_vram + p * 4096, 4096);
		s->vram_copied = s_snap_serial;
	}
	s_stats.t_copy += RSTV_PROF_T() - tp;
	if (s_shot_now && !s_shot_async) {
		rstv_vidsoft_draw_back_line();
		VIDSoftVdp2DrawScreens(); // for comparison: redraw via the CPU path (incl. offset and sprites, overwrites Titan buffers)
	}
	s->layerOn[0] = on[0]; s->layerOn[1] = on[1]; s->layerOn[2] = on[2]; s->layerOn[3] = on[3];
	for (int L = 0; L < 4; L++) // ADR-129
	{
			s->mosaic[L] = rstv_layer_capture[L].captured && rstv_layer_capture[L].mosx > 1 ? (float)rstv_layer_capture[L].mosx : 1.0f;
			s->mosaicOn[L] = s->mosaic[L] > 1.0f ? 1.0f : 0.0f;
			// ADR-161: the sprite window bit is the sprite MSB for types 2-7 without RGB sprites (vidsoft's mask rule)
			const int sw = rstv_layer_capture[L].captured && on[L] > 0.5f ? rstv_layer_capture[L].swin : 0;
			const int swbit = (r->SPCTL & 0x10) && (r->SPCTL & 0xF) >= 2 && (r->SPCTL & 0xF) <= 7 && !(r->SPCTL & 0x20);
			s->layerSw[L] = (float)(swbit && s->sprB[3] > 0.5f ? sw : 0);
			if (sw == 2 && !swbit)
				s->layerOn[L] = 0; // the mask is clear everywhere: the layer shows nowhere
		}
	s->layerAlpha[0] = layer_alpha(r, 0x8, r->CCRNB >> 8);
	s->layerAlpha[1] = layer_alpha(r, 0x4, r->CCRNB);
	s->layerAlpha[2] = layer_alpha(r, 0x2, r->CCRNA >> 8);
	s->layerAlpha[3] = layer_alpha(r, 0x1, r->CCRNA);
	cc_window(r, s);
	s->layerShad[0] = (r->SDCTL >> 3) & 1; s->layerShad[1] = (r->SDCTL >> 2) & 1;
	s->layerShad[2] = (r->SDCTL >> 1) & 1; s->layerShad[3] = r->SDCTL & 1;
	s->layerLs[0] = (r->LNCLEN >> 3) & 1; s->layerLs[1] = (r->LNCLEN >> 2) & 1;
	s->layerLs[2] = (r->LNCLEN >> 1) & 1; s->layerLs[3] = r->LNCLEN & 1;
	s->misc[0] = on[4];
	if (s_gpu_floor & 2) {
		s->layerOn[0] = s->layerOn[1] = s->layerOn[2] = s->layerOn[3] = 0;
		s->misc[0] = 0;
		s->sprB[3] = 0;
	}
	if (s_gpu_floor & 4) // for measurement: sprites only off
		s->sprB[3] = 0;
	s->misc[1] = layer_alpha(r, 0x10, r->CCRR);
	s->misc[2] = (r->SDCTL >> 4) & 1;
	s->misc[3] = (r->LNCLEN >> 4) & 1;
	s->mode[0] = (r->CCCTL & 0x100) ? 2.0f : ((r->CCCTL & 0x200) ? 1.0f : 0.0f);
	s->mode[1] = 256.0f;
	s->mode[2] = (r->SPCTL & 0x10) ? 1.0f : 0.0f;
	s->mode[3] = 0;
}

static int prep_worker(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (;;) {
		if (sceKernelWaitSema(s_prep_go, 1, NULL) < 0)
			break;
		const u64 t0 = RSTV_PROF_T();
		unsigned long long ql0[5], rb0 = s_stats.t_rbgprep, at0 = s_stats.t_atlas, pr0 = s_stats.plane_rows;
		const unsigned long long core0 = s_stats.t_core, v1b0 = s_stats.t_v1build, fl0 = s_stats.t_fill;
		for (int k = 0; k < 5; k++)
			ql0[k] = s_stats.t_qlayer[k];
		if (s_prep_capture) {
			// ADR-079: the layer pass from the snapshot (CPU-drawn layers in prep_b read it too)
			s_prep_capture = 0;
			rstv_vidsoft_set_inputs(s_snap_lines, &s_snap_regs, s_snap_vram, s_snap_cram, s_snap_cells);
			capture_layers();
			prep_b(s_prep_s, s_prep_front, s_prep_tp);
			rstv_vidsoft_set_inputs(NULL, NULL, NULL, NULL, NULL);
		} else
			prep_b(s_prep_s, s_prep_front, s_prep_tp);
		const u64 t1 = RSTV_PROF_T();
		u64 t2 = t1;
		if (s_submit_thr && s_prep_s->gpu) {
			gx_lock();
			t2 = RSTV_PROF_T();
			gpu_submit(s_prep_s, (int)(s_prep_s - s_set), 1);
			gx_unlock();
			s_prep_s->submitted = 1;
		}
		const u64 t3 = RSTV_PROF_T();
		if (t3 - t0 > 15000) // diag: prep over 15ms (golf etc. normally take 9~10ms, so it logged every frame)
			RSTV_LOG("gvdp2: slow prep f%u prep_b %u lock %u submit %u us | to_v1end %u copy %u (pal %u lines %u spr %u slut %u) layers %u rest %u | capture %u v1build %u fill %u | layers %u %u %u %u %u rbgprep %u atlas %u plane rows %u",
				s_frame, (unsigned)(t1 - t0), (unsigned)(t2 - t1), (unsigned)(t3 - t2), (unsigned)(s_pm[0] - t0), (unsigned)(s_pm[1] - s_pm[0]),
				(unsigned)(s_pc[0] - s_pm[0]), (unsigned)(s_pc[1] - s_pc[0]), (unsigned)(s_pc[2] - s_pc[1]), (unsigned)(s_pm[1] - s_pc[2]),
				(unsigned)(s_pm[2] - s_pm[1]), (unsigned)(t1 - s_pm[2]), (unsigned)(s_stats.t_core - core0),
				(unsigned)(s_stats.t_v1build - v1b0), (unsigned)(s_stats.t_fill - fl0),
				(unsigned)(s_stats.t_qlayer[0] - ql0[0]), (unsigned)(s_stats.t_qlayer[1] - ql0[1]), (unsigned)(s_stats.t_qlayer[2] - ql0[2]),
				(unsigned)(s_stats.t_qlayer[3] - ql0[3]), (unsigned)(s_stats.t_qlayer[4] - ql0[4]), (unsigned)(s_stats.t_rbgprep - rb0),
				(unsigned)(s_stats.t_atlas - at0), (unsigned)(s_stats.plane_rows - pr0));
		s_stats.t_prep_worker += RSTV_PROF_T() - t0;
		sceKernelSignalSema(s_prep_done, 1);
	}
	return 0;
}

static void prep_wait(void)
{
	if (s_prep_pending) {
		const u64 t0 = RSTV_PROF_T();
		sceKernelWaitSema(s_prep_done, 1, NULL);
		s_prep_pending = 0;
		s_stats.t_prep_wait += RSTV_PROF_T() - t0;
	}
}

static void nbg_variant_src(char *dst, size_t n, int key)
{
	snprintf(dst, n, "#define NBG_CN %d.0\n#define NBG_TR %d.0\n#define NBG_PDS %d.0\n#define NBG_PWH %d.0\n"
		"#define NBG_AUX %d.0\n#define NBG_M3FFF %d.0\n#define NBG_SCM %d.0\n#define NBG_ALPHA %d.0\n#define NBG_BMP %d.0\n"
		"#define NBG_BTEX %d.0\n%s",
		key & 7, (key >> 3) & 1, 1 + ((key >> 4) & 1), 1 + ((key >> 5) & 1), (key >> 6) & 1, (key >> 7) & 1, (key >> 8) & 3,
		((key >> 10) & 1) ? 0 : 63, (key >> 11) & 1, (key >> 12) & 1, s_fs_nbg);
}
// find/register NBG pixel path variant (inside gx_lock)
static GxProg *nbg_variant(const float (*u)[4], GxParam **loc)
{
	const int key = ((int)u[0][0] & 7) | (u[0][1] > 0.5f ? 8 : 0) | (u[0][2] > 1.5f ? 16 : 0) | (u[0][3] > 1.5f ? 32 : 0) |
		(u[1][0] > 0.5f ? 64 : 0) | (u[1][1] > 0.5f ? 128 : 0) | (((int)u[4][1] & 3) << 8) | ((int)u[1][3] != 63 ? 1024 : 0) |
		(u[6][3] > 0.5f ? 2048 : 0) | (u[6][3] > 1.5f ? 4096 : 0);
	for (int i = 0; i < s_nnvar; i++) {
		NbgVariant *v = &s_nvar_tab[i];
		if (v->key != key)
			continue;
		if (v->state == 1) {
			v->prog = gx_prog_bin(gx_compile(s_vs, 0), v->fbin, s_pix_attr, 2, 16, GX_BLEND_NONE);
			if (v->prog)
				for (int k = 0; k < N_COUNT; k++)
					v->loc[k] = gx_param(v->prog, s_nname[k]);
			v->state = v->prog ? 2 : 3;
			RSTV_LOG("gvdp2: nbg variant %03x %s", key, v->prog ? "ready" : "failed");
		}
		if (v->state != 2)
			return NULL;
		*loc = v->loc;
		return v->prog;
	}
	if (s_nnvar >= 24 || s_cmp_sema < 0)
		return NULL;
	NbgVariant *v = &s_nvar_tab[s_nnvar];
	v->src = NULL; // ADR-193: the compile worker builds the source (no allocation inside the GPU submit)
	v->key = key;
	v->fbin = NULL;
	v->prog = NULL;
	v->state = 0;
	__sync_synchronize();
	v->need_file = 1;
	s_nnvar++;
	sceKernelSignalSema(s_cmp_sema, 1);
	RSTV_LOG("gvdp2: nbg variant %03x queued", key);
	return NULL;
}

// ADR-193: variant sources, built on the compile worker
static char *nbg_variant_src_alloc(int key)
{
	const size_t n = strlen(s_fs_nbg) + 512;
	char *src = (char *)malloc(n);
	if (src)
		nbg_variant_src(src, n, key);
	return src;
}
static char *rbg_variant_src(int key)
{
	const size_t n = strlen(s_fs_rbg2);
	char *src = (char *)malloc(n + 256);
	if (src)
		snprintf(src, n + 256, "#define RBG_SO %d\n#define RBG_RGB %d\n#define RBG_TR %d\n#define RBG_SCM %d\n#define RBG_CCR %d\n#define RBG_BMP %d\n#define RBG_4BPP %d\n%s%s%s",
			key & 1, (key >> 1) & 1, (key >> 2) & 1, (key >> 3) & 3, (key >> 5) & 1, (key >> 6) & 1, (key >> 7) & 1, ((key >> 8) & 1) ? "#define RBG_FLAT 1\n" : "", ((key >> 9) & 1) ? "#define RBG_SO3 1\n" : "", s_fs_rbg2);
	return src;
}

// find/register RBG0 specialized variant (worker thread, or main on sync frames, inside gx_lock)
static GxProg *rbg_variant_key(int key, GxParam **loc);
static GxProg *rbg_variant(const FrameSet *s, GxParam **loc)
{
	const int key = (s->rbg_cfg[0][2] > 1.5f ? 1 : 0) | (s->rbg_cfg[0][3] > 2.5f ? 2 : 0) | (s->rbg_cfg[1][0] > 0.5f ? 4 : 0) |
		(((int)s->rbg_cfg[1][2] & 3) << 3) | (s->rbg_cfg[1][3] > 0.5f ? 32 : 0) | (s->rbg_bmp ? 64 : 0) |
		(s->rbg_bmp && s->rbg_cfg[0][3] < 0.5f ? 128 : 0) | (s->rbg_flat ? 256 : 0) | (s->rbg_cfg[0][2] > 2.5f ? 512 : 0);
	return rbg_variant_key(key, loc);
}
// ADR-067: map-order cell layout is used only after its variant is registered (checked at prep, requests a build if missing)
static int s_rbg_flat_on = 1;  // disable: ux0:data/restv/norbgflat
static int s_rbg_reg_key = -1; // pending registration (registered at submit)
static int rbg_flat_ok(const RstvRotCapture *rc)
{
	if (!s_rbg_flat_on || !s_rbg_spec || s_rbg_dbg || rc->isbitmap || (rc->two_maps && !s_rbg_pages_b))
		return 0;
	const int base = (rc->colornumber > 2 ? 2 : 0) | (rc->transparencyenable ? 4 : 0) |
		((rc->specialcolormode & 3) << 3) | ((rc->alpha & 0xFF) != 63 ? 32 : 0) | 256;
	const int so[2] = { rc->screenover, rc->two_maps ? rc->screenover_b : rc->screenover };
	int ok = 1;
	for (int j = 0; j < (so[1] != so[0] ? 2 : 1); j++) {
		// A (and B with its own screen-over mode, ADR-071): both flat variants must be registered
		const int key = base | (so[j] > 1 ? 1 : 0) | (so[j] == 3 ? 512 : 0);
		int found = 0;
		for (int i = 0; i < s_nrvar; i++)
			if (s_rvar[i].key == key) {
				found = 1;
				if (s_rvar[i].state == 1)
					s_rbg_reg_key = key;
				if (s_rvar[i].state != 2)
					ok = 0;
			}
		if (!found) {
			GxParam *dummy;
			(void)rbg_variant_key(key, &dummy); // build request (state 0)
			ok = 0;
		}
	}
	return ok;
}
static GxProg *rbg_variant_key(int key, GxParam **loc)
{
	for (int i = 0; i < s_nrvar; i++) {
		RbgVariant *v = &s_rvar[i];
		if (v->key != key)
			continue;
		if (v->state == 1) {
			v->prog = gx_prog_bin(gx_compile(s_vs_rbg, 0), v->fbin, s_rbg_attr, 2, 16, GX_BLEND_NONE);
			if (v->prog)
				for (int k = 0; k < R_COUNT; k++)
					v->loc[k] = gx_param(v->prog, s_rname[k]);
			v->state = v->prog ? 2 : 3;
			RSTV_LOG("gvdp2: rbg variant %02x %s", key, v->prog ? "ready" : "failed");
		}
		if (v->state != 2)
			return NULL;
		*loc = v->loc;
		return v->prog;
	}
	if (s_nrvar >= 32 || s_cmp_sema < 0)
		return NULL;
	RbgVariant *v = &s_rvar[s_nrvar];
	v->src = NULL; // ADR-193: the compile worker builds the source (no allocation inside the GPU submit)
	v->key = key;
	v->fbin = NULL;
	v->prog = NULL;
	v->state = 0;
	__sync_synchronize();
	v->need_file = 1;
	s_nrvar++;
	sceKernelSignalSema(s_cmp_sema, 1);
	RSTV_LOG("gvdp2: rbg variant %02x queued", key);
	return NULL;
}

void gvdp2_peek_times(unsigned long long *prep_worker, unsigned long long *prep_wait, unsigned long long *draw)
{
	*prep_worker = s_stats.t_prep_worker;
	*prep_wait = s_stats.t_prep_wait;
	*draw = s_stats.t_draw;
}

// variant file writes (worker thread): new variant marker file, one line of the per-game list. Writing from the main thread stalled
// hundreds of ms when the card was busy (thuntk scene change 254ms, VBlankIN)
static void variant_files(void)
{
	for (int i = 0; i < s_nnvar; i++) {
		NbgVariant *v = &s_nvar_tab[i];
		if (!v->need_file)
			continue;
		v->need_file = 0;
		char path[128];
		snprintf(path, sizeof(path), VARLIST_DIR "/nv_%08x_%03x.txt", (unsigned)fnv_str(s_fs_nbg), v->key);
		FILE *f = fopen(path, "wb");
		if (f)
			fclose(f);
	}
	for (int i = 0; i < s_nrvar; i++) {
		RbgVariant *v = &s_rvar[i];
		if (!v->need_file)
			continue;
		v->need_file = 0;
		char path[128];
		snprintf(path, sizeof(path), VARLIST_DIR "/rv_%08x_%02x.txt", (unsigned)fnv_str(s_fs_rbg2), v->key);
		FILE *f = fopen(path, "wb");
		if (f)
			fclose(f);
	}
	for (int i = 0; i < s_nvar; i++) {
		CmpVariant *v = &s_var[i];
		if (!v->need_list)
			continue;
		v->need_list = 0;
		char path[128];
		snprintf(path, sizeof(path), VARLIST_DIR "/cl_%s.txt", s_game);
		FILE *f = fopen(path, "a");
		if (f) {
			fprintf(f, "%08x\n", (unsigned)v->list_hash);
			fclose(f);
		}
	}
}

// ADR-115: VDP1 render target -> s_v1snap (point sampling, same size)
static void v1_snap_copy(GxTex *src)
{
	float *v = (float *)gx_temp(16 * sizeof(float));
	gx_begin(s_v1snap);
	if (v) {
		const float d[16] = { 0, 0, 0, 0, 512, 0, 512, 0, 0, 256, 0, 256, 512, 256, 512, 256 };
		memcpy(v, d, sizeof(d));
		const float cscale[4] = { 2.0f / 512, 2.0f / 256, 1.0f / 512, 1.0f / 256 };
		gx_use(s_prog_pcopy);
		gx_set4(s_pc_scale, cscale);
		gx_set_tex(s_pc_tex, src);
		gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, v, s_qidx, 6);
	}
	gx_end();
}

// GPU submit: VDP1 pass + GPU layers + composite (ADR-029). Done by the worker thread right after prep (inside gx_lock), or by main at VBlankIN on sync frames
static void gpu_submit(FrameSet *s, int oi, int on_worker)
{
	if (s_argb_pend)
		argb_flush(s);
	if (s->v1_clear >= 0) {
		gx_begin(s_v1rt[s->v1_clear]);
		clear_target();
		gx_end();
		s->v1_clear = -1;
	}
	if (s->v1_nv > 0) {
		const u64 tv1 = RSTV_PROF_T();
		if (s_gpu_finish)
			gx_finish();
		gx_begin(s_v1rt[s->v1_target]);
		for (int b = 0; b < s->v1f.nb; b++) {
			const V1Batch *bt = &s->v1f.batch[b];
			if (!s_v1prog[bt->variant])
				continue;
			if (V1GPU_READS_FB(bt->variant)) {
				// ADR-115: shadow/half-transparency read a copy of the framebuffer drawn so far (end the scene, copy, then continue drawing)
				gx_end();
				v1_snap_copy(s_v1rt[s->v1_target]);
				gx_begin(s_v1rt[s->v1_target]);
				s_stats.v1_snaps++;
			}
			gx_use(s_v1prog[bt->variant]);
			gx_set_tex(s_v1loc[bt->variant], s_v1atlas);
			if (V1GPU_READS_FB(bt->variant))
				gx_set_tex(s_v1snaploc[bt->variant], s_v1snap);
			gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, (float *)s->v1_v + (size_t)bt->first * V1GPU_VERTEX_FLOATS,
				gx_seq_indices(bt->count), bt->count);
		}
		gx_end();
		s_stats.v1_verts += (unsigned)s->v1_nv;
		s->v1_nv = 0;
		if (s_gpu_finish) {
			gx_finish();
			s_stats.t_gpu_v1 += RSTV_PROF_T() - tv1;
		}
	}
	if (s->swin_n > 0 && s->spr_gpu && s_prog_clear) {
		// ADR-100: sprite layer window: clear what it hides on the VDP1 buffer the compose reads
		gx_begin(s_v1rt[s->spr_front]);
		gx_use(s_prog_clear);
		gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->swin_cv, s_qidx, s->swin_n * 6);
		gx_end();
	}
	if (!s->gpu)
		return;
	s_stats.gpu_frames++;
	const u64 td0 = RSTV_PROF_T();

	// stage 2: draw the GPU NBG layers into layer textures first
	if (s_gpu_finish)
		gx_finish();
	for (int L = 0; L < NUM_BG; L++)
		s->lsrc[L] = s->layer[L];
	for (int L = 0; L < NUM_BG; L++) {
		if (!s->gpu_layer[L])
			continue;
		GxTex *tgt = s->layer[L];
		if (s_reuse_on && s->lok[L] && s_lcache[L]) {
			// ADR-057: same inputs as the cached drawing -> no GPU work for this layer
			const uint32_t k = layer_key(s, L);
			s->lsrc[L] = s_lcache[L];
			{
				static uint32_t prev_vh[NUM_BG], prev_gen, prev_pal;
				const uint32_t vh = L == 4 ? s->rbg_vh : s->lvh[L];
				if (vh == prev_vh[L])
					s_stats.lsamev[L]++;
				prev_vh[L] = vh;
				if (L == 0) {
					s_stats.lgen += s_cgen != prev_gen;
					s_stats.lpal += s->pal_serial != prev_pal;
					prev_gen = s_cgen;
					prev_pal = s->pal_serial;
				}
			}
			if (s_lvalid[L] && s_lkey[L] == k) {
				s_stats.lreuse[L]++;
				continue;
			}
			s_lkey[L] = k;
			s_lvalid[L] = 1;
			tgt = s_lcache[L];
		}
		u64 tgl = RSTV_PROF_T();
		if (s->pl_on[L] && s->pl_n[L] >= 0) {
			// ADR-058 pass 1: the plane changed -> its cell quads
			gx_begin(s_plane[L]);
			if (s->pl_cn[L] > 0) {
				gx_use(s_prog_clear);
				gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->pl_cv[L], s_qidx, s->pl_cn[L] * 6);
			}
			if (s->pl_n[L] > 0) {
				const float pscale[4] = { 2.0f / s_plane_w[L], 2.0f / s_plane_h[L], 1.0f / NBGQ_ATLAS, 0 };
				if (s->quad_cn[L] == 3) {
					gx_use(s_prog_qrgb);
					gx_set4(s_qrloc[QR_scale], pscale);
					gx_set_tex(s_qrloc[QR_tRgb], s_argb);
					gx_set4(s_qrloc[QR_qcfg], s->quad_cfg[L][0]);
					gx_set4(s_qrloc[QR_qcodeLo], s->quad_cfg[L][1]);
					gx_set4(s_qrloc[QR_qcodeHi], s->quad_cfg[L][2]);
				} else if (s->pl_idx[L]) {
					gx_use(s_prog_qi);
					gx_set4(s_qi_scale, pscale);
					gx_set_tex(s_qi_atlas, s_atlas[s->quad_cn[L] == 0 ? 0 : 1]);
					gx_set4(s_qi_cfg, s->quad_cfg[L][0]);
					gx_set4(s_qi_lo, s->quad_cfg[L][1]);
					gx_set4(s_qi_hi, s->quad_cfg[L][2]);
				} else {
					gx_use(s_prog_q);
					gx_set4(s_qloc[Q_scale], pscale);
					gx_set_tex(s_qloc[Q_tAtlas], s_atlas[s->quad_cn[L] == 0 ? 0 : 1]);
					gx_set_tex(s_qloc[Q_tPal], s->pal);
					gx_set4(s_qloc[Q_qcfg], s->quad_cfg[L][0]);
					gx_set4(s_qloc[Q_qcodeLo], s->quad_cfg[L][1]);
					gx_set4(s_qloc[Q_qcodeHi], s->quad_cfg[L][2]);
				}
				for (int k = 0; k < s->pl_n[L]; k += MAXQ) // ADR-100: chunks within the u16 index range
					gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, (const int16_t *)s->pl_v[L] + k * 4 * 8, s_qidx,
						(s->pl_n[L] - k < MAXQ ? s->pl_n[L] - k : MAXQ) * 6);
			}
			gx_end();
		}
		const int rbg_half = L == 4 && (rstv_rbg_half == 1 || (rstv_rbg_half == 0 && (rstv_game_hacks & RSTV_HACK_RBG_HALF))) &&
			s_rbg_halftex && s_prog_pcopy && !s->pl_on[L];
		gx_begin(rbg_half ? s_rbg_halftex : tgt);
		if (s->bq_on[L]) {
			// ADR-123: bitmap line runs over its converted texture
			clear_target();
			if (s->bq_ln[L] > 0) {
				const float cscale[4] = { 2.0f / LAYER_W, 2.0f / LAYER_H, 1.0f / 1024.0f, 1.0f / 512.0f };
				if (s->bq_cn[L] < 3) { // palette addresses (ADR-089 pci shader)
					gx_use(s_prog_pci);
					gx_set4(s_pci_scale, cscale);
					gx_set_tex(s_pci_tex, s->bmp_tex[L]);
					gx_set_tex(s_pci_pal, s->pal);
				} else {
					gx_use(s_prog_pcopy);
					gx_set4(s_pc_scale, cscale);
					gx_set_tex(s_pc_tex, s->bmp_tex[L]);
				}
				gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->bq_lv[L], s_qidx, s->bq_ln[L] * 6);
			}
			gx_end();
			if (s_gpu_finish) { gx_finish(); s_stats.t_gpu_layer[L] += RSTV_PROF_T() - tgl; }
			continue;
		}
		if (s->pl_on[L]) {
			// ADR-058 pass 2: line runs copied from the plane
			clear_target();
			if (s->pl_ln[L] > 0) {
				const float cscale[4] = { 2.0f / LAYER_W, 2.0f / LAYER_H, 1.0f / s_plane_w[L], 1.0f / s_plane_h[L] };
				if (s->pl_idx[L]) {
					gx_use(s_prog_pci);
					gx_set4(s_pci_scale, cscale);
					gx_set_tex(s_pci_tex, s_plane[L]);
					gx_set_tex(s_pci_pal, s->pal);
				} else {
					gx_use(s_prog_pcopy);
					gx_set4(s_pc_scale, cscale);
					gx_set_tex(s_pc_tex, s_plane[L]);
				}
				gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->pl_lv[L], s_qidx, s->pl_ln[L] * 6);
			}
			gx_end();
			if (s_gpu_finish) { gx_finish(); s_stats.t_gpu_layer[L] += RSTV_PROF_T() - tgl; }
			continue;
		}
		if (L == 4) {
			// RBG0 rotation: clear, then line quads
			clear_target();
			if (s->rbg_n > 0 || s->rbg_nb > 0) {
				GxProg *rp = s_prog_rbg;
				const GxParam *rl = s_rloc;
				if (s_rbg_reg_key >= 0) {
					GxParam *dl;
					(void)rbg_variant_key(s_rbg_reg_key, &dl); // register plane variant (inside gx_lock)
					s_rbg_reg_key = -1;
				}
				if (s_rbg_spec && !s_rbg_dbg) {
					GxParam *vl;
					GxProg *vp = rbg_variant(s, &vl);
					if (vp) {
						rp = vp;
						rl = vl;
						s_stats.rbg_var_frames++;
					}
				}
				if (s->rbg_bmp && rp == s_prog_rbg) {
					// bitmaps are drawn only by specialized variants (blank for a few frames until compiled on the first run, preregistered from the next run)
					gx_end();
					if (rbg_half)
						rbg_half_copy(s, tgt);
					if (s_gpu_finish) { gx_finish(); s_stats.t_gpu_layer[L] += RSTV_PROF_T() - tgl; }
					continue;
				}
				gx_use(rp);
				gx_set_tex(rl[R_tVram], s->vram);
				gx_set4(rl[R_rcfg2], s->rbg_cfg2);
				gx_set_tex(rl[R_tTable], s->rbg_table);
				gx_set_tex(rl[R_tPages], s_rbg_pages);
				gx_set_tex(rl[R_tAtlas], s_atlas[rstv_rot_capture.colornumber == 0 ? 0 : 1]);
				gx_set_tex(rl[R_tRgb], s_argb);
				gx_set_tex(rl[R_tPal], s->pal);
				for (int k = 0; k < 4; k++)
					gx_set4(rl[R_rcfg0 + k], s->rbg_cfg[k]);
				const float dbg[4] = { (float)s_rbg_dbg, 0, s->rbg_cfg[0][0] > 0 ? 1.0f / s->rbg_cfg[0][0] : 0,
					s->rbg_cfg[0][1] > 0 ? 1.0f / s->rbg_cfg[0][1] : 0 };
				gx_set4(rl[R_rdbg], dbg);
				if (s->rbg_n > 0)
					gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->rbg_v, s_qidx, s->rbg_n * 6);
				if (s->rbg_nb > 0) {
					if ((float)s->rbg_so_b != s->rbg_cfg[0][2]) {
						// ADR-071: shader and uniforms again for B's off-screen mode (generic if no variant, generic reads rcfg0.z)
						const float so_a = s->rbg_cfg[0][2];
						s->rbg_cfg[0][2] = (float)s->rbg_so_b;
						rp = s_prog_rbg;
						rl = s_rloc;
						if (s_rbg_spec && !s_rbg_dbg) {
							GxParam *vl;
							GxProg *vp = rbg_variant(s, &vl);
							if (vp) {
								rp = vp;
								rl = vl;
							}
						}
						gx_use(rp);
						gx_set_tex(rl[R_tVram], s->vram);
						gx_set4(rl[R_rcfg2], s->rbg_cfg2);
						gx_set_tex(rl[R_tAtlas], s_atlas[rstv_rot_capture.colornumber == 0 ? 0 : 1]);
						gx_set_tex(rl[R_tRgb], s_argb);
						gx_set_tex(rl[R_tPal], s->pal);
						gx_set_tex(rl[R_tPages], s_rbg_pages);
						for (int k = 0; k < 4; k++)
							gx_set4(rl[R_rcfg0 + k], s->rbg_cfg[k]);
						gx_set4(rl[R_rdbg], dbg);
						s->rbg_cfg[0][2] = so_a;
					}
					gx_set_tex(rl[R_tTable], s->rbg_table_b);
					if (s->rbg_flat && s_rbg_pages_b)
						gx_set_tex(rl[R_tPages], s_rbg_pages_b);
					gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->rbg_vb, s_qidx, s->rbg_nb * 6);
				}
			}
			gx_end();
			if (rbg_half)
				rbg_half_copy(s, tgt);
			if (s_gpu_finish) { gx_finish(); s_stats.t_gpu_layer[L] += RSTV_PROF_T() - tgl; }
			continue;
		}
		if (s->quad_n[L] >= 0) {
			// cell quads: clear, then draw only the quads
			clear_target();
			if (s->quad_n[L] > 0) {
				const float scale[4] = { 2.0f / LAYER_W, 2.0f / LAYER_H, 1.0f / NBGQ_ATLAS, 0 };
				if (s->quad_cn[L] == 3) {
					// RGB555: RGB atlas
					gx_use(s_prog_qrgb);
					gx_set4(s_qrloc[QR_scale], scale);
					gx_set_tex(s_qrloc[QR_tRgb], s_argb);
					gx_set4(s_qrloc[QR_qcfg], s->quad_cfg[L][0]);
					gx_set4(s_qrloc[QR_qcodeLo], s->quad_cfg[L][1]);
					gx_set4(s_qrloc[QR_qcodeHi], s->quad_cfg[L][2]);
					gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->quad_v[L], s_qidx, s->quad_n[L] * 6);
					if (s->qclr_n[L] > 0) {
						gx_use(s_prog_clear);
						gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->qclr_v[L], s_qidx, s->qclr_n[L] * 6);
					}
					if (s->nbgh_n[L] > 0)
						draw_nbg_lines(s, L, s->nbgh_v[L], s->nbgh_n[L]);
					gx_end();
					if (s_gpu_finish) { gx_finish(); s_stats.t_gpu_layer[L] += RSTV_PROF_T() - tgl; }
					continue;
				}
				gx_use(s_prog_q);
				gx_set4(s_qloc[Q_scale], scale);
				gx_set_tex(s_qloc[Q_tAtlas], s_atlas[s->quad_cn[L] == 0 ? 0 : 1]);
				gx_set_tex(s_qloc[Q_tPal], s->pal);
				gx_set4(s_qloc[Q_qcfg], s->quad_cfg[L][0]);
				gx_set4(s_qloc[Q_qcodeLo], s->quad_cfg[L][1]);
				gx_set4(s_qloc[Q_qcodeHi], s->quad_cfg[L][2]);
				gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->quad_v[L], s_qidx, s->quad_n[L] * 6);
				if (s->qclr_n[L] > 0) {
					gx_use(s_prog_clear);
					gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->qclr_v[L], s_qidx, s->qclr_n[L] * 6);
				}
			}
			if (s->nbgh_n[L] > 0)
				draw_nbg_lines(s, L, s->nbgh_v[L], s->nbgh_n[L]);
			gx_end();
			if (s_gpu_finish) { gx_finish(); s_stats.t_gpu_layer[L] += RSTV_PROF_T() - tgl; }
			continue;
		}
		GxProg *np = s_prog_nbg;
		const GxParam *nl = s_nloc;
		if (s_nbg_spec) {
			GxParam *vl;
			GxProg *vp = nbg_variant((const float (*)[4])s->nbg[L], &vl);
			if (vp) {
				np = vp;
				nl = vl;
			}
		}
		gx_use(np);
		const GxTex *nt[3] = { s->vram, s->pal, s->rows };
		for (int i = 0; i < 3; i++)
			gx_set_tex(nl[N_tVram + i], nt[i]);
		if (s->nbg[L][6][3] > 1.5f)
			gx_set_tex(nl[N_tBmp], s->bmp_tex[L]); // ADR-123
		for (int k = 0; k < 9; k++)
			gx_set4(nl[N_cfgA + k], s->nbg[L][k]);
		if (s->nbgw_v[L]) {
			if (s->nbgw_n[L] > 0)
				gx_draw(SCE_GXM_PRIMITIVE_TRIANGLES, s->nbgw_v[L], s_qidx, s->nbgw_n[L] * 6);
		} else
			draw_quad(s->w, s->h);
		gx_end();
		if (s_gpu_finish) { gx_finish(); s_stats.t_gpu_layer[L] += RSTV_PROF_T() - tgl; }
	}
	if (s_gpu_finish) {
		u64 tg = RSTV_PROF_T();
		gx_finish();
		s_stats.t_gpu_nbg += RSTV_PROF_T() - tg;
	}
	gx_begin(s_out_tex[oi]);
	// fast composite shader if color offset is the same on every line (2 line value reads)
	const int fast = s->off_ok && s_prog_fast;
	const GxParam *loc = fast ? s_loc_fast : s_loc;
	GxProg *cprog = fast ? s_prog_fast : s_prog;
	if (fast && s_cmp_spec) {
		GxParam *vloc;
		const u64 tcv = RSTV_PROF_T();
		GxProg *vp = cmp_variant(s, &vloc);
		if (RSTV_PROF_T() - tcv > 10000) // diag: a slow variant lookup (registration, new source) inside the submit
			RSTV_LOG("gvdp2: slow variant lookup f%u %u us", s_frame, (unsigned)(RSTV_PROF_T() - tcv));
		s_stats.cmp_vi = 0;
		if (vp) {
			cprog = vp;
			loc = vloc;
			s_stats.var_frames++;
			for (int i = 0; i < s_nvar; i++)
				if (s_var[i].prog == vp) {
					s_stats.cmp_vi = i + 1;
					s_stats.cmp_instr = s_var[i].instr;
					break;
				}
		}
		s_stats.nvar = (unsigned)s_nvar;
	}
	if (fast && s_prog_ovr) {
		cprog = s_prog_ovr;
		loc = s_loc_ovr;
	}
	if (s_cmp_exp && s_prog_exp) { // for measurement (cmpexp): check fixed cost
		gx_use(s_prog_exp);
		if (s_cmp_exp == 2) {
			const GxTex *tx[7] = { s->lsrc[0], s->lsrc[1], s->lsrc[2], s->lsrc[3], s->lsrc[4],
				s->spr_gpu ? s_v1rt[s->spr_front] : s->spr, s->line };
			static const char *nm[7] = { "t0", "t1", "t2", "t3", "t4", "t5", "t6" };
			for (int i = 0; i < 7; i++)
				gx_set_tex(gx_param(s_prog_exp, nm[i]), tx[i]);
		}
		draw_quad(s->w, s->h);
		gx_end();
		if (s_gpu_finish) {
			u64 tg = RSTV_PROF_T();
			gx_finish();
			s_stats.t_gpu += RSTV_PROF_T() - tg;
		}
		return;
	}
	gx_use(cprog);
	const GxTex *tex[8] = { s->lsrc[0], s->lsrc[1], s->lsrc[2], s->lsrc[3], s->lsrc[4],
		s->spr_gpu ? s_v1rt[s->spr_front] : s->spr, s->pal, s->line };
	for (int i = 0; i < 8; i++)
		gx_set_tex(loc[U_tN3 + i], tex[i]);
	if (fast) {
		for (int k = 0; k < 7; k++)
			gx_set4(loc[U_offB + k], s->offu[k]);
		gx_set_tex(loc[U_tSLut], s->slut);
	}
	s_stats.fast_frames += fast;
	gx_set4(loc[U_layerOn], s->layerOn);
	gx_set4(loc[U_layerAlpha], s->layerAlpha);
	gx_set4(loc[U_layerShad], s->layerShad);
	gx_set4(loc[U_layerLs], s->layerLs);
	gx_set4(loc[U_misc], s->misc);
	gx_set4(loc[U_sprA], s->sprA);
	gx_set4(loc[U_sprB], s->sprB);
	gx_set4(loc[U_sprC], s->sprC);
	gx_set4(loc[U_sprD], s->sprD);
	gx_set4(loc[U_sprE], s->sprE);
	gx_set4(loc[U_prioLo], s->prioLo);
	gx_set4(loc[U_prioHi], s->prioHi);
	gx_set4(loc[U_ccLo], s->ccLo);
	gx_set4(loc[U_ccHi], s->ccHi);
	gx_set4(loc[U_mode], s->mode);
	gx_set4(loc[U_ccwA], s->ccwA);
	gx_set4(loc[U_ccwB], s->ccwB);
	gx_set4(loc[U_ccw0], s->ccw0);
	gx_set4(loc[U_ccw1], s->ccw1);
	gx_set4(loc[U_mosaic], s->mosaic);
	gx_set4(loc[U_mosaicOn], s->mosaicOn);
	gx_set4(loc[U_layerSw], s->layerSw);
	draw_quad(s->w, s->h);
	gx_end();

	if (s_shot_now && !on_worker) {
		char path[96];
		gx_finish();
		snprintf(path, sizeof(path), "ux0:data/restv/shot_%u_gpu.ppm", s_frame - 1);
		save_ppm(path, s_out_tex[oi]->data, s->w, s->h, LAYER_W);
		TitanRender(dispbuffer);
		snprintf(path, sizeof(path), "ux0:data/restv/shot_%u_cpu.ppm", s_frame - 1);
		save_ppm(path, (const uint8_t *)dispbuffer, s->w, s->h, s->w);
		RSTV_LOG("gvdp2: shot %u", s_frame - 1);
		for (int b = 0; b < 2; b++) // the two VDP1 GPU framebuffers (raw RGBA8: 8-bit dots in R)
			if (s_v1rt[b]) {
				snprintf(path, sizeof(path), "ux0:data/restv/shot_%u_v1rt%d.ppm", s_frame - 1, b);
				save_ppm(path, s_v1rt[b]->data, 512, 256, 512);
			}
		RSTV_LOG("gvdp2: shot %u spr_front %d spr_gpu %d", s_frame - 1, s->spr_front, s->spr_gpu);
	}
	if (s_gpu_finish) {
		u64 tg = RSTV_PROF_T();
		gx_finish();
		s_stats.t_gpu += RSTV_PROF_T() - tg;
	}

	s_stats.t_draw += RSTV_PROF_T() - td0;
}

static void gvdp2_draw_end(void)
{
#ifdef RSTV_DEBUG_TOOLS
	{ // debug fast boot (main.c): no composite, only the VDP1 framebuffer swap
		extern int rstv_fastboot_now;
		if (rstv_fastboot_now) {
			s_cur = -1;
			VIDSoftVdp1SwapFrameBuffer();
			return;
		}
	}
#endif
	if (s_end_defer) {
		// ADR-186: the prep worker starts a frame at V-blank-in and can still be busy at its end (V-blank-out); the
		// main thread waited for it (vfremix 1.1 ms a frame). After 60 such frames the game stays in deferred mode: each
		// frame is finished at the next V-blank-in (draw_screens) and shown one frame later; every frame is still shown
		// once (going back would drop one). CPU-composited frames as before.
		if (s_end_pend >= 0)
			draw_end_work(s_end_pend); // no draw_screens since the last frame
		s_end_pend = -1;
		if (s_cur >= 0 && s_set[s_cur].gpu) {
			if (s_end_defer == 1 && s_prep_pending) {
				static unsigned s_late;
				if (sceKernelPollSema(s_prep_done, 1) == 0)
					s_prep_pending = 0; // done in time: finish now
				else if (++s_late >= 60) { // not for a few load frames
					s_end_defer = 2;
					RSTV_LOG("gvdp2: frame finish deferred from f%u (prep still busy at V-blank-out)", s_frame);
				}
			}
			if (s_end_defer == 2) {
				s_end_pend = s_cur;
				return;
			}
		}
	}
	draw_end_work(s_cur);
}

static void draw_end_work(int cur)
{
	const u64 te0 = RSTV_PROF_T();
	static unsigned gw_prev;
	const unsigned gw0 = gx_gpu_wait_us();
	prep_wait();
	const u64 te1 = RSTV_PROF_T();
	FrameSet *s = (cur >= 0) ? &s_set[cur] : NULL;
	s_frame++;
	const int on_main = s && !s->submitted;
	if (s && !s->submitted)
		gpu_submit(s, cur, 0);
	else if (s && s_shot_now && s->gpu) {
		// capture of a frame drawn by the worker thread (shotasync)
		char path[96];
		gx_lock();
		gx_finish();
		gx_unlock();
		snprintf(path, sizeof(path), "ux0:data/restv/shot_%u_gpu.ppm", s_frame - 1);
		save_ppm(path, s_out_tex[cur]->data, s->w, s->h, LAYER_W);
	}
	const u64 te2 = RSTV_PROF_T();
	if (s)
		s->submitted = 0;
	if (!s || !s->gpu) {
		s_stats.cpu_frames++;
		VIDSoftVdp2DrawEnd(); // Titan composite + VDP1 framebuffer swap + YuiSwapBuffers
		return;
	}
	VIDSoftVdp1SwapFrameBuffer();
	const u64 te3 = RSTV_PROF_T();
	rstv_video_frame_gpu(s_out_tex[cur], s->w, s->h);
	const u64 te4 = RSTV_PROF_T();
	if (te4 - te0 > 10000) // diag: per-stage times of draw_end over 10ms
		RSTV_LOG("gvdp2: slow draw_end f%u gpu wait %u (since last %u) prep_wait %u submit %u (main %d) swapfb %u frame_gpu %u us", s_frame - 1,
			gx_gpu_wait_us() - gw0, gx_gpu_wait_us() - gw_prev,
			(unsigned)(te1 - te0), (unsigned)(te2 - te1), on_main, (unsigned)(te3 - te2), (unsigned)(te4 - te3));
	gw_prev = gx_gpu_wait_us();
}

// shader lab (ux0:data/restv/lab/*.cg): compile fragment sources and record only the GXP instruction count
static void shader_lab_ext(const char *ext, int uniq)
{
	SceUID d = sceIoDopen("ux0:data/restv/lab");
	if (d < 0)
		return;
	SceIoDirent e;
	while (sceIoDread(d, &e) > 0) {
		const size_t n = strlen(e.d_name);
		const size_t el = strlen(ext);
		if (n <= el || strcmp(e.d_name + n - el, ext))
			continue;
		char path[160];
		snprintf(path, sizeof(path), "ux0:data/restv/lab/%s", e.d_name);
		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		fseek(f, 0, SEEK_END);
		const long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *src = (char *)malloc(sz + 64);
		const int ok = src && fread(src, 1, sz, f) == (size_t)sz;
		fclose(f);
		if (!ok) {
			free(src);
			continue;
		}
		src[sz] = 0;
		if (uniq)
			snprintf(src + sz, 64, "\n// %u %u\n", (unsigned)uniq, (unsigned)RSTV_PROF_T());
		const uint8_t *g = (const uint8_t *)gx_compile(src, 1);
		if (g)
			RSTV_LOG("lab: %s instr %u temp %u tex %08x %08x", e.d_name, *(const uint32_t *)(g + 60), *(const uint32_t *)(g + 52),
				*(const uint32_t *)(g + 28), *(const uint32_t *)(g + 32));
		else
			RSTV_LOG("lab: %s compile failed", e.d_name);
		free(src);
	}
	sceIoDclose(d);
}
static void shader_lab(void) { shader_lab_ext(".cg", 0); }

// ADR-035: register composite/RBG0 variants seen last run at startup (load cached GXP and register right away)
static int s_preload_again; // ADR-211: a later game in the same process
static int var_known(const char *key)
{
	for (int i = 0; i < s_nvar; i++)
		if (!strcmp(s_var[i].key, key))
			return 1;
	return 0;
}
static void preload_variants(void)
{
	SceUID d = sceIoDopen(VARLIST_DIR);
	if (d < 0)
		return;
	char pre_c[16], pre_r[16], pre_n[16];
	snprintf(pre_n, sizeof(pre_n), "nv_%08x_", (unsigned)fnv_str(s_fs_nbg));
	int nn = 0;
	snprintf(pre_c, sizeof(pre_c), "cv_%08x_", (unsigned)fnv_str(s_fs_fast));
	snprintf(pre_r, sizeof(pre_r), "rv_%08x_", (unsigned)fnv_str(s_fs_rbg2));
	const u64 t0 = RSTV_PROF_T();
	int nc = 0, nr = 0, nskip = 0;
	cl_load();
	SceIoDirent e;
	// pass 1: this game's listed composite variants + NBG/RBG variants; pass 2 (if a list exists): up to 128 composite variants not yet in the list
	// (avoids the registration delay on first encounter before the list fills, leaves room for new variants)
	for (int pass = 0; pass < 2; pass++) {
	if (pass == 1) {
		sceIoDclose(d);
		d = -1;
		if (!s_cl_loaded || s_nvar >= 128 || (d = sceIoDopen(VARLIST_DIR)) < 0)
			break;
	}
	while (sceIoDread(d, &e) > 0) {
		if (s_cmp_spec && !strncmp(e.d_name, pre_c, 12) && s_nvar < (pass ? 128 : MAX_VARIANTS)) {
			if (s_cl_loaded && cl_has((uint32_t)strtoul(e.d_name + 12, NULL, 16)) != !pass) {
				nskip += pass == 0; // outside the list (another game, or not yet confirmed)
				continue;
			}
			char path[160];
			snprintf(path, sizeof(path), VARLIST_DIR "/%s", e.d_name);
			FILE *f = fopen(path, "rb");
			if (!f)
				continue;
			fseek(f, 0, SEEK_END);
			const long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			char *buf = (char *)malloc(sz + 1);
			const int ok = buf && fread(buf, 1, sz, f) == (size_t)sz;
			fclose(f);
			if (!ok) {
				free(buf);
				continue;
			}
			buf[sz] = 0;
			char *nl = strchr(buf, 10);
			if (!nl) {
				free(buf);
				continue;
			}
			*nl = 0;
			if (s_preload_again && var_known(buf)) { // (already loaded for an earlier game)
				free(buf);
				continue;
			}
			CmpVariant *v = &s_var[s_nvar];
			snprintf(v->key, sizeof(v->key), "%s", buf);
			v->fbin = gx_compile(nl + 1, 1);
			if (v->fbin)
				v->instr = *(const uint32_t *)((const uint8_t *)v->fbin + 60);
			v->prog = v->fbin ? gx_prog_bin(gx_compile(s_vs_fast, 0), v->fbin, s_pix_attr, 2, 16, GX_BLEND_NONE) : NULL;
			if (v->prog) {
				for (int k = 0; k < U_COUNT; k++)
					v->loc[k] = gx_param(v->prog, s_uname[k]);
				v->state = 2;
				v->src = NULL;
				s_nvar++;
				nc++;
				if (gvdp2_progress) // loading screen: the preload is most of the GPU set-up time
					gvdp2_progress(300 + (nc < 160 ? nc * 650 / 160 : 650));
			}
			free(buf);
		} else if (pass == 0 && !s_preload_again && s_nbg_spec && !strncmp(e.d_name, pre_n, 12) && s_nnvar < 24) {
			const int key = (int)strtol(e.d_name + 12, NULL, 16);
			NbgVariant *v = &s_nvar_tab[s_nnvar];
			const size_t n = strlen(s_fs_nbg) + 512;
			char *src = (char *)malloc(n);
			if (!src)
				continue;
			nbg_variant_src(src, n, key);
			v->key = key;
			v->fbin = gx_compile(src, 1);
			free(src);
			v->prog = v->fbin ? gx_prog_bin(gx_compile(s_vs, 0), v->fbin, s_pix_attr, 2, 16, GX_BLEND_NONE) : NULL;
			if (v->prog) {
				for (int k = 0; k < N_COUNT; k++)
					v->loc[k] = gx_param(v->prog, s_nname[k]);
				v->state = 2;
				v->src = NULL;
				s_nnvar++;
				nn++;
			}
		} else if (pass == 0 && !s_preload_again && s_rbg_spec && s_prog_rbg && !strncmp(e.d_name, pre_r, 12) && s_nrvar < 32) {
			const int key = (int)strtol(e.d_name + 12, NULL, 16);
			RbgVariant *v = &s_rvar[s_nrvar];
			const size_t n = strlen(s_fs_rbg2);
			char *src = (char *)malloc(n + 256);
			if (!src)
				continue;
			snprintf(src, n + 256, "#define RBG_SO %d\n#define RBG_RGB %d\n#define RBG_TR %d\n#define RBG_SCM %d\n#define RBG_CCR %d\n#define RBG_BMP %d\n#define RBG_4BPP %d\n%s%s%s",
				key & 1, (key >> 1) & 1, (key >> 2) & 1, (key >> 3) & 3, (key >> 5) & 1, (key >> 6) & 1, (key >> 7) & 1, ((key >> 8) & 1) ? "#define RBG_FLAT 1\n" : "", ((key >> 9) & 1) ? "#define RBG_SO3 1\n" : "", s_fs_rbg2);
			v->key = key;
			v->fbin = gx_compile(src, 1);
			free(src);
			v->prog = v->fbin ? gx_prog_bin(gx_compile(s_vs_rbg, 0), v->fbin, s_rbg_attr, 2, 16, GX_BLEND_NONE) : NULL;
			if (v->prog) {
				for (int k = 0; k < R_COUNT; k++)
					v->loc[k] = gx_param(v->prog, s_rname[k]);
				v->state = 2;
				v->src = NULL;
				s_nrvar++;
				nr++;
			}
		}
	}
	}
	if (d >= 0)
		sceIoDclose(d);
	// ADR-066: migration of cached variants from the previous source: worker thread reads files (mig_scan), main registers a few per frame (mig_take)
	if (!s_preload_again) {
		s_mig_pending = 1;
		if (s_cmp_sema >= 0)
			sceKernelSignalSema(s_cmp_sema, 1);
	}
	RSTV_LOG("gvdp2: preloaded %d compose (%d not in list) + %d rbg variants (%.0f ms, %s %s spec %d %d)", nc, nskip, nr,
		(RSTV_PROF_T() - t0) / 1000.0, pre_c, pre_r, s_cmp_spec, s_rbg_spec);
	{
		FILE *f = fopen("ux0:data/restv/preload.txt", "w");
		if (f) {
			fprintf(f, "preloaded %d compose + %d rbg + %d nbg (%s %s spec %d %d)\n", nc, nr, nn, pre_c, pre_r, s_cmp_spec, s_rbg_spec);
			fclose(f);
		}
	}
}

int gvdp2_init(void)
{
	if (gvdp2_progress)
		gvdp2_progress(0);
	shader_lab();
	const GxAttr pix_attr[2] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 }, { "pix", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 8 } };
	s_prog = gx_prog(s_vs, s_fs, pix_attr, 2, 16, GX_BLEND_NONE);
	if (!s_prog)
		return -1;
	if (gvdp2_progress) // ADR-193: steps through the base shaders (a first run compiles them: about a minute)
		gvdp2_progress(20);
	for (int i = 0; i < U_COUNT; i++) // the ones it does not have stay unset (offsets, slut)
		s_loc[i] = gx_param(s_prog, s_uname[i]);
	s_fs_fast_sw = make_fs_fast_sw();
	s_prog_fast = gx_prog(s_vs_fast, s_fs_fast_sw ? s_fs_fast_sw : s_fs_fast, pix_attr, 2, 16, GX_BLEND_NONE);
	if (gvdp2_progress)
		gvdp2_progress(110);
	if (s_prog_fast)
		for (int i = 0; i < U_COUNT; i++)
			s_loc_fast[i] = gx_param(s_prog_fast, s_uname[i]);
	RSTV_LOG("gvdp2: fast compose shader %s", s_prog_fast ? "ok" : "failed");
	{
		FILE *of = fopen("ux0:data/restv/cmpoverride.cg", "rb");
		if (of) {
			fseek(of, 0, SEEK_END);
			const long sz = ftell(of);
			fseek(of, 0, SEEK_SET);
			char *src = (char *)malloc(sz + 1);
			if (src && fread(src, 1, sz, of) == (size_t)sz) {
				src[sz] = 0;
				s_prog_ovr = gx_prog(s_vs_fast, src, pix_attr, 2, 16, GX_BLEND_NONE);
				if (s_prog_ovr)
					for (int i = 0; i < U_COUNT; i++)
						s_loc_ovr[i] = gx_param(s_prog_ovr, s_uname[i]);
			}
			free(src);
			fclose(of);
			RSTV_LOG("gvdp2: compose override %s", s_prog_ovr ? "ok" : "failed");
		}
	}
	s_pix_attr[0] = pix_attr[0];
	s_pix_attr[1] = pix_attr[1];
	s_cmp_spec = s_prog_fast && !file_exists_q("ux0:data/restv/nocmpspec");
	s_lab_on = file_exists_q("ux0:data/restv/lab");
	s_end_defer = !file_exists_q("ux0:data/restv/noenddefer"); // ADR-186 (the flag file turns it off for A/B tests)
	s_cmp_exp = 0;
	{
		FILE *ef = fopen("ux0:data/restv/cmpexp", "r");
		if (ef) {
			if (fscanf(ef, "%d", &s_cmp_exp) != 1)
				s_cmp_exp = 1;
			fclose(ef);
		}
	}
	if (s_cmp_exp == 1)
		s_prog_exp = gx_prog(s_vs_fast, "float4 main(float2 vPix : TEXCOORD0) : COLOR { return float4(1.0, 0.0, 1.0, 1.0); }", pix_attr, 2, 16, GX_BLEND_NONE);
	else if (s_cmp_exp == 2)
		s_prog_exp = gx_prog(s_vs_fast,
			"float4 main(float2 vPix : TEXCOORD0, float2 vUV : TEXCOORD1, float2 vL0 : TEXCOORD2, float2 vL1 : TEXCOORD3,\n"
			"  uniform sampler2D t0, uniform sampler2D t1, uniform sampler2D t2, uniform sampler2D t3, uniform sampler2D t4,\n"
			"  uniform sampler2D t5, uniform sampler2D t6) : COLOR\n"
			"{ return tex2D(t0, vUV) + tex2D(t1, vUV) + tex2D(t2, vUV) + tex2D(t3, vUV) + tex2D(t4, vUV) + tex2D(t5, vUV) + tex2D(t6, vL0) + tex2D(t6, vL1); }\n",
			pix_attr, 2, 16, GX_BLEND_NONE);
	if (s_cmp_spec) {
		s_cmp_sema = sceKernelCreateSema("rstv_shc", 0, 0, 1 << 20, NULL);
		// ADR-185: on CPU3, not CPU1: there the spinning sound thread starved the compiler (11 ms -> 300-600 ms) while it
		// held locks the prep thread's GPU submit needed (submit stalls of 35-330 ms when a new variant was queued)
		int cpu = 0x80000;
#ifdef RSTV_DEBUG_TOOLS
		{ FILE *ff = fopen("ux0:data/restv/shccpu1", "rb"); if (ff) { fclose(ff); cpu = SCE_KERNEL_CPU_MASK_USER_1; } } // A/B test
#endif
		SceUID t = s_cmp_sema >= 0 ? sceKernelCreateThread("rstv_shc", cmp_worker, 191, 0x400000, 0, cpu, NULL) : -1;
		if (t >= 0)
			sceKernelStartThread(t, 0, NULL);
		else
			s_cmp_spec = 0;
	}

	// NBG pixel shader (for layers that cannot be drawn as quads; falls back to CPU on failure)
	{
		const size_t ln = strlen(s_fs_nbg_gen) + strlen(s_fs_nbg) + 1;
		char *src = (char *)malloc(ln);
		if (src) {
			snprintf(src, ln, "%s%s", s_fs_nbg_gen, s_fs_nbg);
			s_prog_nbg = gx_prog(s_vs, src, pix_attr, 2, 16, GX_BLEND_NONE);
			free(src);
			if (gvdp2_progress)
				gvdp2_progress(200);
		}
		s_nbg_spec = s_prog_nbg && !file_exists_q("ux0:data/restv/nonbgspec");
		s_bmp_tex_on = !file_exists_q("ux0:data/restv/nobmptex"); // ADR-123
	}
	if (s_prog_nbg)
		for (int i = 0; i < N_COUNT; i++)
			s_nloc[i] = gx_param(s_prog_nbg, s_nname[i]);
	RSTV_LOG("gvdp2: nbg shader %s", s_prog_nbg ? "ok" : "failed");

	// cell quad shader (ADR-014). Not used if ux0:data/restv/noquad exists
	if (!file_exists_q("ux0:data/restv/noquad")) {
		// vertex: int16 (x, y, u, v, palette base, priority, scf, 0), read as float with integer values
		const GxAttr q_attr[3] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_S16, 2, 0 }, { "uv", SCE_GXM_ATTRIBUTE_FORMAT_S16, 2, 4 },
			{ "attr", SCE_GXM_ATTRIBUTE_FORMAT_S16, 4, 8 } };
		const GxAttr c_attr[1] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 } };
		s_prog_q = gx_prog(s_vs_q, s_fs_q, q_attr, 3, 16, GX_BLEND_NONE);
		s_prog_clear = gx_prog(s_vs_clear, s_fs_clear, c_attr, 1, 8, GX_BLEND_NONE);
		s_qidx = (uint16_t *)gx_static(MAXQ * 6 * (int)sizeof(uint16_t));
		s_atlas[0] = gx_tex(NBGQ_ATLAS, NBGQ_ATLAS, GX_L8, 0, 0);
		s_atlas[1] = gx_tex(NBGQ_ATLAS, NBGQ_ATLAS, GX_L8, 0, 0);
		if (!s_prog_q || !s_prog_clear || !s_qidx || !s_atlas[0] || !s_atlas[1]) {
			s_prog_q = NULL;
		} else {
			for (int i = 0; i < Q_COUNT; i++)
				s_qloc[i] = gx_param(s_prog_q, s_qname[i]);
			for (int k = 0; k < MAXQ; k++) {
				const uint16_t b = (uint16_t)(k * 4);
				uint16_t *d = s_qidx + k * 6;
				d[0] = b; d[1] = (uint16_t)(b + 1); d[2] = (uint16_t)(b + 2);
				d[3] = (uint16_t)(b + 1); d[4] = (uint16_t)(b + 3); d[5] = (uint16_t)(b + 2);
			}
			s_atlas_px[0] = s_atlas[0]->data;
			s_atlas_px[1] = s_atlas[1]->data;
			// RGB atlas shared by RGB555 cell quads and RBG0 (CDRAM: in LPDDR the RBG shader crashes the GPU)
			s_argb = gx_tex(1024, 1024, GX_RGBA8 | GX_CDRAM, 0, 0);
			if (s_argb)
				s_prog_qrgb = gx_prog(s_vs_q, s_fs_qrgb, q_attr, 3, 16, GX_BLEND_NONE);
			if (s_prog_qrgb)
				for (int i = 0; i < QR_COUNT; i++)
					s_qrloc[i] = gx_param(s_prog_qrgb, s_qrname[i]);
		}
		RSTV_LOG("gvdp2: quad shader %s, rgb %s", s_prog_q ? "ok" : "failed", s_prog_qrgb ? "ok" : "failed");
	}
	// RBG0 rotation shader (ADR-016, shares the cell quad path's atlas). Not used if ux0:data/restv/norbg exists
	if (s_prog_q && !file_exists_q("ux0:data/restv/norbg")) {
		const GxAttr r_attr[2] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 }, { "map", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 8 } };
		s_prog_rbg = gx_prog(s_vs_rbg, s_fs_rbg, r_attr, 2, 16, GX_BLEND_NONE);
		if (gvdp2_progress)
			gvdp2_progress(230);
		s_rbg_attr[0] = r_attr[0];
		s_rbg_attr[1] = r_attr[1];
		s_rbg_spec = !file_exists_q("ux0:data/restv/norbgspec");
		s_rbg_flat_on = !file_exists_q("ux0:data/restv/norbgflat");
		s_rbg_pages = gx_tex(512, 512, GX_RGBA8, 0, 0);
		s_rbg_halftex = gx_tex(LAYER_W / 2, LAYER_H, GX_RGBA8, 0, 20); // ADR-077 (own render target slot)
		s_rbg_pages_b = gx_tex(512, 512, GX_RGBA8, 0, 0);
		if (!s_prog_rbg || !s_rbg_pages || !s_argb)
			s_prog_rbg = NULL;
		else
			for (int i = 0; i < R_COUNT; i++)
				s_rloc[i] = gx_param(s_prog_rbg, s_rname[i]);
		RSTV_LOG("gvdp2: rbg shader %s", s_prog_rbg ? "ok" : "failed");
		FILE *df = fopen("ux0:data/restv/rbgdbg", "r");
		if (df) {
			if (fscanf(df, "%d", &s_rbg_dbg) != 1)
				s_rbg_dbg = 1;
			fclose(df);
		}
	}

	s_capture_main = file_exists_q("ux0:data/restv/capmain"); // ADR-079: layer pass on the main thread (old behaviour)
	s_plane_fullw = !file_exists_q("ux0:data/restv/noplanefull");
	s_plane_all = file_exists_q("ux0:data/restv/planeall");
	s_plane_mag = !file_exists_q("ux0:data/restv/noplanemag");
	s_plane_many = !file_exists_q("ux0:data/restv/noplanemany");
	// layer prep worker thread (ADR-025, CPU2). On main if ux0:data/restv/noprepthr exists
	s_snap_vram = (u8 *)malloc(0x80000);
	s_v1snap_ram = (u8 *)malloc(0x80000);
	if (s_snap_vram && !file_exists_q("ux0:data/restv/noprepthr")) {
		s_prep_go = sceKernelCreateSema("rstv_prep_go", 0, 0, 1, NULL);
		s_prep_done = sceKernelCreateSema("rstv_prep_done", 0, 0, 1, NULL);
		SceUID t = (s_prep_go >= 0 && s_prep_done >= 0) ?
			sceKernelCreateThread("rstv_prep", prep_worker, 65, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL) : -1;
		if (t >= 0 && sceKernelStartThread(t, 0, NULL) >= 0)
			s_prep_thread_ok = 1;
			s_submit_thr = !file_exists_q("ux0:data/restv/nosubmitthr");
	}
	s_shot_async = file_exists_q("ux0:data/restv/shotasync");
	RSTV_LOG("gvdp2: prep worker %s", s_prep_thread_ok ? "on" : "off");

	// VDP1 GPU renderer (ADR-023). Not used if ux0:data/restv/nov1gpu exists
	if (s_prog_q && !file_exists_q("ux0:data/restv/nov1gpu")) {
		const GxAttr v_attr[4] = { { "pos", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 }, { "uv", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 8 },
			{ "clip", SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, 16 }, { "grd", SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, 32 } };
		const int want_cc = !file_exists_q("ux0:data/restv/nov1cc"); // ADR-115
		v1_cc_programs_ok = want_cc;
		for (int vnt = 0; vnt < V1GPU_VARIANTS; vnt++) {
			if ((V1GPU_CC(vnt) || V1GPU_MSB(vnt)) && ((vnt & 2) || !want_cc))
				continue; // gouraud + color calc on CPU (filtered at capture)
			if (V1GPU_MSB_BAD(vnt))
				continue;
			s_v1prog[vnt] = gx_prog(v1gpu_vs(), v1gpu_fs(vnt), v_attr, 4, V1GPU_VERTEX_FLOATS * 4, GX_BLEND_NONE);
			if (s_v1prog[vnt] && !(vnt & 4))
				s_v1loc[vnt] = gx_param(s_v1prog[vnt], "tAtlas");
			if (s_v1prog[vnt] && V1GPU_READS_FB(vnt))
				s_v1snaploc[vnt] = gx_param(s_v1prog[vnt], "tSnap");
			if ((V1GPU_CC(vnt) || V1GPU_MSB(vnt)) && !s_v1prog[vnt])
				v1_cc_programs_ok = 0;
		}
		if (v1_cc_programs_ok)
			s_v1snap = gx_tex(512, 256, GX_RGBA8, 0, 28); // own render target slot (gx.c)
		s_prog_v1 = s_v1prog[0];
		s_v1rt[0] = gx_tex(512, 256, GX_RGBA8, 0, 7);
		s_v1rt[1] = gx_tex(512, 256, GX_RGBA8, 0, 8);
		s_v1atlas = gx_tex(V1GPU_ATLAS, V1GPU_ATLAS, GX_RGBA8, 0, 0);
		if (!s_prog_v1 || !s_v1prog[4] || !s_v1rt[0] || !s_v1rt[1] || !s_v1atlas)
			s_prog_v1 = NULL;
		RSTV_LOG("gvdp2: vdp1 gpu renderer %s", s_prog_v1 ? "ok" : "off");
	}

	v1gpu_grid_adapt = !file_exists_q("ux0:data/restv/v1grid4"); // ADR-090
	s_reuse_on = !file_exists_q("ux0:data/restv/noreuse");
	if (gvdp2_progress)
		gvdp2_progress(280);
	if (s_prog_q && !file_exists_q("ux0:data/restv/noplane")) {
		const GxAttr pc_attr[2] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 }, { "uv", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 8 } };
		s_prog_pcopy = gx_prog(s_vs_pc, s_fs_pc, pc_attr, 2, 16, GX_BLEND_NONE);
		if (s_prog_pcopy) {
			s_pc_scale = gx_param(s_prog_pcopy, "scale");
			s_pc_tex = gx_param(s_prog_pcopy, "tPlane");
		}
		s_plane_on = s_prog_pcopy != NULL;
		rstv_v1_cc_ok = s_prog_v1 && v1_cc_programs_ok && s_v1snap && s_prog_pcopy; // ADR-115
		RSTV_LOG("gvdp2: vdp1 gpu colour calculation %s", rstv_v1_cc_ok ? "ok" : "off");
		if (s_plane_on) {
			const GxAttr q_attr[3] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_S16, 2, 0 }, { "uv", SCE_GXM_ATTRIBUTE_FORMAT_S16, 2, 4 },
				{ "attr", SCE_GXM_ATTRIBUTE_FORMAT_S16, 4, 8 } };
			s_prog_qi = gx_prog(s_vs_q, s_fs_qi, q_attr, 3, 16, GX_BLEND_NONE);
			s_prog_pci = gx_prog(s_vs_pc, s_fs_pci, pc_attr, 2, 16, GX_BLEND_NONE);
			if (s_prog_qi) {
				s_qi_scale = gx_param(s_prog_qi, "scale");
				s_qi_atlas = gx_param(s_prog_qi, "tAtlas");
				s_qi_cfg = gx_param(s_prog_qi, "qcfg");
				s_qi_lo = gx_param(s_prog_qi, "qcodeLo");
				s_qi_hi = gx_param(s_prog_qi, "qcodeHi");
			}
			if (s_prog_pci) {
				s_pci_scale = gx_param(s_prog_pci, "scale");
				s_pci_tex = gx_param(s_prog_pci, "tPlane");
				s_pci_pal = gx_param(s_prog_pci, "tPal");
			}
			s_plane_noidx = file_exists_q("ux0:data/restv/noplaneidx");
			RSTV_LOG("gvdp2: plane index mode %s", s_prog_qi && s_prog_pci && !s_plane_noidx ? "on" : "off");
		}
	}
	RSTV_LOG("gvdp2: plane cache %s", s_plane_on ? "on" : "off");
	// ADR-089: creating a plane texture (2MB, dedicated render target) on first use costs about 9ms for allocation, zero fill and render target creation
	// (14ms layer prep on a minigame's first scene). Create them up front at startup
	for (int L = 0; s_plane_on && L < 4; L++) // NBG only (RBG0 does not use the plane cache)
		if (!s_plane[L])
			s_plane[L] = gx_tex(PLANE_W, PLANE_H, GX_RGBA8, 0, 14 + L);
	for (int L = 0; L < NUM_BG; L++)
		s_lcache[L] = s_reuse_on ? gx_tex(LAYER_W, LAYER_H, GX_RGBA8, 0, 9 + L) /* own render target slot (gx.c) */ : NULL;
	RSTV_LOG("gvdp2: layer reuse %s", s_reuse_on ? "on" : "off");
	for (int i = 0; i < NSETS; i++) {
		FrameSet *s = &s_set[i];
		memset(s, 0, sizeof(*s));
		for (int L = 0; L < NUM_BG; L++) {
			s->layer[L] = gx_tex(LAYER_W, LAYER_H, GX_RGBA8, 0, 1 + L);
			if (!s->layer[L])
				return -1;
			s->layer_px[L] = s->layer[L]->data;
		}
		s->spr = gx_tex(256, 256, GX_RGBA8, 0, 0);
		s->pal = gx_tex(2048, 1, GX_RGBA8, 0, 0);
		s->line = gx_tex(LINE_W, 256, GX_RGBA8, 0, 0);
		s->vram = gx_tex(512, 256, GX_RGBA8, 0, 0);
		s->rows = gx_tex(ROWS_W, 256, GX_RGBA8, 0, 0);
		s->rbg_table = gx_tex(8, 8, GX_RGBA8, 0, 0);
		s->rbg_table_b = gx_tex(8, 8, GX_RGBA8, 0, 0);
		if (!s_slut_pool[0]) {
			int ok = 1;
			for (int k = 0; k < SLUT_POOL; k++)
				ok &= (s_slut_pool[k] = gx_tex(512, 256, GX_RGBA8, 0, 0)) != NULL;
			if (!ok)
				s_slut_pool[0] = NULL;
		}
		s->slut = s_slut_pool[0];
		s->slut_idx = -1;
		if (s->slut)
			s->slut_px = s->slut->data; // marks the table as writable (actual slot is slut_idx)

		if (!s->rbg_table)
			s_prog_rbg = NULL;
		s_out_tex[i] = gx_tex(LAYER_W, LAYER_H, GX_RGBA8, 1, 1 + NUM_BG);
		if (!s->spr || !s->pal || !s->line || !s->vram || !s->rows || !s_out_tex[i])
			return -1;
		s->spr_px = s->spr->data;
		s->pal_px = s->pal->data;
		s->line_px = s->line->data;
		s->vram_px = s->vram->data;
		s->rows_px = s->rows->data;
	}

	// replace video core functions (vidsoft's VDP1 and layer drawing are used as is)
	VIDSoft.Vdp2DrawScreens = gvdp2_draw_screens;
	rstv_skip_back_line = 1; // back/line screens are computed by fill_lines
	VIDSoft.Vdp2DrawEnd = gvdp2_draw_end;
	load_shot_list();
	{
		FILE *gf = fopen("ux0:data/restv/gpufinish", "r");
		s_gpu_finish = 0;
		s_gpu_finish_from = 0;
		if (gf) {
			// if the content is a number >= 2, start at that frame (so game progress matches the run before instrumentation)
			int from = 0;
			if (fscanf(gf, "%d", &from) == 1 && from > 1)
				s_gpu_finish_from = (unsigned)from;
			else
				s_gpu_finish = 1;
			fclose(gf);
		}
		gf = fopen("ux0:data/restv/gpufloor", "r"); // content: 1 = NBG only, 2 = composite only, 3 = both, 4 = sprites only off in composite
		s_gpu_floor = 0;
		if (gf && fscanf(gf, "%d", &s_gpu_floor) != 1)
			s_gpu_floor = 3;
		if (gf)
			fclose(gf);
	}
	// test (ux0:data/restv/gxtest): draw magenta into layer and composite output textures to see if it stays in memory
	if (file_exists_q("ux0:data/restv/gxtest")) {
		static const char *fs_m = "float4 main() : COLOR\n{\n  return float4(1.0, 0.0, 1.0, 1.0);\n}\n";
		const GxAttr c_attr[1] = { { "position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 0 } };
		GxProg *pm = gx_prog(s_vs_clear, fs_m, c_attr, 1, 8, GX_BLEND_NONE);
		GxTex *targets[3] = { s_set[0].layer[0], s_set[0].layer[3], s_out_tex[0] };
		for (int k = 0; k < 3 && pm; k++) {
			gx_begin(targets[k]);
			float *v = (float *)gx_temp(8 * sizeof(float));
			const float d[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
			memcpy(v, d, sizeof(d));
			gx_use(pm);
			gx_draw(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, gx_seq_indices(4), 4);
			gx_end();
		}
		gx_finish();
		for (int k = 0; k < 3; k++)
			RSTV_LOG("gxtest: target %d first px %08x mid px %08x", k, ((uint32_t *)targets[k]->data)[0],
				((uint32_t *)targets[k]->data)[100 * 512 + 100]);
	}
	if (gvdp2_progress)
		gvdp2_progress(300);
	preload_variants();
	if (gvdp2_progress)
		gvdp2_progress(1000);
	s_ok = 1;
	RSTV_LOG("gvdp2: ready");
	return 0;
}

// ADR-211 (owner: fast game list entry, as ReSys12 / ReiREM): the app goes back to the game list in the same process
void gvdp2_game_end(void)
{
	if (!s_ok)
		return;
	prep_wait();
	if (s_end_pend >= 0) { // (the deferred end of the last frame, ADR-186)
		draw_end_work(s_end_pend);
		s_end_pend = -1;
	}
	gx_finish();
}
int gvdp2_game_start(void)
{
	if (!s_ok)
		return gvdp2_init();
	if (gvdp2_progress)
		gvdp2_progress(0);
	// (YabauseInit made every VRAM page and the palette new for the caches, as after a state load.) What was kept
	// from the previous game is dropped
	memset(s_lvalid, 0, sizeof(s_lvalid));
	memset(s_plane_valid, 0, sizeof(s_plane_valid));
	memset(s_plane_tag, 0xFF, sizeof(s_plane_tag));
	memset(s_slut_pool_valid, 0, sizeof(s_slut_pool_valid));
	memset(s_argb_dirty, 0, sizeof(s_argb_dirty));
	s_argb_pend = 0;
	s_atlas_copied = 0;
	s_snap_valid = 0;
	s_snap_vram_serial = 0;
	s_snap_cram_serial = 0xFFFFFFFFu;
	s_v1snap_valid = 0;
	s_cgen++;
	RbgqState *const qs[2] = { &s_rbgq, &s_rbgq_b };
	for (int k = 0; k < 2; k++) {
		memset(qs[k]->valid, 0, sizeof(qs[k]->valid));
		memset(qs[k]->flat_slot, 0xFF, sizeof(qs[k]->flat_slot));
	}
	v1gpu_reset();
	s_frame = 0;
	// this game's variant list (gvdp2_set_game): its compose variants not loaded yet, and the list noted afresh
	s_cl_n = 0;
	s_cl_loaded = 0;
	for (int i = 0; i < s_nvar; i++)
		s_var[i].listed = 0;
	s_preload_again = 1;
	preload_variants();
	if (gvdp2_progress)
		gvdp2_progress(1000);
	RSTV_LOG("gvdp2: ready (game %s)", s_game);
	return 0;
}

void gvdp2_get_stats(gvdp2_stats *out)
{
	*out = s_stats;
	s_stats.t_core = s_stats.t_copy = s_stats.t_fill = s_stats.t_gpu = s_stats.t_gpu_nbg = 0;
	s_stats.t_atlas = 0;
	s_stats.t_gpu_v1 = 0;
	s_stats.v1_verts = 0;
	s_stats.v1_snaps = 0;
	s_stats.t_prep_worker = 0;
	s_stats.t_prep_wait = 0;
	s_stats.v1_decoded = 0;
	s_stats.t_v1build = 0;
	s_stats.t_slut = 0;
	s_stats.t_rbgprep = 0;
	s_stats.atlas_pages = 0;
	memset(s_stats.t_gpu_layer, 0, sizeof(s_stats.t_gpu_layer));
	s_stats.t_quad = s_stats.t_draw = 0;
	memset(s_stats.t_qlayer, 0, sizeof(s_stats.t_qlayer));
}
