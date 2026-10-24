// Core glue (Vita). Starts the core with the same settings as the PC harness tools/harness/main.c.
#include "frontend/emu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cdbase.h"
#include "cs0.h"
#include "m68kcore.h"
#include "peripheral.h"
#include "scsp.h"
#include "sh2core.h"
#include "sh2int.h"
#include "stvio.h"
#include "vidsoft.h"
#include "yabause.h"
#include "yui.h"

#include "frontend/audio.h"
#include "frontend/cheats.h"
#include "platform/log.h"
#include "video/video.h"

// ---- core list ----
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

// ---- sound: SCSP output (44.1kHz, 32-bit) → INT16 → audio ring buffer ----
#define SNDCORE_VITA 21
static int16_t s_snd_buf[4096 * 2];

static int snd_init(void) { return 0; }
static void snd_deinit(void) {}
static int snd_reset(void) { return 0; }
static int snd_vf(int f) { (void)f; return 0; }
// ADR-192 (owner): the output was about half of MAME's (6 dB lower in dnmtdeka, diehard, vfkids). Doubled, with a soft
// limit above 75% of full scale instead of clipping (loud sets such as thuntk already peak near full scale): past the
// knee the curve bends smoothly towards full scale (same slope at the knee, never reaches it)
static inline int16_t snd_out(int32_t v)
{
	enum { KNEE = 24576, ROOM = 32767 - KNEE };
	v *= 2;
	const int32_t m = v < 0 ? -v : v;
	if (m <= KNEE)
		return (int16_t)v;
	const int32_t e = m - KNEE, y = KNEE + (int32_t)((int64_t)e * ROOM / (e + ROOM));
	return (int16_t)(v < 0 ? -y : y);
}
static void snd_update(u32 *l, u32 *r, u32 n)
{
	while (n > 0) {
		u32 k = n > 4096 ? 4096 : n;
		for (u32 i = 0; i < k; i++) {
			s_snd_buf[i * 2] = snd_out((int32_t)l[i]);
			s_snd_buf[i * 2 + 1] = snd_out((int32_t)r[i]);
		}
		rstv_audio_push(s_snd_buf, (int)k);
		l += k;
		r += k;
		n -= k;
	}
}
static u32 snd_space(void) { return 0x10000; }
static void snd_nop(void) {}
static void snd_vol(int v) { (void)v; }
static SoundInterface_struct SNDVita = { SNDCORE_VITA, "Vita", snd_init, snd_deinit, snd_reset, snd_vf,
	snd_update, snd_space, snd_nop, snd_nop, snd_vol };
SoundInterface_struct *SNDCoreList[] = { &SNDVita, &SNDDummy, NULL };

// ---- Yui callbacks ----
static int s_w = 320, s_h = 224;

void YuiErrorMsg(const char *string) { RSTV_LOG("core error: %s", string); }
void YuiMsg(const char *format, ...)
{
#ifdef RSTV_ENABLE_LOG
	char buf[256];
	va_list ap;
	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	RSTV_LOG("core: %s", buf);
#else
	(void)format;
#endif
}
int YuiUseOGLOnThisThread(void) { return 0; }
int YuiRevokeOGLOnThisThread(void) { return 0; }
int YuiGetFB(void) { return 0; }

// called when the soft renderer finishes a frame → copy to texture
void YuiSwapBuffers(void)
{
	int interlace = 0;
	VIDCore->GetNativeResolution(&s_w, &s_h, &interlace);
	if (!dispbuffer || s_w <= 0 || s_h <= 0)
		return;
	if (s_w > RSTV_VIDEO_TEX_W) s_w = RSTV_VIDEO_TEX_W;
	if (s_h > RSTV_VIDEO_TEX_H) s_h = RSTV_VIDEO_TEX_H;
	uint32_t *dst = rstv_video_frame_begin(s_w, s_h);
	const pixel_t *src = dispbuffer;
	for (int y = 0; y < s_h; y++)
		memcpy(dst + y * RSTV_VIDEO_PITCH, src + y * s_w, s_w * sizeof(uint32_t));
	rstv_video_frame_end();
}

int rstv_emu_init(const char *rom_dir, const char *romset, int use_dynarec, char *err, int errlen)
{
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
	yinit.sndcoretype = SNDCORE_VITA;
	yinit.m68kcoretype = M68KCORE_MUSASHI;
	yinit.cdcoretype = CDCORE_DUMMY;
	yinit.carttype = CART_NONE;
	yinit.buppath = "ux0:data/restv/bup.bin";
	yinit.videoformattype = VIDEOFORMATTYPE_NTSC;
	yinit.clocksync = 1;
	yinit.use_new_scsp = 1;
	yinit.scsp_sync_count_per_frame = 1;
	yinit.stv_rom_dir = rom_dir;
	yinit.stv_romset = romset;
	int r = YabauseInit(&yinit);
	if (r != 0) {
		snprintf(err, errlen, "YabauseInit %d (%s)", r, romset);
		return -1;
	}
	return 0;
}

void rstv_emu_shutdown(void)
{
	// ADR-103: the sound and VDP1 threads finish their work before the core is freed
	extern void rstv_sound_quiesce(void);
	extern void rstv_vdp1_quiesce(void);
	rstv_sound_quiesce();
	rstv_vdp1_quiesce();
	YabauseDeInit();
}

void rstv_emu_run_frame(uint8_t p1, uint8_t p2, uint8_t system)
{
	extern void rstv_v1watch_frame(void);
	StvIoSetInputs(p1, p2, system);
	YabauseExec();
	rstv_v1watch_frame();
	rstv_cheats_frame();
	if ((yabsys.frame_count & 127) == 0)
		rstv_log_flush(); // diag file (logfile flag)
	// diag: ux0:data/restv/pclog = "from to" frames: master/slave PC and PR after each frame in that range
	static int pl_from = -2, pl_to;
	if (pl_from == -2) {
		FILE *pf = fopen("ux0:data/restv/pclog", "r");
		pl_from = -1;
		if (pf) {
			if (fscanf(pf, "%d %d", &pl_from, &pl_to) != 2)
				pl_from = -1;
			fclose(pf);
		}
	}
	if (pl_from >= 0 && (int)yabsys.frame_count >= pl_from && (int)yabsys.frame_count <= pl_to)
		RSTV_LOG("pclog f%u m %08x pr %08x sr %03x s %08x", (unsigned)yabsys.frame_count, (unsigned)SH2Core->GetPC(MSH2),
			(unsigned)SH2Core->GetPR(MSH2), (unsigned)SH2Core->GetSR(MSH2), (unsigned)SH2Core->GetPC(SSH2));
}

void rstv_emu_reset(void)
{
	extern void rstv_sound_quiesce(void);
	extern void rstv_vdp1_quiesce(void);
	rstv_sound_quiesce();
	rstv_vdp1_quiesce();
	YabauseResetNoLoad();
}

void rstv_emu_screen_size(int *w, int *h)
{
	*w = s_w;
	*h = s_h;
}
