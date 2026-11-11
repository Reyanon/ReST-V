// The game list's preview (ADR-239, from ReBOX vita/src/frontend/avsnap.c, same author, GPL-2.0-or-later):
// ux0:data/restv/snap/<set>.mp4 (AvPlayer, looped, with its sound), .png or .jpg, or the parent's. A thread of its own
// opens, closes and plays the sound, so the list never waits; the frames are copied to two buffers of ours, so the GPU
// never reads one the player is writing.
#include "frontend/avsnap.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include "platform/log.h"

#define FADE_FRAMES 40
#define FRAME_BLOCK 0x40000u   // (a copied frame's memory: 320 x 240 and more)

static SceUID s_thread = -1, s_sema = -1;
static volatile unsigned s_req;       // the newest request's number
static char s_req_game[32], s_req_parent[32];
static SceAvPlayerHandle s_player;    // (the thread's)
static int s_loaded;
static volatile int s_kind;           // RSTV_SNAP_*: what is shown now
static GxTex *volatile s_img;
static int s_img_w, s_img_h;          // (its picture: the texture is wider, to 8)
static GxTex s_tex;                   // the video's picture (a YUV texture on our copy of the frame)
static void *s_buf[2];
static int s_cur, s_have, s_fade_n;
static int s_port = -1, s_port_grain, s_port_rate, s_port_mode;

static void *mem_alloc(void *arg, uint32_t alignment, uint32_t size)
{
	(void)arg;
	return memalign(alignment < 64 ? 64 : alignment, size);
}
static void mem_free(void *arg, void *p)
{
	(void)arg;
	free(p);
}
// memory the GPU reads
static void *gpu_alloc(uint32_t size)
{
	size = (size + 0x3ffff) & ~0x3ffffu;
	const SceUID uid = sceKernelAllocMemBlock("rstv_avsnap", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
	void *p = NULL;
	if (uid < 0 || sceKernelGetMemBlockBase(uid, &p) < 0)
		return NULL;
	if (sceGxmMapMemory(p, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) {
		sceKernelFreeMemBlock(uid);
		return NULL;
	}
	return p;
}
static void *frame_alloc(void *arg, uint32_t alignment, uint32_t size)
{
	(void)arg;
	(void)alignment;
	return gpu_alloc(size);
}
static void frame_free(void *arg, void *p)
{
	(void)arg;
	const SceUID uid = sceKernelFindMemBlockByAddr(p, 0);
	if (uid >= 0) {
		sceGxmUnmapMemory(p);
		sceKernelFreeMemBlock(uid);
	}
}

static int open_video(const char *path)
{
	if (!s_loaded) {
		const int m = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
		if (m < 0) {
			RSTV_LOG("avsnap: AvPlayer module %08x", (unsigned)m);
			return 0;
		}
		s_loaded = 1;
	}
	SceAvPlayerInitData d;
	memset(&d, 0, sizeof(d));
	d.memoryReplacement.allocate = mem_alloc;
	d.memoryReplacement.deallocate = mem_free;
	d.memoryReplacement.allocateTexture = frame_alloc;
	d.memoryReplacement.deallocateTexture = frame_free;
	d.basePriority = 0xA0;
	d.numOutputVideoFrameBuffers = 2;
	d.autoStart = SCE_TRUE;
	s_player = sceAvPlayerInit(&d);
	if (!s_player || ((uint32_t)s_player >> 16) == 0x806A) {   // (the handle is an address; errors are 0x806Axxxx)
		RSTV_LOG("avsnap: init failed %08x", (unsigned)s_player);
		s_player = 0;
		return 0;
	}
	const int rc = sceAvPlayerAddSource(s_player, path);
	if (rc < 0) {
		RSTV_LOG("avsnap: %s: %08x", path, (unsigned)rc);
		sceAvPlayerClose(s_player);
		s_player = 0;
		return 0;
	}
	sceAvPlayerSetLooping(s_player, SCE_TRUE);
	RSTV_LOG("avsnap: %s opened", path);
	return 1;
}

static void close_all(void)
{
	s_kind = RSTV_SNAP_NONE;
	sceKernelDelayThread(40000);   // (the frames being drawn are done with what was shown)
	if (s_player) {
		sceAvPlayerStop(s_player);
		sceAvPlayerClose(s_player);
		s_player = 0;
	}
	if (s_img) {
		gx_tex_free(s_img); // (the frames that drew it are done: 40 ms above)
		s_img = NULL;
	}
	s_have = 0;
	s_fade_n = 0;
}

extern uint8_t *rstv_video_load_image(const char *path, int *w, int *h); // (png or jpg, RGBA; video_gxm.c)
static GxTex *load_image(const char *name)
{
	char path[160];
	int w = 0, h = 0;
	snprintf(path, sizeof(path), "ux0:data/restv/snap/%s.png", name);
	uint8_t *b = rstv_video_load_image(path, &w, &h);
	if (!b) {
		snprintf(path, sizeof(path), "ux0:data/restv/snap/%s.jpg", name);
		b = rstv_video_load_image(path, &w, &h);
	}
	if (!b)
		return NULL;
	GxTex *t = w <= 2048 && h <= 2048 ? gx_tex(w, h, GX_RGBA8, 1, 0) : NULL;
	s_img_w = w;
	s_img_h = h;
	if (t)
		for (int y = 0; y < h; y++)
			memcpy(t->data + (size_t)y * t->stride, b + (size_t)y * w * 4, (size_t)w * 4);
	free(b);
	return t;
}

static void open_game(const char *game, const char *parent)
{
	const char *names[2] = { game, parent };
	for (int k = 0; k < 2; k++) {   // (a video first, the set's or else its parent's)
		char path[160];
		SceIoStat st;
		if (!names[k][0])
			continue;
		snprintf(path, sizeof(path), "ux0:data/restv/snap/%s.mp4", names[k]);
		if (sceIoGetstat(path, &st) >= 0 && open_video(path)) {
			s_kind = RSTV_SNAP_VIDEO;
			return;
		}
	}
	for (int k = 0; k < 2; k++)
		if (names[k][0] && (s_img = load_image(names[k])) != NULL) {
			s_kind = RSTV_SNAP_IMAGE;
			return;
		}
}

// its sound at full volume, at its own speed: this is also what keeps the player's clock right
static void play_sound(void)
{
	SceAvPlayerFrameInfo f;
	memset(&f, 0, sizeof(f));
	if (!sceAvPlayerGetAudioData(s_player, &f) || !f.pData || !f.details.audio.sampleRate) {
		sceKernelDelayThread(4000);
		return;
	}
	const int ch = f.details.audio.channelCount, grain = (int)(f.details.audio.size / (2u * (ch ? ch : 1)));
	const int rate = (int)f.details.audio.sampleRate;
	const int mode = ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
	if (s_port < 0) {
		s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, rate, mode);
		s_port_grain = grain;
		s_port_rate = rate;
		s_port_mode = mode;
		if (s_port >= 0) {
			int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
			sceAudioOutSetVolume(s_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
		}
	} else if (grain != s_port_grain || rate != s_port_rate || mode != s_port_mode) {
		sceAudioOutSetConfig(s_port, grain, rate, mode);
		s_port_grain = grain;
		s_port_rate = rate;
		s_port_mode = mode;
	}
	if (s_port >= 0)
		sceAudioOutOutput(s_port, f.pData);
}

static int snap_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	unsigned done = 0;
	for (;;) {
		if (done == s_req && !s_player)
			sceKernelWaitSema(s_sema, 1, NULL);
		if (done != s_req) {
			done = s_req;
			char game[32], parent[32];
			memcpy(game, s_req_game, sizeof(game));
			memcpy(parent, s_req_parent, sizeof(parent));
			close_all();
			if (game[0] && done == s_req)
				open_game(game, parent);
			continue;
		}
		if (s_player)
			play_sound();
	}
	return 0;
}

void rstv_snap_request(const char *game, const char *parent)
{
	s_kind = RSTV_SNAP_NONE;   // (gone at once; the thread closes it)
	if (s_thread < 0) {
		s_sema = sceKernelCreateSema("rstv_avsnap", 0, 0, 1, NULL);
		s_thread = sceKernelCreateThread("rstv_avsnap", snap_thread, 0x60, 0x40000, 0, 0, NULL);
		if (s_thread < 0)
			return;
		sceKernelStartThread(s_thread, 0, NULL);
	}
	snprintf(s_req_game, sizeof(s_req_game), "%s", game ? game : "");
	snprintf(s_req_parent, sizeof(s_req_parent), "%s", parent ? parent : "");
	s_req++;
	sceKernelSignalSema(s_sema, 1);
}

int rstv_snap_kind(void) { return s_have ? s_kind : RSTV_SNAP_NONE; }
float rstv_snap_fade(void) { return (float)s_fade_n / FADE_FRAMES; }

const GxTex *rstv_snap_frame(float *u1, float *v1)
{
	const GxTex *t = NULL;
	*u1 = *v1 = 1.0f;
	if (s_kind == RSTV_SNAP_IMAGE) {
		t = s_img;
		s_have = t != NULL;
		if (t) {
			*u1 = (float)s_img_w / t->w;
			*v1 = (float)s_img_h / t->h;
		}
	} else if (s_kind == RSTV_SNAP_VIDEO && s_player) {
		SceAvPlayerFrameInfo f;
		memset(&f, 0, sizeof(f));
		if (sceAvPlayerIsActive(s_player) && sceAvPlayerGetVideoData(s_player, &f) && f.pData) {
			const uint32_t w = f.details.video.width, h = f.details.video.height, size = w * h * 3 / 2;
			if (!s_have)
				RSTV_LOG("avsnap: first frame %ux%u", (unsigned)w, (unsigned)h);
			if (size <= FRAME_BLOCK) {
				const int next = !s_cur;
				if (!s_buf[next])
					s_buf[next] = gpu_alloc(FRAME_BLOCK);
				if (s_buf[next]) {
					memcpy(s_buf[next], f.pData, size);
					sceGxmTextureInitLinear(&s_tex.tex, s_buf[next], SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, w, h, 0);
					sceGxmTextureSetMinFilter(&s_tex.tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
					sceGxmTextureSetMagFilter(&s_tex.tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
					s_tex.w = (int)w;
					s_tex.h = (int)h;
					s_cur = next;
					s_have = 1;
				}
			}
		}
		t = s_have ? &s_tex : NULL;
	}
	if (t && s_fade_n < FADE_FRAMES)
		s_fade_n++;
	return t;
}
