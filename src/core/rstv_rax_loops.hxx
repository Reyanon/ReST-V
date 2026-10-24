// ReST-V (ADR-180): native runs of the batmanfr sound driver's inner DO loops (RAX board, ADSP-2181).
// Included by rstv_rax.cpp, before adsp21xx_device::run().
//
// A loop body is listed by its instruction words; each word becomes inline code at compile time (nx<OP>) working on a
// local copy of the registers (RaxN), so the compiler keeps them in ARM registers and drops the flag work nobody reads.
// The semantics are the interpreter's (2100ops.hxx: mac_op_mr / mac_op_mf / alu_op_ar / alu_op_af, the DAG
// accessors, write_reg0), flags included. At the loop top, with the loop end on the loop stack (CE, CNTR >= 2), all
// iterations but the last one run here (as many as the slice allows); the last one goes through the normal path,
// which pops the loop as usual. Before a run the address range of each I register over all its iterations is worked
// out: a range in the control registers (3FE0-3FFF) or across 1FFF / 2000, a program write range over the loop itself,
// bit reverse on, or I registers other than the listed circular ones with L != 0 leave the loop to the usual paths.
// Inside, linear I registers (L = 0) step without the wrap, circular ones as the DAG does.
// Harness check: RSTV_RAXNLCHK=1 compares every native run with the interpreter running the same instructions.

struct RaxN
{
	uint16_t ax0, ax1, mx0, mx1, ay0, ay1, my0, my1, si, se, ar, af, mf, sr0, sr1, zero;
	uint64_t mr;
	uint32_t i[8], base[8], l[8];
	int32_t m[8];
	uint32_t az, an, av, ac, as, mv; // flags (0 / 1; AS is only read)
	uint32_t sticky, sat, shift;
	uint8_t px;
	uint16_t *mp[8]; // data memory of each static I register's range: the bank (0000-1FFF) or internal RAM, by address
	uint16_t *db;    // the bank (dynamic I registers)
	int skip, ran, xpc; // CF bodies: words to skip (-1: stopped), words run, where it stopped
	uint32_t last;   // the last word run
};

// ---- registers (read_reg0 / write_reg0 order: AX0 AX1 MX0 MX1 AY0 AY1 MY0 MY1 SI SE AR MR0 MR1 MR2 SR0 SR1) ----
template <int R> static RAX_AI uint16_t nr0(const RaxN &r)
{
	return R == 0 ? r.ax0 : R == 1 ? r.ax1 : R == 2 ? r.mx0 : R == 3 ? r.mx1 : R == 4 ? r.ay0 : R == 5 ? r.ay1 :
		R == 6 ? r.my0 : R == 7 ? r.my1 : R == 8 ? r.si : R == 9 ? r.se : R == 10 ? r.ar : R == 11 ? (uint16_t)r.mr :
		R == 12 ? (uint16_t)(r.mr >> 16) : R == 13 ? (uint16_t)(r.mr >> 32) : R == 14 ? r.sr0 : r.sr1;
}
template <int R> static RAX_AI int32_t nrd0(const RaxN &r) { return (int16_t)nr0<R>(r); }
template <int R> static RAX_AI void nwr0(RaxN &r, int32_t v)
{
	const uint16_t u = (uint16_t)v;
	switch (R) {
	case 0: r.ax0 = u; break;
	case 1: r.ax1 = u; break;
	case 2: r.mx0 = u; break;
	case 3: r.mx1 = u; break;
	case 4: r.ay0 = u; break;
	case 5: r.ay1 = u; break;
	case 6: r.my0 = u; break;
	case 7: r.my1 = u; break;
	case 8: r.si = u; break;
	case 9: r.se = (uint16_t)(int16_t)(int8_t)v; break;
	case 10: r.ar = u; break;
	case 11: r.mr = (r.mr & ~0xffffull) | u; break;
	case 12: r.mr = (r.mr & ~0xffffffff0000ull) | ((uint64_t)u << 16) | ((uint64_t)(uint16_t)((int16_t)u >> 15) << 32); break;
	case 13: r.mr = (r.mr & ~(0xffffull << 32)) | ((uint64_t)(uint16_t)(int16_t)(int8_t)v << 32); break;
	case 14: r.sr0 = u; break;
	case 15: r.sr1 = u; break;
	}
}
// MAC operands: X = MX0 MX1 AR MR0 MR1 MR2 SR0 SR1, Y = MY0 MY1 MF 0; ALU: X = AX0 AX1 AR MR0 MR1 MR2 SR0 SR1, Y = AY0 AY1 AF 0
template <int X> static RAX_AI uint16_t nmacx(const RaxN &r)
{
	return X == 0 ? r.mx0 : X == 1 ? r.mx1 : X == 2 ? r.ar : X == 3 ? (uint16_t)r.mr : X == 4 ? (uint16_t)(r.mr >> 16) :
		X == 5 ? (uint16_t)(r.mr >> 32) : X == 6 ? r.sr0 : r.sr1;
}
template <int Y> static RAX_AI uint16_t nmacy(const RaxN &r) { return Y == 0 ? r.my0 : Y == 1 ? r.my1 : Y == 2 ? r.mf : r.zero; }
template <int X> static RAX_AI uint16_t nalux(const RaxN &r)
{
	return X == 0 ? r.ax0 : X == 1 ? r.ax1 : X == 2 ? r.ar : X == 3 ? (uint16_t)r.mr : X == 4 ? (uint16_t)(r.mr >> 16) :
		X == 5 ? (uint16_t)(r.mr >> 32) : X == 6 ? r.sr0 : r.sr1;
}
template <int Y> static RAX_AI uint16_t naluy(const RaxN &r) { return Y == 0 ? r.ay0 : Y == 1 ? r.ay1 : Y == 2 ? r.af : r.zero; }

// ---- MAC (mac_op_mr / mac_op_mf, not the xop == yop forms) ----
template <uint32_t OP> static RAX_AI int64_t nmac(const RaxN &r)
{
	enum { X = (OP >> 8) & 7, Y = (OP >> 11) & 3, F = (OP >> 13) & 15 };
	const int32_t xs = (int16_t)nmacx<X>(r), ys = (int16_t)nmacy<Y>(r);
	const int32_t xu = nmacx<X>(r), yu = nmacy<Y>(r);
	// signed / unsigned operands by function: SS for 1-4, 8, 12; SU 5, 9, 13; US 6, 10, 14; UU 7, 11, 15
	const int32_t xo = (F == 6 || F == 7 || F == 10 || F == 11 || F == 14 || F == 15) ? xu : xs;
	const int32_t yo = (F == 5 || F == 7 || F == 9 || F == 11 || F == 13 || F == 15) ? yu : ys;
	int32_t temp = (int32_t)((uint32_t)((uint32_t)xo * (uint32_t)yo) << r.shift);
	const int64_t mr = (int64_t)r.mr;
	int64_t res;
	switch (F) {
	case 1: case 2: case 3:
		res = F == 1 ? (int64_t)temp : F == 2 ? mr + (int64_t)temp : mr - (int64_t)temp;
		temp &= 0xffff;
		res += 0x8000;
		if (temp == 0x8000)
			res &= ~((uint64_t)0x10000);
		break;
	case 4: case 5: case 6: case 7: res = (int64_t)temp; break;
	case 8: case 9: case 10: case 11: res = mr + (int64_t)temp; break;
	default: res = mr - (int64_t)temp; break;
	}
	return res;
}
template <uint32_t OP> static RAX_AI void nmac_mr(RaxN &r)
{
	if (((OP >> 13) & 15) == 0)
		return;
	const int64_t res = nmac<OP>(r);
	const uint32_t t = (uint32_t)(res >> 31) & 0x1ff;
	r.mv = t != 0x000 && t != 0x1ff;
	r.mr = (uint64_t)res;
}
template <uint32_t OP> static RAX_AI void nmac_mf(RaxN &r)
{
	if (((OP >> 13) & 15) == 0)
		return;
	r.mf = (uint16_t)((uint32_t)nmac<OP>(r) >> 16);
}

// ---- ALU (alu_op_ar / alu_op_af and their constant forms, without ABS or DIVS / DIVQ) ----
template <uint32_t OP, int TO_AR, int CONST> static RAX_AI void nalu(RaxN &r)
{
	enum { X = (OP >> 8) & 7, Y = (OP >> 11) & 3, F = (OP >> 13) & 15 };
	const int32_t x = nalux<X>(r), y = CONST ? constants[((OP >> 5) & 7) | ((OP >> 11) & 3) << 3] : naluy<Y>(r);
	int32_t res = 0;
	uint32_t v = 0, c = 0, nzvc = 0; // nzvc: N, Z, V, C from the sum / difference formulas
	int32_t s = 0, d = 0;
	switch (F) {
	case 0x00: res = y; break;
	case 0x01: res = y + 1; v = y == 0x7fff; c = y != 0x7fff && y == 0xffff; break;
	case 0x02: s = x; d = y + (int32_t)r.ac; res = s + d; nzvc = 1; break;
	case 0x03: s = x; d = y; res = x + y; nzvc = 1; break;
	case 0x04: res = y ^ 0xffff; break;
	case 0x05: res = -y; v = y == 0x8000; c = y == 0x0000; break;
	case 0x06: s = x; d = y; res = x - y + (int32_t)r.ac - 1; nzvc = 2; break;
	case 0x07: s = x; d = y; res = x - y; nzvc = 2; break;
	case 0x08: res = y - 1; v = y == 0x8000; c = y != 0x8000 && y == 0x0000; break;
	case 0x09: s = y; d = x; res = y - x; nzvc = 2; break;
	case 0x0a: s = y; d = x; res = y - x + (int32_t)r.ac - 1; nzvc = 2; break;
	case 0x0b: res = x ^ 0xffff; break;
	case 0x0c: res = x & y; break;
	case 0x0d: res = x | y; break;
	case 0x0e: res = x ^ y; break;
	}
	if (nzvc) {
		v = ((uint32_t)(s ^ d ^ res ^ (res >> 1)) >> 15) & 1;
		c = nzvc == 1 ? ((uint32_t)res >> 16) & 1 : ((uint32_t)~res >> 16) & 1;
	}
	r.an = ((uint32_t)res >> 15) & 1;
	r.az = (res & 0xffff) == 0;
	r.av = (r.sticky & r.av) | v; // CLR_FLAGS keeps V in sticky mode
	r.ac = c;
	if (TO_AR) {
		if (r.sat && r.av)
			res = r.ac ? -32768 : 32767;
		r.ar = (uint16_t)res;
	} else
		r.af = (uint16_t)res;
}

// function on the 4 kinds of ALU / MAC with a data move (op bits 16-17: MAC to MR, ALU to AR, MAC to MF, ALU to AF)
template <uint32_t OP, int K, int CONST = 0> static RAX_AI void namf(RaxN &r)
{
	switch (K & 3) {
	case 0: nmac_mr<OP>(r); break;
	case 1: nalu<OP, 1, CONST>(r); break;
	case 2: nmac_mf<OP>(r); break;
	case 3: nalu<OP, 0, CONST>(r); break;
	}
}

// ---- conditions (m_condition_table; CE is not used here) ----
template <int C> static RAX_AI int ncond(const RaxN &r)
{
	return C == 0 ? r.az : C == 1 ? !r.az : C == 2 ? !((r.an ^ r.av) | r.az) : C == 3 ? ((r.an ^ r.av) | r.az) :
		C == 4 ? (r.an ^ r.av) : C == 5 ? !(r.an ^ r.av) : C == 6 ? r.av : C == 7 ? !r.av : C == 8 ? r.ac : C == 9 ? !r.ac :
		C == 10 ? r.as : C == 11 ? !r.as : C == 12 ? r.mv : C == 13 ? !r.mv : 1;
}

// ---- shifter by SE (shift_op: LSHIFT / ASHIFT, HI / LO, OR); X = SI SI AR MR0 MR1 MR2 SR0 SR1 ----
template <int X> static RAX_AI uint16_t nshx(const RaxN &r)
{
	return X <= 1 ? r.si : X == 2 ? r.ar : X == 3 ? (uint16_t)r.mr : X == 4 ? (uint16_t)(r.mr >> 16) :
		X == 5 ? (uint16_t)(r.mr >> 32) : X == 6 ? r.sr0 : r.sr1;
}
template <uint32_t OP> static RAX_AI void nshift(RaxN &r)
{
	enum { X = (OP >> 8) & 7, F = (OP >> 11) & 15 };
	const int32_t sc = (int8_t)r.se;
	const uint32_t xu = nshx<X>(r);
	const int32_t xs = (int16_t)nshx<X>(r);
	uint32_t res;
	if (F <= 3) { // LSHIFT
		const uint32_t x = F <= 1 ? xu << 16 : xu;
		res = sc > 0 ? (sc < 32 ? x << sc : 0) : (sc > -32 ? x >> -sc : 0);
	} else { // ASHIFT
		const int32_t x = F <= 5 ? (int32_t)((uint32_t)xs << 16) : xs;
		res = sc > 0 ? (sc < 32 ? (uint32_t)x << sc : 0) : (uint32_t)(sc > -32 ? x >> -sc : x >> 31);
	}
	if (F & 1)
		res |= (uint32_t)r.sr0 | (uint32_t)r.sr1 << 16;
	r.sr0 = (uint16_t)res;
	r.sr1 = (uint16_t)(res >> 16);
}

// ---- DAG / memory ----
// CM: I registers stepped with the wrap (circular in this run); DY: I registers whose addresses are only known while
// the loop runs (their M is written in the body, or the body jumps): stepped with the wrap and checked at each use.
// Others step linearly (L = 0, or a run that never reaches the wrap) inside the range checked before the run.
template <int I, int M, uint32_t CM> static RAX_AI uint32_t ndag(RaxN &r)
{
	const uint32_t a = r.i[I];
	if (CM >> I & 1) {
		uint32_t i = (a + r.m[M]) & 0x3fff;
		if (i < r.base[I]) i += r.l[I];
		else if (i >= r.base[I] + r.l[I]) i -= r.l[I];
		r.i[I] = i;
	} else
		r.i[I] = a + r.m[M]; // no wrap in this run
	return a;
}
template <int I, uint32_t DY> static RAX_AI uint16_t *nmem(RaxN &r, uint32_t a) // the word at data address a
{
	return (DY >> I & 1) ? (a < 0x2000 ? &r.db[a] : &s_iram[a - 0x2000]) : &r.mp[I][a];
}
template <uint32_t OP, uint32_t CM, uint32_t DY> static RAX_AI uint16_t &ndm1(RaxN &r) // DM(I, M) through DAG1 (op bits 0-3)
{
	enum { I = (OP >> 2) & 3, M = OP & 3 };
	return *nmem<I, DY>(r, ndag<I, M, CM | DY>(r));
}
template <uint32_t OP, uint32_t CM, uint32_t DY> static RAX_AI uint16_t &ndm2(RaxN &r) // through DAG2
{
	enum { I = 4 + ((OP >> 2) & 3), M = 4 + (OP & 3) };
	return *nmem<I, DY>(r, ndag<I, M, CM | DY>(r));
}
template <uint32_t OP, uint32_t CM, uint32_t DY> static RAX_AI uint32_t npm_r(RaxN &r)
{
	enum { I = 4 + ((OP >> 2) & 3), M = 4 + (OP & 3) };
	const uint32_t p = s_pram[ndag<I, M, CM | DY>(r) & 0x3fff];
	r.px = (uint8_t)p;
	return p >> 8;
}

// ---- what a word uses (compile time) ----
static constexpr uint32_t nx_t(uint32_t op) { return op >> 16; }
static constexpr bool nx_imm1(uint32_t op) { return nx_t(op) >= 0xa0 && nx_t(op) <= 0xaf; } // DM(I, M) = 16-bit immediate
static constexpr bool nx_imm2(uint32_t op) { return nx_t(op) >= 0xb0 && nx_t(op) <= 0xbf; }
static constexpr bool nx_dm1(uint32_t op)
{
	return (nx_t(op) >= 0x60 && nx_t(op) <= 0x6f) || nx_t(op) == 0x12 || nx_imm1(op) || nx_t(op) >= 0xc0;
}
static constexpr bool nx_dm2(uint32_t op) { return (nx_t(op) >= 0x70 && nx_t(op) <= 0x7f) || nx_t(op) == 0x13 || nx_imm2(op); }
static constexpr bool nx_pm(uint32_t op) { return nx_t(op) >= 0x50 && nx_t(op) <= 0x5f; }
static constexpr bool nx_pmw(uint32_t op) { return nx_pm(op) && (nx_t(op) & 8); }
static constexpr bool nx_jmp(uint32_t op) { return nx_t(op) >= 0x18 && nx_t(op) <= 0x1b; }
// M registers a word writes (0D register moves to M0-M3 / M4-M7)
static constexpr uint32_t nx_mw(uint32_t op)
{
	return nx_t(op) != 0x0d || ((op >> 4) & 15) < 4 || ((op >> 4) & 15) > 7 ? 0 :
		((op >> 8) & 15) == 4 ? 1u << (((op >> 4) & 15) - 4) : ((op >> 8) & 15) == 8 ? 1u << ((op >> 4) & 15) : 0;
}
// I registers a word steps, and those it steps with one of the M registers in mw
static constexpr uint32_t nx_iu(uint32_t op)
{
	return (nx_dm1(op) ? 1u << ((op >> 2) & 3) : 0) | (nx_dm2(op) || nx_pm(op) ? 1u << (4 + ((op >> 2) & 3)) : 0) |
		(nx_t(op) >= 0xc0 ? 1u << (4 + ((op >> 6) & 3)) : 0);
}
static constexpr uint32_t nx_idyn(uint32_t op, uint32_t mw)
{
	return (nx_dm1(op) && (mw >> (op & 3) & 1) ? 1u << ((op >> 2) & 3) : 0) |
		((nx_dm2(op) || nx_pm(op)) && (mw >> (4 + (op & 3)) & 1) ? 1u << (4 + ((op >> 2) & 3)) : 0) |
		(nx_t(op) >= 0xc0 && (mw >> (4 + ((op >> 4) & 3)) & 1) ? 1u << (4 + ((op >> 6) & 3)) : 0);
}
static constexpr uint32_t nf_mw() { return 0; }
template <class... T> static constexpr uint32_t nf_mw(uint32_t a, T... r) { return nx_mw(a) | nf_mw(r...); }
static constexpr uint32_t nf_iu() { return 0; }
template <class... T> static constexpr uint32_t nf_iu(uint32_t a, T... r) { return nx_iu(a) | nf_iu(r...); }
static constexpr bool nf_jmp() { return false; }
template <class... T> static constexpr bool nf_jmp(uint32_t a, T... r) { return nx_jmp(a) || nf_jmp(r...); }
static constexpr uint32_t nf_idyn(uint32_t) { return 0; }
template <class... T> static constexpr uint32_t nf_idyn(uint32_t mw, uint32_t a, T... r) { return nx_idyn(a, mw) | nf_idyn(mw, r...); }
// the dynamic I registers of a body: all it steps when it jumps (a skipped step changes the run's ranges)
template <uint32_t... OPS> static constexpr uint32_t nf_dyn()
{
	return nf_jmp(OPS...) ? nf_iu(OPS...) : nf_idyn(nf_mw(OPS...), OPS...);
}

// supported words (anything else is a compile error)
static constexpr bool nx_ok(uint32_t op)
{
	const uint32_t t = op >> 16;
	return t == 0x05 ? (op & 0xffff) == 0 :
		(t >= 0x20 && t <= 0x27) ? (op & 15) == 15 && !((t == 0x20 || t == 0x21 || t == 0x24 || t == 0x25) && (op & 0x0018f0) == 0x10) &&
			!((t == 0x22 || t == 0x23 || t == 0x26 || t == 0x27) && ((op >> 4) & 1) && ((op >> 13) & 15) == 15) :
		(t == 0x28 || t == 0x29) ? true :
		t == 0x0d ? ((op >> 8) & 15) == 0 || nx_mw(op) != 0 :
		(t == 0x10 || t == 0x12 || t == 0x13) ? ((op >> 11) & 15) <= 7 :
		nx_jmp(op) ? (op & 15) != 14 :
		(t >= 0x50 && t <= 0x7f) ? !((t & 2) && ((op >> 13) & 15) == 15) : // no ABS (S flag)
		nx_imm1(op) || nx_imm2(op) ? true :
		t >= 0xc0 ? !(((op >> 17) & 1) && ((op >> 13) & 15) == 15) : false;
}

// the addresses a word with dynamic I registers will use are fine (checked before it does anything)
template <uint32_t OP, uint32_t DY, uint32_t TOP, uint32_t LEN> static RAX_AI int nx_dyok(const RaxN &r)
{
	int ok = 1;
	if (nx_dm1(OP) && (DY >> ((OP >> 2) & 3) & 1))
		ok &= r.i[(OP >> 2) & 3] < 0x3fe0;
	if (nx_dm2(OP) && (DY >> (4 + ((OP >> 2) & 3)) & 1))
		ok &= r.i[4 + ((OP >> 2) & 3)] < 0x3fe0;
	if (nx_pmw(OP) && (DY >> (4 + ((OP >> 2) & 3)) & 1))
		ok &= r.i[4 + ((OP >> 2) & 3)] - TOP >= LEN;
	return ok;
}

// one instruction at TOP + POS (exec order: data writes before the function, reads after it)
template <uint32_t CM, uint32_t DY, uint32_t TOP, uint32_t LEN, uint32_t POS, uint32_t OP> static RAX_AI void nx(RaxN &r)
{
	static_assert(nx_ok(OP), "RAX native loop: unsupported instruction");
	enum { T = OP >> 16, D = (OP >> 4) & 15 };
	if (T == 0x05) {
		if (r.mv) {
			if ((uint16_t)(r.mr >> 32) & 0x80)
				r.mr = (r.mr & ~0xffffffffffffull) | (0xffffull << 32) | (0x8000ull << 16);
			else
				r.mr = (r.mr & ~0xffffffffffffull) | (0x7fffull << 16) | 0xffffull;
		}
	} else if (T == 0x0d) {
		const int32_t v = nrd0<OP & 15>(r);
		if (((OP >> 8) & 15) == 0)
			nwr0<D>(r, v);
		else
			r.m[((OP >> 8) & 15) == 4 ? D - 4 : D] = (int32_t)((uint32_t)v << 18) >> 18; // write_reg1 / 2: M = sext(14)
	} else if (T == 0x10) {
		nshift<OP>(r);
		nwr0<D>(r, nrd0<OP & 15>(r));
	} else if (T == 0x12 || T == 0x13) {
		if ((OP >> 15) & 1) {
			const int32_t v = nrd0<D>(r);
			(T == 0x12 ? ndm1<OP, CM, DY>(r) : ndm2<OP, CM, DY>(r)) = (uint16_t)v;
			nshift<OP>(r);
		} else {
			nshift<OP>(r);
			nwr0<D>(r, T == 0x12 ? ndm1<OP, CM, DY>(r) : ndm2<OP, CM, DY>(r));
		}
	} else if (T >= 0x18 && T <= 0x1b) {
		enum { TGT = (OP >> 4) & 0x3fff };
		static_assert(!(TGT >= TOP && TGT <= TOP + POS), "RAX native loop: backward jump in the body");
		if (ncond<OP & 15>(r)) {
			if (TGT > TOP + POS && TGT < TOP + LEN)
				r.skip = (int)(TGT - (TOP + POS) - 1); // forward in the body
			else {
				r.skip = -1; // out of the loop (the loop stack stays as it is)
				r.xpc = (int)TGT;
			}
		}
	} else if (T >= 0x20 && T <= 0x27) {
		namf<OP, (T >> 1) & 3, (T & 2) && ((OP >> 4) & 1)>(r); // MAC MR, ALU AR, MAC MF, ALU AF (condition TRUE)
	} else if (T == 0x28 || T == 0x29) {
		const int32_t v = nrd0<OP & 15>(r);
		nmac_mr<OP>(r);
		nwr0<D>(r, v);
	} else if (nx_imm1(OP)) {
		ndm1<OP, CM, DY>(r) = (uint16_t)(OP >> 4);
	} else if (nx_imm2(OP)) {
		ndm2<OP, CM, DY>(r) = (uint16_t)(OP >> 4);
	} else if (T >= 0x50 && T <= 0x7f) {
		if (T & 8) { // write
			const int32_t v = nrd0<D>(r);
			if (T < 0x60) {
				enum { I = 4 + ((OP >> 2) & 3), M = 4 + (OP & 3) };
				s_cpu.program_write(ndag<I, M, CM | DY>(r), ((uint32_t)v << 8) | r.px);
			} else if (T < 0x70)
				ndm1<OP, CM, DY>(r) = (uint16_t)v;
			else
				ndm2<OP, CM, DY>(r) = (uint16_t)v;
			namf<OP, (T >> 1) & 3>(r);
		} else {
			namf<OP, (T >> 1) & 3>(r);
			const uint32_t v = T < 0x60 ? npm_r<OP, CM, DY>(r) : T < 0x70 ? ndm1<OP, CM, DY>(r) : ndm2<OP, CM, DY>(r);
			nwr0<D>(r, (int32_t)v);
		}
	} else if (T >= 0xc0) {
		if ((OP >> 17) & 1)
			nalu<OP, 1, 0>(r);
		else
			nmac_mr<OP>(r);
		const uint16_t d = ndm1<OP, CM, DY>(r);
		switch ((OP >> 18) & 3) {
		case 0: r.ax0 = d; break;
		case 1: r.ax1 = d; break;
		case 2: r.mx0 = d; break;
		case 3: r.mx1 = d; break;
		}
		const uint16_t p = (uint16_t)npm_r<(OP >> 4), CM, DY>(r);
		switch ((OP >> 20) & 3) {
		case 0: r.ay0 = p; break;
		case 1: r.ay1 = p; break;
		case 2: r.my0 = p; break;
		case 3: r.my1 = p; break;
		}
	}
}

// a body with jumps or dynamic I registers (CF) skips the words a jump passes over and stops at a jump out of the loop
// or before a word whose address is not fine (the usual paths go on from there, mid-loop)
template <uint32_t CM, uint32_t DY, uint32_t TOP, uint32_t LEN, int CF, uint32_t POS, uint32_t OP> static RAX_AI void nstep(RaxN &r)
{
	if (CF) {
		if (r.skip) {
			if (r.skip > 0)
				r.skip--;
			return;
		}
		if (!nx_dyok<OP, DY, TOP, LEN>(r)) {
			r.skip = -1;
			r.xpc = (int)(TOP + POS);
			return;
		}
	}
	nx<CM, DY, TOP, LEN, POS, OP>(r);
	if (CF) {
		r.ran++;
		r.last = TOP + POS;
	}
}
template <uint32_t CM, uint32_t DY, uint32_t TOP, uint32_t LEN, int CF, uint32_t POS> static RAX_AI void nseq(RaxN &) {}
template <uint32_t CM, uint32_t DY, uint32_t TOP, uint32_t LEN, int CF, uint32_t POS, uint32_t OP, uint32_t... REST>
static RAX_AI void nseq(RaxN &r)
{
	nstep<CM, DY, TOP, LEN, CF, POS, OP>(r);
	nseq<CM, DY, TOP, LEN, CF, POS + 1, REST...>(r);
}

// ---- the registers a body uses (compile time), so a run loads and stores only those ----
// bits 0-14: AX0 AX1 MX0 MX1 AY0 AY1 MY0 MY1 SI SE AR AF MF SR0 SR1, 15 MR, 16 flags, 17 PX, 18 the zero register,
// 32-39 I0-I7 (with base and L), 40-47 M0-M7
enum : uint64_t { NU_MR = 1ull << 15, NU_FL = 1ull << 16, NU_PX = 1ull << 17, NU_ZERO = 1ull << 18 };
static constexpr uint64_t nu_r0(uint32_t x) { return x < 11 ? 1ull << x : x < 14 ? NU_MR : x == 14 ? 1ull << 13 : 1ull << 14; }
static constexpr uint64_t nu_x(uint32_t x, uint32_t r0, uint32_t r1) // X operand: r0 r1 AR MR0 MR1 MR2 SR0 SR1
{
	return x == 0 ? 1ull << r0 : x == 1 ? 1ull << r1 : x == 2 ? 1ull << 10 : x <= 5 ? NU_MR : x == 6 ? 1ull << 13 : 1ull << 14;
}
static constexpr uint64_t nu_y(uint32_t y, uint32_t r0, uint32_t r1, uint32_t r2) // Y operand: r0 r1 r2 0
{
	return y == 0 ? 1ull << r0 : y == 1 ? 1ull << r1 : y == 2 ? 1ull << r2 : NU_ZERO;
}
static constexpr uint64_t nu_amf(uint32_t op, uint32_t kind, bool cnst)
{
	return ((op >> 13) & 15) == 0 && !(kind & 1) ? 0 : // MAC no-op
		(kind & 1) ? nu_x((op >> 8) & 7, 0, 1) | (cnst ? 0 : nu_y((op >> 11) & 3, 4, 5, 11)) | NU_FL | (kind == 1 ? 1ull << 10 : 1ull << 11) :
		nu_x((op >> 8) & 7, 2, 3) | nu_y((op >> 11) & 3, 6, 7, 12) | NU_MR | (kind == 0 ? NU_FL : 1ull << 12);
}
static constexpr uint64_t nu_shift(uint32_t op)
{
	return (((op >> 8) & 7) <= 1 ? 1ull << 8 : nu_x((op >> 8) & 7, 8, 8)) | 1ull << 9 | 1ull << 13 | 1ull << 14;
}
static constexpr uint64_t nu_dag(uint32_t i, uint32_t m) { return 1ull << (32 + i) | 1ull << (40 + m); }
static constexpr uint64_t nx_use(uint32_t op)
{
	return (nx_dm1(op) ? nu_dag((op >> 2) & 3, op & 3) : 0) | (nx_dm2(op) || nx_pm(op) ? nu_dag(4 + ((op >> 2) & 3), 4 + (op & 3)) : 0) |
		(nx_pm(op) ? NU_PX : 0) |
		(nx_t(op) == 0x05 ? NU_MR | NU_FL :
		nx_t(op) == 0x0d ? nu_r0(op & 15) | (((op >> 8) & 15) == 0 ? nu_r0((op >> 4) & 15) :
			1ull << (40 + (((op >> 8) & 15) == 4 ? ((op >> 4) & 15) - 4 : (op >> 4) & 15))) :
		nx_t(op) == 0x10 ? nu_shift(op) | nu_r0(op & 15) | nu_r0((op >> 4) & 15) :
		nx_t(op) == 0x12 || nx_t(op) == 0x13 ? nu_shift(op) | nu_r0((op >> 4) & 15) :
		nx_jmp(op) ? NU_FL :
		nx_t(op) >= 0x20 && nx_t(op) <= 0x27 ? nu_amf(op, (nx_t(op) >> 1) & 3, (nx_t(op) & 2) && ((op >> 4) & 1)) :
		nx_t(op) == 0x28 || nx_t(op) == 0x29 ? nu_r0(op & 15) | nu_r0((op >> 4) & 15) | nu_amf(op, 0, false) :
		nx_t(op) >= 0x50 && nx_t(op) <= 0x7f ? nu_r0((op >> 4) & 15) | nu_amf(op, (nx_t(op) >> 1) & 3, false) :
		nx_t(op) >= 0xc0 ? nu_amf(op, ((op >> 17) & 1) ? 1 : 0, false) | 1ull << (((op >> 18) & 3) == 0 ? 0 : ((op >> 18) & 3) == 1 ? 1 :
			((op >> 18) & 3) == 2 ? 2 : 3) | 1ull << (((op >> 20) & 3) == 0 ? 4 : ((op >> 20) & 3) == 1 ? 5 : ((op >> 20) & 3) == 2 ? 6 : 7) |
			nu_dag(4 + ((op >> 6) & 3), 4 + ((op >> 4) & 3)) | NU_PX : 0);
}
static constexpr uint64_t nf_use() { return 0; }
template <class... T> static constexpr uint64_t nf_use(uint32_t a, T... r) { return nx_use(a) | nf_use(r...); }

template <uint64_t U> static RAX_AI void nload(RaxN &r, const adsp21xx_device *c)
{
	const auto &k = c->m_core;
	if (U & 1ull << 0) r.ax0 = k.ax0.u;
	if (U & 1ull << 1) r.ax1 = k.ax1.u;
	if (U & 1ull << 2) r.mx0 = k.mx0.u;
	if (U & 1ull << 3) r.mx1 = k.mx1.u;
	if (U & 1ull << 4) r.ay0 = k.ay0.u;
	if (U & 1ull << 5) r.ay1 = k.ay1.u;
	if (U & 1ull << 6) r.my0 = k.my0.u;
	if (U & 1ull << 7) r.my1 = k.my1.u;
	if (U & 1ull << 8) r.si = k.si.u;
	if (U & 1ull << 9) r.se = k.se.u;
	if (U & 1ull << 10) r.ar = k.ar.u;
	if (U & 1ull << 11) r.af = k.af.u;
	if (U & 1ull << 12) r.mf = k.mf.u;
	if (U & 1ull << 13) r.sr0 = k.sr.srx.sr0.u;
	if (U & 1ull << 14) r.sr1 = k.sr.srx.sr1.u;
	if (U & NU_MR) r.mr = k.mr.mr;
	if (U & NU_ZERO) r.zero = k.zero.u;
	for (int i = 0; i < 8; i++) {
		if (U >> (32 + i) & 1) { r.i[i] = c->m_i[i]; r.base[i] = c->m_base[i]; r.l[i] = c->m_l[i]; }
		if (U >> (40 + i) & 1) r.m[i] = c->m_m[i];
	}
	if (U & NU_FL) {
		const uint32_t as = c->m_astat;
		r.az = as & 1; r.an = (as >> 1) & 1; r.av = (as >> 2) & 1; r.ac = (as >> 3) & 1; r.as = (as >> 4) & 1; r.mv = (as >> 6) & 1;
	}
	const uint32_t ms = c->m_mstat;
	r.sticky = (ms & MSTAT_STICKYV) ? 1 : 0;
	r.sat = (ms & MSTAT_SATURATE) ? 1 : 0;
	r.shift = (ms & MSTAT_INTEGER) ? 0 : 1;
	if (U & NU_PX) r.px = c->m_px;
	r.db = s_dbank;
}
template <uint64_t U> static RAX_AI void nstore(const RaxN &r, adsp21xx_device *c)
{
	auto &k = c->m_core;
	if (U & 1ull << 0) k.ax0.u = r.ax0;
	if (U & 1ull << 1) k.ax1.u = r.ax1;
	if (U & 1ull << 2) k.mx0.u = r.mx0;
	if (U & 1ull << 3) k.mx1.u = r.mx1;
	if (U & 1ull << 4) k.ay0.u = r.ay0;
	if (U & 1ull << 5) k.ay1.u = r.ay1;
	if (U & 1ull << 6) k.my0.u = r.my0;
	if (U & 1ull << 7) k.my1.u = r.my1;
	if (U & 1ull << 8) k.si.u = r.si;
	if (U & 1ull << 9) k.se.u = r.se;
	if (U & 1ull << 10) k.ar.u = r.ar;
	if (U & 1ull << 11) k.af.u = r.af;
	if (U & 1ull << 12) k.mf.u = r.mf;
	if (U & 1ull << 13) k.sr.srx.sr0.u = r.sr0;
	if (U & 1ull << 14) k.sr.srx.sr1.u = r.sr1;
	if (U & NU_MR) k.mr.mr = r.mr;
	for (int i = 0; i < 8; i++) {
		if (U >> (32 + i) & 1) c->m_i[i] = r.i[i];
		if (U >> (40 + i) & 1) c->m_m[i] = r.m[i];
	}
	if (U & NU_FL)
		c->m_astat = (c->m_astat & ~0x4fu) | r.az | r.an << 1 | r.av << 2 | r.ac << 3 | r.mv << 6;
	if (U & NU_PX) c->m_px = r.px;
}

// what a run did: instructions, whole iterations, where it stopped (xpc -1: at the loop top after whole iterations)
struct RaxNRes { int ins, full, xpc; uint32_t last; };
// up to k iterations (the caller checked the static I registers' ranges and set mp)
template <uint32_t CM, uint32_t TOP, uint32_t... OPS> static RaxNRes nl_run(adsp21xx_device *c, uint16_t *const *mp, int k)
{
	enum : uint32_t { LEN = sizeof...(OPS), DY = nf_dyn<OPS...>() };
	enum { CF = DY != 0 || nf_jmp(OPS...) };
	static constexpr uint64_t U = nf_use(OPS...);
	RaxN r;
	nload<U>(r, c);
	for (int i = 0; i < 8; i++) r.mp[i] = mp[i];
	r.skip = 0;
	r.ran = 0;
	r.xpc = -1;
	r.last = TOP + LEN - 1;
	int n = 0;
	for (; n < k; n++) {
		nseq<CM & ~DY, DY, TOP, LEN, CF, 0, OPS...>(r);
		if (CF && r.skip) // stopped (skip -1)
			break;
	}
	nstore<U>(r, c);
	RaxNRes res;
	res.ins = CF ? r.ran : n * (int)LEN;
	res.full = n;
	res.xpc = r.xpc;
	res.last = r.last;
	return res;
}

// what a loop body does with each I register it uses (from its words, at start): the M registers of its steps in
// order, data memory or program writes through it
struct RaxNReg { uint8_t i, nu, dm, pmw, mu[9]; };
struct RaxNLoop
{
	uint16_t top;
	uint8_t len, cm[2]; // the I registers stepped with the wrap in the two compiled forms (the second one covers the first)
	uint32_t ops[9];
	RaxNRes (*run[2])(adsp21xx_device *, uint16_t *const *, int);
	RaxNReg reg[8];
	uint8_t nreg, dy; // dy: the dynamic I registers (no range check before the run)
};
#define RAX_NL(top, cm, cm2, ...) { top, sizeof((uint32_t[]){ __VA_ARGS__ }) / 4, { cm, cm2 }, { __VA_ARGS__ }, 	{ nl_run<cm, top, __VA_ARGS__>, nl_run<cm2, top, __VA_ARGS__> }, {}, 0, 0 }
static RaxNLoop s_nloops[] = {
	RAX_NL(0x316c, 0x00, 0x30, 0x60803c, 0x606940, 0x632458, 0x6a64a1, 0x6881a5, 0xf84844, 0x526c60, 0x6b2cad, 0x6800a9), // FFT
	RAX_NL(0x318a, 0x00, 0x30, 0x708039, 0x606940, 0x632458, 0x6a64a1, 0x6881a5, 0xf84844, 0x526c60, 0x6b2cad, 0x6800a9),
	RAX_NL(0x3e3a, 0x00, 0x0f, 0x627cc6, 0x25880f, 0x6800a7, 0x289142, 0x6264cd, 0x6996a1, 0x7884c1, 0x615729), // filter
	RAX_NL(0x3e4e, 0x00, 0x0f, 0x627cc6, 0x25880f, 0x6800a7, 0x289142, 0x6264cd, 0x6996a1, 0x7884c1, 0x615720),
	RAX_NL(0x3622, 0x00, 0xff, 0x58a0a1, 0xca6421),
	RAX_NL(0x3607, 0x00, 0xff, 0x50a040, 0xea6461, 0x5800a1),
	RAX_NL(0x3653, 0x00, 0xff, 0xe80061, 0x70a040, 0x22640f, 0x7800a1),
	RAX_NL(0x365c, 0x00, 0xff, 0x70a040, 0x626421, 0x7800a1),
	RAX_NL(0x30e4, 0x00, 0xff, 0xfc8058, 0x20690f, 0x6888c5, 0xe84114, 0x6800ca),
	RAX_NL(0x30fa, 0x00, 0xff, 0xe8001c, 0xfc8055, 0x604901, 0x6888cb, 0x68610e, 0x7800ca),
	RAX_NL(0x3124, 0x00, 0xff, 0x667b23, 0x22730f, 0x68a0a2),
	RAX_NL(0x3e66, 0x00, 0xff, 0x68b0a9, 0x6902ad, 0x050000, 0x627c21),
	RAX_NL(0x3e76, 0x00, 0xff, 0x68b0a9, 0x21020f, 0x050000, 0x627c21),
	RAX_NL(0x3e86, 0x00, 0xff, 0x6890a1, 0x21020f, 0x627c20),
	RAX_NL(0x30da, 0x00, 0xff, 0x600000, 0x60004c, 0x680042, 0x68000f),
	RAX_NL(0x3c91, 0x00, 0xff, 0x638609, 0x1bc940, 0x600009, 0x2278df, 0x1bca07, 0x0d047a, 0x12160b), // bit unpack (jumps out)
	RAX_NL(0x3daa, 0x00, 0xff, 0x608aa6, 0x6800c7),
	RAX_NL(0x00ad, 0x00, 0xff, 0x500001, 0x680005), // copies and fills
	RAX_NL(0x3a6a, 0x00, 0xff, 0xa00001),
	RAX_NL(0x3a7f, 0x00, 0xff, 0x6800a1),
	RAX_NL(0x35a8, 0x00, 0xff, 0x580081),
	RAX_NL(0x3166, 0x00, 0x00, 0x600000), // (a table entry for the ADR-182 routine at 3166; its loop form never matches)
};
#undef RAX_NL
enum { RAX_NLN = sizeof(s_nloops) / sizeof(s_nloops[0]) };
static uint8_t s_nl_at[0x4000];    // loop index + 1 at its top address
unsigned rstv_rax_nl_runs, rstv_rax_nl_insns, rstv_rax_nl_miss; // diag
static int s_nl_on = -1;
static void rax_nl_init()
{
	memset(s_nl_at, 0, sizeof(s_nl_at));
	for (unsigned k = 0; k < RAX_NLN; k++) {
		RaxNLoop &L = s_nloops[k];
		s_nl_at[L.top] = (uint8_t)(k + 1);
		L.nreg = 0;
		{ // the dynamic I registers, as nf_dyn
			uint32_t mw = 0, iu = 0, idyn = 0;
			bool jmp = false;
			for (unsigned j = 0; j < L.len; j++) { mw |= nx_mw(L.ops[j]); iu |= nx_iu(L.ops[j]); jmp = jmp || nx_jmp(L.ops[j]); }
			for (unsigned j = 0; j < L.len; j++) idyn |= nx_idyn(L.ops[j], mw);
			L.dy = (uint8_t)(jmp ? iu : idyn);
		}
		for (unsigned j = 0; j < L.len; j++) {
			const uint32_t op = L.ops[j], t = op >> 16;
			auto add = [&](int i, int m, int kind) { // kind 0: data memory, 1: program read, 2: program write
				int x = 0;
				while (x < L.nreg && L.reg[x].i != i) x++;
				if (x == L.nreg) { memset(&L.reg[x], 0, sizeof(L.reg[x])); L.reg[x].i = (uint8_t)i; L.nreg++; }
				RaxNReg &g = L.reg[x];
				g.mu[g.nu++] = (uint8_t)m;
				g.dm |= kind == 0;
				g.pmw |= kind == 2;
			};
			if (nx_pm(op)) add(4 + ((op >> 2) & 3), 4 + (op & 3), nx_pmw(op) ? 2 : 1);
			if (nx_dm1(op)) add((op >> 2) & 3, op & 3, 0);
			if (nx_dm2(op)) add(4 + ((op >> 2) & 3), 4 + (op & 3), 0);
			if (t >= 0xc0) add(4 + ((op >> 6) & 3), 4 + ((op >> 4) & 3), 1);
		}
	}
#if !defined(__vita__)
	s_nl_on = getenv("RSTV_RAXNL") ? atoi(getenv("RSTV_RAXNL")) : 1; // harness: RSTV_RAXNL=0 turns them off
#else
	s_nl_on = 1;
#endif
}

// the address ranges of k iterations: mp per I register; returns the I registers that wrap (circular steps), -1 if the
// run must not go natively. An I register with L != 0 that stays inside [base, base + L) for the whole run steps as a
// linear one (the wrap never fires).
static int rax_nl_ranges(const adsp21xx_device *c, const RaxNLoop &L, uint32_t top, int k, uint16_t **mp)
{
	if (c->m_mstat & MSTAT_REVERSE)
		return -1;
	int wraps = 0;
	for (int x = 0; x < L.nreg; x++) {
		const RaxNReg &g = L.reg[x];
		const int I = g.i;
		if (L.dy >> I & 1)
			continue; // checked at each use
		// unwrapped: addresses i0 + t * D + P(j) for iteration t and the j-th step, P(j) the M sum of the steps before it
		const int32_t i0 = (int32_t)c->m_i[I], l = (int32_t)c->m_l[I];
		int32_t p = 0, pmin = 0, pmax = 0, mabs = 0;
		for (int j = 0; j < g.nu; j++) {
			if (p < pmin) pmin = p;
			if (p > pmax) pmax = p;
			const int32_t m = c->m_m[g.mu[j]];
			p += m;
			mabs |= m < 0 ? -m : m;
		}
		const int64_t dk = (int64_t)p * (k - 1), fin = i0 + (int64_t)p * k; // fin: I after the last step
		int64_t lo = i0 + pmin + (dk < 0 ? dk : 0), hi = i0 + pmax + (dk > 0 ? dk : 0);
		if (fin < lo) lo = fin;
		if (fin > hi) hi = fin;
		if (l) {
			const int32_t b = (int32_t)c->m_base[I], e = b + l - 1;
			if (lo < b || hi > e) { // it wraps: one wrap a step keeps I in [base, base + L) when no |M| is above L
				if (mabs > l) // (an OR of the |M|: at most twice the largest one)
					return -1;
				lo = i0 < b ? i0 : b;
				hi = i0 > e ? i0 : e;
				wraps |= 1 << I;
			}
		}
		if (lo < 0 || hi > 0x3fff)
			return -1;
		if (g.dm) {
			if (hi < 0x2000)
				mp[I] = s_dbank;
			else if (lo >= 0x2000 && hi < 0x3fe0)
				mp[I] = s_iram - 0x2000;
			else
				return -1;
		}
		if (g.pmw && !(hi < (int64_t)top || lo > (int64_t)(top + L.len - 1)))
			return -1;
	}
	return wraps;
}

// ---- ADR-181: the bit unpack loop with its symbol path (3C91-3C97 and 3CA0-3CA8) as one native routine ----
// The loop at 3C91 ends at a tree leaf by a jump out to 3CA0; that path stores the symbol and jumps back to 3C91 while
// the counter lasts (IF NOT CE). Both counter steps (the loop end at 3C97 and 3CA8) are done here while CNTR >= 2; the
// last count (a pop), the refill at 3C98, the call at 3CA6 and the exit to 3CAB are left to the usual paths.
static const uint32_t s_nr_unpack_a[7] = { 0x638609, 0x1bc940, 0x600009, 0x2278df, 0x1bca07, 0x0d047a, 0x12160b }; // 3C91
static const uint32_t s_nr_unpack_b[9] = { 0x22f89f, 0x1bca34, 0x1bcabf, 0x22f81f, 0x1296a5, 0x27100f, 0x1fcd83, 0x87f022,
	0x1bc91e }; // 3CA0
static int rax_nr_unpack_ok(const adsp21xx_device *c)
{
	return c->m_loop == 0x3c97 && c->m_loop_condition == 14 && !(c->m_mstat & MSTAT_REVERSE) &&
		!memcmp(&s_pram[0x3c91], s_nr_unpack_a, sizeof(s_nr_unpack_a)) && !memcmp(&s_pram[0x3ca0], s_nr_unpack_b, sizeof(s_nr_unpack_b));
}
static RaxNRes rax_nr_unpack(adsp21xx_device *c)
{
	enum : uint32_t { T = 0x3c91, DY = 0xff }; // every I register checked at each use
	RaxN r;
	nload<~0ull>(r, c);
	int budget = c->m_icount, ins = 0, xpc = -1;
	uint32_t cntr = c->m_cntr, last = c->m_ppc;
// a word at a (stops before it when the slice is used up or its address is not fine); a jump word: counted only
#define NRW(a, op) do { if (budget <= 0 || !nx_dyok<op, DY, T, 24>(r)) { xpc = a; goto out; } \
	nx<0, DY, T, 24, (a) - T, op>(r); budget--; ins++; last = a; } while (0)
#define NRJ(a) do { if (budget <= 0) { xpc = a; goto out; } budget--; ins++; last = a; } while (0)
top:
	NRW(0x3c91, 0x638609);
	NRJ(0x3c92);
	if (r.az) goto w3c94; // IF EQ JUMP 3C94
	NRW(0x3c93, 0x600009);
w3c94:
	NRW(0x3c94, 0x2278df);
	NRJ(0x3c95);
	if (!r.av) goto w3ca0; // IF NOT AV JUMP 3CA0
	NRW(0x3c96, 0x0d047a);
	if (cntr < 2) { xpc = 0x3c97; goto out; } // loop end: the CE step at its fetch
	NRW(0x3c97, 0x12160b);
	cntr--;
	goto top;
w3ca0:
	NRW(0x3ca0, 0x22f89f);
	NRJ(0x3ca1);
	if (r.an ^ r.av) goto w3ca3; // IF LT JUMP 3CA3
	NRJ(0x3ca2); // JUMP 3CAB
	xpc = 0x3cab;
	goto out;
w3ca3:
	NRW(0x3ca3, 0x22f81f);
	NRW(0x3ca4, 0x1296a5);
	NRW(0x3ca5, 0x27100f);
	if ((r.an ^ r.av) | r.az) { xpc = 0x3ca6; goto out; } // IF LE CALL 3CD8: the call by the usual path
	NRJ(0x3ca6);
	if (budget <= 0) { xpc = 0x3ca7; goto out; }
	r.i[2] = s_iram[0x3f02 - 0x2000] & 0x3fff; // I2 = DM(3F02) (write_reg1: I and its base)
	r.base[2] = r.i[2] & c->m_lmask[2];
	budget--; ins++; last = 0x3ca7;
	if (cntr < 2) { xpc = 0x3ca8; goto out; } // IF NOT CE JUMP 3C91
	NRJ(0x3ca8);
	cntr--;
	goto top;
#undef NRW
#undef NRJ
out:
	nstore<~0ull>(r, c);
	c->m_base[2] = r.base[2];
	c->m_cntr = cntr;
	RaxNRes res;
	res.ins = ins;
	res.full = 0; // (the counter is set here)
	res.xpc = xpc;
	res.last = last;
	return res;
}

// ---- ADR-182: one FFT pass's group loop (DO 3178 at 3166, with its inner DO 3174) as one native routine ----
// The group loop runs the butterfly loop once per group with few iterations in the early passes, so most of its time
// was the 19 words around it. Here the CNTR push, the DO push and the pops at the inner loop end are done on the
// device's own stacks (its push / pop functions), so the run can stop before any word with the state the usual
// paths expect; the outer loop end is done here while CNTR >= 2.
static const uint32_t s_nr_fft[19] = { 0x3a1000, 0x3a1801, 0x8fd855, 0xe80004, 0x500074, 0x17174e, 0x60803c, 0x606940,
	0x632458, 0x6a64a1, 0x6881a5, 0xf84844, 0x526c60, 0x6b2cad, 0x6800a9, 0x090002, 0x090006, 0x09000a, 0x09000e }; // 3166
static int rax_nr_fft_ok(const adsp21xx_device *c)
{
	return c->m_loop == 0x3178 && c->m_loop_condition == 14 && !(c->m_mstat & MSTAT_REVERSE) &&
		!memcmp(&s_pram[0x3166], s_nr_fft, sizeof(s_nr_fft));
}
static RAX_AI void nr_modify(RaxN &r, int I, int M) // modify_address
{
	uint32_t i = (r.i[I] + r.m[M]) & 0x3fff;
	if (i < r.base[I]) i += r.l[I];
	else if (i >= r.base[I] + r.l[I]) i -= r.l[I];
	r.i[I] = i;
}
static RaxNRes rax_nr_fft(adsp21xx_device *c)
{
	enum : uint32_t { T = 0x3166, DY = 0xff };
	RaxN r;
	nload<~0ull>(r, c);
	int budget = c->m_icount, ins = 0, xpc = -1;
	uint32_t last = c->m_ppc;
#define NRW(a, op) do { if (budget <= 0 || !nx_dyok<op, DY, T, 19>(r)) { xpc = a; goto out; } \
	nx<0, DY, T, 19, (a) - T, op>(r); budget--; ins++; last = a; } while (0)
#define NRS(a) do { if (budget <= 0) { xpc = a; goto out; } } while (0)
#define NRD(a) do { budget--; ins++; last = a; } while (0)
group:
	NRS(0x3166); r.i[4] = 0x2100; r.base[4] = 0x2100 & c->m_lmask[4]; NRD(0x3166); // I4 = 2100
	NRS(0x3167); r.i[5] = 0x2180; r.base[5] = 0x2180 & c->m_lmask[5]; NRD(0x3167); // I5 = 2180
	NRS(0x3168); c->cntr_stack_push(); c->m_cntr = s_iram[0x3d85 - 0x2000] & 0x3fff; NRD(0x3168); // CNTR = DM(3D85)
	NRW(0x3169, 0xe80004);
	NRW(0x316a, 0x500074);
	NRS(0x316b); c->loop_stack_push(0x3174e); c->m_pc = 0x316c; c->pc_stack_push(); NRD(0x316b); // DO 3174 UNTIL CE
inner:
	NRW(0x316c, 0x60803c);
	NRW(0x316d, 0x606940);
	NRW(0x316e, 0x632458);
	NRW(0x316f, 0x6a64a1);
	NRW(0x3170, 0x6881a5);
	NRW(0x3171, 0xf84844);
	NRW(0x3172, 0x526c60);
	NRW(0x3173, 0x6b2cad);
	{ // loop end 3174: the CE step at its fetch, then the word
		if (budget <= 0 || !nx_dyok<0x6800a9, DY, T, 19>(r)) { xpc = 0x3174; goto out; }
		const int again = (int32_t)--c->m_cntr > 0;
		if (!again) {
			c->cntr_stack_pop();
			c->loop_stack_pop();
			c->pc_stack_pop_val();
		}
		nx<0, DY, T, 19, 0x3174 - T, 0x6800a9>(r);
		NRD(0x3174);
		if (again)
			goto inner;
	}
	NRS(0x3175); nr_modify(r, 0, 2); NRD(0x3175); // MODIFY (I0..I3, M2)
	NRS(0x3176); nr_modify(r, 1, 2); NRD(0x3176);
	NRS(0x3177); nr_modify(r, 2, 2); NRD(0x3177);
	NRS(0x3178);
	if ((int32_t)c->m_cntr < 2) { xpc = 0x3178; goto out; } // outer loop end: the last count pops (usual path)
	c->m_cntr--;
	nr_modify(r, 3, 2);
	NRD(0x3178);
	goto group;
#undef NRW
#undef NRS
#undef NRD
out:
	nstore<~0ull>(r, c);
	c->m_base[4] = r.base[4];
	c->m_base[5] = r.base[5];
	RaxNRes res;
	res.ins = ins;
	res.full = 0;
	res.xpc = xpc < 0 ? 0x3166 : xpc;
	res.last = last;
	return res;
}

#if !defined(__vita__)
static unsigned s_nlchk_n, s_nlchk_bad;
#endif
// at the top of a listed loop: the instructions run natively (0: none, the caller goes on as usual)
static RAX_AI int rax_nl_try(adsp21xx_device *c, uint32_t pc)
{
	const uint8_t li = s_nl_at[pc & 0x3fff];
	if (!li || s_nl_on <= 0)
		return 0;
	const RaxNLoop &L = s_nloops[li - 1];
	const int rt = pc == 0x3c91 ? rax_nr_unpack_ok(c) : pc == 0x3166 ? 2 * rax_nr_fft_ok(c) : 0; // ADR-181 / 182 routines
	int k = 0, form = 0, um = 0;
	uint16_t *mp[8] = { 0 };
	if (!rt) {
	if (c->m_loop != pc + L.len - 1u || c->m_loop_condition != 14 || (int32_t)c->m_cntr < 2 ||
		memcmp(&s_pram[pc], L.ops, L.len * 4))
		return 0;
	k = (int)c->m_cntr - 1;
	if (k > c->m_icount / L.len)
		k = c->m_icount / L.len;
	if (k <= 0)
		return 0;
	um = rax_nl_ranges(c, L, pc, k, mp); // the I registers that wrap in this run
	// a form whose circular set covers the wrapping ones (a circular step on an I that does not wrap is the linear one)
	form = um < 0 ? -1 : !(um & ~L.cm[0]) ? 0 : !(um & ~L.cm[1]) ? 1 : -1;
	}
	if (form < 0) {
		rstv_rax_nl_miss++;
#if !defined(__vita__)
		static uint8_t logged[RAX_NLN][257];
		const int lk = um + 1; // (0: the ranges do not allow it)
		if (getenv("RSTV_RAXLOG") && !logged[li - 1][lk]) {
			logged[li - 1][lk] = 1;
			fprintf(stderr, "raxnl miss %04x form %d um %02x k %d rev %d", pc, form, um, k, (c->m_mstat & MSTAT_REVERSE) != 0);
			for (int i = 0; i < 8; i++)
				fprintf(stderr, " i%d %04x/%x/%04x m %d", i, c->m_i[i], c->m_l[i], c->m_base[i], c->m_m[i]);
			fprintf(stderr, "\n");
		}
#endif
		return 0;
	}
#if !defined(__vita__)
	static int chk = -1;
	if (chk < 0) chk = getenv("RSTV_RAXNLCHK") ? 1 : 0;
	static uint8_t c0[sizeof(adsp21xx_device)], cn[sizeof(adsp21xx_device)];
	static uint16_t m0b[0x2000 * 5], m0i[0x2000], mnb[0x2000 * 5], mni[0x2000];
	static uint32_t p0[0x4000], pn[0x4000];
	if (chk) {
		memcpy(c0, c, sizeof(*c)); memcpy(m0b, s_banked, sizeof(s_banked)); memcpy(m0i, s_iram, sizeof(s_iram));
		memcpy(p0, s_pram, sizeof(s_pram));
	}
#endif
	const RaxNRes res = rt == 1 ? rax_nr_unpack(c) : rt == 2 ? rax_nr_fft(c) : L.run[form](c, mp, k);
	const int ins = res.ins;
	if (!ins)
		return 0;
	c->m_cntr -= (uint32_t)res.full;
	if (res.xpc >= 0)
		c->m_pc = (uint32_t)res.xpc; // a jump out of the loop, or the word the run stopped before
	c->m_ppc = res.last;
	rstv_rax_nl_runs++;
	rstv_rax_nl_insns += (unsigned)ins;
#if !defined(__vita__)
	{ // diag (RSTV_RAXLOG): runs and instructions of each loop, every 2^18 runs
		static unsigned long long lr[RAX_NLN], lins[RAX_NLN];
		lr[li - 1]++;
		lins[li - 1] += (unsigned)ins;
		if ((rstv_rax_nl_runs & 0x3ffff) == 0 && getenv("RSTV_RAXLOG"))
			for (unsigned x = 0; x < RAX_NLN; x++)
				fprintf(stderr, "raxnl %04x runs %llu ins %llu (%.1f a run)\n", s_nloops[x].top, lr[x], lins[x],
					lr[x] ? (double)lins[x] / lr[x] : 0.0);
	}
	if (chk) {
		memcpy(cn, c, sizeof(*c)); memcpy(mnb, s_banked, sizeof(s_banked)); memcpy(mni, s_iram, sizeof(s_iram));
		memcpy(pn, s_pram, sizeof(s_pram));
		memcpy(c, c0, sizeof(*c)); memcpy(s_banked, m0b, sizeof(s_banked)); memcpy(s_iram, m0i, sizeof(s_iram));
		memcpy(s_pram, p0, sizeof(s_pram));
		for (int i = 0; i < ins; i++)
			c->step1();
		const adsp21xx_device *N = (const adsp21xx_device *)cn;
		c->m_icount = N->m_icount; // (step1 counts like the caller does below)
		const size_t core = offsetof(adsp21xx_device, m_read0_ptr);
		s_nlchk_n++;
		const int bad = memcmp(cn, c, core) || memcmp(mnb, s_banked, sizeof(s_banked)) || memcmp(mni, s_iram, sizeof(s_iram)) ||
			memcmp(pn, s_pram, sizeof(s_pram));
		if (bad && s_nlchk_bad++ < 20) {
			fprintf(stderr, "raxnlchk BAD loop %04x x%d ins %d:", pc, res.full, ins);
			for (size_t o = 0; o < core; o++)
				if (cn[o] != ((uint8_t *)c)[o]) fprintf(stderr, " c+%u %02x/%02x", (unsigned)o, cn[o], ((uint8_t *)c)[o]);
			for (int a = 0; a < 0x2000 * 5; a++) if (mnb[a] != s_banked[a]) { fprintf(stderr, " bank %04x %04x/%04x", a, mnb[a], s_banked[a]); break; }
			for (int a = 0; a < 0x2000; a++) if (mni[a] != s_iram[a]) { fprintf(stderr, " iram %04x %04x/%04x", a + 0x2000, mni[a], s_iram[a]); break; }
			for (int a = 0; a < 0x4000; a++) if (pn[a] != s_pram[a]) { fprintf(stderr, " pram %04x %06x/%06x", a, pn[a], s_pram[a]); break; }
			fprintf(stderr, "\n");
		}
		if ((s_nlchk_n & 0x3fff) == 0)
			fprintf(stderr, "raxnlchk %u runs, %u bad, %u missed\n", s_nlchk_n, s_nlchk_bad, rstv_rax_nl_miss);
		// go on from the interpreter's state
	}
#endif
	return ins;
}
