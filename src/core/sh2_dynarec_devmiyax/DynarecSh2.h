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

#ifndef _DYNAREC_SH2_H_
#define _DYNAREC_SH2_H_

#include <list>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <map>
#include <string>
#include <unordered_map>

#include <sys/types.h>
#include <stdint.h>

#include "debug.h"
extern "C" int rstv_hle_in_checked(u32 addr); // ADR-100: the write is inside a checked range (not just its chunk)
extern "C" unsigned rstv_hle_codegen; // ReST-V: moves on every write to code a native routine checked (ADR-073)
extern "C" unsigned char rstv_hle_chunk[0x100000 >> 5]; // ReST-V: 32-byte chunks of high work RAM a native routine checked
#include "threads.h"

//****************************************************
// Defiens
//****************************************************

// instruction Format
#define ZERO_F  0
#define N_F     1
#define M_F     2
#define NM_F    3
#define MD_F    4
#define ND4_F   5
#define NMD_F   6
#define D_F     7 
#define D12_F   8
#define ND8_F   9
#define I_F     10
#define NI_F    11

#if defined(RSTV)
/* ReST-V: executable memory through rstv_jit (Vita VM domain / mmap) */
#include "../rstv_jit.h"
#define ALLOCATE(x) rstv_jit_code_alloc(x);
#define FREEMEM(x,a)
#elif defined(_WINDOWS)
#include <Windows.h>
#define ALLOCATE(x) VirtualAlloc(NULL, x, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#define FREEMEM(x,a)	if(x){ VirtualFree(x, a,MEM_RELEASE ); x = NULL;}
#elif defined(ARCH_IS_LINUX)
#include <sys/mman.h>
#define ALLOCATE(x) mmap (NULL, x, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_FILE|MAP_PRIVATE ,-1, 0);
//#define ALLOCATE(x) mmap ((void*)0x6000000, x, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS ,-1, 0);
#define FREEMEM(x,a) munmap(x,a);
#else
#include <sys/mman.h>
#define ALLOCATE(x) mmap (NULL, x, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_FILE|MAP_PRIVATE ,-1, 0);
#define FREEMEM(x,a) munmap(x,a);
//#define ALLOCATE(x)	malloc(x)
//#define FREEMEM(x,a)	if(x){ free(x); x = NULL;}
#endif

const int MAX_INSTSIZE = 0xFFFF+1;

using std::list;
using std::map;
using std::string;

#if defined(RSTV)
// ReST-V (ADR-036): blocks containing an address. Usually 1-2 entries: kept inline (std::list allocated a node per
// compiled instruction). Same operations as the std::list the dynarec used.
struct addrs {
  u32 in_[2];
  u32 *ext_ = nullptr;
  u16 n_ = 0, cap_ = 2;
  ~addrs() { free(ext_); }
  u32 *data() { return ext_ ? ext_ : in_; }
  size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  u32 *begin() { return data(); }
  u32 *end() { return data() + n_; }
  u32 back() { return data()[n_ - 1]; }
  void push_back(u32 v) {
    if (n_ == cap_) {
      const u16 nc = (u16)(cap_ * 2);
      u32 *e = (u32 *)malloc(nc * sizeof(u32));
      memcpy(e, data(), n_ * sizeof(u32));
      free(ext_);
      ext_ = e;
      cap_ = nc;
    }
    data()[n_++] = v;
  }
  void clear() { n_ = 0; }
  void remove(u32 v) {
    u32 *d = data();
    u16 w = 0;
    for (u16 i = 0; i < n_; i++)
      if (d[i] != v) d[w++] = d[i];
    n_ = w;
  }
};
#else
typedef list<u32> addrs;
#endif

struct CompileStaticsNode {
  u32 time;
  u32 count;
  u32 end_addr;
};

typedef map<u32, CompileStaticsNode> MapCompileStatics;

//****************************************************
// Structs
//****************************************************

#if defined(RSTV)
// ReST-V (ADR-085): 8192 slots of 1KB (was 2048 of 4KB, same 8.5MB: Vita VM block allocation fails at ~17MB). Blocks average
// about 430 bytes of ARM code, so the 4KB slots held a quarter as many blocks: going back to a scene recompiled hundreds of
// blocks in one frame (30-45ms). A longer block now ends early and goes on through rstv_chain.
// The block header offset (MAXBLOCKSIZE) is also in rstv_memfast_arm.s (rstv_chain, rstv_chain_ic)
const int NUMOFBLOCKS = 1024*8;
const int MAXBLOCKSIZE = 1024;
#else
const int NUMOFBLOCKS = 1024*4;
//const int MAXBLOCKSIZE = 3072-(4*4);
const int MAXBLOCKSIZE = 4096;
#endif
#define MAINMEMORY_SIZE (0x100000);
#define ROM_SIZE (0x80000);

struct Block
{
  u8  code[MAXBLOCKSIZE];
  u32 b_addr; //beginning PC
  u32 e_addr; //ending PC
  u32 id;
  u32 flags;
};

#define BLOCK_LOOP  (0x01)
#define BLOCK_WRITE (0x02)
#define BLOCK_NOCHAIN (0x04) // ReST-V: idle-loop head, always return to Execute before it

#define IN_INFINITY_LOOP (-1)

// Sh2 Registris
extern "C" { extern u32 rstv_link_gen; } // ADR-048: bumped whenever a compiled block is removed or replaced
struct tagSH2
{
  u32 GenReg[16];
  u32 CtrlReg[3];
  u32 SysReg[6];
  uintptr_t getmembyte;
  uintptr_t getmemword;
  uintptr_t getmemlong;
  uintptr_t setmembyte;
  uintptr_t setmemword;
  uintptr_t setmemlong;
  uintptr_t eachclock;
  u32 exitcount;          // 128
  // ReST-V block chaining (ADR-018), offsets fixed for rstv_memfast_arm.s
  uintptr_t chain;        // 132: where block exits jump (rstv_chain or rstv_chain_off)
  uintptr_t memcycle_ptr; // 136: &DynarecSh2::memcycle_
  uintptr_t lookup;       // 140: CompileBlocks::LookupTable (high work RAM blocks)
  uintptr_t curblock;     // 144: Block that ran last (set by Execute and rstv_chain)
  uintptr_t hwram;        // 148: HighWram (inline high work RAM reads in templates, ADR-033)
  uintptr_t chain_ic;     // 152: slow path of the linked exit stubs (rstv_chain_ic, ADR-048)
  uintptr_t macl_fn;      // 156: mac.l helper (ADR-049: the 272-byte template made the SGL code too big for the caches)
};

// Instruction
struct i_desc
{
  s32 format;
  const char *mnem;
  u16 mask;
  u16 bits;
  u8 dat;
  void (FASTCALL *func)(tagSH2*);
} ;

 
extern i_desc opcode_list[];


inline u32 adress_mask(u32 addr) {
  return (addr & 0x000FFFFF)>>1;
}

// Interrupt Table
struct dIntcTbl
{
  u8 Vector;
  u8 level;
};
#if defined(RSTV)
// ReST-V: fixed-size interrupt queue with the std::list operations the core uses (no allocation per
// interrupt; HBlank sends two every line). sort is stable by level (high first), unique drops adjacent same vectors.
struct dlstIntct {
  dIntcTbl e[64];
  int n = 0;
  typedef dIntcTbl *iterator;
  void clear() { n = 0; }
  size_t size() const { return (size_t)n; }
  dIntcTbl *begin() { return e; }
  dIntcTbl *end() { return e + n; }
  void push_back(const dIntcTbl &t) { if (n < 64) e[n++] = t; }
  void pop_front() { for (int i = 1; i < n; i++) e[i - 1] = e[i]; if (n) n--; }
  void sort() {
    for (int i = 1; i < n; i++) {
      const dIntcTbl t = e[i];
      int j = i;
      while (j > 0 && e[j - 1].level < t.level) { e[j] = e[j - 1]; j--; }
      e[j] = t;
    }
  }
  void unique() {
    if (n < 2) return;
    int w = 1;
    for (int i = 1; i < n; i++)
      if (e[i].Vector != e[w - 1].Vector) e[w++] = e[i];
    n = w;
  }
  template <class P> void remove_if(P pred) {
    int w = 0;
    for (int i = 0; i < n; i++)
      if (!pred(e[i])) e[w++] = e[i];
    n = w;
  }
};
#else
typedef std::list<dIntcTbl> dlstIntct;
#endif


struct x86op_desc
{
  void(*func)();
  const unsigned short *size;
  const unsigned char *src;
  const unsigned char *dest;
  const unsigned char *off1;
  const unsigned char *imm;
  const unsigned char *off3;
  unsigned char delay;
  unsigned char cycle;
  unsigned char write_count;
  unsigned char build_count;

  x86op_desc(void(*ifunc)(), const unsigned short *isize, const unsigned char *isrc,
    const unsigned char *idest, const unsigned char *ioff1, const unsigned char *iimm,
    const unsigned char *ioff3, const unsigned char idelay, const unsigned char icycle, const unsigned char iwrite_count = 0)
  {
    func = ifunc;
    size = isize;
    src = isrc;
    dest = idest;
    off1 = ioff1;
    imm = iimm;
    off3 = ioff3;
    delay = idelay;
    cycle = icycle;
    write_count = iwrite_count;
    build_count = 0;
  };

};

#define SET_DIRTY
extern "C" {
  void DebugLog(const char * format, ...);
}

class CompileBlocks {
private:
  CompileBlocks(){
    dCode = NULL;
    debug_mode_ = false;
    BuildInstructionList();
    Init();
#ifdef SET_DIRTY
    LookupParentTable = new addrs[0x100000 >> 1];
#else
    LookupParentTable = NULL;
#endif
    compile_count_ = 0;
    exec_count_ = 0;
    remove_count_ = 0;
  }
  ~CompileBlocks(){
    FREEMEM(dCode, sizeof(Block)*NUMOFBLOCKS);
  }
  static CompileBlocks * instance_;
  bool show_code_ = false;
public:
  void setShowCode( bool b ){ show_code_ = b; }
  static CompileBlocks * getInstance(){
    if( instance_ == NULL ){
      instance_ = new CompileBlocks();
    }
    return instance_;
  }

  int blockCount;
  int LastMakeBlock; 
  bool debug_mode_;
  Block * g_CompleBlock;
  
  u8 dsh2_instructions[MAX_INSTSIZE];
  Block* LookupTable[0x100000>>1];    
  //addrs LookupParentTable[0x100000>>1];
  addrs * LookupParentTable;
  Block* LookupTableRom[0x80000>>1];
  Block* LookupTableLow[0x100000>>1];
  Block* LookupTableC[0x8000>>1];
  Block * dCode;
  
  std::unordered_map<u32, int> self_modify_block;

  inline void setDirty(u32 addr) {
    // ReST-V (ADR-073): only a write to code a native routine checked makes them check it again (data next to other
    // compiled code is written all the time: 500 re-hashes a frame in sanjeon's 3D games)
    if (rstv_hle_chunk[(addr & 0xFFFFF) >> 5] && rstv_hle_in_checked(addr))
      rstv_hle_codegen++;
    addr = adress_mask(addr);
    if (LookupParentTable[addr].size() == 0) return;
    for (auto it = LookupParentTable[addr].begin(); it != LookupParentTable[addr].end(); it++) {
      if (LookupTable[*it] != NULL) {
        for (u32 i = adress_mask(LookupTable[*it]->b_addr) ; i <= adress_mask(LookupTable[*it]->e_addr); i++ ) {
          if (i != addr) {
            LookupParentTable[i].remove(*it);
          }
        }
         LOG("%d %08X is removed", LookupTable[*it]->id, (*it) << 1);
        remove_count_++;
        self_modify_block[ (((*it) << 1) | 0x06000000) ] = LookupTable[*it]->id;
        LookupTable[*it] = NULL;
        rstv_link_gen++; // ADR-048
      }
    }
    LookupParentTable[addr].clear();
  }

  void Init();

  Block * CompileBlock( u32 pc, addrs * ParentT );

  void opcodePass(x86op_desc *op, u16 opcode, u8 *ptr);  
  int  opcodeIndex(u16 code );
  void FindOpCode(u16 opcode, u8 * instindex);
  void BuildInstructionList();

  int findFreeBlock(u32 pc);
  int EmmitCode(Block *page, addrs * ParentT = NULL);

  // statics
  u32 compile_count_ ;
  u32 exec_count_;
  u32 remove_count_;


  void ShowStatics();
  void SetDebugMode(bool debug) { debug_mode_ = debug;  }
};

typedef void(*dynaFunc)(tagSH2*);

class DynarecSh2
{
protected:
  SH2_struct *parent;
  tagSH2 *  m_pDynaSh2;
  CompileBlocks * m_pCompiler;
  int       m_ClockCounter;
  bool      m_bIntruptSort;
  bool one_step_;
  u32 pre_exe_count_;
  int rstv_sleep_ = 0, rstv_sleep_n_ = 0; // ADR-050: idle at an idle PC with no interrupt pending: skip slices
  bool is_slave_ = false;
  u32 pre_PC_;
  SH2_struct * ctx_;
  YabMutex * mtx_;
#if defined(RSTV)
  std::atomic_flag ilock_ = ATOMIC_FLAG_INIT; // ReST-V: the queue is touched by the sound thread rarely; spin instead of a kernel mutex
  void ILock() { while (ilock_.test_and_set(std::memory_order_acquire)) {} }
  void IUnlock() { ilock_.clear(std::memory_order_release); }
#endif
  bool logenable_;
#if defined(RSTV)
  int RstvTierRun(); // ADR-044: run a not yet compiled block with the interpreter
  int RstvHle(int kind); // ADR-045: C library routine run natively (1 = done)
  int RstvHleSglVtx(const struct RstvSglDesc *dsc, u32 *cyc); // ADR-047
  int RstvHleTkVtx(u32 *cyc); // ADR-055
  int RstvHleSjInterp(u32 *cyc); // ADR-056
  int RstvHleSglSpr(u32 *cyc, u32 *npc); // ADR-069
  int RstvHleSglOnScr(u32 *cyc); // ADR-074
  int RstvHleSglPoly(u32 *cyc, u32 *npc); // ADR-060
  int RstvHleSglPoly1(u32 *cyc, u32 *npc);
#if !defined(__vita__)
  int RstvHleCheck(int kind);
#endif
#endif

  u32 pre_cnt_;
  u32 interruput_chk_cnt_;
  u32 interruput_cnt_;
  u32 loopskip_cnt_;

  enum enDebugState {
    NORMAL,
    REQUESTED,
    COLLECTING,
    FINISHED,
  };
  
  enDebugState statics_trigger_ = NORMAL;
  MapCompileStatics compie_statics_;
  string message_buf;

public:
  DynarecSh2();
  ~DynarecSh2();
  static DynarecSh2 * CurrentContext;
  void SetCurrentContext(){ CurrentContext = this; }
  int RstvHleStub(); // ADR-081 (called from native-call blocks)
  struct Block *RstvHleOrigBlock(u32 pc); // ADR-081
  void SetSlave(bool is_slave) { is_slave_ = is_slave; }
  void SetContext(SH2_struct * ctx) { ctx_= ctx;}
  bool IsSlave() { return is_slave_;  }

  dlstIntct m_IntruptTbl;
  void RemoveInterrupt(u8 Vector, u8 level);
  void AddInterrupt( u8 Vector, u8 level );
  int CheckInterupt();
  int InterruptRutine(u8 Vector, u8 level);
  int CheckOneStep();

  void ResetCPU();  
  void ExecuteCount(u32 Count );
  void ExecuteNested(u32 Count); // ReST-V: input-capture run inside the other CPU's memory write
  int Execute();
  void Undecoded();

  void AddCycle(u32 cycle) {
    addcycle_ += cycle;
  }

  u32 addcycle_ = 0;
  u32 memcycle_ = 0;
  void ShowStatics();
  void ShowCompileInfo();
  void ResetCompileInfo();

  void onFrame(){
    m_pCompiler->self_modify_block.clear();
  }

  tagSH2 * getDynaSh(){ return m_pDynaSh2; }; 

  inline u32 * GetGenRegPtr() { return m_pDynaSh2->GenReg; }
  inline u32 GET_MACH() { return m_pDynaSh2->SysReg[0]; }
  inline u32 GET_MACL() { return m_pDynaSh2->SysReg[1]; }
  inline u32 GET_PR() { return m_pDynaSh2->SysReg[2]; }
  inline u32 GET_PC() { return m_pDynaSh2->SysReg[3]; }
  inline u32 GET_COUNT() { return m_pDynaSh2->SysReg[4]; } 
  inline u32 GET_ICOUNT() { return m_pDynaSh2->SysReg[5]; } 
  inline u32 GET_SR() { return m_pDynaSh2->CtrlReg[0]; }
  inline u32 GET_GBR() { return m_pDynaSh2->CtrlReg[1]; }
  inline u32 GET_VBR() { return m_pDynaSh2->CtrlReg[2]; }
  inline void SET_MACH( u32 v ) { m_pDynaSh2->SysReg[0] = v; }
  inline void SET_MACL( u32 v ) { m_pDynaSh2->SysReg[1] = v; }
  inline void SET_PR( u32 v ) { m_pDynaSh2->SysReg[2] = v; }
  inline void SET_PC( u32 v ) { m_pDynaSh2->SysReg[3] = v; }
  inline void SET_COUNT( u32 v ) { m_pDynaSh2->SysReg[4] = v; } 
  inline void SET_ICOUNT(u32 v ) { m_pDynaSh2->SysReg[5] = v; } 
  inline void SET_SR(u32 v ) { m_pDynaSh2->CtrlReg[0] = v; }
  inline void SET_GBR( u32 v ) { m_pDynaSh2->CtrlReg[1] = v; }
  inline void SET_VBR( u32 v ) { m_pDynaSh2->CtrlReg[2] = v; }  

  int GetCurrentStatics(MapCompileStatics & buf);
  int Resume();
};


// callback from cpu emulation
extern "C"
{
  int EachClock();
  int DelayEachClock();
  int DebugEachClock();
  int DebugDelayClock();

  void memSetByte(u32, u8 );
  void memSetWord(u32, u16);
  void memSetLong(u32, u32);

  u8 memGetByte(u32);
  u16 memGetWord(u32);
  u32 memGetLong(u32);  
  
}

#ifdef _WINDOWS

#define dynaLock()	__asm \
{                         \
    __asm push edx         \
   /*__asm push ebx*/         \
}
#define dynaFree()	__asm \
{                         \
   /*__asm pop ebx*/          \
   __asm pop edx          \
}

#else
#define dynaLock()
#define dynaFree()
#endif


#endif // _DYNAREC_SH2_H_
