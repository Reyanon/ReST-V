/*  ReST-V: executable memory for the SH-2 dynamic recompiler (see rstv_jit.h).
    GPL-2.0-or-later. */
#include "rstv_jit.h"

#include <stdint.h>

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

static SceUID s_uid = -1;
static void *s_base;
static size_t s_size;
static size_t s_used; // ADR-110: bump allocation (SH-2 blocks, then the SCSP DSP code)

int rstv_jit_code_reserve(size_t size)
{
	if (s_uid >= 0)
		return 0;
	size = (size + 0xFFFFF) & ~(size_t)0xFFFFF;
	SceUID uid = sceKernelAllocMemBlockForVM("rstv_sh2_code", size);
	if (uid < 0)
		return uid;
	void *base = NULL;
	int r = sceKernelGetMemBlockBase(uid, &base);
	if (r < 0) {
		sceKernelFreeMemBlock(uid);
		return r;
	}
	s_uid = uid;
	s_base = base;
	s_size = size;
	return 0;
}

void *rstv_jit_code_alloc(size_t size)
{
	if (s_uid < 0 && rstv_jit_code_reserve(size) != 0)
		return NULL;
	size = (size + 63) & ~(size_t)63;
	if (size > s_size - s_used)
		return NULL;
	void *p = (char *)s_base + s_used;
	s_used += size;
	return p;
}

size_t rstv_jit_code_avail(void)
{
	return s_uid >= 0 ? s_size - s_used : 0;
}

void *rstv_jit_code_region(size_t *size)
{
	if (size)
		*size = s_size;
	return s_uid >= 0 ? s_base : NULL;
}

int rstv_jit_keep_open = 1; // ADR-043: open the VM domain once and leave it open (no open/close per block)
// ADR-110: the domain is opened per thread (the sound thread writing the SCSP DSP code got a data abort while the
// emu thread had it open), so remember the threads that opened it
static SceUID s_open_tid[4];
static int s_open;

void rstv_jit_code_write_begin(void)
{
	if (rstv_jit_keep_open) {
		const SceUID tid = sceKernelGetThreadId();
		for (int i = 0; i < s_open; i++)
			if (s_open_tid[i] == tid)
				return;
		sceKernelOpenVMDomain();
		if (s_open < 4)
			s_open_tid[s_open++] = tid;
		return;
	}
	sceKernelOpenVMDomain();
}

void rstv_jit_code_write_end(void *p, size_t len)
{
	sceKernelSyncVMDomain(s_uid, p, len);
	if (!rstv_jit_keep_open)
		sceKernelCloseVMDomain();
}
void rstv_jit_code_sync(void *p, size_t len)
{
	sceKernelSyncVMDomain(s_uid, p, len);
}

/* ReST-V (ADR-086): close the VM domain left open (ADR-043) and free the code block before the process exits */
void rstv_jit_shutdown(void)
{
	if (s_open) {
		sceKernelCloseVMDomain();
		s_open = 0;
	}
	if (s_uid >= 0) {
		sceKernelFreeMemBlock(s_uid);
		s_uid = -1;
		s_base = NULL;
		s_used = 0;
	}
}

#else
void rstv_jit_shutdown(void) {}
#include <sys/mman.h>

int rstv_jit_keep_open = 1; // code memory is always writable here (ADR-043/048)

int rstv_jit_code_reserve(size_t size) { (void)size; return 0; }
void *rstv_jit_code_region(size_t *size) { if (size) *size = 0; return NULL; }
size_t rstv_jit_code_avail(void) { return (size_t)-1; }

void *rstv_jit_code_alloc(size_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

void rstv_jit_code_write_begin(void) {}

void rstv_jit_code_write_end(void *p, size_t len)
{
	__builtin___clear_cache((char *)p, (char *)p + len);
}
void rstv_jit_code_sync(void *p, size_t len)
{
	__builtin___clear_cache((char *)p, (char *)p + len);
}
#endif
