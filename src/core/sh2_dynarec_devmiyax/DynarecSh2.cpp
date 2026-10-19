/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/


#include <stdio.h>
extern "C" { extern int rstv_idle_hint_n[2]; extern int rstv_idle_auto; int rstv_idle_hint_at(int w, unsigned int addr); } // ADR-106
#include <string.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <stdint.h>
#include <core.h>
#include <unordered_map>

#include "sh2core.h"
#include "debug.h"
#include "yabause.h"
#include "bios.h"
extern "C" {
#include "scu.h"
}

#include "DynarecSh2.h"
#include "rstv_prof.h"
#include "opcodes.h"
#if defined(RSTV)
#include "../sh2int.h"
#include "../stv.h"
#endif
#if defined(WEBINTERFACE)
#define DEBUG_CPU
#endif
//#define DEBUG_CPU
//#define EXECUTE_STAT
//#define BUILD_INFO
//#define LOG printf

CompileBlocks * CompileBlocks::instance_ = NULL;
DynarecSh2 * DynarecSh2::CurrentContext = NULL;
// diag (vdp1.cpp v1watch): PC / PR / R0..R15 of the recompiled CPU running now. 0 = none, 1 = master, 2 = slave
extern "C" int rstv_dyn_where(u32 *pc, u32 *pr, u32 *r)
{
  DynarecSh2 *c = DynarecSh2::CurrentContext;
  if (!c)
    return 0;
  *pc = c->GET_PC();
  *pr = c->GET_PR();
  memcpy(r, c->GetGenRegPtr(), 16 * sizeof(u32));
  return c->IsSlave() ? 2 : 1;
}
#if !defined(_WINDOWS)
#if defined(__arm__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{ 
    const int syscall = 0xf0002;
      __asm __volatile (
     "mov   r0, %0\n"      
     "mov   r1, %1\n"
     "mov   r7, %2\n"
     "mov     r2, #0x0\n"
     "svc     0x00000000\n"
     :
     : "r" (begin), "r" (end), "r" (syscall)
     : "r0", "r1", "r7"
    );
}
#elif defined(__aarch64__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{
    // Don't rely on GCC's __clear_cache implementation, as it caches
    // icache/dcache cache line sizes, that can vary between cores on
    // big.LITTLE architectures.
    uint64_t addr, ctr_el0;
    static size_t icache_line_size = 0xffff, dcache_line_size = 0xffff;
    size_t isize, dsize;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
    isize = 4 << ((ctr_el0 >> 0) & 0xf);
    dsize = 4 << ((ctr_el0 >> 16) & 0xf);

    // use the global minimum cache line size
    icache_line_size = isize = icache_line_size < isize ? icache_line_size : isize;
    dcache_line_size = dsize = dcache_line_size < dsize ? dcache_line_size : dsize;

    addr = (uint64_t)begin & ~(uint64_t)(dsize - 1);
    for (; addr < (uint64_t)end; addr += dsize)
        // use "civac" instead of "cvau", as this is the suggested workaround for
        // Cortex-A53 errata 819472, 826319, 827319 and 824069.
            __asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");

    addr = (uint64_t)begin & ~(uint64_t)(isize - 1);
    for (; addr < (uint64_t)end; addr += isize)
            __asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");

    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");
}
#else
void cacheflush(uintptr_t begin, uintptr_t end, int flag ){
  __builtin___clear_cache((void*)begin,(void*)end);
}
#endif
#endif

i_desc opcode_list[] =
{
  { ZERO_F,	"clrt",						0xffff, 0x8,	0, sh2_CLRT},
  { ZERO_F,   "clrmac",					0xffff, 0x28,	0, sh2_CLRMAC},
  { ZERO_F,   "div0u",					0xffff, 0x19,	0, sh2_DIV0U},
  { ZERO_F,   "nop",						0xffff, 0x9,	0, sh2_NOP},
  { ZERO_F,   "rte",						0xffff, 0x2b,	0, sh2_RTE},
  { ZERO_F,   "rts",						0xffff, 0xb,	0, sh2_RTS},
  { ZERO_F,   "sett",						0xffff, 0x18,	0, sh2_SETT},
  { ZERO_F,   "sleep",					0xffff, 0x1b,	0, sh2_SLEEP},
  { N_F,      "cmp/pl r%d",				0xf0ff, 0x4015,	0, sh2_CMP_PL},
  { N_F,      "cmp/pz r%d",				0xf0ff, 0x4011,	0, sh2_CMP_PZ},
  { N_F,      "dt r%d",					0xf0ff, 0x4010,	0, sh2_DT},
  { N_F,      "movt r%d",					0xf0ff, 0x0029,	0, sh2_MOVT},
  { N_F,      "rotl r%d",					0xf0ff, 0x4004,	0, sh2_ROTL},
  { N_F,      "rotr r%d",					0xf0ff, 0x4005,	0, sh2_ROTR},
  { N_F,      "rotcl r%d",				0xf0ff, 0x4024,	0, sh2_ROTCL},
  { N_F,      "rotcr r%d",				0xf0ff, 0x4025,	0, sh2_ROTCR},
  { N_F,      "shal r%d",					0xf0ff, 0x4020,	0, sh2_SHL},
  { N_F,      "shar r%d",					0xf0ff, 0x4021,	0, sh2_SHAR},
  { N_F,      "shll r%d",					0xf0ff, 0x4000,	0, sh2_SHL},
  { N_F,      "shlr r%d",					0xf0ff, 0x4001,	0, sh2_SHLR},
  { N_F,      "shll2 r%d",				0xf0ff, 0x4008,	0, sh2_SHLL2},
  { N_F,      "shlr2 r%d",				0xf0ff, 0x4009,	0, sh2_SHLR2},
  { N_F,      "shll8 r%d",				0xf0ff, 0x4018,	0, sh2_SHLL8},
  { N_F,      "shlr8 r%d",				0xf0ff, 0x4019,	0, sh2_SHLR8},
  { N_F,      "shll16 r%d",				0xf0ff, 0x4028,	0, sh2_SHLL16},
  { N_F,      "shlr16 r%d",				0xf0ff, 0x4029,	0, sh2_SHLL16},
  { N_F,      "stc sr, r%d",				0xf0ff, 0x0002,	0, sh2_STC_SR},
  { N_F,      "stc gbr, r%d",				0xf0ff, 0x0012,	0, sh2_STC_GBR},
  { N_F,      "stc vbr, r%d",				0xf0ff, 0x0022,	0, sh2_STC_VBR},
  { N_F,      "sts mach, r%d",			0xf0ff, 0x000a,	0, sh2_STS_MACH},
  { N_F,      "sts macl, r%d",			0xf0ff, 0x001a,	0, sh2_STS_MACL},
  { N_F,      "sts pr, r%d",				0xf0ff, 0x002a,	0, sh2_STS_PR},
  { N_F,      "tas.b @r%d",				0xf0ff, 0x401b,	0, sh2_TAS_B},
  { N_F,      "stc.l sr, @-r%d",			0xf0ff, 0x4003,	0, sh2_STC_SR_DEC},
  { N_F,      "stc.l gbr, @-r%d",			0xf0ff, 0x4013,	0, sh2_STC_GBR_DEC},
  { N_F,      "stc.l vbr, @-r%d",			0xf0ff, 0x4023,	0, sh2_STC_VBR_DEC},
  { N_F,      "sts.l mach, @-r%d",		0xf0ff, 0x4002,	0, sh2_STS_MACH_DEC},
  { N_F,      "sts.l macl, @-r%d",		0xf0ff, 0x4012,	0, sh2_STS_MACL_DEC},
  { N_F,      "sts.l pr, @-r%d",			0xf0ff, 0x4022,	0, sh2_STS_PR_DEC},
  { M_F,      "ldc r%d, sr",				0xf0ff, 0x400e,	0, sh2_LDC_SR},
  { M_F,      "ldc r%d, gbr",				0xf0ff, 0x401e,	0, sh2_LDC_GBR},
  { M_F,      "ldc r%d, vbr",				0xf0ff, 0x402e,	0, sh2_LDC_VBR},
  { M_F,      "lds r%d, mach",			0xf0ff, 0x400a,	0, sh2_LDS_MACH},
  { M_F,      "lds r%d, macl",			0xf0ff, 0x401a,	0, sh2_LDS_MACL},
  { M_F,      "lds r%d, pr",				0xf0ff, 0x402a,	0, sh2_LDS_PR},
  { M_F,      "jmp @r%d",					0xf0ff, 0x402b,	0, sh2_JMP},
  { M_F,      "jsr @r%d",					0xf0ff, 0x400b,	0, sh2_JSR}, 
  { M_F,      "ldc.l @r%d+, sr",			0xf0ff, 0x4007,	0, sh2_LDC_SR_INC},
  { M_F,      "ldc.l @r%d+, gbr",			0xf0ff, 0x4017,	0, sh2_LDC_GBR_INC},
  { M_F,      "ldc.l @r%d+, vbr",			0xf0ff, 0x4027,	0, sh2_LDC_VBR_INC},
  { M_F,      "lds.l @r%d+, mach",		0xf0ff, 0x4006,	0, sh2_LDS_MACH_INC},
  { M_F,      "lds.l @r%d+, macl",		0xf0ff, 0x4016,	0, sh2_LDS_MACL_INC},
  { M_F,      "lds.l @r%d+, pr",			0xf0ff, 0x4026,	0, sh2_LDS_PR_INC},
  { M_F,      "braf r%d",					0xf0ff, 0x0023,	0, sh2_BRAF},
  { M_F,      "bsrf r%d",					0xf0ff, 0x0003,	0, sh2_BSRF},
  { NM_F,     "add r%d, r%d",				0xf00f, 0x300c,	0, sh2_ADD},
  { NM_F,     "addc r%d, r%d",			0xf00f, 0x300e,	0, sh2_ADDC},
  { NM_F,     "addv r%d, r%d",			0xf00f, 0x300f,	0, sh2_ADDV},
  { NM_F,     "and r%d, r%d",				0xf00f, 0x2009,	0, sh2_AND},
  { NM_F,     "cmp/eq r%d, r%d",			0xf00f, 0x3000,	0, sh2_CMP_EQ},
  { NM_F,     "cmp/hs r%d, r%d",			0xf00f, 0x3002,	0, sh2_CMP_HS},
  { NM_F,     "cmp/ge r%d, r%d",			0xf00f, 0x3003,	0, sh2_CMP_GE},
  { NM_F,     "cmp/hi r%d, r%d",			0xf00f, 0x3006,	0, sh2_CMP_HI},
  { NM_F,     "cmp/gt r%d, r%d",			0xf00f, 0x3007,	0, sh2_CMP_GT},
  { NM_F,     "cmp/str r%d, r%d",			0xf00f, 0x200c,	0, sh2_CMP_STR},
  { NM_F,     "div1 r%d, r%d",			0xf00f, 0x3004,	0, sh2_DIV1},
  { NM_F,     "div0s r%d, r%d",			0xf00f, 0x2007,	0, sh2_DIV0S},
  { NM_F,     "dmuls.l r%d, r%d",			0xf00f, 0x300d,	0, sh2_DMULS_L},
  { NM_F,     "dmulu.l r%d, r%d",			0xf00f, 0x3005,	0, sh2_DMULU_L},
  { NM_F,     "exts.b r%d, r%d",			0xf00f, 0x600e,	0, sh2_EXTS_B},
  { NM_F,     "exts.w r%d, r%d",			0xf00f, 0x600f,	0, sh2_EXTS_W},
  { NM_F,     "extu.b r%d, r%d",			0xf00f, 0x600c,	0, sh2_EXTU_B},
  { NM_F,     "extu.w r%d, r%d",			0xf00f, 0x600d,	0, sh2_EXTU_W},
  { NM_F,     "mov r%d, r%d",				0xf00f, 0x6003,	0, sh2_MOVR},
  { NM_F,     "mul.l r%d, r%d",			0xf00f, 0x0007,	0, sh2_MUL_L},
  { NM_F,     "muls.w r%d, r%d",			0xf00f, 0x200f,	0, sh2_MULS},
  { NM_F,     "mulu.w r%d, r%d",			0xf00f, 0x200e,	0, sh2_MULU},
  { NM_F,     "neg r%d, r%d",				0xf00f, 0x600b,	0, sh2_NEG},
  { NM_F,     "negc r%d, r%d",			0xf00f, 0x600a,	0, sh2_NEGC},
  { NM_F,     "not r%d, r%d",				0xf00f, 0x6007,	0, sh2_NOT},
  { NM_F,     "or r%d, r%d",				0xf00f, 0x200b,	0, sh2_OR},
  { NM_F,     "sub r%d, r%d",				0xf00f, 0x3008,	0, sh2_SUB},
  { NM_F,     "subc r%d, r%d",			0xf00f, 0x300a,	0, sh2_SUBC},
  { NM_F,     "subv r%d, r%d",			0xf00f, 0x300b,	0, sh2_SUBV},
  { NM_F,     "swap.b r%d, r%d",			0xf00f, 0x6008,	0, sh2_SWAP_B},
  { NM_F,     "swap.w r%d, r%d",			0xf00f, 0x6009,	0, sh2_SWAP_W},
  { NM_F,     "tst r%d, r%d",				0xf00f, 0x2008,	0, sh2_TST},
  { NM_F,     "xor r%d, r%d",				0xf00f, 0x200a,	0, sh2_XOR},
  { NM_F,     "xtrct r%d, r%d",			0xf00f, 0x200d,	0, sh2_XTRCT},
  { NM_F,     "mov.b r%d, @r%d",			0xf00f, 0x2000,	0, sh2_MOVB},
  { NM_F,     "mov.w r%d, @r%d",			0xf00f, 0x2001,	0, sh2_MOVW},
  { NM_F,     "mov.l r%d, @r%d",			0xf00f, 0x2002,	0, sh2_MOVL},
  { NM_F,     "mov.b @r%d, r%d",			0xf00f, 0x6000,	0, sh2_MOVB_MEM},
  { NM_F,     "mov.w @r%d, r%d",			0xf00f, 0x6001,	0, sh2_MOVW_MEM},
  { NM_F,     "mov.l @r%d, r%d",			0xf00f, 0x6002,	0, sh2_MOVL_MEM},
  { NM_F,     "mac.l @r%d+, @r%d+",		0xf00f, 0x000f,	0, sh2_MAC_L},
  { NM_F,     "mac.w @r%d+, @r%d+",		0xf00f, 0x400f,	0, sh2_MAC_W},
  { NM_F,     "mov.b @r%d+, r%d",			0xf00f, 0x6004,	0, sh2_MOVB_INC},
  { NM_F,     "mov.w @r%d+, r%d",			0xf00f, 0x6005,	0, sh2_MOVW_INC},
  { NM_F,     "mov.l @r%d+, r%d",			0xf00f, 0x6006,	0, sh2_MOVL_INC},
  { NM_F,     "mov.b r%d, @-r%d",			0xf00f, 0x2004,	0, sh2_MOVB_DEC},
  { NM_F,     "mov.w r%d, @-r%d",			0xf00f, 0x2005,	0, sh2_MOVW_DEC},
  { NM_F,     "mov.l r%d, @-r%d",			0xf00f, 0x2006,	0, sh2_MOVL_DEC},
  { NM_F,     "mov.b r%d, @(r0, r%d)",	0xf00f, 0x0004,	0, sh2_MOVB_R0},
  { NM_F,     "mov.w r%d, @(r0, r%d)",	0xf00f, 0x0005,	0, sh2_MOVW_R0},
  { NM_F,     "mov.l r%d, @(r0, r%d)",	0xf00f, 0x0006,	0, sh2_MOVL_R0},
  { NM_F,     "mov.b @(r0, r%d), r%d",	0xf00f, 0x000c,	0, sh2_MOVB_R0_MEM},
  { NM_F,     "mov.w @(r0, r%d), r%d",	0xf00f, 0x000d,	0, sh2_MOVW_R0_MEM},
  { NM_F,     "mov.l @(r0, r%d), r%d",	0xf00f, 0x000e,	0, sh2_MOVL_R0_MEM},
  { MD_F,     "mov.b @(0x%03X, r%d), r0",	0xff00, 0x8400,	0, sh2_MOVB_DISP_R0},
  { MD_F,     "mov.w @(0x%03X, r%d), r0", 0xff00, 0x8500,	0, sh2_MOVW_DISP_R0},
  { ND4_F,    "mov.b r0, @(0x%03X, r%d)", 0xff00, 0x8000,	0, sh2_MOVB_R0_DISP},
  { ND4_F,    "mov.w r0, @(0x%03X, r%d)", 0xff00, 0x8100,	0, sh2_MOVW_R0_DISP},
  { NMD_F,    "mov.l r%d, @(0x%03X, r%d)",0xf000, 0x1000,	0, sh2_MOVL_DISP_MEM},
  { NMD_F,    "mov.l @(0x%03X, r%d), r%d",0xf000, 0x5000,	0, MOVLL4},
  { D_F,      "mov.b r0, @(0x%03X, gbr)",	0xff00, 0xc000,	1, sh2_MOVB_R0_GBR},
  { D_F,      "mov.w r0, @(0x%03X, gbr)",	0xff00, 0xc100,	2, sh2_MOVW_R0_GBR},
  { D_F,      "mov.l r0, @(0x%03X, gbr)",	0xff00, 0xc200,	4, sh2_MOVL_R0_GBR},
  { D_F,      "mov.b @(0x%03X, gbr), r0",	0xff00, 0xc400,	1, sh2_MOVB_GBR_R0},
  { D_F,      "mov.w @(0x%03X, gbr), r0",	0xff00, 0xc500,	2, sh2_MOVW_GBR_R0},
  { D_F,      "mov.l @(0x%03X, gbr), r0",	0xff00, 0xc600,	4, sh2_MOVL_GBR_R0},
  { D_F,      "mova @(0x%03X, pc), r0",	0xff00, 0xc700,	4, sh2_MOVA},
  { D_F,      "bf 0x%08X",				0xff00, 0x8b00,	4, sh2_BF},
  { D_F,      "bf/s 0x%08X",				0xff00, 0x8f00,	5, sh2_BF_S},
  { D_F,      "bt 0x%08X",				0xff00, 0x8900,	5, sh2_BT},
  { D_F,      "bt/s 0x%08X",				0xff00, 0x8d00,	5, sh2_BT_S},
  { D12_F,    "bra 0x%08X",				0xf000, 0xa000,	5, sh2_BRA},
  { D12_F,    "bsr 0x%08X",				0xf000, 0xb000,	0, sh2_BSR},
  { ND8_F,    "mov.w @(0x%03X, pc), r%d", 0xf000, 0x9000,	0, sh2_MOV_DISP_W},
  { ND8_F,    "mov.l @(0x%03X, pc), r%d",	0xf000, 0xd000,	2, sh2_MOV_DISP_L},
  { I_F,      "and.b #0x%02X, @(r0, gbr)",0xff00, 0xcd00,	4, sh2_AND_B},
  { I_F,      "or.b #0x%02X, @(r0, gbr)", 0xff00, 0xcf00,	0, sh2_OR_B},
  { I_F,      "tst.b #0x%02X, @(r0, gbr)",0xff00, 0xcc00,	0, sh2_TST_B},
  { I_F,      "xor.b #0x%02X, @(r0, gbr)",0xff00, 0xce00,	0, sh2_XOR_B},
  { I_F,      "and #0x%02X, r0",			0xff00, 0xc900,	0, sh2_ANDI},
  { I_F,      "cmp/eq #0x%02X, r0",		0xff00, 0x8800,	0, sh2_CMP_EQ_IMM},
  { I_F,      "or #0x%02X, r0",			0xff00, 0xcb00,	0, sh2_ORI},
  { I_F,      "tst #0x%02X, r0",			0xff00, 0xc800,	0, sh2_TST_R0},
  { I_F,      "xor #0x%02X, r0",			0xff00, 0xca00,	0, sh2_XORI},
  { I_F,      "trapa #0x%X",				0xff00, 0xc300,	0, sh2_TRAPA},
  { NI_F,     "add #0x%02X, r%d",			0xf000, 0x7000,	0, sh2_ADDI},
  { NI_F,     "mov #0x%02X, r%d",			0xf000, 0xe000,	0, sh2_MOVI},
  { 0,        NULL,						0,      0,		0, 0}
};


void DumpInstX( int i, u32 pc, u16 op  )
{
  char buf[256];
  //sprintf( buf, "%08X : ", pc );

  if (opcode_list[i].format == ZERO_F)
    sprintf( buf,"%s", opcode_list[i].mnem);
  else if (opcode_list[i].format == N_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 8) & 0xf);
  else if (opcode_list[i].format == M_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 8) & 0xf);
  else if (opcode_list[i].format == NM_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 4) & 0xf, (op >> 8) & 0xf);
  else if (opcode_list[i].format == MD_F)
  {
    if (op & 0x100)
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 2, (op >> 4) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, op & 0xf, (op >> 4) & 0xf);
  }
  else if (opcode_list[i].format == ND4_F)
  {
    if (op & 0x100)
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 2, (op >> 4) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xf), (op >> 4) & 0xf);
  }
  else if (opcode_list[i].format == NMD_F)
  {
    if ((op & 0xf000) == 0x1000)
      sprintf( buf,opcode_list[i].mnem, (op >> 4) & 0xf, (op & 0xf) * 4, (op >> 8) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 4, (op >> 4) & 0xf, (op >> 8) & 0xf);
  }
  else if (opcode_list[i].format == D_F)
  {
    if (opcode_list[i].dat <= 4)
    {
      if ((op & 0xff00) == 0xc700)
      {
        sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4);
//				LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + PC);
      }
      else
        sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat);
    }
    else
    {
      if (op & 0x80)  /* sign extend */
        sprintf( buf,opcode_list[i].mnem, (((op & 0xff) + 0xffffff00) * 2) + pc + 4);
      else
        sprintf( buf,opcode_list[i].mnem, ((op & 0xff) * 2) + pc + 4);
    }        
  }
  else if (opcode_list[i].format == D12_F)
  {
    if (op & 0x800)         /* sign extend */
      sprintf( buf,opcode_list[i].mnem, ((op & 0xfff) + 0xfffff000) * 2 + pc + 4);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xfff) * 2 + pc + 4);
  }
  else if (opcode_list[i].format == ND8_F)
  {
    if ((op & 0xf000) == 0x9000)    /* .W */
    {
      sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4, (op >> 8) & 0xf);
      //LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + pc);
    }
    else  /* .L */
    {
      sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4, (op >> 8) & 0xf);
      //LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + ((pc) & 0xfffffffc));
    }
  }
  else if (opcode_list[i].format == I_F)
    sprintf( buf,opcode_list[i].mnem, op & 0xff);
  else if (opcode_list[i].format == NI_F)
    sprintf( buf,opcode_list[i].mnem, op & 0xff, (op >> 8) & 0xf);
  else
  {
    sprintf( buf,"unrecognized\n");
    return;
  }
  LOG("%08X:%04X %s\n", pc , op, buf );
  return;
}


#define opdesc(op, y, c, d)	x86op_desc(x86_##op, &op##_size, &op##_src, &op##_dest, &op##_off1, &op##_imm, &op##_off3, y, c, d)

#define opNULL			x86op_desc(0,0,0,0,0,0,0,0,0,0)

#if defined(_WINDOWS)
#define PROLOGSIZE		     27    
#define EPILOGSIZE		      3
#define SEPERATORSIZE	     10
#define SEPERATORSIZE_NORMAL 7
#define SEPERATORSIZE_DEBUG  24
#define SEPERATORSIZE_DELAY  7
#define SEPERATORSIZE_DELAY_SLOT  27
#define SEPERATORSIZE_DELAY_AFTER  10 
#define SEPERATORSIZE_DELAYD_DEBUG 34
#define DELAYJUMPSIZE	     17
#define DALAY_CLOCK_OFFSET 6
#define DALAY_CLOCK_OFFSET_DEBUG 6
#define NORMAL_CLOCK_OFFSET 6
#define NORMAL_CLOCK_OFFSET_DEBUG 3
#elif defined(AARCH64)
#define PROLOGSIZE		     (11*4)    
#define SEPERATORSIZE_NORMAL (2*4)
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  (18*4)
#define SEPERATORSIZE_DELAY_SLOT  (15*4)
#define SEPERATORSIZE_DELAY_AFTER  (12*4)
#define DALAY_CLOCK_OFFSET (2*4)
#define DALAY_CLOCK_OFFSET_DEBUG (10*4)
#define SEPERATORSIZE_DELAYD_DEBUG (20*4)
#define EPILOGSIZE		      (10*4)
#define DELAYJUMPSIZE	     (11*4)
#else // ARMv7
#define PROLOGSIZE		     16    
#define SEPERATORSIZE_NORMAL 8
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  36
#define SEPERATORSIZE_DELAY_SLOT  36
#define SEPERATORSIZE_DELAY_AFTER  20
#define DALAY_CLOCK_OFFSET 8
#define DALAY_CLOCK_OFFSET_DEBUG 8
#define SEPERATORSIZE_DELAYD_DEBUG 60
#define EPILOGSIZE		      12
#define DELAYJUMPSIZE	     32
#endif


#define MININSTRSIZE    3
#define MAXINSTRSIZE	416
#define MAXJUMPSIZE		46

#define SAFEPAGESIZE	MAXBLOCKSIZE - MAXINSTRSIZE - SEPERATORSIZE_DELAY_SLOT - SEPERATORSIZE_DELAY_AFTER - SEPERATORSIZE_NORMAL - EPILOGSIZE
#define MAXINSTRCNT		MAXBLOCKSIZE/(MININSTRSIZE+SEPERATORSIZE_NORMAL)
int instrSize[NUMOFBLOCKS][MAXINSTRCNT];

#define opinit(x)	extern const unsigned short x##_size; \
                    extern const unsigned char x##_src, x##_dest, x##_off1, x##_imm, x##_off3; \
            void x86_##x();


extern "C" {

opinit(CLRT);
opinit(CLRMAC);
opinit(NOP);
opinit(DIV0U);
opinit(SETT);
opinit(SLEEP);
opinit(STS_MACH);
opinit(STS_MACL);
opinit(STS_MACH_DEC);
opinit(STS_MACL_DEC);
opinit(STS_PR);
opinit(STC_SR);
opinit(STC_VBR);
opinit(STC_GBR);
opinit(LDS_PR);
opinit(LDS_MACH);
opinit(LDS_MACL);
opinit(LDS_PR_INC);
opinit(LDS_MACH_INC);
opinit(LDS_MACL_INC);
opinit(LDC_SR);
opinit(LDC_VBR);
opinit(LDCGBR);
opinit(LDC_SR_INC);
opinit(LDC_VBR_INC);
opinit(LDC_GBR_INC);
opinit(STC_SR_MEM);
opinit(STC_VBR_MEM);
opinit(STC_GBR_MEM);
opinit(STSMPR);
opinit(CMP_EQ);
opinit(CMP_HI);
opinit(CMP_GE);
opinit(CMP_HS);
opinit(CMP_GT);
opinit(DT);
opinit(CMP_PL);
opinit(CMP_PZ);
opinit(ROTL);
opinit(ROTR);
opinit(ROTCL);
opinit(ROTCR);
opinit(SHL);
opinit(SHLR);
opinit(SHAR);
opinit(SHLL2);
opinit(SHLR2);
opinit(SHLL8);
opinit(SHLR8);
opinit(SHLL16);
opinit(SHLR16);
opinit(AND);
opinit(OR);
opinit(XOR);
opinit(NOT);
opinit(XTRCT);
opinit(ADD);
opinit(ADDC);
opinit(SUB);
opinit(SUBC);
opinit(NEG);
opinit(NEGC);
opinit(TST);
opinit(TSTI);
opinit(AND_B);
opinit(OR_B);
opinit(TST_B);
opinit(XOR_B);
opinit(ADDI);
opinit(ANDI);
opinit(ORI);
opinit(XORI);
opinit(MOVI);
opinit(CMP_EQ_IMM);
opinit(SWAP_B);
opinit(SWAP_W);
opinit(EXTUB);
opinit(EXTU_W);
opinit(EXTS_B);
opinit(EXTS_W);
opinit(BT)
opinit(BF);
opinit(BF_S);
opinit(JMP);
opinit(JSR);
opinit(BRA);
opinit(BSR);
opinit(BSRF);
opinit(BRAF);
opinit(RTE);
opinit(RTS);
opinit(MOVA);
opinit(MOVT);
opinit(MOVBL);
opinit(MOVWL);
opinit(MOVL_MEM_REG);
opinit(MOVBS);
opinit(MOVWS);
opinit(MOVLS);
opinit(MOVWI);
opinit(MOVLI);
opinit(MOVBS4);
opinit(MOVWS4);
opinit(MOVLS4);
opinit(MOVR);
opinit(MOVBP);
opinit(MOVWP);
opinit(MOVLP);
opinit(MOVBL0);
opinit(MOVWL0);
opinit(MOVLL0);
opinit(MOVBS0);
opinit(MOVWS0);
opinit(MOVLS0);
opinit(MOVBSG);
opinit(MOVLSG);
opinit(MOVWL4);  // 0x8500
opinit(MOVLL4);
opinit(MOVLM);
opinit(TAS);
opinit(ADDV);
opinit(CMPSTR);
opinit(DIV0S);
opinit(DMULS);
opinit(DMULU);
opinit(MULL);
opinit(MULS);
opinit(MULU);
opinit(SUBV);
opinit(MAC_L);
opinit(MOVBM);
opinit(MOVWM);
opinit(MOVBL4);
opinit(MOVWSG);
opinit(MOVBLG);
opinit(MOVWLG);
opinit(MOVLLG);
opinit(TRAPA);
opinit(DIV1);
opinit(MAC_W);


void prologue(void);
void epilogue(void);
void seperator(void);
void seperator_normal(void);
void seperator_delay(void);
void seperator_delay_slot(void);
void seperator_delay_after(void);

void seperator_d_normal(void);
void seperator_d_delay(void);

void PageJump(void); // jumps to a different page
void PageFlip(void); // "flips" the page

#if defined(AARCH64)
void internal_jmp(void);
void internal_delay_jmp(void);
#else
void internal_jmp(void){ printf("nope"); }
void internal_delay_jmp(void){ printf("nope"); }
#endif
int internal_jmp_size = 18*4;
int internal_delay_jmp_size = 15*4;
const intptr_t internal_jmp_to_offset = 4*4;
const intptr_t internal_delay_jmp_to_offset = 4*4;
 
extern x86op_desc asm_list[];

}

x86op_desc asm_list[] =
{
  opdesc(CLRT,0,1,0),       
  opdesc(CLRMAC,0,1,0),
  opdesc(DIV0U,0,1,0),
  opdesc(NOP,0,1,0),
  opdesc(RTE,4,4,0),
  opdesc(RTS,4,2,0),
  opdesc(SETT,0,1,0),
  opdesc(SLEEP,0,8,0),
  opdesc(CMP_PL,0,1,0),
  opdesc(CMP_PZ,0,1,0),
  opdesc(DT,0,1,1),
  opdesc(MOVT,0,1,0),
  opdesc(ROTL,0,1,0),
  opdesc(ROTR,0,1,0),
  opdesc(ROTCL,0,1,0),
  opdesc(ROTCR,0,1,0),
  opdesc(SHL,0,1,0),
  opdesc(SHAR,0,1,0),
  opdesc(SHL,0,1,0),
  opdesc(SHLR,0,1,0),
  opdesc(SHLL2,0,1,0),
  opdesc(SHLR2,0,1,0),
  opdesc(SHLL8,0,1,0),
  opdesc(SHLR8,0,1,0),
  opdesc(SHLL16,0,1,0),
  opdesc(SHLR16,0,1,0),
  opdesc(STC_SR,0xFF,1,1),
  opdesc(STC_GBR,0xFF,1,1),
  opdesc(STC_VBR,0xFF,1,1),
  opdesc(STS_MACH,0xFF,1,0),
  opdesc(STS_MACL,0xFF,1,0),
  opdesc(STS_PR,0xFF,1,0),
  opdesc(TAS,0,4,1),         // 0x401b
  opdesc(STC_SR_MEM,0xFF,2,1),
  opdesc(STC_GBR_MEM,0xFF,2,1),
  opdesc(STC_VBR_MEM,0xFF,2,1),
  opdesc(STS_MACH_DEC,0xFF,2,1),
  opdesc(STS_MACL_DEC,0xFF,2,1),
  opdesc(STSMPR,0xFF,1,1),     // 0x4022
  opdesc(LDC_SR,0xFF,1,0),
  opdesc(LDCGBR,0xFF,1,0),
  opdesc(LDC_VBR,0xFF,1,0),
  opdesc(LDS_MACH,0xFF,1,0),
  opdesc(LDS_MACL,0xFF,1,0),
  opdesc(LDS_PR,0xFF,1,0),
  opdesc(JMP, 3,2,0),
  opdesc(JSR, 4,2,0),
  opdesc(LDC_SR_INC,0xFF,3,0),  
  opdesc(LDC_GBR_INC,0xFF,3,0),
  opdesc(LDC_VBR_INC,0xFF,3,0),
  opdesc(LDS_MACH_INC,0xFF,1,0),
  opdesc(LDS_MACL_INC,0xFF,1,0),
  opdesc(LDS_PR_INC,0xFF,1,0),
  opdesc(BRAF,4,2,0),
  opdesc(BSRF,4,2,0),
  opdesc(ADD,0,1,1),
  opdesc(ADDC,0,1,1),
  opdesc(ADDV,0,1,1),  // 0x300F
  opdesc(AND,0,1,0),
  opdesc(CMP_EQ,0,1,0),
  opdesc(CMP_HS,0,1,0),
  opdesc(CMP_GE,0,1,0),
  opdesc(CMP_HI,0,1,0),
  opdesc(CMP_GT,0,1,0),
  opdesc(CMPSTR,0,1,0), // 0x200C
  opdesc(DIV1,0,1,0),   // 0x3004
  opdesc(DIV0S,0,1,0),  // 0x2007
  opdesc(DMULS,0,4,0),  // 0x300D
  opdesc(DMULU,0,4,0),  // 0x3005
  opdesc(EXTS_B,0,1,0),
  opdesc(EXTS_W,0,1,0),
  opdesc(EXTUB,0,1,0),
  opdesc(EXTU_W,0,1,0),
  opdesc(MOVR,0,1,0),  // 0x6003
  opdesc(MULL,0,4,0),  // 0x0007
  opdesc(MULS,0,3,0),  // 0x200f
  opdesc(MULU,0,3,0),  // 0x200e
  opdesc(NEG,0,1,0),
  opdesc(NEGC,0,1,0),
  opdesc(NOT,0,1,0),
  opdesc(OR,0,1,0),
  opdesc(SUB,0,1,0),
  opdesc(SUBC,0,1,1),
  opdesc(SUBV,0,1,1),  // 0x300B
  opdesc(SWAP_B,0,1,0),
  opdesc(SWAP_W,0,1,0),
  opdesc(TST,0,1,0),
  opdesc(XOR,0,1,0),
  opdesc(XTRCT,0,1,0),
  opdesc(MOVBS,0,1,1),
  opdesc(MOVWS,0,1,1),
  opdesc(MOVLS,0,1,1),
  opdesc(MOVBL,0,1,0), // 6000
  opdesc(MOVWL,0,1,0), // 6001
  opdesc(MOVL_MEM_REG,0,1,0),
  opdesc(MAC_L,0,3,0),  // 0x000f
  opdesc(MAC_W,0,3,0),  // 0x400f
  opdesc(MOVBP,0,1,0),  // 6004
  opdesc(MOVWP,0,1,0),  // 6005
  opdesc(MOVLP,0,1,1),  // 6006
  opdesc(MOVBM,0,1,1),  // 0x2004,
  opdesc(MOVWM,0,1, 0),  // 0x2005,
  opdesc(MOVLM,0,1,1),  // 0x2006
  opdesc(MOVBS0,0,1,1), // 0x0004
  opdesc(MOVWS0,0,1,1), // 0x0005
  opdesc(MOVLS0,0,1,1), // 0x0006
  opdesc(MOVBL0,0,1, 0), // 0x000C
  opdesc(MOVWL0,0,1, 0), // 0x000D 
  opdesc(MOVLL0,0,1, 0), // 0x000E
  opdesc(MOVBL4,0,1,1), // 0x8400
  opdesc(MOVWL4,0,1, 0), // 0x8500
  opdesc(MOVBS4,0,1, 0), // 0x8000
  opdesc(MOVWS4,0,1,1), // 0x8100
  opdesc(MOVLS4,0,1,1), // 0x1000
  opdesc(MOVLL4,0,1, 0), // 0x5000 ,1
  opdesc(MOVBSG,0,1,1), // 0xC000
  opdesc(MOVWSG,0,1,1), // 0xc100
  opdesc(MOVLSG,0,1,1), // 0xC200
  opdesc(MOVBLG,0,1, 0), // 0xC400
  opdesc(MOVWLG,0,1, 0), // 0xC500
  opdesc(MOVLLG,0,1, 0), // 0xC600
  opdesc(MOVA,0,1, 0),
  opdesc(BF,1,3, 0),
  opdesc(BF_S,3,2, 0),
  opdesc(BT,1,3, 0),
  opdesc(BT,3,2, 0),
  opdesc(BRA,2,2, 0),
  opdesc(BSR,2,2, 0),
  opdesc(MOVWI,0,1, 0),
  opdesc(MOVLI,0, 1, 0),
  opdesc(AND_B,0,3,1),
  opdesc(OR_B,0,3,1),
  opdesc(TST_B,0, 1, 0),
  opdesc(XOR_B,0,3,1),
  opdesc(ANDI,0,1, 0),
  opdesc(CMP_EQ_IMM,0,1, 0),
  opdesc(ORI,0,1, 0),
  opdesc(TSTI,0,1, 0),  // C800
  opdesc(XORI,0,1, 0),
  opdesc(TRAPA,5,8,1), // 0xc300
  opdesc(ADDI,0,1, 1),
  opdesc(MOVI,0,1, 0),
  opNULL
}; 

void CompileBlocks::Init()
{
  // ReST-V: allocate the code region once (the original re-allocated on every reset)
  if (dCode == NULL)
    dCode = (Block*)ALLOCATE(sizeof(Block)*NUMOFBLOCKS);
  if (dCode == NULL) {
    fprintf(stderr, "dynarec: code region allocation failed\n");
    abort();
  }
#if defined(RSTV)
  rstv_jit_code_write_begin();
#endif
  memset((void*)dCode, 0, sizeof(Block)*NUMOFBLOCKS);

  memset(LookupTable, 0, sizeof(LookupTable));
  rstv_link_gen++;
  memset(LookupTableRom, 0, sizeof(LookupTableRom));
  memset(LookupTableLow, 0, sizeof(LookupTableLow));
  memset(LookupTableC, 0, sizeof(LookupTableC));

  blockCount = 0;
  LastMakeBlock = 0;

  g_CompleBlock = dCode;
  for (int i = 0; i < NUMOFBLOCKS; i++ ) {
    g_CompleBlock[i].id = i;
  }
#if defined(RSTV)
  rstv_jit_code_write_end(dCode, sizeof(Block)*NUMOFBLOCKS);
#endif
  return;
}

int CompileBlocks::opcodeIndex(u16 op)
{
  /*register*/ int i = 0;
  while (((op & opcode_list[i].mask) != opcode_list[i].bits) && opcode_list[i].mnem != 0) i++;
  return i;
}

void CompileBlocks::FindOpCode(u16 opcode, u8 * instindex)
{
  *instindex = opcodeIndex(opcode);
  return;
}

void CompileBlocks::BuildInstructionList()
{
  u32 optest;
  memset(dsh2_instructions, 0, sizeof(u8)*MAX_INSTSIZE);
  for (optest = 0; optest <= 0xFFFF; optest++) {
    FindOpCode(optest, &dsh2_instructions[optest]);
  }
}

#if defined(RSTV)
// ReST-V (ADR-017): 32-byte chunks of high work RAM that hold compiled code. Stores to these go through
// the C memSet* functions (setDirty); the asm fast path (rstv_memfast_arm.s) handles the rest.
extern "C" {
  u8 rstv_code_chunk[0x100000 >> 5];
  int rstv_memfast = 1;
  int rstv_chain_on = 1; // ADR-018 block chaining
  unsigned rstv_dyn_chain;
  void rstv_chain(void); void rstv_chain_off(void);
  u8 rstv_memGetByte(u32); u16 rstv_memGetWord(u32); u32 rstv_memGetLong(u32);
  void rstv_memSetByte(u32, u8); void rstv_memSetWord(u32, u16); void rstv_memSetLong(u32, u32);
}
#define RSTV_MARK_CODE(a) (rstv_code_chunk[((a) & 0xFFFFF) >> 5] = 1)
// ADR-081: native-call blocks. A hook PC whose native routine has run once gets a block that calls rstv_hle_stub_c and goes
// on through rstv_chain, so chained calls reach the routine without the dispatcher (two Execute round trips a call).
// If the routine refuses, the block returns to Execute with rstv_hle_bypass_pc set, which runs the original code from a
// block kept outside LookupTable.
#define BLOCK_HLESTUB 0x08
extern "C" { int rstv_hle_stub_on = 1; u32 rstv_hle_bypass_pc; unsigned rstv_hle_stub_calls; int rstv_hle_stub_c(void *); }
static int rstv_emit_nostub;
extern "C" { unsigned rstv_hle_items[8]; } // performance line: vertices (k0), polygons (k3), nested dispatches (6), code rechecks (7)
extern "C" int rstv_hle_on;
// ADR-048: block exits jump straight to the next block through a per-exit stub (inline cache: exit PC, target, generation)
extern "C" { u32 rstv_link_gen = 1; int rstv_link_on = 0; /* off: see ADR-048 (ux0:data/restv/link turns it on) */ unsigned rstv_link_stubs, rstv_link_skip; void rstv_chain_ic(void); void rstv_macl_helper(void); }
#define RSTV_STUB_WORDS 33
static int rstv_link_stubs_fit(const u8 *start, const u8 *end, int room)
{
  int n = 0;
  for (const u32 *w = (const u32 *)start; w < (const u32 *)end; w++)
    if (*w == 0xE597F084u) n++;
  return n * RSTV_STUB_WORDS * 4 <= room ? n : -1;
}
// replace every "ldr pc, [r7, #132]" exit in [start, ptr) by a branch to a stub appended at ptr; returns the new end
static u8 *rstv_link_emit(u8 *start, u8 *ptr)
{
  u32 *end = (u32 *)ptr;
  for (u32 *w = (u32 *)start; w < (u32 *)ptr; w++) {
    if (*w != 0xE597F084u)
      continue;
    u32 *st = end;
    static const u32 tmpl[RSTV_STUB_WORDS - 4] = {
      0xE5970088, 0xE5901000, 0xE3510000, 0x10899001, 0x13A02000, 0x15802000, 0x1587905C, // memory wait cycles (and COUNT, as rstv_chain)
      0xE5972080, 0xE1590002, 0x2A000010,                                     // r9 >= exitcount -> slow
      0xE5970040, 0xE20000F0, 0xE5971060, 0xE1500001, 0x3A00000B,             // interrupt pending -> slow
      0xE59F0030, 0xE5900000, 0xE59F102C, 0xE1500001, 0x1A000006,             // link generation changed -> slow
      0xE5978058, 0xE59F1020, 0xE1510008, 0x1A000002,                         // r8 = PC; other PC -> slow
      0xE59F2018, 0xE5872090, 0xE282F010,                                     // curblock = target; jump past its prologue
      0xE28F3000, 0xE597F098 };                                               // slow: r3 = stub data; rstv_chain_ic
    memcpy(st, tmpl, sizeof(tmpl));
    st[29] = (u32)(uintptr_t)&rstv_link_gen;
    st[30] = 0; st[31] = 0xFFFFFFFFu; st[32] = 0;
    *w = 0xEA000000u | ((u32)(((st - w) - 2)) & 0x00FFFFFFu); // b stub
    end += RSTV_STUB_WORDS;
    rstv_link_stubs++;
  }
  return (u8 *)end;
}
static u8 rstv_hle_bits[0x100000 >> 4]; // ADR-045/047: high work RAM PCs run natively (one bit per instruction)
// ReST-V: counters for the performance line (block dispatches, compiles)
extern "C" { unsigned rstv_dyn_cyc[2]; extern volatile int rstv_act; }
// sampler labels for the compiler (ADR-043 measurement): 25 fetch+parent, 26 emit, 27 compile other, 28 sync
#define RSTV_ACT(x) (rstv_act = (x))
extern "C" { unsigned rstv_dyn_exec, rstv_dyn_compile; extern volatile unsigned rstv_act_pc; unsigned rstv_comp_us[3]; }
extern "C" { unsigned rstv_dyn_blk_bytes, rstv_dyn_blk_n, rstv_dyn_blk_max; }
static unsigned rstv_dyn_blk_last; // size of the block just emitted (ADR-036: sync only the written bytes)
extern "C" { void PageFlipCont(void); int rstv_dyn_cont = 1; unsigned rstv_dyn_cont_n; }
#define PAGEFLIPCONT_SIZE 20
// ReST-V (ADR-026): "rotcl Rq; div1 Rm,Rn" x32 (GCC/Hitachi software division) as one C call.
// R = GenReg (SR at R[16]: T bit 0, Q bit 8, M bit 9); same result as the 64 instructions one by one.
extern "C" { int rstv_dyn_div32 = 1; unsigned rstv_dyn_div32_n; }
// ReST-V (ADR-028): SGL fixed-point divide (r4 << 16) / r5 through the on-chip divider:
//   mov.l @(disp,pc),r0 (= 0xFFFFFF00); swap.w r4,r2; mov.l r5,@r0; exts.w r2,r2; mov.l r2,@(16,r0); shll16 r4; mov.l r4,@(20,r0)
// as one call (same register and divider state; the following rts / delay-slot read stay normal instructions)
extern "C" { int rstv_dyn_divfx = 1; unsigned rstv_dyn_divfx_n; }
// ReST-V (ADR-030): lazy PC / cycle update. Plain instructions do not get the "add r8,#2; add r9,#n" separator;
// the sums are added before an instruction that reads r8 (MOVA, MOV.W/L @(disp,PC), SLEEP), before branches and at the block end.
extern "C" { int rstv_dyn_lazy = 1; }
extern "C" void FASTCALL OnchipWriteLong(u32 addr, u32 val);
extern "C" void rstv_divfx(u32 *R)
{
  const u32 r4 = R[4];
  R[0] = 0xFFFFFF00;
  OnchipWriteLong(0x100, R[5]);
  R[2] = (u32)(s32)(s16)(r4 >> 16);
  OnchipWriteLong(0x110, R[2]);
  R[4] = r4 << 16;
  OnchipWriteLong(0x114, R[4]);
}
extern "C" void rstv_div1x32(u32 *R, u32 packed)
{
  const int q = (packed >> 8) & 15, m = (packed >> 4) & 15, n = packed & 15;
  u32 sr = R[16];
  u32 T = sr & 1, Q = (sr >> 8) & 1;
  const u32 M = (sr >> 9) & 1;
  if (q != n && q != m && m != n) {
    // no aliasing: keep the three registers in locals (same steps as below)
    u32 rq = R[q], rn = R[n];
    const u32 rm = R[m];
    for (int k = 0; k < 32; k++) {
      const u32 tq = rq >> 31;
      rq = (rq << 1) | T;
      const u32 oldq = Q;
      Q = rn >> 31;
      rn = (rn << 1) | tq;
      u32 tmp1;
      if (oldq == M) {
        const u32 t0 = rn;
        rn -= rm;
        tmp1 = rn > t0;
      } else {
        const u32 t0 = rn;
        rn += rm;
        tmp1 = rn < t0;
      }
      Q ^= tmp1 ^ M;
      T = (Q == M);
    }
    R[q] = rq;
    R[n] = rn;
    R[16] = (sr & ~0x301u) | T | (Q << 8) | (M << 9);
    return;
  }
  for (int k = 0; k < 32; k++) {
    // rotcl Rq
    const u32 rq = R[q];
    R[q] = (rq << 1) | T;
    T = rq >> 31;
    // div1 Rm, Rn
    const u32 oldq = Q;
    u32 rn = R[n];
    Q = rn >> 31;
    rn = (rn << 1) | T;
    const u32 rm = R[m];
    u32 tmp1;
    if (oldq == M) {
      const u32 t0 = rn;
      rn -= rm;
      tmp1 = rn > t0;
    } else {
      const u32 t0 = rn;
      rn += rm;
      tmp1 = rn < t0;
    }
    R[n] = rn;
    Q ^= tmp1 ^ M;
    T = (Q == M);
  }
  R[16] = (sr & ~0x301u) | T | (Q << 8) | (M << 9);
}
extern "C" { u32 rstv_strace[1024]; unsigned rstv_strace_n; int rstv_strace_on; int rstv_strace_hit; u32 rstv_strace_regs[17], rstv_strace_mem[33]; }
#endif
#if defined(RSTV)
// blocks currently running on the (up to two) nested dynarec contexts
static Block *rstv_running[4];
static int rstv_running_n;
static int rstv_block_in_use(Block *b) {
  for (int i = 0; i < rstv_running_n; i++)
    if (rstv_running[i] == b) return 1;
  return 0;
}
extern "C" { unsigned rstv_nested_us, rstv_nested_n; } // performance line: input-capture runs inside the other CPU
extern "C" void SH2DynExecNested(SH2_struct *context, u32 cycles) {
  DynarecSh2 *pctx = (DynarecSh2 *)context->ext;
  DynarecSh2 *prev = DynarecSh2::CurrentContext;
  if (prev && rstv_running_n < 4)
    rstv_running[rstv_running_n++] = (Block *)prev->getDynaSh()->curblock;
  pctx->SetCurrentContext();
#if !defined(__vita__)
  static int qlog = -1; if (qlog < 0) qlog = getenv("RSTV_QLOG") != NULL;
  const u32 pc0 = pctx->getDynaSh()->SysReg[3];
#endif
  const int act_saved = rstv_act;
  const u32 act_pc_saved = rstv_act_pc;
  rstv_act = context->isslave ? 2 : 1; // sampler: the nested run belongs to the woken CPU
  const u64 nt0 = RSTV_PROF_TF(); // fine only: 200 a frame in 3D scenes (ADR-075)
  pctx->ExecuteNested(cycles);
  rstv_nested_us += (unsigned)(RSTV_PROF_TF() - nt0);
  rstv_nested_n++;
  rstv_act = act_saved;
  rstv_act_pc = act_pc_saved;
#if !defined(__vita__)
  if (qlog) printf("Q nested %s pc %08x -> %08x sr %08x\n", context->isslave ? "S" : "M", pc0, pctx->getDynaSh()->SysReg[3], pctx->getDynaSh()->CtrlReg[0]);
#endif
  if (prev) {
    prev->SetCurrentContext();
    rstv_running_n--;
  }
}
#endif
Block * CompileBlocks::CompileBlock(u32 pc, addrs * ParentT = NULL)
{
  compile_count_++;
#if defined(RSTV)
  rstv_dyn_compile++;
#if !defined(__vita__)
  {
    // harness: RSTV_COMPLOG=<from frame> logs compiled block addresses (recompile diagnosis)
    static int on = -1;
    if (on < 0) { const char *e = getenv("RSTV_COMPLOG"); on = e ? atoi(e) : 0; }
    extern u32 rstv_harness_frame;
    if (on && rstv_harness_frame >= (u32)on && rstv_harness_frame < (u32)on + 120)
      fprintf(stderr, "comp f%u %08x\n", rstv_harness_frame, pc);
  }
#endif
#endif

  auto block_index = self_modify_block.find(pc);
  if( block_index != self_modify_block.end()  ){
      blockCount = block_index->second ;
      self_modify_block.erase(pc);
      LOG( "hit! last_modified_block = %d, last_modified_pc = %08X",blockCount, pc );
  }else{
    blockCount = LastMakeBlock;
    LastMakeBlock++;
    LastMakeBlock &= (NUMOFBLOCKS-1);
#if defined(RSTV)
    if (rstv_block_in_use(&g_CompleBlock[blockCount])) {
      blockCount = LastMakeBlock;
      LastMakeBlock++;
      LastMakeBlock &= (NUMOFBLOCKS-1);
    }
#endif
  }
  
  if (g_CompleBlock[blockCount].b_addr != 0x00) {
#if defined(RSTV)
    rstv_link_gen++; // ADR-048: this block is replaced
#endif
    
    if ((g_CompleBlock[blockCount].b_addr & 0xFF000000) == 0xC0000000) {
      LookupTableC[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
    }
    else {
      switch (g_CompleBlock[blockCount].b_addr & 0x0FF00000) {
      case 0x00000000:
        if (yabsys.emulatebios) {
          //return NULL; do nothing
        }
        else {
          LookupTableRom[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
        }
        break;
      case 0x00200000:
        LookupTableLow[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
        break;
      case 0x06000000:
        /*case 0x06100000:*/
        LookupTable[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
        //LOG("%d, %08X is removed due to overflow", blockCount, g_CompleBlock[blockCount].b_addr);
        break;
      default:
        break;
      }
    }
  }

#if defined(RSTV)
  // the Block header and its code live in the executable (VM) region
  const int rstv_act_saved = rstv_act;
  RSTV_ACT(27);
  const u64 rt0 = RSTV_PROF_T();
  rstv_jit_code_write_begin();
  const u64 rt1 = RSTV_PROF_T();
#endif
  g_CompleBlock[blockCount].b_addr = pc;


  //LOG("%d,%08X is compiled",blockCount,pc );
  int emit_rc = EmmitCode(&g_CompleBlock[blockCount], ParentT);
#if defined(RSTV)
  const u64 rt2 = RSTV_PROF_T();
  RSTV_ACT(28);
  // ADR-036: make only the written code and the header coherent (was the whole 4KB block: about 50us per block)
  if (emit_rc == 0 && rstv_dyn_blk_last > 0 && rstv_dyn_blk_last <= MAXBLOCKSIZE) {
    // the header (b_addr..flags) is data read by C code, not executed: no I-cache sync needed
    rstv_jit_code_write_end(g_CompleBlock[blockCount].code, rstv_dyn_blk_last);
  } else
    rstv_jit_code_write_end(&g_CompleBlock[blockCount], sizeof(Block));
  const u64 rt3 = RSTV_PROF_T();
  rstv_comp_us[0] += (unsigned)(rt1 - rt0);
  rstv_comp_us[1] += (unsigned)(rt2 - rt1);
  rstv_comp_us[2] += (unsigned)(rt3 - rt2);
  RSTV_ACT(rstv_act_saved);
#endif
  if (emit_rc != 0) {
    return NULL;
  }

  return &g_CompleBlock[blockCount];
}

void CompileBlocks::ShowStatics() {
  //LOG("Compile\t%d\t%d\t%d\n", compile_count_, exec_count_, remove_count_);
  compile_count_ = 0;
  exec_count_ = 0;
  remove_count_ = 0;
}

// memo DirectMemoryAccess
// MOVLI,MOVWI


void CompileBlocks::opcodePass(x86op_desc *op, u16 opcode, u8 *ptr)
{
#if defined(AARCH64)
  // multiply source and dest regions by 4 (size of register) 

  if (*(op->src) != 0xFF) // C
  {
    *(u32*)(ptr + *(op->src)) |= ((u32)((opcode >> 4) & 0xf) << (2+5));
  }

  if (*(op->dest) != 0xFF) // B
  {
    *(u32*)(ptr + *(op->dest)) |= ((u32)((opcode >> 8) & 0xf) << (2+5)) ;
  }

  if (*(op->off1) != 0xFF){
    *(u32*)(ptr + *(op->off1)) |= ((u32)(opcode & 0xf) << 5) ;
  }

  if (*(op->imm) != 0xFF){
    *(u32*)(ptr + *(op->imm)) |= ((u32)opcode & 0xff)<<5;
  }

  if (*(op->off3) != 0xFF) {
    *(u32*)(ptr + *(op->off3)) |= ((u32)opcode & 0xfff) << 5;
  }

#else

#if defined(RSTV)
  // ADR-032: templates address SH-2 registers as [r7, #0x400] (R[m]) / [r7, #0x800] (R[n]) in any number of places
  {
    u32 *w = (u32 *)ptr;
    const int nw = *(op->size) / 4;
    for (int k = 0; k < nw; k++) {
      const u32 x = w[k];
      if ((x & 0x0E0F0000) == 0x04070000) { // LDR/STR(B) Rt, [r7, #imm12]
        const u32 imm = x & 0xFFF;
        if (imm == 0x400)
          w[k] = (x & ~0xFFFu) | (((opcode >> 4) & 0xf) << 2);
        else if (imm == 0x800)
          w[k] = (x & ~0xFFFu) | (((opcode >> 8) & 0xf) << 2);
      }
    }
  }
#endif

  if (*(op->src) != 0xFF) // C
    *(ptr + *(op->src)) = (u8)(((opcode >> 4) & 0xf) << 2);

  if (*(op->dest) != 0xFF) // B
    *(ptr + *(op->dest)) = (u8)(((opcode >> 8) & 0xf) << 2);

  if (*(op->off1) != 0xFF)
    *(ptr + *(op->off1)) = (u8)(opcode & 0xf);

  if (*(op->imm) != 0xFF)
    *(ptr + *(op->imm)) = (u8)(opcode & 0xff);

#if _WINDOWS
  if (*(op->off3) != 0xFF)
    *(u16*)(ptr + *(op->off3)) = (u16)(opcode & 0xfff);
#else  
  if (*(op->off3) != 0xFF) {
    *(ptr + *(op->off3)) = (u8)((opcode >> 8) & 0x0f);
    *(ptr + *(op->off3) + 4) = (u8)(opcode & 0xff);
}
#endif  

#endif
}

#define Y_MAX(a, b) ((a) > (b) ? (a) : (b))
#define Y_MIN(a, b) ((a) < (b) ? (a) : (b))

int CompileBlocks::EmmitCode(Block *page, addrs * ParentT )
{
  int i, j, jmp = 0, count = 0;
  u16 op, temp;
  u32 addr = page->b_addr;
  u32 start_addr = page->b_addr;
  u8 *ptr, *startptr;
  u32 instruction_counter = 0;
  u32 write_memory_counter = 0;
  u32 calsize;
  std::unordered_map<u32, uintptr_t> addr_map;
  int rstv_continued = 0; // ReST-V (ADR-024): a BT/BF in this block did not end it
  u32 pend_pc = 0, pend_cyc = 0; // ReST-V (ADR-030): PC / cycle increments not emitted yet

#if defined(RSTV) && !defined(AARCH64)
  // ADR-043: build the block in ordinary (cached) memory and copy it to the code region once at the end;
  // the templates are patched in place (read-modify-write), which was slow on the JIT region
  static u8 rstv_emit_buf[MAXBLOCKSIZE] __attribute__((aligned(16)));
  startptr = ptr = rstv_emit_buf;
#else
  startptr = ptr = page->code;
#endif
#if defined(RSTV) && !defined(AARCH64)
  auto rstv_flush = [&]() {
    if (!pend_pc && !pend_cyc)
      return;
    u32 *w = (u32 *)ptr;
    int k = 0;
    if (pend_pc & 0xFF) w[k++] = 0xE2888000 | (pend_pc & 0xFF);          // add r8, r8, #lo
    if (pend_pc >> 8) w[k++] = 0xE2888C00 | ((pend_pc >> 8) & 0xFF);     // add r8, r8, #hi << 8
    if (pend_cyc & 0xFF) w[k++] = 0xE2899000 | (pend_cyc & 0xFF);
    if (pend_cyc >> 8) w[k++] = 0xE2899C00 | ((pend_cyc >> 8) & 0xFF);
    ptr += k * 4;
    pend_pc = pend_cyc = 0;
  };
#endif

  i = 0;
  j = 0;
  count = 0;  
#if !defined(RSTV) || defined(AARCH64)
  memset((void*)ptr,0,sizeof(char)*MAXBLOCKSIZE); // ReST-V ARM32: templates are copied whole, no need (ADR-036)
#endif
  memcpy((void*)ptr, (void*)prologue, PROLOGSIZE);
  ptr += PROLOGSIZE;
  int MaxSize = 0;

  void * nomal_seperator;
  u32 nomal_seperator_size;
  u8 nomal_seperator_counter_offset;
  void * delay_seperator;
  u32 delay_seperator_size;
  u8 delayslot_seperator_counter_offset;

  if (debug_mode_) {
    nomal_seperator = (void*)seperator_d_normal;
    nomal_seperator_size = SEPERATORSIZE_DEBUG;
    nomal_seperator_counter_offset = NORMAL_CLOCK_OFFSET_DEBUG;
    delay_seperator = (void*)seperator_d_delay;
    delay_seperator_size = SEPERATORSIZE_DELAYD_DEBUG;
    delayslot_seperator_counter_offset = DALAY_CLOCK_OFFSET_DEBUG;
  }
  else {
    nomal_seperator = (void*)seperator_normal;
    nomal_seperator_size = SEPERATORSIZE_NORMAL;
    nomal_seperator_counter_offset = NORMAL_CLOCK_OFFSET;
    delay_seperator = (void*)seperator_delay_slot;
    delay_seperator_size = SEPERATORSIZE_DELAY_SLOT;
    delayslot_seperator_counter_offset = DALAY_CLOCK_OFFSET;
  }
  
  page->flags = 0;
#if defined(RSTV)
  {
    for (int w = 0; w < 2; w++)
      if (rstv_idle_hint_n[w] || ((rstv_idle_auto >> w) & 5))
        rstv_idle_hint_at(w, addr); // ADR-106: MAME pcflush hint -> loop head
  }
  for (int w = 0; w < 2; w++)
    for (int k = 0; k < rstv_idle_n[w]; k++)
      if (rstv_idle_pc[w][k] == addr)
        page->flags |= BLOCK_NOCHAIN;
  if (rstv_once_fn && addr == rstv_once_fn)
    page->flags |= BLOCK_NOCHAIN;
  for (int k = 0; k < rstv_hle_n; k++)
    if (rstv_hle[k].pc == addr)
      page->flags |= BLOCK_NOCHAIN;
#endif
#if defined(RSTV) && !defined(AARCH64)
  if (rstv_hle_stub_on && !rstv_emit_nostub && !debug_mode_ && (addr & 0x0FF00000) == 0x06000000) {
    int hk = -1;
    for (int k = 0; k < rstv_hle_n; k++)
      if (rstv_hle[k].pc == addr) {
        hk = k;
        break;
      }
    if (hk >= 0) {
      // ADR-081: str r9,[r7,#COUNT]; mov r0,r7; movw/movt r10,fn; blx r10; ldr r9,[r7,#COUNT]; cmp r0,#0;
      // ldrne pc,[r7,#132] (rstv_chain); pop {r4-r10,pc} (back to Execute: the original code runs)
      const u32 fn = (u32)(uintptr_t)rstv_hle_stub_c;
      u32 *w = (u32 *)ptr;
      w[0] = 0xE587905C;
      w[1] = 0xE1A00007;
      w[2] = 0xE300A000 | (((fn >> 12) & 15) << 16) | (fn & 0xFFF);
      w[3] = 0xE340A000 | (((fn >> 28) & 15) << 16) | ((fn >> 16) & 0xFFF);
      w[4] = 0xE12FFF3A;
      w[5] = 0xE597905C;
      w[6] = 0xE3500000;
      w[7] = 0x1597F084;
      w[8] = 0xE8BD87F0;
      ptr += 9 * 4;
      page->flags = BLOCK_HLESTUB;
      page->e_addr = addr;
#ifdef SET_DIRTY
      if (ParentT) {
        addrs &pl = ParentT[adress_mask(addr)];
        const u32 sa = adress_mask(start_addr);
        if (pl.empty() || pl.back() != sa) pl.push_back(sa);
        RSTV_MARK_CODE(addr);
      }
#endif
      const unsigned sz = (unsigned)(ptr - startptr);
      rstv_dyn_blk_last = sz;
      memcpy(page->code, startptr, sz);
      return 0;
    }
  }
#endif
  
#ifdef BUILD_INFO  
  if( show_code_ ) LOG("*********** start block %08X *************\n", addr );
#endif
  //LOG("Compile %08X\n", addr );
  //MaxSize = MAXBLOCKSIZE - MAXINSTRSIZE- delay_seperator_size - SEPERATORSIZE_DELAY_AFTER - nomal_seperator_size - EPILOGSIZE;
  //while (ptr - startptr < MaxSize) {
  while (1) {
    // translate the opcode and insert code
#if defined(RSTV)
    RSTV_ACT(25);
    if (addr != start_addr && rstv_hle_on && (rstv_hle_bits[((addr & 0xFFFFF) >> 1) >> 3] >> (((addr & 0xFFFFF) >> 1) & 7) & 1) &&
        (addr & 0x0FF00000) == 0x06000000)
      break; // ADR-047: a native routine starts here: end the block so the dispatcher sees this PC
#endif
    op = MappedMemoryReadWord(addr, NULL);
#ifdef SET_DIRTY
    if (ParentT) {
      u32 keepaddr = adress_mask(addr);
      { addrs &pl = ParentT[keepaddr]; const u32 sa = adress_mask(start_addr);
        if (pl.empty() || pl.back() != sa) pl.push_back(sa); } // ReST-V: same as push_back + unique, no O(n) pass
      RSTV_MARK_CODE(addr);
    }
#endif

#if defined(AARCH64)
    addr_map[addr] = (uintptr_t)ptr; // only the AArch64 internal jumps use it
#endif
#if defined(RSTV)
    RSTV_ACT(26);
#endif

    i = dsh2_instructions[op];
    if (asm_list[i].func == 0) {
      LOG("bad instruction code @ PC=%08X code=%04X\n", addr, op );
      return -1;  // bad instruction code
    }

    // CheckSize
    u8 delay = asm_list[i].delay;
#if defined(AARCH64)
    if ( delay == 0 || delay == 0xFF) {
      calsize  = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + EPILOGSIZE;
    }else if(delay == 1 || delay == 5) {
      calsize = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + Y_MAX(internal_jmp_size,DELAYJUMPSIZE) + EPILOGSIZE;
    } else {
      u32 op2 = memGetWord(addr+2);
      u32 delayop = dsh2_instructions[op2];
      calsize = (ptr - startptr) + *asm_list[i].size + *asm_list[delayop].size + 
      delay_seperator_size + Y_MAX(internal_delay_jmp_size,SEPERATORSIZE_DELAY_AFTER) + EPILOGSIZE;
    }
#else    
    if ( delay == 0 || delay == 0xFF) {
      calsize  = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + EPILOGSIZE;
    }else if(delay == 1 || delay == 5) {
      calsize = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + DELAYJUMPSIZE + EPILOGSIZE;
    } else {
      u32 op2 = MappedMemoryReadWord(addr+2,NULL);
      u32 delayop = dsh2_instructions[op2];
      calsize = (ptr - startptr) + *asm_list[i].size + *asm_list[delayop].size + delay_seperator_size + SEPERATORSIZE_DELAY_AFTER + EPILOGSIZE;
    }
#endif
    if (calsize + 16 >= MAXBLOCKSIZE) { // ReST-V: room for a lazy PC/cycle flush (ADR-030)
      break; // no space is available
    }
#if defined(RSTV) && !defined(AARCH64)
    // ADR-028: on-chip fixed-point divide sequence -> one call
    if (rstv_dyn_divfx && !debug_mode_ && (op & 0xFF00) == 0xD000 && calsize + 64 < MAXBLOCKSIZE) {
      static const u16 pat[6] = { 0x6249, 0x2052, 0x622F, 0x1024, 0x4428, 0x1045 };
      const u32 lit = ((addr & ~3u) + 4 + (op & 0xFF) * 4);
      int ok = MappedMemoryReadLong(lit, NULL) == 0xFFFFFF00;
      for (int k = 0; ok && k < 6; k++)
        ok = MappedMemoryReadWord(addr + 2 + k * 2, NULL) == pat[k];
      if (ok) {
        u32 cyc = asm_list[dsh2_instructions[op]].cycle;
        for (int k = 0; k < 6; k++)
          cyc += asm_list[dsh2_instructions[pat[k]]].cycle;
        const u32 fn = (u32)(uintptr_t)rstv_divfx;
        u32 *w = (u32 *)ptr;
        w[0] = 0xE1A00007;                                                  // mov r0, r7
        w[1] = 0xE300A000 | (((fn >> 12) & 15) << 16) | (fn & 0xFFF);       // movw r10, #lo
        w[2] = 0xE340A000 | (((fn >> 28) & 15) << 16) | ((fn >> 16) & 0xFFF); // movt r10, #hi
        w[3] = 0xE12FFF3A;                                                  // blx r10
        instrSize[blockCount][count++] = 4 * 4;
        ptr += 4 * 4;
        pend_pc += 14;
        pend_cyc += cyc;
        if (!rstv_dyn_lazy)
          rstv_flush();
        addr += 2;
        for (int k = 1; k < 7; k++) {
#ifdef SET_DIRTY
          if (ParentT) {
            u32 keepaddr = adress_mask(addr);
            { addrs &pl = ParentT[keepaddr]; const u32 sa = adress_mask(start_addr);
              if (pl.empty() || pl.back() != sa) pl.push_back(sa); } // ReST-V: same as push_back + unique, no O(n) pass
            RSTV_MARK_CODE(addr);
          }
#endif
          addr += 2;
        }
        instruction_counter += 6;
        write_memory_counter += 3;
        rstv_dyn_divfx_n++;
        continue;
      }
    }
    // ADR-026: rotcl Rq + (div1 Rm,Rn; rotcl Rq) ... = 32 pairs -> one call
    if (rstv_dyn_div32 && !debug_mode_ && (op & 0xF0FF) == 0x4024 && calsize + 64 < MAXBLOCKSIZE) {
      // addr = this rotcl (not advanced yet)
      const u16 d1 = MappedMemoryReadWord(addr + 2, NULL);
      int ok = (d1 & 0xF00F) == 0x3004;
      for (int k = 2; ok && k < 64; k++)
        ok = MappedMemoryReadWord(addr + k * 2, NULL) == ((k & 1) ? d1 : op);
      if (ok) {
        const u32 packed = (((op >> 8) & 15) << 8) | (((d1 >> 4) & 15) << 4) | ((d1 >> 8) & 15);
        const u32 fn = (u32)(uintptr_t)rstv_div1x32;
        u32 *w = (u32 *)ptr;
        w[0] = 0xE1A00007;                                                  // mov r0, r7
        w[1] = 0xE3001000 | ((packed >> 12) << 16) | (packed & 0xFFF);      // movw r1, #packed
        w[2] = 0xE300A000 | (((fn >> 12) & 15) << 16) | (fn & 0xFFF);       // movw r10, #lo
        w[3] = 0xE340A000 | (((fn >> 28) & 15) << 16) | ((fn >> 16) & 0xFFF); // movt r10, #hi
        w[4] = 0xE12FFF3A;                                                  // blx r10
        instrSize[blockCount][count++] = 5 * 4;
        ptr += 5 * 4;
        pend_pc += 128;
        pend_cyc += 64;
        if (!rstv_dyn_lazy)
          rstv_flush();
        addr += 2;
        for (int k = 1; k < 64; k++) {
#ifdef SET_DIRTY
          if (ParentT) {
            u32 keepaddr = adress_mask(addr);
            { addrs &pl = ParentT[keepaddr]; const u32 sa = adress_mask(start_addr);
              if (pl.empty() || pl.back() != sa) pl.push_back(sa); } // ReST-V: same as push_back + unique, no O(n) pass
            RSTV_MARK_CODE(addr);
          }
#endif
          addr += 2;
        }
        instruction_counter += 63;
        rstv_dyn_div32_n++;
        continue;
      }
    }
#endif

    if (0x1b == op) { // SLEEP
      if (rstv_continued)
        break; // ReST-V: a continued block must not get BLOCK_LOOP (ADR-024); SLEEP starts its own block
      page->flags |= BLOCK_LOOP;
    }

    // Inifinity Loop Detection
    if (count == 0 && (op & 0xF00F) == 0x6000) { // mov ? R0
      u32 loopcheck = memGetLong(addr + 2);
      if ((loopcheck & 0xFF00FFFF) == 0xC80089FC) { // test, bf
        page->flags |= BLOCK_LOOP;
      }
      if( (loopcheck&0xF00FFFFF) == 0x20088DFC ){ // slave waits intrrupt capture
          page->flags |= BLOCK_LOOP;
      }
    }

//#ifdef BUILD_INFO  
//    LOG("compiling %08X, 0x%04X @ 0x%08X\n", startptr, op, addr);
//#endif    

    addr += 2;

#ifdef BUILD_INFO
    if(show_code_) DumpInstX( i, addr-2, op  );
#endif

    instruction_counter++;
    asm_list[i].build_count++;
    write_memory_counter += asm_list[i].write_count;

    u32 jumppc = 0xFFFFFFFF;
    uintptr_t jumpptr = 0xFFFFFFFF;
    if (asm_list[i].delay != 0xFF && asm_list[i].delay != 0x00) {
      if (asm_list[i].delay == 1) {
        jumppc = addr + ((signed char)(op & 0xff) << 1) + 2;
      }
      else if (asm_list[i].delay == 2) {
        temp = (op & 0xfff) << 1;
        if (temp & 0x1000)
          temp |= 0xfffff000;
        jumppc = addr + ((signed)(op & 0xfff) << 1) + 2;
      }
      else if (asm_list[i].delay == 3) {
        jumppc = addr + ((signed char)(op & 0xff) << 1) + 2;
      }
#if defined(AARCH64)
      if ( addr_map.count(jumppc) != 0 && write_memory_counter != 0 ) {
        jumpptr = addr_map[jumppc];
        //LOG("jumpptr = %lx\n", jumpptr );
      }
#endif      
    }


#if defined(RSTV) && !defined(AARCH64)
    // ADR-030: instructions that read r8 / branches see the exact PC and cycle count
    const int rstv_reads_pc = (op & 0xFF00) == 0xC700 || (op & 0xF000) == 0x9000 || (op & 0xF000) == 0xD000 || op == 0x001B;
    if (asm_list[i].delay != 0 || rstv_reads_pc || !rstv_dyn_lazy || debug_mode_)
      rstv_flush();
    if (asm_list[i].delay == 0 && !rstv_reads_pc && rstv_dyn_lazy && !debug_mode_) {
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      instrSize[blockCount][count++] = *(asm_list[i].size);
      opcodePass(&asm_list[i], op, ptr);
      ptr += *(asm_list[i].size);
      pend_pc += 2;
      pend_cyc += asm_list[i].cycle;
    } else
#endif
    // Regular Opcode ( No Delay Branch )
    if (asm_list[i].delay == 0) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      instrSize[blockCount][count++] = *(asm_list[i].size) + nomal_seperator_size;
      opcodePass(&asm_list[i], op, ptr);
#if defined(AARCH64)
      u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
      *counterpos |= ((asm_list[i].cycle)<<10);
#else
      u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
      *counterpos = asm_list[i].cycle;
#endif

      ptr += *(asm_list[i].size) + nomal_seperator_size;
    }

    // No Intrupt Func ToDo: Never end block these functions
    else if (asm_list[i].delay == 0xFF ) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      instrSize[blockCount][count++] = *(asm_list[i].size) + nomal_seperator_size;
      opcodePass(&asm_list[i], op, ptr);
      ptr += *(asm_list[i].size) + nomal_seperator_size;
    }

    // Normal Jump
    else if (asm_list[i].delay == 1 || asm_list[i].delay == 5 ) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      if (jumpptr != 0xFFFFFFFF ) {
        intptr_t offset = *(asm_list[i].size) + nomal_seperator_size;
        memcpy((void*)(ptr + offset), (void*)internal_jmp, internal_jmp_size);
        instrSize[blockCount][count++] = offset + internal_jmp_size;
        opcodePass(&asm_list[i], op, ptr);
        intptr_t jump_offset = jumpptr - (intptr_t)(ptr + offset + internal_jmp_to_offset);

        u32 * jumppos = (u32*)(ptr + offset + internal_jmp_to_offset);
        LOG("nomal jump_offset = %08X- %08X = %08X\n", jumpptr, (intptr_t)(ptr + offset + internal_jmp_to_offset), jump_offset);
        // cbnz w0,#imm(aarch64)
        //*jumppos = 0x35000000 | ((((jump_offset)>>2)&0x7FFFF)<<5);
        *jumppos = 0x14000000 | ((jump_offset>>2)&0x3FFFFFF);

#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
        *counterpos |= ((asm_list[i].cycle) << 10);
#else
        u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
        *counterpos = asm_list[i].cycle;
#endif
        ptr += *(asm_list[i].size) + nomal_seperator_size + internal_jmp_size;
        write_memory_counter = 0;
        continue;
      }
      else {
#if defined(RSTV) && !defined(AARCH64)
        // ADR-024: BT/BF to outside the block keeps compiling the not-taken path in the same block
        const int cont = rstv_dyn_cont && !debug_mode_ && ((op & 0xFF00) == 0x8900 || (op & 0xFF00) == 0x8B00) &&
          !(page->flags & BLOCK_LOOP) && !(jumppc >= start_addr && jumppc < addr);
#else
        const int cont = 0;
#endif
        const u32 flipsize = cont ? PAGEFLIPCONT_SIZE : DELAYJUMPSIZE;
        memcpy((void*)(ptr + *(asm_list[i].size) + nomal_seperator_size), cont ? (void*)PageFlipCont : (void*)PageFlip, flipsize);
        instrSize[blockCount][count++] = *(asm_list[i].size) + nomal_seperator_size + flipsize;
        opcodePass(&asm_list[i], op, ptr);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
        *counterpos = asm_list[i].cycle;
#endif
        ptr += *(asm_list[i].size) + nomal_seperator_size + flipsize;
        if (cont) {
          rstv_continued = 1;
          rstv_dyn_cont_n++;
          continue;
        }
      }
    }

    // Jmp With Delay Operation
    else { 

      u32 cycle = asm_list[i].cycle;
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)delay_seperator, delay_seperator_size);
      instrSize[blockCount][count++] = *(asm_list[i].size) + delay_seperator_size;
      opcodePass(&asm_list[i], op, ptr);
      ptr += *(asm_list[i].size) + delay_seperator_size;

      // Get NExt instruction
      temp = MappedMemoryReadWord(addr,NULL);
#ifdef SET_DIRTY
      if (ParentT) {
        u32 keepaddr = adress_mask(addr);
        { addrs &pl = ParentT[keepaddr]; const u32 sa = adress_mask(start_addr);
          if (pl.empty() || pl.back() != sa) pl.push_back(sa); } // ReST-V: same as push_back + unique, no O(n) pass
        RSTV_MARK_CODE(addr);
      }
#endif
      addr += 2;
      j = opcodeIndex(temp);
      write_memory_counter += asm_list[j].write_count;

#ifdef BUILD_INFO
      if(show_code_) DumpInstX( j, addr-2, temp  );
#endif
      if (asm_list[j].func == 0) {
        LOG("Unimplemented Opcode (0x%4x) at 0x%8x\n", temp, addr-2);
        return -1;
        break;
      }
      
      asm_list[j].build_count++;
      write_memory_counter += asm_list[j].write_count;

      cycle += asm_list[j].cycle;
      
      intptr_t offset = 0;
      memcpy((void*)ptr, (void*)(asm_list[j].func), *(asm_list[j].size));
      offset = *(asm_list[j].size);

      // internal loop
      if (jumpptr != 0xFFFFFFFF) {

        u32 cpsize = internal_delay_jmp_size;
        memcpy((void*)(ptr + offset), (void*)internal_delay_jmp, internal_delay_jmp_size);
        instrSize[blockCount][count++] = offset + internal_delay_jmp_size;
        opcodePass(&asm_list[j], temp, ptr);

        intptr_t jump_offset = jumpptr - (intptr_t)(ptr + offset + internal_delay_jmp_to_offset);
        u32 * jumppos = (u32*)(ptr + offset + internal_delay_jmp_to_offset);

        LOG("delay jump_offset = %lX - %lX = %08X\n", jumpptr, (intptr_t)(ptr + offset + internal_jmp_to_offset), jump_offset);

        *jumppos = 0x14000000 | ((jump_offset>>2)&0x3FFFFFF);
        // cbnz w0,#imm(aarch64)
        //*jumppos = 0x35000000 | (((jump_offset>>2)&0x7FFFF)<<5);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset;
        *counterpos = cycle;
#endif
        ptr += *(asm_list[j].size) + internal_delay_jmp_size;
        write_memory_counter = 0;
        continue;
      }
      else {
        memcpy((void*)(ptr + offset), (void*)seperator_delay_after, SEPERATORSIZE_DELAY_AFTER);
        instrSize[blockCount][count++] = offset + SEPERATORSIZE_DELAY_AFTER;
        opcodePass(&asm_list[j], temp, ptr);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset;
        *counterpos = cycle;
#endif
        ptr += *(asm_list[j].size) + SEPERATORSIZE_DELAY_AFTER;
      }
    }

    if (asm_list[i].delay != 0xFF && asm_list[i].delay != 0x00) {

      // jump to inside and no write is happend
      if (jumppc >= start_addr &&  jumppc < addr ) {
        if (write_memory_counter == 0 && !rstv_continued) {
          page->flags |= BLOCK_LOOP;
#ifdef BUILD_INFO 
              LOG("InfinityLoop block %08X 0x%04X  from 0x%08X to 0x%08X\n", start_addr, op, addr - 2, jumppc);
#endif
        }
      }
      else if (jumppc < start_addr && write_memory_counter == 0 ) {

        Block * tmp = NULL; 
        if ( (jumppc&0x0FF00000) == 0x06000000 && (start_addr & 0x0FF00000) == 0x06000000) {
          tmp = LookupTable[(jumppc & 0x000FFFFF) >> 1];
        }else if ((jumppc & 0x0FF00000) == 0x00200000 && (start_addr & 0x0FF00000) == 0x00200000) {
          tmp = LookupTableLow[(jumppc & 0x000FFFFF) >> 1];
        }
        else if ((jumppc & 0x0FF00000) == 0x00000000 && (start_addr & 0x0FF00000) == 0x00000000) {
          tmp = LookupTableRom[(jumppc & 0x000FFFFF) >> 1];
        }
        if (tmp != NULL && (tmp->flags&BLOCK_WRITE) == 0 && (tmp->e_addr+2) == page->b_addr && !rstv_continued) {
          page->flags |= BLOCK_LOOP;
        }

      }
      write_memory_counter = 0;
      //if( (op&0xFF00) == 0x8900) continue;  // BT
      //if( (op&0xFF00) == 0x8B00) continue;  // BF
      break;
    }

    if ( (op & 0xF0FF) == 0x400e || (op & 0xF0FF) == 0x4007) // sh2_LDC_SR
    {
      break;
    }

  }
  page->e_addr = addr-2;
#if defined(RSTV) && !defined(AARCH64)
  rstv_flush();
#endif
  memcpy((void*)ptr, (void*)epilogue, EPILOGSIZE);
  ptr += EPILOGSIZE;
#if defined(RSTV) && !defined(AARCH64)
  if (rstv_link_on && !debug_mode_) {
    if (rstv_link_stubs_fit(startptr, ptr, MAXBLOCKSIZE - (int)(ptr - startptr)) >= 0)
      ptr = rstv_link_emit(startptr, ptr);
    else
      rstv_link_skip++;
  }
#endif

  if (write_memory_counter > 0) {
    page->flags |= BLOCK_WRITE;
  }
#if defined(RSTV)
  {
    // ReST-V: block size statistics (log every 256 compiles)
    const unsigned sz = (unsigned)(ptr - startptr);
    rstv_dyn_blk_bytes += sz; rstv_dyn_blk_n++; if (sz > rstv_dyn_blk_max) rstv_dyn_blk_max = sz;
    rstv_dyn_blk_last = sz;
#if !defined(AARCH64)
    RSTV_ACT(29);
    memcpy(page->code, startptr, sz);
    RSTV_ACT(26);
#endif
#if !defined(__vita__)
    {
      // harness debug: RSTV_DUMPBLK=<pc hex> writes that block's ARM code to /tmp/blk_<pc>.bin
      static u32 dump_pc = 1;
      if (dump_pc == 1) { const char *e = getenv("RSTV_DUMPBLK"); dump_pc = e ? (u32)strtoul(e, NULL, 16) : 0; }
      if (dump_pc && page->b_addr == dump_pc) {
        char fn[64]; snprintf(fn, sizeof(fn), "/tmp/blk_%08x.bin", dump_pc);
        FILE *f = fopen(fn, "wb"); if (f) { fwrite(startptr, 1, sz, f); fclose(f); }
      }
    }
#endif
  }
#endif

#ifdef BUILD_INFO 
  //LOG("*********** end block size = %08X *************\n", (ptr - startptr));
  if( show_code_ ) {
      if( page->flags & BLOCK_LOOP ) LOG("Infinity loop");
      LOG("*********** end block *************\n");
  }
#endif 

#if defined(ARCH_IS_LINUX)
  cacheflush((uintptr_t)page->code,(uintptr_t)(page->code + (ptr - startptr)),0);
#endif

#if 0 //defined(BUILD_INFO) // Dump code
  char fname[64];
#if defined(ANDROID)
  sprintf(fname,"/mnt/sdcard/yabause/%08X.bin",start_addr);
#else
  sprintf(fname,"%08X.bin",start_addr);
#endif
   FILE * fp = fopen(fname, "wb");
  if(fp){
    fwrite(page->code, sizeof(char), (uintptr_t)ptr - (uintptr_t)page->code, fp);
    fclose(fp);
  }
#endif

  return 0;
}

DynarecSh2::DynarecSh2() {
  m_pDynaSh2     = new tagSH2;
  m_pDynaSh2->getmembyte = (uintptr_t)memGetByte;
  m_pDynaSh2->getmemword = (uintptr_t)memGetWord;
  m_pDynaSh2->getmemlong = (uintptr_t)memGetLong;
  m_pDynaSh2->setmembyte = (uintptr_t)memSetByte;
  m_pDynaSh2->setmemword = (uintptr_t)memSetWord;
  m_pDynaSh2->setmemlong = (uintptr_t)memSetLong;
#if defined(RSTV) && defined(__arm__)
  m_pDynaSh2->chain = rstv_chain_on ? (uintptr_t)rstv_chain : (uintptr_t)rstv_chain_off;
  m_pDynaSh2->macl_fn = (uintptr_t)rstv_macl_helper;
  {
    extern int rstv_jit_keep_open;
    m_pDynaSh2->chain_ic = !rstv_chain_on ? (uintptr_t)rstv_chain_off :
      (rstv_link_on && rstv_jit_keep_open) ? (uintptr_t)rstv_chain_ic : (uintptr_t)rstv_chain;
  }
  m_pDynaSh2->hwram = (uintptr_t)HighWram;
  m_pDynaSh2->memcycle_ptr = (uintptr_t)&memcycle_;
  m_pDynaSh2->lookup = 0;
  m_pDynaSh2->curblock = 0;
  // ReST-V (ADR-017): high work RAM handled inline in asm, the rest falls through to the C functions
  if (rstv_memfast) {
    m_pDynaSh2->getmembyte = (uintptr_t)rstv_memGetByte;
    m_pDynaSh2->getmemword = (uintptr_t)rstv_memGetWord;
    m_pDynaSh2->getmemlong = (uintptr_t)rstv_memGetLong;
    m_pDynaSh2->setmembyte = (uintptr_t)rstv_memSetByte;
    m_pDynaSh2->setmemword = (uintptr_t)rstv_memSetWord;
    m_pDynaSh2->setmemlong = (uintptr_t)rstv_memSetLong;
  }
#endif
  m_pDynaSh2->eachclock = (uintptr_t)DebugEachClock;

  m_pCompiler = CompileBlocks::getInstance();
  m_ClockCounter = 0;
  m_IntruptTbl.clear();
  m_bIntruptSort = true;
  pre_cnt_ = 0;
  pre_exe_count_ = 0;
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  pre_PC_ = 0;
  ctx_ = NULL;
  mtx_ = YabThreadCreateMutex();
  logenable_ = false;
  memcycle_ = 0;
}

DynarecSh2::~DynarecSh2(){
  YabThreadFreeMutex(mtx_);
}

void DynarecSh2::ResetCPU(){
  memset((void*)m_pDynaSh2->GenReg, 0, sizeof(u32) * 16);
  memset((void*)m_pDynaSh2->CtrlReg, 0, sizeof(u32) * 3);
  memset((void*)m_pDynaSh2->SysReg, 0, sizeof(u32) * 6);

  m_pDynaSh2->CtrlReg[0] = 0x000000;  // SR
  m_pDynaSh2->CtrlReg[2] = 0x000000; // VBR
  m_pDynaSh2->SysReg[3] = MappedMemoryReadLong(m_pDynaSh2->CtrlReg[2],NULL);
  m_pDynaSh2->GenReg[15] = MappedMemoryReadLong(m_pDynaSh2->CtrlReg[2] + 4,NULL);
  m_pDynaSh2->SysReg[4] = 0;
  m_pDynaSh2->SysReg[5] = 0;
  pre_cnt_ = 0;
  pre_exe_count_ = 0;
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  memcycle_ = 0;
  m_IntruptTbl.clear();
}

#if defined(RSTV)
// ReST-V: Yabause runs the other CPU for 32 cycles inside an input-capture write (sh2core.c
// SSH2InputCaptureWriteWord). ExecuteCount would skip such a short run whenever the previous slice overshot
// (pre_exe_count_ > Count), so the slave never saw the master's queue reset in time (sanjeon). This runs whole
// blocks until Count cycles have passed and leaves the slice bookkeeping (pre_exe_count_) alone.
void DynarecSh2::ExecuteNested(u32 Count) {
  rstv_sleep_ = 0; // ADR-050: the writer woke this CPU
  const u32 saved_count = m_pDynaSh2->SysReg[4], saved_exit = m_pDynaSh2->exitcount, saved_mem = memcycle_;
  m_pDynaSh2->SysReg[4] = 0;
  m_pDynaSh2->exitcount = Count;
  memcycle_ = 0;
  // run until the woken CPU is back in its wait/idle loop (it has handled what the writer signalled), at most Count
  while (m_pDynaSh2->SysReg[4] < Count) {
    rstv_hle_items[6]++;
    if (Execute() == IN_INFINITY_LOOP)
      break;
    m_pDynaSh2->SysReg[4] += memcycle_;
    memcycle_ = 0;
  }
  m_pDynaSh2->SysReg[4] = saved_count;
  m_pDynaSh2->exitcount = saved_exit;
  memcycle_ = saved_mem;
}
#endif

extern "C" { int rstv_sleep_max = 8; unsigned rstv_sleep_slices; } // ADR-050 (0 = off: ux0:data/restv/nosleep)
void DynarecSh2::ExecuteCount( u32 Count ) {
  u32 targetcnt = 0;
#if defined(RSTV)
  // ADR-050: the CPU stopped at an idle-loop head and no unmasked interrupt is pending: the loop would only spin.
  // Skip up to rstv_sleep_max slices (then run the loop once again: it may wait for memory, not only an interrupt)
  if (rstv_sleep_ && rstv_sleep_n_ < rstv_sleep_max && (GET_SR() & 0xF0) >= GET_ICOUNT()) {
    rstv_sleep_n_++;
    rstv_sleep_slices++;
    if (Count > pre_exe_count_) {
      targetcnt = Count - pre_exe_count_;
      m_pDynaSh2->SysReg[4] = targetcnt;
      CurrentSH2->cycles = targetcnt;
      pre_exe_count_ = 0;
    } else
      pre_exe_count_ -= Count;
    return;
  }
  rstv_sleep_ = 0;
  rstv_sleep_n_ = 0;
#endif
  
  m_pDynaSh2->SysReg[4] = 0;
    if (Count > pre_exe_count_) {
    targetcnt = Count - pre_exe_count_;
  }
  else {
    // Just Onestep
    //Execute();
    pre_exe_count_ = (pre_exe_count_ + m_pDynaSh2->SysReg[4]) - Count ;
    return;
  }

#if 0
  // Overflow
  if (targetcnt < m_pDynaSh2->SysReg[4]){
    targetcnt = Count + (0xFFFFFFFF - m_pDynaSh2->SysReg[4]) + 1;
    m_pDynaSh2->SysReg[4] = 0;
  }
#endif

  m_pDynaSh2->exitcount = targetcnt;
#if defined(RSTV)
  u32 rstv_skipped = 0;
#endif

  //if ((GET_SR() & 0xF0) < GET_ICOUNT()) {
  //  this->CheckInterupt();
  //}
  memcycle_ = 0;
  while (m_pDynaSh2->SysReg[4] < targetcnt) {
    if (Execute() == IN_INFINITY_LOOP ) {
#if defined(RSTV)
        if (m_pDynaSh2->SysReg[4] < targetcnt) rstv_skipped += targetcnt - m_pDynaSh2->SysReg[4];
#endif
        SET_COUNT(targetcnt);
        loopskip_cnt_++;
    }
    m_pDynaSh2->SysReg[4] += memcycle_;
    memcycle_ = 0;
    //printf("%d/%d\n",GET_COUNT(),targetcnt);
  }

#if defined(RSTV)
  rstv_dyn_cyc[is_slave_ ? 1 : 0] += m_pDynaSh2->SysReg[4] - rstv_skipped; // cycles really run (performance line)
#endif
  CurrentSH2->cycles = m_pDynaSh2->SysReg[4];
  //if (Count == 1) {
  //  one_step_ = true;
  //  pre_exe_count_ = 0;
  //}
  //else {
  //  one_step_ = false;
    pre_exe_count_ = m_pDynaSh2->SysReg[4] - targetcnt;
  //}
}

int DynarecSh2::CheckOneStep() {
  if (one_step_ == 1) {
    m_pDynaSh2->SysReg[3] += 2;
    return 1;
  }
  return 0;
}

void DynarecSh2::Undecoded(){

  LOG("Undecoded %08X", GET_PC());
  // Save regs.SR on stack
  GetGenRegPtr()[15] -= 4;
  memSetLong(GetGenRegPtr()[15], GET_SR());

  // Save regs.PC on stack
  GetGenRegPtr()[15] -= 4;
  memSetLong(GetGenRegPtr()[15], GET_PC()+2);


  // What caused the exception? The delay slot or a general instruction?
  // 4 for General Instructions, 6 for delay slot
  u32 vectnum = 4; //  Fix me

  // Jump to Exception service routine
  u32 newpc = memGetLong(GET_VBR() + (vectnum << 2));
  SET_PC(newpc);

  return;
}


#if defined(RSTV) && !defined(__vita__)
// ReST-V (harness, qemu-arm): differential check of every dynarec block against the interpreter.
// RSTV_DYNCHK=1 → after each block, re-run the same instructions with the interpreter from the saved registers
// and compare. The first mismatch is printed with a disassembly and the process exits.
#include <stdlib.h>
#include "sh2d.h"
#include "sh2int.h"
static int s_dynchk = -1;
static void rstv_dynchk(SH2_struct *ctx, const u32 *pre, const u32 *post, int slave)
{
  if (s_dynchk < 0) {
    s_dynchk = getenv("RSTV_DYNCHK") != NULL;
    if (s_dynchk)
      SH2Interpreter.Init();
  }
  if (!s_dynchk)
    return;
  extern int rstv_dynchk_io, rstv_io_mode;
  extern unsigned rstv_io_n, rstv_io_pos, rstv_io_bad;
  if (rstv_dynchk_io || rstv_io_bad) { // nested run of the other CPU, or the I/O log overflowed
    extern unsigned rstv_undo_n;
    rstv_undo_n = 0;
    return;
  }
  static SH2_struct tmp;
  // restore RAM written by the dynarec block, newest first
  extern int rstv_undo_on; extern unsigned rstv_undo_n; extern u32 rstv_undo_addr[], rstv_undo_val[];
  extern u8 rstv_undo_size[]; extern u8 *rstv_undo_mem[];
  for (int k = (int)rstv_undo_n - 1; k >= 0; k--) {
    u8 *m = rstv_undo_mem[k]; const u32 o = rstv_undo_addr[k], v = rstv_undo_val[k];
    if (rstv_undo_size[k] == 1) T2WriteByte(m, o, (u8)v); else if (rstv_undo_size[k] == 2) T2WriteWord(m, o, (u16)v); else T2WriteLong(m, o, v);
  }
  const unsigned undo_n = rstv_undo_n;
  rstv_undo_n = 0;
  memcpy(&tmp, ctx, sizeof(tmp));
  for (int i = 0; i < 16; i++) tmp.regs.R[i] = pre[i];
  tmp.regs.SR.all = pre[16]; tmp.regs.GBR = pre[17]; tmp.regs.VBR = pre[18];
  tmp.regs.MACH = pre[19]; tmp.regs.MACL = pre[20]; tmp.regs.PR = pre[21]; tmp.regs.PC = pre[22];
  const u32 target = post[22];
  u32 trace_pc[8], trace_op[8], trace_r0[8];
  int steps = 0;
  SH2_struct *saved = CurrentSH2;
  const u32 c0 = tmp.cycles, dc = post[23] - pre[23]; // dynarec cycles spent in the block
  // run until the interpreter reaches the block's exit PC with the same registers (the interpreter folds delay slots
  // into the branch and counts memory waits, so instruction/cycle counts are not comparable)
  int bad = 1;
  rstv_io_pos = 0;
  rstv_io_mode = 2;
  while (steps < (int)dc * 2 + 16) {
    tmp.instruction = fetchlist[(tmp.regs.PC >> 20) & 0x0FF](tmp.regs.PC);
    if (steps < 8) { trace_pc[steps] = tmp.regs.PC; trace_op[steps] = tmp.instruction; }
    opcodes[tmp.instruction](&tmp);
    if (steps < 8) trace_r0[steps] = tmp.regs.R[0];
    steps++;
    if (tmp.regs.PC != target)
      continue;
    int diff = 0;
    for (int i = 0; i < 16; i++) diff |= tmp.regs.R[i] != post[i];
    diff |= (tmp.regs.SR.all & 0x3F3) != (post[16] & 0x3F3);
    diff |= tmp.regs.GBR != post[17] || tmp.regs.MACH != post[19] || tmp.regs.MACL != post[20] || tmp.regs.PR != post[21];
    if (!diff && rstv_io_pos == rstv_io_n && !rstv_io_bad) { bad = 0; break; }
  }
  rstv_io_mode = 0;
  CurrentSH2 = saved;
  static unsigned long long checked;
  checked++;
  if (!bad)
    return;
  for (int k = 0; k < 8 && k < steps; k++) printf("  step %d pc %08x op %04x r0 %08x\n", k, trace_pc[k], trace_op[k], trace_r0[k]);
  { extern unsigned rstv_dyn_mem[12]; extern int rstv_undo_on; unsigned t = 0; for (int k = 0; k < 12; k++) t += rstv_dyn_mem[k];
    printf("dynchk: io log %u used %u bad %u\n", rstv_io_n, rstv_io_pos, rstv_io_bad);
    printf("dynchk: ram writes undone %u io %d slowmem total %u undo_on %d memfast %d\n", undo_n, rstv_dynchk_io, t, rstv_undo_on, rstv_memfast); }
  printf("dynchk: cycles dyn %u interp %u\n", dc, tmp.cycles - c0);
  printf("dynchk: %s block %08x -> %08x mismatch after %llu blocks (interp steps %d)\n", slave ? "slave" : "master", pre[22],
    target, checked, steps);
  static const char *nm[23] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "sr", "gbr", "vbr", "mach", "macl", "pr", "pc" };
  const u32 it[23] = { tmp.regs.R[0], tmp.regs.R[1], tmp.regs.R[2], tmp.regs.R[3], tmp.regs.R[4], tmp.regs.R[5], tmp.regs.R[6],
    tmp.regs.R[7], tmp.regs.R[8], tmp.regs.R[9], tmp.regs.R[10], tmp.regs.R[11], tmp.regs.R[12], tmp.regs.R[13], tmp.regs.R[14],
    tmp.regs.R[15], tmp.regs.SR.all, tmp.regs.GBR, tmp.regs.VBR, tmp.regs.MACH, tmp.regs.MACL, tmp.regs.PR, tmp.regs.PC };
  for (int i = 0; i < 23; i++)
    printf("  %-4s pre %08x dyn %08x interp %08x%s\n", nm[i], pre[i], post[i], it[i], post[i] != it[i] ? "   <--" : "");
  for (u32 a = pre[22]; a < pre[22] + 64; a += 2) {
    char str[128];
    const u16 op = MappedMemoryReadWord(a, NULL);
    SH2Disasm(a, op, 0, NULL, str);
    printf("  %s\n", str);
  }
  fflush(stdout);
  exit(3);
}
static void rstv_dyn_regs(tagSH2 *d, u32 *o)
{
  for (int i = 0; i < 16; i++) o[i] = d->GenReg[i];
  o[16] = d->CtrlReg[0]; o[17] = d->CtrlReg[1]; o[18] = d->CtrlReg[2];
  o[19] = d->SysReg[0]; o[20] = d->SysReg[1]; o[21] = d->SysReg[2]; o[22] = d->SysReg[3]; o[23] = d->SysReg[4];
}
#endif
#if defined(RSTV)
extern "C" void SH2DynWriteNotify(u32 start, u32 length);
// ADR-044: tiered execution. A high work RAM block that has not been compiled runs with the interpreter the first
// rstv_tier_n times (code that runs once at a scene change costs about 1us interpreted instead of 40us to compile).
extern "C" {
  int rstv_tier_n = 2;        // 0 = off (ux0:data/restv/notier)
  int rstv_tier_wnotify;      // memory.c: interpreted writes to high work RAM invalidate compiled code
  unsigned rstv_tier_runs;    // performance line
  static u8 rstv_tier_hot[0x100000 >> 1];
  void rstv_tier_notify(u32 addr, u32 size)
  {
    if (rstv_code_chunk[(addr & 0xFFFFF) >> 5] || rstv_code_chunk[((addr + size - 1) & 0xFFFFF) >> 5])
      SH2DynWriteNotify(addr & ~1u, (size + 1) & ~1u);
  }
}
int DynarecSh2::RstvTierRun()
{
  static int init;
  if (!init) {
    init = 1;
    if (opcodes[0x0009] == NULL)
      SH2Interpreter.Init();
  }
  SH2_struct *sh = ctx_;
  SH2_struct *saved_cur = CurrentSH2;
  const sh2regs_struct saved_regs = sh->regs;
  const u32 saved_cycles = sh->cycles, saved_instr = sh->instruction;
  CurrentSH2 = sh;
  memcpy(sh->regs.R, GetGenRegPtr(), sizeof(sh->regs.R));
  sh->regs.SR.all = GET_SR(); sh->regs.GBR = GET_GBR(); sh->regs.VBR = GET_VBR();
  sh->regs.MACH = GET_MACH(); sh->regs.MACL = GET_MACL(); sh->regs.PR = GET_PR(); sh->regs.PC = GET_PC();
  sh->cycles = 0;
  rstv_tier_wnotify++;
  for (int n = 0; n < 256; n++) {
    const u32 pc = sh->regs.PC;
    sh->instruction = fetchlist[(pc >> 20) & 0x0FF](pc);
    const u16 op = (u16)sh->instruction;
    opcodes[sh->instruction](sh);
    if (sh->regs.PC != pc + 2) break;                                   // branch (delay slot included), sleep
    if ((op & 0xF0FF) == 0x400E || (op & 0xF0FF) == 0x4007) break;     // ldc Rm,sr / ldc.l: interrupt check
    if ((sh->regs.PC & 0x0FF00000) != 0x06000000) break;
  }
  rstv_tier_wnotify--;
  memcpy(GetGenRegPtr(), sh->regs.R, sizeof(sh->regs.R));
  SET_SR(sh->regs.SR.all); SET_GBR(sh->regs.GBR); SET_VBR(sh->regs.VBR);
  SET_MACH(sh->regs.MACH); SET_MACL(sh->regs.MACL); SET_PR(sh->regs.PR); SET_PC(sh->regs.PC);
  const u32 cyc = sh->cycles & ~0x40000000u; // RSTV_IDLE_FLAG: the dynarec checks idle PCs itself
  sh->regs = saved_regs;
  sh->cycles = saved_cycles;
  sh->instruction = saved_instr;
  CurrentSH2 = saved_cur;
  m_pDynaSh2->SysReg[4] += cyc ? cyc : 1;
  rstv_tier_runs++;
  return 0;
}
#endif

#if defined(RSTV)
// ADR-045: game C library routines (stv.c hle table) run natively. The routine's code is compared with the expected
// opcodes first, the memory ranges must be in work RAM; otherwise the SH-2 code runs. The SH-2 cycles of the original
// loop are still counted, so the game's timing does not change.
#include "../stv.h"
extern "C" { u32 rstv_hle_big[4]; }
extern "C" { unsigned rstv_hle_runs, rstv_hle_bytes, rstv_hle_kind[8], rstv_hle_fail[8], rstv_hle_why, rstv_hle_us[8], rstv_hle_auto_n;
  int rstv_hle_on = 1, rstv_hle_auto = 1; }
// ReST-V: library routine code signatures (length in words, FNV-1a over the words). The code itself is game data
// and is not kept here (no ROM data in the repository)
typedef struct { int n; u32 hash; int skip0, skip1; } RstvCodeSig; // words skip0..skip1 are not hashed (-1: none)
static const RstvCodeSig rstv_hle_sig[7] = {
  { 0, 0x811c9dc5u, -1, -1 },
  { 36, 0xac861708u, -1, -1 },
  { 13, 0x07d75e63u, -1, -1 },
  { 12, 0xddd3d94cu, -1, -1 },
  { 12, 0xd89d918du, -1, -1 },
  { 14, 0x920da3d5u, -1, -1 },
  { 6, 0x9af6a5ffu, -1, -1 },
};
static const int rstv_hle_len[7] = { 0, 36, 13, 12, 12, 14, 6 };
// ADR-047: SGL vertex transform loop (sasissu / sanjeon 0606ECB6 .. 0606ED4C)
static const RstvCodeSig rstv_sgl_vtx2_sig = { 398, 0x452d1e96u, -1, -1 };
static const RstvCodeSig rstv_sgl_vtx_sig = { 364, 0x32c0b60eu, -1, -1 };
// work RAM byte pointer for [a, a + n), or NULL
static u8 *rstv_hle_mem(u32 a, u32 n, u32 *off)
{
  if ((a >> 29) > 1 || n > 0x100000)
    return NULL;
  const u32 b = a & 0x0FFFFFFF;
  if ((b & 0x0FF00000) == 0x06000000 && (b & 0xFFFFF) + n <= 0x100000) { *off = b & 0xFFFFF; return HighWram; }
  if ((b & 0x0FF00000) == 0x00200000 && (b & 0xFFFFF) + n <= 0x100000) { *off = b & 0xFFFFF; return LowWram; }
  return NULL;
}
// plain memory other than work RAM, accessed through the memory map: VDP1 VRAM, VDP2 VRAM / CRAM, sound RAM
static int rstv_hle_mapped(u32 a, u32 n)
{
  if ((a >> 29) > 1 || n > 0x100000)
    return 0;
  const u32 b = a & 0x0FFFFFFF, e = b + n;
  return (b >= 0x05C00000 && e <= 0x05C80000) || (b >= 0x05E00000 && e <= 0x05F00000) || (b >= 0x05F00000 && e <= 0x05F01000) ||
    (b >= 0x05A00000 && e <= 0x05A80000);
}
// m: host pointer (t1: byte order as in memory (VDP RAM, cartridge ROM), else 16-bit swapped work RAM)
typedef struct { u8 *m; u32 o, a; u8 t1; } RstvHleSide;
extern u8 *Vdp1Ram, *Vdp2Ram;
extern "C" u8 *rstv_cart_rom(void);
extern "C" u8 *rstv_sound_ram(u32 off, u32 n);
static int rstv_hle_side(RstvHleSide *x, u32 a, u32 n, int ro = 0)
{
  x->a = a;
  x->t1 = 0;
  x->m = rstv_hle_mem(a, n, &x->o);
  if (x->m != NULL)
    return 1;
  const u32 b = a & 0x0FFFFFFF;
  if ((a >> 29) <= 1 && n <= 0x80000) {
    // VDP1 / VDP2 VRAM through a pointer: the pages are marked once afterwards (rstv_hle_notify). A byte at a time
    // through the memory map took 42 ms for a 270 KB copy (thuntk scene changes)
    if (b >= 0x05C00000 && b + n <= 0x05C80000) { x->m = Vdp1Ram; x->o = b - 0x05C00000; x->t1 = 1; return 1; }
    if (b >= 0x05E00000 && b + n <= 0x05E80000) { x->m = Vdp2Ram; x->o = b - 0x05E00000; x->t1 = 1; return 1; }
    if (b >= 0x05A00000 && b + n <= 0x05B00000) {
      u8 *snd = rstv_sound_ram(b - 0x05A00000, n);
      if (snd) { x->m = snd; x->o = b - 0x05A00000; return 1; } // 16-bit swapped like work RAM
    }
  }
  if (rstv_hle_mapped(a, n))
    return 1;
  // read-only source: cartridge ROM (not the protection / I/O area at the top)
  if (ro && (a >> 29) <= 1 && b >= 0x02000000 && b + n <= 0x04F00000) {
    u8 *rom = rstv_cart_rom();
    if (rom) { x->m = rom; x->o = b - 0x02000000; x->t1 = 1; }
    return 1;
  }
  return 0;
}
static inline u8 rstv_hle_rd(const RstvHleSide *x, u32 i)
{
  return x->m ? (x->t1 ? T1ReadByte(x->m, x->o + i) : T2ReadByte(x->m, x->o + i)) : MappedMemoryReadByte(x->a + i, NULL);
}
static inline void rstv_hle_wr(const RstvHleSide *x, u32 i, u8 v)
{
  if (x->m) {
    if (x->t1) T1WriteByte(x->m, x->o + i, v); else T2WriteByte(x->m, x->o + i, v);
  } else {
    u32 c = 0;
    MappedMemoryWriteByte(x->a + i, v, &c);
  }
}
// compiled code in the written range must go
extern "C" void rstv_vdp1_ram_touch(u32 addr, u32 bytes);
extern "C" void rstv_vdp2_ram_touch(u32 addr, u32 bytes);
static void rstv_hle_notify(u32 a, u32 n)
{
  if (!n)
    return;
  const u32 b = a & 0x0FFFFFFF;
  if (b >= 0x05C00000 && b + n <= 0x05C80000) { rstv_vdp1_ram_touch(b, n); return; }
  if (b >= 0x05E00000 && b + n <= 0x05E80000) { rstv_vdp2_ram_touch(b, n); return; }
  if ((b & 0x0FF00000) != 0x00200000 && (b & 0x0FF00000) != 0x06000000)
    return;
  if ((b & 0x0FF00000) == 0x00200000) {
    SH2DynWriteNotify(b & ~1u, (n + 2) & ~1u);
    return;
  }
  for (u32 c = (b & 0xFFFFF) >> 5; c <= (((b & 0xFFFFF) + n - 1) >> 5); c++)
    if (rstv_code_chunk[c])
      SH2DynWriteNotify(0x06000000 | (c << 5), 32);
}
static const RstvCodeSig rstv_lz_code_sig = { 204, 0xf5be3333u, 110, 119 };
static const RstvCodeSig rstv_lz_shar_sig = { 12, 0x80fff583u, -1, -1 };
extern "C" { unsigned rstv_hle_codegen = 1; unsigned char rstv_hle_chunk[0x100000 >> 5]; }
static u32 rstv_hle_rng[32][2]; // checked code ranges (high work RAM offsets, [start, end))
static int rstv_hle_nrng;
extern "C" int rstv_hle_in_checked(u32 addr)
{
  const u32 o = addr & 0xFFFFF;
  for (int k = 0; k < rstv_hle_nrng; k++)
    if (o + 4 > rstv_hle_rng[k][0] && o < rstv_hle_rng[k][1])
      return 1;
  return 0;
}
static int rstv_hle_match(u32 a, const RstvCodeSig *sig);
// The code of a native routine is compared once; its 32-byte chunks are then marked as code so a write to them reaches
// setDirty (rstv_memfast_arm.s), which moves rstv_hle_codegen and makes the next call compare again
static int rstv_hle_checked(unsigned *gen, u32 a, const RstvCodeSig *sig)
{
  if (*gen == rstv_hle_codegen)
    return 1;
  rstv_hle_items[7]++;
  if (!rstv_hle_match(a, sig))
    return 0;
  for (u32 c = a & ~31u; c < a + (u32)sig->n * 2; c += 32) {
    RSTV_MARK_CODE(c);
    rstv_hle_chunk[(c & 0xFFFFF) >> 5] = 1;
  }
  // ADR-100: the exact range too: twcup98's vertex routine copies its matrix right after its code, into the same chunk
  // (420 re-hashes a frame)
  {
    const u32 r0 = a & 0xFFFFF, r1 = r0 + (u32)sig->n * 2;
    int k = 0;
    while (k < rstv_hle_nrng && !(rstv_hle_rng[k][0] == r0 && rstv_hle_rng[k][1] == r1))
      k++;
    if (k == rstv_hle_nrng && k < 32) {
      rstv_hle_rng[k][0] = r0;
      rstv_hle_rng[k][1] = r1;
      rstv_hle_nrng++;
    }
  }
  *gen = rstv_hle_codegen;
  return 1;
}
static int rstv_hle_match(u32 a, const RstvCodeSig *sig)
{
  if ((a & 0x0FF00000) != 0x06000000 || (a & 0xFFFFF) + sig->n * 2 > 0x100000)
    return 0;
  u32 h = 2166136261u;
  for (int k = 0; k < sig->n; k++)
    if (k < sig->skip0 || k > sig->skip1)
      h = (h ^ T2ReadWord(HighWram, (a & 0xFFFFF) + k * 2)) * 16777619u;
  return h == sig->hash;
}
// thuntk 0601EFCC: LZ decoder step. r4 = &src, r5 = &dst, r6 = dst end (inclusive), r7 = tokens (<= 16).
// A 16-bit flag word, then per token a 16-bit word w: flag bit i set -> two literal bytes (w >> 8, w & 0xFF);
// else (w & 0xF000) == 0 -> repeat the previous byte w + 3 times; == 0x1000 -> copy (next byte + 0x11) bytes from dst - (w & 0xFFF);
// else copy (w >> 12) + 1 bytes from dst - (w & 0xFFF). Lengths are clipped at the end. Returns 0, 1 or -1 like the SH-2 code.
static int rstv_hle_lz(u32 psrc, u32 pdst, u32 end, s32 count, u32 *ret, u32 *cyc, u32 *bytes)
{
  u32 src = memGetLong(psrc), dst = memGetLong(pdst);
  const u32 dst0 = dst;
  RstvHleSide xs, xd;
  const u32 lo = dst - 0x1000;
  if (count > 16 || end < dst || end - dst > 0x80000 || !rstv_hle_side(&xs, src, 2 + 3 * (count > 0 ? count : 0), 1) ||
      !rstv_hle_side(&xd, lo, end - lo + 1))
    return 0;
  u32 si = 0, c = 40, r = 0;
#define LZ_RD() rstv_hle_rd(&xs, si++)
#define LZ_DRD(a) rstv_hle_rd(&xd, (a) - lo)
#define LZ_DWR(a, v) rstv_hle_wr(&xd, (a) - lo, (v))
  {
    const u32 b0 = LZ_RD(), b1 = LZ_RD();
    const u32 flags = ((b0 << 8) | b1) & 0xFFFF;
    for (s32 i = 0; i < count; i++) {
      if (dst > end) { r = 0xFFFFFFFFu; break; }
      const u32 h = LZ_RD(), l = LZ_RD();
      const u32 w = ((h << 8) | l) & 0xFFFF;
      if (flags & (1u << i)) {
        LZ_DWR(dst, (u8)(w >> 8));
        dst++;
        c += 45;
        if (dst > end) { r = 1; break; }
        LZ_DWR(dst, (u8)w);
        dst++;
        continue;
      }
      const u32 rem = end - dst + 1;
      if ((w & 0xF000) == 0) {
        const u32 len = w + 3;
        const u32 n = (len >= rem ? rem : len) & 0xFFFF;
        const u8 v = LZ_DRD(dst - 1);
        for (u32 k = 0; k < n; k++)
          LZ_DWR(dst + k, v);
        dst += n;
        c += 40 + n * 6;
      } else {
        u32 len;
        if ((w & 0xF000) == 0x1000)
          len = LZ_RD() + 0x11;
        else
          len = (w >> 12) + 1;
        const u32 n = (len >= rem ? rem : len) & 0xFFFF;
        const u32 from = dst - (w & 0x0FFF);
        if (from < lo)
          return -1; // outside the checked range: give up (nothing has been written that the SH-2 code would not write)
        for (u32 k = 0; k < n; k++)
          LZ_DWR(dst + k, LZ_DRD(from + k));
        dst += n;
        c += 50 + n * 7;
      }
    }
  }
#undef LZ_RD
#undef LZ_DRD
#undef LZ_DWR
  memSetLong(psrc, src + si);
  memSetLong(pdst, dst);
  rstv_hle_notify(dst0, dst - dst0);
  *ret = r;
  *cyc = c;
  *bytes = dst - dst0;
  return 1;
}

// LZ decoder at pc: the body (literal pool words skipped), its literals, the bit table, the shar chains and the
// memset / memcpy it calls must all be the known code (thuntk 0601EFCC, sandor 0601F0A0)
static u32 rstv_hle_long(u32 a) { return ((u32)T2ReadWord(HighWram, a & 0xFFFFF) << 16) | T2ReadWord(HighWram, (a + 2) & 0xFFFFF); }
static int rstv_lz_check(u32 pc)
{
  if ((pc & 0x0FF00000) != 0x06000000 || (pc & 0xFFFFF) + 0x1D0 > 0x100000)
    return 0;
  if (!rstv_hle_match(pc, &rstv_lz_code_sig))
    return 0;
  if (T2ReadWord(HighWram, (pc + 0xDC) & 0xFFFFF) != 0x00FF || rstv_hle_long(pc + 0xE8) != 0x0000F000 ||
      T2ReadWord(HighWram, (pc + 0x1BA) & 0xFFFFF) != 0x1000 || T2ReadWord(HighWram, (pc + 0x1BC) & 0xFFFFF) != 0x0FFF ||
      rstv_hle_long(pc + 0x1C0) != 0x0000F000)
    return 0;
  const u32 tbl = rstv_hle_long(pc + 0xE0), shar8 = rstv_hle_long(pc + 0xE4), mset = rstv_hle_long(pc + 0xEC);
  const u32 mcpy = rstv_hle_long(pc + 0x1C4), shar12 = rstv_hle_long(pc + 0x1C8);
  if (shar8 != shar12 + 8 || !rstv_hle_match(shar12, &rstv_lz_shar_sig) || T2ReadWord(HighWram, (shar12 + 24) & 0xFFFFF) != 0x4021 ||
      !rstv_hle_match(mset, &rstv_hle_sig[RSTV_HLE_MEMSET8]) || !rstv_hle_match(mcpy, &rstv_hle_sig[RSTV_HLE_MEMCPY]))
    return 0;
  if ((tbl & 0x0FF00000) != 0x06000000 || (tbl & 0xFFFFF) + 32 > 0x100000)
    return 0;
  for (int k = 0; k < 16; k++)
    if (T2ReadWord(HighWram, (tbl & 0xFFFFF) + k * 2) != (1u << k))
      return 0;
  return 1;
}
// overlay BSS clear loop test at pc (loop body at pc - 6, see RSTV_HLE_BSSCLR)
static int rstv_bss_check(u32 pc)
{
  return (pc & 0x0FF00000) == 0x06000000 && (pc & 0xFFFFF) >= 6 && rstv_hle_match(pc - 6, &rstv_hle_sig[RSTV_HLE_BSSCLR]);
}

// ADR-091: the other overlay BSS loop (thunt / sandor C start-up, value r5, end pointer r6), entered at its test:
//   pc-4: mov.b r5,@r4 / add #1,r4 / pc: mov.l @r6,r3 / cmp/hs r3,r4 / bf pc-4
// 450KB a byte at a time cost thunt 5 frames of 73ms at game start
static int rstv_bss2_check(u32 pc)
{
  const u32 a = pc & 0xFFFFF;
  if ((pc & 0x0FF00000) != 0x06000000 || a < 4 || a + 6 > 0x100000)
    return 0;
  return T2ReadWord(HighWram, a - 4) == 0x2450 && T2ReadWord(HighWram, a - 2) == 0x7401 && T2ReadWord(HighWram, a) == 0x6362 &&
         T2ReadWord(HighWram, a + 2) == 0x3432 && T2ReadWord(HighWram, a + 4) == 0x8bfa;
}

// ADR-109: compiler delay loop on a stack variable, entered at its body (the bf target):
//   pc: mov.l @r15,r2 / add #-1,r2 / mov.l r2,@r15 / mov.l @r15,r0 / tst r0,r0 / bf pc
// (bakubaku's slave spends the frame in it). Run natively with the same iterations per time slice
static int rstv_delay_cyc = 8, rstv_delay_nt = 2; // dynarec cycles of one iteration; less when the bf falls through
#define RSTV_DELAY_CYC ((u64)rstv_delay_cyc)
#define RSTV_DELAY_NT ((u32)rstv_delay_nt)
static int rstv_delay_check(u32 pc)
{
#if !defined(__vita__)
  static int env = 0;
  if (!env) {
    env = 1;
    if (getenv("RSTV_DELAYCYC")) sscanf(getenv("RSTV_DELAYCYC"), "%d,%d", &rstv_delay_cyc, &rstv_delay_nt);
  }
#endif
  const u32 a = pc & 0xFFFFF;
  if ((pc & 0x0FF00000) != 0x06000000 || a + 12 > 0x100000)
    return 0;
  static const u16 w[6] = { 0x62f2, 0x72ff, 0x2f22, 0x60f2, 0x2008, 0x8bf9 };
  for (int i = 0; i < 6; i++)
    if (T2ReadWord(HighWram, a + i * 2) != w[i])
      return 0;
  return 1;
}

// ADR-109: the register form (vfkids master), entered at the test: pc: tst rN,rN / bf/s pc / add #-1,rN
static int rstv_delayr_cyc = 4, rstv_delayr_nt = 1;
static int rstv_delayr_check(u32 pc)
{
#if !defined(__vita__)
  static int env = 0;
  if (!env) {
    env = 1;
    if (getenv("RSTV_DELAYRCYC")) sscanf(getenv("RSTV_DELAYRCYC"), "%d,%d", &rstv_delayr_cyc, &rstv_delayr_nt);
  }
#endif
  const u32 a = pc & 0xFFFFF;
  if ((pc & 0x0FF00000) != 0x06000000 || a + 6 > 0x100000)
    return -1;
  const u16 w0 = T2ReadWord(HighWram, a), w1 = T2ReadWord(HighWram, a + 2), w2 = T2ReadWord(HighWram, a + 4);
  const int n = (w0 >> 8) & 15;
  if ((w0 & 0xF00F) != 0x2008 || ((w0 >> 4) & 15) != n || w1 != 0x8FFD || w2 != (0x70FF | (n << 8)))
    return -1;
  return n;
}

// ADR-045: library routines found by their code (position independent bodies), any game: checked the first time a
// high work RAM PC is not compiled. Found PCs join rstv_hle[] and a bit map (the dispatcher looks them up there).
static int rstv_hle_bits_n = -1;
static int rstv_hle_detect(u32 pc)
{
  if ((pc & 0x0FF00000) != 0x06000000)
    return 0;
  const u16 w0 = T2ReadWord(HighWram, pc & 0xFFFFF);
  int kind = 0;
  if (w0 == 0x2fd6) kind = RSTV_HLE_MEMMOVE;
  else if (w0 == 0xe700) kind = RSTV_HLE_MEMSET8;
  else if (w0 == 0x6743) kind = RSTV_HLE_MEMCPY;
  else if (w0 == 0xe300) kind = (pc & 0xFFFFF) + 8 < 0x100000 && T2ReadWord(HighWram, (pc & 0xFFFFF) + 8) == 0x2752 ? RSTV_HLE_FILL32 : RSTV_HLE_FILL16;
  else if (w0 == 0x2fe6) kind = rstv_lz_check(pc) ? RSTV_HLE_LZ : 0;
  else if (w0 == 0x6372) kind = rstv_bss_check(pc) ? RSTV_HLE_BSSCLR : 0;
  else if (w0 == 0x6362) kind = rstv_bss2_check(pc) ? RSTV_HLE_BSSCLR2 : 0;
  else if (w0 == 0x62f2) kind = rstv_delay_check(pc) ? RSTV_HLE_DELAY : 0;
  else if ((w0 & 0xF00F) == 0x2008) kind = rstv_delayr_check(pc) >= 0 ? RSTV_HLE_DELAYR : 0;
  if (!kind)
    return 0;
  if (kind != RSTV_HLE_LZ && kind != RSTV_HLE_BSSCLR && kind != RSTV_HLE_BSSCLR2 && kind != RSTV_HLE_DELAY && kind != RSTV_HLE_DELAYR &&
      !rstv_hle_match(pc, &rstv_hle_sig[kind]))
    return 0;
  if (rstv_hle_n >= RSTV_MAX_HLE)
    return 0;
  rstv_hle[rstv_hle_n].pc = pc;
  rstv_hle[rstv_hle_n].kind = kind;
  rstv_hle_n++;
  return kind;
}
static void rstv_hle_bits_sync(void)
{
  if (rstv_hle_bits_n == rstv_hle_n)
    return;
  memset(rstv_hle_bits, 0, sizeof(rstv_hle_bits));
  for (int k = 0; k < rstv_hle_n; k++) {
    const u32 i = (rstv_hle[k].pc & 0xFFFFF) >> 1;
    if ((rstv_hle[k].pc & 0x0FF00000) == 0x06000000)
      rstv_hle_bits[i >> 3] |= (u8)(1u << (i & 7));
  }
  rstv_hle_bits_n = rstv_hle_n;
}

// work RAM long read / write without checks (the caller checked the ranges)
static inline u32 rstv_hw_rd(u32 a) { return T2ReadLong(HighWram, a & 0xFFFFF); }
static inline void rstv_hw_wr(u32 a, u32 v) { T2WriteLong(HighWram, a & 0xFFFFF, v); }
static inline int rstv_hw_in(u32 a, u32 n) { return (a & 0x0FF00000) == 0x06000000 && (a >> 29) <= 1 && (a & 0xFFFFF) + n <= 0x100000; }

// ADR-047: the SGL vertex loop. Software pipelined: each pass computes z (row 2), starts the 64/32 division
// (DVDNTH = [r15 + 4], DVSR = max(z, r8), DVDNTL = [r15]), projects the previous vertex x, y with the previous
// quotient r7 and builds its clip flags, then computes x, y of this vertex (rows 0, 1). r10 = vertex (3 longs, walks
// back 12 a pass), r6 = matrix 3 x 4, r11 = output (walks back 16 a pass), r9 = count. Returns the exit PC.
// SGL keeps |x|, |y| <= 512 (0606EAC0 / 0606EB8C): halve both until inside, then 4 bisection steps along the larger one.
// a = x (r3 or r2), b = y (r5); r0, r1, r4, r7 are its scratch registers (their final values are visible to the game).
static void rstv_sgl_clip(u32 *pa, u32 *pb, u32 *pr0, u32 *pr1, u32 *pr4, u32 *pr7, u32 lim)
{
  s32 a = (s32)*pa, b = (s32)*pb, r0 = (s32)lim, r1 = 0;
  while (a > r0) { r1 = 1; a >>= 1; b >>= 1; }
  while (b > r0) { r1 = 1; a >>= 1; b >>= 1; }
  for (;;) {                                   // 0606EADC: the loop goes back to the neg
    r0 = -r0;
    if (a >= r0) break;
    r1 = 1; a >>= 1; b >>= 1;
  }
  while (!(b >= r0)) { r1 = 1; a >>= 1; b >>= 1; }
  if (r1 == 0) {
    *pa = (u32)a; *pb = (u32)b; *pr0 = (u32)r0; *pr1 = 0;
    return;                                    // r4, r7 untouched
  }
  s32 r4 = a, r7 = b;
  r0 = a < 0 ? -a : a;
  r1 = b < 0 ? -b : b;
  const int bmaj = r1 >= r0;
  r0 = 4;
  if (!bmaj) {
    if (a >= 0) {
      r1 = (s32)lim;
      do { const s32 t = (s32)((u32)a + (u32)r4); if (!(t > r1)) { b = (s32)((u32)b + (u32)r7); a = t; } r7 >>= 1; r0--; r4 >>= 1; } while (r0);
    } else {
      r1 = -(s32)lim;
      do { const s32 t = (s32)((u32)a + (u32)r4); if (t >= r1) { b = (s32)((u32)b + (u32)r7); a = t; } r7 >>= 1; r0--; r4 >>= 1; } while (r0);
    }
  } else {
    if (b >= 0) {
      r1 = (s32)lim;
      do { const s32 t = (s32)((u32)b + (u32)r7); if (!(t > r1)) { a = (s32)((u32)a + (u32)r4); b = t; } r4 >>= 1; r0--; r7 >>= 1; } while (r0);
    } else {
      r1 = -(s32)lim;
      do { const s32 t = (s32)((u32)b + (u32)r7); if (t >= r1) { a = (s32)((u32)a + (u32)r4); b = t; } r4 >>= 1; r0--; r7 >>= 1; } while (r0);
    }
  }
  *pa = (u32)a; *pb = (u32)b; *pr0 = (u32)r0; *pr1 = (u32)r1; *pr4 = (u32)r4; *pr7 = (u32)r7;
}

// two copies of the routine in sasissu / sanjeon (different work area offsets, different ending)
struct RstvSglDesc {
  u32 head, code, lit;               // loop head PC, start of the checked code, address of the 512 literal
  const RstvCodeSig *sig;
  u16 ocx, ocy, o388, o378;          // GBR offsets: screen centre x, y; bias; clip mask
  int epi;                           // 0: 0606ED86 (pop r7, rts, r0 = r11); 1: 060727CA (r10 = r11, pop r4, r6 -= 32, bra 06072814)
  u32 epi_pc;                        // epi 1: the bra target
};
// ADR-100: twcup98 has the same two copies (the same SGL code at other addresses: master 0605CD86, slave 060631DE)
static const RstvSglDesc rstv_sgl_desc[4] = {
  { 0x0606ECB6, 0x0606EAC0, 0x0606ED94, &rstv_sgl_vtx_sig, 0x90, 0x92, 0x388, 0x378, 0, 0 },
  { 0x060726FA, 0x060724C4, 0x060727DC, &rstv_sgl_vtx2_sig, 0x94, 0x96, 0x38C, 0x37C, 1, 0x06072814 },
  { 0x0605CD86, 0x0605CB90, 0x0605CE64, &rstv_sgl_vtx_sig, 0x90, 0x92, 0x388, 0x378, 0, 0 },
  { 0x060631DE, 0x06062FA8, 0x060632C0, &rstv_sgl_vtx2_sig, 0x94, 0x96, 0x38C, 0x37C, 1, 0x060632F8 },
};
extern "C" void rstv_div64_ext(s64 dividend, s32 divisor, s64 *q, s32 *r);
// ADR-047: the SGL vertex routine from its loop head (0606ECB6) to its return, clip helpers included.
// Software pipelined: each pass computes z (row 2), starts the 64/32 division (DVDNTH = [r15 + 4], DVSR = max(z, r8),
// DVDNTL = [r15]), projects the previous vertex x, y with the previous quotient r7 and builds its flags, then computes
// x, y of this vertex (rows 0, 1). r10 = vertex (walks back 12 a pass), r6 = matrix 3 x 4, r11 = output (back 16), r9 = count.
int DynarecSh2::RstvHleSglVtx(const RstvSglDesc *dsc, u32 *cyc_out)
{
  u32 *R = GetGenRegPtr();
  const u32 sr = GET_SR();
  if (sr & 2)
    return 0; // S bit: saturating MAC, not handled
  const u32 gbr = GET_GBR();
  const u32 n = R[9];
  if (n == 0 || n > 4096 || R[14] != 0xFFFFFF00)
    return 0;
  rstv_hle_items[0] += n;
  if (!rstv_hw_in(R[10] - 12 * n - 24, 12 * n + 48) || !rstv_hw_in(R[6] - 48, 112) || !rstv_hw_in(R[11] - 16 * n - 32, 16 * n + 64) ||
      !rstv_hw_in(R[15], 16) || !rstv_hw_in(gbr + 0x90, 0x300))
    return 0;
  u32 r0 = R[0], r1 = R[1], r2 = R[2], r3 = R[3], r4 = R[4], r5 = R[5], r6 = R[6], r7 = R[7], r9 = R[9], r10 = R[10], r11 = R[11];
  u32 r15 = R[15];
  const u32 r8 = R[8], r12 = R[12], r13 = R[13];
  const s32 cx = (s16)T2ReadWord(HighWram, (gbr + dsc->ocx) & 0xFFFFF), cy = (s16)T2ReadWord(HighWram, (gbr + dsc->ocy) & 0xFFFFF);
  const u32 g388 = rstv_hw_rd(gbr + dsc->o388), g378 = rstv_hw_rd(gbr + dsc->o378);
  const u32 s0 = rstv_hw_rd(r15), s4 = rstv_hw_rd(r15 + 4);
  const u32 lim = rstv_hw_rd(dsc->lit);
  s64 mac;
  u32 T, cyc = 0;
  // the matrix (3 rows of x, y, z, t at r6 - 32) does not move during the call; r6 walks inside it and is back at the
  // start after each pass
  const u32 R6 = r6;
  s32 m[12];
  for (int k = 0; k < 12; k++)
    m[k] = (s32)rstv_hw_rd(R6 - 32 + 4 * k);
  // division unit: dividend = [r15 + 4] : [r15], divisor = max(z, r8) per pass; computed here, registers written at the end
  Onchip_struct *oc = &ctx_->onchip;
  const int fastdiv = !(oc->DVCR & 2);
  const s64 dvd = (s64)(((u64)s4 << 32) | (u64)s0);
  u32 dvsr = oc->DVSR, dvh = oc->DVDNTH, dvl = oc->DVDNTL, dvcr = oc->DVCR;
#define DIVIDE(divisor) do { \
    if (fastdiv && (divisor) != 0) { \
      s64 q_; s32 rm_; rstv_div64_ext(dvd, (s32)(divisor), &q_, &rm_); \
      if (q_ > 0x7FFFFFFF) { dvcr |= 1; dvl = 0x7FFFFFFF; dvh = 0xFFFFFFFE; } \
      else if ((s32)(q_ >> 32) < -1) { dvcr |= 1; dvl = 0x80000000; dvh = 0xFFFFFFFE; } \
      else { dvl = (u32)q_; dvh = (u32)rm_; } \
      dvsr = (divisor); \
    } else { \
      oc->DVSR = dvsr; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr; \
      OnchipWriteLong(0x110, s4); OnchipWriteLong(0x100, (divisor)); OnchipWriteLong(0x114, s0); \
      dvsr = oc->DVSR; dvh = oc->DVDNTH; dvl = oc->DVDNTL; dvcr = oc->DVCR; \
    } } while (0)
  u32 v0, v1, v2; // this pass's vertex
#define DOT(row) ((s64)(s32)v0 * m[(row) * 4] + (s64)(s32)v1 * m[(row) * 4 + 1] + (s64)(s32)v2 * m[(row) * 4 + 2])
#define XT(m, n) (((u32)(m) << 16) | ((u32)(n) >> 16))
  for (;;) {
    v0 = rstv_hw_rd(r10); v1 = rstv_hw_rd(r10 + 4); v2 = rstv_hw_rd(r10 + 8);
    mac = DOT(2);                              // clrmac, 3 x mac.l (row 2)
    r1 = (u32)m[11];                           // mov.l @(4,r6),r1
    r2 = (u32)(mac >> 32); r3 = (u32)mac;
    r3 = XT(r2, r3);
    r3 += r1;
    T = (s32)r3 >= (s32)r8;
    r0 = s0;                                   // delay slot mov.l @r15,r0
    if (!T) r3 = r8;
    r11 -= 16;
    DIVIDE(r3);                                // DVDNTH = [r15 + 4], DVSR = r3, DVDNTL = [r15]
    mac = (s64)(s32)r4 * (s64)(s32)r7;         // dmuls.l r4,r7
    r0 = (u32)cx;
    r4 >>= 1;                                  // shlr r4
    rstv_hw_wr(r11 + 0xC, r3);
    r3 = (u32)(mac >> 32);
    mac = (s64)(s32)r5 * (s64)(s32)r7;         // dmuls.l r5,r7
    r0 += r3;
    rstv_hw_wr(r11 + 0x18, r5);
    r1 = r0;
    r0 = g388 + r1;
    T = r0 >= r12;                             // cmp/hs r12,r0
    { const u64 t = (u64)r4 + r4 + T; r4 = (u32)t; T = (u32)(t >> 32); } // addc r4,r4
    r5 = (u32)(mac >> 32);
    r0 = (u32)cy;
    mac = DOT(0);                              // clrmac, 3 x mac.l (row 0)
    r0 += r5;
    r1 = r0;
    r0 = g388 + r1;
    T = r0 >= r13;
    r0 = T;                                    // movt
    r4 |= r0;
    rstv_hw_wr(r11 + 0x14, r4);
    r2 = (u32)m[3];                            // mov.l @r6+,r2
    r0 = g378;
    r4 &= r0;
    T = r4 & 1; r4 = (u32)((s32)r4 >> 1);      // shar r4
    cyc += 52;
    if (T) {                                   // bra 0606EAC0 .. bra 0606ED22
      rstv_sgl_clip(&r3, &r5, &r0, &r1, &r4, &r7, lim);
      cyc += 40;
    }
    r5 <<= 16;                                 // 0606ED22
    r1 = (u32)(mac >> 32); r4 = (u32)mac;
    mac = DOT(1);                              // clrmac, 3 x mac.l (row 1)
    r5 = XT(r3, r5);
    rstv_hw_wr(r11 + 0x10, r5);
    r4 = XT(r1, r4);
    r4 += r2;
    r2 = (u32)m[7];                            // mov.l @r6,r2
    r10 -= 12;                                 // net of the three passes over the vertex
    r1 = (u32)(mac >> 32); r5 = (u32)mac;
    r7 = dvl;                                  // DVDNTL (read as DVDNTUL, same value)
    r5 = XT(r1, r5);
    r9--;                                      // dt r9
    r5 += r2;                                  // delay slot
    cyc += 36;
    if (r9 == 0) break;
  }
  // 0606ED4E: the last vertex
  mac = (s64)(s32)r4 * (s64)(s32)r7;           // dmuls.l r4,r7
  r0 = (u32)cx;
  r4 >>= 1;                                    // shlr r4
  rstv_hw_wr(r11 + 8, r5);
  r2 = (u32)(mac >> 32);
  mac = (s64)(s32)r5 * (s64)(s32)r7;           // dmuls.l r5,r7
  r0 += r2;
  r1 = r0;
  r0 = g388 + r1;
  T = r0 >= r12;
  r5 = (u32)(mac >> 32);
  r0 = (u32)cy;
  { const u64 t = (u64)r4 + r4 + T; r4 = (u32)t; T = (u32)(t >> 32); }
  r0 += r5;
  r1 = r0;
  r0 = g388 + r1;
  T = r0 >= r13;
  r0 = T;
  r4 |= r0;
  rstv_hw_wr(r11 + 4, r4);
  r0 = g378;
  r4 &= r0;
  T = r4 & 1; r4 = (u32)((s32)r4 >> 1);
  if (T) {                                     // bra 0606EB8C .. bra 0606ED86
    rstv_sgl_clip(&r2, &r5, &r0, &r1, &r4, &r7, lim);
    cyc += 40;
  }
  r5 <<= 16;                                   // 0606ED86 / 060727CA
  r5 = XT(r2, r5);
  rstv_hw_wr(r11, r5);
  u32 npc;
  if (dsc->epi == 0) {
    r15 += 8;
    r7 = rstv_hw_rd(r15); r15 += 4;            // mov.l @r15+,r7
    r0 = r11;                                  // rts delay slot
    npc = GET_PR();
  } else {
    r10 = r11;                                 // mov r11,r10
    r15 += 8;
    r4 = rstv_hw_rd(r15); r15 += 4;            // mov.l @r15+,r4
    r6 -= 32;                                  // bra 06072814 delay slot
    npc = dsc->epi_pc;
  }
  cyc += 30;
#undef DOT
#undef DIVIDE
#undef XT
  oc->DVSR = dvsr; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr;
  r6 = R6 - (dsc->epi ? 32 : 0);
  R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[4] = r4; R[5] = r5; R[6] = r6; R[7] = r7; R[9] = r9; R[10] = r10; R[11] = r11;
  R[15] = r15;
  SET_MACH((u32)(mac >> 32)); SET_MACL((u32)mac);
  SET_SR((sr & ~1u) | T);
  rstv_hle_notify(R[11], 16 * n + 32);
  *cyc_out = cyc;
  return (int)npc;
}

// ADR-074: SGL on-screen test 0606AEA8 (leaf, master; about 111 calls a frame in sanjeon's 3D mini games).
// r4 = point (x, y, z), r5 = radius, matrix [gbr + 0x1C]. z = row 2 + t; the near limit is [gbr + 0x68] >> [gbr + 0xAC]
// (dt loop, shlr in the delay slot). z < near: r0 = -1. Else DVSR = z starts (s16)(k >> 16) : k << 16 / z (k = [gbr + 0x68]),
// z - near above [gbr + 0x70] << 16 (unsigned): r0 = -2. Else x, y = rows 0, 1, scaled by the quotient (>> 16) with the
// radius r5 / 2 (dmulu) against [gbr + 0x78 / 0x7C] (x) and [gbr + 0x7A / 0x7E] (y): outside -> -2, inside -> z - near.
// r0 is pushed and popped around the loop (the word below r15 keeps k).
static const RstvCodeSig rstv_onscr_sig = { 99, 0x87dda37eu, -1, -1 }; // 0606AEA8 .. 0606AF6D
int DynarecSh2::RstvHleSglOnScr(u32 *cyc_out)
{
  u32 *R = GetGenRegPtr();
  const u32 sr = GET_SR();
  if (sr & 2)
    return 0; // S bit: saturating MAC
  const u32 gbr = GET_GBR();
  if (!rstv_hw_in(gbr, 0xB0))
    return 0;
  const u32 M = rstv_hw_rd(gbr + 0x1C);
  const u32 p = R[4];
  const u32 sp = R[15];
  if (!rstv_hw_in(M, 48) || !rstv_hw_in(p, 12) || (p & 3) || (M & 3) || !rstv_hw_in(sp - 4, 4))
    return 0;
  const s32 sh = (s32)(s8)T2ReadByte(HighWram, (gbr + 0xAC) & 0xFFFFF);
  if (sh <= 0 || sh > 32)
    return 0; // dt loop would run 2^32 times
#define OSRD(a) rstv_hw_rd(a)
#define OSXT(m, l) (((u32)(m) << 16) | ((u32)(l) >> 16))
#define OSW(o) ((u32)(s32)(s16)T2ReadWord(HighWram, (gbr + (o)) & 0xFFFFF))
  const s32 v0 = (s32)OSRD(p), v1 = (s32)OSRD(p + 4), v2 = (s32)OSRD(p + 8);
  s64 mac = (s64)v0 * (s32)OSRD(M + 0x20) + (s64)v1 * (s32)OSRD(M + 0x24) + (s64)v2 * (s32)OSRD(M + 0x28);
  const u32 k = OSRD(gbr + 0x68);
  u32 r1 = OSRD(M + 0x2C);
  const u32 dvdh = (u32)(s32)(s16)(k >> 16);
  OnchipWriteLong(0x110, dvdh);                // mov.l r2,@(0x10,r7): DVDNTH
  rstv_hw_wr(sp - 4, k);                       // push r0 (popped in the delay slot below)
  u32 r3 = OSXT((u32)(mac >> 32), (u32)mac) + r1; // z
  u32 r2 = sh >= 32 ? 0 : k >> sh;
  u32 r0 = k, r4 = p + 12, r5 = R[5], r6 = M + 0x2C, r7 = 0xFFFFFF00, T;
  u32 cyc = 26 + 2 * (u32)sh;
  if (!((s32)r3 >= (s32)r2)) {                 // cmp/ge r2,r3 / bf/s: too near
    r0 = 0xFFFFFFFF;
    T = 0;
    cyc += 2;
  } else {
    // DVSR = z, DVDNTL = k << 16: the 64 / 32 division (DVDNTH set above)
    Onchip_struct *oc = &ctx_->onchip;
    const u32 dvdl = k << 16;
    if (!(oc->DVCR & 2) && r3 != 0) {
      s64 q_; s32 rm_;
      rstv_div64_ext((s64)(((u64)dvdh << 32) | dvdl), (s32)r3, &q_, &rm_);
      u32 dvl, dvh, dvcr = oc->DVCR;
      if (q_ > 0x7FFFFFFF) { dvcr |= 1; dvl = 0x7FFFFFFF; dvh = 0xFFFFFFFE; }
      else if ((s32)(q_ >> 32) < -1) { dvcr |= 1; dvl = 0x80000000; dvh = 0xFFFFFFFE; }
      else { dvl = (u32)q_; dvh = (u32)rm_; }
      oc->DVSR = r3; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr;
    } else {
      OnchipWriteLong(0x100, r3); OnchipWriteLong(0x114, dvdl);
    }
    r3 -= r2;
    mac = (s64)v0 * (s32)OSRD(M) + (s64)v1 * (s32)OSRD(M + 4) + (s64)v2 * (s32)OSRD(M + 8); // row 0
    r0 = OSW(0x70) << 16;
    r4 = p + 12;
    r6 = M + 12;
    cyc += 15;
    if (r3 > r0) {                             // cmp/hi r0,r3: too far
      r0 = 0xFFFFFFFE;
      T = 1;
      cyc += 2;
    } else {
      r2 = OSRD(M + 0x0C);
      r0 = (u32)(mac >> 32);
      r1 = OSXT(r0, (u32)mac) + r2;            // x
      mac = (s64)v0 * (s32)OSRD(M + 0x10) + (s64)v1 * (s32)OSRD(M + 0x14) + (s64)v2 * (s32)OSRD(M + 0x18); // row 1
      r4 = OSRD(M + 0x1C);
      r6 = M + 0x20;
      r0 = (u32)(mac >> 32);
      r2 = OSXT(r0, (u32)mac) + r4;            // y
      r5 = (u32)((s32)r5 >> 1);                // shar r5
      r7 = oc->DVDNTUL;                        // mov.l @(0x1C,r7),r7: quotient
      mac = (s64)(s32)r1 * (s32)r7;            // dmuls.l r1,r7
      r6 = OSW(0x78);
      r0 = (u32)(mac >> 32);
      r1 = OSXT(r0, (u32)mac);                 // screen x
      mac = (s64)((u64)r5 * (u64)r7);          // dmulu.l r5,r7
      r0 = OSW(0x7C);
      r6 <<= 16;
      r4 = (u32)(mac >> 32);
      r5 = OSXT(r4, (u32)mac);                 // screen radius
      mac = (s64)(s32)r2 * (s32)r7;            // dmuls.l r2,r7
      r1 += r5;
      cyc += 30;
      T = (s32)r1 >= (s32)r6;
      if (!T) {                                // bf 0606AF66
        r0 = 0xFFFFFFFE;
        cyc += 2;
      } else {
        r0 <<= 16;
        r1 -= r5; r1 -= r5;
        T = (s32)r1 > (s32)r0;
        cyc += 5;
        if (T) {
          r0 = 0xFFFFFFFE;
          cyc += 2;
        } else {
          r1 = (u32)(mac >> 32);
          r0 = OSW(0x7A);
          r2 = OSXT(r1, (u32)mac) + r5;        // screen y + radius
          r0 <<= 16;
          T = (s32)r2 >= (s32)r0;
          cyc += 6;
          if (!T) {
            r0 = 0xFFFFFFFE;
            cyc += 2;
          } else {
            r0 = OSW(0x7E) << 16;
            r2 -= r5; r2 -= r5;
            T = (s32)r2 >= (s32)r0;
            cyc += 6;
            r0 = T ? 0xFFFFFFFE : r3;
            cyc += 2;
          }
        }
      }
    }
  }
#undef OSRD
#undef OSXT
#undef OSW
  R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[4] = r4; R[5] = r5; R[6] = r6; R[7] = r7;
  SET_MACH((u32)(mac >> 32)); SET_MACL((u32)mac);
  SET_SR((sr & ~1u) | T);
  *cyc_out = cyc;
  return 1;
}

// ADR-055: thuntk perspective vertex routine 0602FEF8 (leaf, saves r8-r14 and MAC on the stack).
// Arguments: r4 = matrix 3 x (x, y, z, t), r5 = vertices (x, y, z), r6 = output (x, y, z) per vertex, r7 = screen (x, y)
// halfwords; stack: [sp] = count, [sp + 4] -> (r9, r10) focal x, y, [sp + 8] -> (r11, r12) x, y bounds,
// [sp + 12] -> (r13, r14) near, far. Per vertex: z = row 2; outside [near, far] -> out z = 0. Else out z = z, the 64/32
// division r9:0 / z runs while x = row 0 is checked against (r11 * z) >> 16 (outside -> out z = 0, next vertex);
// screen x = (q * x) >> 32, then the same for y (outside -> out z = 0 but the screen y is still written).
static const RstvCodeSig rstv_tk_vtx_sig = { 136, 0x1cfed398u, -1, -1 };
int DynarecSh2::RstvHleTkVtx(u32 *cyc_out)
{
  u32 *R = GetGenRegPtr();
  u32 sr = GET_SR();
  if (sr & 2)
    return 0; // S bit: saturating MAC, not handled
  const u32 sp = R[15];
  if (!rstv_hw_in(sp - 36, 52))
    return 0;
  const u32 n = rstv_hw_rd(sp);
  if (n == 0 || n > 4096)
    return 0;
  const u32 p9 = rstv_hw_rd(sp + 4), p11 = rstv_hw_rd(sp + 8), p13 = rstv_hw_rd(sp + 12);
  const u32 r4 = R[4];
  u32 r5 = R[5], r6 = R[6], r7 = R[7];
  if (!rstv_hw_in(p9, 8) || !rstv_hw_in(p11, 8) || !rstv_hw_in(p13, 8) || !rstv_hw_in(r4, 48) || !rstv_hw_in(r5, 12 * n) ||
      !rstv_hw_in(r6, 12 * n) || !rstv_hw_in(r7, 4 * n))
    return 0;
  // pushes (the stack below sp holds them afterwards, as on the SH-2)
  rstv_hw_wr(sp - 4, R[14]); rstv_hw_wr(sp - 8, R[13]); rstv_hw_wr(sp - 12, R[12]); rstv_hw_wr(sp - 16, R[11]);
  rstv_hw_wr(sp - 20, R[10]); rstv_hw_wr(sp - 24, R[9]); rstv_hw_wr(sp - 28, R[8]); rstv_hw_wr(sp - 32, GET_MACH());
  rstv_hw_wr(sp - 36, GET_MACL());
  const u32 r9 = rstv_hw_rd(p9), r10 = rstv_hw_rd(p9 + 4), r11 = rstv_hw_rd(p11), r12 = rstv_hw_rd(p11 + 4);
  const s32 r13 = (s32)rstv_hw_rd(p13), r14 = (s32)rstv_hw_rd(p13 + 4);
  u32 r0 = R[0], r1 = R[1], r2 = R[2], r3 = R[3];
  u32 T = sr & 1, cyc = 35;
  s64 mac;
  Onchip_struct *oc = &ctx_->onchip;
  const int fastdiv = !(oc->DVCR & 2);
  u32 dvsr = oc->DVSR, dvh = oc->DVDNTH, dvl = oc->DVDNTL, dvcr = oc->DVCR;
  // DVSR = divisor, DVDNTH = hi, DVDNTL = 0 (starts the 64/32 division)
#define TKDIV(hi, divisor) do { \
    if (fastdiv && (divisor) != 0) { \
      s64 q_; s32 rm_; rstv_div64_ext((s64)((u64)(hi) << 32), (s32)(divisor), &q_, &rm_); \
      if (q_ > 0x7FFFFFFF) { dvcr |= 1; dvl = 0x7FFFFFFF; dvh = 0xFFFFFFFE; } \
      else if ((s32)(q_ >> 32) < -1) { dvcr |= 1; dvl = 0x80000000; dvh = 0xFFFFFFFE; } \
      else { dvl = (u32)q_; dvh = (u32)rm_; } \
      dvsr = (divisor); \
    } else { \
      oc->DVSR = dvsr; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr; \
      OnchipWriteLong(0x100, (divisor)); OnchipWriteLong(0x110, (hi)); OnchipWriteLong(0x114, 0); \
      dvsr = oc->DVSR; dvh = oc->DVDNTH; dvl = oc->DVDNTL; dvcr = oc->DVCR; \
    } } while (0)
  // clrmac + 3 x mac.l over a matrix row and the vertex (read now: the output may overwrite the vertex in place)
#define TKDOT(row) ((s64)(s32)rstv_hw_rd(r5) * (s32)rstv_hw_rd(r4 + (row)) + \
    (s64)(s32)rstv_hw_rd(r5 + 4) * (s32)rstv_hw_rd(r4 + (row) + 4) + (s64)(s32)rstv_hw_rd(r5 + 8) * (s32)rstv_hw_rd(r4 + (row) + 8))
#define TKXT(m, l) (((u32)(m) << 16) | ((u32)(l) >> 16))
  for (u32 i = 0; i < n; i++) {
    mac = TKDOT(32);                                       // row 2
    r1 = (u32)(mac >> 32); r0 = TKXT(r1, (u32)mac);
    r3 = rstv_hw_rd(r4 + 44) + r0;                         // z
    if (r13 > (s32)r3 || !(r14 >= (s32)r3)) {              // cmp/gt r3,r13 / cmp/ge r3,r14
      T = r13 > (s32)r3;
      r0 = 0;
      rstv_hw_wr(r6 + 8, 0);
      cyc += 30;
    } else {
      rstv_hw_wr(r6 + 8, r3);
      r0 = 0xFFFFFF00;
      r2 = 0;
      TKDIV(r9, r3);
      mac = TKDOT(0);                                      // row 0
      r1 = (u32)(mac >> 32); r0 = TKXT(r1, (u32)mac);
      r2 = rstv_hw_rd(r4 + 12) + r0;                       // x
      rstv_hw_wr(r6, r2);
      mac = (s64)(s32)r11 * (s32)r3;                       // dmuls.l r11,r3
      r1 = (u32)(mac >> 32); r0 = TKXT(r1, (u32)mac);
      int ok;
      if (r0 == 0) { T = 1; ok = 1; }
      else {
        T = (s32)r2 > (s32)r0;
        if (T) ok = 0;
        else { r0 = (u32)-(s32)r0; T = (s32)r2 > (s32)r0; ok = T; }
      }
      if (!ok) {                                           // 0602FF82
        r0 = 0;
        rstv_hw_wr(r6 + 8, 0);
        r0 = 0xFFFFFF00;
        r1 = dvl;
        cyc += 55;
      } else {                                             // 0602FF94
        r0 = 0xFFFFFF00;
        r1 = dvl;
        mac = (s64)(s32)r1 * (s32)r2;
        r1 = (u32)(mac >> 32);
        T2WriteWord(HighWram, r7 & 0xFFFFF, (u16)r1);
        r2 = 0;
        TKDIV(r10, r3);
        mac = TKDOT(16);                                   // row 1
        r1 = (u32)(mac >> 32); r0 = TKXT(r1, (u32)mac);
        r2 = rstv_hw_rd(r4 + 28) + r0;                     // y
        rstv_hw_wr(r6 + 4, r2);
        mac = (s64)(s32)r12 * (s32)r3;                     // dmuls.l r12,r3
        r1 = (u32)(mac >> 32); r0 = TKXT(r1, (u32)mac);
        if (r0 == 0) T = 1;
        else {
          T = (s32)r2 > (s32)r0;
          if (T) { r0 = 0; rstv_hw_wr(r6 + 8, 0); }
          else {
            r0 = (u32)-(s32)r0;
            T = (s32)r2 >= (s32)r0;
            if (!T) { r0 = 0; rstv_hw_wr(r6 + 8, 0); }
          }
        }
        r0 = 0xFFFFFF00;                                   // 0602FFD6
        r1 = dvl;
        mac = (s64)(s32)r1 * (s32)r2;
        r0 = (u32)(mac >> 32);
        T2WriteWord(HighWram, (r7 + 2) & 0xFFFFF, (u16)r0);
        cyc += 100;
      }
    }
    r5 += 12; r6 += 12; r7 += 4;
  }
#undef TKDIV
#undef TKDOT
#undef TKXT
  (void)T;
  oc->DVSR = dvsr; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr;
  R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[5] = r5; R[6] = r6; R[7] = r7;
  SET_SR(sr | 1u);                                         // dt r8 left T = 1; r8-r14 and MAC are restored by the epilogue
  *cyc_out = cyc;
  return 1;
}

// ADR-056: sanjeon interpolation loop 0600B288..0600B33E (hook at its test 0600B33A), with the leaf routines it calls:
// 0606AE94 fixdiv (r0 = (r5 << 16) / r4 through the division unit), 0606AF70 (3 x 4 matrix at [gbr + 0x1C] times the
// vector r4, r5, r6 pushed on the stack, out at r7) and r14 = 0606AE88 fixmul (r0 = (r4 * r5) >> 16). For each entry i of
// the table [r12] (count [r12 + 4], 12 bytes each): t = fixdiv([sp + 4], e[1] - [sp + 0x4C]); p = M * e (to sp + 0x14);
// out[i] (table [r10]) = t * p[c] + (1 - t) * e[c] (c = 1: (1 - t) * ([sp + 0x40] * e[1])), via the pointers at [sp + 8 + 4c].
static const RstvCodeSig rstv_sj_loop_sig = { 92, 0xb2822a9fu, -1, -1 };
static const RstvCodeSig rstv_sj_lit_sig = { 6, 0xbaed46d0u, -1, -1 }; // 0600B42C: fixdiv, 0x10000, matvec
static const RstvCodeSig rstv_sj_fix_sig = { 16, 0xc289419au, -1, -1 }; // 0606AE88 fixmul, 0606AE94 fixdiv
static const RstvCodeSig rstv_sj_mv_sig = { 22, 0xb8df1a50u, -1, -1 }; // 0606AF70
int DynarecSh2::RstvHleSjInterp(u32 *cyc_out)
{
  u32 *R = GetGenRegPtr();
  const u32 sr = GET_SR();
  if (sr & 2)
    return 0;
  if (R[14] != 0x0606AE88)
    return 0;
  const u32 sp = R[15], gbr = GET_GBR(), r10 = R[10], r12 = R[12];
  if (!rstv_hw_in(sp - 12, 0x60) || !rstv_hw_in(r12, 8) || !rstv_hw_in(r10, 4) || !rstv_hw_in(gbr + 0x1C, 4))
    return 0;
  u32 r8 = R[8];
  const u32 count = rstv_hw_rd(r12 + 4);
  u32 r0 = R[0], r1 = R[1], r2 = R[2], r3, r4 = R[4], r5 = R[5], r6 = R[6], r7 = R[7], r9 = R[9], r11 = R[11], r13 = R[13];
  u32 pr = GET_PR();
  s64 mac = ((s64)GET_MACH() << 32) | GET_MACL();
  u32 cyc = 4;
  if (r8 < count) {
    const u32 n = count - r8;
    const u32 base = rstv_hw_rd(r12), outb = rstv_hw_rd(r10), M = rstv_hw_rd(gbr + 0x1C);
    if (n > 4096 || !rstv_hw_in(base + 12 * r8, 12 * n) || !rstv_hw_in(outb + 12 * r8, 12 * n) || !rstv_hw_in(M, 48))
      return 0;
    u32 ptr[3];
    for (int c = 0; c < 3; c++) {
      ptr[c] = rstv_hw_rd(sp + 8 + 4 * c);
      if (!rstv_hw_in(ptr[c], 4))
        return 0;
    }
  }
  Onchip_struct *oc = &ctx_->onchip;
  const int fastdiv = !(oc->DVCR & 2);
  u32 dvsr = oc->DVSR, dvh = oc->DVDNTH, dvl = oc->DVDNTL, dvcr = oc->DVCR;
#define SJMUL(a, b) (mac = (s64)(s32)(a) * (s32)(b), (((u32)(mac >> 32)) << 16) | ((u32)mac >> 16))
  while (r8 < count) {
    r13 = r8 * 12;
    u32 e = rstv_hw_rd(r12) + r13;
    // fixdiv([sp + 4], e[1] - [sp + 0x4C])
    r4 = rstv_hw_rd(sp + 4);
    r5 = rstv_hw_rd(e + 4) - rstv_hw_rd(sp + 0x4C);
    r3 = 0xFFFFFF00;
    r2 = (u32)(s32)(s16)(r5 >> 16);
    r5 <<= 16;
    if (fastdiv && r4 != 0) {
      s64 q_; s32 rm_;
      rstv_div64_ext((s64)(((u64)r2 << 32) | r5), (s32)r4, &q_, &rm_);
      if (q_ > 0x7FFFFFFF) { dvcr |= 1; dvl = 0x7FFFFFFF; dvh = 0xFFFFFFFE; }
      else if ((s32)(q_ >> 32) < -1) { dvcr |= 1; dvl = 0x80000000; dvh = 0xFFFFFFFE; }
      else { dvl = (u32)q_; dvh = (u32)rm_; }
      dvsr = r4;
    } else {
      oc->DVSR = dvsr; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr;
      OnchipWriteLong(0x100, r4); OnchipWriteLong(0x110, r2); OnchipWriteLong(0x114, r5);
      dvsr = oc->DVSR; dvh = oc->DVDNTH; dvl = oc->DVDNTUL; dvcr = oc->DVCR;
    }
    r0 = dvl;                                              // mov.l @(0x1C,r3),r0 (DVDNTUL)
    r9 = 0x10000;
    r7 = sp;
    r11 = r0;
    r7 += 0x14;
    r9 -= r11;
    r6 = rstv_hw_rd(r12) + r13;
    rstv_hw_wr(sp, r6);
    r6 = rstv_hw_rd(r6 + 8);
    r5 = rstv_hw_rd(rstv_hw_rd(sp) + 4);
    r4 = rstv_hw_rd(rstv_hw_rd(sp));
    // 0606AF70: push r6, r5, r4; 3 rows of M (x, y, z, t) times the pushed vector, out at r7
    rstv_hw_wr(sp - 4, r6); rstv_hw_wr(sp - 8, r5); rstv_hw_wr(sp - 12, r4);
    {
      u32 m = rstv_hw_rd(gbr + 0x1C);
      r1 = 3;
      do {
        mac = (s64)(s32)rstv_hw_rd(sp - 12) * (s32)rstv_hw_rd(m) + (s64)(s32)rstv_hw_rd(sp - 8) * (s32)rstv_hw_rd(m + 4) +
          (s64)(s32)rstv_hw_rd(sp - 4) * (s32)rstv_hw_rd(m + 8);
        r5 = rstv_hw_rd(m + 12);
        m += 16;
        r2 = (u32)(mac >> 32);
        r3 = ((r2 << 16) | ((u32)mac >> 16)) + r5;
        rstv_hw_wr(r7, r3);
        r7 += 4;
      } while (--r1);
      r0 = m;
      r4 = sp - 12;
    }
    for (int c = 0; c < 3; c++) {
      r2 = rstv_hw_rd(r10) + r13 + 4 * c;
      rstv_hw_wr(sp - 4, r2);                              // push the output address
      r5 = rstv_hw_rd(rstv_hw_rd(sp + 8 + 4 * c));
      r4 = r11;
      r0 = SJMUL(r4, r5);                                  // t * p[c]
      rstv_hw_wr(sp - 8, r0);
      r5 = rstv_hw_rd(r12) + r13;
      if (c == 1) {
        r4 = rstv_hw_rd(sp + 0x40);
        r5 = rstv_hw_rd(r5 + 4);
        r0 = SJMUL(r4, r5);
        r5 = r0;
      } else
        r5 = rstv_hw_rd(r5 + 4 * c);
      r4 = r9;
      r0 = SJMUL(r4, r5);                                  // (1 - t) * ...
      r3 = rstv_hw_rd(sp - 8);
      r1 = rstv_hw_rd(sp - 4);
      r0 += r3;
      rstv_hw_wr(r1, r0);
    }
    r8++;
    cyc += 210;
    pr = 0x0600B330;
  }
#undef SJMUL
  r3 = count;                                              // 0600B33A: mov.l @(4,r12),r3 / cmp/hs r3,r8
  oc->DVSR = dvsr; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr;
  R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[4] = r4; R[5] = r5; R[6] = r6; R[7] = r7; R[8] = r8; R[9] = r9; R[11] = r11;
  R[13] = r13;
  SET_MACH((u32)(mac >> 32));
  SET_MACL((u32)mac);
  SET_PR(pr);
  SET_SR(sr | 1u);
  *cyc_out = cyc;
  return 1;
}

// ADR-060: SGL polygon loop (sasissu / sanjeon 0607284E .. 06072922, run by both CPUs; the slave spends most of its time
// here). One polygon per call, from the loop head to the next loop head (or the loop exit 06072916), so the HLE checker
// can compare it with the interpreter. Per polygon: the face normal (r7 stream) times the 3 x 4 matrix r6 goes to
// [r15 .. r15 + 8]; its dot with vertex 0 (r10 + index * 16 + 4) against [gbr + 0x384] culls back faces (double sided:
// the normal is negated); then the stage list of the attribute (table 06072B94, 4 words: sort, vertex order / colour,
// shading, 0) runs: average or farthest z (outside [gbr + 0xA0] of [r15 + 0xC]: dropped), the screen positions in an
// order that starts at an unclipped vertex (all clipped: the one nearest to the centre) with the command flip bits, the
// light level from the normal and [gbr + 0x58] through the table [gbr + 0x64]; the command (0x24 bytes at r11) is linked
// into the z bucket [gbr + 0x38][z >> 8]. Stages other than these leave the polygon to the SH-2 code.
// ADR-100: the same code sits at another address in twcup98 (slave 06063332): every address of the module is
// sanjeon's + rstv_poly_d (the stage table holds relocated pointers too)
static u32 rstv_poly_d;
#define RSTV_PA(a) ((u32)(a) + rstv_poly_d)
// ADR-101: twcup98's master runs another build of the loop (0605C04E): 4-word stage rows at 0605C14C (sort, drop =
// loop end 0605C10C, colour = the slave module's 06063E78 / 06063E84, writer 0605C100 or shading 0605C94C), stage
// jumps through r4, back faces against [gbr + 0x380], z range [gbr + 0x70] with r12, light at gbr + 0x4C, and the writer
// stacks commands downwards (r11 -= 0x24) and counts them in [r15 + 0x18] instead of linking z buckets.
// rstv_poly_m = 1 runs that build; its colour stage is the slave module (rstv_poly_d = the slave delta)
static int rstv_poly_m;
static const RstvCodeSig rstv_polym_main_sig = { 127, 0xc37868c0u, -1, -1 };  // 0605C04E .. 0605C14B
static const RstvCodeSig rstv_polym_table_sig = { 96, 0xaa47dfedu, -1, -1 }; // 0605C14C .. 0605C20B
static const RstvCodeSig rstv_polym_sort_sig = { 119, 0xf529637bu, -1, -1 }; // 0605CE6C .. 0605CF59
static const RstvCodeSig rstv_polym_shade_sig = { 63, 0xb200e5c5u, -1, -1 }; // 0605C94C .. 0605C9C9
// ADR-101: the near-plane sorts (32-bit z: dropped beyond the far limit or nearer than half the near limit, then
// every-vertex-clipped and screen-bounds tests): sanjeon / slave module 06072A0C .. 06072B93, master 0605CF5C .. 0605D0DF
static const RstvCodeSig rstv_poly_near_sig = { 196, 0x134665f2u, -1, -1 };
static const RstvCodeSig rstv_polym_near_sig = { 194, 0x608e7b3bu, -1, -1 };
static const RstvCodeSig rstv_poly_main_sig = { 107, 0x7e58368bu, -1, -1 };  // 0607284E .. 06072923
static const RstvCodeSig rstv_poly_sort_sig = { 97, 0xfce97eeeu, -1, -1 };   // 06072924 .. 060729E5
static const RstvCodeSig rstv_poly_stage_sig = { 203, 0x7ebdd247u, -1, -1 }; // 06073394 .. 06073529
#if !defined(__vita__)
#define POLY_FAIL(n) do { static int c_; if (c_++ < 3) printf("sglpoly fail %d\n", n); return 0; } while (0)
#else
#define POLY_FAIL(n) return 0
#endif
// all polygons from here on (one at a time when the HLE checker compares each with the interpreter); stops at a polygon
// whose stages are not handled here, which the SH-2 code then runs
int DynarecSh2::RstvHleSglPoly(u32 *cyc_out, u32 *npc)
{
#if !defined(__vita__)
  static int single = -1;
  if (single < 0)
    single = getenv("RSTV_HLECHK") != NULL;
#else
  const int single = 0;
#endif
  u32 total = 0, c = 0, np = 0;
  int n = 0;
  while (RstvHleSglPoly1(&c, &np)) {
    n++;
    rstv_hle_items[3]++;
    total += c;
    *npc = np;
    if (single || np != (rstv_poly_m ? 0x0605C04Eu : RSTV_PA(0x0607284E)) || total > 40000)
      break;
  }
  *cyc_out = total;
  return n > 0;
}

int DynarecSh2::RstvHleSglPoly1(u32 *cyc_out, u32 *npc)
{
  u32 *R = GetGenRegPtr();
  const u32 sr = GET_SR();
  if (sr & 2)
    POLY_FAIL(1);
  const u32 gbr = GET_GBR();
  u32 r0, r1, r2, r3, r4, r5, r9, r12, r13;
  u32 r6 = R[6], r7 = R[7], r8 = R[8], r10 = R[10], r11 = R[11], r14 = R[14], r15 = R[15];
  const int m = rstv_poly_m;
  if (GET_PR() != (m ? 0x0605C100u : RSTV_PA(0x060728F8)))
    POLY_FAIL(2);
  // ---- check everything the polygon reads or writes is high work RAM, and that its stages are ours ----
  if (!rstv_hw_in(r6, 48) || !rstv_hw_in(r7, 20) || !rstv_hw_in(r8, 12) || !rstv_hw_in(r15 - 4, 0x1C) || !rstv_hw_in(gbr, 0x388) ||
      !rstv_hw_in(r11, 0x24))
    POLY_FAIL(3);
#define SX16(v) ((u32)(s32)(s16)(v))
#define RW(a) SX16(T2ReadWord(HighWram, (a) & 0xFFFFF))
#define RB(a) ((u32)(s32)(s8)T2ReadByte(HighWram, (a) & 0xFFFFF))
#define WW(a, v) T2WriteWord(HighWram, (a) & 0xFFFFF, (u16)(v))
#define SWAPB(x) (((x) & 0xFFFF0000u) | (((x) & 0xFFu) << 8) | (((x) >> 8) & 0xFFu))
#define XT(m, n) (((u32)(m) << 16) | ((u32)(n) >> 16))
  const u32 vtx[4] = { r10 + (SX16(RW(r7 + 12)) << 4), r10 + (SX16(RW(r7 + 14)) << 4), r10 + (SX16(RW(r7 + 16)) << 4),
                       r10 + (SX16(RW(r7 + 18)) << 4) };
  for (int k = 0; k < 4; k++)
    if (!rstv_hw_in(vtx[k], 16))
      POLY_FAIL(4);
  const u32 attr = RW(r8);
  const u32 stl = (m ? 0x0605C14Cu : RSTV_PA(0x06072B94)) + ((attr & 0x7F) << 4);
  if (!rstv_hw_in(stl, 16))
    POLY_FAIL(5);
  // the master build's rows: sort, drop, colour, writer -> st1, st2 (colour), st3 (writer / shading) as in sanjeon
  u32 st1 = rstv_hw_rd(stl), st2 = rstv_hw_rd(stl + 4), st3 = rstv_hw_rd(stl + 8);
  int sk = -1; // sort kind: 0 average, 1 nearest, 2 farthest, 3 none
  if (m) {
    const u32 drop = st2;
    st2 = st3;
    st3 = rstv_hw_rd(stl + 12);
    sk = st1 == 0x0605CEB0u ? 0 : st1 == 0x0605CE6Cu ? 1 : st1 == 0x0605CEF0u ? 2 : st1 == 0x0605CF34u ? 3 :
      st1 == 0x0605CFA8u ? 4 : st1 == 0x0605CF5Cu ? 5 : st1 == 0x0605CFECu ? 6 : st1 == 0x0605D038u ? 3 : -1;
    if (drop != 0x0605C10Cu || sk < 0 || (st2 != RSTV_PA(0x06073394) && st2 != RSTV_PA(0x060733A0) && st2 != RSTV_PA(0x060733A8)) ||
        (st3 != 0x0605C100u && st3 != 0x0605C94Cu)) {
#if !defined(__vita__)
      static int c7; if (c7++ < 8) printf("sglpoly(m) stages %08x %08x %08x %08x\n", st1, drop, st2, st3);
#endif
      return 0;
    }
  } else
    sk = st1 == RSTV_PA(0x06072924) ? 0 : st1 == RSTV_PA(0x06072960) ? 1 : st1 == RSTV_PA(0x060729A4) ? 2 :
      st1 == RSTV_PA(0x060729E8) ? 3 : st1 == RSTV_PA(0x06072A0C) ? 4 : st1 == RSTV_PA(0x06072A50) ? 5 :
      st1 == RSTV_PA(0x06072A9C) ? 6 : st1 == RSTV_PA(0x06072AE8) ? 3 : -1;
  const int shade = m ? st3 == 0x0605C94Cu : st3 == RSTV_PA(0x060734AC);
  if (!m && (sk < 0 || (st2 != RSTV_PA(0x06073394) && st2 != RSTV_PA(0x060733A0) && st2 != RSTV_PA(0x060733A8)) ||
      (st3 != RSTV_PA(0x060728F8) && st3 != RSTV_PA(0x060734AC)))) {
#if !defined(__vita__)
    static int c6; if (c6++ < 8) printf("sglpoly stages %08x %08x %08x\n", st1, st2, st3);
#endif
    return 0;
  }
  const u32 bucket = rstv_hw_rd(gbr + 0x38), lt = rstv_hw_rd(gbr + 0x64);
  if ((!m && !rstv_hw_in(bucket, 0x400)) || (shade && !rstv_hw_in(lt, 0xC00)))
    POLY_FAIL(7);
  if (st2 == RSTV_PA(0x06073394)) {
    const u32 ct = rstv_hw_rd(r15 + 0x14) + (RW(r8 + 2) << 3);
    if (!rstv_hw_in(ct, 4))
      POLY_FAIL(8);
  }
  // ---- run it (nothing below can fail) ----
  s64 mac;
  u32 T;
  u32 cyc = 0;
#define MACL(pa, pb) do { mac += (s64)(s32)rstv_hw_rd(pa) * (s64)(s32)rstv_hw_rd(pb); (pa) += 4; (pb) += 4; } while (0)
  // 0607284E: normal times the matrix
  mac = 0;
  MACL(r7, r6);
  r0 = RW(r7 + 8);
  r5 = r15;
  MACL(r7, r6);
  r1 = SX16(r0);
  r1 <<= 2;
  MACL(r7, r6);
  r6 += 4;
  r7 -= 12;
  r1 <<= 2;
  r0 = (u32)(mac >> 32); r13 = (u32)mac;
  mac = 0;
  MACL(r7, r6);
  r13 = XT(r0, r13);
  rstv_hw_wr(r5, r13);
  MACL(r7, r6);
  r1 += r10;
  r4 = 4;
  MACL(r7, r6);
  r6 += 4;
  r7 -= 12;
  r4 += r1;
  r0 = (u32)(mac >> 32); r2 = (u32)mac;
  mac = 0;
  MACL(r7, r6);
  r2 = XT(r0, r2);
  rstv_hw_wr(r5 + 4, r2);
  MACL(r7, r6);
  r12 = RW(r8);
  r9 = 0x7F;
  MACL(r7, r6);
  r6 -= 44;
  r9 &= r12;
  r9 <<= 2;
  r0 = (u32)(mac >> 32); r3 = (u32)mac;
  mac = 0;
  MACL(r4, r5);                                  // vertex 0 . normal
  r3 = XT(r0, r3);
  r0 = m ? 0x0605C14Cu : RSTV_PA(0x06072B94);
  MACL(r4, r5);
  rstv_hw_wr(r5, r3);                            // [r15 + 8] (read by the next mac.l)
  r9 <<= 2;
  MACL(r4, r5);
  r9 += r0;
  r0 = RW(r8 + 10);
  r7 += 2;
  r4 = (u32)(mac >> 32);
  WW(r11, r0);
  r0 = rstv_hw_rd(gbr + (m ? 0x380 : 0x384));
  T = (s32)r4 >= (s32)r0;
  r0 = SWAPB(r12);
  cyc += 46;
  u32 stage;
  if (T) {
    // 060728C6: back face
    T = (r0 & 1) == 0;
    r3 = (u32)-(s32)r3;
    if (T) {
      r7 += 6;                                   // 060728DE: dropped
      cyc += 5;
      goto loop_end;
    }
    r5 -= 4; rstv_hw_wr(r5, r3);
    r2 = (u32)-(s32)r2;
    r5 -= 4; rstv_hw_wr(r5, r2);
    r13 = (u32)-(s32)r13;
    r5 -= 4; rstv_hw_wr(r5, r13);
    cyc += 8;
  }
  if (m)
    r4 = rstv_hw_rd(r9);                         // mov.l @r9+,r4; jmp @r4 (r0 keeps the swapped attribute)
  else
    r0 = rstv_hw_rd(r9);
  r9 += 4;
  r2 = RW(r7); r7 += 2;
  r2 <<= 2;
  stage = st1;
  // ---- stage 1: sort ----
  if (sk == 0) {
    // average of the four z
    r3 = RW(r7); r7 += 2;
    r2 <<= 2;
    r14 = rstv_hw_rd(r1 + 12);
    r2 += r10;
    r4 = RW(r7); r7 += 2;
    r0 = rstv_hw_rd(r2 + 12);
    r3 <<= 2;
    r14 += r0;
    r3 <<= 2;
    r3 += r10;
    r4 <<= 2;
    r0 = rstv_hw_rd(r3 + 12);
    r4 <<= 2;
    r14 += r0;
    r4 += r10;
    r0 = rstv_hw_rd(r4 + 12);
    r14 += r0;
    r14 >>= 16;
    r14 = SX16(r14);
    r14 = (u32)((s32)r14 >> 1);
    T = r14 & 1; r14 = (u32)((s32)r14 >> 1);    // shar twice: T = the bit shifted out last
    cyc += 22;
  } else if (sk == 3) {
    // 060729E8: no sort, the z kept from before (r14) only checked against [gbr + 0xA0]
    r0 = RW(gbr + (m ? 0x70 : 0xA0));
    r2 <<= 2;
    T = r14 > r0;                                // cmp/hi
    r2 += r10;
    if (T) {
      r7 += 4;                                   // 06072A06: dropped
      if (m)
        r0 = rstv_hw_rd(r9);                     // 0605CF54: mov.l @r9,r0 (the drop row) -> loop end
      cyc += 6;
      goto loop_end;
    }
    if (m)
      r9 += 4;                                   // skip the drop row
    r3 = RW(r7); r7 += 2;
    r3 <<= 2;
    r4 = RW(r7); r7 += 2;
    r3 <<= 2;
    r3 += r10;
    r4 <<= 2;
    r0 = rstv_hw_rd(r9); r9 += 4;
    r4 <<= 2;
    r4 += r10;
    stage = r0;
    cyc += 14;
    goto stage2;
  } else if (sk == 1) {
    // 06072960: nearest z (high words)
    r0 = RW(r1 + 12);
    r2 <<= 2;
    r3 = RW(r7); r7 += 2;
    r14 = SX16(r0);
    r4 = RW(r7); r7 += 2;
    r2 += r10;
    r0 = RW(r2 + 12);
    r3 <<= 2;
    T = (s32)r14 > (s32)r0;
    r3 <<= 2;
    if (T) r14 = r0;
    r3 += r10;
    r4 <<= 2;
    r0 = RW(r3 + 12);
    r4 <<= 2;
    T = (s32)r14 > (s32)r0;
    r4 += r10;
    if (T) r14 = r0;
    r0 = RW(r4 + 12);
    T = (s32)r14 > (s32)r0;
    if (T) r14 = r0;
    cyc += 22;
  } else if (sk >= 4) {
    // ADR-101: 06072A0C average / 06072A50 nearest / 06072A9C farthest of the 32-bit z (master 0605CFA8 / CF5C / CFEC)
    if (sk == 4) {
      r3 = RW(r7); r7 += 2;
      r2 <<= 2;
      r14 = rstv_hw_rd(r1 + 12);
      r2 += r10;
      r4 = RW(r7); r7 += 2;
      r0 = rstv_hw_rd(r2 + 12);
      r3 <<= 4;
      r14 += r0;
      r3 += r10;
      r4 <<= 4;
      r0 = rstv_hw_rd(r3 + 12);
      r14 += r0;
      r4 += r10;
      r0 = rstv_hw_rd(r4 + 12);
      r14 += r0;
      r14 = (u32)((s32)r14 >> 2);
    } else if (sk == 5) {
      r14 = rstv_hw_rd(r1 + 12);
      r2 <<= 2;
      r3 = RW(r7); r7 += 2;
      r4 = RW(r7); r7 += 2;
      r2 += r10;
      r0 = rstv_hw_rd(r2 + 12);
      r3 <<= 4;
      if ((s32)r14 > (s32)r0) r14 = r0;
      r3 += r10;
      r4 <<= 4;
      r0 = rstv_hw_rd(r3 + 12);
      r4 += r10;
      if ((s32)r14 > (s32)r0) r14 = r0;
      r0 = rstv_hw_rd(r4 + 12);
      if ((s32)r14 > (s32)r0) r14 = r0;
    } else {
      r3 = RW(r7); r7 += 2;
      r2 <<= 2;
      r14 = rstv_hw_rd(r1 + 12);
      r2 += r10;
      r4 = RW(r7); r7 += 2;
      r0 = rstv_hw_rd(r2 + 12);
      r3 <<= 4;
      r3 += r10;
      if (!((s32)r14 >= (s32)r0)) r14 = r0;
      r4 <<= 4;
      r0 = rstv_hw_rd(r3 + 12);
      r4 += r10;
      if (!((s32)r14 >= (s32)r0)) r14 = r0;
      r0 = rstv_hw_rd(r4 + 12);
      if (!((s32)r14 >= (s32)r0)) r14 = r0;
    }
    u32 rz = rstv_hw_rd(r15 + 0xC);              // r12 (master) / r5
    r0 = RW(gbr + (m ? 0x70 : 0xA0));
    rz <<= 16;
    r0 <<= 16;
    r0 += rz;
    T = r14 > r0;                                // cmp/hi: beyond the far limit
    rz >>= 1;
    if (m) r12 = rz; else r5 = rz;
    int dropped = T;
    if (!dropped) {
      T = (s32)r14 > (s32)rz;                    // cmp/gt: nearer than half the near limit -> dropped
      dropped = !T;
    }
    cyc += 30;
    if (dropped) {
      if (m) { r0 = rstv_hw_rd(r9); r9 += 4; }  // the drop row
      goto loop_end;
    }
    r14 >>= 16;                                  // shlr16 (delay slot of the bra to the near tests)
    // 06072B12 (0605D05E): every vertex clipped -> dropped
    if ((rstv_hw_rd(r1 + 4) & 1) && (rstv_hw_rd(r2 + 4) & 1) && (rstv_hw_rd(r3 + 4) & 1) && (rstv_hw_rd(r4 + 4) & 1)) {
      if (m) { r0 = rstv_hw_rd(r9); r9 += 4; }
      cyc += 12;
      goto loop_end;
    }
    r0 = rstv_hw_rd(gbr + (m ? 0x378 : 0x37C));
    T = r0 & 1;                                  // rotr
    r0 = (r0 >> 1) | (r0 << 31);
    if (m) { r15 -= 4; rstv_hw_wr(r15, r14); }  // pushed in the delay slot
    if (!T) {
      // screen bounds: every x, y within -1024 .. 1023 (else dropped)
      if (!m) { r15 -= 4; rstv_hw_wr(r15, r14); }
      r14 = 0xFFFFFC00u;
      r5 = 0x3FF;
      const u32 vv[4] = { r1, r2, r3, r4 };
      int out = 0;
      for (int k = 0; k < 8 && !out; k++) {
        const s32 c = (s32)RW(vv[k >> 1] + (k & 1) * 2);
        out = c > (s32)r5 || c < (s32)r14;
      }
      r14 = rstv_hw_rd(r15); r15 += 4;
      cyc += 40;
      if (out) {
        if (m) { r0 = rstv_hw_rd(r9); r9 += 4; }
        goto loop_end;
      }
      if (m) r9 += 4;
    } else if (m) {
      r9 += 4;
      r14 = rstv_hw_rd(r15); r15 += 4;
    }
    r0 = rstv_hw_rd(r9); r9 += 4;
    stage = r0;
    cyc += 10;
    goto stage2;
  } else {
    // 060729A4: farthest z (high words)
    r3 = RW(r7); r7 += 2;
    r2 <<= 2;
    r0 = RW(r1 + 12);
    r2 += r10;
    r4 = RW(r7); r7 += 2;
    r14 = SX16(r0);
    r0 = RW(r2 + 12);
    r3 <<= 2;
    r3 <<= 2;
    r3 += r10;
    T = (s32)r14 >= (s32)r0;
    r4 <<= 2;
    if (!T) r14 = r0;
    r0 = RW(r3 + 12);
    r4 <<= 2;
    T = (s32)r14 >= (s32)r0;
    r4 += r10;
    if (!T) r14 = r0;
    r0 = RW(r4 + 12);
    T = (s32)r14 >= (s32)r0;
    if (!T) r14 = r0;
    cyc += 22;
  }
  if (m) {
    r12 = rstv_hw_rd(r15 + 0xC);
    r0 = RW(gbr + 0x70);
    r14 -= r12;
    T = r14 > r0;
    r14 += r12;
    if (!T)
      r9 += 4;                                   // in range: past the drop row
    r0 = rstv_hw_rd(r9); r9 += 4;
    if (T) {
      cyc += 4;
      goto loop_end;                             // the drop row: loop end
    }
    stage = r0;
    cyc += 8;
  } else {
  r5 = rstv_hw_rd(r15 + 0xC);
  r0 = RW(gbr + 0xA0);
  r14 -= r5;
  T = r14 > r0;                                  // cmp/hi
  r14 += r5;
  if (T) {
    cyc += 3;
    goto loop_end;                               // outside the z range: dropped
  }
  r0 = rstv_hw_rd(r9); r9 += 4;
  stage = r0;
  cyc += 7;
  }
stage2:
  // ---- stage 2: colour, vertex order ----
  if (stage == RSTV_PA(0x06073394)) {
    r0 = RW(r8 + 2);
    r5 = rstv_hw_rd(r15 + 0x14);
    r0 <<= 2;
    r0 += r0;
    r0 = rstv_hw_rd(r5 + r0);
    rstv_hw_wr(r11 + 8, r0);
    cyc += 6;
  }
  if (stage == RSTV_PA(0x06073394) || stage == RSTV_PA(0x060733A0)) {
    r0 = RW(r8 + 8);
    r5 = rstv_hw_rd(r8 + 4);
    WW(r11 + 0x1C, r0);
    rstv_hw_wr(r11 + 4, r5);
    cyc += 4;
  }
  // 060733A8
  r5 >>= 16;
  r5 >>= 2;
  T = r5 & 1; r5 >>= 1;
  {
    int order; // 0: 1 2 3 4 (0607345E), 1: 06073474 (xor 0x10), 2: 06073488 (xor 0x30), 3: 0607349C (xor 0x20)
    if (T) {
      r1 = rstv_hw_rd(r1);
      r5 = rstv_hw_rd(r2);
      r3 = rstv_hw_rd(r3); r2 = r5; r4 = rstv_hw_rd(r4);    // 06073458
      order = 0;
      cyc += 8;
    } else {
      r0 = rstv_hw_rd(r1 + 4);
      r1 = rstv_hw_rd(r1);
      T = (r0 & 1) == 0;
      r5 = rstv_hw_rd(r2);
      if (T) {
        r3 = rstv_hw_rd(r3); r2 = r5; r4 = rstv_hw_rd(r4);  // 06073458
        order = 0;
        cyc += 9;
      } else {
        r0 = rstv_hw_rd(r2 + 4);
        r2 = r5;
        T = (r0 & 1) == 0;
        r5 = rstv_hw_rd(r3);
        if (T) {
          r4 = rstv_hw_rd(r4); r3 = r5;                      // 0607346E
          order = 1;
          cyc += 12;
        } else {
          r0 = rstv_hw_rd(r3 + 4);
          r3 = r5;
          T = (r0 & 1) == 0;
          r5 = rstv_hw_rd(r4);
          if (T) {
            r4 = r5;                                         // 06073484
            order = 2;
            cyc += 15;
          } else {
            r0 = rstv_hw_rd(r4 + 4);
            r4 = r5;
            T = (r0 & 1) == 0;
            r5 = 1;
            if (T) {
              order = 3;                                     // 0607349A
              cyc += 18;
            } else {
              // 060733DE: every vertex clipped: start at the one with the smallest |x| + |y| (one's complement)
              r15 -= 4; rstv_hw_wr(r15, r9);
              r5 <<= 16;
              r5 >>= 1;                                      // 0x8000
#define DIST(v, out) do { r9 = ((v) << 16) | ((v) >> 16); T = (r9 & r5) == 0; out = (u32)0 - T; out = ~out; out ^= r9; \
    T = ((v) & r5) == 0; r9 = (u32)0 - T; r9 = ~r9; r9 ^= (v); out += r9; out &= 0xFFFF; } while (0)
              DIST(r4, r13);
              r0 = RSTV_PA(0x0607349C);
              DIST(r3, r12);
              T = (s32)r12 > (s32)r13;
              if (!T) { r13 = r12; r0 = RSTV_PA(0x06073488); }
              DIST(r2, r12);
              T = (s32)r12 > (s32)r13;
              if (!T) { r13 = r12; r0 = RSTV_PA(0x06073474); }
              DIST(r1, r12);
              T = (s32)r13 >= (s32)r12;
              r9 = rstv_hw_rd(r15); r15 += 4;
#undef DIST
              cyc += 60;
              if (T) {
                order = 0;                                   // 0607345E
              } else {
                order = r0 == RSTV_PA(0x06073474) ? 1 : r0 == RSTV_PA(0x06073488) ? 2 : 3;
                r5 = rstv_hw_rd(r9); r9 += 4;                // jmp @r0 delay slot
                goto ordered;
              }
            }
          }
        }
      }
    }
    // the stage 3 address (0607345E / 06073472 / 06073486 / 0607349A)
    r5 = rstv_hw_rd(r9); r9 += 4;
  ordered:
    if (order == 0) {
      rstv_hw_wr(r11 + 0xC, r1);
      rstv_hw_wr(r11 + 0x10, r2);
      rstv_hw_wr(r11 + 0x14, r3);
      rstv_hw_wr(r11 + 0x18, r4);
    } else {
      static const u32 fl[4] = { 0, 0x10, 0x30, 0x20 };
      static const u8 slot[4][4] = { { 0, 0, 0, 0 }, { 0x10, 0x0C, 0x18, 0x14 }, { 0x14, 0x18, 0x0C, 0x10 }, { 0x18, 0x14, 0x10, 0x0C } };
      r0 = RW(r11);
      rstv_hw_wr(r11 + slot[order][0], r1);
      r0 ^= fl[order];
      WW(r11, r0);
      rstv_hw_wr(r11 + slot[order][1], r2);
      rstv_hw_wr(r11 + slot[order][2], r3);
      rstv_hw_wr(r11 + slot[order][3], r4);
    }
    cyc += 8;
  }
  stage = r5;
  // ---- stage 3: shading (060734AC returns to the writer 060728F8 through PR) ----
  if (m ? stage == 0x0605C94Cu : stage == RSTV_PA(0x060734AC)) {
    mac = 0;
    r5 = gbr;
    r4 = r15;
    r5 += m ? 0x4C : 0x58;
    MACL(r4, r5);
    r0 = RW(r8 + 6);
    r1 = 0x1F;
    r3 = SWAPB(r0);
    MACL(r4, r5);
    r3 >>= 2;
    r3 &= r1;
    r1 &= r0;
    MACL(r4, r5);
    r0 >>= 2;
    r0 >>= 2;
    r2 = 0x1F;
    r0 >>= 1;
    r2 &= r0;
    r5 = (u32)(mac >> 32);
    r4 = (u32)mac;
    T = r5 == 0;
    r0 = lt;
    if (T) {
      r5 = 0x80;                                 // 060734EC
      r4 >>= 16; r4 >>= 8; r4 >>= 1;
      r4 |= r5;
      goto shade_f8;
    }
    r5 += 1;
    T = r5 == 0;
    if (T) {
      r4 >>= 16; r4 >>= 8; r4 >>= 1;             // 060734F0
      r4 |= r5;
      goto shade_f8;
    }
    r4 = 0xFFFFFFFFu;
    T = (s32)r5 > 0;
    r4 &= 0xFF;
    if (T)
      goto shade_f8;
    cyc += 2;
    goto shade_500;                              // 060734E8: bra 06073500
  shade_f8:
    r5 = 0xFFFFFFF8u;
    r4 &= r5;
    r4 <<= 2;
    r0 += r4;
  shade_500:
    r5 = 4;
    r5 <<= 8;
    r1 = RB(r0 + r1);
    r0 += r5;
    r2 = RB(r0 + r2);
    r0 += r5;
    r0 = RB(r0 + r3);
    r0 |= 0x20;
    r0 = SWAPB(r0);
    r2 <<= 2;
    r2 <<= 1;
    r0 += r2;
    r0 <<= 2;
    r1 |= r0;
    r0 = RW(r8 + 4);
    r1 <<= 16;
    r1 = XT(r0, r1);
    r0 = RW(r8 + 8);
    rstv_hw_wr(r11 + 4, r1);
    WW(r11 + 0x1C, r0);
    cyc += 45;
  }
  if (m) {
    // 0605C100: write the z, count the command, next command 0x24 bytes down
    r0 = r14;
    WW(r11 + 0x1E, r0);
    r0 = rstv_hw_rd(r15 + 0x18);
    r11 -= 0x24;
    r0 += 1;
    rstv_hw_wr(r15 + 0x18, r0);
    cyc += 6;
    goto loop_end;
  }
  // 060728F8: write the z and link the command into its bucket
  r0 = r14;
  WW(r11 + 0x1E, r0);
  r0 = bucket;
  r1 = SWAPB(r14);
  r1 &= 0xFF;
  r1 <<= 2;
  r2 = rstv_hw_rd(r0 + r1);
  rstv_hw_wr(r0 + r1, r11);
  rstv_hw_wr(r11 + 0x20, r2);
  r11 += 0x24;
  cyc += 10;
loop_end:
  // 0607290C
  r0 = rstv_hw_rd(r15 + 0x10);
  r8 += 0xC;
  r0 -= 1;
  T = r0 == 0;
  rstv_hw_wr(r15 + 0x10, r0);
  cyc += 5;
#undef MACL
#undef SX16
#undef RW
#undef RB
#undef WW
#undef SWAPB
#undef XT
  R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[4] = r4; R[5] = r5; R[6] = r6; R[7] = r7; R[8] = r8; R[9] = r9; R[10] = r10;
  R[11] = r11; R[12] = r12; R[13] = r13; R[14] = r14; R[15] = r15;
  SET_MACH((u32)(mac >> 32));
  SET_MACL((u32)mac);
  SET_SR((sr & ~1u) | T);
  *npc = m ? (T ? 0x0605C116u : 0x0605C04Eu) : T ? RSTV_PA(0x06072916) : RSTV_PA(0x0607284E);
  *cyc_out = cyc;
  return 1;
}

// ADR-069: SGL 3D sprite entry 0606DC9A (sasissu / sanjeon). r4 = position (x, y, z, size), r5 = attributes (5 words),
// r6 = flip / direction flags. z = (row 2 . p) >> 16 + M[2][3]; rejected (return 1, [gbr + 0x73] = 1) when
// z - ([gbr + 0x68] >> [gbr + 0xAC]) > [gbr + 0x70] << 16 (unsigned) or when the command count [gbr + 0x74] reached
// [gbr + 0x88]. Otherwise the division unit gets ([gbr + 0x68] << 16) / z, x / y come from rows 0 / 1, and a 0x1C or
// 0x24 byte command goes to [gbr + 0x48] (two corner words through the table at 06062C38 when r6 & 0xFFFF).
// The HLE stops at 0606DDD0 (the write to 0x21000000 that wakes the slave) or at the reject exits: the SH-2 runs those.
static const RstvCodeSig rstv_sgl_spr_sig = { 175, 0x8211ffceu, -1, -1 }; // 0606DC9A..0606DDF6 (code and literals)
int DynarecSh2::RstvHleSglSpr(u32 *cyc_out, u32 *npc)
{
  u32 *R = GetGenRegPtr();
  const u32 sr = GET_SR();
  if (sr & 2)
    return 0; // S bit: saturating MAC
  const u32 gbr = GET_GBR();
  if (!rstv_hw_in(gbr, 0xB0))
    return 0;
  const u32 M = rstv_hw_rd(gbr + 0x1C);
  const u32 pos = R[4] & ~3u;
  if (!rstv_hw_in(M, 48) || !rstv_hw_in(pos, 16))
    return 0;
  const s32 sh = (s32)(s8)T2ReadByte(HighWram, (gbr + 0xAC) & 0xFFFFF);
  if (sh <= 0)
    return 0; // dt loop would run 2^32 times
  // every refusal happens here, before anything is written (the SH-2 then runs the routine from its start)
  const u32 out = rstv_hw_rd(gbr + 0x48);
  const u32 sp = R[15];
  const u32 tab = rstv_hw_rd(0x0606DDE0); // corner table (06062C38), indices & 0x3FFE
  if (!rstv_hw_in(R[5] & ~1u, 10) || !rstv_hw_in(out, 0x24) || !rstv_hw_in(sp - 8, 8) || ((R[6] & 0xFFFF) && !rstv_hw_in(tab, 0x4000)))
    return 0;
#define SPRRD(a) rstv_hw_rd(a)
#define SPRXT(m, l) (((u32)(m) << 16) | ((u32)(l) >> 16))
  u32 cyc = 0;
  // counter [gbr + 0xAA] += 1 (mov.w sign-extends, add, mov.w)
  u32 r0 = (u32)(s32)(s16)T2ReadWord(HighWram, (gbr + 0xAA) & 0xFFFFF) + 1;
  T2WriteWord(HighWram, (gbr + 0xAA) & 0xFFFFF, (u16)r0);
  s64 mac = (s64)(s32)SPRRD(pos) * (s32)SPRRD(M + 0x20) + (s64)(s32)SPRRD(pos + 4) * (s32)SPRRD(M + 0x24) +
            (s64)(s32)SPRRD(pos + 8) * (s32)SPRRD(M + 0x28);
  u32 r2 = SPRRD(M + 0x2C);
  const u32 k = SPRRD(gbr + 0x68);
  u32 r3 = sh >= 32 ? 0 : k >> sh;
  // T after the loop: the last bit shifted out (shlr in the delay slot); overwritten by cmp/hi below
  u32 r1 = (u32)(mac >> 32);
  u32 r7 = SPRXT(r1, (u32)mac) + r2; // z
  r3 = (u32)-(s32)r3 + r7;
  r0 = (u32)(s32)(s16)T2ReadWord(HighWram, (gbr + 0x70) & 0xFFFFF) << 16;
  u32 r4 = pos;
  cyc += 25 + 2 * (u32)(sh > 1000 ? 1000 : sh);
  if (r3 > r0) {                         // cmp/hi r0,r3: reject
    R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[4] = r4; R[7] = r7;
    SET_MACH((u32)(mac >> 32)); SET_MACL((u32)mac);
    SET_SR(sr | 1u);
    *npc = 0x0606DC94;
    *cyc_out = cyc;
    return 1;
  }
  // division unit: DVSR = z, DVDNTH = (s16)(k >> 16), DVDNTL = k << 16 (starts the 64 / 32 division)
  Onchip_struct *oc = &ctx_->onchip;
  const u32 dvdh = (u32)(s32)(s16)(k >> 16), dvdl = k << 16;
  if (!(oc->DVCR & 2) && r7 != 0) {
    s64 q_; s32 rm_;
    rstv_div64_ext((s64)(((u64)dvdh << 32) | dvdl), (s32)r7, &q_, &rm_);
    u32 dvl, dvh, dvcr = oc->DVCR;
    if (q_ > 0x7FFFFFFF) { dvcr |= 1; dvl = 0x7FFFFFFF; dvh = 0xFFFFFFFE; }
    else if ((s32)(q_ >> 32) < -1) { dvcr |= 1; dvl = 0x80000000; dvh = 0xFFFFFFFE; }
    else { dvl = (u32)q_; dvh = (u32)rm_; }
    oc->DVSR = r7; oc->DVDNTH = dvh; oc->DVDNTL = dvl; oc->DVDNTUH = dvh; oc->DVDNTUL = dvl; oc->DVCR = dvcr;
  } else {
    OnchipWriteLong(0x100, r7); OnchipWriteLong(0x110, dvdh); OnchipWriteLong(0x114, dvdl);
  }
  r7 = dvdh;
  const u32 lim = T2ReadWord(HighWram, (gbr + 0x88) & 0xFFFFF);
  const u32 cnt = T2ReadWord(HighWram, (gbr + 0x74) & 0xFFFFF);
  r1 = 0xFFFFFFFE;
  if (cnt >= lim) {                      // cmp/hs r1,r0: command list full
    R[0] = cnt; R[1] = r1; R[2] = r2; R[3] = r3; R[4] = r4; R[7] = r7;
    SET_MACH((u32)(mac >> 32)); SET_MACL((u32)mac);
    SET_SR(sr | 1u);
    *npc = 0x0606DDD4;
    *cyc_out = cyc + 10;
    return 1;
  }
  u32 r5 = R[5] & r1;
  T2WriteWord(HighWram, (gbr + 0x74) & 0xFFFFF, (u16)(cnt + 1));
  rstv_hw_wr(sp - 4, R[9]);              // push r9, r8 (popped by the code: r8 here, r9 by the SH-2)
  rstv_hw_wr(sp - 8, R[8]);
  const u32 a0 = (u32)(s32)(s16)T2ReadWord(HighWram, r5 & 0xFFFFF);
  const u32 a1 = (u32)(s32)(s16)T2ReadWord(HighWram, (r5 + 2) & 0xFFFFF);
  const u32 a2 = (u32)(s32)(s16)T2ReadWord(HighWram, (r5 + 4) & 0xFFFFF);
  const u32 a3 = (u32)(s32)(s16)T2ReadWord(HighWram, (r5 + 6) & 0xFFFFF);
  const u32 a4 = (u32)(s32)(s16)T2ReadWord(HighWram, (r5 + 8) & 0xFFFFF);
  r5 += 8;                               // mov.w @r5+ x 4 (the fifth word is read with a displacement)
  // row 0 -> x, the output's second long, row 1 -> y
  mac = (s64)(s32)SPRRD(pos) * (s32)SPRRD(M) + (s64)(s32)SPRRD(pos + 4) * (s32)SPRRD(M + 4) +
        (s64)(s32)SPRRD(pos + 8) * (s32)SPRRD(M + 8);
  const u32 m03 = SPRRD(M + 0x0C);
  r3 = SPRXT(a4, r3);
  rstv_hw_wr(out + 4, r3);
  u32 r8 = SPRXT((u32)(mac >> 32), (u32)mac) + m03;
  mac = (s64)(s32)SPRRD(pos) * (s32)SPRRD(M + 0x10) + (s64)(s32)SPRRD(pos + 4) * (s32)SPRRD(M + 0x14) +
        (s64)(s32)SPRRD(pos + 8) * (s32)SPRRD(M + 0x18);
  r1 = SPRXT(a1, a2 << 16);
  r7 = SPRXT((u32)(mac >> 32), (u32)mac) + SPRRD(M + 0x1C);
  r4 = SPRRD(pos + 12);
  if ((s32)r4 < 0)
    r4 = (u32)-(s32)r4;
  rstv_hw_wr(out + 0x0C, r1);
  r3 = SPRXT(a0, a3 << 16);
  const u32 q = oc->DVDNTUL;             // mov.l @(0x1C, 0xFFFFFF00)
  rstv_hw_wr(out + 8, r3);
  mac = (s64)(s32)q * (s32)r4;
  r4 = SPRXT((u32)(mac >> 32), (u32)mac);
  rstv_hw_wr(out + 0x18, r4);
  mac = (s64)(s32)q * (s32)r8;
  r3 = SPRXT((u32)(mac >> 32), (u32)mac);
  rstv_hw_wr(out + 0x10, r3);
  mac = (s64)(s32)q * (s32)r7;
  r2 = (u32)(mac >> 32);
  r3 = SPRXT(r2, (u32)mac);
  rstv_hw_wr(out + 0x14, r3);
  r4 = R[6] & 0xFFFF;
  u32 T, len;
  if (r4 == 0) {
    T = 1;
    r0 = 0x1C;
    rstv_hw_wr(out, 0x1C);
    len = 0x1C;
  } else {
    T = 0;
    r0 = ((r4 & 0xFF) << 8 | (r4 >> 8)) >> 4;  // swap.b, shlr2, shlr2 (r4 < 0x10000)
    u32 tb = 0x0606DDE4 + (r0 & 0x0C);
    const u32 f0 = (u32)(s32)(s8)T2ReadByte(HighWram, tb & 0xFFFFF), f1 = (u32)(s32)(s8)T2ReadByte(HighWram, (tb + 1) & 0xFFFFF);
    const u32 f2 = (u32)(s32)(s8)T2ReadByte(HighWram, (tb + 2) & 0xFFFFF), f3 = (u32)(s32)(s8)T2ReadByte(HighWram, (tb + 3) & 0xFFFFF);
    const u32 mask = (u32)(s32)(s16)T2ReadWord(HighWram, 0x0006DDF4);
    u32 e4 = (r4 ^ f0) & mask, e5 = (r4 ^ f1) & mask;
    e4 = T2ReadWord(HighWram, (tab + e4) & 0xFFFFF);
    e5 = T2ReadWord(HighWram, (tab + e5) & 0xFFFFF);
    r4 = (e4 + 2) ^ f2;
    r5 = (e5 + 2) ^ f3;
    rstv_hw_wr(out + 0x1C, r4);
    rstv_hw_wr(out + 0x20, r5);
    rstv_hw_wr(out, 0x18);
    r1 = f3;
    r2 = tb + 4;
    len = 0x24;
    cyc += 25;
  }
  // 0606DDC8: pop r8, next pointer, [gbr + 0x48]; r1 = 0x21000000 (the SH-2 writes it at 0606DDD0)
  r0 = len + out;
  rstv_hw_wr(gbr + 0x48, r0);
  R[0] = r0; R[1] = SPRRD(0x0606DDDC); R[2] = r2; R[3] = r3; R[4] = r4; R[5] = r5; R[7] = r7;
  R[8] = SPRRD(sp - 8);                  // mov.l @r15+, r8
  R[9] = out;                            // r9 is popped at 0606DDD2 by the SH-2
  R[15] = sp - 4;
  SET_MACH((u32)(mac >> 32)); SET_MACL((u32)mac);
  SET_SR((sr & ~1u) | T);
  *npc = 0x0606DDD0;
  *cyc_out = cyc + 90;
  return 1;
#undef SPRRD
#undef SPRXT
}


#if !defined(__vita__)
// harness: RSTV_HLECHK=1 runs each native routine, keeps its result, restores the state, runs the interpreter from the
// same state to the routine's exit PC and compares registers, work RAM and the division unit. Prints the first mismatches.

int DynarecSh2::RstvHleCheck(int kind)
{
  static u8 *hw0, *lw0, *hw1, *lw1;
  static unsigned nchk, nbad;
  if (!hw0) {
    hw0 = (u8 *)malloc(0x100000); lw0 = (u8 *)malloc(0x100000); hw1 = (u8 *)malloc(0x100000); lw1 = (u8 *)malloc(0x100000);
    if (opcodes[0x0009] == NULL)
      SH2Interpreter.Init();
  }
  u32 *R = GetGenRegPtr();
  u32 r0[16], c0[8];
  memcpy(r0, R, sizeof(r0));
  c0[0] = GET_SR(); c0[1] = GET_GBR(); c0[2] = GET_VBR(); c0[3] = GET_MACH(); c0[4] = GET_MACL(); c0[5] = GET_PR(); c0[6] = GET_PC();
  c0[7] = m_pDynaSh2->SysReg[4];
  memcpy(hw0, HighWram, 0x100000); memcpy(lw0, LowWram, 0x100000);
  SH2_struct *sh = ctx_;
  const Onchip_struct oc0 = sh->onchip;
  if (!RstvHle(kind))
    return 0;
  u32 r1[16], c1[7];
  memcpy(r1, R, sizeof(r1));
  c1[0] = GET_SR(); c1[3] = GET_MACH(); c1[4] = GET_MACL(); c1[5] = GET_PR(); c1[6] = GET_PC();
  const u32 cyc1 = m_pDynaSh2->SysReg[4];
  memcpy(hw1, HighWram, 0x100000); memcpy(lw1, LowWram, 0x100000);
  const Onchip_struct oc1 = sh->onchip;
  // back to the start, run the interpreter to the same exit
  memcpy(HighWram, hw0, 0x100000); memcpy(LowWram, lw0, 0x100000);
  sh->onchip = oc0;
  SH2_struct *saved_cur = CurrentSH2;
  const sh2regs_struct saved_regs = sh->regs;
  CurrentSH2 = sh;
  memcpy(sh->regs.R, r0, sizeof(r0));
  sh->regs.SR.all = c0[0]; sh->regs.GBR = c0[1]; sh->regs.VBR = c0[2]; sh->regs.MACH = c0[3]; sh->regs.MACL = c0[4];
  sh->regs.PR = c0[5]; sh->regs.PC = c0[6];
  int steps = 0;
  rstv_tier_wnotify++;
  while (steps < 4000000) {
    const u32 pc = sh->regs.PC;
    sh->instruction = fetchlist[(pc >> 20) & 0x0FF](pc);
    opcodes[sh->instruction](sh);
    steps++;
    if (sh->regs.PC == c1[6])
      break;
  }
  rstv_tier_wnotify--;
  nchk++;
  int bad = 0;
  char msg[512];
  int mn = 0;
  for (int i = 0; i < 16; i++)
    if (sh->regs.R[i] != r1[i]) { bad = 1; mn += snprintf(msg + mn, sizeof(msg) - mn, " r%d %08x/%08x", i, sh->regs.R[i], r1[i]); }
  if ((sh->regs.SR.all & 0x3F3) != (c1[0] & 0x3F3)) { bad = 1; mn += snprintf(msg + mn, sizeof(msg) - mn, " sr %x/%x", sh->regs.SR.all, c1[0]); }
  if (sh->regs.MACH != c1[3] || sh->regs.MACL != c1[4]) { bad = 1; mn += snprintf(msg + mn, sizeof(msg) - mn, " mac %08x%08x/%08x%08x", sh->regs.MACH, sh->regs.MACL, c1[3], c1[4]); }
  if (sh->regs.PC != c1[6]) { bad = 1; mn += snprintf(msg + mn, sizeof(msg) - mn, " pc %08x/%08x", sh->regs.PC, c1[6]); }
  if (sh->onchip.DVSR != oc1.DVSR || sh->onchip.DVDNTL != oc1.DVDNTL || sh->onchip.DVDNTH != oc1.DVDNTH || sh->onchip.DVCR != oc1.DVCR)
    { bad = 1; mn += snprintf(msg + mn, sizeof(msg) - mn, " div"); }
  for (u32 a = 0; a < 0x100000 && mn < 400; a += 4)
    if (memcmp(HighWram + a, hw1 + a, 4)) { bad = 1; mn += snprintf(msg + mn, sizeof(msg) - mn, " H%05x %08x/%08x", a, T2ReadLong(HighWram, a), T2ReadLong(hw1, a)); }
  for (u32 a = 0; a < 0x100000 && mn < 400; a += 4)
    if (memcmp(LowWram + a, lw1 + a, 4)) { bad = 1; mn += snprintf(msg + mn, sizeof(msg) - mn, " L%05x", a); }
  if (bad) {
    nbad++;
    if (nbad <= 20)
      printf("hlechk BAD f%d k%d pc %08x steps %d:%s\n", (int)yabsys.frame_count, kind, c0[6], steps, msg);
  } else if ((nchk & 1023) == 1)
    printf("hlechk ok %u (bad %u) f%d k%d steps %d\n", nchk, nbad, (int)yabsys.frame_count, kind, steps);
  // continue from the interpreter's result
  memcpy(R, sh->regs.R, sizeof(r0));
  SET_SR(sh->regs.SR.all); SET_MACH(sh->regs.MACH); SET_MACL(sh->regs.MACL); SET_PR(sh->regs.PR); SET_PC(sh->regs.PC);
  m_pDynaSh2->SysReg[4] = cyc1;
  sh->regs = saved_regs;
  CurrentSH2 = saved_cur;
  return 1;
}
#endif

int DynarecSh2::RstvHle(int kind)
{
  if (!rstv_hle_on)
    return 0;
#if !defined(__vita__)
  {
    // harness: RSTV_HLEMASK=bits of kinds to run natively, RSTV_HLELOG=1 prints each call
    static int mask = -2, log = 0;
    if (mask == -2) {
      const char *e = getenv("RSTV_HLEMASK");
      mask = e ? (int)strtol(e, NULL, 0) : -1;
      log = getenv("RSTV_HLELOG") != NULL;
    }
    if (!((mask >> kind) & 1))
      return 0;
    if (log)
      printf("hle f%d k%d pc %08x r4 %08x r5 %08x r6 %08x r7 %08x pr %08x\n", (int)yabsys.frame_count, kind, GET_PC(),
        GetGenRegPtr()[4], GetGenRegPtr()[5], GetGenRegPtr()[6], GetGenRegPtr()[7], GET_PR());
  }
#endif
  const u32 pc = GET_PC();
  if (kind == RSTV_HLE_SGLPOLY) {
    // ADR-100: sanjeon / sasissu 0607284E, twcup98 slave 06063332 (the same module moved)
    // ADR-101: twcup98 master 0605C04E (another build of the loop, its colour stage in the slave module)
    static unsigned g0[2], g1[2], g2[2], gm[5], gn[2];
    const int pm = pc == 0x0605C04E;
    const int pi = (pc == 0x06063332 || pm) ? 1 : 0;
    rstv_poly_d = pi ? 0x06063332u - 0x0607284Eu : 0;
    rstv_poly_m = pm;
    if (pm ? (!rstv_hle_checked(&gm[0], 0x0605C04E, &rstv_polym_main_sig) ||
              !rstv_hle_checked(&gm[1], 0x0605C14C, &rstv_polym_table_sig) ||
              !rstv_hle_checked(&gm[2], 0x0605CE6C, &rstv_polym_sort_sig) ||
              !rstv_hle_checked(&gm[3], 0x0605C94C, &rstv_polym_shade_sig) ||
              !rstv_hle_checked(&gm[4], 0x0605CF5C, &rstv_polym_near_sig) || !rstv_hle_checked(&gn[1], RSTV_PA(0x06072A0C), &rstv_poly_near_sig) ||
              !rstv_hle_checked(&g2[1], RSTV_PA(0x06073394), &rstv_poly_stage_sig)) :
        ((pc != 0x0607284E && pc != 0x06063332) || !rstv_hle_checked(&g0[pi], RSTV_PA(0x0607284E), &rstv_poly_main_sig) ||
        !rstv_hle_checked(&g1[pi], RSTV_PA(0x06072924), &rstv_poly_sort_sig) ||
        !rstv_hle_checked(&g2[pi], RSTV_PA(0x06073394), &rstv_poly_stage_sig) ||
        !rstv_hle_checked(&gn[pi], RSTV_PA(0x06072A0C), &rstv_poly_near_sig))) {
#if !defined(__vita__)
      static int c_; if (c_++ < 3) { u32 h = 2166136261u; for (int k = 0; k < 107; k++) h = (h ^ T2ReadWord(HighWram, (0x0607284E & 0xFFFFF) + k * 2)) * 16777619u;
        printf("sglpoly sig %d %d %d main %08x w0 %04x\n", rstv_hle_match(0x0607284E, &rstv_poly_main_sig),
        rstv_hle_match(0x06072924, &rstv_poly_sort_sig), rstv_hle_match(0x06073394, &rstv_poly_stage_sig), h, T2ReadWord(HighWram, 0x7284E)); }
#endif
      return 0;
    }
    u32 cyc = 0, npc = 0;
    const int okp = RstvHleSglPoly(&cyc, &npc);
    if (!okp)
      return 0;
    SET_PC(npc);
    m_pDynaSh2->SysReg[4] += cyc;
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_SGLSPR) {
    static unsigned gs;
    if (pc != 0x0606DC9A || !rstv_hle_checked(&gs, pc, &rstv_sgl_spr_sig))
      return 0;
    u32 cyc = 0, npc = 0;
    if (!RstvHleSglSpr(&cyc, &npc))
      return 0;
    SET_PC(npc);
    m_pDynaSh2->SysReg[4] += cyc;
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_SGLONSCR) {
    static unsigned go;
    if (pc != 0x0606AEA8 || !rstv_hle_checked(&go, pc, &rstv_onscr_sig))
      return 0;
    u32 cyc = 0;
    if (!RstvHleSglOnScr(&cyc))
      return 0;
    SET_PC(GET_PR());
    m_pDynaSh2->SysReg[4] += cyc;
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_SJINTERP) {
    static unsigned g0, g1, g2, g3;
    if (pc != 0x0600B33A || !rstv_hle_checked(&g0, 0x0600B288, &rstv_sj_loop_sig) || !rstv_hle_checked(&g1, 0x0600B42C, &rstv_sj_lit_sig) ||
        !rstv_hle_checked(&g2, 0x0606AE88, &rstv_sj_fix_sig) || !rstv_hle_checked(&g3, 0x0606AF70, &rstv_sj_mv_sig))
      return 0;
    u32 cyc = 0;
    if (!RstvHleSjInterp(&cyc))
      return 0;
    SET_PC(0x0600B340);
    m_pDynaSh2->SysReg[4] += cyc;
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_TKVTX) {
    static unsigned gt;
    if (pc != 0x0602FEF8 || !rstv_hle_checked(&gt, pc, &rstv_tk_vtx_sig))
      return 0;
    u32 cyc = 0;
    if (!RstvHleTkVtx(&cyc))
      return 0;
    SET_PC(GET_PR());
    m_pDynaSh2->SysReg[4] += cyc;
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_SGLVTX) {
    int di = 0;
    while (di < 4 && rstv_sgl_desc[di].head != pc)
      di++;
    const RstvSglDesc *dsc = di < 4 ? &rstv_sgl_desc[di] : NULL;
    static unsigned gs[4];
    if (!dsc || !rstv_hle_checked(&gs[di], dsc->code, dsc->sig))
      return 0;
    u32 cyc = 0;
    const int npc = RstvHleSglVtx(dsc, &cyc);
    if (!npc)
      return 0;
    SET_PC((u32)npc);
    m_pDynaSh2->SysReg[4] += cyc;
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_LZ) {
    extern unsigned rstv_hle_why;
    if (!rstv_lz_check(pc)) { rstv_hle_why = 1; return 0; }
    {
      const u32 *R0 = GetGenRegPtr();
      rstv_hle_why = 0x100 | ((memGetLong(R0[4]) >> 24) << 16) | ((memGetLong(R0[5]) >> 20) & 0xFFF); // src region / dst region
    }
    u32 *R = GetGenRegPtr();
    u32 ret = 0, cyc = 0, bytes = 0;
    const int ok = rstv_hle_lz(R[4], R[5], R[6], (s32)R[7], &ret, &cyc, &bytes);
    if (ok <= 0) {
      if (ok < 0)
        rstv_hle_on = 0; // the decoder state is now unknown to the SH-2 code: should not happen, stop using it
      return ok < 0 ? 0 : 0;
    }
    R[0] = ret;
    SET_PC(GET_PR());
    m_pDynaSh2->SysReg[4] += cyc;
    rstv_hle_runs++;
    rstv_hle_bytes += bytes;
    return 1;
  }
  if (kind == RSTV_HLE_DELAYR) {
    // ADR-109: rN is tested, then decremented in the delay slot either way; the loop leaves when the test sees 0
    const int rn = rstv_delayr_check(pc);
    if (rn < 0)
      return 0;
    u32 *R = GetGenRegPtr();
    const u32 v = R[rn];
    const u64 todo = (u64)v + 1; // tests until one sees 0
    const s32 left = (s32)(m_pDynaSh2->exitcount - m_pDynaSh2->SysReg[4]);
    const u64 can = left > 0 ? ((u64)left + rstv_delayr_cyc - 1) / rstv_delayr_cyc : 1;
    const u64 n = todo < can ? todo : can;
    R[rn] = v - (u32)n;
    if (n == todo) {
      SET_SR(GET_SR() | 1u);
      SET_PC(pc + 6);
    } else {
      SET_SR(GET_SR() & ~1u);
      SET_PC(pc);
    }
    m_pDynaSh2->SysReg[4] += (u32)(n * rstv_delayr_cyc) - (n == todo ? (u32)rstv_delayr_nt : 0);
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_DELAY) {
    // ADR-109: see rstv_delay_check. The block loop runs whole iterations while the count is below the slice end, so
    // run the same number here (ceil(left / per)) and charge the same cycles; the counter ends where the code leaves it
    if (!rstv_delay_check(pc))
      return 0;
    u32 *R = GetGenRegPtr();
    const u32 sp = R[15];
    if ((sp & 0x0FF00000) != 0x06000000 || (sp & 3))
      return 0;
    const u32 c = memGetLong(sp);
    const u64 todo = c ? (u64)c : 0x100000000ull; // iterations until the counter reads 0
    const s32 left = (s32)(m_pDynaSh2->exitcount - m_pDynaSh2->SysReg[4]);
    const u64 can = left > 0 ? ((u64)left + RSTV_DELAY_CYC - 1) / RSTV_DELAY_CYC : 1;
    const u64 n = todo < can ? todo : can;
    const u32 v = c - (u32)n;
    memSetLong(sp, v);
    R[2] = v;
    R[0] = v;
    if (v) {
      SET_SR(GET_SR() & ~1u);
      SET_PC(pc);
    } else {
      SET_SR(GET_SR() | 1u);
      SET_PC(pc + 12);
    }
    m_pDynaSh2->SysReg[4] += (u32)(n * RSTV_DELAY_CYC) - (v ? 0 : RSTV_DELAY_NT);
    rstv_hle_runs++;
    return 1;
  }
  if (kind == RSTV_HLE_BSSCLR2) {
    // ADR-091: see rstv_bss2_check. Leaves r3 = end, r4 = max(start, end), T = 1, continues after the bf
    if (!rstv_bss2_check(pc))
      return 0;
    u32 *R = GetGenRegPtr();
    const u32 end = memGetLong(R[6]), d = R[4];
    const u32 n = end > d ? end - d : 0;
    RstvHleSide xd;
    if (!rstv_hle_side(&xd, d, n))
      return 0;
    const u8 v = (u8)R[5];
    for (u32 i = 0; i < n; i++)
      rstv_hle_wr(&xd, i, v);
    if (n) R[4] = end;
    R[3] = end;
    SET_SR(GET_SR() | 1u);
    rstv_hle_notify(d, n);
    SET_PC(pc + 6);
    m_pDynaSh2->SysReg[4] += 4 + n * 5;
    rstv_hle_runs++;
    rstv_hle_bytes += n;
    if (n >= 0x10000) {
      rstv_hle_big[0] = (u32)kind; rstv_hle_big[1] = d; rstv_hle_big[2] = 0; rstv_hle_big[3] = n;
    }
    return 1;
  }
  const u32 base = kind == RSTV_HLE_BSSCLR ? pc - 6 : pc;
  if ((base & 0x0FF00000) != 0x06000000 || (base & 0xFFFFF) + 72 > 0x100000)
    return 0;
  if (!rstv_hle_match(base, &rstv_hle_sig[kind]))
    return 0;
  u32 *R = GetGenRegPtr();
  u32 sr = GET_SR();
  u32 cyc = 0, n = 0;
  if (kind == RSTV_HLE_MEMCPY || kind == RSTV_HLE_MEMMOVE) {
    const u32 d = R[4], sp = R[5];
    n = R[6];
    RstvHleSide xd, xs;
    if (!rstv_hle_side(&xd, d, n) || !rstv_hle_side(&xs, sp, n, 1))
      return 0;
    if (kind == RSTV_HLE_MEMCPY) {
      // forward byte copy (overlap repeats the pattern, as the LZ decoder expects)
      u8 last = 0;
      if (xd.m && xs.m && !xd.t1 && !xs.t1) {
        for (u32 i = 0; i < n; i++) {
          last = T2ReadByte(xs.m, xs.o + i);
          T2WriteByte(xd.m, xd.o + i, last);
        }
      } else
        for (u32 i = 0; i < n; i++) {
          last = rstv_hle_rd(&xs, i);
          rstv_hle_wr(&xd, i, last);
        }
      if (n) { R[1] = (u32)(s32)(s8)last; R[6] = 0; }
      R[3] = 0; R[7] = d + n; R[0] = d;
      sr &= ~1u;
      cyc = 6 + n * 7;
    } else {
      if (d == sp || !n) {
        R[0] = d;
      } else if (d < sp) {
        u8 last = 0;
        for (u32 i = 0; i < n; i++) {
          last = rstv_hle_rd(&xs, i);
          rstv_hle_wr(&xd, i, last);
        }
        R[1] = (u32)(s32)(s8)last; R[2] = d + n - 1; R[5] = n; R[7] = 0;
      } else {
        u8 last = 0;
        for (u32 i = n; i-- > 0;) {
          last = rstv_hle_rd(&xs, i);
          rstv_hle_wr(&xd, i, last);
        }
        R[3] = (u32)(s32)(s8)last; R[5] = n; R[7] = 0;
      }
      R[0] = d;
      sr |= 1u;
      cyc = 10 + n * 7;
    }
    rstv_hle_notify(d, n);
    SET_PC(GET_PR());
  } else if (kind == RSTV_HLE_MEMSET8) {
    const u32 d = R[4];
    n = R[6];
    RstvHleSide xd;
    if (!rstv_hle_side(&xd, d, n))
      return 0;
    const u8 v = (u8)R[5];
    for (u32 i = 0; i < n; i++)
      rstv_hle_wr(&xd, i, v);
    if (n) R[2] = v;
    R[3] = 0; R[7] = n; R[0] = d;
    sr |= 1u;
    cyc = 6 + n * 6;
    rstv_hle_notify(d, n);
    SET_PC(GET_PR());
  } else if (kind == RSTV_HLE_FILL16 || kind == RSTV_HLE_FILL32) {
    const u32 d = R[4], unit = kind == RSTV_HLE_FILL16 ? 2 : 4;
    const u32 cnt = R[6];
    n = cnt * unit;
    RstvHleSide xd;
    if (cnt > 0x40000 || (d & (unit - 1)) || !rstv_hle_side(&xd, d, n))
      return 0;
    for (u32 i = 0; i < cnt; i++)
      if (xd.m && xd.t1) {
        if (unit == 2) T1WriteWord(xd.m, xd.o + i * 2, (u16)R[5]); else T1WriteLong(xd.m, xd.o + i * 4, R[5]);
      } else if (xd.m) {
        if (unit == 2) T2WriteWord(xd.m, xd.o + i * 2, (u16)R[5]); else T2WriteLong(xd.m, xd.o + i * 4, R[5]);
      } else {
        u32 c = 0;
        if (unit == 2) MappedMemoryWriteWord(d + i * 2, (u16)R[5], &c); else MappedMemoryWriteLong(d + i * 4, R[5], &c);
      }
    R[2] = 0; R[3] = 0; R[6] = 0; R[7] = d + n; R[0] = d;
    sr &= ~1u;
    cyc = 6 + cnt * 6;
    rstv_hle_notify(d, n);
    SET_PC(GET_PR());
  } else if (kind == RSTV_HLE_BSSCLR) {
    // 0608002A: extu.b r6,r2 / mov.b r2,@r4 / add #1,r4 / 06080030: mov.l @r7,r3 / cmp/hs r3,r4 / bf 0608002A
    const u32 end = memGetLong(R[7]), d = R[4];
    n = end > d ? end - d : 0;
    RstvHleSide xd;
    if (!rstv_hle_side(&xd, d, n))
      return 0;
    const u8 v = (u8)R[6];
    for (u32 i = 0; i < n; i++)
      rstv_hle_wr(&xd, i, v);
    if (n) { R[2] = v; R[4] = end; }
    R[3] = end;
    sr |= 1u;
    cyc = 4 + n * 6;
    rstv_hle_notify(d, n);
    SET_PC(pc + 6);
  } else
    return 0;
  SET_SR(sr);
  m_pDynaSh2->SysReg[4] += cyc;
  rstv_hle_runs++;
  rstv_hle_bytes += n;
  if (n >= 0x10000) { // diagnostics: the last big run (kind, destination, source, bytes)
    rstv_hle_big[0] = (u32)kind; rstv_hle_big[1] = R[0]; rstv_hle_big[2] = (kind == RSTV_HLE_MEMCPY || kind == RSTV_HLE_MEMMOVE) ? R[5] : 0;
    rstv_hle_big[3] = n;
  }
  return 1;
}
#endif

// ADR-081: the original code of a hook PC, compiled outside LookupTable (the table holds its native-call block)
Block *DynarecSh2::RstvHleOrigBlock(u32 pc)
{
  static Block *orig[RSTV_MAX_HLE];
  static unsigned orig_gen[RSTV_MAX_HLE];
  int k = 0;
  while (k < rstv_hle_n && rstv_hle[k].pc != pc)
    k++;
  if (k >= rstv_hle_n)
    k = RSTV_MAX_HLE - 1; // no longer a hook: one shared slot
  Block *b = orig[k];
  if (b && b->b_addr == pc && !(b->flags & BLOCK_HLESTUB) && orig_gen[k] == rstv_hle_codegen)
    return b;
  rstv_emit_nostub = 1;
  b = m_pCompiler->CompileBlock(pc, NULL);
  rstv_emit_nostub = 0;
  orig[k] = b;
  orig_gen[k] = rstv_hle_codegen;
  return b;
}

// ADR-081: called from a native-call block (r0 = the context's register block); 1 = done, PC and cycles updated
int DynarecSh2::RstvHleStub()
{
  const u32 pc = GET_PC();
  rstv_hle_stub_calls++;
  for (int k = 0; k < rstv_hle_n; k++)
    if (rstv_hle[k].pc == pc) {
      if (!rstv_hle_on)
        break;
      rstv_act_pc = 0x14;
      const int kind = rstv_hle[k].kind;
      if (RstvHle(kind)) {
        rstv_hle_kind[kind & 7]++;
        return 1;
      }
      rstv_hle_fail[kind & 7]++;
      break;
    }
  rstv_hle_bypass_pc = pc; // Execute runs the original code
  return 0;
}
extern "C" int rstv_hle_stub_c(void *genreg)
{
  (void)genreg;
  return DynarecSh2::CurrentContext->RstvHleStub();
}

inline int DynarecSh2::Execute(){
#if defined(RSTV)
  rstv_dyn_exec++;
  int rstv_orig = 0; // ADR-081: run the original code of a hook PC (the native routine refused)
#endif

  Block * pBlock = NULL;

  m_pCompiler->exec_count_++;
#if defined(EXECUTE_STAT)
  m_pCompiler->setShowCode( is_slave_ );
#endif
//#endif

  if ((GET_PC() & 0xFF000000) == 0xC0000000)
  {
    pBlock = m_pCompiler->LookupTableC[(GET_PC() & 0x000FFFFF) >> 1];
    if (pBlock == NULL)
    {
      pBlock = m_pCompiler->CompileBlock(GET_PC());
      m_pCompiler->LookupTableC[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      if (pBlock == NULL) {
        Undecoded();
        return IN_INFINITY_LOOP;
      }
    }
  }
  else {

    switch (GET_PC() & 0x0FF00000)
    {

      // ROM
    case 0x00000000:
      if (yabsys.extend_backup) {
        const u32 bupaddr = 0x0007d600; // MappedMemoryReadLong(0x06000358);
        if (GET_PC() == bupaddr) {
          LOG("BUP_Init");
          BiosBUPInit(ctx_);
          yabsys.extend_backup = 2;
          return IN_INFINITY_LOOP;
        }
        else if (yabsys.extend_backup == 2 &&
          GET_PC() >= 0x0380 &&
          GET_PC() <= 0x03A8) {
          BiosHandleFunc(ctx_);
          return IN_INFINITY_LOOP;
        }
      }
      if (yabsys.emulatebios) {
        ctx_->cycles = 0;
         BiosHandleFunc(ctx_);
         memcycle_ += ctx_->cycles;
        return 0;
      }
      pBlock = m_pCompiler->LookupTableRom[(GET_PC() & 0x000FFFFF) >> 1];
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC());
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTableRom[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // Low Memory
    case 0x00200000:
      pBlock = m_pCompiler->LookupTableLow[(GET_PC() & 0x000FFFFF) >> 1];
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC());
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTableLow[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // High Memory
    case 0x06000000:
      /*case 0x06100000:*/

#if defined(RSTV)
      rstv_hle_bits_sync();
      rstv_orig = 0;
      if (rstv_hle_bypass_pc && rstv_hle_bypass_pc == GET_PC()) {
        rstv_hle_bypass_pc = 0;
        rstv_orig = 1;
      } else
      if (rstv_hle_on && (rstv_hle_bits[((GET_PC() & 0xFFFFF) >> 1) >> 3] >> (((GET_PC() & 0xFFFFF) >> 1) & 7) & 1))
      for (int k = 0; k < rstv_hle_n; k++)
        if (rstv_hle[k].pc == GET_PC()) {
          rstv_act_pc = 0x14; // sampler: native routine (ADR-045/047)
          const u64 rstv_h0 = RSTV_PROF_TF(); // fine only: hundreds of calls a frame (ADR-075)
#if !defined(__vita__)
          static int rstv_hlechk = -1;
          static unsigned rstv_hlechk_n;
          if (rstv_hlechk < 0) rstv_hlechk = getenv("RSTV_HLECHK") ? atoi(getenv("RSTV_HLECHK")) : 0;
          const int rstv_hok = (rstv_hlechk > 0 && (rstv_hlechk_n++ % (unsigned)rstv_hlechk) == 0) ? RstvHleCheck(rstv_hle[k].kind) :
            RstvHle(rstv_hle[k].kind);
#else
          const int rstv_hok = RstvHle(rstv_hle[k].kind);
#endif
          rstv_hle_us[rstv_hle[k].kind & 7] += (unsigned)(RSTV_PROF_TF() - rstv_h0);
          if (rstv_hok ? (rstv_hle_kind[rstv_hle[k].kind & 7]++, 1) : (rstv_hle_fail[rstv_hle[k].kind & 7]++, 0)) {
            // ADR-081: from now on chained calls reach the routine through a native-call block
            if (rstv_hle_stub_on && m_pCompiler->LookupTable[(rstv_hle[k].pc & 0x000FFFFF) >> 1] == NULL) {
              Block *sb = m_pCompiler->CompileBlock(rstv_hle[k].pc, m_pCompiler->LookupParentTable);
              if (sb && (sb->flags & BLOCK_HLESTUB))
                m_pCompiler->LookupTable[(rstv_hle[k].pc & 0x000FFFFF) >> 1] = sb;
            }
            if ((GET_SR() & 0xF0) < GET_ICOUNT())
              this->CheckInterupt();
            return 0;
          }
          rstv_orig = 1;
          break;
        }
#endif
      pBlock = m_pCompiler->LookupTable[(GET_PC() & 0x000FFFFF) >> 1];
#if defined(RSTV)
      if (rstv_orig || (pBlock && (pBlock->flags & BLOCK_HLESTUB) && !rstv_hle_on))
        pBlock = RstvHleOrigBlock(GET_PC());
#endif
#if defined(RSTV)
      if (pBlock == NULL && rstv_hle_on && rstv_hle_auto && rstv_tier_hot[(GET_PC() & 0x000FFFFF) >> 1] == 0 &&
          rstv_hle_detect(GET_PC())) {
        rstv_hle_auto_n++;
        rstv_hle_bits_sync();
        const int kind = rstv_hle[rstv_hle_n - 1].kind;
        if (RstvHle(kind)) {
          rstv_hle_kind[kind & 7]++;
          if ((GET_SR() & 0xF0) < GET_ICOUNT())
            this->CheckInterupt();
          return 0;
        }
      }
      if (pBlock == NULL && rstv_tier_n && !m_pCompiler->debug_mode_ && rstv_tier_hot[(GET_PC() & 0x000FFFFF) >> 1] < rstv_tier_n) {
        rstv_tier_hot[(GET_PC() & 0x000FFFFF) >> 1]++;
        RstvTierRun();
        if ((GET_SR() & 0xF0) < GET_ICOUNT())
          this->CheckInterupt();
        const int w = is_slave_ ? 1 : 0;
        const u32 pc = m_pDynaSh2->SysReg[3];
        for (int k = rstv_idle_n[w] - 1; k >= 0; k--)
          if (pc == rstv_idle_pc[w][k])
            return IN_INFINITY_LOOP;
        return 0;
      }
#endif
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC(), m_pCompiler->LookupParentTable);
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTable[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // Cache
    default:
      pBlock = m_pCompiler->CompileBlock(GET_PC());
      if (pBlock == NULL) {
        Undecoded();
        return IN_INFINITY_LOOP;
      }
      break;
    }
  }
    
#if 0
    static FILE * fp = NULL;
    char fname[64];
    sprintf(fname,"/mnt/sdcard/yabause/intlog.txt");
    if( fp == NULL ) {
        fp = fopen(fname, "w");
    }
    if(fp){
        fprintf(fp,"\n---dynaExecute %08X----\n", GET_PC());
        fflush(fp);
    }
#endif
//  if(yabsys.frame_count == 7){
//    logenable_ = true;
//  }
//  if (logenable_) {
//    LOG("[%s] dynaExecute start %08X %08X", (is_slave_ == false) ? "M" : "S", GET_PC(), GET_PR());
//  }
#if defined(DEBUG_CPU) || defined(EXECUTE_STAT)
    u32 prepc = GET_PC();
  if (is_slave_) { //statics_trigger_ == COLLECTING) {
    u64 pretime = YabauseGetTicks();
    ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
    compie_statics_[prepc].count++;
    compie_statics_[prepc].time += YabauseGetTicks() - pretime;
    compie_statics_[prepc].end_addr = pBlock->e_addr;
  }
  else {
    ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
  }
#else
#if defined(RSTV)
  rstv_act_pc = GET_PC();
#if !defined(__vita__)
  {
    // harness debug: RSTV_BTRACE=frame,line0,line1 -> every block entered in that window (frame_count as the core counts it)
    static int bt_f = -2, bt_l0, bt_l1;
    if (bt_f == -2) { const char *e = getenv("RSTV_BTRACE"); bt_f = -1; if (e) sscanf(e, "%d,%d,%d", &bt_f, &bt_l0, &bt_l1); }
    if (bt_f >= 0 && (int)yabsys.frame_count == bt_f && (int)yabsys.LineCount >= bt_l0 && (int)yabsys.LineCount <= bt_l1) {
      const u32 *R = GetGenRegPtr();
      printf("bt l%d %s %08x pr %08x r0 %08x r1 %08x r4 %08x r5 %08x r6 %08x r8 %08x sr %03x\n", (int)yabsys.LineCount, is_slave_ ? "S" : "M", GET_PC(), GET_PR(),
             R[0], R[1], R[4], R[5], R[6], R[8], (unsigned)GET_SR());
    }
  }
#endif
  if (is_slave_ && rstv_strace_on) {
    // debug: ring buffer of slave block addresses (on change only)
    const u32 pc = GET_PC();
    if (rstv_strace[(rstv_strace_n - 1) & 1023] != pc)
      rstv_strace[(rstv_strace_n++) & 1023] = pc;
    if (pc == 0x06002056 && !rstv_strace_hit) {
      rstv_strace_hit = 1;
      for (int k = 0; k < 16; k++) rstv_strace_regs[k] = GetGenRegPtr()[k];
      rstv_strace_regs[16] = GET_PR();
      const u32 p = GetGenRegPtr()[4] - 0x20;
      for (int k = 0; k < 32; k++) rstv_strace_mem[k] = memGetLong(p + k * 4);
      rstv_strace_mem[32] = memGetLong(0x260FFC48);
    }
  }
  m_pDynaSh2->curblock = (uintptr_t)pBlock;
  m_pDynaSh2->lookup = (uintptr_t)m_pCompiler->LookupTable;
#endif
#if defined(RSTV) && !defined(__vita__)
  u32 rstv_pre[24];
  rstv_dyn_regs(m_pDynaSh2, rstv_pre);
  static int rstv_depth;
  if (rstv_depth++ == 0) { extern int rstv_undo_on, rstv_dynchk_io, rstv_io_mode; extern unsigned rstv_undo_n, rstv_io_n, rstv_io_bad;
    rstv_undo_on = s_dynchk != 0; rstv_undo_n = 0; rstv_dynchk_io = 0; rstv_io_mode = s_dynchk != 0; rstv_io_n = 0; rstv_io_bad = 0; }
#endif
  ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
#if defined(RSTV) && !defined(__vita__)
  {
    u32 rstv_post[24];
    rstv_dyn_regs(m_pDynaSh2, rstv_post);
    if (--rstv_depth == 0) {
      extern int rstv_undo_on, rstv_io_mode;
      rstv_undo_on = 0;
      rstv_io_mode = 0;
      rstv_dynchk(ctx_, rstv_pre, rstv_post, is_slave_);
    } else {
      // nested (another CPU ran inside a memory handler of the outer block): the outer block is not comparable
      extern int rstv_dynchk_io;
      rstv_dynchk_io = 1;
    }
  }
#endif
#if defined(RSTV)
  rstv_act_pc = 0x10; // dispatcher (sampler)
  pBlock = (Block *)m_pDynaSh2->curblock; // the block that ran last (chaining, ADR-018)
#endif
#endif
  
  if ((GET_SR() & 0xF0) < GET_ICOUNT()) {
    this->CheckInterupt();
  }

  if (!m_pCompiler->debug_mode_ && (pBlock->flags&BLOCK_LOOP) ){
    if (m_pDynaSh2->SysReg[3] < pBlock->e_addr && m_pDynaSh2->SysReg[3] >= pBlock->b_addr) {
      return IN_INFINITY_LOOP;
    } else {
      return 0;
    }
  }
#if defined(RSTV)
  {
    // ReST-V: per-game idle-loop heads (stv.c, same table as the interpreter)
    const int w = is_slave_ ? 1 : 0;
    const u32 pc = m_pDynaSh2->SysReg[3];
    for (int k = rstv_idle_n[w] - 1; k >= 0; k--)
      if (pc == rstv_idle_pc[w][k]) {
        rstv_sleep_ = rstv_sleep_max > 0; // ADR-050
        return IN_INFINITY_LOOP;
      }
    if (!w && pc == rstv_once_fn && rstv_once_fn) {
      // ADR-042: per-scanline polling function while the game waits for vblank: once per frame, then rts
      if (rstv_once_last == rstv_vblank_n) {
        SET_PC(GET_PR());
        return IN_INFINITY_LOOP;
      }
      rstv_once_last = rstv_vblank_n;
    }
  }
#endif
  return 0;
}

bool operator < (const dIntcTbl & data1 , const dIntcTbl & data2 )
{
  return data1.level > data2.level;
} 
bool operator == (const dIntcTbl & data1 , const dIntcTbl & data2 )
{
  return ( data1.Vector == data2.Vector );
}

#if defined(RSTV)
#define RSTV_ILOCK() ILock()
#define RSTV_IUNLOCK() IUnlock()
#else
#define RSTV_ILOCK() YabThreadLock(mtx_)
#define RSTV_IUNLOCK() YabThreadUnLock(mtx_)
#endif
void DynarecSh2::RemoveInterrupt(u8 Vector, u8 level) {
  RSTV_ILOCK();
  m_IntruptTbl.remove_if([&](const dIntcTbl & n) { 
    return n.Vector == Vector; 
  });
  if (m_IntruptTbl.size() != 0) {
    m_IntruptTbl.sort();
    m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level << 4;
  }
  else {
    m_pDynaSh2->SysReg[5] = 0x0000;
  }
  RSTV_IUNLOCK();
}

void DynarecSh2::AddInterrupt( u8 Vector, u8 level )
{
  // Ignore Timer0 and Timer1 when masked
  //if ((Vector == 67 /*|| Vector == 68*/) && level <= ((m_pDynaSh2->CtrlReg[0] >> 4) & 0x0F)){
  //  LOG("Vector %d is skiped\n", Vector);
  //  return;
  //}

  dIntcTbl tmp;
  tmp.Vector = Vector;
  tmp.level  = level;

  RSTV_ILOCK();
  m_bIntruptSort = false;
  m_IntruptTbl.push_back(tmp);
  if( m_IntruptTbl.size() > 1 ) {
    m_IntruptTbl.sort();
    m_IntruptTbl.unique();
  }
  m_bIntruptSort = true;
  m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level<<4;
  RSTV_IUNLOCK();
}


int DynarecSh2::CheckInterupt(){

  interruput_chk_cnt_++;

  if( m_IntruptTbl.size() == 0 ) {
    return 0;
  }

  
    
  RSTV_ILOCK();  
  dlstIntct::iterator pos = m_IntruptTbl.begin();
  if( InterruptRutine((*pos).Vector, (*pos).level ) != 0 ) {
    m_IntruptTbl.pop_front();
    if( m_IntruptTbl.size() != 0 ) {
      m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level<<4;
    }else{
      m_pDynaSh2->SysReg[5] = 0x0000;
    }
    RSTV_IUNLOCK();
    return 1;
  }
  RSTV_IUNLOCK();
  return 0;
}

int DynarecSh2::InterruptRutine(u8 Vector, u8 level)
{
  if (((u32)level) > ((m_pDynaSh2->CtrlReg[0] >> 4) & 0x0F)) {

    u32 prepc = m_pDynaSh2->SysReg[3];

    interruput_cnt_++;
    m_pDynaSh2->GenReg[15] -= 4;
    MappedMemoryWriteLong(m_pDynaSh2->GenReg[15], m_pDynaSh2->CtrlReg[0],NULL);
    m_pDynaSh2->GenReg[15] -= 4;
    MappedMemoryWriteLong(m_pDynaSh2->GenReg[15], m_pDynaSh2->SysReg[3],NULL);
    if (level == 0x10) { //NMI
      m_pDynaSh2->CtrlReg[0] |= 0x000000F0;
    }
    else {
      m_pDynaSh2->CtrlReg[0] &= ~0x000000F0;
      m_pDynaSh2->CtrlReg[0] |= ((u32)(level << 4) & 0x000000F0);
    }
    m_pDynaSh2->SysReg[3] = memGetLong(m_pDynaSh2->CtrlReg[2] + (((u32)Vector) << 2));

    //LOG("**** [%s] Exception vecnum=%s(%x), PC=%08X to %08X, level=%08X\n", (is_slave_ == false) ? "M" : "S", ScuGetVectorString(Vector), Vector,prepc, m_pDynaSh2->SysReg[3], level);

#if defined(DEBUG_CPU)
//    LOG("**** [%s] Exception vecnum=%u, PC=%08X to %08X, level=%08X\n", (is_slave_==false)?"M":"S", Vector, prepc, m_pDynaSh2->SysReg[3], level);
#endif
    return 1;
  }
  return 0; 
}


int DynarecSh2GetDisasmebleString(string & out, u32 from, u32 to) {
  char linebuf[128];
  if (from > to) return -1;
  for (u32 i = from; i < (to+2); i += 2) {
    SH2Disasm(i, MappedMemoryReadWord(i,NULL), 0, NULL, linebuf);
    out += linebuf;
    out += "\n";
  }
  return 0;
}

int DynarecSh2::Resume() {
  statics_trigger_ = NORMAL;
  return 0;
}

void DynarecSh2::ShowStatics(){
#if defined(DEBUG_CPU)
  LOG("\nExec cnt %d loopskip_cnt_ = %d, interruput_chk_cnt_ = %d, interruput_cnt_ = %d\n", GET_COUNT() - pre_cnt_, loopskip_cnt_, interruput_chk_cnt_, interruput_cnt_ );
  pre_cnt_ = GET_COUNT();
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  loopskip_cnt_ = 0;

  switch (statics_trigger_) {
  case NORMAL:
    break;
  case REQUESTED:
    statics_trigger_ = COLLECTING;
    break;
  case COLLECTING:
    statics_trigger_ = FINISHED;
    while (FINISHED == statics_trigger_) {
      YabThreadUSleep(10000);
    }
    break;
  case FINISHED:
    break;
  }
#elif defined(EXECUTE_STAT)
  LOG("\nExec cnt %d loopskip_cnt_ = %d, interruput_chk_cnt_ = %d, interruput_cnt_ = %d\n", GET_COUNT() , loopskip_cnt_, interruput_chk_cnt_, interruput_cnt_ );
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  loopskip_cnt_ = 0;

for( auto i = compie_statics_.begin(); i != compie_statics_.end() ; ++i ) {
    if(i->second.time>100) LOG("%08X\t%d\t%d",i->first,i->second.count, i->second.time);
  }
compie_statics_.clear();
#endif
}

int DynarecSh2::GetCurrentStatics(MapCompileStatics & buf){
#if !defined(DEBUG_CPU)
  return -1;
#else

  if (statics_trigger_ != NORMAL && statics_trigger_ != FINISHED) return -1;

  statics_trigger_ = REQUESTED;
  while (statics_trigger_!= FINISHED) {
    YabThreadUSleep(10000);
  }
  
  buf = compie_statics_;
  compie_statics_.clear();
#endif
  return 0;
}

void DynarecSh2::ShowCompileInfo(){
  int i = 0;
  while (asm_list[i].func != NULL) {
    printf("%s: %d\n", opcode_list[i].mnem, asm_list[i].build_count);
    i++;
  }
}

void DynarecSh2::ResetCompileInfo() {
  int i = 0;
  while (asm_list[i].func != NULL) {
    asm_list[i].build_count = 0;
    i++;
  }
}

