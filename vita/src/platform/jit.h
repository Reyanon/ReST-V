#ifndef RSTV_PLATFORM_JIT_H
#define RSTV_PLATFORM_JIT_H

#include <stddef.h>
#include <stdint.h>

// SH-2 recompiler code area size (2048 blocks × 4KB + header ≈ 8.4MB, ADR-010)
#define RSTV_JIT_CODE_SIZE (9u << 20)

// Allocate the code area at startup and check generate → run → overwrite → rerun on real hardware.
// Result in msg; returns 0 (success) or negative. The area is never freed (the recompiler uses it)
int rstv_jit_selftest(char *msg, size_t msg_len);

#endif
