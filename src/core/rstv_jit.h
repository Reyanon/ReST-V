/*  ReST-V: executable memory for the SH-2 dynamic recompiler.
    Vita: sceKernelAllocMemBlockForVM + Open/CloseVMDomain (writes must be bracketed).
    Other ARM Linux (qemu test harness): mmap RWX, writes need no bracketing.
    GPL-2.0-or-later. */
#ifndef RSTV_JIT_H
#define RSTV_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Vita: call early (before the GPU allocations take the free memory) to reserve the code region.
// rstv_jit_code_alloc then hands out this reservation.
int rstv_jit_code_reserve(size_t size);
void *rstv_jit_code_alloc(size_t size);
// bytes rstv_jit_code_alloc can still hand out (Vita; (size_t)-1 elsewhere)
size_t rstv_jit_code_avail(void);
// Reserved region (Vita), for the startup self test. NULL if nothing reserved
void *rstv_jit_code_region(size_t *size);
// Call before writing generated code (or any data inside the code region)
void rstv_jit_code_write_begin(void);
// Call after writing: makes [p, p+len) executable and coherent with the I-cache
void rstv_jit_code_write_end(void *p, size_t len);
// Makes [p, p+len) coherent without ending the write section (several ranges, then write_end on the last)
void rstv_jit_code_sync(void *p, size_t len);
void rstv_jit_shutdown(void); /* ADR-086: before the process exits */

#ifdef __cplusplus
}
#endif

#endif
