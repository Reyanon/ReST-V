// Direct sceGxm backend (ADR-015). Setup order follows vitaGL (gxm.c) and the SDK samples.
#include "video/gx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <vitashark.h>

#include "platform/log.h"

#define DISP_W 960
#define DISP_H 544
#define DISP_STRIDE 960
#define DISP_BUFS 3
#define RT_W 512
#define RT_H 256
#define TEMP_SEG (6 * 1024 * 1024) // per-frame temp memory (vertices, indices)
#define SHADER_DIR "ux0:data/restv/shaders"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

typedef struct {
	void *addr;
} DispData;

static SceGxmContext *s_ctx;
static SceGxmShaderPatcher *s_patcher;
static SceGxmRenderTarget *s_rt_disp, *s_rt_tex;
static SceGxmColorSurface s_disp_surf[DISP_BUFS];
static void *s_disp_addr[DISP_BUFS];
static SceGxmSyncObject *s_disp_sync[DISP_BUFS];
static int s_front, s_back;
static int s_vsync;
static volatile unsigned int *s_notif;
static unsigned int s_submitted;   // frames pushed to the display queue
static int s_frame_ready;          // already waited on this frame's fence?
static uint8_t *s_temp_base;
static int s_temp_off;
static int s_in_scene, s_scene_disp;
static void *s_vbuf, *s_fbuf;      // reserved default uniform buffers
static uint16_t *s_seq;
static int s_shark_ok;

// ---- memory ----

static void *gpu_alloc(SceKernelMemBlockType type, unsigned size, SceGxmMemoryAttribFlags attr, SceUID *out_uid)
{
	size = (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) ? ALIGN(size, 256 * 1024) : ALIGN(size, 4 * 1024);
	SceUID uid = sceKernelAllocMemBlock("restv_gpu", type, size, NULL);
	if (uid < 0)
		return NULL;
	void *mem = NULL;
	sceKernelGetMemBlockBase(uid, &mem);
	if (sceGxmMapMemory(mem, size, attr) < 0) {
		sceKernelFreeMemBlock(uid);
		return NULL;
	}
	if (out_uid)
		*out_uid = uid;
	return mem;
}

static void *usse_alloc(unsigned size, int fragment, unsigned *offset)
{
	size = ALIGN(size, 4 * 1024);
	SceUID uid = sceKernelAllocMemBlock("restv_usse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
	if (uid < 0)
		return NULL;
	void *mem = NULL;
	sceKernelGetMemBlockBase(uid, &mem);
	if (fragment)
		sceGxmMapFragmentUsseMemory(mem, size, offset);
	else
		sceGxmMapVertexUsseMemory(mem, size, offset);
	return mem;
}

static void *patcher_host_alloc(void *user, unsigned int size)
{
	(void)user;
	return malloc(size);
}

static void patcher_host_free(void *user, void *mem)
{
	(void)user;
	free(mem);
}

// ---- display ----

static void display_cb(const void *data)
{
	const DispData *d = (const DispData *)data;
	SceDisplayFrameBuf fb;
	memset(&fb, 0, sizeof(fb));
	fb.size = sizeof(fb);
	fb.base = d->addr;
	fb.pitch = DISP_STRIDE;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width = DISP_W;
	fb.height = DISP_H;
	sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
	if (s_vsync)
		sceDisplayWaitVblankStart();
}

static int make_rt(int w, int h, int scenes, SceGxmRenderTarget **rt)
{
	SceGxmRenderTargetParams p;
	memset(&p, 0, sizeof(p));
	p.width = (uint16_t)w;
	p.height = (uint16_t)h;
	p.scenesPerFrame = (uint16_t)scenes;
	p.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
	p.driverMemBlock = -1;
	return sceGxmCreateRenderTarget(&p, rt);
}

int gx_init(int vsync)
{
	s_vsync = vsync;
	SceGxmInitializeParams ip;
	memset(&ip, 0, sizeof(ip));
	ip.displayQueueMaxPendingCount = 2;
	ip.displayQueueCallback = display_cb;
	ip.displayQueueCallbackDataSize = sizeof(DispData);
	ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
	int r = sceGxmInitialize(&ip);
	if (r < 0) {
		RSTV_LOG("gx: sceGxmInitialize %08x", r);
		return -1;
	}

	// context
	SceGxmContextParams cp;
	memset(&cp, 0, sizeof(cp));
	cp.hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
	cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
	cp.vdmRingBufferMem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, NULL);
	cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
	cp.vertexRingBufferMem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, NULL);
	cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
	cp.fragmentRingBufferMem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, NULL);
	cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
	unsigned fu_off = 0;
	cp.fragmentUsseRingBufferMem = usse_alloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, 1, &fu_off);
	cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
	cp.fragmentUsseRingBufferOffset = fu_off;
	r = sceGxmCreateContext(&cp, &s_ctx);
	if (r < 0) {
		RSTV_LOG("gx: sceGxmCreateContext %08x", r);
		return -1;
	}

	// shader patcher
	// ADR-119: at 256KB each, fragment program registration failed with 805b0024 in every game (composite variants fell back to the generic shader)
	const unsigned pbuf = 1024 * 1024, pvu = 512 * 1024, pfu = 2048 * 1024;
	SceGxmShaderPatcherParams pp;
	memset(&pp, 0, sizeof(pp));
	pp.hostAllocCallback = patcher_host_alloc;
	pp.hostFreeCallback = patcher_host_free;
	pp.bufferMem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, pbuf, SCE_GXM_MEMORY_ATTRIB_READ, NULL);
	pp.bufferMemSize = pbuf;
	pp.vertexUsseMem = usse_alloc(pvu, 0, &pp.vertexUsseOffset);
	pp.vertexUsseMemSize = pvu;
	pp.fragmentUsseMem = usse_alloc(pfu, 1, &pp.fragmentUsseOffset);
	pp.fragmentUsseMemSize = pfu;
	r = sceGxmShaderPatcherCreate(&pp, &s_patcher);
	if (r < 0) {
		RSTV_LOG("gx: sceGxmShaderPatcherCreate %08x", r);
		return -1;
	}

	// display buffers
	if (make_rt(DISP_W, DISP_H, 1, &s_rt_disp) < 0 || make_rt(RT_W, RT_H, 8, &s_rt_tex) < 0) {
		RSTV_LOG("gx: render target failed");
		return -1;
	}
	for (int i = 0; i < DISP_BUFS; i++) {
		s_disp_addr[i] = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DISP_STRIDE * DISP_H * 4, SCE_GXM_MEMORY_ATTRIB_RW, NULL);
		if (!s_disp_addr[i])
			return -1;
		memset(s_disp_addr[i], 0, DISP_STRIDE * DISP_H * 4);
		sceGxmColorSurfaceInit(&s_disp_surf[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
			SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, DISP_W, DISP_H, DISP_STRIDE, s_disp_addr[i]);
		sceGxmSyncObjectCreate(&s_disp_sync[i]);
	}
	s_front = 0;
	s_back = 1;

	s_notif = sceGxmGetNotificationRegion();
	*s_notif = 0;
	s_temp_base = (uint8_t *)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, TEMP_SEG * DISP_BUFS, SCE_GXM_MEMORY_ATTRIB_READ, NULL);
	s_seq = (uint16_t *)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 65536 * 2, SCE_GXM_MEMORY_ATTRIB_READ, NULL);
	if (!s_temp_base || !s_seq)
		return -1;
	for (int i = 0; i < 65536; i++)
		s_seq[i] = (uint16_t)i;

	// shader compiler (libshacccg)
	shark_set_allocators(malloc, free); // without this, compile fails with "fatal internal error"
	s_shark_ok = shark_init(NULL) >= 0;
	if (!s_shark_ok)
		s_shark_ok = shark_init("ur0:data/external/libshacccg.suprx") >= 0;
	sceIoMkdir(SHADER_DIR, 0777);
	RSTV_LOG("gx: init ok (vsync %d, shader compiler %s)", vsync, s_shark_ok ? "ok" : "missing");
	return 0;
}

void gx_shutdown(void)
{
	if (s_ctx)
		sceGxmFinish(s_ctx);
}

// ---- textures ----

// put all textures and render targets in CDRAM (GPU bandwidth). If ux0:data/restv/nocdram exists, only those marked GX_CDRAM
static int s_cdram_all = -1;

GxTex *gx_tex(int w, int h, int fmt, int linear, int rt)
{
	GxTex *t = (GxTex *)calloc(1, sizeof(GxTex));
	if (s_cdram_all < 0) {
		SceUID fd = sceIoOpen("ux0:data/restv/nocdram", SCE_O_RDONLY, 0);
		s_cdram_all = fd < 0;
		if (fd >= 0)
			sceIoClose(fd);
		RSTV_LOG("gx: textures in %s", s_cdram_all ? "CDRAM" : "LPDDR (GX_CDRAM only)");
	}
	int cdram = (fmt & GX_CDRAM) != 0 || s_cdram_all;
	fmt &= ~GX_CDRAM;
	t->w = ALIGN(w, 8);
	t->h = h;
	t->bpp = fmt == GX_L8 ? 1 : 4;
	t->stride = t->w * t->bpp;
	t->rt = rt;
	t->data = NULL;
	if (cdram)
		t->data = (uint8_t *)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, t->stride * h, SCE_GXM_MEMORY_ATTRIB_RW, NULL);
	if (!t->data)
		t->data = (uint8_t *)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, t->stride * h,
			rt ? SCE_GXM_MEMORY_ATTRIB_RW : SCE_GXM_MEMORY_ATTRIB_READ, NULL);
	if (!t->data) {
		RSTV_LOG("gx: texture %dx%d alloc failed", w, h);
		free(t);
		return NULL;
	}
	memset(t->data, 0, t->stride * h);
	sceGxmTextureInitLinear(&t->tex, t->data, fmt == GX_L8 ? SCE_GXM_TEXTURE_FORMAT_R8 : SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
		t->w, h, 0);
	const SceGxmTextureFilter f = linear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
	sceGxmTextureSetMinFilter(&t->tex, f);
	sceGxmTextureSetMagFilter(&t->tex, f);
	sceGxmTextureSetUAddrMode(&t->tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
	sceGxmTextureSetVAddrMode(&t->tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (rt) {
		sceGxmColorSurfaceInit(&t->surf, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
			SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, t->w, h, t->w, t->data);
		// vitaGL also uses a dedicated single-scene render target for FBOs wider than 256. When shared, only the frame's last scene survived
		// rt = slot number (1..31): textures in the same slot share a render target (1 scene per slot per frame).
		// slots in use: 1~5 batch layers, 6 composite output, 7/8 VDP1, 9~13 layer cache (ADR-057)
		static SceGxmRenderTarget *slots[32];
		const int slot = rt & 31;
		if (!slots[slot]) {
			const int rr = make_rt(t->w, h, 1, &slots[slot]);
			RSTV_LOG("gx: render target slot %d %dx%d -> %08x", slot, t->w, h, rr);
			if (rr < 0)
				slots[slot] = s_rt_tex;
		}
		t->target = slots[slot];
	}
	return t;
}

void gx_tex_filter(GxTex *t, int linear)
{
	const SceGxmTextureFilter f = linear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
	sceGxmTextureSetMinFilter(&t->tex, f);
	sceGxmTextureSetMagFilter(&t->tex, f);
}

void gx_tex_repeat_u(GxTex *t)
{
	sceGxmTextureSetUAddrMode(&t->tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
}

// ---- shaders ----

static void shark_log(const char *msg, shark_log_level level, int line)
{
	RSTV_LOG("gx: shader %d line %d: %s", (int)level, line, msg);
}

// Cg source -> GXP. Cached in ux0:data/restv/shaders/<hash>.gxp
static const SceGxmProgram *compile(const char *src, int fragment);
static const SceGxmProgram *compile_uncached(const char *src, int fragment, uint32_t h);
const void *gx_compile(const char *src, int fragment) { return compile(src, fragment); }

// only a few vertex shaders, looked up again on every shader variant registration: keep in memory (ADR-084: a memory card read at registration
// happened inside the GPU submit lock and stalled for 182ms)
static struct { uint32_t h; const SceGxmProgram *p; } s_vcache[16];
static int s_nvcache;
static volatile int s_vcache_lock;

static const SceGxmProgram *compile(const char *src, int fragment)
{
	uint32_t h = 2166136261u;
	for (const char *p = src; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	h ^= fragment ? 0x9E3779B9u : 0x85EBCA6Bu;
	if (!fragment) {
		const SceGxmProgram *hit = NULL;
		while (__sync_lock_test_and_set(&s_vcache_lock, 1))
			;
		for (int i = 0; i < s_nvcache; i++)
			if (s_vcache[i].h == h)
				hit = s_vcache[i].p;
		__sync_lock_release(&s_vcache_lock);
		if (hit)
			return hit;
		const SceGxmProgram *p = compile_uncached(src, fragment, h);
		if (p) {
			while (__sync_lock_test_and_set(&s_vcache_lock, 1))
				;
			if (s_nvcache < 16) {
				s_vcache[s_nvcache].h = h;
				s_vcache[s_nvcache].p = p;
				s_nvcache++;
			}
			__sync_lock_release(&s_vcache_lock);
		}
		return p;
	}
	return compile_uncached(src, fragment, h);
}

// ADR-193: while the loading screen is up (gx_busy_cb set, main thread), a compile runs on a helper thread and the
// main thread calls gx_busy_cb about every 50 ms (the loading bar moves instead of the screen freezing for seconds)
void (*gx_busy_cb)(void);
volatile int gx_compiling; // a compile is running on the helper thread (the loading screen says so)
static SceUID s_busy_thread_owner = -1; // the thread allowed to use gx_busy_cb (the one that set it)
void gx_set_busy(void (*cb)(void))
{
	gx_busy_cb = cb;
	s_busy_thread_owner = cb ? sceKernelGetThreadId() : -1;
}
struct ShJob { const char *src; uint32_t size; int fragment; SceGxmProgram *prog; };
static int shark_job(SceSize args, void *argp)
{
	struct ShJob *j = *(struct ShJob **)argp;
	(void)args;
	j->prog = shark_compile_shader_extended(j->src, &j->size, j->fragment ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER,
		SHARK_OPT_FAST, 1, 0, 1);
	return 0;
}
static SceGxmProgram *shark_compile_busy(const char *src, uint32_t *size, int fragment)
{
	if (gx_busy_cb && sceKernelGetThreadId() == s_busy_thread_owner) {
		struct ShJob j = { src, *size, fragment, NULL }, *jp = &j;
		// CPU2: the render worker is not running yet; on CPU1 the spinning sound thread would starve it (ADR-185)
		SceUID t = sceKernelCreateThread("rstv_shjob", shark_job, 160, 0x400000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
		if (t >= 0 && sceKernelStartThread(t, sizeof(jp), &jp) >= 0) {
			gx_compiling = 1;
			for (;;) {
				SceUInt to = 50000;
				if (sceKernelWaitThreadEnd(t, NULL, &to) >= 0)
					break;
				gx_busy_cb();
			}
			sceKernelDeleteThread(t);
			gx_compiling = 0;
			*size = j.size;
			return j.prog;
		}
		if (t >= 0)
			sceKernelDeleteThread(t);
	}
	return shark_compile_shader_extended(src, size, fragment ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER, SHARK_OPT_FAST, 1, 0, 1);
}

static const SceGxmProgram *compile_uncached(const char *src, int fragment, uint32_t h)
{
	char path[96];
	snprintf(path, sizeof(path), SHADER_DIR "/%08x.gxp", (unsigned)h);
	FILE *f = NULL;
#ifdef RSTV_DEBUG_TOOLS
	static int s_fresh = -1; // test: ux0:data/restv/shcfresh compiles fragment shaders again instead of reading the cache
	if (s_fresh < 0) {
		FILE *ff = fopen("ux0:data/restv/shcfresh", "rb");
		s_fresh = ff != NULL;
		if (ff)
			fclose(ff);
	}
	if (!(s_fresh && fragment))
#endif
	f = fopen(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		void *buf = n > 0 ? malloc(n) : NULL;
		const int ok = buf && fread(buf, 1, n, f) == (size_t)n;
		fclose(f);
		if (ok && sceGxmProgramCheck((const SceGxmProgram *)buf) == 0)
			return (const SceGxmProgram *)buf;
		free(buf);
	}
	if (!s_shark_ok)
		return NULL;
	shark_install_log_cb(shark_log);
	uint32_t size = (uint32_t)strlen(src); // in: source length, out: GXP size
	// same options as vitaGL defaults (FAST, fastmath, fastint)
	SceGxmProgram *prog = shark_compile_busy(src, &size, fragment);
	if (!prog || !size) {
		shark_clear_output();
		{
			SceKernelFreeMemorySizeInfo fm;
			fm.size = sizeof(fm);
			sceKernelGetFreeMemorySize(&fm);
			void *probe = malloc(8 << 20);
			RSTV_LOG("gx: compile failed (%s) free user %u cdram %u phycont %u, newlib 8MB %s", fragment ? "fragment" : "vertex",
				fm.size_user, fm.size_cdram, fm.size_phycont, probe ? "ok" : "fail");
			free(probe);
		}
		return NULL;
	}
	void *copy = malloc(size);
	memcpy(copy, prog, size);
	shark_clear_output();
	f = fopen(path, "wb");
	if (f) {
		fwrite(copy, 1, size, f);
		fclose(f);
	}
	return (const SceGxmProgram *)copy;
}

GxProg *gx_prog(const char *vs_src, const char *fs_src, const GxAttr *attrs, int nattr, int stride, int blend)
{
	const SceGxmProgram *vg = compile(vs_src, 0), *fg = vg ? compile(fs_src, 1) : NULL;
	if (!vg || !fg)
		return NULL;
	return gx_prog_bin(vg, fg, attrs, nattr, stride, blend);
}

GxProg *gx_prog_bin(const void *vgp, const void *fgp, const GxAttr *attrs, int nattr, int stride, int blend)
{
	const SceGxmProgram *vg = (const SceGxmProgram *)vgp, *fg = (const SceGxmProgram *)fgp;
	if (!vg || !fg)
		return NULL;
	SceGxmShaderPatcherId vid, fid;
	if (sceGxmShaderPatcherRegisterProgram(s_patcher, vg, &vid) < 0 || sceGxmShaderPatcherRegisterProgram(s_patcher, fg, &fid) < 0)
		return NULL;
	SceGxmVertexAttribute va[16];
	for (int i = 0; i < nattr && i < 16; i++) {
		const SceGxmProgramParameter *p = sceGxmProgramFindParameterByName(vg, attrs[i].name);
		if (!p) {
			RSTV_LOG("gx: attribute %s not found", attrs[i].name);
			return NULL;
		}
		va[i].streamIndex = 0;
		va[i].offset = (uint16_t)attrs[i].offset;
		va[i].format = (uint8_t)attrs[i].fmt;
		va[i].componentCount = (uint8_t)attrs[i].count;
		va[i].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(p);
	}
	SceGxmVertexStream vs;
	vs.stride = (uint16_t)stride;
	vs.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	GxProg *g = (GxProg *)calloc(1, sizeof(GxProg));
	g->vgxp = vg;
	g->fgxp = fg;
	int r = sceGxmShaderPatcherCreateVertexProgram(s_patcher, vid, va, nattr, &vs, 1, &g->vp);
	if (r < 0) {
		RSTV_LOG("gx: vertex program %08x", r);
		free(g);
		return NULL;
	}
	SceGxmBlendInfo bi;
	memset(&bi, 0, sizeof(bi));
	bi.colorMask = SCE_GXM_COLOR_MASK_ALL;
	bi.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
	bi.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
	bi.colorSrc = blend == GX_BLEND_PREMUL ? SCE_GXM_BLEND_FACTOR_ONE : SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	bi.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	bi.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
	bi.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	r = sceGxmShaderPatcherCreateFragmentProgram(s_patcher, fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		blend != GX_BLEND_NONE ? &bi : NULL, vg, &g->fp);
	if (r < 0) {
		RSTV_LOG("gx: fragment program %08x", r);
		free(g);
		return NULL;
	}
	g->vbuf = sceGxmProgramGetDefaultUniformBufferSize(vg) > 0;
	g->fbuf = sceGxmProgramGetDefaultUniformBufferSize(fg) > 0;
	return g;
}

GxParam gx_param(const GxProg *p, const char *name)
{
	GxParam u;
	memset(&u, 0, sizeof(u));
	u.p = sceGxmProgramFindParameterByName(p->fgxp, name);
	if (!u.p) {
		u.p = sceGxmProgramFindParameterByName(p->vgxp, name);
		u.vertex = 1;
	}
	if (u.p)
		u.index = sceGxmProgramParameterGetResourceIndex(u.p);
	else
		RSTV_LOG("gx: parameter %s not found", name);
	return u;
}

// ---- frames / scenes ----

// ADR-029: worker thread (CPU2) also uses GXM. Scene use is serialized with gx_lock, temp memory allocation with a short spinlock
static volatile int s_gxlock, s_templock;
static void spin_lock(volatile int *l)
{
	int n = 0;
	while (__sync_lock_test_and_set(l, 1))
		if (++n > 64)
			sceKernelDelayThread(10);
}
static void spin_unlock(volatile int *l) { __sync_lock_release(l); }
void gx_lock(void) { spin_lock(&s_gxlock); }
void gx_unlock(void) { spin_unlock(&s_gxlock); }

static void ensure_frame_locked(void);
static volatile unsigned s_gpu_wait_us; // diag: time spent waiting with the GPU 2 frames behind (rough, 100us units)
unsigned gx_gpu_wait_us(void) { return s_gpu_wait_us; }
static void ensure_frame(void)
{
	spin_lock(&s_templock);
	ensure_frame_locked();
	spin_unlock(&s_templock);
}
static void ensure_frame_locked(void)
{
	if (s_frame_ready)
		return;
	// wait if the GPU is more than 2 frames behind -> triple-buffered resources (texture sets, temp memory) can be reused safely
	int waited = 0;
	while (s_submitted >= 2 && *s_notif + 2 < s_submitted) {
		sceKernelDelayThread(100);
		s_gpu_wait_us += 100;
		if (++waited == 20000) // 2 s
			RSTV_LOG("gx: GPU stalled? completed %u submitted %u", *s_notif, s_submitted);
	}
	s_temp_off = 0;
	s_frame_ready = 1;
}

void gx_frame_begin(void)
{
	ensure_frame();
}

static int s_trace = 40; // log the first few scenes/flips (to locate hangs)
void gx_begin(GxTex *target)
{
	ensure_frame();
	if (s_trace > 0) {
		s_trace--;
		RSTV_LOG("gx: begin %s (submitted %u completed %u)", target ? "tex" : "disp", s_submitted, *s_notif);
	}
	int w, h;
	if (!target) {
		sceGxmBeginScene(s_ctx, 0, s_rt_disp, NULL, NULL, s_disp_sync[s_back], &s_disp_surf[s_back], NULL);
		w = DISP_W;
		h = DISP_H;
		s_scene_disp = 1;
	} else {
		const int r = sceGxmBeginScene(s_ctx, 0, target->target, NULL, NULL, NULL, &target->surf, NULL);
		static int logged;
		if (r < 0 && logged < 5) {
			logged++;
			RSTV_LOG("gx: begin scene (texture %dx%d) %08x", target->w, target->h, r);
		}
		// ADR-058: textures with their own render target (plane cache 1024x512) use their full size
		w = (target->target != s_rt_tex || target->w < RT_W) ? target->w : RT_W;
		h = (target->target != s_rt_tex || target->h < RT_H) ? target->h : RT_H;
		s_scene_disp = 0;
	}
	// screen line 0 = NDC y -1 (shaders build coords with this rule)
	sceGxmSetViewport(s_ctx, w * 0.5f, w * 0.5f, h * 0.5f, h * 0.5f, 0.5f, 0.5f);
	sceGxmSetRegionClip(s_ctx, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, w - 1, h - 1);
	sceGxmSetFrontDepthFunc(s_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(s_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
	s_in_scene = 1;
}

void gx_end(void)
{
	if (!s_in_scene)
		return;
	if (s_scene_disp) {
		SceGxmNotification n;
		n.address = s_notif;
		n.value = s_submitted + 1;
		sceGxmEndScene(s_ctx, NULL, &n);
	} else {
		const int r = sceGxmEndScene(s_ctx, NULL, NULL);
		static int logged;
		if (r < 0 && logged < 5) {
			logged++;
			RSTV_LOG("gx: end scene %08x", r);
		}
	}
	s_in_scene = 0;
}

void gx_swap(void)
{
	if (s_trace > 0) {
		s_trace--;
		RSTV_LOG("gx: swap %u", s_submitted);
	}
	sceGxmPadHeartbeat(&s_disp_surf[s_back], s_disp_sync[s_back]);
	DispData d;
	d.addr = s_disp_addr[s_back];
	sceGxmDisplayQueueAddEntry(s_disp_sync[s_front], s_disp_sync[s_back], &d);
	s_front = s_back;
	s_back = (s_back + 1) % DISP_BUFS;
	spin_lock(&s_templock);
	s_submitted++;
	s_frame_ready = 0;
	spin_unlock(&s_templock);
}

void gx_finish(void)
{
	sceGxmFinish(s_ctx);
}

int gx_frame_number(void)
{
	return (int)s_submitted;
}

// ---- drawing ----

void gx_use(const GxProg *p)
{
	sceGxmSetVertexProgram(s_ctx, p->vp);
	sceGxmSetFragmentProgram(s_ctx, p->fp);
	s_vbuf = s_fbuf = NULL;
	if (p->vbuf)
		sceGxmReserveVertexDefaultUniformBuffer(s_ctx, &s_vbuf);
	if (p->fbuf)
		sceGxmReserveFragmentDefaultUniformBuffer(s_ctx, &s_fbuf);
}

void gx_set4(GxParam u, const float *v)
{
	void *buf = u.vertex ? s_vbuf : s_fbuf;
	if (u.p && buf)
		sceGxmSetUniformDataF(buf, u.p, 0, 4, v);
}

void gx_set_tex(GxParam sampler, const GxTex *t)
{
	if (sampler.p && t)
		sceGxmSetFragmentTexture(s_ctx, sampler.index, &t->tex);
}

void *gx_temp(int bytes)
{
	spin_lock(&s_templock);
	ensure_frame_locked();
	bytes = ALIGN(bytes, 16);
	if (s_temp_off + bytes > TEMP_SEG) {
		spin_unlock(&s_templock);
		RSTV_LOG("gx: temp memory exhausted (%d)", bytes);
		return NULL;
	}
	void *p = s_temp_base + (s_submitted % DISP_BUFS) * TEMP_SEG + s_temp_off;
	s_temp_off += bytes;
	spin_unlock(&s_templock);
	return p;
}

void gx_draw(SceGxmPrimitiveType prim, const void *vertices, const uint16_t *indices, int count)
{
	sceGxmSetVertexStream(s_ctx, 0, vertices);
	const int r = sceGxmDraw(s_ctx, prim, SCE_GXM_INDEX_FORMAT_U16, indices, count);
	static int logged;
	if (r < 0 && logged < 5) {
		logged++;
		RSTV_LOG("gx: draw %08x (count %d, scene %d)", r, count, s_in_scene);
	}
}

void *gx_static(int bytes)
{
	return gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, (unsigned)bytes, SCE_GXM_MEMORY_ATTRIB_READ, NULL);
}

const uint16_t *gx_seq_indices(int count)
{
	(void)count;
	return s_seq;
}
