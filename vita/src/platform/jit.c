// JIT check on real hardware. Executable memory is the core's rstv_jit (SH-2 recompiler code area).
// The Vita cannot reallocate a VM block once freed, so allocate once at startup and keep it.
#include "platform/jit.h"

#include <stdio.h>

#include "rstv_jit.h"
#include "platform/timer.h"

typedef int (*JitFn)(int);

int rstv_jit_selftest(char *msg, size_t msg_len)
{
	int r = rstv_jit_code_reserve(RSTV_JIT_CODE_SIZE);
	if (r != 0)
		r = rstv_jit_code_reserve(9u << 20); // the size before ADR-136 (no slave block cache: slaveasync stays off)
	size_t size = 0;
	uint8_t *base = (uint8_t *)rstv_jit_code_region(&size);
	if (r != 0 || !base) {
		snprintf(msg, msg_len, "JIT FAIL reserve 0x%08x", r);
		return -1;
	}

	// ARM: add r0, r0, #imm ; bx lr
	uint32_t *code = (uint32_t *)base;
	uint64_t t0 = rstv_time_us();
	rstv_jit_code_write_begin();
	code[0] = 0xE2800000u | 42;
	code[1] = 0xE12FFF1Eu;
	rstv_jit_code_write_end(code, 8);
	uint64_t t1 = rstv_time_us();
	int v1 = ((JitFn)code)(100);

	rstv_jit_code_write_begin();
	code[0] = 0xE2800000u | 7;
	rstv_jit_code_write_end(code, 8);
	int v2 = ((JitFn)code)(100);

	uint32_t *tail = (uint32_t *)(base + size - 8);
	rstv_jit_code_write_begin();
	tail[0] = 0xE2800000u | 1;
	tail[1] = 0xE12FFF1Eu;
	rstv_jit_code_write_end(tail, 8);
	int v3 = ((JitFn)tail)(100);

	uint64_t t2 = rstv_time_us();
	for (int i = 0; i < 1000; i++) {
		rstv_jit_code_write_begin();
		rstv_jit_code_write_end(code, 8);
	}
	uint64_t t3 = rstv_time_us();

	int ok = (v1 == 142 && v2 == 107 && v3 == 101);
	snprintf(msg, msg_len, "JIT %s v=%d,%d,%d size=%uMB first=%lluus open/close=%.2fus",
		ok ? "OK" : "FAIL", v1, v2, v3, (unsigned)(size >> 20),
		(unsigned long long)(t1 - t0), (double)(t3 - t2) / 1000.0);
	return ok ? 0 : -2;
}
