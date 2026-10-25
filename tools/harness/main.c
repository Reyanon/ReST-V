// ReST-V PC harness: runs an ST-V set headless for N frames and writes frame/audio hashes and screenshots (PPM).
//   harness --roms DIR --game SET [--frames N] [--checkpoint K] [--shot F1,F2,...] [--out DIR]
//           [--coin F] [--start F]   (hold coin/start for 6 frames at that frame)
//           [--gpucheck]             (compare C port of Vita NBG shader against vidsoft layers, nbgcheck.c)
//           [--vdp1thread]           (VDP1 drawing on a helper thread, same path as Vita)
//           [--savestate F PATH] [--loadstate F PATH]  (save/load state after frame F)
//           [--merge]                (run the 9 decilines before HBlankIN as one SH-2 slice, Vita default)
//           [--scspbatch N]          (generate sound N samples at a time with the Vita async-thread batch engine)
//           [--nofmbatch]            (per-sample pipeline instead of batch when FM slots exist, pre ADR-076)
//           [--wav FILE]             (save audio as raw 16-bit stereo)
//           [--pchist CPU FROM]      (CPU 1=master 2=slave, print top per-PC execution counts from frame FROM)
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "cdbase.h"
#include "cs0.h"
#include "m68kcore.h"
#include "peripheral.h"
#include "scsp.h"
#include "scspdsp.h"
#include "scu.h"
#include "sh2core.h"
#include "sh2int.h"
#include "sh2d.h"
#include "memory.h"
#include "stvio.h"
#include "stv.h"
#include "smpc.h"
#include "rstv_prof.h"
#include "vdp1.h"
#include "vdp2.h"
extern u8 *Vdp1Ram;

// --vdp2stats: count frames per VDP2 register combination (at frame end) (feature survey for the new renderer)
#define MAX_COMBOS 256
static char s_combo[MAX_COMBOS][256];
static int s_combo_n[MAX_COMBOS], s_combos;
static void vdp2_stats_frame(void)
{
	const Vdp2 *r = Vdp2Regs;
	char buf[256];
	snprintf(buf, sizeof(buf),
		"TVMD=%04x BGON=%04x CHCTL=%04x/%04x BMPN=%04x/%04x PNCN=%04x,%04x,%04x,%04x PLSZ=%04x MZ=%04x SCR=%04x "
		"W=%04x,%04x,%04x,%04x CC=%04x SFCC=%04x SFPR=%04x LNCL=%04x SPCTL=%04x CLOF=%04x/%04x RPMD=%04x KT=%04x RAM=%04x",
		r->TVMD, r->BGON, r->CHCTLA, r->CHCTLB, r->BMPNA, r->BMPNB, r->PNCN0, r->PNCN1, r->PNCN2, r->PNCN3, r->PLSZ,
		r->MZCTL, r->SCRCTL, r->WCTLA, r->WCTLB, r->WCTLC, r->WCTLD, r->CCCTL, r->SFCCMD, r->SFPRMD, r->LNCLEN, r->SPCTL,
		r->CLOFEN, r->CLOFSL, r->RPMD, r->KTCTL, r->RAMCTL);
	for (int i = 0; i < s_combos; i++)
		if (!strcmp(s_combo[i], buf)) { s_combo_n[i]++; return; }
	if (s_combos < MAX_COMBOS) {
		strcpy(s_combo[s_combos], buf);
		s_combo_n[s_combos++] = 1;
	}
}
#include "vidsoft.h"
#include "yabause.h"
#include "yui.h"

// ---- core lists (defined by the frontend) ----
#ifdef RSTV_DYNAREC
extern SH2Interface_struct SH2Dyn;
SH2Interface_struct *SH2CoreList[] = { &SH2Interpreter, &SH2Dyn, NULL };
#else
SH2Interface_struct *SH2CoreList[] = { &SH2Interpreter, NULL };
#endif
PerInterface_struct *PERCoreList[] = { &PERDummy, NULL };
CDInterface *CDCoreList[] = { &DummyCD, NULL };
VideoInterface_struct *VIDCoreList[] = { &VIDSoft, NULL };
M68K_struct *M68KCoreList[] = { &M68KMusashi, NULL };

// ---- sound: collect samples and hash ----
#define SNDCORE_HARNESS 20
static uint64_t s_audio_hash = 0xcbf29ce484222325ull;
static uint64_t s_audio_samples;

static void fnv(uint64_t *h, const void *p, size_t n)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++) {
		*h ^= b[i];
		*h *= 0x100000001b3ull;
	}
}

static int snd_init(void) { return 0; }
static void snd_deinit(void) {}
static int snd_reset(void) { return 0; }
static int snd_vf(int f) { (void)f; return 0; }
static FILE *s_wav; // --wav: raw 16-bit stereo sample dump
static void snd_update(u32 *l, u32 *r, u32 n)
{
	for (u32 i = 0; i < n; i++) {
		int32_t a = (int32_t)l[i], b = (int32_t)r[i];
		int16_t s[2] = { (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a),
			(int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b) };
		fnv(&s_audio_hash, s, sizeof(s));
		if (s_wav)
			fwrite(s, sizeof(s), 1, s_wav);
	}
	s_audio_samples += n;
}
static u32 snd_space(void) { return 0x10000; }
static void snd_nop(void) {}
static void snd_vol(int v) { (void)v; }
static SoundInterface_struct SNDHarness = { SNDCORE_HARNESS, "Harness", snd_init, snd_deinit, snd_reset,
	snd_vf, snd_update, snd_space, snd_nop, snd_nop, snd_vol };
SoundInterface_struct *SNDCoreList[] = { &SNDHarness, &SNDDummy, NULL };

// ---- Yui callbacks ----
static uint32_t s_frames_drawn;
static int s_w, s_h;
static uint64_t s_video_hash = 0xcbf29ce484222325ull;

void YuiErrorMsg(const char *string) { fprintf(stderr, "core error: %s\n", string); }
void YuiMsg(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}
int YuiUseOGLOnThisThread(void) { return 0; }
int YuiRevokeOGLOnThisThread(void) { return 0; }
int YuiGetFB(void) { return 0; }

void YuiSwapBuffers(void)
{
	int interlace = 0;
	VIDCore->GetNativeResolution(&s_w, &s_h, &interlace);
	if (dispbuffer && s_w > 0 && s_h > 0)
		fnv(&s_video_hash, dispbuffer, (size_t)s_w * s_h * sizeof(pixel_t));
	s_frames_drawn++;
}

static void save_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f || !dispbuffer)
		return;
	fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
	for (int i = 0; i < s_w * s_h; i++) {
		uint32_t p = dispbuffer[i];
		uint8_t rgb[3] = { p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static u64 prof_clock(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

u32 rstv_harness_frame; /* compile log (DynarecSh2.cpp RSTV_COMPLOG) */
void rstv_log_printf(const char *fmt, ...) /* the Vita UDP log (core diags); printed with RSTV_LOG=1 */
{
	if (!getenv("RSTV_LOG")) return;
	va_list ap;
	va_start(ap, fmt);
	printf("log ");
	vprintf(fmt, ap);
	putchar(10);
	va_end(ap);
}
#ifdef RSTV_DYNAREC
#include <pthread.h>
void *rstv_watchdog(void *arg)
{
	(void)arg;
	u32 last = 0;
	int same = 0;
	for (;;) {
		sleep(3);
		if (rstv_harness_frame == last) {
			if (++same == 2) {
				extern void rstv_slave_debug(char *out, int size);
				char b[256];
				rstv_slave_debug(b, sizeof(b));
				extern volatile int rstv_act; extern volatile unsigned rstv_act_pc;
				fprintf(stderr, "watchdog: stuck at frame %u line %d deciline %d: %s msh2 pc %08x ssh2 pc %08x act %d actpc %08x%c", last, (int)yabsys.LineCount,
					(int)yabsys.DecilineCount, b, SH2Core->GetPC(MSH2), SH2Core->GetPC(SSH2), rstv_act, rstv_act_pc, 10);
				fflush(stderr);
			}
		} else
			same = 0;
		last = rstv_harness_frame;
	}
	return NULL;
}
#endif
static FILE *s68_fp;
static long s68_left;
static void trace68(unsigned pc)
{
	extern void (*rstv_m68k_trace_fn)(unsigned pc);
	if (s68_left-- <= 0) {
		rstv_m68k_trace_fn = NULL;
		fclose(s68_fp);
		s68_fp = NULL;
		return;
	}
	fprintf(s68_fp, "%06x\n", pc & 0xFFFFFF);
}

static void harness_load_tick(void) {}
int main(int argc, char **argv)
{
	const char *roms = "roms", *game = "sandor", *out = ".";
	int frames = 600, checkpoint = 60;
	int save_at = -1, load_at = -1; // --savestate F PATH / --loadstate F PATH: after frame F
	const char *save_path = NULL, *load_path = NULL;
	char shots[512] = "";
	int coin_at = -1, start_at = -1, mash_at = -1; // --mash F: random lever/button mashing, same as Vita measurement (run_perf MASH)
	unsigned dis_addr = 0, dis_n = 0;
	int use_dynarec = 0;
	int no_idle = 0, vdp2stats = 0, gpucheck = 0, pchist_cpu = 0, pchist_from = 0;
	extern int rstv_pc_hist_on;
	extern u32 rstv_pc_hist[0x80000];
	extern int rstv_vdp1_thread;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--roms") && i + 1 < argc) roms = argv[++i];
		else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpoint = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) snprintf(shots, sizeof(shots), ",%s,", argv[++i]);
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "--coin") && i + 1 < argc) coin_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--start") && i + 1 < argc) start_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mash") && i + 1 < argc) mash_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dis") && i + 2 < argc) { dis_addr = strtoul(argv[++i], NULL, 16); dis_n = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--no-idle")) no_idle = 1;
		else if (!strcmp(argv[i], "--loadtick")) { extern void (*rstv_load_tick)(void); rstv_load_tick = harness_load_tick; } // inflate in steps (Vita)
		else if (!strcmp(argv[i], "--no-icap-nested")) { extern int rstv_icap_nested; rstv_icap_nested = 0; }
		else if (!strcmp(argv[i], "--dynarec")) {
			use_dynarec = 1;
#ifdef RSTV_DYNAREC
			// differential check (RSTV_DYNCHK) runs one block at a time: disable block linking
			extern int rstv_chain_on;
			extern int rstv_memfast;
			if (getenv("RSTV_DYNCHK"))
				rstv_chain_on = 0, rstv_memfast = 0;
			if (getenv("RSTV_QLOG"))
				rstv_memfast = 0;
			if (getenv("RSTV_NOCHAIN")) // every block through Execute (cycle probes with RSTV_BTRACE)
				rstv_chain_on = 0;
			extern int rstv_tier_n;
			if (getenv("RSTV_ICAPCYC")) { // dynarec: cycles the woken CPU runs inside an input-capture write
				extern int rstv_icap_cycles;
				rstv_icap_cycles = atoi(getenv("RSTV_ICAPCYC"));
			}
			if (getenv("RSTV_NOTIER"))
				rstv_tier_n = 0;
			if (getenv("RSTV_DEVTRAP")) { extern unsigned int rstv_dev_trap; rstv_dev_trap = (unsigned)strtoul(getenv("RSTV_DEVTRAP"), NULL, 16); }
			if (getenv("RSTV_CHTRACE")) { // "FROM TO": first-seen master block PCs (chained "c" or from Execute "x")
				extern unsigned int rstv_dev_trace_from, rstv_dev_trace_to;
				extern int rstv_chain_trace_on, rstv_chain_prof;
				sscanf(getenv("RSTV_CHTRACE"), "%u %u", &rstv_dev_trace_from, &rstv_dev_trace_to);
				rstv_chain_trace_on = rstv_chain_prof = 1;
			}
			extern int rstv_dyn_litc, rstv_dyn_lazyff; // ADR-134
			if (getenv("RSTV_NOLITC"))
				rstv_dyn_litc = 0;
			if (getenv("RSTV_NOLAZYFF"))
				rstv_dyn_lazyff = 0;
			extern int rstv_dyn_rc, rstv_dyn_blkcap; // ADR-135
			if (getenv("RSTV_NORC"))
				rstv_dyn_rc = 0;
			if (getenv("RSTV_BLKCAP"))
				rstv_dyn_blkcap = atoi(getenv("RSTV_BLKCAP"));
			extern int rstv_dyn_selfloop;
			if (getenv("RSTV_NOSELFLOOP"))
				rstv_dyn_selfloop = 0;
			extern int rstv_dyn_rcbr;
			if (getenv("RSTV_NORCBR"))
				rstv_dyn_rcbr = 0;
			extern int rstv_slave_cfg, rstv_slave_lead; // ADR-136: RSTV_ASYNC=lead runs the slave SH-2 on its own thread
			if (getenv("RSTV_ASYNC")) {
				rstv_slave_cfg = 1;
				if (atoi(getenv("RSTV_ASYNC")) > 0)
					rstv_slave_lead = atoi(getenv("RSTV_ASYNC"));
			}
			extern int rstv_hle_on;
			if (getenv("RSTV_NOHLE"))
				rstv_hle_on = 0;
			{ extern int rstv_hle_redetect; if (getenv("RSTV_NOREDETECT")) rstv_hle_redetect = 0; } // ADR-198 A/B
			{ extern int rstv_lzb_off; if (getenv("RSTV_NOLZB")) rstv_lzb_off = 1; }
			extern int rstv_hle_stub_on; // ADR-081: native-call blocks (off with the HLE checker, which runs in Execute)
			if (getenv("RSTV_NOHLESTUB") || getenv("RSTV_HLECHK"))
				rstv_hle_stub_on = 0;
			extern int rstv_sleep_max;
			if (getenv("RSTV_NOSLEEP"))
				rstv_sleep_max = 0;
			if (getenv("RSTV_SLEEPMAX"))
				rstv_sleep_max = atoi(getenv("RSTV_SLEEPMAX"));
			extern int rstv_link_on;
			if (getenv("RSTV_LINK"))
				rstv_link_on = 1;
#endif
		}
		else if (!strcmp(argv[i], "--vdp2stats")) vdp2stats = 1;
		else if (!strcmp(argv[i], "--gpucheck")) gpucheck = 1;
		else if (!strcmp(argv[i], "--savestate") && i + 2 < argc) { save_at = atoi(argv[++i]); save_path = argv[++i]; }
		else if (!strcmp(argv[i], "--loadstate") && i + 2 < argc) { load_at = atoi(argv[++i]); load_path = argv[++i]; }
		else if (!strcmp(argv[i], "--vdp1thread")) rstv_vdp1_thread = 1;
		else if (!strcmp(argv[i], "--merge")) { extern int rstv_deciline_merge; rstv_deciline_merge = 1; }
		else if (!strcmp(argv[i], "--scspbatch") && i + 1 < argc) { extern int rstv_scsp_batch_mode; rstv_scsp_batch_mode = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--dsppipe")) { extern int rstv_scsp_dsp_thread; rstv_scsp_dsp_thread = 2; } // ADR-158 check (inline)
		else if (!strcmp(argv[i], "--trap") && i + 1 < argc) { extern u32 rstv_trap_pc; extern int rstv_trap_from; char *e; rstv_trap_pc = (u32)strtoul(argv[++i], &e, 16); if (*e == '@') rstv_trap_from = atoi(e + 1); }
		else if (!strcmp(argv[i], "--tracepc") && i + 2 < argc) { extern u32 rstv_trace_pc, rstv_trace_n; rstv_trace_pc = (u32)strtoul(argv[++i], NULL, 16); rstv_trace_n = (u32)atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--nofmbatch")) { extern int rstv_scsp_fm_batch; rstv_scsp_fm_batch = 0; }
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) s_wav = fopen(argv[++i], "wb");
		else if (!strcmp(argv[i], "--pchist") && i + 2 < argc) { pchist_cpu = atoi(argv[++i]); pchist_from = atoi(argv[++i]); }
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}

	yabauseinit_struct yinit;
	memset(&yinit, 0, sizeof(yinit));
	yinit.percoretype = PERCORE_DUMMY;
	yinit.sh2coretype = SH2CORE_INTERPRETER;
#ifdef RSTV_DYNAREC
	if (use_dynarec)
		yinit.sh2coretype = 3; // SH2CORE_DYNAMIC (devmiyax)
#else
	(void)use_dynarec;
#endif
	yinit.vidcoretype = VIDCORE_SOFT;
	yinit.sndcoretype = SNDCORE_HARNESS;
	yinit.m68kcoretype = M68KCORE_MUSASHI;
	yinit.cdcoretype = CDCORE_DUMMY;
	yinit.carttype = CART_NONE;
	yinit.regionid = 0;
	yinit.biospath = NULL;
	yinit.cdpath = NULL;
	yinit.buppath = "harness_bup.bin";
	yinit.mpegpath = NULL;
	yinit.cartpath = NULL;
	yinit.videoformattype = VIDEOFORMATTYPE_NTSC;
	yinit.clocksync = 1;
	yinit.basetime = 0;
	yinit.usethreads = 0;
	yinit.use_new_scsp = 1;
	yinit.scsp_sync_count_per_frame = 1;
	yinit.scsp_main_mode = 0;
	{ extern int rstv_scsp_lag; if (getenv("RSTV_SNDLAG")) rstv_scsp_lag = 1; } // the Vita default (sound one frame behind)
	yinit.stv_rom_dir = roms;
	yinit.stv_romset = game;

	rstv_prof_clock = prof_clock;
	double t0 = now_s();
	if (YabauseInit(&yinit) != 0) {
		fprintf(stderr, "YabauseInit failed\n");
		return 1;
	}
	fprintf(stderr, "init %.2fs\n", now_s() - t0);
	if (getenv("RSTV_NV")) { // EEPROM file like the Vita's ux0:data/restv/nvram/<set>.nv
		extern int eeprom_load(const char *path);
		fprintf(stderr, "eeprom %s: %s\n", getenv("RSTV_NV"), eeprom_load(getenv("RSTV_NV")) == 0 ? "loaded" : "failed");
	}
	if (no_idle)
		STVSetIdleHacks(0);
	extern void nbgcheck_install(void), nbgcheck_report(void);
	if (gpucheck)
		nbgcheck_install();

	t0 = now_s();
	double tc = t0;
	if (getenv("RSTV_V1WATCH")) { // diag: print who writes 0xFFFF to the VDP1 control registers
		extern int rstv_v1watch;
		rstv_v1watch = atoi(getenv("RSTV_V1WATCH")) > 1 ? 2 : 1; // 2 = also list level 0 DMA starts
	}
	if (getenv("RSTV_V1GPULOG")) { // diag: record VDP1 GPU lists like the Vita (no GPU here) and log them
		extern int rstv_vdp1_gpu_on;
		rstv_vdp1_gpu_on = 1;
	}
	if (getenv("RSTV_DMAINSTANT")) { // SCU DMA finishes when started (Vita: ux0:data/restv/dmainstant)
		extern int rstv_dma_instant;
		rstv_dma_instant = 1;
	}
	if (getenv("RSTV_V1RAMW")) { // diag: writes into VDP1 RAM [lo, hi) (hex offsets)
		extern u32 rstv_v1ramw_lo, rstv_v1ramw_hi;
		sscanf(getenv("RSTV_V1RAMW"), "%x,%x", &rstv_v1ramw_lo, &rstv_v1ramw_hi);
	}
	if (getenv("RSTV_HALF")) { // ADR-100: hi-res / interlace screens at half size (Vita default)
		extern int rstv_hires_half;
		rstv_hires_half = 1;
	}
	if (getenv("RSTV_LAYEROFF")) { // debug: bit n = do not draw NBGn (vidsoft)
		extern int rstv_layer_off;
		rstv_layer_off = atoi(getenv("RSTV_LAYEROFF"));
	}
#ifdef RSTV_DYNAREC
	if (getenv("RSTV_ASYNC")) {
		// diag: a watchdog prints the slave thread state when frames stop advancing (deadlock hunting)
		extern void *rstv_watchdog(void *);
		pthread_t wt;
		pthread_create(&wt, NULL, rstv_watchdog, NULL);
	}
#endif
	for (int f = 1; f <= frames; f++) {
		rstv_harness_frame = (u32)f;
		{ static int pl = -2; if (pl == -2) { const char *e = getenv("RSTV_PCLOG"); pl = e ? atoi(e) : -1; }
		  if (pl >= 0 && f >= pl && f < pl + 8) // diag: dynarec PCs and PR at frame start
			printf("pclog f%d m %08x pr %08x r11 %08x r13 %08x s %08x pr %08x%c", f, SH2Core->GetPC(MSH2), SH2Core->GetPR(MSH2), SH2Core->GetGPR(MSH2, 11), SH2Core->GetGPR(MSH2, 13), SH2Core->GetPC(SSH2), SH2Core->GetPR(SSH2), 10); }
#ifdef RSTV_DYNAREC
		{ extern void rstv_litwatch_check(u32 frame); rstv_litwatch_check((u32)f); }
#endif
		u8 sys = 0;
		if (coin_at >= 0 && f >= coin_at && f < coin_at + 6) sys |= STV_SYS_COIN1;
		if (start_at >= 0 && f >= start_at && f < start_at + 6) sys |= STV_SYS_START1;
		u8 p1 = 0;
		{ // RSTV_B1EVERY=FROM,PERIOD: button 1 for 6 frames every PERIOD frames from FROM (MAME coinstart.lua BTN_AT)
			static int bf = -2, bp = 90;
			if (bf == -2) { const char *e = getenv("RSTV_B1EVERY"); bf = -1; if (e) sscanf(e, "%d,%d", &bf, &bp); }
			if (bf >= 0 && f >= bf && (f - bf) % bp < 6) p1 |= STV_IN_B1;
		}
		if (mash_at >= 0 && f >= mash_at) {
			// deterministic random, same as MASH in vita/src/frontend/main.c: new combo every 8 frames, coin/start every 600 frames
			static uint32_t rng = 0x12345678u;
			static u8 held;
			const unsigned t = (unsigned)(f - mash_at);
			if ((t & 7) == 0 || getenv("RSTV_MASHHASH")) {
				// RSTV_MASHHASH: the combination is a function of the 8-frame slot only (a run loaded mid-way gets the
				// same inputs as the run it was saved from)
				if (getenv("RSTV_MASHHASH")) { rng = (u32)(f / 8) * 2654435761u + 0x9E3779B9u; rng ^= rng >> 15; rng *= 2246822519u; rng ^= rng >> 13; }
				else { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; }
				static const u8 dirs[5] = { 0, STV_IN_UP, STV_IN_DOWN, STV_IN_LEFT, STV_IN_RIGHT };
				held = (u8)(dirs[rng % 5] | (((rng >> 8) & 1) ? STV_IN_B1 : 0) | (((rng >> 9) & 1) ? STV_IN_B2 : 0) |
					(((rng >> 10) & 3) == 0 ? STV_IN_B3 : 0));
			}
			p1 = held;
			if (t % 600 < 6) sys |= STV_SYS_COIN1;
			else if (t % 600 >= 30 && t % 600 < 36) sys |= STV_SYS_START1;
		}
		{	// RSTV_HOLD=from,to,bits: P1 bits (STV_IN_*) held over [from, to) — scripted input to compare with MAME
			static int hf = -2, ht, hb;
			if (hf == -2) { hf = -1; const char *e = getenv("RSTV_HOLD"); if (e) sscanf(e, "%d,%d,%i", &hf, &ht, &hb); }
			if (hf >= 0 && f >= hf && f < ht) p1 |= (u8)hb;
		}
		{	// RSTV_SYSHOLD=from,to,bits: system bits (STV_SYS_*, e.g. 4 = TEST) held over [from, to)
			static int sf = -2, st, sb;
			if (sf == -2) { sf = -1; const char *e = getenv("RSTV_SYSHOLD"); if (e) sscanf(e, "%d,%d,%i", &sf, &st, &sb); }
			if (sf >= 0 && f >= sf && f < st) sys |= (u8)sb;
		}
		StvIoSetInputs(p1, 0, sys);
		{	// RSTV_MJ=from,to,row,bits: mahjong panel keys (row layout in stvio.h)
			static int mf = -2, mt, mr, mb;
			if (mf == -2) { mf = -1; const char *e = getenv("RSTV_MJ"); if (e) sscanf(e, "%d,%d,%d,%i", &mf, &mt, &mr, &mb); }
			u8 rows[5] = { 0, 0, 0, 0, 0 };
			if (mf >= 0 && f >= mf && f < mt && mr >= 0 && mr < 5) rows[mr] = (u8)mb;
			if (mf >= 0) StvIoSetMahjong(rows);
		}
		{
			extern int rstv_smpc_log;
			static int log_from = -2;
			if (log_from == -2) { const char *e = getenv("RSTV_SMPC_LOG"); log_from = e ? atoi(e) : -1; }
			static int log_n = -1;
			if (log_n < 0) { const char *e = getenv("RSTV_SMPC_LOGN"); log_n = e ? atoi(e) : 30; }
			rstv_smpc_log = (log_from >= 0 && f >= log_from && f < log_from + log_n);
		}

		{	// RSTV_68KTRACE=from,count[,file]: log sound 68K instruction PCs (count entries from frame from)
			static int tf = -2;
			if (tf == -2) {
				tf = -1;
				const char *e = getenv("RSTV_68KTRACE");
				if (e) {
					static char path[256] = "/tmp/68ktrace.txt";
					sscanf(e, "%d,%ld,%255s", &tf, &s68_left, path);
					s68_fp = fopen(path, "w");
				}
			}
			extern void (*rstv_m68k_trace_fn)(unsigned pc);
			if (tf >= 0 && f == tf && s68_fp)
				rstv_m68k_trace_fn = trace68;
		}
		if (pchist_cpu && f == pchist_from)
			rstv_pc_hist_on = pchist_cpu;
		{ // diag: RSTV_POKE=F1-F2:FILE (and RSTV_POKE2) writes the bytes "addr value" (hex, one per line) before every frame F1..F2
			static int pinit, pf1[2] = { -1, -1 }, pf2[2], pn[2];
			static u32 pa[2][4096];
			static u8 pv[2][4096];
			if (!pinit) {
				pinit = 1;
				for (int w = 0; w < 2; w++) {
					const char *e = getenv(w ? "RSTV_POKE2" : "RSTV_POKE");
					char path[512];
					if (e && sscanf(e, "%d-%d:%511s", &pf1[w], &pf2[w], path) == 3) {
						FILE *pfp = fopen(path, "r");
						unsigned a, v;
						while (pfp && pn[w] < 4096 && fscanf(pfp, "%x %x", &a, &v) == 2) { pa[w][pn[w]] = a; pv[w][pn[w]] = (u8)v; pn[w]++; }
						if (pfp) fclose(pfp);
						printf("poke%d %d bytes f%d-%d\n", w + 1, pn[w], pf1[w], pf2[w]);
					} else
						pf1[w] = -1;
				}
			}
			for (int w = 0; w < 2; w++)
				if (pf1[w] >= 0 && f >= pf1[w] && f <= pf2[w])
					for (int k = 0; k < pn[w]; k++)
						MappedMemoryWriteByteNocache(pa[w][k], pv[w][k]);
		}
		YabauseExec();
		{ extern void rstv_v1watch_frame(void); rstv_v1watch_frame(); }
		// load before save: --loadstate F A --savestate F B re-saves what was just loaded (state round-trip check)
		if (f == load_at)
			printf("loadstate f%d %s: %d\n", f, load_path, rstv_state_load(load_path));
		if (f == save_at)
			printf("savestate f%d %s: %d\n", f, save_path, rstv_state_save(save_path));
		if (getenv("RSTV_SCSPDSP") && f == atoi(getenv("RSTV_SCSPDSP"))) {
			extern void rstv_scsp_dsp_report(void);
			rstv_scsp_dsp_report();
		}
		if (getenv("RSTV_DSPPROF")) { // "F0 F1": SCU DSP instructions per frame over [F0, F1], then the hot program addresses
			static unsigned i0;
			int f0 = 0, f1 = 0;
			sscanf(getenv("RSTV_DSPPROF"), "%d %d", &f0, &f1);
			extern unsigned rstv_dsp_insns, rstv_dsp_pchist[256];
			extern unsigned long long rstv_dsp_ns;
			static unsigned long long ns0;
			extern scudspregs_struct *ScuDsp;
			if (f == f0) {
				i0 = rstv_dsp_insns;
				ns0 = rstv_dsp_ns;
				memset(rstv_dsp_pchist, 0, sizeof(rstv_dsp_pchist));
			}
			if (f == f1 && f1 > f0) {
				printf("dspprof f%d-f%d: %.0f insns a frame, %.1f ns an insn, hash %08x%c", f0, f1, (double)(rstv_dsp_insns - i0) / (f1 - f0),
					(double)(rstv_dsp_ns - ns0) / (rstv_dsp_insns - i0 + 1), 0u, 10);
				if (getenv("RSTV_DSPQUIET"))
					memset(rstv_dsp_pchist, 0, sizeof(rstv_dsp_pchist));
				for (int a = 0; a < 256; a++)
					if (rstv_dsp_pchist[a]) {
						char buf[128];
						ScuDspDisasm((u8)a, buf);
						printf("dsp %02x %8.1f %08x %s%c", a, (double)rstv_dsp_pchist[a] / (f1 - f0), (unsigned)ScuDsp->ProgramRam[a], buf, 10);
					}
			}
		}
		if (getenv("RSTV_V2DIFF")) { // "F0 F1": VDP2 VRAM bytes changed this frame, per 4KB page (first..last offset)
			static u8 *prev;
			int f0 = 0, f1 = 0;
			sscanf(getenv("RSTV_V2DIFF"), "%d %d", &f0, &f1);
			extern u8 *Vdp2Ram;
			if (f >= f0 && f <= f1 && prev) {
				printf("v2diff f%d:", f);
				for (int pg = 0; pg < 128; pg++) {
					int n = 0, a = -1, b = -1;
					for (int k = 0; k < 4096; k++)
						if (Vdp2Ram[pg * 4096 + k] != prev[pg * 4096 + k]) {
							n++;
							if (a < 0) a = k;
							b = k;
						}
					if (n)
						printf(" p%d:%d[%03x-%03x]", pg, n, a, b);
				}
				printf("%c", 10);
			}
			if (!prev)
				prev = (u8 *)malloc(0x80000);
			memcpy(prev, Vdp2Ram, 0x80000);
		}
		if (getenv("RSTV_V2VRAM") && f == atoi(getenv("RSTV_V2VRAM"))) { // VDP2 VRAM 512KB (as the SH-2 reads it) -> /tmp/v2vram.bin
			FILE *vf = fopen("/tmp/v2vram.bin", "wb");
			for (u32 a = 0; vf && a < 0x80000; a += 4) {
				const u32 v = MappedMemoryReadLongNocache(0x25E00000 + a);
				const u8 b4[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
				fwrite(b4, 1, 4, vf);
			}
			if (vf) fclose(vf);
		}
		if (getenv("RSTV_V2LOAD") && f == atoi(getenv("RSTV_V2LOAD"))) { // VDP2 VRAM 512KB from /tmp/v2load.bin (e.g. a MAME dump: data vs. renderer)
			FILE *vf = fopen("/tmp/v2load.bin", "rb");
			u8 b4[4];
			for (u32 a = 0; vf && a < 0x80000 && fread(b4, 1, 4, vf) == 4; a += 4)
				MappedMemoryWriteLongNocache(0x25E00000 + a, (u32)b4[0] << 24 | (u32)b4[1] << 16 | (u32)b4[2] << 8 | b4[3]);
			if (vf) fclose(vf);
		}
		if (getenv("RSTV_V2DUMP") && f == atoi(getenv("RSTV_V2DUMP"))) { // VDP2 registers by name (end of frame and line 100)
			for (int w = 0; w < 2; w++) {
				const Vdp2 *r = w ? &Vdp2Lines[100] : Vdp2Regs;
				printf("v2 %s: BGON %04x CHCTLA %04x CHCTLB %04x PNCN0 %04x PNCN1 %04x PLSZ %04x SCRCTL %04x ZMCTL %04x MZCTL %04x\n",
					w ? "line100" : "end", r->BGON, r->CHCTLA, r->CHCTLB, r->PNCN0, r->PNCN1, r->PLSZ, r->SCRCTL, r->ZMCTL, r->MZCTL);
				printf("v2 %s: PRINA %04x PRINB %04x PRIR %04x PRISA %04x SPCTL %04x SDCTL %04x CCCTL %04x SFPRMD %04x SFCCMD %04x SFSEL %04x SFCODE %04x\n",
					w ? "line100" : "end", r->PRINA, r->PRINB, r->PRIR, r->PRISA, r->SPCTL, r->SDCTL, r->CCCTL, r->SFPRMD, r->SFCCMD, r->SFSEL, r->SFCODE);
				printf("v2 %s: WCTLA %04x WCTLB %04x WCTLC %04x WCTLD %04x WPSX0 %d WPSY0 %d WPEX0 %d WPEY0 %d LWTA0 %08x LNCLEN %04x CCRNA %04x\n",
					w ? "line100" : "end", r->WCTLA, r->WCTLB, r->WCTLC, r->WCTLD, r->WPSX0, r->WPSY0, r->WPEX0, r->WPEY0, (unsigned)r->LWTA0.all,
					r->LNCLEN, r->CCRNA);
				printf("v2 %s: SCXIN0 %d SCYIN0 %d SCXIN1 %d SCYIN1 %d MPOFN %04x MPABN0 %04x MPCDN0 %04x MPABN1 %04x MPCDN1 %04x CRAOFA %04x\n",
					w ? "line100" : "end", r->SCXIN0, r->SCYIN0, r->SCXIN1, r->SCYIN1, r->MPOFN, r->MPABN0, r->MPCDN0, r->MPABN1, r->MPCDN1, r->CRAOFA);
			}
			{
				extern int vdp2width, vdp2height, vdp2_x_hires, vdp2_interlace;
				printf("v2 mode: TVMD %04x %dx%d hires %d interlace %d | VDP1 TVMR %04x FBCR %04x PTMR %04x EWDR %04x EWLR %04x EWRR %04x\n",
					Vdp2Regs->TVMD, vdp2width, vdp2height, vdp2_x_hires, vdp2_interlace, Vdp1Regs->TVMR, Vdp1Regs->FBCR, Vdp1Regs->PTMR,
					Vdp1Regs->EWDR, Vdp1Regs->EWLR, Vdp1Regs->EWRR);
			}
			{
				// NBG1 line scroll table (LSTA1, VDP2 VRAM): x integer per entry (8.8 fixed, bits 26..16)
				const Vdp2 *r = Vdp2Regs;
				const u32 lsta = ((u32)r->LSTA1.all & 0x7FFFE) << 1;
				printf("v2 LSTA1 %05x SCXIN1 %d SCXDN1 %04x SCYIN1 %d:", (unsigned)lsta, r->SCXIN1, r->SCXDN1, r->SCYIN1);
				for (int k = 0; k < 32; k++)
					printf(" %d", (int)((T1ReadLong(Vdp2Ram, (lsta + k * 4) & 0x7FFFF) >> 16) & 0x7FF));
				printf("\n");
			}
		}
		if (getenv("RSTV_V1DUMP") && f == atoi(getenv("RSTV_V1DUMP"))) { // VDP1 command list (walks jumps/calls like the VDP1)
			u32 ad = 0, ret = 0;
			int sub = 0;
			for (int n = 0; n < 3000; n++) {
				const u16 ctrl = T1ReadWord(Vdp1Ram, ad);
				if (ctrl & 0x8000) {
					printf("v1 end @%05x\n", (unsigned)ad);
					break;
				}
				const int jp = (ctrl >> 12) & 7;
				const u16 link = T1ReadWord(Vdp1Ram, ad + 2);
				const s16 xa = (s16)T1ReadWord(Vdp1Ram, ad + 0x0C), ya = (s16)T1ReadWord(Vdp1Ram, ad + 0x0E);
				const s16 xb = (s16)T1ReadWord(Vdp1Ram, ad + 0x10), yb = (s16)T1ReadWord(Vdp1Ram, ad + 0x12);
				const s16 xc = (s16)T1ReadWord(Vdp1Ram, ad + 0x14), yc = (s16)T1ReadWord(Vdp1Ram, ad + 0x16);
				const s16 xd = (s16)T1ReadWord(Vdp1Ram, ad + 0x18), yd = (s16)T1ReadWord(Vdp1Ram, ad + 0x1A);
				printf("v1 %4d @%05x %s cmd %x jp %d dir %d pmod %04x colr %04x srca %05x size %dx%d A(%d,%d) B(%d,%d) C(%d,%d) D(%d,%d) grda %04x\n",
					n, (unsigned)ad, (jp & 4) ? "SKIP" : "    ", ctrl & 0xF, jp, (ctrl >> 4) & 3, T1ReadWord(Vdp1Ram, ad + 4),
					T1ReadWord(Vdp1Ram, ad + 6), (unsigned)T1ReadWord(Vdp1Ram, ad + 8) << 3, ((T1ReadWord(Vdp1Ram, ad + 10) >> 8) & 0x3F) * 8,
					T1ReadWord(Vdp1Ram, ad + 10) & 0xFF, xa, ya, xb, yb, xc, yc, xd, yd, T1ReadWord(Vdp1Ram, ad + 0x1C));
				switch (jp & 3) {
				case 0: ad += 0x20; break;                                   /* next */
				case 1: ad = (u32)link << 3; break;                          /* assign */
				case 2: if (!sub) { ret = ad + 0x20; sub = 1; } ad = (u32)link << 3; break; /* call */
				case 3: if (sub) { ad = ret; sub = 0; } else ad += 0x20; break; /* return */
				}
				ad &= 0x7FFFF;
			}
		}
		if (getenv("RSTV_PCF")) // per frame: master / slave PC, PR, SR (where each CPU ends the frame)
			printf("pcf f%d M pc %08x pr %08x sr %03x | S pc %08x pr %08x sr %03x\n", f, MSH2->regs.PC, MSH2->regs.PR, (unsigned)MSH2->regs.SR.all,
				SSH2->regs.PC, SSH2->regs.PR, (unsigned)SSH2->regs.SR.all);
		{
			// RSTV_ROTWHY: capture RBG0 like the Vita GPU path (the CPU then does not draw it) and print the reason
			// the capture refused, whenever it changes
			static int on = -1, last = -1;
			extern int rstv_capture_mode, rstv_rot_why;
			if (on < 0) on = getenv("RSTV_ROTWHY") != NULL;
			if (on) {
				rstv_capture_mode = 1;
				extern int rstv_rot_dbg[4];
				if (rstv_rot_why != last || (rstv_rot_why & 0x100))
					printf("rotwhy f%d %x dkax %d dkast %d mode %d kast %d KTCTL %04x LNCLEN %04x RPMD %04x\n", f, rstv_rot_why, rstv_rot_dbg[0], rstv_rot_dbg[1],
						rstv_rot_dbg[2], rstv_rot_dbg[3], Vdp2Regs->KTCTL, Vdp2Regs->LNCLEN, Vdp2Regs->RPMD);
				last = rstv_rot_why;
			}
		}
		{
			// RSTV_ROTLOG: frame ranges where RBG0 is drawn without coefficients and without rotation (GPU reason 0x400)
			static int on = -1, in;
			static unsigned long long prev;
			extern unsigned long long rstv_rot_stats[16];
			if (on < 0) on = getenv("RSTV_ROTLOG") != NULL;
			if (on) {
				const int now = rstv_rot_stats[1] != prev;
				if (now != in) printf("rotlog f%d %s\n", f, now ? "start" : "end");
				in = now;
				prev = rstv_rot_stats[1];
			}
		}
		{
			// RSTV_DUMPRAM="F1,F2,..." : after these frames, high work RAM (1MB, as the SH-2 sees it) -> <out>/ram_<F>.bin
			static char dl[512] = { 1 };
			if (dl[0] == 1) { const char *e = getenv("RSTV_DUMPRAM"); snprintf(dl, sizeof(dl), ",%s,", e ? e : ""); }
			char t2[32];
			snprintf(t2, sizeof(t2), ",%d,", f);
			if (strstr(dl, t2)) {
				char path[1024];
				snprintf(path, sizeof(path), "%s/ram_%d.bin", out ? out : ".", f);
				FILE *fp = fopen(path, "wb");
				if (fp) {
					for (u32 a = 0; a < 0x100000; a++)
						fputc(MappedMemoryReadByteNocache(0x06000000 + a), fp);
					fclose(fp);
				}
			}
		}
		if (vdp2stats && f > 1300)
			vdp2_stats_frame();

		char tag[32];
		snprintf(tag, sizeof(tag), ",%d,", f);
		if (shots[0] && strstr(shots, tag)) {
			char path[1024];
			snprintf(path, sizeof(path), "%s/%s_%05d.ppm", out, game, f);
			save_ppm(path);
			// VDP1 front framebuffer (16-bit: RGB555 if MSB set, else low 8 bits as gray)
			{
				extern void rstv_vidsoft_vdp1_info(int *, int *, int *, u8 **);
				int vw, vh, vps; u8 *fb;
				rstv_vidsoft_vdp1_info(&vw, &vh, &vps, &fb);
				snprintf(path, sizeof(path), "%s/%s_%05d_vdp1.ppm", out, game, f);
				FILE *vf = fopen(path, "wb");
				if (vf && fb) {
					fprintf(vf, "P6\n512 256\n255\n");
					for (int i = 0; i < 512 * 256; i++) {
						u16 p = (u16)((fb[i * 2] << 8) | fb[i * 2 + 1]);
						uint8_t rgb[3];
						if (p & 0x8000) { rgb[0] = (p & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = ((p >> 10) & 31) << 3; }
						else { rgb[0] = rgb[1] = rgb[2] = (uint8_t)(p & 0xFF); }
						fwrite(rgb, 1, 3, vf);
					}
				}
				if (vf) fclose(vf);
				printf("vdp1 fb %dx%d ps %d TVMR %04x FBCR %04x PTMR %04x EW %04x %04x %04x local %d,%d | VDP2 SPCTL %04x BGON %04x PRISA %04x TVMD %04x\n", vw, vh, vps,
					Vdp1Regs->TVMR, Vdp1Regs->FBCR, Vdp1Regs->PTMR, Vdp1Regs->EWDR, Vdp1Regs->EWLR, Vdp1Regs->EWRR, (int)Vdp1Regs->localX, (int)Vdp1Regs->localY, Vdp2Regs->SPCTL, Vdp2Regs->BGON, Vdp2Regs->PRISA, Vdp2Regs->TVMD);
			}
		}
		{
			// RSTV_TVMDLOG: VDP2 TVMD changes and VDP1 command y range (local + vertices), for frame alignment with MAME
			static int tl = -1, lastv = -1;
			if (tl < 0) tl = getenv("RSTV_TVMDLOG") != NULL;
			if (tl) {
				if (Vdp2Regs->TVMD != lastv) { printf("tvmd f%d %04x\n", f, Vdp2Regs->TVMD); lastv = Vdp2Regs->TVMD; }
				if (f == 1600) {
					printf("cram f%d CRAOFA %04x CRAOFB %04x RAMCTL %04x:", f, Vdp2Regs->CRAOFA, Vdp2Regs->CRAOFB, Vdp2Regs->RAMCTL);
					for (int k = 0x60; k < 0x68; k++)
						printf(" %04x", T2ReadWord(Vdp2ColorRam, k * 2));
					printf("\n");
					printf("pri f%d PRINA %04x PRINB %04x PRIR %04x SFPRMD %04x SFSEL %04x SFCODE %04x BGON %04x CHCTLA %04x CHCTLB %04x PNCN0 %04x PNCN1 %04x PNCN2 %04x PNCN3 %04x PNCR %04x WCTLA %04x WCTLB %04x WCTLC %04x WCTLD %04x\n",
						f, Vdp2Regs->PRINA, Vdp2Regs->PRINB, Vdp2Regs->PRIR, Vdp2Regs->SFPRMD, Vdp2Regs->SFSEL, Vdp2Regs->SFCODE,
						Vdp2Regs->BGON, Vdp2Regs->CHCTLA, Vdp2Regs->CHCTLB, Vdp2Regs->PNCN0, Vdp2Regs->PNCN1, Vdp2Regs->PNCN2,
						Vdp2Regs->PNCN3, Vdp2Regs->PNCR, Vdp2Regs->WCTLA, Vdp2Regs->WCTLB, Vdp2Regs->WCTLC, Vdp2Regs->WCTLD);
				}
				if (f % 50 == 0)
					printf("clip f%d user %d,%d-%d,%d sys %d,%d-%d,%d\n", f, Vdp1Regs->userclipX1, Vdp1Regs->userclipY1, Vdp1Regs->userclipX2,
						Vdp1Regs->userclipY2, Vdp1Regs->systemclipX1, Vdp1Regs->systemclipY1, Vdp1Regs->systemclipX2, Vdp1Regs->systemclipY2);
				if (f % 100 == 0)
					printf("cc f%d CCCTL %04x SPCTL %04x CCRSA %04x CCRSB %04x CCRSC %04x CCRSD %04x SFCCMD %04x CCRLB %04x PRISA %04x PRISB %04x\n",
						f, Vdp2Regs->CCCTL, Vdp2Regs->SPCTL, Vdp2Regs->CCRSA, Vdp2Regs->CCRSB, Vdp2Regs->CCRSC, Vdp2Regs->CCRSD,
						Vdp2Regs->SFCCMD, Vdp2Regs->CCRLB, Vdp2Regs->PRISA, Vdp2Regs->PRISB);
				int a = 0, n = 0, ymin = 9999, ymax = -9999, ly = 0;
				for (int i = 0; i < 2000; i++) {
					const u16 ctrl = T1ReadWord(Vdp1Ram, a);
					if (ctrl & 0x8000) break;
					const int cm = ctrl & 15;
					if (!(ctrl & 0x4000)) {
						if (cm == 0xA) ly = (s16)T1ReadWord(Vdp1Ram, a + 0xE);
						if (cm <= 6 && cm != 1 && cm != 3) {
							for (int k = 0; k < 4; k++) { const int y = (s16)T1ReadWord(Vdp1Ram, a + 0xE + k * 4) + ly; if (y < ymin) ymin = y; if (y > ymax) ymax = y; }
							n++;
						}
					}
					a = (((ctrl >> 12) & 3) == 1) ? T1ReadWord(Vdp1Ram, a + 2) * 8 : a + 32;
					a &= 0x7FFFF;
				}
				if (f % 10 == 0) printf("v1y f%d cmds %d y %d..%d\n", f, n, ymin, ymax);
				if (getenv("RSTV_TEXH") && f == atoi(getenv("RSTV_CMDDUMP") ? getenv("RSTV_CMDDUMP") : "0")) {
					// diag: hash of VDP1 RAM ranges listed in the file ("hexaddr bytes" lines), same hash as scratchpad mame/texh.lua
					FILE *tf = fopen(getenv("RSTV_TEXH"), "r");
					unsigned ad, nb;
					while (tf && fscanf(tf, "%x %u", &ad, &nb) == 2) {
						unsigned long long h = 0;
						for (unsigned i = 0; i < nb; i++)
							h = (h * 31 + T1ReadByte(Vdp1Ram, (ad + i) & 0x7FFFF)) % 4294967296ULL;
						printf("texh %05x %u %08llx\n", ad, nb, h);
					}
					if (tf) fclose(tf);
				}
				if (getenv("RSTV_CMDDUMP") && f == atoi(getenv("RSTV_CMDDUMP"))) {
					int b = 0;
					for (int i = 0; i < 2000; i++) {
						const u16 ctrl = T1ReadWord(Vdp1Ram, b);
						if (ctrl & 0x8000) break;
						if (!(ctrl & 0x4000))
							printf("cmd f%d i%d ctrl %04x pmod %04x colr %04x srca %05x size %04x xy %d,%d %d,%d %d,%d %d,%d\n", f, i, ctrl,
								T1ReadWord(Vdp1Ram, b + 4), T1ReadWord(Vdp1Ram, b + 6), T1ReadWord(Vdp1Ram, b + 8) * 8, T1ReadWord(Vdp1Ram, b + 10),
								(s16)T1ReadWord(Vdp1Ram, b + 12), (s16)T1ReadWord(Vdp1Ram, b + 14), (s16)T1ReadWord(Vdp1Ram, b + 16),
								(s16)T1ReadWord(Vdp1Ram, b + 18), (s16)T1ReadWord(Vdp1Ram, b + 20), (s16)T1ReadWord(Vdp1Ram, b + 22),
								(s16)T1ReadWord(Vdp1Ram, b + 24), (s16)T1ReadWord(Vdp1Ram, b + 26));
						b = (((ctrl >> 12) & 3) == 1) ? T1ReadWord(Vdp1Ram, b + 2) * 8 : b + 32;
						b &= 0x7FFFF;
					}
				}
			}
			// RSTV_STAMPCHK: VDP1 RAM pages whose bytes changed this frame without a write stamp (the Vita GPU path
			// keeps decoded textures until a stamp says the page changed)
			static int on = -1;
			static u8 *shadow;
			static u32 seen[128];
			if (on < 0) on = getenv("RSTV_STAMPCHK") != NULL;
			if (on) {
				extern u32 rstv_vdp1_page_stamp[128];
				if (!shadow) { shadow = (u8 *)malloc(0x80000); memcpy(shadow, Vdp1Ram, 0x80000); memcpy(seen, rstv_vdp1_page_stamp, sizeof(seen)); }
				for (int p = 0; p < 128; p++) {
					if (memcmp(shadow + p * 4096, Vdp1Ram + p * 4096, 4096) && rstv_vdp1_page_stamp[p] == seen[p])
						printf("stampchk f%d page %d (0x%05x) changed without a stamp\n", f, p, p * 4096);
					memcpy(shadow + p * 4096, Vdp1Ram + p * 4096, 4096);
					seen[p] = rstv_vdp1_page_stamp[p];
				}
			}
		}
		if (checkpoint > 0 && f % checkpoint == 0) {
			double t = now_s();
			printf("frame %5d drawn %5u %dx%d video %016llx audio %016llx samples %llu  %.1f fps  mpc %08x spc %08x\n", f,
				s_frames_drawn, s_w, s_h, (unsigned long long)s_video_hash, (unsigned long long)s_audio_hash,
				(unsigned long long)s_audio_samples, checkpoint / (t - tc), SH2Core->GetPC(MSH2), SH2Core->GetPC(SSH2)); // (the core's own PC: the recompiler keeps it outside regs)
#if defined(RSTV_DYNAREC)
			{ // dynarec: cycles really run per frame since the last checkpoint (idle skips excluded), master / slave
				extern unsigned rstv_dyn_cyc[2];
				static unsigned last[2];
				printf("  dyncyc M %uk S %uk\n", (rstv_dyn_cyc[0] - last[0]) / checkpoint / 1000, (rstv_dyn_cyc[1] - last[1]) / checkpoint / 1000);
				last[0] = rstv_dyn_cyc[0]; last[1] = rstv_dyn_cyc[1];
			}
#endif
			if (getenv("RSTV_PEEK")) { // diag: RSTV_PEEK=addr,addr,... byte values (each checkpoint)
				char tmp[256];
				snprintf(tmp, sizeof(tmp), "%s", getenv("RSTV_PEEK"));
				printf("  peek");
				for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ","))
					printf(" %s=%02x", t, MappedMemoryReadByteNocache((u32)strtoul(t, NULL, 16)));
				printf(" | spc %08x ftcsr %02x%c", (unsigned)SSH2->regs.PC, (unsigned)SSH2->onchip.FTCSR, 10);
			}
			if (getenv("RSTV_SCUI")) { // diag: SCU interrupt mask/status, master SR/VBR, pending interrupt count
				printf("  scu ims %08x ist %08x | msh2 sr %08x vbr %08x nint %d | ssh2 sr %08x%c", (unsigned)ScuRegs->IMS, (unsigned)ScuRegs->IST,
					(unsigned)MSH2->regs.SR.all, (unsigned)MSH2->regs.VBR, (int)MSH2->NumberOfInterrupts, (unsigned)SSH2->regs.SR.all, 32); printf("  int0 lvl %u vec %u  isSleeping %d%c", (unsigned)MSH2->interrupts[0].level, (unsigned)MSH2->interrupts[0].vector, (int)MSH2->isSleeping, 10);
			}
			if (getenv("RSTV_68K")) { // diag: sound CPU PC, SH-2<->68K mailbox (sound RAM 0x700)
				extern u8 *SoundRam;
				printf("  68k pc %08x  vec %02x%02x%02x%02x %02x%02x%02x%02x  snd700 %02x%02x %02x%02x %02x%02x %02x%02x%c", M68K->GetPC(), SoundRam[0], SoundRam[1], SoundRam[2], SoundRam[3], SoundRam[4], SoundRam[5], SoundRam[6], SoundRam[7], SoundRam[0x700], SoundRam[0x701],
					SoundRam[0x702], SoundRam[0x703], SoundRam[0x704], SoundRam[0x705], SoundRam[0x706], SoundRam[0x707], 10);
			}
			if (getenv("RSTV_FRC")) // diag: master/slave FRT (FRC value, TCR divider)
				printf("  frc m %04x tcr %02x | s %04x tcr %02x\n", MSH2->onchip.FRC.all, MSH2->onchip.TCR, SSH2->onchip.FRC.all, SSH2->onchip.TCR);
			if (getenv("RSTV_DMACLOG")) { // diag: on-chip DMA units since the last checkpoint per region pair (type, last addresses)
				extern unsigned rstv_dmac_hist[64][3];
				for (int k = 0; k < 64; k++)
					if (rstv_dmac_hist[k][0])
						printf("  dmac %02x: %u units/frame type %u last sar %08x dar %07x%c", k, rstv_dmac_hist[k][0] / checkpoint,
							rstv_dmac_hist[k][2] >> 28, rstv_dmac_hist[k][1], rstv_dmac_hist[k][2] & 0x0FFFFFFF, 10);
				memset(rstv_dmac_hist, 0, sizeof(rstv_dmac_hist));
			}
			if (getenv("RSTV_DMAREG")) { // diag: master/slave on-chip DMA channels 0/1
				for (int c = 0; c < 2; c++) {
					SH2_struct *sh = c ? SSH2 : MSH2;
					const Onchip_struct *o = &sh->onchip;
					printf("  dma %s ch0 sar %08x dar %08x tcr %08x chcr %08x | ch1 sar %08x dar %08x tcr %08x chcr %08x | dmaor %08x clk %d %d\n", c ? "S" : "M",
						o->SAR0, o->DAR0, o->TCR0, o->CHCR0, o->SAR1, o->DAR1, o->TCR1, o->CHCR1, o->DMAOR,
						sh->dma_ch0.copy_clock, sh->dma_ch1.copy_clock);
				}
			}
			printf("   prof ms/frame: msh2 %.2f ssh2 %.2f vdp %.2f sound %.2f other %.2f\n",
				rstv_prof_us[RSTV_PROF_MSH2] / 1000.0 / checkpoint, rstv_prof_us[RSTV_PROF_SSH2] / 1000.0 / checkpoint,
				rstv_prof_us[RSTV_PROF_VDP] / 1000.0 / checkpoint, rstv_prof_us[RSTV_PROF_SOUND] / 1000.0 / checkpoint,
				rstv_prof_us[RSTV_PROF_OTHER] / 1000.0 / checkpoint);
			printf("   vdp detail: vdp1 %.2f vdp2 layers %.2f compose %.2f\n", rstv_prof_us[RSTV_PROF_VDP1] / 1000.0 / checkpoint,
				rstv_prof_us[RSTV_PROF_VDP2L] / 1000.0 / checkpoint, rstv_prof_us[RSTV_PROF_COMPOSE] / 1000.0 / checkpoint);
			printf("   layers: spr %.2f n0 %.2f n1 %.2f n2 %.2f n3 %.2f r0 %.2f\n",
				rstv_prof_us[RSTV_PROF_L_SPR] / 1000.0 / checkpoint, rstv_prof_us[RSTV_PROF_L_N0] / 1000.0 / checkpoint,
				rstv_prof_us[RSTV_PROF_L_N1] / 1000.0 / checkpoint, rstv_prof_us[RSTV_PROF_L_N2] / 1000.0 / checkpoint,
				rstv_prof_us[RSTV_PROF_L_N3] / 1000.0 / checkpoint, rstv_prof_us[RSTV_PROF_L_R0] / 1000.0 / checkpoint);
			{
				extern u64 rstv_sh2_insn[2], rstv_sh2_idle[2];
				printf("   sh2 insn/frame: m %llu s %llu  idle slices/frame: m %llu s %llu\n",
					(unsigned long long)(rstv_sh2_insn[0] / checkpoint), (unsigned long long)(rstv_sh2_insn[1] / checkpoint),
					(unsigned long long)(rstv_sh2_idle[0] / checkpoint), (unsigned long long)(rstv_sh2_idle[1] / checkpoint));
				memset(rstv_sh2_insn, 0, sizeof(rstv_sh2_insn));
				memset(rstv_sh2_idle, 0, sizeof(rstv_sh2_idle));
			}
			memset(rstv_prof_us, 0, sizeof(rstv_prof_us));
			fflush(stdout);
			tc = t;
		}
	}
	for (unsigned k = 0; k < dis_n; k++) {
		char buf[128];
		u32 a = dis_addr + k * 2;
		u16 op = MappedMemoryReadWordNocache(a);
		SH2Disasm(a, op, 0, NULL, buf);
		printf("%08x: %04x  %s\n", a, op, buf);
	}
	if (dis_n) {
		sh2regs_struct r = MSH2->regs;
		printf("R: ");
		for (int k = 0; k < 16; k++) printf("r%d=%08x ", k, r.R[k]);
		printf("\nSR=%08x GBR=%08x PR=%08x\n", r.SR.all, r.GBR, r.PR);
		printf("SMPC COMREG=%02x SF=%02x IREG0..2=%02x %02x %02x OREG31=%02x timing=%d intback=%d\n", SmpcRegs->COMREG,
			SmpcRegs->SF, SmpcRegs->IREG[0], SmpcRegs->IREG[1], SmpcRegs->IREG[2], SmpcRegs->OREG[31],
			SmpcInternalVars->timing, SmpcInternalVars->intback);
	}
	for (int i = 0; i < s_combos; i++)
		printf("%6d  %s\n", s_combo_n[i], s_combo[i]);
	if (gpucheck)
		nbgcheck_report();
	if (pchist_cpu) {
		// top 40 PCs by execution count (contiguous ranges grouped as loops for readability)
		{
			unsigned long long tot = 0, lo = 0, distinct = 0;
			for (u32 k = 0; k < 0x80000; k++) {
				distinct += rstv_pc_hist[k] != 0;
				tot += rstv_pc_hist[k];
				if (k * 2 + 0x06000000 >= 0x06070000 && k * 2 + 0x06000000 < 0x06071000) lo += rstv_pc_hist[k];
			}
			printf("pc distinct %llu, total %.0f/frame (06070xxx %.0f/frame)\n", distinct, (double)tot / (frames - pchist_from + 1), (double)lo / (frames - pchist_from + 1));
			(void)0;
		}
		if (getenv("RSTV_OPHIST")) {
			// executed instructions by shape (disassembly with register numbers and immediates removed)
			enum { NSH = 512 };
			static char shape[NSH][48];
			static unsigned long long shn[NSH];
			int ns = 0;
			unsigned long long tot = 0;
			for (u32 k = 0; k < 0x80000; k++) {
				if (!rstv_pc_hist[k])
					continue;
				char str[128], s[48];
				const u32 a = 0x06000000 + k * 2;
				SH2Disasm(a, MappedMemoryReadWord(a, NULL), 0, NULL, str);
				const char *p = strstr(str, ": ");
				p = p ? p + 2 : str;
				int n = 0;
				for (; *p && *p != ';' && n < 47; p++) {
					if ((p[0] == 'r' && p[1] >= '0' && p[1] <= '9')) { s[n++] = 'R'; while (p[1] >= '0' && p[1] <= '9') p++; }
					else if (p[0] == '0' && p[1] == 'x') { s[n++] = '#'; p++; while (isxdigit((unsigned char)p[1])) p++; }
					else if (*p != ' ' || (n && s[n - 1] != ' ')) s[n++] = *p;
				}
				while (n && s[n - 1] == ' ') n--;
				s[n] = 0;
				int j = 0;
				while (j < ns && strcmp(shape[j], s)) j++;
				if (j == ns && ns < NSH) strcpy(shape[ns++], s);
				if (j < NSH) shn[j] += rstv_pc_hist[k];
				tot += rstv_pc_hist[k];
			}
			for (int t = 0; t < 70; t++) {
				int b = -1;
				for (int j = 0; j < ns; j++)
					if (shn[j] && (b < 0 || shn[j] > shn[b])) b = j;
				if (b < 0) break;
				printf("op %5.1f%%  %s\n", 100.0 * shn[b] / tot, shape[b]);
				shn[b] = 0;
			}
		}
		for (int t = 0; t < (getenv("RSTV_PCN") ? atoi(getenv("RSTV_PCN")) : 40); t++) {
			u32 best = 0, bi = 0;
			for (u32 k = 0; k < 0x80000; k++)
				if (rstv_pc_hist[k] > best) { best = rstv_pc_hist[k]; bi = k; }
			if (!best) break;
			printf("pc %08x  %10u  (%.1f/frame)\n", 0x06000000 + bi * 2, best, (double)best / (frames - pchist_from + 1));
			rstv_pc_hist[bi] = 0;
		}
	}
	if (getenv("RSTV_FIND")) {
		// RSTV_FIND=w1,w2,... : addresses in high work RAM where these 16-bit words follow each other (last state)
		u16 w[64];
		int nw = 0;
		const char *e = getenv("RSTV_FIND");
		while (*e && nw < 64) {
			w[nw++] = (u16)strtoul(e, (char **)&e, 16);
			if (*e == ',') e++;
		}
		for (u32 a = 0x06000000; a + nw * 2 <= 0x06100000; a += 2) {
			int k = 0;
			while (k < nw && MappedMemoryReadWord(a + k * 2, NULL) == w[k]) k++;
			if (k == nw)
				printf("find %08x\n", a);
		}
	}
	if (getenv("RSTV_DIS")) {
		// RSTV_DIS=addr,count : disassemble SH-2 code in final state (for finding idle loops)
		unsigned addr = 0, cnt = 16;
		sscanf(getenv("RSTV_DIS"), "%x,%u", &addr, &cnt);
		for (unsigned k = 0; k < cnt; k++, addr += 2) {
			char str[128];
			const u16 op = MappedMemoryReadWord(addr, NULL);
			SH2Disasm(addr, op, 0, NULL, str);
			printf("dis %08x %04x  %s\n", addr, op, str);
		}
	}
	extern void rstv_vdp1_stats_print(void);
	rstv_vdp1_stats_print();
	{
		extern unsigned long long rstv_rot_stats[16];
		static const char *nm[15] = { "rot frames", "nocoef-unrotated", "nocoef-rotated", "coef", "coef-per-dot", "coefmode0",
			"coefmode1", "coefmode2", "coefmode3", "paramAB", "bitmap", "screenover2+", "window", "color>256", "linescreen" };
		if (rstv_rot_stats[0])
			for (int i = 0; i < 15; i++)
				printf("rbg %-18s %llu\n", nm[i], rstv_rot_stats[i]);
	}
	{ extern unsigned rstv_dsp_slices; printf("scu dsp slices %u\n", rstv_dsp_slices); }
	{ extern unsigned rstv_dsp_insns; printf("scu dsp insns %u\n", rstv_dsp_insns); }
#ifdef RSTV_DYNAREC
	{ extern unsigned rstv_dyn_div32_n, rstv_dyn_cont_n, rstv_dyn_divfx_n, rstv_dyn_litc_n; printf("dyn div32 %u cont %u divfx %u litc %u\n", rstv_dyn_div32_n, rstv_dyn_cont_n, rstv_dyn_divfx_n, rstv_dyn_litc_n); }
	{ extern unsigned long long rstv_icount[2]; if (rstv_icount[1]) printf("icount arm %llu sh2 %llu (%.2f per sh2 instruction)\n", rstv_icount[0], rstv_icount[1], (double)rstv_icount[0] / rstv_icount[1]); }
	{ extern unsigned rstv_dyn_rc_n, rstv_dyn_rc_miss[16]; printf("rc native %u miss", rstv_dyn_rc_n); for (int k = 0; k < 16; k++) printf(" %x:%u", k, rstv_dyn_rc_miss[k]); printf("\n"); }
#endif
	printf("sh2 core id %d\n", SH2Core ? SH2Core->id : -1);
	{ extern unsigned rstv_icap_count[2]; printf("input capture writes: master %u slave %u\n", rstv_icap_count[0], rstv_icap_count[1]); }
	{ extern unsigned rstv_dspjit_builds, rstv_dspjit_fallback; extern int rstv_dspjit_on;
	  printf("dsp jit on %d builds %u fallback %u\n", rstv_dspjit_on, rstv_dspjit_builds, rstv_dspjit_fallback); }
	if (getenv("RSTV_DSPDUMP")) { // SCSP DSP program at the end (step: 64-bit word)
		extern ScspDsp scsp_dsp;
		int nz = 0;
		for (int i = 0; i < 128; i++)
			if (scsp_dsp.mpro[i]) {
				nz++;
				printf("dsp %3d %016llx\n", i, (unsigned long long)scsp_dsp.mpro[i]);
			}
		printf("dsp last_step %d nonzero %d\n", scsp_dsp.last_step, nz);
	}
	double dt = now_s() - t0;
	printf("done %d frames in %.2fs (%.1f fps)\n", frames, dt, frames / dt);
	YabauseDeInit();
	return 0;
}
