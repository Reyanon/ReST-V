/*  ReST-V (ADR-110): SCSP DSP program -> ARM (A32) code, one call per sample.
    Same results as ScspDspRun (scspdsp.c): the interpreter spent 18-23 ms a frame on the Vita for bakubaku / colmns97
    (about 130 cycles a step, most of it register spills and branches over unused fields). Each step here only gets the
    instructions its fields need.
    - registers: r4 inputs, r5 shift_reg, r6 y_reg, r7 frc_reg, r8 read_value, r9 mdec_ct, r10 sound RAM, r11 dsp
    - the memory read / write pending after a step is known from the program (mrd / mwt of the earlier steps), so only the
      state at the start of a sample can differ: the code checks it and returns 0 (the caller runs ScspDspRun) when it is
      not the one the code was made for (first sample, program change, state load)
    - code is made again when the program (MPRO), its length, RBL or RBP changes
    GPL-2.0-or-later. */
#include "scsp.h"
#include "scspdsp.h"
#include "rstv_jit.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__arm__)

u32 rstv_dsp_i2f(u32 v); // scspdsp.c: int_to_float

typedef int (*DspFn)(ScspDsp *dsp, u16 *ram);

#define CODE_SIZE (64 * 1024)
// The sound thread makes the code in s_stage; the emu thread copies it into the code region (rstv_dspjit_install, once a
// frame): on the Vita only the thread that opened the VM domain may write there (the sound thread got data aborts)
enum { ST_NONE, ST_STAGED, ST_LIVE, ST_COPYING };
static u32 s_stage[CODE_SIZE / 4];
static size_t s_stage_len;
static volatile int s_state;
static u32 *s_code;
static DspFn s_fn;
static int s_ok; // s_stage matches the program below (1), or no code for it (-1)
static u64 s_prog[128];
static int s_steps, s_rbl, s_rbp;
static int s_entry_rp, s_entry_wp; // pending read / write at the start of a sample the code was made for
static int s_exit_rp, s_exit_wp;
static unsigned s_calls;
static int s_cmp; // compare the program at the next call
int rstv_dspjit_on = 1;
unsigned rstv_dspjit_fallback, rstv_dspjit_builds;

// ---- A32 encoder ----
static u32 *s_p;
#define AL 0xE0000000u
enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC };
enum { OP_AND = 0, OP_EOR = 1, OP_SUB = 2, OP_RSB = 3, OP_ADD = 4, OP_CMP = 10, OP_ORR = 12, OP_MOV = 13, OP_BIC = 14 };
enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2 };
#define CC_EQ 0x00000000u
#define CC_NE 0x10000000u
#define CC_HI 0x80000000u
#define CC_LS 0x90000000u

static void emit(u32 w) { *s_p++ = w; }
// imm8 rotated right by 2*rot; returns -1 if not encodable
static int arm_imm(u32 v)
{
	for (int r = 0; r < 16; r++) {
		const u32 x = r ? ((v << (2 * r)) | (v >> (32 - 2 * r))) : v;
		if (x < 256)
			return (r << 8) | (int)x;
	}
	return -1;
}
static void dp_imm(u32 cc, int op, int rd, int rn, u32 imm)
{
	const int e = arm_imm(imm);
	if (e < 0)
		abort();
	emit(cc | 0x02000000u | (op << 21) | (op == OP_CMP ? 1u << 20 : 0) | (rn << 16) | (rd << 12) | (u32)e);
}
static void dp_reg(u32 cc, int op, int rd, int rn, int rm, int sh, int amt)
{
	emit(cc | (op << 21) | (op == OP_CMP ? 1u << 20 : 0) | (rn << 16) | (rd << 12) | ((amt & 31) << 7) | (sh << 5) | rm);
}
static void mov_reg(int rd, int rm) { dp_reg(AL, OP_MOV, rd, 0, rm, SH_LSL, 0); }
static void mov_sh(int rd, int rm, int sh, int amt) { dp_reg(AL, OP_MOV, rd, 0, rm, sh, amt); }
static void mov_asr_reg(int rd, int rm, int rs) { emit(AL | 0x01A00050u | (rd << 12) | (rs << 8) | rm); }
static void movw(int rd, u32 v) { emit(AL | 0x03000000u | ((v >> 12 & 15) << 16) | (rd << 12) | (v & 0xFFF)); }
static void movt(int rd, u32 v) { emit(AL | 0x03400000u | ((v >> 12 & 15) << 16) | (rd << 12) | (v & 0xFFF)); }
static void mov32(int rd, u32 v) { movw(rd, v & 0xFFFF); if (v >> 16) movt(rd, v >> 16); }
// rd = rn + imm (any imm, rd used as scratch when it does not encode)
static void add_imm(int rd, int rn, u32 imm)
{
	if (!imm) {
		if (rd != rn)
			mov_reg(rd, rn);
	} else if (arm_imm(imm) >= 0)
		dp_imm(AL, OP_ADD, rd, rn, imm);
	else if (arm_imm(-imm) >= 0)
		dp_imm(AL, OP_SUB, rd, rn, -imm);
	else {
		if (rd == rn)
			abort();
		mov32(rd, imm);
		dp_reg(AL, OP_ADD, rd, rd, rn, SH_LSL, 0);
	}
}
static void ldr_imm(int rt, int rn, int off) { emit(AL | 0x05900000u | (rn << 16) | (rt << 12) | off); }
static void str_imm(int rt, int rn, int off) { emit(AL | 0x05800000u | (rn << 16) | (rt << 12) | off); }
static void ldr_reg_lsl(int rt, int rn, int rm, int amt) { emit(AL | 0x07900000u | (rn << 16) | (rt << 12) | (amt << 7) | rm); }
static void str_reg_lsl(int rt, int rn, int rm, int amt) { emit(AL | 0x07800000u | (rn << 16) | (rt << 12) | (amt << 7) | rm); }
static void hw_imm(u32 bits, int rt, int rn, int off) { emit(AL | bits | (rn << 16) | (rt << 12) | ((off >> 4 & 15) << 8) | (off & 15)); }
static void ldrh_imm(int rt, int rn, int off) { hw_imm(0x01D000B0u, rt, rn, off); }
static void ldrsh_imm(int rt, int rn, int off) { hw_imm(0x01D000F0u, rt, rn, off); }
static void strh_imm(int rt, int rn, int off) { hw_imm(0x01C000B0u, rt, rn, off); }
static void ldrh_reg(int rt, int rn, int rm) { emit(AL | 0x019000B0u | (rn << 16) | (rt << 12) | rm); }
static void strh_reg(int rt, int rn, int rm) { emit(AL | 0x018000B0u | (rn << 16) | (rt << 12) | rm); }
static void sbfx(int rd, int rn, int lsb, int width) { emit(AL | 0x07A00050u | ((width - 1) << 16) | (rd << 12) | (lsb << 7) | rn); }
static void ubfx(int rd, int rn, int lsb, int width) { emit(AL | 0x07E00050u | ((width - 1) << 16) | (rd << 12) | (lsb << 7) | rn); }
static void ssat(int rd, int sat, int rn) { emit(AL | 0x06A00010u | ((sat - 1) << 16) | (rd << 12) | rn); }
static void smull(int rdlo, int rdhi, int rn, int rm) { emit(AL | 0x00C00090u | (rdhi << 16) | (rdlo << 12) | (rm << 8) | rn); }
static void blx_reg(int rm) { emit(AL | 0x012FFF30u | rm); }

// ---- struct offsets ----
#define OFF(f) ((int)offsetof(ScspDsp, f))
// halfword fields past ldrh's 255-byte reach are addressed from r11 + HB
#define HB 1792

static void ldrh_dsp(int rt, int off, int scratch)
{
	if (off < 256)
		ldrh_imm(rt, R11, off);
	else {
		dp_imm(AL, OP_ADD, scratch, R11, HB);
		ldrh_imm(rt, scratch, off - HB);
	}
}
static void strh_dsp(int rt, int off, int scratch)
{
	if (off < 256)
		strh_imm(rt, R11, off);
	else {
		dp_imm(AL, OP_ADD, scratch, R11, HB);
		strh_imm(rt, scratch, off - HB);
	}
}

typedef struct {
	u8 tra, twt, twa, xsel, ysel, ira, iwt, iwa, table, mwt, mrd, ewt, ewa, adrl, frcl, sh0, sh1, yrl, negb, zero, bsel, nofl,
		coef, masa, adreb, nxadr;
} Step;
static void decode(Step *d, u64 w)
{
	union ScspDspInstruction inst;
	inst.all = w;
	d->tra = inst.part.tra; d->twt = inst.part.twt; d->twa = inst.part.twa;
	d->xsel = inst.part.xsel; d->ysel = inst.part.ysel; d->ira = inst.part.ira; d->iwt = inst.part.iwt;
	d->iwa = inst.part.iwa; d->table = inst.part.table; d->mwt = inst.part.mwt; d->mrd = inst.part.mrd;
	d->ewt = inst.part.ewt; d->ewa = inst.part.ewa; d->adrl = inst.part.adrl; d->frcl = inst.part.frcl;
	d->sh0 = inst.part.shift0; d->sh1 = inst.part.shift1; d->yrl = inst.part.yrl; d->negb = inst.part.negb;
	d->zero = inst.part.zero; d->bsel = inst.part.bsel; d->nofl = inst.part.nofl; d->coef = inst.part.coef;
	d->masa = inst.part.masa; d->adreb = inst.part.adreb; d->nxadr = inst.part.nxadr;
}

// pending memory operation at a step: 0 none, 1 read (float), 2 read (nofl), 3 write
static void simulate(const Step *st, int n, int *rp, int *wp, u8 *op)
{
	for (int i = 0; i < n; i++) {
		int o = 0;
		if (*rp) {
			o = *rp;
			*rp = 0;
		} else if (*wp) {
			o = 3;
			*wp = 0;
		}
		if (op)
			op[i] = (u8)o;
		if (st[i].mrd)
			*rp = 1 + st[i].nofl;
		if (st[i].mwt)
			*wp = 1;
	}
}

static int build(const ScspDsp *dsp)
{
	static Step st[128];
	static u8 op[129];
	const int n = dsp->last_step;
	for (int i = 0; i < n; i++)
		decode(&st[i], dsp->mpro[i]);
	// state at the start of a sample in the steady run: two passes from idle
	int rp = 0, wp = 0;
	simulate(st, n, &rp, &wp, NULL);
	simulate(st, n, &rp, &wp, NULL);
	const int erp = rp, ewp = wp;
	simulate(st, n, &rp, &wp, op);
	if (rp != erp || wp != ewp)
		return 0; // no fixed point: leave it to the interpreter
	s_entry_rp = erp; s_entry_wp = ewp;
	s_exit_rp = rp; s_exit_wp = wp;
	{ // operation at the next sample's step 0 (needs the io address of the last step)
		int r2 = rp, w2 = wp;
		op[n] = (u8)(r2 ? r2 : w2 ? 3 : 0);
	}
	const int ring_bits = 13 + dsp->rbl;
	const u32 rbp = (u32)dsp->rbp << 12;

	s_p = s_stage;
	emit(0xE92D4FF8u); // push {r3-r11, lr}
	mov_reg(R11, R0);
	mov_reg(R10, R1);
	ldr_imm(R1, R11, OFF(read_pending));
	ldr_imm(R2, R11, OFF(write_pending));
	dp_imm(AL, OP_CMP, 0, R1, (u32)erp);
	dp_imm(CC_EQ, OP_CMP, 0, R2, (u32)ewp);
	dp_imm(CC_NE, OP_MOV, R0, 0, 0);
	emit(CC_NE | 0x08BD8FF8u); // popne {r3-r11, pc}
	ldr_imm(R4, R11, OFF(inputs));
	ldr_imm(R5, R11, OFF(shift_reg));
	ldr_imm(R6, R11, OFF(y_reg));
	ldrh_dsp(R7, OFF(frc_reg), R12);
	ldr_imm(R8, R11, OFF(read_value));
	ldr_imm(R9, R11, OFF(mdec_ct));

	for (int i = 0; i < n; i++) {
		const Step *d = &st[i];
		const int needF = d->ewt || d->twt || d->frcl || d->mwt || (d->adrl && d->sh0 && d->sh1);
		const int needT = !d->xsel || (!d->zero && !d->bsel);
		const int needI = d->xsel || (d->adrl && !(d->sh0 && d->sh1));
		const int needA = op[i + 1] != 0; // the next step (or sample) reads / writes at this step's address
		// inputs
		if (d->ira & 0x20) {
			if (d->ira & 0x10) {
				if (!(d->ira & 0xE)) {
					dp_imm(AL, OP_ADD, R12, R11, HB);
					ldrsh_imm(R4, R12, OFF(exts) - HB + 2 * (d->ira & 1));
					mov_sh(R4, R4, SH_LSL, 8);
				}
			} else {
				ldr_imm(R4, R11, OFF(mixs) + 4 * (d->ira & 0xF));
				mov_sh(R4, R4, SH_LSL, 4);
			}
		} else
			ldr_imm(R4, R11, OFF(mems) + 4 * (d->ira & 0x1F));
		if (needI)
			sbfx(R0, R4, 0, 24);
		if (needT) {
			add_imm(R1, R9, d->tra);
			dp_imm(AL, OP_AND, R1, R1, 0x7F);
			dp_reg(AL, OP_ADD, R1, R11, R1, SH_LSL, 2);
			ldr_imm(R1, R1, OFF(temp));
			sbfx(R1, R1, 0, 24);
		}
		// Y input (before YRL and FRCL change its sources)
		switch (d->ysel) {
		case 0: sbfx(R2, R7, 0, 13); break;
		case 1: ldrh_imm(R2, R11, OFF(coef) + 2 * d->coef); sbfx(R2, R2, 0, 13); break;
		case 2: sbfx(R2, R6, 11, 13); break;
		default: ubfx(R2, R6, 4, 12); break;
		}
		if (d->yrl)
			ubfx(R6, R4, 0, 24);
		if (needF) {
			sbfx(R3, R5, 0, 26);
			if (d->sh0 ^ d->sh1)
				mov_sh(R3, R3, SH_LSL, 1);
			if (!d->sh1)
				ssat(R3, 24, R3);
			ubfx(R3, R3, 0, 24);
		}
		if (d->ewt) {
			mov_sh(LR, R3, SH_LSR, 8);
			strh_dsp(LR, OFF(efreg) + 2 * d->ewa, R12);
		}
		if (d->twt) {
			add_imm(R12, R9, d->twa);
			dp_imm(AL, OP_AND, R12, R12, 0x7F);
			dp_reg(AL, OP_ADD, R12, R11, R12, SH_LSL, 2);
			str_imm(R3, R12, OFF(temp));
		}
		if (d->frcl) {
			if (d->sh0 & d->sh1)
				ubfx(R7, R3, 0, 12);
			else
				mov_sh(R7, R3, SH_LSR, 11);
		}
		// product = (sext13(Y) * X) >> 12, shift_reg = (product + SGA) & 0x3FFFFFF
		smull(R12, LR, R2, d->xsel ? R0 : R1);
		mov_sh(R12, R12, SH_LSR, 12);
		dp_reg(AL, OP_ORR, R12, R12, LR, SH_LSL, 20);
		if (i == n - 1) {
			str_imm(R12, R11, OFF(product));
			mov_sh(LR, LR, SH_ASR, 12);
			str_imm(LR, R11, OFF(product) + 4);
		}
		if (d->zero)
			mov_reg(R5, R12);
		else
			dp_reg(AL, d->negb ? OP_SUB : OP_ADD, R5, R12, d->bsel ? R5 : R1, SH_LSL, 0);
		ubfx(R5, R5, 0, 26);
		if (d->iwt)
			str_imm(R8, R11, OFF(mems) + 4 * d->iwa);
		// the read / write started by an earlier step
		if (op[i] == 1 || op[i] == 2) {
			ldr_imm(R12, R11, OFF(io_addr));
			dp_reg(AL, OP_ADD, R12, R10, R12, SH_LSL, 1);
			ldrh_imm(R12, R12, 0);
			if (op[i] == 2)
				mov_sh(R8, R12, SH_LSL, 8);
			else { // float_to_int
				ubfx(R1, R12, 11, 4);             // exponent
				mov_sh(R2, R12, SH_LSR, 15);       // sign
				dp_imm(AL, OP_CMP, 0, R1, 11);
				dp_imm(CC_HI, OP_MOV, R1, 0, 11);
				dp_reg(CC_HI, OP_MOV, LR, 0, R2, SH_LSL, 0);
				dp_imm(CC_LS, OP_EOR, LR, R2, 1);
				mov_sh(LR, LR, SH_LSL, 30);
				dp_reg(AL, OP_ORR, LR, LR, R2, SH_LSL, 31);
				ubfx(R2, R12, 0, 11);
				dp_reg(AL, OP_ORR, LR, LR, R2, SH_LSL, 19);
				dp_imm(AL, OP_ADD, R1, R1, 8);
				mov_asr_reg(R8, LR, R1);
			}
		} else if (op[i] == 3) {
			ldr_imm(R12, R11, OFF(io_addr));
			ldr_imm(LR, R11, OFF(write_value));
			dp_reg(AL, OP_ADD, R12, R10, R12, SH_LSL, 1);
			strh_imm(LR, R12, 0);
		}
		if (needA) {
			ldrh_imm(R1, R11, OFF(madrs) + 2 * d->masa);
			if (d->nxadr)
				dp_imm(AL, OP_ADD, R1, R1, 1);
			if (d->adreb) {
				ldrh_dsp(LR, OFF(adrs_reg), R12);
				sbfx(LR, LR, 0, 12);
				dp_reg(AL, OP_ADD, R1, R1, LR, SH_LSL, 0);
			}
			if (!d->table) {
				dp_reg(AL, OP_ADD, R1, R1, R9, SH_LSL, 0);
				ubfx(R1, R1, 0, ring_bits);
			} else
				ubfx(R1, R1, 0, 16);
			if (rbp) {
				if (arm_imm(rbp) >= 0)
					dp_imm(AL, OP_ADD, R1, R1, rbp);
				else {
					mov32(R12, rbp);
					dp_reg(AL, OP_ADD, R1, R1, R12, SH_LSL, 0);
				}
			}
			ubfx(R1, R1, 0, 18);
			str_imm(R1, R11, OFF(io_addr));
		}
		if (d->adrl) {
			if (d->sh0 & d->sh1)
				mov_sh(LR, R3, SH_LSR, 12);
			else
				ubfx(LR, R0, 16, 12);
			strh_dsp(LR, OFF(adrs_reg), R12);
		}
		if (d->mwt) {
			if (d->nofl) {
				mov_sh(R12, R3, SH_LSR, 8);
				str_imm(R12, R11, OFF(write_value));
			} else {
				mov_reg(R0, R3);
				mov32(R12, (u32)(uintptr_t)rstv_dsp_i2f);
				blx_reg(R12);
				str_imm(R0, R11, OFF(write_value));
			}
		}
	}
	str_imm(R4, R11, OFF(inputs));
	str_imm(R5, R11, OFF(shift_reg));
	str_imm(R6, R11, OFF(y_reg));
	strh_dsp(R7, OFF(frc_reg), R12);
	str_imm(R8, R11, OFF(read_value));
	dp_imm(AL, OP_MOV, R0, 0, (u32)s_exit_rp);
	str_imm(R0, R11, OFF(read_pending));
	dp_imm(AL, OP_MOV, R0, 0, (u32)s_exit_wp);
	str_imm(R0, R11, OFF(write_pending));
	dp_imm(AL, OP_MOV, R0, 0, 1);
	emit(0xE8BD8FF8u); // pop {r3-r11, pc}
	if ((size_t)((char *)s_p - (char *)s_stage) > CODE_SIZE)
		abort();
	s_stage_len = (size_t)((char *)s_p - (char *)s_stage);
	return 1;
}

int rstv_dspjit_run(ScspDsp *dsp, u8 *sound_ram)
{
	if (!rstv_dspjit_on || dsp->last_step <= 0)
		return 0;
#if !defined(__vita__)
	{
		static int env = -1;
		if (env < 0)
			env = getenv("RSTV_NODSPJIT") != NULL;
		if (env) {
			rstv_dspjit_on = 0;
			return 0;
		}
	}
#endif
	// MPRO writes ask for a compare (rstv_dspjit_dirty); the program is also compared now and then (state loads)
	if (s_ok && (s_steps != dsp->last_step || s_rbl != dsp->rbl || s_rbp != dsp->rbp))
		s_ok = 0;
	if (s_ok && (s_cmp || (++s_calls & 1023) == 0)) {
		s_cmp = 0;
		if (memcmp(s_prog, dsp->mpro, sizeof(u64) * (size_t)s_steps))
			s_ok = 0;
	}
	if (!s_ok) {
		// take the code back from the emu thread first (it may be copying the last one)
		for (;;) {
			const int st = s_state;
			if (st != ST_COPYING && __sync_bool_compare_and_swap(&s_state, st, ST_NONE))
				break;
		}
		s_steps = dsp->last_step;
		s_rbl = dsp->rbl;
		s_rbp = dsp->rbp;
		memcpy(s_prog, dsp->mpro, sizeof(u64) * (size_t)s_steps);
		s_ok = build(dsp) ? 1 : -1; // -1: no code for this program (interpreter until it changes)
		rstv_dspjit_builds++;
		if (s_ok > 0) {
			__sync_synchronize();
			s_state = ST_STAGED;
		}
	}
	if (s_ok < 0 || s_state != ST_LIVE)
		return 0;
	__sync_synchronize();
	if (!s_fn(dsp, (u16 *)sound_ram)) {
		rstv_dspjit_fallback++;
		return 0;
	}
	return 1;
}

void rstv_dspjit_dirty(void) { s_cmp = 1; } // MPRO written (often the same words again)

// emu thread, once a frame: put staged code in the code region
void rstv_dspjit_install(void)
{
	if (s_state != ST_STAGED || !__sync_bool_compare_and_swap(&s_state, ST_STAGED, ST_COPYING))
		return;
	if (!s_code) {
		s_code = (u32 *)rstv_jit_code_alloc(CODE_SIZE);
		if (!s_code) {
			rstv_dspjit_on = 0;
			s_state = ST_NONE;
			return;
		}
		s_fn = (DspFn)(void *)s_code;
	}
	rstv_jit_code_write_begin();
	memcpy(s_code, s_stage, s_stage_len);
	rstv_jit_code_write_end(s_code, s_stage_len);
	__sync_synchronize();
	s_state = ST_LIVE;
}

#else
int rstv_dspjit_on;
unsigned rstv_dspjit_fallback, rstv_dspjit_builds;
int rstv_dspjit_run(ScspDsp *dsp, u8 *sound_ram) { (void)dsp; (void)sound_ram; return 0; }
void rstv_dspjit_dirty(void) {}
void rstv_dspjit_install(void) {}
#endif
