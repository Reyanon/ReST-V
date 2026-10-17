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
static void snd_update(u32 *l, u32 *r, u32 n)
{
	while (n > 0) {
		u32 k = n > 4096 ? 4096 : n;
		for (u32 i = 0; i < k; i++) {
			int32_t a = (int32_t)l[i], b = (int32_t)r[i];
			s_snd_buf[i * 2] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
			s_snd_buf[i * 2 + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
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
	StvIoSetInputs(p1, p2, system);
	YabauseExec();
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
