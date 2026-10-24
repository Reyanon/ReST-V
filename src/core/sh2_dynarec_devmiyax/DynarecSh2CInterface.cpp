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

#include "../core.h"
#include <stdio.h>
#include <string.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <stdint.h>
#include "../sh2core.h"
#include "DynarecSh2.h"
#include "../debug.h"
#include "yabause.h"


#define SH2CORE_DYNAMIC             3
#define SH2CORE_DYNAMIC_DEBUG             4

extern "C" {
int SH2DynInit(void);
void SH2DynDeInit(void);
void SH2DynReset(SH2_struct *context);
void SH2DynDebugReset(SH2_struct *context);
void FASTCALL SH2DynExec(SH2_struct *context, u32 cycles);
void SH2DynGetRegisters(SH2_struct *context, sh2regs_struct *regs);
u32 SH2DynGetGPR(SH2_struct *context, int num);
u32 SH2DynGetSR(SH2_struct *context);
u32 SH2DynGetGBR(SH2_struct *context);
u32 SH2DynGetVBR(SH2_struct *context);
u32 SH2DynGetMACH(SH2_struct *context);
u32 SH2DynGetMACL(SH2_struct *context);
u32 SH2DynGetPR(SH2_struct *context);
u32 SH2DynGetPC(SH2_struct *context);
void SH2DynSetRegisters(SH2_struct *context, const sh2regs_struct *regs);
void SH2DynSetGPR(SH2_struct *context, int num, u32 value);
void SH2DynSetSR(SH2_struct *context, u32 value);
void SH2DynSetGBR(SH2_struct *context, u32 value);
void SH2DynSetVBR(SH2_struct *context, u32 value);
void SH2DynSetMACH(SH2_struct *context, u32 value);
void SH2DynSetMACL(SH2_struct *context, u32 value);
void SH2DynSetPR(SH2_struct *context, u32 value);
void SH2DynSetPC(SH2_struct *context, u32 value);
void SH2DynOnFrame(SH2_struct *context);
void SH2DynSendInterrupt(SH2_struct *context, u8 level, u8 vector);
void SH2DynRemoveInterrupt(SH2_struct *context, u8 level, u8 vector);
int SH2DynGetInterrupts(SH2_struct *context, interrupt_struct interrupts[MAX_INTERRUPTS]);
void SH2DynSetInterrupts(SH2_struct *context, int num_interrupts, const interrupt_struct interrupts[MAX_INTERRUPTS]);
void SH2DynWriteNotify(u32 start, u32 length);
void SH2DynAddCycle(SH2_struct *context, u32 value);

SH2Interface_struct SH2Dyn = {
  SH2CORE_DYNAMIC,
  "SH2 Dynamic Recompiler",

  SH2DynInit,
  SH2DynDeInit,
  SH2DynReset,
  SH2DynExec,

  SH2DynGetRegisters,
  SH2DynGetGPR,
  SH2DynGetSR,
  SH2DynGetGBR,
  SH2DynGetVBR,
  SH2DynGetMACH,
  SH2DynGetMACL,
  SH2DynGetPR,
  SH2DynGetPC,

  SH2DynSetRegisters,
  SH2DynSetGPR,
  SH2DynSetSR,
  SH2DynSetGBR,
  SH2DynSetVBR,
  SH2DynSetMACH,
  SH2DynSetMACL,
  SH2DynSetPR,
  SH2DynSetPC,
  SH2DynOnFrame,

  SH2DynSendInterrupt,
  SH2DynRemoveInterrupt,
  SH2DynGetInterrupts,
  SH2DynSetInterrupts,
  SH2DynWriteNotify,
  SH2DynAddCycle
};

SH2Interface_struct SH2DynDebug = {
  SH2CORE_DYNAMIC_DEBUG,
  "SH2 Dynamic Recompiler Debug",

  SH2DynInit,
  SH2DynDeInit,
  SH2DynDebugReset,
  SH2DynExec,

  SH2DynGetRegisters,
  SH2DynGetGPR,
  SH2DynGetSR,
  SH2DynGetGBR,
  SH2DynGetVBR,
  SH2DynGetMACH,
  SH2DynGetMACL,
  SH2DynGetPR,
  SH2DynGetPC,

  SH2DynSetRegisters,
  SH2DynSetGPR,
  SH2DynSetSR,
  SH2DynSetGBR,
  SH2DynSetVBR,
  SH2DynSetMACH,
  SH2DynSetMACL,
  SH2DynSetPR,
  SH2DynSetPC,
  SH2DynOnFrame,

  SH2DynSendInterrupt,
  SH2DynRemoveInterrupt,
  SH2DynGetInterrupts,
  SH2DynSetInterrupts,
  SH2DynWriteNotify,
  SH2DynAddCycle
};

int SH2DynInit(void) {
  return 0;
}

void SH2DynDeInit(void){
}

void SH2DynReset(SH2_struct *context) {

  if (context->ext == NULL) {
    DynarecSh2 * pctx = new DynarecSh2();
    context->ext = (void*)pctx;
    pctx->SetContext(context);
  }

  DynarecSh2 * pctx = (DynarecSh2*)context->ext;
  if (context->isslave) {
    pctx->SetSlave(true);
  }
  else {
    pctx->SetSlave(false);
  }
  CompileBlocks * block = CompileBlocks::getInstance();
#if defined(RSTV)
  /* ReST-V: the compiled code is shared by both CPUs and follows memory writes, so a CPU reset does not make it
     stale. Clearing the 9MB region on every slave reset (SMPC SSHON/SSHOFF/clock change) cost 110ms plus the
     recompilation of the master's code. Only the master's reset (system reset) starts the region over */
  static int rstv_blocks_ready;
  if (!rstv_blocks_ready || !context->isslave) {
    block->Init();
    if (CompileBlocks::at(1))
      CompileBlocks::at(1)->Init(); // ADR-136: the slave's own blocks start over with the master's
    rstv_blocks_ready = 1;
  }
#else
  block->Init();
#endif
  block->SetDebugMode(false);
  pctx->ResetCPU();
}

void SH2DynDebugReset(SH2_struct *context) {

  if (context->ext == NULL) {
    DynarecSh2 * pctx = new DynarecSh2();
    context->ext = (void*)pctx;
    pctx->SetContext(context);
  }

  DynarecSh2 * pctx = (DynarecSh2*)context->ext;
  if (context->isslave) {
    pctx->SetSlave(true);
  }
  else {
    pctx->SetSlave(false);
  }
  CompileBlocks * block = CompileBlocks::getInstance();
  block->Init();
  block->SetDebugMode(true);
  pctx->ResetCPU();
}

void FASTCALL SH2DynExec(SH2_struct *context, u32 cycles){
  DynarecSh2* pctx = ((DynarecSh2*)context->ext);
  pctx->SetCurrentContext();
  pctx->ExecuteCount(cycles);
}

// ADR-136: the I mask of a CPU (another thread may read it: only a hint for waking it)
extern "C" int SH2DynInterruptMask(SH2_struct *context)
{
  DynarecSh2 *pctx = (DynarecSh2 *)context->ext;
  return pctx ? (int)((pctx->GET_SR() >> 4) & 15) : 15;
}
// ADR-136: end an idle sleep (ADR-050) from another thread (it polls a flag that just changed)
extern "C" void SH2DynWake(SH2_struct *context)
{
  DynarecSh2 *pctx = (DynarecSh2 *)context->ext;
  if (pctx)
    pctx->WakeSleep();
}
// ADR-136: the slave thread compiles into its own block cache
extern "C" int SH2DynUseOwnBlocks(SH2_struct *context)
{
  DynarecSh2 *pctx = (DynarecSh2 *)context->ext;
  if (!pctx)
    return 0;
  // the slave's block cache comes from the code region: not enough left -> stay on the shared cache (no async)
  if (CompileBlocks::at(1) == NULL && rstv_jit_code_avail() < sizeof(Block) * 2048 + 64) {
    fprintf(stderr, "dynarec: no code memory for the slave block cache (%u bytes left)%c", (unsigned)rstv_jit_code_avail(), 10);
    return 0;
  }
  pctx->UseCompiler(CompileBlocks::getSlaveInstance());
  return 1;
}

void SH2DynSendInterrupt(SH2_struct *context, u8 vector, u8 level){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->AddInterrupt(vector, level);
}

void SH2DynRemoveInterrupt(SH2_struct *context, u8 vector, u8 level) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->RemoveInterrupt(vector, level);
}


int SH2DynGetInterrupts(SH2_struct *context, interrupt_struct interrupts[MAX_INTERRUPTS]){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  dlstIntct::iterator itr = pctx->m_IntruptTbl.begin();
  int i = 0;
  while (itr != pctx->m_IntruptTbl.end() && i < MAX_INTERRUPTS) {
    interrupts[i].level = itr->level;
    interrupts[i].vector = itr->Vector;
    itr++;
    i++;
  }

  // ReST-V (ADR-104): the count (was 0: states saved with the dynarec lost every pending interrupt, and a game that
  // was waiting for one desynchronised its CPUs after loading — twcup98 players running wild, then a RAM-clearing reset)
  return i;
}

void SH2DynSetInterrupts(SH2_struct *context, int num_interrupts, const interrupt_struct interrupts[MAX_INTERRUPTS]){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->m_IntruptTbl.clear();
  for (int i = 0; i < num_interrupts; i++) {
    dIntcTbl tmp;
    tmp.level = interrupts[i].level;
    tmp.Vector = interrupts[i].vector;
    pctx->AddInterrupt(interrupts[i].vector, interrupts[i].level );
  }
  return;
}

void SH2DynGetRegisters(SH2_struct *context, sh2regs_struct *regs) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  memcpy(regs->R, pctx->GetGenRegPtr(), sizeof(regs->R));
  regs->GBR = pctx->GET_GBR();
  regs->VBR = pctx->GET_VBR();
  regs->SR.all = pctx->GET_SR();
  regs->MACH = pctx->GET_MACH();
  regs->MACL = pctx->GET_MACL();
  regs->PC = pctx->GET_PC();
  regs->PR = pctx->GET_PR();
}

u32 SH2DynGetGPR(SH2_struct *context, int num){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GetGenRegPtr()[num];
}

u32 SH2DynGetSR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_SR();
}

u32 SH2DynGetGBR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_GBR();
}

u32 SH2DynGetVBR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_VBR();
}

u32 SH2DynGetMACH(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_MACH();
}

u32 SH2DynGetMACL(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_MACL();
}

u32 SH2DynGetPR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_PR();
}

u32 SH2DynGetPC(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_PC();
}

void SH2DynSetRegisters(SH2_struct *context, const sh2regs_struct *regs){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  memcpy(pctx->GetGenRegPtr(), regs->R , sizeof(regs->R));
  pctx->SET_GBR(regs->GBR);
  pctx->SET_VBR(regs->VBR);
  pctx->SET_SR(regs->SR.all);
  pctx->SET_MACH(regs->MACH);
  pctx->SET_MACL(regs->MACL);
  pctx->SET_PC(regs->PC);
  pctx->SET_PR(regs->PR);
}

void SH2DynSetGPR(SH2_struct *context, int num, u32 value) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->GetGenRegPtr()[num] = value;
}

void SH2DynSetSR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_SR(value);
}

void SH2DynSetGBR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_GBR(value);
}

void SH2DynSetVBR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_VBR(value);
}

void SH2DynSetMACH(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_MACH(value);
}

void SH2DynSetMACL(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_MACL(value);
}
void SH2DynSetPR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_PR(value);
}
void SH2DynSetPC(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_PC(value);
}

void SH2DynAddCycle(SH2_struct *context, u32 value) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->AddCycle(value);
}


// ADR-136: a write drops the code it hits in every block cache (master's, and the slave's when it has its own)
#define RSTV_CB_INVAL(tbl, idx) do { for (int k_ = 0; k_ < 2; k_++) { CompileBlocks *b_ = CompileBlocks::at(k_); if (!b_) continue;     Block *&e_ = b_->tbl[idx]; if (e_) { e_ = NULL; rstv_link_gen++; } } } while (0)
#define RSTV_CB_DIRTY(a) do { for (int k_ = 0; k_ < 2; k_++) { CompileBlocks *b_ = CompileBlocks::at(k_); if (b_) b_->setDirty(a); } } while (0)
void SH2DynWriteNotify(u32 start, u32 length){
  CompileBlocks * block = CompileBlocks::getInstance();
  
  switch (start & 0x0FF00000){
    // ROM
  case 0x00000000:
      RSTV_CB_INVAL(LookupTableRom, (start&0x000FFFFF)>>1);
    break;

  // Low Memory
  case 0x00200000:
    for (u32 addr = start; addr< start + length; addr += 2)
      RSTV_CB_INVAL(LookupTableLow, (addr&0x000FFFFF)>>1);
    break;
    // High Memory
  case 0x06000000:
    for( u32 addr = start; addr< start+length; addr+=2 )
#if defined(SET_DIRTY)
    RSTV_CB_DIRTY(addr);
#else
    RSTV_CB_INVAL(LookupTable, (addr&0x000FFFFF)>>1);
#endif
    break;

    // Cache
  default:
    if ((start & 0xFF000000) == 0xC0000000){
      RSTV_CB_INVAL(LookupTableC, (start&0x000FFFFF)>>1);
    }
    break;
  }
}

void SH2DynShowSttaics(SH2_struct * master, SH2_struct * slave ){
  CompileBlocks * block = CompileBlocks::getInstance();

  DynarecSh2 *pctx = (DynarecSh2*)master->ext;
  pctx->ShowStatics();

  DynarecSh2 *pctxs = (DynarecSh2*)slave->ext;
  pctxs->ShowStatics();

  block->ShowStatics();
}

//********************************************************************
// MemoyAcess from DynarecCPU
//********************************************************************

#if defined(__arm__) || defined(__aarch64__)
#pragma GCC push_options
#pragma GCC optimize ("O1")
#endif



#if defined(RSTV) && !defined(__vita__)
// ReST-V harness differential check (DynarecSh2.cpp rstv_dynchk): RAM writes of a block are logged so the
// interpreter can re-run the block from the same memory
extern "C" { int rstv_dynchk_io; int rstv_undo_on; unsigned rstv_undo_n; u32 rstv_undo_addr[4096]; u32 rstv_undo_val[4096]; u8 rstv_undo_size[4096]; u8 *rstv_undo_mem[4096]; }
static inline void rstv_undo(u8 *mem, u32 off, int size)
{
  if (!rstv_undo_on || rstv_undo_n >= 4096) return;
  const unsigned k = rstv_undo_n++;
  rstv_undo_mem[k] = mem; rstv_undo_addr[k] = off; rstv_undo_size[k] = (u8)size;
  rstv_undo_val[k] = size == 1 ? T2ReadByte(mem, off) : size == 2 ? T2ReadWord(mem, off) : T2ReadLong(mem, off);
}
#define RSTV_UNDO(mem, off, size) rstv_undo(mem, off, size)
#else
#define RSTV_UNDO(mem, off, size)
#endif

#if defined(RSTV)
// ReST-V: slow-path memory access counters by region (performance line)
extern "C" { unsigned rstv_dyn_mem[12]; }
static inline int rstv_mem_region(u32 a)
{
  a &= 0x0FFFFFFF;
  if (a >= 0x02000000 && a < 0x05000000) return 0;           // cart
  if (a >= 0x05A00000 && a < 0x05C00000) return 1;           // scsp
  if (a >= 0x05C00000 && a < 0x05E00000) return 2;           // vdp1
  if (a >= 0x05E00000 && a < 0x05F00000) return 3;           // vdp2 vram
  if (a >= 0x05FE0000 && a < 0x05FF0000) return 5;           // scu
  if (a >= 0x05F00000 && a < 0x06000000) return 4;           // vdp2 cram/regs
  if (a >= 0x0FFFF000) return 6;                             // on-chip
  if (a < 0x00100000) return 7;                              // bios
  if (a >= 0x00100000 && a < 0x00200000) return 8;           // smpc/backup
  if (a >= 0x00400000 && a < 0x00500000) return 9;           // stv io
  return 9;
}
#if !defined(__vita__)
#define RSTV_MEMCNT(a) (rstv_dyn_mem[rstv_mem_region(a)]++, rstv_dynchk_io = 1)
#else
#define RSTV_MEMCNT(a) (rstv_dyn_mem[rstv_mem_region(a)]++)
#endif
#define RSTV_HCNT(w) (rstv_dyn_mem[10 + (w)]++)
#else
#define RSTV_MEMCNT(a)
#define RSTV_HCNT(w)
#endif
#if !defined(__vita__)
// harness diag (RSTV_WWATCH=addr): log CPU stores to one high work RAM byte (its chunk is marked as code so the asm fast
// path sends them here)
extern "C" u8 rstv_code_chunk[0x100000 >> 5];
static u32 rstv_ww = 1;
static void rstv_wwatch(u32 addr, u32 size, u32 data)
{
  if (rstv_ww == 1) { const char *e = getenv("RSTV_WWATCH"); rstv_ww = e ? (u32)strtoul(e, NULL, 16) & 0xFFFFF : 0; if (rstv_ww) rstv_code_chunk[rstv_ww >> 5] = 1; }
  if (!rstv_ww) return;
  if ((addr & 0xDFF00000) != 0x06000000) return;
  const u32 o = addr & 0xFFFFF;
  if (o <= rstv_ww && o + size > rstv_ww) {
    extern u32 rstv_harness_frame;
    printf("ww f%u %s %08x=%0*x pc %08x%c", rstv_harness_frame, DynarecSh2::Cur()->IsSlave() ? "S" : "M", addr, (int)(size * 2), data, DynarecSh2::Cur()->GET_PC(), 10);
  }
}
#define RSTV_WW(a, n, d) rstv_wwatch(a, n, d)
#else
#define RSTV_WW(a, n, d)
#endif
void memSetByte(u32 addr , u8 data )
{
  RSTV_WW(addr, 1, data);
  dynaLock();
  u32 cycle = 0;
  //LOG("memSetWord %08X, %08X\n", addr, data);
  CompileBlocks * block = CompileBlocks::getInstance();
  switch (addr & 0xDFF00000)
  {
  // Low Memory
  case 0x00200000:
    RSTV_CB_INVAL(LookupTableLow, (addr&0x000FFFFF)>>1);
    RSTV_HCNT(1);
    RSTV_UNDO(LowWram, addr & 0xFFFFF, 1);
    T2WriteByte(LowWram, addr & 0xFFFFF, data);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 7;
    dynaFree();
    return;
    break;
  // High Memory
  case 0x06000000:
#if defined(SET_DIRTY)
    RSTV_CB_DIRTY(addr);
#else
    RSTV_CB_INVAL(LookupTable, (addr&0x000FFFFF)>>1);
#endif
    RSTV_UNDO(HighWram, addr & 0xFFFFF, 1);
    T2WriteByte(HighWram, addr & 0xFFFFF, data);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 2;
    dynaFree();
    return;
    break;

  // Cache
  default:
    if ((addr & 0xFF000000) == 0xC0000000)
    {
      RSTV_CB_INVAL(LookupTableC, (addr&0x000FFFFF)>>1);
    }
  }
  RSTV_MEMCNT(addr);
  MappedMemoryWriteByte(addr, data, &cycle);
  DynarecSh2::Cur()->memcycle_ += cycle;
  dynaFree();
}

void memSetWord(u32 addr, u16 data )
{
  RSTV_WW(addr, 2, data);
  dynaLock();
  u32 cycle = 0;
  //LOG("memSetWord %08X, %08X\n", addr, data);

  CompileBlocks * block = CompileBlocks::getInstance();
  switch (addr & 0xDFF00000)
  {
  // Low Memory
   case 0x00200000:
    RSTV_CB_INVAL(LookupTableLow, (addr&0x000FFFFF)>>1);
    RSTV_HCNT(1);
    RSTV_UNDO(LowWram, addr & 0xFFFFF, 2);
    T2WriteWord(LowWram, addr & 0xFFFFF, data);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 7;
    dynaFree();
    return;
    break;
  // High Memory
   case 0x06000000:  {
#if defined(SET_DIRTY)
     RSTV_CB_DIRTY(addr);
#else
     RSTV_CB_INVAL(LookupTable, (addr & 0x000FFFFF) >> 1);
#endif
    RSTV_UNDO(HighWram, addr & 0xFFFFF, 2);
    T2WriteWord(HighWram, addr & 0xFFFFF, data);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 2;
    dynaFree();
    return;
   }
    break;
  // Cache
  default:
    if ((addr & 0xFF000000) == 0xC0000000)
    {
      RSTV_CB_INVAL(LookupTableC, (addr&0x000FFFFF) >> 1);
    }
  }
  RSTV_MEMCNT(addr);
  MappedMemoryWriteWord(addr, data, &cycle);
  DynarecSh2::Cur()->memcycle_ += cycle;
  dynaFree();
}

void memSetLong(u32 addr , u32 data )
{
  RSTV_WW(addr, 4, data);
  dynaLock();
  //LOG("memSetLong %08X, %08X\n", addr, data);
  u32 cycle = 0;

  CompileBlocks * block = CompileBlocks::getInstance();
  switch (addr & 0xDFF00000)
  {  
    // Low Memory
  case 0x00200000:
    RSTV_CB_INVAL(LookupTableLow, (addr & 0x000FFFFF)>>1);
    RSTV_CB_INVAL(LookupTableLow, ((addr & 0x000FFFFF)>>1) + 1);
    RSTV_HCNT(1);
    RSTV_UNDO(LowWram, addr & 0xFFFFF, 4);
    T2WriteLong(LowWram, addr & 0xFFFFF, data);
    if(addr&0x20000000) DynarecSh2::Cur()->memcycle_ += 7;
    dynaFree();
    return;
    break;
  // High Memory
  case 0x06000000:
#if defined(SET_DIRTY)
    RSTV_CB_DIRTY(addr);
    RSTV_CB_DIRTY(addr+2);
#else
    RSTV_CB_INVAL(LookupTable, (addr & 0x000FFFFF) >> 1);
    RSTV_CB_INVAL(LookupTable, ((addr & 0x000FFFFF) >> 1) + 1);
#endif
    RSTV_UNDO(HighWram, addr & 0xFFFFF, 4);
#ifdef RSTV_IOREPLAY
    { extern void rstv_qlog(u32, u32); rstv_qlog(addr, data); }
#endif
    T2WriteLong(HighWram, addr & 0xFFFFF, data);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 2;
    dynaFree();
    return;
    break;

  // Cache
  default:
    if ((addr & 0xFF000000) == 0xC0000000)
    {
      RSTV_CB_INVAL(LookupTableC, (addr&0x000FFFFF)>>1);
    }
  }
  RSTV_MEMCNT(addr);
  MappedMemoryWriteLong(addr, data, &cycle);
  DynarecSh2::Cur()->memcycle_ += cycle;
  dynaFree();
}

u8 memGetByte(u32 addr)
{
  dynaLock();
  u8 val;
  u32 cycle = 0;
  
  switch (addr & 0xDFF00000)
  {
    // Low Memory
  case 0x00200000:
    RSTV_HCNT(0);
    val = T2ReadByte(LowWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 4;
    dynaFree();
    return val;
    break;
    // High Memory
  case 0x06000000:
    val = T2ReadByte(HighWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 2;
    dynaFree();
    return val;
    break;
  }
  RSTV_MEMCNT(addr);
  val = MappedMemoryReadByte(addr, &cycle);
  DynarecSh2::Cur()->memcycle_ += cycle;
  dynaFree();
  return val;
}
 
u16 memGetWord(u32 addr)
{
  dynaLock();
  u16 val;
  u32 cycle = 0;

  switch (addr & 0xDFF00000)
  {
  // Low Memory
  case 0x00200000:
    RSTV_HCNT(0);
    val = T2ReadWord(LowWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 4;
    dynaFree();
    return val;
    break;
    // High Memory
  case 0x06000000:
    val = T2ReadWord(HighWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 2;
    dynaFree();
    return val;
    break;
  }
  RSTV_MEMCNT(addr);
  val = MappedMemoryReadWord(addr, &cycle);
  DynarecSh2::Cur()->memcycle_ += cycle;
  dynaFree();
  return val;
}

u32 memGetLong(u32 addr)
{
  dynaLock();
  u32 val;
  u32 cycle = 0;
  switch (addr & 0xDFF00000)
  {
  // Low Memory
  case 0x00200000:
    RSTV_HCNT(0);
    val = T2ReadLong(LowWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 4;
    dynaFree();
    return val;
    break;
    // High Memory
  case 0x06000000:
    val = T2ReadLong(HighWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::Cur()->memcycle_ += 2;
    dynaFree();
    return val;
    break;
  }
  RSTV_MEMCNT(addr);
  val = MappedMemoryReadLong(addr, &cycle);
  DynarecSh2::Cur()->memcycle_ += cycle;
  dynaFree();
  return val;
}

#if defined(__arm__) || defined(__aarch64__)
#pragma GCC pop_options
#endif


void SH2DynOnFrame(SH2_struct *context) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_COUNT(0);
  pctx->onFrame();
}


//************************************************
// Callbacks from DynarecCPU
//************************************************
extern "C" {
  void SH2HandleBreakpoints(SH2_struct *context);
}

void DynaCheckBreakPoint(u32 pc) {
  CurrentSH2->regs.PC = pc;
  SH2HandleBreakpoints(CurrentSH2);
}


int DelayEachClock() {
  return 0;
}

int DebugDelayClock() {
  dynaLock();
  DynaCheckBreakPoint(DynarecSh2::Cur()->GET_PC());
  dynaFree();
  return 0;
}

int DebugEachClock() {
  dynaLock();

  #define INSTRUCTION_B(x) ((x & 0x0F00) >> 8)
  #define INSTRUCTION_C(x) ((x & 0x00F0) >> 4)

  //printf("PC:%08X\n",DynarecSh2::Cur()->GET_PC());

#if 0
  u32 pc = DynarecSh2::Cur()->GET_PC();
  u16 inst = memGetWord(pc);
  s32 m = INSTRUCTION_C(inst);
  s32 n = INSTRUCTION_B(inst);

  LOG("%08X: rotcrout R%d=%08X, SR=%08X\n", 
    DynarecSh2::Cur()->GET_PC(), 
    n,DynarecSh2::Cur()->GetGenRegPtr()[n], 
    DynarecSh2::Cur()->GET_SR());
#endif

#if 0  
  LOG("%08X: subc R%d=%08X R%d=%08X SR=%08X\n", 
    DynarecSh2::Cur()->GET_PC(), 
    m,DynarecSh2::Cur()->GetGenRegPtr()[m], 
    n,DynarecSh2::Cur()->GetGenRegPtr()[n], 
    DynarecSh2::Cur()->GET_SR());
#endif

#if 0
if( DynarecSh2::Cur()->GET_PC() >= 0x0602E3C2 &&  DynarecSh2::Cur()->GET_PC() < 0x0602E468 ) {
   u32 addrn = DynarecSh2::Cur()->GetGenRegPtr()[6]-4;
   u32 addrm = DynarecSh2::Cur()->GetGenRegPtr()[7]-4;
   printf("%08X: MACL R[%d]=%08X@%08X,R[%d]=%08X@%08X,MACH=%08X,MACL=%08X\n",
      DynarecSh2::Cur()->GET_PC(),
      6,addrn,MappedMemoryReadLong(addrn),
      7,addrm,MappedMemoryReadLong(addrm),
      DynarecSh2::Cur()->GET_MACH(),
      DynarecSh2::Cur()->GET_MACL()
   );
}
#endif

#if 0
  #define INSTRUCTION_B(x) ((x & 0x0F00) >> 8)
  #define INSTRUCTION_C(x) ((x & 0x00F0) >> 4)

  u32 pc = DynarecSh2::Cur()->GET_PC();
  u16 inst = memGetWord(pc);
  s32 m = INSTRUCTION_C(inst);
  s32 n = INSTRUCTION_B(inst);
  printf("%08X: DIV0S %04X R[%d]:%08X,R[%d]:%08X,SR:%08X\n", 
    DynarecSh2::Cur()->GET_PC(), 
    inst,
    m,DynarecSh2::Cur()->GetGenRegPtr()[m], 
    n,DynarecSh2::Cur()->GetGenRegPtr()[n], 
    DynarecSh2::Cur()->GET_SR());
#endif

#if 0
u32 pc = DynarecSh2::Cur()->GET_PC();
if( pc == 0x060133C8 ) {
  u16 inst = memGetWord(pc);
  s32 m = INSTRUCTION_C(inst);
  s32 n = INSTRUCTION_B(inst);
  printf("%08X: DIV1(O) m:%08X,n:%08X,SR:%08X\n", 
    DynarecSh2::Cur()->GET_PC(), 
    DynarecSh2::Cur()->GetGenRegPtr()[m], 
    DynarecSh2::Cur()->GetGenRegPtr()[n], 
    DynarecSh2::Cur()->GET_SR());
}
#endif


#ifdef DMPHISTORY
  CurrentSH2->pchistory_index++;
  CurrentSH2->pchistory[CurrentSH2->pchistory_index & (MAX_DMPHISTORY-1) ] = DynarecSh2::Cur()->GET_PC();
  //CurrentSH2->regshistory[CurrentSH2->pchistory_index & 0xFF] = NULL;
#endif
  DynaCheckBreakPoint(DynarecSh2::Cur()->GET_PC());

  if (DynarecSh2::Cur()->CheckOneStep()){
    dynaFree();
    return 1;
  }

  dynaFree();
  return 0;
}
  
int EachClock() {
  dynaLock();
  if (DynarecSh2::Cur()->CheckInterupt()) {
      dynaFree();
      return 1;
  }
  dynaFree();
  return 0;
}


}

