/*  ReST-V (ADR-169): SCU DSP program -> ARM (A32) code.
    Same results as the interpreter loop in scu.c (rstv_dsp_interp): vfremix runs ~15k DSP instructions a frame (heavy
    frames ~32k), which took ~3 ms a frame on the Vita (60-90 cycles an instruction: branches over the instruction fields).
    Each instruction here gets only the code its fields need; the DSP state stays in scudspregs_struct (memory, L1).
    - registers: r4 = ScuDsp, r5 = cycle counter, r8 = &ScuDsp->PC (small offsets for the byte / half-word fields),
      r6 / r7 = taken flag / target of a delayed jump; r0-r3, r12, lr scratch
    - not compiled (the interpreter runs them, one instruction at a time): DMA, END / ENDI, MVI to PC, jumps whose delay
      slot is not a plain instruction. The code is entered only with T0 = 0, no DMA wait and no pending jump, so T0 is 0
      inside it and no data RAM access has to finish a DMA first.
    - the counter is decremented per instruction as the interpreter does (a delay slot costs nothing); the code returns
      at an instruction boundary when it reaches 0 (a jump decided but not yet done is stored as jmpaddr / delayed = 1)
    - the program is compared once per ScuExec slice (and after a DSP DMA into program RAM) and compiled again on change
    GPL-2.0-or-later. */
#include "core.h"
#include "scu.h"
#include "rstv_jit.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rstv_scujit_on = 1;
unsigned rstv_scujit_builds, rstv_scujit_runs;
static int s_dirty;
void rstv_scujit_dirty(void) { s_dirty = 1; }

#if defined(__arm__)

#define CODE_SIZE (160 * 1024)
typedef s32 (*JitFn)(scudspregs_struct *d, s32 counter);

static u32 *s_code;
static JitFn s_fn;
static u32 s_prog[256];
static int s_ok;          // 1: code for s_prog, -1: none (too large)
static u8 s_fb[256];      // 1: the interpreter runs this instruction
static u32 *s_entry[256]; // code address of each instruction (read by the dispatch at entry and by BTM)

// ---- A32 encoder ----
static u32 *s_p, *s_end;
static int s_overflow;
#define AL 0xE0000000u
enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC };
#define CC_EQ 0x00000000u
#define CC_NE 0x10000000u
#define CC_CS 0x20000000u
#define CC_CC 0x30000000u
#define CC_MI 0x40000000u
#define CC_GT 0xC0000000u
#define CC_LE 0xD0000000u
enum { OP_AND = 0, OP_EOR = 1, OP_SUB = 2, OP_RSB = 3, OP_ADD = 4, OP_ADC = 5, OP_TST = 8, OP_CMP = 10, OP_ORR = 12, OP_MOV = 13,
       OP_BIC = 14, OP_MVN = 15 };
enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3 };

static void emit(u32 w)
{
	if (s_p < s_end)
		*s_p++ = w;
	else
		s_overflow = 1;
}
static int arm_imm(u32 v, u32 *enc)
{
	for (u32 r = 0; r < 16; r++) {
		const u32 x = r ? ((v << (2 * r)) | (v >> (32 - 2 * r))) : v;
		if (x <= 0xFF) {
			*enc = (r << 8) | x;
			return 1;
		}
	}
	return 0;
}
// data processing with an immediate (must be encodable: only constants checked below are used)
static void dpi(u32 cc, int op, int s, int rd, int rn, u32 imm)
{
	u32 e = 0;
	if (!arm_imm(imm, &e))
		s_overflow = 1;
	emit(cc | 0x02000000u | ((u32)op << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | e);
}
static void dpr(u32 cc, int op, int s, int rd, int rn, int rm, int sh, int amt)
{
	emit(cc | ((u32)op << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | ((u32)amt << 7) | ((u32)sh << 5) | (u32)rm);
}
static void mov32(int rd, u32 v)
{
	u32 e;
	if (arm_imm(v, &e))
		emit(AL | 0x03A00000u | ((u32)rd << 12) | e);
	else if (arm_imm(~v, &e))
		emit(AL | 0x03E00000u | ((u32)rd << 12) | e);
	else {
		emit(AL | 0x03000000u | (((v >> 12) & 15) << 16) | ((u32)rd << 12) | (v & 0xFFF)); // movw
		if (v >> 16)
			emit(AL | 0x03400000u | (((v >> 28) & 15) << 16) | ((u32)rd << 12) | ((v >> 16) & 0xFFF)); // movt
	}
}
// word / byte loads and stores with a 12-bit offset (up = offset >= 0)
static void ls(u32 cc, u32 bits, int rt, int rn, int off)
{
	const u32 u = off >= 0 ? 0x00800000u : 0;
	emit(cc | bits | u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)(off >= 0 ? off : -off));
}
static void ldr(int rt, int rn, int off) { ls(AL, 0x05100000u, rt, rn, off); }
static void str(int rt, int rn, int off) { ls(AL, 0x05000000u, rt, rn, off); }
static void ldrb(int rt, int rn, int off) { ls(AL, 0x05500000u, rt, rn, off); }
static void strb_cc(u32 cc, int rt, int rn, int off) { ls(cc, 0x05400000u, rt, rn, off); }
static void strb(int rt, int rn, int off) { strb_cc(AL, rt, rn, off); }
// half words (offset 0..255)
static void ldrh(int rt, int rn, int off) { emit(AL | 0x01D000B0u | ((u32)rn << 16) | ((u32)rt << 12) | (((u32)off >> 4 & 15) << 8) | ((u32)off & 15)); }
static void strh(int rt, int rn, int off) { emit(AL | 0x01C000B0u | ((u32)rn << 16) | ((u32)rt << 12) | (((u32)off >> 4 & 15) << 8) | ((u32)off & 15)); }
static void b_to(u32 cc, const u32 *at, const u32 *target) { (void)at; emit(cc | 0x0A000000u | ((u32)(target - (s_p + 2)) & 0x00FFFFFFu)); }
static u32 *b_fwd(u32 cc) { u32 *at = s_p; emit(cc | 0x0A000000u); return at; }
static void patch(u32 *at, const u32 *target)
{
	if (at < s_end)
		*at = (*at & 0xFF000000u) | ((u32)(target - (at + 2)) & 0x00FFFFFFu);
}

// ---- fields ----
#define O(f) ((int)offsetof(scudspregs_struct, f))
#define OB(f) (O(f) - O(PC)) // relative to r8
#define PORT O(ProgControlPort)
#define FL_C (1u << 20)
#define FL_Z (1u << 21)
#define FL_S (1u << 22)
#define AC_LO OB(AC)
#define AC_HI (OB(AC) + 4)
#define P_LO OB(P)
#define P_HI (OB(P) + 4)
#define ALU_LO OB(ALU)
#define ALU_HI (OB(ALU) + 4)

static u32 s_inc; // CT increments pending in the instruction being compiled (bit n = CTn)

static void ct_inc(u32 mask)
{
	for (int k = 0; k < 4; k++)
		if (mask & (1u << k)) {
			ldrb(R12, R8, OB(CT) + k);
			dpi(AL, OP_ADD, 0, R12, R12, 1);
			dpi(AL, OP_AND, 0, R12, R12, 0x3F);
			strb(R12, R8, OB(CT) + k);
		}
}
// rd = data RAM n (0-3) at CTn
static void md_addr(int k)
{
	ldrb(R12, R8, OB(CT) + k);
	dpi(AL, OP_AND, 0, R12, R12, 0x3F);
	dpr(AL, OP_ADD, 0, R12, R4, R12, SH_LSL, 2);
}
// readgensrc
static void src(int n, int rd)
{
	if (n <= 7) {
		s_inc |= ((u32)(n >> 2) & 1) << (n & 3);
		md_addr(n & 3);
		ldr(rd, R12, O(MD) + 256 * (n & 3));
	} else if (n == 9)
		ldr(rd, R8, ALU_LO);
	else if (n == 0xA) {
		ldr(rd, R8, ALU_LO);
		ldr(R12, R8, ALU_HI);
		dpr(AL, OP_MOV, 0, rd, 0, rd, SH_LSR, 16);
		dpr(AL, OP_ORR, 0, rd, rd, R12, SH_LSL, 16);
	} else
		mov32(rd, 0xFFFFFFFFu);
}
// 64-bit field = (s32)r
static void store_sext(int off, int r)
{
	str(r, R8, off);
	dpr(AL, OP_MOV, 0, R12, 0, r, SH_ASR, 31);
	str(R12, R8, off + 4);
}
// writed1busdest (imm = 0) / writeloadimdest (imm = 1); value in r0
static void dest(int n, int imm)
{
	switch (n) {
	case 0: case 1: case 2: case 3:
		md_addr(n);
		str(R0, R12, O(MD) + 256 * n);
		s_inc |= 1u << n;
		break;
	case 4: str(R0, R8, OB(RX)); break;
	case 5: store_sext(P_LO, R0); break;
	case 6: case 7:
		if (imm) {
			dpi(AL, OP_BIC, 0, R0, R0, 0xFE000000u);
		}
		str(R0, R8, n == 6 ? OB(RA0) : OB(WA0));
		break;
	case 0xA:
		if (imm) {
			mov32(R12, 0x0FFF);
			dpr(AL, OP_AND, 0, R0, R0, R12, SH_LSL, 0);
		}
		strh(R0, R8, OB(LOP));
		break;
	case 0xB:
		if (!imm)
			strb(R0, R8, OB(TOP));
		break;
	case 0xC: case 0xD: case 0xE: case 0xF:
		if (!imm) {
			strb(R0, R8, OB(CT) + (n & 3));
			s_inc &= ~(1u << (n & 3));
		}
		break;
	default: break;
	}
}

// ALU stage: ALU = AC, then the operation (flags in the control port)
static void alu(u32 ins)
{
	const u32 op = ins >> 30 ? 0 : ins >> 26;
	ldr(R0, R8, AC_LO);
	ldr(R1, R8, AC_HI);
	switch (op) {
	case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x8: case 0x9: case 0xA: case 0xB: case 0xF:
		ldr(R2, R8, P_LO);
		ldr(R3, R4, PORT);
		dpi(AL, OP_BIC, 0, R3, R3, FL_C | FL_Z | FL_S);
		switch (op) {
		case 0x1: dpr(AL, OP_AND, 1, R0, R0, R2, SH_LSL, 0); break;
		case 0x2: dpr(AL, OP_ORR, 1, R0, R0, R2, SH_LSL, 0); break;
		case 0x3: dpr(AL, OP_EOR, 1, R0, R0, R2, SH_LSL, 0); break;
		case 0x4: dpr(AL, OP_ADD, 1, R0, R0, R2, SH_LSL, 0); dpi(CC_CS, OP_ORR, 0, R3, R3, FL_C); break;
		case 0x5: dpr(AL, OP_SUB, 1, R0, R0, R2, SH_LSL, 0); dpi(CC_CC, OP_ORR, 0, R3, R3, FL_C); break;
		case 0x8: dpr(AL, OP_MOV, 1, R0, 0, R0, SH_ASR, 1); dpi(CC_CS, OP_ORR, 0, R3, R3, FL_C); break;
		case 0x9: dpr(AL, OP_MOV, 1, R0, 0, R0, SH_ROR, 1); dpi(CC_CS, OP_ORR, 0, R3, R3, FL_C); break;
		case 0xA: dpr(AL, OP_MOV, 1, R0, 0, R0, SH_LSL, 1); dpi(CC_CS, OP_ORR, 0, R3, R3, FL_C); break;
		case 0xB:
			dpr(AL, OP_MOV, 1, R0, 0, R0, SH_LSL, 1);
			dpi(CC_CS, OP_ORR, 0, R3, R3, FL_C);
			dpi(AL, OP_ADC, 0, R0, R0, 0);
			dpi(AL, OP_CMP, 1, 0, R0, 0);
			break;
		case 0xF:
			dpi(AL, OP_TST, 1, 0, R0, 0x01000000u);
			dpi(CC_NE, OP_ORR, 0, R3, R3, FL_C);
			dpr(AL, OP_MOV, 0, R0, 0, R0, SH_ROR, 24);
			dpi(AL, OP_CMP, 1, 0, R0, 0);
			break;
		}
		dpi(CC_EQ, OP_ORR, 0, R3, R3, FL_Z);
		dpi(CC_MI, OP_ORR, 0, R3, R3, FL_S);
		str(R3, R4, PORT);
		break;
	case 0x6: // AD2
		ldr(R2, R8, P_LO);
		ldr(R12, R8, P_HI);
		ldr(R3, R4, PORT);
		dpi(AL, OP_BIC, 0, R3, R3, FL_C | FL_Z | FL_S);
		dpr(AL, OP_ADD, 1, R0, R0, R2, SH_LSL, 0);
		emit(AL | 0x06FF0070u | ((u32)R2 << 12) | R1);  // uxth r2, r1
		emit(AL | 0x06FF0070u | ((u32)LR << 12) | R12); // uxth lr, r12
		dpr(AL, OP_ADC, 0, R2, R2, LR, SH_LSL, 0);      // bit 16 = carry out of 48 bits
		dpr(AL, OP_ADC, 0, R1, R1, R12, SH_LSL, 0);
		dpi(AL, OP_TST, 1, 0, R2, 0x10000);
		dpi(CC_NE, OP_ORR, 0, R3, R3, FL_C);
		dpi(AL, OP_TST, 1, 0, R1, 0x8000);
		dpi(CC_NE, OP_ORR, 0, R3, R3, FL_S);
		dpr(AL, OP_ORR, 1, R2, R0, R1, SH_LSL, 0);
		dpi(CC_EQ, OP_ORR, 0, R3, R3, FL_Z);
		str(R3, R4, PORT);
		break;
	default: break;
	}
	str(R0, R8, ALU_LO);
	str(R1, R8, ALU_HI);
}

// condition of MVI / JMP (bits: 0x40 = test set, 0x20 = Z/S/C set, low bits Z 1 S 2 C 4 T0 8) -> -1 never, 1 always,
// 0 test: emitted as flags for `cc` (returned through *cc)
static int cond(u32 c, u32 *cc)
{
	const u32 neg = !(c & 0x20); // NZ, NS, NZS, NC, NT0
	const u32 k = c & 0x1F;
	if (k == 0x08)
		return neg ? 1 : -1; // T0 is 0 in this code
	u32 m;
	switch (k) {
	case 0x01: m = FL_Z; break;
	case 0x02: m = FL_S; break;
	case 0x03: m = FL_Z | FL_S; break;
	case 0x04: m = FL_C; break;
	default: return -2; // unknown
	}
	ldr(R3, R4, PORT);
	dpi(AL, OP_TST, 1, 0, R3, m);
	*cc = neg ? CC_EQ : CC_NE;
	return 0;
}

enum { K_PLAIN, K_JUMP, K_FALLBACK };
// kind of an instruction: plain (no control flow), jump (JMP, BTM, LPS: may set jmpaddr), fallback
static int kind(u32 ins)
{
	switch (ins >> 30) {
	case 0: case 1: return K_PLAIN;
	case 2: return ((ins >> 26) & 0xF) == 0xC ? K_FALLBACK : K_PLAIN; // MVI to PC
	default:
		switch ((ins >> 28) & 0xF) {
		case 0xC: case 0xF: return K_FALLBACK; // DMA, END
		case 0xD: {
			const u32 c = (ins >> 19) & 0x7F;
			if (c == 0)
				return K_JUMP;
			if (!(c & 0x40))
				return K_PLAIN; // unknown condition: no jump
			const u32 k = c & 0x1F;
			if (k == 0x08)
				return (c & 0x20) ? K_PLAIN : K_JUMP; // JMP T0 never, NT0 always
			return (k == 1 || k == 2 || k == 3 || k == 4) && (c & 0x60) != 0x20 ? K_JUMP : K_PLAIN;
		}
		case 0xE: return K_JUMP;
		default: return K_PLAIN;
		}
	}
}

// body of a plain instruction (no counter)
static void body(u32 ins)
{
	s_inc = 0;
	alu(ins);
	switch (ins >> 30) {
	case 0: {
		const u32 pm = (ins >> 23) & 3;
		if (pm == 2) {
			ldr(R0, R8, OB(RX));
			ldr(R1, R8, OB(RY));
			emit(AL | 0x00C00090u | ((u32)R3 << 16) | ((u32)R2 << 12) | ((u32)R1 << 8) | R0); // smull r2, r3, r0, r1
			str(R2, R8, P_LO);
			str(R3, R8, P_HI);
		} else if (pm == 3) {
			src((int)((ins >> 20) & 7), R0);
			store_sext(P_LO, R0);
		}
		if ((ins >> 23) & 4) {
			src((int)((ins >> 20) & 7), R0);
			str(R0, R8, OB(RX));
		}
		if ((ins >> 17) & 4) {
			src((int)((ins >> 14) & 7), R0);
			str(R0, R8, OB(RY));
		}
		switch ((ins >> 17) & 3) {
		case 1:
			mov32(R0, 0);
			str(R0, R8, AC_LO);
			str(R0, R8, AC_HI);
			break;
		case 2:
			ldr(R0, R8, ALU_LO);
			ldr(R1, R8, ALU_HI);
			str(R0, R8, AC_LO);
			str(R1, R8, AC_HI);
			break;
		case 3:
			src((int)((ins >> 14) & 7), R0);
			store_sext(AC_LO, R0);
			break;
		}
		switch ((ins >> 12) & 3) {
		case 1:
			ct_inc(s_inc);
			s_inc = 0;
			mov32(R0, (u32)(s32)(s8)(ins & 0xFF));
			dest((int)((ins >> 8) & 0xF), 0);
			break;
		case 3:
			src((int)(ins & 0xF), R0);
			dest((int)((ins >> 8) & 0xF), 0);
			break;
		}
		ct_inc(s_inc);
		break;
	}
	case 2: {
		const int dn = (int)((ins >> 26) & 0xF);
		if ((ins >> 25) & 1) {
			const u32 c = (ins >> 19) & 0x3F;
			u32 val = ins & 0x7FFFF;
			if (val & 0x40000)
				val |= 0xFFF80000u;
			// MVI conditions use the same codes as JMP without the 0x40 bit
			u32 cc = 0;
			const int t = (c == 0x01 || c == 0x02 || c == 0x03 || c == 0x04 || c == 0x08 || c == 0x21 || c == 0x22 ||
			               c == 0x23 || c == 0x24 || c == 0x28) ? cond(c, &cc) : -1;
			if (t < 0)
				break;
			u32 *skip = NULL;
			if (t == 0)
				skip = b_fwd(cc ^ 0x10000000u); // condition false -> skip
			mov32(R0, val);
			dest(dn, 1);
			ct_inc(s_inc);
			if (skip)
				patch(skip, s_p);
		} else {
			u32 val = ins & 0x1FFFFFF;
			if (val & 0x1000000)
				val |= 0xFE000000u;
			mov32(R0, val);
			dest(dn, 1);
			ct_inc(s_inc);
		}
		break;
	}
	default: break; // class 1 (invalid) and the other class 3 commands: only ALU = AC
	}
}

typedef struct { u32 *at; int target; } Fix;
static Fix s_fix[1024];
static int s_nfix;
static void b_ins(u32 cc, int target)
{
	if (s_nfix < (int)(sizeof(s_fix) / sizeof(s_fix[0]))) {
		s_fix[s_nfix].at = s_p;
		s_fix[s_nfix].target = target & 0xFF;
		s_nfix++;
	} else
		s_overflow = 1;
	emit(cc | 0x0A000000u);
}

static int build(void)
{
	static u32 *epi;
	s_p = s_code;
	s_end = s_code + CODE_SIZE / 4;
	s_overflow = 0;
	s_nfix = 0;
	// entry: r0 = ScuDsp, r1 = counter; dispatch on PC
	emit(0xE92D4FF0u); // push {r4-r11, lr}
	dpr(AL, OP_MOV, 0, R4, 0, R0, SH_LSL, 0);
	dpr(AL, OP_MOV, 0, R5, 0, R1, SH_LSL, 0);
	dpi(AL, OP_ADD, 0, R8, R4, (u32)O(PC) & 0xF00); // O(PC) = 0x804 on 32-bit ARM: two immediates
	if (O(PC) & 0xFF)
		dpi(AL, OP_ADD, 0, R8, R8, (u32)O(PC) & 0xFF);
	ldrb(R1, R8, 0);
	mov32(R12, (u32)(uintptr_t)s_entry);
	emit(AL | 0x0790F101u | ((u32)R12 << 16)); // ldr pc, [r12, r1, lsl #2]
	epi = s_p;
	dpr(AL, OP_MOV, 0, R0, 0, R5, SH_LSL, 0);
	emit(0xE8BD8FF0u); // pop {r4-r11, pc}
	for (int i = 0; i < 256; i++) {
		const u32 ins = s_prog[i];
		const int nx = (i + 1) & 0xFF;
		int k = kind(ins);
		if (k == K_JUMP && kind(s_prog[nx]) != K_PLAIN)
			k = K_FALLBACK;
		s_fb[i] = k == K_FALLBACK;
		s_entry[i] = s_p;
		if (k == K_FALLBACK) {
			mov32(R1, (u32)i);
			strb(R1, R8, 0);
			b_to(AL, s_p, epi);
			continue;
		}
		if (k == K_PLAIN) {
			body(ins);
			dpi(AL, OP_SUB, 1, R5, R5, 1);
			mov32(R1, (u32)nx); // mov: flags kept
			strb_cc(CC_LE, R1, R8, 0);
			b_to(CC_LE, s_p, epi);
			if (i == 255)
				b_ins(AL, 0);
			continue;
		}
		// jump: ALU = AC, decide (r6 = taken, r7 = target), counter, then delay slot + jump or fall through
		alu(ins);
		const u32 grp = (ins >> 28) & 0xF;
		int stat_target = -1; // static target, or -1: TOP (BTM)
		if (grp == 0xD) {
			const u32 c = (ins >> 19) & 0x7F;
			u32 cc = 0;
			const int t = c == 0 ? 1 : cond(c & 0x3F, &cc);
			mov32(R6, t == 1 ? 1u : 0u);
			if (t == 0)
				dpi(cc, OP_MOV, 0, R6, 0, 1);
			stat_target = (int)(ins & 0xFF);
			mov32(R7, ins & 0xFF);
		} else {
			// BTM / LPS: if (LOP) { jmpaddr = TOP / PC; LOP--; }
			ldrh(R0, R8, OB(LOP));
			mov32(R6, 0);
			if (ins & 0x8000000) {
				stat_target = i;
				mov32(R7, (u32)i);
			} else
				ldrb(R7, R8, OB(TOP));
			dpi(AL, OP_CMP, 1, 0, R0, 0);
			dpi(CC_NE, OP_SUB, 0, R0, R0, 1);
			emit(CC_NE | 0x01C000B0u | ((u32)R8 << 16) | ((u32)R0 << 12) | (((u32)OB(LOP) >> 4 & 15) << 8) | ((u32)OB(LOP) & 15)); // strhne
			dpi(CC_NE, OP_MOV, 0, R6, 0, 1);
		}
		dpi(AL, OP_SUB, 1, R5, R5, 1);
		u32 *go = b_fwd(CC_GT);
		// out of cycles after the jump instruction: store PC (and the pending jump)
		mov32(R1, (u32)nx);
		strb(R1, R8, 0);
		dpi(AL, OP_CMP, 1, 0, R6, 0);
		b_to(CC_EQ, s_p, epi);
		str(R7, R8, OB(jmpaddr));
		mov32(R1, 1);
		str(R1, R8, OB(delayed));
		b_to(AL, s_p, epi);
		patch(go, s_p);
		dpi(AL, OP_CMP, 1, 0, R6, 0);
		b_ins(CC_EQ, nx);
		// taken: the interpreter leaves delayed = 1 after the jump (jmpaddr goes back to -1)
		mov32(R1, 1);
		str(R1, R8, OB(delayed));
		body(s_prog[nx]); // delay slot (free: the interpreter adds the cycle back)
		if (stat_target >= 0)
			b_ins(AL, stat_target);
		else {
			mov32(R12, (u32)(uintptr_t)s_entry);
			emit(AL | 0x0790F107u | ((u32)R12 << 16)); // ldr pc, [r12, r7, lsl #2]
		}
		if (i == 255)
			b_ins(AL, 0);
	}
	for (int k = 0; k < s_nfix; k++)
		patch(s_fix[k].at, s_entry[s_fix[k].target]);
	return !s_overflow;
}

// once per ScuExec slice: compare the program, compile again on change. 0: no code (interpreter)
int rstv_scujit_prepare(scudspregs_struct *d)
{
	if (!rstv_scujit_on)
		return 0;
#if !defined(__vita__)
	{
		static int env = -1;
		if (env < 0)
			env = getenv("RSTV_NOSCUJIT") != NULL;
		if (env) {
			rstv_scujit_on = 0;
			return 0;
		}
	}
#endif
	if (!s_code) {
		s_code = (u32 *)rstv_jit_code_alloc(CODE_SIZE);
		if (!s_code) {
			rstv_scujit_on = 0;
			return 0;
		}
		s_fn = (JitFn)(void *)s_code;
	}
	s_dirty = 0;
	if (s_ok && !memcmp(s_prog, d->ProgramRam, sizeof(s_prog)))
		return s_ok > 0;
	memcpy(s_prog, d->ProgramRam, sizeof(s_prog));
	rstv_jit_code_write_begin();
	s_ok = build() ? 1 : -1;
	rstv_jit_code_write_end(s_code, (size_t)((char *)s_p - (char *)s_code));
	rstv_scujit_builds++;
	return s_ok > 0;
}

#if !defined(__vita__)
// harness RSTV_SCUJITCHK: every call is run again by the interpreter from the same state and compared
extern s32 rstv_dsp_interp_n(s32 counter, int maxn);
static int s_chk = -1;
static unsigned s_nchk, s_nbad;
#endif

// runs compiled code from the current PC; 0 when the interpreter must run the next instruction
int rstv_scujit_run(scudspregs_struct *d, s32 *counter)
{
	if (s_dirty && !rstv_scujit_prepare(d))
		return 0;
	if (s_ok <= 0 || s_fb[d->PC] || d->ProgControlPort.part.T0 || d->dsp_dma_wait > 0 || d->jmpaddr != (s32)0xFFFFFFFF)
		return 0;
	const s32 c0 = *counter;
#if !defined(__vita__)
	if (s_chk < 0)
		s_chk = getenv("RSTV_SCUJITCHK") != NULL;
	if (s_chk) {
		static scudspregs_struct before, jit;
		memcpy(&before, d, sizeof(before));
		const s32 c1 = s_fn(d, c0);
		memcpy(&jit, d, sizeof(jit));
		memcpy(d, &before, sizeof(before));
		const s32 c2 = rstv_dsp_interp_n(c0 - c1, 0x7FFFFFFF);
		s_nchk++;
		if (c2 != 0 || memcmp(&jit, d, sizeof(jit))) {
			s_nbad++;
			if (s_nbad <= 20) {
				printf("scujit BAD pc %02x c %d/%d (left %d) -> pc %02x/%02x", before.PC, c0, c1, c2, jit.PC, d->PC);
				const u32 *a = (const u32 *)&jit, *b = (const u32 *)d;
				for (u32 w = 0; w < sizeof(jit) / 4; w++)
					if (a[w] != b[w])
						printf(" @%x %08x/%08x", w * 4, a[w], b[w]);
				printf("\n");
			}
		} else if ((s_nchk & 0xFFF) == 1)
			printf("scujit ok %u (bad %u)\n", s_nchk, s_nbad);
		*counter = c1;
		rstv_scujit_runs++;
		return 1;
	}
#endif
	*counter = s_fn(d, c0);
	rstv_scujit_runs++;
	return 1;
}

#else
int rstv_scujit_prepare(scudspregs_struct *d) { (void)d; return 0; }
int rstv_scujit_run(scudspregs_struct *d, s32 *counter) { (void)d; (void)counter; return 0; }
#endif
