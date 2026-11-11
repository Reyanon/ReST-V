#define HOST_REGS 13
#define HOST_CCREG 10
#define EXCLUDE_REG 11
#define SLAVERA_REG 8

#define HOST_IMM8 1
#define HAVE_CMOV_IMM 1
#define CORTEX_A8_BRANCH_PREDICTION_HACK 1
#define USE_MINI_HT 1
//#define REG_PREFETCH 1

/* ARM calling convention:
   r0-r3, r12: caller-save
   r4-r11: callee-save */

#define ARG1_REG 0
#define ARG2_REG 1
#define ARG3_REG 2
#define ARG4_REG 3

/* GCC register naming convention:
   r10 = sl (base)
   r11 = fp (frame pointer)
   r12 = ip (scratch)
   r13 = sp (stack pointer)
   r14 = lr (link register)
   r15 = pc (program counter) */

#define FP 11
#define LR 14
#define HOST_TEMPREG 14

// Note: FP is set to &dynarec_local when executing generated code.
// Thus the local variables are actually global and not on the stack.

extern u32 memory_map[1048576]; // 32-bit

// ReST-V (ADR-222): the code buffer is a variable. Vita: the VM block of rstv_jit (rstv_jit_code_alloc), cache sync
// through it; elsewhere (ARM harness) a mapping at 0x90000000, so pointers have the top bit set as on the Vita
extern u8 *ari_code_base;
#define BASE_ADDR ((u32)ari_code_base)
#define TARGET_SIZE_2 23 // 8 MB of the Vita's 16 MB code region; devmiyax is not in the same process then (ADR-229)
#ifdef __vita__
#define __clear_cache ari_clear_cache
void ari_clear_cache(void *start, void *end);
#endif

#ifdef ANDROID
#define __clear_cache clear_cache
#endif
