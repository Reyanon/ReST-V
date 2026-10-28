#ifndef RSTV_GVDP2_H
#define RSTV_GVDP2_H

// GPU VDP2 composite (ADR-011). Stage 1: CPU (vidsoft) draws the VDP2 background layers into the Titan buffers;
// sprite layer decode, priority, color calc, color offset, shadow and line screen are composited by a GPU shader.
// Frames in an unsupported state go through the vidsoft CPU path unchanged.

// Call after gx_init and after YabauseInit. 0 = success (on failure only the CPU path is used)
void gvdp2_set_game(const char *game); // per-game shader variant list (before gvdp2_init)
int gvdp2_init(void);
// ADR-211: back to the game list without a new process. gvdp2_game_end before the core is freed (the frame in flight
// finishes); gvdp2_game_start in place of gvdp2_init for the games after the first (GPU resources stay, caches go)
void gvdp2_game_end(void);
int gvdp2_game_start(void);
// for the spike line: accumulated times (not reset): prep thread work, prep wait in draw_end, draw_end CPU
void gvdp2_peek_times(unsigned long long *prep_worker, unsigned long long *prep_wait, unsigned long long *draw);

// stats (frame counts)
typedef struct {
	unsigned gpu_frames;
	unsigned cpu_frames;
	unsigned last_fallback_reason;
	unsigned gpu_layers; // NBG layers drawn by GPU (cumulative)
	unsigned cpu_layers; // layers captured but redrawn on CPU (cumulative)
	unsigned quad_layers; // of those, layers drawn as cell quads (cumulative)
	unsigned fast_frames; // frames drawn with the fast composite shader (cumulative)
	unsigned var_frames;  // frames drawn with a specialized variant (cumulative)
	unsigned cmp_vi, cmp_instr; // last frame's composite variant number (0 = generic) and GXP instruction count
	int qn[5], qtry[5];             // last frame's quad count per layer (-1 = pixel path/CPU)
	int qclr[5], qhyb[5];           // last frame's clear strips outside window, scaled-line pixel shader quads (ADR-052)
	unsigned lreuse[5];             // ADR-057: frames where the layer was not redrawn
	int qplane[5];                  // ADR-058: layers drawn from the plane cache last frame
	unsigned plane_draws;           // ADR-058: plane redraw count
	unsigned plane_why[4];          // redraw reason: first, config key, VRAM page, palette
	unsigned plane_rows;            // plane rows redrawn
	unsigned long long t_qlayer[5]; // per-layer prep time (us, 60 frames)
	unsigned lsamev[5], lgen, lpal; // diag: frames with vertices same as previous, frames where generation/palette changed
	unsigned long long t_qbuild, t_qfill, t_qcopy; // quad path: nbgq_build, vertex fill, GPU memory copy (us)
	unsigned nvar;        // variants built
	unsigned rbg_var_frames; // frames drawn with an RBG0 specialized variant (cumulative)
	unsigned rbg_flat_frames, rbg_two_maps; // ADR-067: frames drawn with map-order cell layout, two-map (no plane) frames (cumulative)
	unsigned long long t_slut; // sprite decode table CPU time (worker thread)
	unsigned rbg_layers;  // frames with RBG0 drawn by the GPU rotation shader (cumulative)
	unsigned v1_gpu;      // frames with VDP1 drawn on GPU (cumulative, ADR-023)
	unsigned long long t_gpu_v1; // VDP1 pass GPU time with gpufinish
	unsigned v1_verts;    // VDP1 vertex count (interval sum)
	unsigned v1_snaps;    // ADR-115: framebuffer copies for shadow/half-transparency (interval sum)
	unsigned v1_decoded;  // pixels newly decoded into the sprite cache (interval sum)
	unsigned long long t_v1build; // VDP1 vertex/cache prep CPU time
	unsigned long long t_prep_worker, t_prep_wait; // prep worker thread time, time draw_end waited
	unsigned prep_sync;   // frames prepped on main (cumulative)
	unsigned cpu_mask;    // bits of layers drawn on CPU last frame (1 << L)
	unsigned nbg_why;     // capture why bits of the last CPU NBG layer
	unsigned rbg_why;     // last RBG0 capture reject reason (rstv_rot_why)
	// layer prep time per stage (us, read and reset to 0 by gvdp2_get_stats)
	unsigned long long t_core;  // vidsoft (capture + CPU layers)
	unsigned long long t_copy;  // palette/line/sprite/VRAM copy
	unsigned long long t_fill;  // Titan buffer -> layer texture
	unsigned long long t_gpu;   // with ux0:data/restv/gpufinish: until glFinish right after composite (approx. GPU time)
	unsigned long long t_gpu_nbg; // same method, up to the NBG layer pass
	unsigned long long t_gpu_layer[5]; // per-layer-pass GPU time with gpufinish (N3 N2 N1 N0 R0)
	unsigned long long t_quad;  // cell quad build (incl. atlas update)
	unsigned long long t_atlas; // of that, character atlas update
	unsigned atlas_pages;       // VRAM 4KB pages re-decoded into the atlas (cumulative)
	unsigned long long t_rbgprep; // RBG0 page decode, line vertices
	unsigned long long t_draw;  // CPU time of gvdp2_draw_end (GL calls)
} gvdp2_stats;
void gvdp2_get_stats(gvdp2_stats *out);

#endif
