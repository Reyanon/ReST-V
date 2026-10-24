/* ReST-V (ADR-173): Acclaim RAX sound board (batmanfr): ADSP-2181 at 16.67 MHz, SPORT0 autobuffer to a stereo DAC.
   CPU core: MAME 0.264 src/devices/cpu/adsp2100 (BSD-3-Clause, copyright-holders: Aaron Giles). 2100ops.hxx is
   included unchanged; the class below carries the members it uses, and the execute loop, IRQ and table code are
   copied from adsp2100.cpp (ADSP-2181 paths only). Board: MAME src/mame/shared/rax.cpp (BSD-3-Clause,
   copyright-holders: Phil Bennett), with MAME timers replaced by DSP cycle counts.
   The SH-2 only writes to the board (MAME maps no read), so the words go through a queue: the emu thread adds them
   with a marker at each frame end, and the sound thread hands one frame's words to the DSP when it runs that frame
   (deterministic whatever the thread timing). */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include <array>
#include <utility>
extern "C" {
#include "rstv_rax.h"
}
#include "rstv_prof.h"

#define LSB_FIRST 1

namespace util {
static inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }
}
template <typename T> static inline T BIT(T x, int n) { return (x >> n) & T(1); }
template <typename T> static inline T BIT(T x, int n, int w) { return (x >> n) & ((T(1) << w) - 1); }
enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
static inline uint8_t count_leading_zeros_32(uint32_t v) { return v ? (uint8_t)__builtin_clz(v) : 32; }
static inline uint8_t count_leading_ones_32(uint32_t v) { return count_leading_zeros_32(~v); }

// ADSP-2181 IRQs (adsp2100.h); the 2100/2101 names are only used on paths the 2181 never takes
const int ADSP2181_IRQ0 = 0, ADSP2181_SPORT1_RX = 0, ADSP2181_IRQ1 = 1, ADSP2181_SPORT1_TX = 1, ADSP2181_IRQ2 = 2,
	ADSP2181_SPORT0_RX = 3, ADSP2181_SPORT0_TX = 4, ADSP2181_TIMER = 5, ADSP2181_IRQE = 6, ADSP2181_BDMA = 7,
	ADSP2181_IRQL1 = 8, ADSP2181_IRQL0 = 9;
const int ADSP2101_IRQ0 = 0, ADSP2101_IRQ1 = 1, ADSP2101_IRQ2 = 2, ADSP2101_SPORT0_RX = 3, ADSP2101_SPORT0_TX = 4;

// ADR-173: the helpers are forced inline so the handlers below fold the instruction's kind and ALU / MAC function
#define RAX_AI inline __attribute__((always_inline))

class adsp21xx_device
{
public:
	enum { CHIP_TYPE_ADSP2100, CHIP_TYPE_ADSP2101, CHIP_TYPE_ADSP2104, CHIP_TYPE_ADSP2105, CHIP_TYPE_ADSP2115, CHIP_TYPE_ADSP2181 };

	void init();
	void setup_pointers();
	void reset();
	int run(int cycles);          // returns the cycles used (an aborted slice only counts what ran, as MAME)
	void step1();                 // one instruction the interpreter's way (ADR-177 check)
	void loop_end_step(uint32_t a); // at the fetch of the loop end a: m_pc = loop top, or pop and a + 1
	RAX_AI void exec(uint32_t op); // one instruction (the PC already moved on)
	void set_input(int line, int state);

	void create_tables();
	inline void update_mstat();
	inline uint32_t pc_stack_top();
	inline void set_pc_stack_top(uint32_t top);
	inline void pc_stack_push();
	inline void pc_stack_push_val(uint32_t val);
	inline void pc_stack_pop();
	inline uint32_t pc_stack_pop_val();
	inline uint32_t cntr_stack_top();
	inline void cntr_stack_push();
	inline void cntr_stack_pop();
	inline uint32_t loop_stack_top();
	inline void loop_stack_push(uint32_t value);
	inline void loop_stack_pop();
	inline void stat_stack_push();
	inline void stat_stack_pop();
	int slow_condition();
	inline void modify_address(uint32_t ireg, uint32_t mreg);
	inline void data_write_dag1(uint32_t op, int32_t val);
	inline uint32_t data_read_dag1(uint32_t op);
	inline void data_write_dag2(uint32_t op, int32_t val);
	inline uint32_t data_read_dag2(uint32_t op);
	inline void pgm_write_dag2(uint32_t op, int32_t val);
	inline uint32_t pgm_read_dag2(uint32_t op);
	RAX_AI void alu_op_ar(uint32_t op);
	RAX_AI void alu_op_ar_const(uint32_t op);
	RAX_AI void alu_op_af(uint32_t op);
	RAX_AI void alu_op_af_const(uint32_t op);
	RAX_AI void alu_op_none(uint32_t op);
	RAX_AI void mac_op_mr(uint32_t op);
	RAX_AI void mac_op_mr_xop(uint32_t op);
	RAX_AI void mac_op_mf(uint32_t op);
	RAX_AI void mac_op_mf_xop(uint32_t op);
	RAX_AI void shift_op(uint32_t op);
	RAX_AI void shift_op_imm(uint32_t op);

	inline uint16_t data_read(uint32_t addr);
	inline void data_write(uint32_t addr, uint16_t data);
	inline uint16_t io_read(uint32_t addr);
	inline void io_write(uint32_t addr, uint16_t data);
	inline uint32_t program_read(uint32_t addr);
	inline void program_write(uint32_t addr, uint32_t data);
	inline uint32_t opcode_read();

	inline void update_i(int which);
	inline void update_l(int which);
	inline void update_dmovlay();
	inline void write_reg0(int regnum, int32_t val);
	inline void write_reg1(int regnum, int32_t val);
	inline void write_reg2(int regnum, int32_t val);
	inline void write_reg3(int regnum, int32_t val);
	inline int32_t read_reg0(int regnum);
	inline int32_t read_reg1(int regnum);
	inline int32_t read_reg2(int regnum);
	inline int32_t read_reg3(int regnum);

	bool generate_irq(int which, int indx);
	bool irq_pending() const;     // check_irqs would take one (no side effects)
	void check_irqs();

	// board callbacks (rax.cpp)
	void m_sport_tx_cb(int which, uint32_t data, uint32_t mask);
	int32_t m_sport_rx_cb(int which) { return 0; }
	void m_timer_fired_cb(int state) {}
	void m_dmovlay_cb(uint32_t data);
	template <typename... A> static void logerror(const char *, A...) {}

	static const int PC_STACK_DEPTH = 16;
	static const int CNTR_STACK_DEPTH = 4;
	static const int STAT_STACK_DEPTH = 4;
	static const int LOOP_STACK_DEPTH = 4;

	union adsp_reg16 { uint16_t u; int16_t s; };
	union adsp_shift { struct { adsp_reg16 sr0, sr1; } srx; uint32_t sr; };
	union adsp_mac { struct { adsp_reg16 mr0, mr1, mr2, mrzero; } mrx; struct { uint32_t mr0, mr1; } mry; uint64_t mr; };
	struct adsp_core
	{
		adsp_reg16 ax0, ax1, ay0, ay1, ar, af;
		adsp_reg16 mx0, mx1, my0, my1;
		adsp_mac mr;
		adsp_reg16 mf;
		adsp_reg16 si, se, sb;
		adsp_shift sr;
		adsp_reg16 zero;
	};

	static const uint32_t m_chip_type = CHIP_TYPE_ADSP2181;

	uint32_t m_pc, m_ppc, m_loop, m_loop_condition, m_cntr;
	uint32_t m_astat, m_sstat, m_mstat, m_mstat_prev, m_astat_clear, m_idle;
	adsp_core m_core;
	uint32_t m_i[8];
	int32_t m_m[8];
	uint32_t m_l[8], m_lmask[8], m_base[8];
	uint8_t m_px;
	uint32_t m_pmovlay, m_dmovlay;
	uint32_t m_loop_stack[LOOP_STACK_DEPTH], m_cntr_stack[CNTR_STACK_DEPTH], m_pc_stack[PC_STACK_DEPTH];
	uint16_t m_stat_stack[STAT_STACK_DEPTH][3];
	int32_t m_pc_sp, m_cntr_sp, m_stat_sp, m_loop_sp;
	uint8_t m_flagout, m_flagin, m_fl0, m_fl1, m_fl2;
	uint16_t m_imask;
	uint8_t m_icntl;
	uint16_t m_ifc;
	uint8_t m_irq_state[10], m_irq_latch[10];
	int m_icount;
	int m_abort;                  // the slice was cut (abort_timeslice): only executed cycles count
	unsigned m_insns;
	int m_jn;                     // instructions run by the last compiled block (ADR-175)
	uint32_t m_skip_pc;           // ADR-173 idle loops: last loop head seen in this slice
	int m_skip_n;                 // and the instruction count there
	int m_mstat_mask, m_imask_mask;
	int16_t *m_read0_ptr[16];
	uint32_t *m_read1_ptr[16], *m_read2_ptr[16];
	void *m_alu_xregs[8], *m_alu_yregs[4], *m_mac_xregs[8], *m_mac_yregs[4], *m_shift_xregs[8];
	adsp_core m_alt;
	uint8_t m_condition_table[0x1000];
	uint16_t m_mask_table[0x4000];
	uint16_t m_reverse_table[0x4000];

	static const int SSFLAG = 0x80, MVFLAG = 0x40, QFLAG = 0x20, SFLAG = 0x10, CFLAG = 0x08, VFLAG = 0x04, NFLAG = 0x02, ZFLAG = 0x01;
};

#include "adsp2100/2100ops.hxx"

/* ---- board state (rax.cpp) ---- */
enum
{
	IDMA_CONTROL_REG = 0, BDMA_INT_ADDR_REG, BDMA_EXT_ADDR_REG, BDMA_CONTROL_REG, BDMA_WORD_COUNT_REG,
	PROG_FLAG_DATA_REG, PROG_FLAG_CONTROL_REG,
	S1_AUTOBUF_REG = 15, S1_RFSDIV_REG, S1_SCLKDIV_REG, S1_CONTROL_REG, S0_AUTOBUF_REG, S0_RFSDIV_REG, S0_SCLKDIV_REG,
	S0_CONTROL_REG, S0_MCTXLO_REG, S0_MCTXHI_REG, S0_MCRXLO_REG, S0_MCRXHI_REG, TIMER_SCALE_REG, TIMER_COUNT_REG,
	TIMER_PERIOD_REG, WAITSTATES_REG, SYSCONTROL_REG
};

#define RAX_CLOCK 16670000u
#define RAX_SLICE 2048 /* longest DSP run between checks: an IDLE ends a slice like a MAME timeslice (ADR-177: 256 -> 2048, fewer
                         * block entries cut short at a slice end) */

static adsp21xx_device s_cpu;
#if !defined(__vita__)
static unsigned *s_hist; // diag (harness): RSTV_RAXHIST=1, DSP PC histogram printed every 600 frames
#endif
static uint32_t s_pram[0x4000];
// ADR-173: the two wait loops of the batmanfr sound driver (009A / 00A1, both call the host flag check at 2167)
static const uint32_t s_loop9a[] = { 0x430001, 0x22715f, 0x0d0243, 0x2ae2aa, 0x180a84, 0x1e167f, 0x1809af };
static const uint32_t s_loopa1[] = { 0x430001, 0x22715f, 0x0d0243, 0x2ae2aa, 0x180a82, 0x1e167f, 0x180a1f };
static const uint32_t s_flag2167[] = { 0x9b46e2, 0x9b46f6, 0x9b470a, 0x83fe5a, 0x23821f, 0x0a0000 };
static int rstv_rax_loop_ok(uint32_t pc)
{
	return !memcmp(&s_pram[pc], pc == 0x009a ? s_loop9a : s_loopa1, sizeof(s_loop9a)) &&
		!memcmp(&s_pram[0x2167], s_flag2167, sizeof(s_flag2167));
}
static uint16_t s_banked[0x2000 * 5];
static uint16_t s_iram[0x2000];
static uint16_t *s_dbank = s_banked;
static const uint8_t *s_rom;
static uint32_t s_rom_size;
static uint16_t s_control[32];
static uint32_t s_snd_pf0, s_data_bank, s_rom_bank, s_dmovlay_val;
static uint16_t s_size, s_incs, s_ireg_base;
static uint32_t s_ireg;
static int s_dac_on;
static uint32_t s_dac_rate;      // per channel, Hz
static uint64_t s_now;           // DSP cycles
static uint64_t s_t0;            // start of the running slice
static int s_len;                // its length
static int s_idle;               // the DSP waits in an idle loop until the next event (ADR-173)
unsigned rstv_rax_jit_gens, rstv_rax_jit_insns; // ADR-175 diag: block generations, instructions run in blocks
static inline uint64_t rax_time() { return s_t0 + (uint64_t)(s_len - s_cpu.m_icount); } // the running instruction
static uint64_t s_reg_next, s_reg_period; // autobuffer timer (0 period = off)
static uint64_t s_dma_next;      // BDMA timer (0 = off)
static uint32_t s_dma_param;
static uint16_t s_data_in, s_data_out;
int rstv_rax_active;
unsigned rstv_rax_insns;         // instructions run (profiling, performance line)
u64 rstv_rax_us;                 // time in rstv_rax_mix (performance line)
u64 rstv_rax_cpu_us;             // CPU time of the sound thread in rstv_rax_mix (Vita: wall time minus preemption)
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
static u64 rax_thread_cpu_us()
{
	SceKernelThreadInfo ti;
	memset(&ti, 0, sizeof(ti));
	ti.size = sizeof(ti);
	return sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) < 0 ? 0 : (u64)ti.runClocks;
}
#endif

/* SH-2 -> DSP words: 0x10000 marks a frame end */
#define RAX_QN 4096
static volatile uint32_t s_q[RAX_QN];
static volatile uint32_t s_qw, s_qr;
static uint16_t s_cmd[RAX_QN];
static unsigned s_cmd_r, s_cmd_w;

/* DAC output (stereo, DAC rate) -> resampled in rstv_rax_mix */
#define RAX_ON 8192
static int16_t s_out[RAX_ON][2];
static unsigned s_out_r, s_out_w;
static uint32_t s_out_frac;

static void cpu_reset_line()
{
	s_cpu.reset();
}

unsigned rstv_rax_bank_switches; // diag
static void update_data_ram_bank()
{
	uint16_t *const b = s_dmovlay_val == 0 ? &s_banked[0] : &s_banked[0x2000 * (1 + s_data_bank)];
	if (b != s_dbank)
		rstv_rax_bank_switches++;
	s_dbank = b;
}

void adsp21xx_device::m_dmovlay_cb(uint32_t data)
{
	if (data <= 1) {
		s_dmovlay_val = data;
		update_data_ram_bank();
	}
}

static uint16_t rax_control_r(uint32_t offset)
{
	return offset == PROG_FLAG_DATA_REG ? (uint16_t)s_snd_pf0 : s_control[offset];
}

static void recompute_sample_rate()
{
	const uint64_t sample_period = (uint64_t)(s_control[S0_SCLKDIV_REG] + 1) * 16; // DSP cycles per channel sample
	s_dac_rate = (uint32_t)(RAX_CLOCK / sample_period);
	s_dac_on = 1;
	if (s_incs) {
		s_reg_period = sample_period * s_size / (4 * 2 * s_incs);
		if (!s_reg_period)
			s_reg_period = 1;
		s_reg_next = rax_time() + s_reg_period;
	}
}

static void rax_control_w(uint32_t offset, uint16_t data)
{
	s_control[offset] = data;
	switch (offset) {
	case 0x1: s_control[BDMA_INT_ADDR_REG] = data & 0x3fff; break;
	case 0x2: s_control[BDMA_EXT_ADDR_REG] = data & 0x3fff; break;
	case 0x3: s_control[BDMA_CONTROL_REG] = data & 0xff0f; break;
	case 0x4: {
		s_control[BDMA_WORD_COUNT_REG] = data & 0x3fff;
		// a transfer running into the control registers would start itself again (stack overflow): only the outer one runs
		static int s_in_bdma;
		if (s_in_bdma)
			break;
		struct InBdma { InBdma() { s_in_bdma = 1; } ~InBdma() { s_in_bdma = 0; } } in_bdma;
		const uint32_t base = s_rom_bank * 0x400000;
		uint32_t page = (s_control[BDMA_CONTROL_REG] >> 8) & 0xff;
		uint32_t dir = (s_control[BDMA_CONTROL_REG] >> 2) & 1;
		uint32_t type = s_control[BDMA_CONTROL_REG] & 3;
		uint32_t src_addr = (page << 14) | s_control[BDMA_EXT_ADDR_REG];
		uint32_t count = s_control[BDMA_WORD_COUNT_REG];
#define RAX_ROM(a) ((base + (a)) < s_rom_size ? s_rom[base + (a)] : 0xff)
		if (dir == 0) {
			if (type == 0) {
				while (count) {
					const uint32_t w = (RAX_ROM(src_addr) << 16) | (RAX_ROM(src_addr + 1) << 8) | RAX_ROM(src_addr + 2);
					s_cpu.program_write(s_control[BDMA_INT_ADDR_REG], w);
					src_addr += 3;
					++s_control[BDMA_INT_ADDR_REG];
					--count;
				}
			} else if (type == 1) {
				while (count) {
					s_cpu.data_write(s_control[BDMA_INT_ADDR_REG], (uint16_t)((RAX_ROM(src_addr) << 8) | RAX_ROM(src_addr + 1)));
					src_addr += 2;
					++s_control[BDMA_INT_ADDR_REG];
					--count;
				}
			} else {
				const int shift = type == 2 ? 8 : 0;
				while (count) {
					s_cpu.data_write(s_control[BDMA_INT_ADDR_REG], (uint16_t)(RAX_ROM(src_addr) << shift));
					++src_addr;
					++s_control[BDMA_INT_ADDR_REG];
					--count;
				}
			}
		}
		// dir 1 (DSP -> byte memory): MAME stops there ("last stage in Batman Forever!?"); ignored here
#undef RAX_ROM
#if !defined(__vita__)
		if (getenv("RSTV_RAXDMA"))
			fprintf(stderr, "bdma t%llu bank %u page %02x ext %04x type %u dir %u int %04x n %u\n", (unsigned long long)s_now,
				s_rom_bank, (s_control[BDMA_CONTROL_REG] >> 8) & 0xff, s_control[BDMA_EXT_ADDR_REG], type, dir,
				(unsigned)(s_control[BDMA_INT_ADDR_REG] - (data & 0x3fff)), data & 0x3fff);
#endif
		const uint64_t period = data & 0x3fff;
		s_dma_param = src_addr;
		s_dma_next = rax_time() + (period ? period : 1);
		s_cpu.m_icount = 0; // MAME: a timer due before the slice end aborts the timeslice
		s_cpu.m_abort = 1;
		break;
	}
	case S1_AUTOBUF_REG:
		break;
	case S0_AUTOBUF_REG:
		if ((data & 0x0002) == 0) {
			s_dac_on = 0;
			s_reg_period = 0;
		}
		break;
	default:
		break;
	}
}

inline uint16_t adsp21xx_device::data_read(uint32_t addr)
{
	addr &= 0x3fff;
	if (addr < 0x2000)
		return s_dbank[addr];
	if (addr < 0x3fe0)
		return s_iram[addr - 0x2000];
	return rax_control_r(addr - 0x3fe0);
}

inline void adsp21xx_device::data_write(uint32_t addr, uint16_t data)
{
	addr &= 0x3fff;
	if (addr < 0x2000)
		s_dbank[addr] = data;
	else if (addr < 0x3fe0)
		s_iram[addr - 0x2000] = data;
	else
		rax_control_w(addr - 0x3fe0, data);
}

inline uint16_t adsp21xx_device::io_read(uint32_t addr)
{
	if ((addr & 0x7ff) == 3) { // host_r
		set_input(ADSP2181_IRQL0, CLEAR_LINE);
		if (s_cmd_r != s_cmd_w)
			s_data_in = s_cmd[s_cmd_r++ % RAX_QN];
		return s_data_in;
	}
	return 0xffff;
}

inline void adsp21xx_device::io_write(uint32_t addr, uint16_t data)
{
	switch (addr & 0x7ff) {
	case 0: s_data_bank = data & 3; update_data_ram_bank(); break; // ram_bank_w
	case 1: s_rom_bank = data; break;                               // rom_bank_w
	case 3: s_data_out = data; s_snd_pf0 = 0; break;                // host_w
	}
}

inline uint32_t adsp21xx_device::program_read(uint32_t addr) { return s_pram[addr & 0x3fff]; }
static inline void rax_fn_set(uint32_t a);
static inline void raxj_dirty();
static inline void raxj_dirty_at(uint32_t a);
inline void adsp21xx_device::program_write(uint32_t addr, uint32_t data)
{
	s_pram[addr & 0x3fff] = data & 0xffffff;
	rax_fn_set(addr);
	raxj_dirty_at(addr & 0x3fff);
}
inline uint32_t adsp21xx_device::opcode_read() { return s_pram[m_pc & 0x3fff]; }

void adsp21xx_device::m_sport_tx_cb(int which, uint32_t data, uint32_t mask)
{
	if (which != 0)
		return;
	if (s_control[SYSCONTROL_REG] & 0x1000) {
		if (s_control[S0_AUTOBUF_REG] & 0x0002) {
			s_ireg = (s_control[S0_AUTOBUF_REG] >> 9) & 7;
			int mreg = (s_control[S0_AUTOBUF_REG] >> 7) & 3;
			mreg |= s_ireg & 0x04;
			const int lreg = s_ireg;
			uint16_t source = m_i[s_ireg] & 0x3fff;
			s_incs = (uint16_t)m_m[mreg];
			s_size = m_l[lreg] & 0x3fff;
			source -= s_incs;
			m_i[s_ireg] = source & 0x3fff; // set_state_int -> state_import -> update_i
			update_i(s_ireg);
			s_ireg_base = source;
			recompute_sample_rate();
			return;
		}
	}
	s_dac_on = 0;
	s_reg_period = 0;
}

static void adsp_irq0() // autobuffer half: the DAC takes the next words
{
	uint32_t reg = s_cpu.m_i[s_ireg] & 0x3fff;
	const int count = s_size / (4 * (s_incs ? s_incs : 1));
	for (int i = 0; i + 1 < count; i += 2) {
		const int16_t l = (int16_t)s_cpu.data_read(reg);
		reg += s_incs;
		const int16_t r = (int16_t)s_cpu.data_read(reg);
		reg += s_incs;
		if (s_out_w - s_out_r < RAX_ON) {
			s_out[s_out_w % RAX_ON][0] = s_dac_on ? l : 0;
			s_out[s_out_w % RAX_ON][1] = s_dac_on ? r : 0;
			s_out_w++;
		}
	}
	if (count & 1) // MAME reads count words and plays count/2 pairs
		reg += s_incs;
	if (reg >= (uint32_t)s_ireg_base + s_size)
		reg = s_ireg_base;
	s_cpu.m_i[s_ireg] = reg & 0x3fff;
	s_cpu.update_i(s_ireg);
}

static void dma_timer() // BDMA done
{
	s_control[BDMA_WORD_COUNT_REG] = 0;
	s_control[BDMA_EXT_ADDR_REG] = s_dma_param & 0x3fff;
	s_control[BDMA_CONTROL_REG] &= ~0xff00;
	s_control[BDMA_CONTROL_REG] |= ((s_dma_param >> 14) & 0xff) << 8;
	if (s_control[BDMA_CONTROL_REG] & 8)
		cpu_reset_line();
	else {
		s_cpu.set_input(ADSP2181_BDMA, ASSERT_LINE);
		s_cpu.set_input(ADSP2181_BDMA, CLEAR_LINE);
	}
}

/* ---- CPU core glue copied from adsp2100.cpp (ADSP-2181) ---- */
void adsp21xx_device::init()
{
	memset(this, 0, sizeof(*this));
	m_mstat_mask = 0x7f;
	m_imask_mask = 0x3ff;
	create_tables();
	setup_pointers();
}

void adsp21xx_device::setup_pointers()
{
	m_read0_ptr[0x00] = &m_core.ax0.s;
	m_read0_ptr[0x01] = &m_core.ax1.s;
	m_read0_ptr[0x02] = &m_core.mx0.s;
	m_read0_ptr[0x03] = &m_core.mx1.s;
	m_read0_ptr[0x04] = &m_core.ay0.s;
	m_read0_ptr[0x05] = &m_core.ay1.s;
	m_read0_ptr[0x06] = &m_core.my0.s;
	m_read0_ptr[0x07] = &m_core.my1.s;
	m_read0_ptr[0x08] = &m_core.si.s;
	m_read0_ptr[0x09] = &m_core.se.s;
	m_read0_ptr[0x0a] = &m_core.ar.s;
	m_read0_ptr[0x0b] = &m_core.mr.mrx.mr0.s;
	m_read0_ptr[0x0c] = &m_core.mr.mrx.mr1.s;
	m_read0_ptr[0x0d] = &m_core.mr.mrx.mr2.s;
	m_read0_ptr[0x0e] = &m_core.sr.srx.sr0.s;
	m_read0_ptr[0x0f] = &m_core.sr.srx.sr1.s;
	for (int index = 0; index < 4; index++) {
		m_read1_ptr[0x00 + index] = &m_i[0 + index];
		m_read1_ptr[0x04 + index] = (uint32_t *)&m_m[0 + index];
		m_read1_ptr[0x08 + index] = &m_l[0 + index];
		m_read1_ptr[0x0c + index] = &m_l[0 + index];
		m_read2_ptr[0x00 + index] = &m_i[4 + index];
		m_read2_ptr[0x04 + index] = (uint32_t *)&m_m[4 + index];
		m_read2_ptr[0x08 + index] = &m_l[4 + index];
		m_read2_ptr[0x0c + index] = &m_l[4 + index];
	}
	m_alu_xregs[0] = &m_core.ax0;
	m_alu_xregs[1] = &m_core.ax1;
	m_alu_xregs[2] = &m_core.ar;
	m_alu_xregs[3] = &m_core.mr.mrx.mr0;
	m_alu_xregs[4] = &m_core.mr.mrx.mr1;
	m_alu_xregs[5] = &m_core.mr.mrx.mr2;
	m_alu_xregs[6] = &m_core.sr.srx.sr0;
	m_alu_xregs[7] = &m_core.sr.srx.sr1;
	m_alu_yregs[0] = &m_core.ay0;
	m_alu_yregs[1] = &m_core.ay1;
	m_alu_yregs[2] = &m_core.af;
	m_alu_yregs[3] = &m_core.zero;
	m_mac_xregs[0] = &m_core.mx0;
	m_mac_xregs[1] = &m_core.mx1;
	m_mac_xregs[2] = &m_core.ar;
	m_mac_xregs[3] = &m_core.mr.mrx.mr0;
	m_mac_xregs[4] = &m_core.mr.mrx.mr1;
	m_mac_xregs[5] = &m_core.mr.mrx.mr2;
	m_mac_xregs[6] = &m_core.sr.srx.sr0;
	m_mac_xregs[7] = &m_core.sr.srx.sr1;
	m_mac_yregs[0] = &m_core.my0;
	m_mac_yregs[1] = &m_core.my1;
	m_mac_yregs[2] = &m_core.mf;
	m_mac_yregs[3] = &m_core.zero;
	m_shift_xregs[0] = &m_core.si;
	m_shift_xregs[1] = &m_core.si;
	m_shift_xregs[2] = &m_core.ar;
	m_shift_xregs[3] = &m_core.mr.mrx.mr0;
	m_shift_xregs[4] = &m_core.mr.mrx.mr1;
	m_shift_xregs[5] = &m_core.mr.mrx.mr2;
	m_shift_xregs[6] = &m_core.sr.srx.sr0;
	m_shift_xregs[7] = &m_core.sr.srx.sr1;
}

void adsp21xx_device::reset()
{
	m_core.zero.u = m_alt.zero.u = 0;
	write_reg1(0x08, m_l[0]);   write_reg1(0x00, m_i[0]);
	write_reg1(0x09, m_l[1]);   write_reg1(0x01, m_i[1]);
	write_reg1(0x0a, m_l[2]);   write_reg1(0x02, m_i[2]);
	write_reg1(0x0b, m_l[3]);   write_reg1(0x03, m_i[3]);
	write_reg2(0x08, m_l[4]);   write_reg2(0x00, m_i[4]);
	write_reg2(0x09, m_l[5]);   write_reg2(0x01, m_i[5]);
	write_reg2(0x0a, m_l[6]);   write_reg2(0x02, m_i[6]);
	write_reg2(0x0b, m_l[7]);   write_reg2(0x03, m_i[7]);
	m_pmovlay = m_dmovlay = 0;
	update_dmovlay();
	m_pc = 0;
	m_ppc = m_pc;
	m_loop = 0xffff;
	m_loop_condition = 0;
	m_astat_clear = ~(CFLAG | VFLAG | NFLAG | ZFLAG);
	m_mstat = 0;
	m_sstat = 0x55;
	m_idle = 0;
	update_mstat();
	m_pc_sp = 0;
	m_cntr_sp = 0;
	m_stat_sp = 0;
	m_loop_sp = 0;
	m_flagout = 0;
	m_flagin = 0;
	m_fl0 = 0;
	m_fl1 = 0;
	m_fl2 = 0;
	m_imask = 0;
	for (int irq = 0; irq < 10; irq++)
		m_irq_state[irq] = m_irq_latch[irq] = CLEAR_LINE;
}

void adsp21xx_device::set_input(int inputnum, int state)
{
	if (state != CLEAR_LINE && m_irq_state[inputnum] == CLEAR_LINE)
		m_irq_latch[inputnum] = 1;
	m_irq_state[inputnum] = state;
}

bool adsp21xx_device::irq_pending() const
{
	if (((m_icntl & 4) ? m_irq_latch[ADSP2181_IRQ2] : m_irq_state[ADSP2181_IRQ2]) && (m_imask & (0x200 >> 0))) return true;
	if (m_irq_state[ADSP2181_IRQL1] && (m_imask & (0x200 >> 1))) return true;
	if (m_irq_state[ADSP2181_IRQL0] && (m_imask & (0x200 >> 2))) return true;
	if (m_irq_latch[ADSP2181_SPORT0_TX] && (m_imask & (0x200 >> 3))) return true;
	if (m_irq_latch[ADSP2181_SPORT0_RX] && (m_imask & (0x200 >> 4))) return true;
	if (m_irq_latch[ADSP2181_IRQE] && (m_imask & (0x200 >> 5))) return true;
	if (m_irq_latch[ADSP2181_BDMA] && (m_imask & (0x200 >> 6))) return true;
	if (((m_icntl & 2) ? m_irq_latch[ADSP2181_IRQ1] : m_irq_state[ADSP2181_IRQ1]) && (m_imask & (0x200 >> 7))) return true;
	if (((m_icntl & 1) ? m_irq_latch[ADSP2181_IRQ0] : m_irq_state[ADSP2181_IRQ0]) && (m_imask & (0x200 >> 8))) return true;
	if (m_irq_latch[ADSP2181_TIMER] && (m_imask & (0x200 >> 9))) return true;
	return false;
}

bool adsp21xx_device::generate_irq(int which, int indx)
{
	if (!(m_imask & (0x200 >> indx)))
		return false;
	m_irq_latch[which] = 0;
	pc_stack_push();
	stat_stack_push();
	m_pc = 0x04 + indx * 4;
	m_idle = 0;
	if (m_icntl & 0x10) m_imask &= ~(0x3ff >> indx);
	else m_imask &= ~0x3ff;
	return true;
}

void adsp21xx_device::check_irqs()
{
	uint8_t check;
	check = (m_icntl & 4) ? m_irq_latch[ADSP2181_IRQ2] : m_irq_state[ADSP2181_IRQ2];
	if (check && generate_irq(ADSP2181_IRQ2, 0)) return;
	check = m_irq_state[ADSP2181_IRQL1];
	if (check && generate_irq(ADSP2181_IRQL1, 1)) return;
	check = m_irq_state[ADSP2181_IRQL0];
	if (check && generate_irq(ADSP2181_IRQL0, 2)) return;
	check = m_irq_latch[ADSP2181_SPORT0_TX];
	if (check && generate_irq(ADSP2181_SPORT0_TX, 3)) return;
	check = m_irq_latch[ADSP2181_SPORT0_RX];
	if (check && generate_irq(ADSP2181_SPORT0_RX, 4)) return;
	check = m_irq_latch[ADSP2181_IRQE];
	if (check && generate_irq(ADSP2181_IRQE, 5)) return;
	check = m_irq_latch[ADSP2181_BDMA];
	if (check && generate_irq(ADSP2181_BDMA, 6)) return;
	check = (m_icntl & 2) ? m_irq_latch[ADSP2181_IRQ1] : m_irq_state[ADSP2181_IRQ1];
	if (check && generate_irq(ADSP2181_IRQ1, 7)) return;
	check = (m_icntl & 1) ? m_irq_latch[ADSP2181_IRQ0] : m_irq_state[ADSP2181_IRQ0];
	if (check && generate_irq(ADSP2181_IRQ0, 8)) return;
	check = m_irq_latch[ADSP2181_TIMER];
	if (check && generate_irq(ADSP2181_TIMER, 9)) return;
}

void adsp21xx_device::create_tables()
{
	for (int i = 0; i < 0x4000; i++) {
		uint16_t data = 0;
		data |= (i >> 13) & 0x0001;
		data |= (i >> 11) & 0x0002;
		data |= (i >> 9)  & 0x0004;
		data |= (i >> 7)  & 0x0008;
		data |= (i >> 5)  & 0x0010;
		data |= (i >> 3)  & 0x0020;
		data |= (i >> 1)  & 0x0040;
		data |= (i << 1)  & 0x0080;
		data |= (i << 3)  & 0x0100;
		data |= (i << 5)  & 0x0200;
		data |= (i << 7)  & 0x0400;
		data |= (i << 9)  & 0x0800;
		data |= (i << 11) & 0x1000;
		data |= (i << 13) & 0x2000;
		m_reverse_table[i] = data;
	}
	for (int i = 0; i < 0x4000; i++) {
		if (i > 0x2000)      m_mask_table[i] = 0x0000;
		else if (i > 0x1000) m_mask_table[i] = 0x2000;
		else if (i > 0x0800) m_mask_table[i] = 0x3000;
		else if (i > 0x0400) m_mask_table[i] = 0x3800;
		else if (i > 0x0200) m_mask_table[i] = 0x3c00;
		else if (i > 0x0100) m_mask_table[i] = 0x3e00;
		else if (i > 0x0080) m_mask_table[i] = 0x3f00;
		else if (i > 0x0040) m_mask_table[i] = 0x3f80;
		else if (i > 0x0020) m_mask_table[i] = 0x3fc0;
		else if (i > 0x0010) m_mask_table[i] = 0x3fe0;
		else if (i > 0x0008) m_mask_table[i] = 0x3ff0;
		else if (i > 0x0004) m_mask_table[i] = 0x3ff8;
		else if (i > 0x0002) m_mask_table[i] = 0x3ffc;
		else if (i > 0x0001) m_mask_table[i] = 0x3ffe;
		else                 m_mask_table[i] = 0x3fff;
	}
	for (int i = 0; i < 0x100; i++) {
		int az = ((i & ZFLAG) != 0);
		int an = ((i & NFLAG) != 0);
		int av = ((i & VFLAG) != 0);
		int ac = ((i & CFLAG) != 0);
		int mv = ((i & MVFLAG) != 0);
		int as = ((i & SFLAG) != 0);
		m_condition_table[i | 0x000] = az;
		m_condition_table[i | 0x100] = !az;
		m_condition_table[i | 0x200] = !((an ^ av) | az);
		m_condition_table[i | 0x300] = (an ^ av) | az;
		m_condition_table[i | 0x400] = an ^ av;
		m_condition_table[i | 0x500] = !(an ^ av);
		m_condition_table[i | 0x600] = av;
		m_condition_table[i | 0x700] = !av;
		m_condition_table[i | 0x800] = ac;
		m_condition_table[i | 0x900] = !ac;
		m_condition_table[i | 0xa00] = as;
		m_condition_table[i | 0xb00] = !as;
		m_condition_table[i | 0xc00] = mv;
		m_condition_table[i | 0xd00] = !mv;
		m_condition_table[i | 0xf00] = 1;
	}
}

// the run() loop body after the fetch (adsp2100.cpp execute_run)
RAX_AI void adsp21xx_device::exec(uint32_t op)
{
	uint32_t temp;
	switch (BIT(op, 16, 8))
	{
		case 0x00:
			break;
		case 0x01:
			if (!BIT(op, 15))
				write_reg0(BIT(op, 0, 4), io_read(BIT(op, 4, 11)));
			else
				io_write(BIT(op, 4, 11), read_reg0(BIT(op, 0, 4)));
			break;
		case 0x02:
			if (BIT(op, 15))
			{
				m_idle = 1;
				m_icount = 0;
			}
			else
			{
				if (condition(BIT(op, 0, 4)))
				{
					if (BIT(op, 5)) m_flagout = 0;
					if (BIT(op, 4)) m_flagout ^= 1;
					if (BIT(op, 7)) m_fl0 = 0;
					if (BIT(op, 6)) m_fl0 ^= 1;
					if (BIT(op, 9)) m_fl1 = 0;
					if (BIT(op, 8)) m_fl1 ^= 1;
					if (BIT(op, 11)) m_fl2 = 0;
					if (BIT(op, 10)) m_fl2 ^= 1;
				}
			}
			break;
		case 0x03:
			if (BIT(op, 1) ? m_flagin : !m_flagin)
			{
				if (BIT(op, 0))
					pc_stack_push();
				m_pc = BIT(op, 4, 12) | BIT(op, 2, 2) << 12;
			}
			break;
		case 0x04:
			if (BIT(op, 4)) pc_stack_pop_val();
			if (BIT(op, 3)) loop_stack_pop();
			if (BIT(op, 2)) cntr_stack_pop();
			if (BIT(op, 1))
			{
				if (BIT(op, 0)) stat_stack_pop();
				else stat_stack_push();
			}
			break;
		case 0x05:
			if (GET_MV)
			{
				if (m_core.mr.mrx.mr2.u & 0x80)
					m_core.mr.mrx.mr2.u = 0xffff, m_core.mr.mrx.mr1.u = 0x8000, m_core.mr.mrx.mr0.u = 0x0000;
				else
					m_core.mr.mrx.mr2.u = 0x0000, m_core.mr.mrx.mr1.u = 0x7fff, m_core.mr.mrx.mr0.u = 0xffff;
			}
			break;
		case 0x06:
			{
				int xop = BIT(op, 8, 3);
				int yop = BIT(op, 11, 2);
				xop = ALU_GETXREG_UNSIGNED(xop);
				yop = ALU_GETYREG_UNSIGNED(yop);
				temp = xop ^ yop;
				m_astat = (m_astat & ~QFLAG) | ((temp >> 10) & QFLAG);
				m_core.af.u = (yop << 1) | (m_core.ay0.u >> 15);
				m_core.ay0.u = (m_core.ay0.u << 1) | (temp >> 15);
			}
			break;
		case 0x07:
			{
				int xop = BIT(op, 8, 3);
				int res;
				xop = ALU_GETXREG_UNSIGNED(xop);
				if (GET_Q)
					res = m_core.af.u + xop;
				else
					res = m_core.af.u - xop;
				temp = res ^ xop;
				m_astat = (m_astat & ~QFLAG) | ((temp >> 10) & QFLAG);
				m_core.af.u = (res << 1) | (m_core.ay0.u >> 15);
				m_core.ay0.u = (m_core.ay0.u << 1) | ((~temp >> 15) & 0x0001);
			}
			break;
		case 0x08:
			break;
		case 0x09:
			temp = BIT(op, 2, 3);
			modify_address(temp, (temp & 4) | (op & 3));
			break;
		case 0x0a:
			if (condition(BIT(op, 0, 4)))
			{
				pc_stack_pop();
				if (BIT(op, 4))
					stat_stack_pop();
			}
			break;
		case 0x0b:
			if (condition(BIT(op, 0, 4)))
			{
				if (BIT(op, 4))
					pc_stack_push();
				m_pc = m_i[4 + BIT(op, 6, 2)] & 0x3fff;
			}
			break;
		case 0x0c:
			if (BIT(op, 3)) m_mstat = BIT(op, 2) ? (m_mstat | MSTAT_GOMODE) : (m_mstat & ~MSTAT_GOMODE);
			if (BIT(op, 13)) m_mstat = BIT(op, 12) ? (m_mstat | MSTAT_INTEGER) : (m_mstat & ~MSTAT_INTEGER);
			if (BIT(op, 15)) m_mstat = BIT(op, 14) ? (m_mstat | MSTAT_TIMER) : (m_mstat & ~MSTAT_TIMER);
			if (BIT(op, 5)) m_mstat = BIT(op, 4) ? (m_mstat | MSTAT_BANK) : (m_mstat & ~MSTAT_BANK);
			if (BIT(op, 7)) m_mstat = BIT(op, 6) ? (m_mstat | MSTAT_REVERSE) : (m_mstat & ~MSTAT_REVERSE);
			if (BIT(op, 9)) m_mstat = BIT(op, 8) ? (m_mstat | MSTAT_STICKYV) : (m_mstat & ~MSTAT_STICKYV);
			if (BIT(op, 11)) m_mstat = BIT(op, 10) ? (m_mstat | MSTAT_SATURATE) : (m_mstat & ~MSTAT_SATURATE);
			update_mstat();
			break;
		case 0x0d:
			switch (BIT(op, 8, 4))
			{
				case 0x00:  write_reg0(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
				case 0x01:  write_reg0(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
				case 0x02:  write_reg0(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
				case 0x03:  write_reg0(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
				case 0x04:  write_reg1(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
				case 0x05:  write_reg1(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
				case 0x06:  write_reg1(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
				case 0x07:  write_reg1(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
				case 0x08:  write_reg2(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
				case 0x09:  write_reg2(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
				case 0x0a:  write_reg2(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
				case 0x0b:  write_reg2(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
				case 0x0c:  write_reg3(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
				case 0x0d:  write_reg3(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
				case 0x0e:  write_reg3(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
				case 0x0f:  write_reg3(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
			}
			break;
		case 0x0e:
			if (condition(BIT(op, 0, 4))) shift_op(op);
			break;
		case 0x0f:
			shift_op_imm(op);
			break;
		case 0x10:
			shift_op(op);
			temp = read_reg0(BIT(op, 0, 4));
			write_reg0(BIT(op, 4, 4), temp);
			break;
		case 0x11:
			if (BIT(op, 15))
			{
				pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
				shift_op(op);
			}
			else
			{
				shift_op(op);
				write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
			}
			break;
		case 0x12:
			if (BIT(op, 15))
			{
				data_write_dag1(op, read_reg0(BIT(op, 4, 4)));
				shift_op(op);
			}
			else
			{
				shift_op(op);
				write_reg0(BIT(op, 4, 4), data_read_dag1(op));
			}
			break;
		case 0x13:
			if (BIT(op, 15))
			{
				data_write_dag2(op, read_reg0(BIT(op, 4, 4)));
				shift_op(op);
			}
			else
			{
				shift_op(op);
				write_reg0(BIT(op, 4, 4), data_read_dag2(op));
			}
			break;
		case 0x14: case 0x15: case 0x16: case 0x17:
			loop_stack_push(op & 0x3ffff);
			pc_stack_push();
			break;
		case 0x18: case 0x19: case 0x1a: case 0x1b:
			if (condition(BIT(op, 0, 4)))
			{
				m_pc = BIT(op, 4, 14);
				if (m_pc == m_ppc)
					m_icount = 0;
			}
			break;
		case 0x1c: case 0x1d: case 0x1e: case 0x1f:
			if (condition(BIT(op, 0, 4)))
			{
				pc_stack_push();
				m_pc = BIT(op, 4, 14);
			}
			break;
		case 0x20: case 0x21:
			if (condition(BIT(op, 0, 4)))
			{
				if ((op & 0x0018f0) == 0x000010)
					mac_op_mr_xop(op);
				else
					mac_op_mr(op);
			}
			break;
		case 0x22: case 0x23:
			if (condition(BIT(op, 0, 4)))
			{
				if (BIT(op, 4))
					alu_op_ar_const(op);
				else
					alu_op_ar(op);
			}
			break;
		case 0x24: case 0x25:
			if (condition(BIT(op, 0, 4)))
			{
				if ((op & 0x0018f0) == 0x000010)
					mac_op_mf_xop(op);
				else
					mac_op_mf(op);
			}
			break;
		case 0x26: case 0x27:
			if (condition(BIT(op, 0, 4)))
			{
				if (BIT(op, 4))
					alu_op_af_const(op);
				else
					alu_op_af(op);
			}
			break;
		case 0x28: case 0x29:
			temp = read_reg0(BIT(op, 0, 4));
			mac_op_mr(op);
			write_reg0(BIT(op, 4, 4), temp);
			break;
		case 0x2a: case 0x2b:
			if (BIT(op, 0, 8) == 0xaa)
				alu_op_none(op);
			else
			{
				temp = read_reg0(BIT(op, 0, 4));
				alu_op_ar(op);
				write_reg0(BIT(op, 4, 4), temp);
			}
			break;
		case 0x2c: case 0x2d:
			temp = read_reg0(BIT(op, 0, 4));
			mac_op_mf(op);
			write_reg0(BIT(op, 4, 4), temp);
			break;
		case 0x2e: case 0x2f:
			temp = read_reg0(BIT(op, 0, 4));
			alu_op_af(op);
			write_reg0(BIT(op, 4, 4), temp);
			break;
		case 0x30: case 0x31: case 0x32: case 0x33:
			write_reg0(BIT(op, 0, 4), util::sext(op >> 4, 14));
			break;
		case 0x34: case 0x35: case 0x36: case 0x37:
			write_reg1(BIT(op, 0, 4), util::sext(op >> 4, 14));
			break;
		case 0x38: case 0x39: case 0x3a: case 0x3b:
			write_reg2(BIT(op, 0, 4), util::sext(op >> 4, 14));
			break;
		case 0x3c: case 0x3d: case 0x3e: case 0x3f:
			write_reg3(BIT(op, 0, 4), util::sext(op >> 4, 14));
			break;
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
		case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
			write_reg0(BIT(op, 0, 4), BIT(op, 4, 16));
			break;
		case 0x50: case 0x51:
			mac_op_mr(op);
			write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
			break;
		case 0x52: case 0x53:
			alu_op_ar(op);
			write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
			break;
		case 0x54: case 0x55:
			mac_op_mf(op);
			write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
			break;
		case 0x56: case 0x57:
			alu_op_af(op);
			write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
			break;
		case 0x58: case 0x59:
			pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			mac_op_mr(op);
			break;
		case 0x5a: case 0x5b:
			pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			alu_op_ar(op);
			break;
		case 0x5c: case 0x5d:
			pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			mac_op_mf(op);
			break;
		case 0x5e: case 0x5f:
			pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			alu_op_af(op);
			break;
		case 0x60: case 0x61:
			mac_op_mr(op);
			write_reg0(BIT(op, 4, 4), data_read_dag1(op));
			break;
		case 0x62: case 0x63:
			alu_op_ar(op);
			write_reg0(BIT(op, 4, 4), data_read_dag1(op));
			break;
		case 0x64: case 0x65:
			mac_op_mf(op);
			write_reg0(BIT(op, 4, 4), data_read_dag1(op));
			break;
		case 0x66: case 0x67:
			alu_op_af(op);
			write_reg0(BIT(op, 4, 4), data_read_dag1(op));
			break;
		case 0x68: case 0x69:
			data_write_dag1(op, read_reg0(BIT(op, 4, 4)));
			mac_op_mr(op);
			break;
		case 0x6a: case 0x6b:
			data_write_dag1(op, read_reg0(BIT(op, 4, 4)));
			alu_op_ar(op);
			break;
		case 0x6c: case 0x6d:
			data_write_dag1(op, read_reg0(BIT(op, 4, 4)));
			mac_op_mf(op);
			break;
		case 0x6e: case 0x6f:
			data_write_dag1(op, read_reg0(BIT(op, 4, 4)));
			alu_op_af(op);
			break;
		case 0x70: case 0x71:
			mac_op_mr(op);
			write_reg0(BIT(op, 4, 4), data_read_dag2(op));
			break;
		case 0x72: case 0x73:
			alu_op_ar(op);
			write_reg0(BIT(op, 4, 4), data_read_dag2(op));
			break;
		case 0x74: case 0x75:
			mac_op_mf(op);
			write_reg0(BIT(op, 4, 4), data_read_dag2(op));
			break;
		case 0x76: case 0x77:
			alu_op_af(op);
			write_reg0(BIT(op, 4, 4), data_read_dag2(op));
			break;
		case 0x78: case 0x79:
			data_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			mac_op_mr(op);
			break;
		case 0x7a: case 0x7b:
			data_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			alu_op_ar(op);
			break;
		case 0x7c: case 0x7d:
			data_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			mac_op_mf(op);
			break;
		case 0x7e: case 0x7f:
			data_write_dag2(op, read_reg0(BIT(op, 4, 4)));
			alu_op_af(op);
			break;
		case 0x80: case 0x81: case 0x82: case 0x83:
			write_reg0(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
			break;
		case 0x84: case 0x85: case 0x86: case 0x87:
			write_reg1(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
			break;
		case 0x88: case 0x89: case 0x8a: case 0x8b:
			write_reg2(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
			break;
		case 0x8c: case 0x8d: case 0x8e: case 0x8f:
			write_reg3(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
			break;
		case 0x90: case 0x91: case 0x92: case 0x93:
			data_write(BIT(op, 4, 14), read_reg0(BIT(op, 0, 4)));
			break;
		case 0x94: case 0x95: case 0x96: case 0x97:
			data_write(BIT(op, 4, 14), read_reg1(BIT(op, 0, 4)));
			break;
		case 0x98: case 0x99: case 0x9a: case 0x9b:
			data_write(BIT(op, 4, 14), read_reg2(BIT(op, 0, 4)));
			break;
		case 0x9c: case 0x9d: case 0x9e: case 0x9f:
			data_write(BIT(op, 4, 14), read_reg3(BIT(op, 0, 4)));
			break;
		case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
		case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
			data_write_dag1(op, BIT(op, 4, 16));
			break;
		case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			data_write_dag2(op, BIT(op, 4, 16));
			break;
		// 11xxxxxx: MAC/ALU with a data read (DAG1) and a pgm read (DAG2); bits 18-21 pick the two registers
		default:
			{
				if (BIT(op, 17))
					alu_op_ar(op);
				else
					mac_op_mr(op);
				const uint32_t d = data_read_dag1(op);
				switch (BIT(op, 18, 2))
				{
					case 0: m_core.ax0.u = d; break;
					case 1: m_core.ax1.u = d; break;
					case 2: m_core.mx0.u = d; break;
					case 3: m_core.mx1.u = d; break;
				}
				const uint32_t p = pgm_read_dag2(op >> 4);
				switch (BIT(op, 20, 2))
				{
					case 0: m_core.ay0.u = p; break;
					case 1: m_core.ay1.u = p; break;
					case 2: m_core.my0.u = p; break;
					case 3: m_core.my1.u = p; break;
				}
			}
			break;
	}
}

// ADR-173: handlers specialised on op bits 13-23 (instruction kind and ALU / MAC function): the constant bits fold the
// switches. Kinds without an ALU / MAC / shift function (whose bits 13-15 are immediates or addresses) only fold the
// kind (bits 16-23).
static constexpr bool rax_k_amf(uint32_t t)
{
	return t >= 0x60 && t <= 0x7f; // the most run kinds (ALU / MAC with a data memory move)
}
static constexpr uint32_t rax_k_canon(uint32_t k) { return rax_k_amf(k >> 3) ? k : (k & ~7u); }
typedef void (*RaxExecFn)(adsp21xx_device *c, uint32_t op);
template <uint32_t K> static void rax_exec_k(adsp21xx_device *c, uint32_t op)
{
	if ((K & 7) == 0 && !rax_k_amf(K >> 3))
		c->exec((op & 0xFFFFu) | ((K >> 3) << 16));
	else
		c->exec((op & 0x1FFFu) | (K << 13));
}
template <size_t... I> static constexpr std::array<RaxExecFn, sizeof...(I)> rax_make_tab(std::index_sequence<I...>)
{
	return {{ &rax_exec_k<rax_k_canon((uint32_t)I)>... }};
}
static const std::array<RaxExecFn, 2048> s_exec_tab = rax_make_tab(std::make_index_sequence<2048>{});
// ADR-177: addresses a DO in program memory names as its loop end (never cleared: only extra checks); a compiled block
// checks m_loop only at these
static uint8_t s_loopend[0x4000];
static inline void raxj_loopend_new(uint32_t a);
// each program word with its handler, side by side (one cache line for both at each step)
struct RaxIns { RaxExecFn fn; uint32_t op; };
static RaxIns s_ins[0x4000];
static inline void rax_fn_set(uint32_t a)
{
	const uint32_t op = s_pram[a & 0x3fff];
	s_ins[a & 0x3fff].fn = s_exec_tab[(op >> 13) & 0x7ff];
	s_ins[a & 0x3fff].op = op;
	if ((op >> 18) == 0x05) { // DO (0x14-0x17): loop end = bits 4-17
		const uint32_t e = (op >> 4) & 0x3fff;
		if (!s_loopend[e]) {
			s_loopend[e] = 1;
			raxj_loopend_new(e);
		}
	}
}
static inline void raxj_dirty();
static void rax_fn_all() { for (uint32_t a = 0; a < 0x4000; a++) rax_fn_set(a); raxj_dirty(); }

/* ---- ADR-175: block compiler (A32) ----
   A block is the code from one PC to the first instruction that may change the flow (jumps, calls, returns, DO, stack
   ops, register group 3, program memory writes), at most RAXJ_MAX instructions. Per instruction it does what run()
   does: stop before an address equal to the loop end (r6 = m_loop at entry; run() handles the loop end), m_pc = next,
   call the instruction's handler (s_fn), m_icount--, and leave when that reaches 0 (an IDLE or a BDMA start set it
   to 0 in the handler). r5 counts the instructions run (m_jn on exit). The handlers are the interpreter's own, so
   the results are the same; what goes away is the fetch, the dispatch and the loop bookkeeping.
   Vita: only the emu thread may write the code region (ADR-110). The sound thread compiles a block into a staging slot
   (the code is position independent) and interprets meanwhile; the emu thread copies staged slots into the code region
   at the start of each frame (rstv_rax_jit_install); the sound thread takes the installed ones at its next frame.
   A program write into compiled code starts a new generation: the sound thread drops its blocks, the emu thread
   starts the region again. */
#if defined(__arm__)
#include "rstv_jit.h"
#define RAXJ_CODE (2816 * 1024)
#define RAXJ_MAX 48
#define RAXJ_SLOTS 32
#define RAXJ_SLOTW (200 * RAXJ_MAX + 64)
typedef void (*RaxBlock)(adsp21xx_device *c);
enum { RJ_FREE, RJ_STAGED, RJ_INSTALLED };
struct RaxSlot {
	volatile int state;
	uint32_t pc, gen, mode;
	int len, n;                      // words, instructions
	uint32_t stamp;                  // program write count when compiled
	uint32_t *volatile code;
	uint32_t w[RAXJ_SLOTW];
};
static RaxSlot s_slot[RAXJ_SLOTS];
static uint32_t *s_jp, *s_jend;      // the staging buffer being written
static RaxBlock s_blk[0x4000][2];    // sound thread only; two MSTAT modes per address (tag in s_blkt)
static uint8_t s_blkt[0x4000][2];
static uint8_t s_jcov[0x4000];       // 1: inside a compiled block
static uint8_t s_jpend[0x4000];      // 1: staged, not taken yet
static int s_jdirty, s_jfull, s_jon = -1;
static volatile uint32_t s_jgen = 1; // generation (sound thread writes)
static volatile int s_jregion_full;  // emu thread: the region ran out; nothing more is installed
// ADR-177: code region addresses are never used twice (a reused address ran stale instructions on the sound thread's
// core); a program write drops only the blocks over that address
static uint8_t s_blk_n[0x4000];      // instructions of the block at each address
static uint32_t s_wstamp[0x4000], s_wcount; // program writes into compiled code: when
static int s_jlast_n;
unsigned rstv_rax_jit_fb;          // instructions run through a handler inside blocks (diag)
unsigned rstv_rax_jit_compiles, rstv_rax_jit_drops, rstv_rax_jit_wdrops;
#if !defined(__vita__)
static unsigned s_jfb_kind[256];   // harness: those per top byte
extern "C" void rax_jfb_count(uint32_t op) { s_jfb_kind[op >> 16]++; }
#endif
// emu thread
static uint32_t *s_jcode;
static size_t s_jused;
static uint32_t s_jgen_seen;
static int s_jalloc_failed;
unsigned rstv_rax_jit_blocks;
int rstv_rax_jit_on = 2; // ADR-177: on (ux0:data/restv/raxjit "0" turns it off; harness: RSTV_RAXJIT=1 to use it)
#if defined(__vita__)
extern "C" void rstv_log_printf(const char *fmt, ...);
#define RAXJ_LOG(...) rstv_log_printf(__VA_ARGS__)
#else
#define RAXJ_LOG(...) ((void)0)
#endif

static inline void jw(uint32_t w) { if (s_jp < s_jend) *s_jp++ = w; else s_jfull = 1; }
#define JAL 0xE0000000u
static void j_movw(int rd, uint32_t v) { jw(JAL | 0x03000000u | (((v >> 12) & 15) << 16) | ((uint32_t)rd << 12) | (v & 0xFFF)); }
static void j_movt(int rd, uint32_t v) { jw(JAL | 0x03400000u | (((v >> 28) & 15) << 16) | ((uint32_t)rd << 12) | ((v >> 16) & 0xFFF)); }
static void j_mov32(int rd, uint32_t v) { j_movw(rd, v & 0xFFFF); if (v >> 16) j_movt(rd, v); }
static void j_ldr(int rt, int rn, int off) { jw(JAL | 0x05900000u | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (uint32_t)off); }
static void j_str(int rt, int rn, int off) { jw(JAL | 0x05800000u | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (uint32_t)off); }
static uint32_t *j_bfwd(uint32_t cc) { uint32_t *at = s_jp; jw(cc | 0x0A000000u); return at; }
static void j_patch(uint32_t *at, const uint32_t *to) { if (at < s_jend) *at = (*at & 0xFF000000u) | ((uint32_t)(to - (at + 2)) & 0x00FFFFFFu); }

// instructions that end a block (after running them)
static int raxj_ender(uint32_t op)
{
	const uint32_t t = op >> 16;
	switch (t) {
	case 0x03: case 0x04: case 0x0a: case 0x0b: return 1;
	case 0x0c: return 1;                                                  // mode control: MSTAT changes (ADR-177)
	case 0x0d: return BIT(op, 8, 4) >= 0x0c || (BIT(op, 8, 4) & 3) == 3; // to or from group 3
	case 0x11: return BIT(op, 15);                                        // shift + pgm write (handler)
	}
	if (t >= 0x14 && t <= 0x1f) return 1;                                 // DO, jumps, calls
	if (t >= 0x3c && t <= 0x3f) return 1;                                 // group 3 immediate
	if ((t >= 0x8c && t <= 0x8f) || (t >= 0x9c && t <= 0x9f)) return 1; // DM <-> group 3
	return 0;
}

// ---- ADR-177: instruction bodies in A32 ----
// Block registers: r4 = this, r5 = m_icount (live), r6 = m_loop, r7 = s_dbank (data 0000-1FFF), r8 = s_iram - 0x4000
// (byte offset addr * 2 lands in s_iram for 2000-3FDF), r9 = m_condition_table, r10 = instructions run, r11 = s_pram.
// r0-r3, r12, lr scratch. Instructions not handled here call their interpreter handler (as in ADR-175), with m_pc and
// m_icount stored first and r5 / r7 reloaded after. The block is made for the MSTAT mode bits it was compiled with
// (bit reverse, sticky V, saturate, integer): another mode at entry makes run() interpret (m_jn = -1).
static uint32_t s_jmstat;          // mode bits the block being compiled assumes
enum { JR0 = 0, JR1, JR2, JR3, JR4, JR5, JR6, JR7, JR8, JR9, JR10, JR11, JR12, JSP, JLR, JPC };
static const uint32_t JMS = 0x1e;  // MSTAT bits a block depends on
static int s_jfb;                  // instructions left to handlers in the block being compiled (diag)

static int j_imm(uint32_t v, uint32_t *enc)
{
	for (uint32_t r = 0; r < 16; r++) {
		const uint32_t x = r ? ((v << (2 * r)) | (v >> (32 - 2 * r))) : v;
		if (x <= 0xFF) { *enc = (r << 8) | x; return 1; }
	}
	return 0;
}
enum { JAND = 0, JEOR = 1, JSUB = 2, JRSB = 3, JADD = 4, JADC = 5, JSBC = 6, JTST = 8, JCMP = 10, JORR = 12, JMOV = 13, JBIC = 14, JMVN = 15 };
enum { JLSL = 0, JLSR = 1, JASR = 2 };
#define JC(cc) ((uint32_t)(cc) << 28)
enum { CEQ = 0, CNE = 1, CHS = 2, CLO = 3, CMI = 4, CHI = 8, CLS = 9, CLT = 11, CGT = 12, CLE = 13, CAL = 14 };
static void j_dpi(int cc, int op, int s, int rd, int rn, uint32_t imm)
{
	uint32_t e = 0;
	if (!j_imm(imm, &e)) s_jfull = 1; // only encodable constants are used; flag a slip as overflow
	jw(JC(cc) | 0x02000000u | ((uint32_t)op << 21) | ((uint32_t)s << 20) | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | e);
}
static void j_dpr(int cc, int op, int s, int rd, int rn, int rm, int sh = 0, int amt = 0)
{
	jw(JC(cc) | ((uint32_t)op << 21) | ((uint32_t)s << 20) | ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | ((uint32_t)amt << 7) |
	   ((uint32_t)sh << 5) | (uint32_t)rm);
}
static void j_mov(int rd, int rm) { j_dpr(CAL, JMOV, 0, rd, 0, rm); }
static void j_hw(uint32_t base, int cc, int rt, int rn, int off) // ldrh / strh / ldrsh / ldrd / strd, 8-bit offset
{
	jw(JC(cc) | base | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (((uint32_t)off >> 4 & 15) << 8) | ((uint32_t)off & 15));
}
static void j_ldrh(int rt, int rn, int off) { j_hw(0x01D000B0u, CAL, rt, rn, off); }
static void j_ldrsh(int rt, int rn, int off) { j_hw(0x01D000F0u, CAL, rt, rn, off); }
static void j_strh(int rt, int rn, int off) { j_hw(0x01C000B0u, CAL, rt, rn, off); }
static void j_ldrd(int rt, int rn, int off) { j_hw(0x01C000D0u, CAL, rt, rn, off); }
static void j_strd(int rt, int rn, int off) { j_hw(0x01C000F0u, CAL, rt, rn, off); }
static void j_ldrh_r(int cc, int rt, int rn, int rm) { jw(JC(cc) | 0x019000B0u | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (uint32_t)rm); }
static void j_strh_r(int cc, int rt, int rn, int rm) { jw(JC(cc) | 0x018000B0u | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (uint32_t)rm); }
static void j_ldrb_off(int rt, int rn, int off) { jw(JC(CAL) | 0x05D00000u | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (uint32_t)off); }
static void j_strb_off(int rt, int rn, int off) { jw(JC(CAL) | 0x05C00000u | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (uint32_t)off); }
static void j_ldr_rl2(int rt, int rn, int rm) { jw(JC(CAL) | 0x07900100u | ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | (uint32_t)rm); }
static void j_ubfx(int rd, int rn, int lsb, int w) { jw(0xE7E00050u | ((uint32_t)(w - 1) << 16) | ((uint32_t)rd << 12) | ((uint32_t)lsb << 7) | (uint32_t)rn); }
static void j_sxth(int rd, int rm) { jw(0xE6BF0070u | ((uint32_t)rd << 12) | (uint32_t)rm); }
static void j_sxtb(int rd, int rm) { jw(0xE6AF0070u | ((uint32_t)rd << 12) | (uint32_t)rm); }
static void j_uxth(int rd, int rm) { jw(0xE6FF0070u | ((uint32_t)rd << 12) | (uint32_t)rm); }
static void j_mul(int rd, int rm, int rs) { jw(0xE0000090u | ((uint32_t)rd << 16) | ((uint32_t)rs << 8) | (uint32_t)rm); }
static void j_call(uint32_t fn) { j_mov32(JR12, fn); jw(0xE12FFF3Cu); }
static uint32_t *j_b(int cc) { uint32_t *at = s_jp; jw(JC(cc) | 0x0A000000u); return at; }

// member offsets
#define JO(m) ((int)offsetof(adsp21xx_device, m))
static int j_reg0_off(int r) // register group 0 (read_reg0 / write_reg0 order)
{
	static const int off[16] = { JO(m_core.ax0), JO(m_core.ax1), JO(m_core.mx0), JO(m_core.mx1), JO(m_core.ay0), JO(m_core.ay1),
		JO(m_core.my0), JO(m_core.my1), JO(m_core.si), JO(m_core.se), JO(m_core.ar), JO(m_core.mr.mrx.mr0), JO(m_core.mr.mrx.mr1),
		JO(m_core.mr.mrx.mr2), JO(m_core.sr.srx.sr0), JO(m_core.sr.srx.sr1) };
	return off[r & 15];
}
static int j_alux_off(int x) { static const int o[8] = { JO(m_core.ax0), JO(m_core.ax1), JO(m_core.ar), JO(m_core.mr.mrx.mr0), JO(m_core.mr.mrx.mr1), JO(m_core.mr.mrx.mr2), JO(m_core.sr.srx.sr0), JO(m_core.sr.srx.sr1) }; return o[x & 7]; }
static int j_aluy_off(int y) { static const int o[4] = { JO(m_core.ay0), JO(m_core.ay1), JO(m_core.af), -1 }; return o[y & 3]; }
static int j_macx_off(int x) { static const int o[8] = { JO(m_core.mx0), JO(m_core.mx1), JO(m_core.ar), JO(m_core.mr.mrx.mr0), JO(m_core.mr.mrx.mr1), JO(m_core.mr.mrx.mr2), JO(m_core.sr.srx.sr0), JO(m_core.sr.srx.sr1) }; return o[x & 7]; }
static int j_macy_off(int y) { static const int o[4] = { JO(m_core.my0), JO(m_core.my1), JO(m_core.mf), -1 }; return o[y & 3]; }

// read_reg0 (signed 16) into rd
static void je_rd0(int rd, int r) { j_ldrsh(rd, JR4, j_reg0_off(r)); }
// write_reg0 r = rs (low 16 bits used; MR1 also sets MR2 to its sign, SE and MR2 keep 8 bits signed)
static void je_wr0(int r, int rs)
{
	switch (r & 15) {
	case 0x09: case 0x0d:
		j_sxtb(JR12, rs);
		j_strh(JR12, JR4, j_reg0_off(r));
		break;
	case 0x0c:
		j_strh(rs, JR4, j_reg0_off(r));
		j_sxth(JR12, rs);
		j_dpr(CAL, JMOV, 0, JR12, 0, JR12, JASR, 15);
		j_strh(JR12, JR4, JO(m_core.mr.mrx.mr2));
		break;
	default:
		j_strh(rs, JR4, j_reg0_off(r));
	}
}

// DAG: r1 = the address (I before the update), I += M modulo L. ir / mr 0-7
static void je_dag(int ir, int mr)
{
	j_ldr(JR1, JR4, JO(m_i) + 4 * ir);
	j_ldr(JR2, JR4, JO(m_m) + 4 * mr);
	j_ldr(JR3, JR4, JO(m_base) + 4 * ir);
	j_ldr(JR12, JR4, JO(m_l) + 4 * ir);
	j_dpr(CAL, JADD, 0, JR2, JR1, JR2);
	j_ubfx(JR2, JR2, 0, 14);
	j_dpr(CAL, JCMP, 1, 0, JR2, JR3);
	j_dpr(CLO, JADD, 0, JR2, JR2, JR12);
	j_dpr(CHS, JADD, 0, JLR, JR3, JR12);
	j_dpr(CHS, JCMP, 1, 0, JR2, JLR);
	j_dpr(CHS, JSUB, 0, JR2, JR2, JR12);
	j_str(JR2, JR4, JO(m_i) + 4 * ir);
}
extern "C" uint32_t rax_dm_read_slow(adsp21xx_device *c, uint32_t a) { return c->data_read(a); }
extern "C" void rax_dm_write_slow(adsp21xx_device *c, uint32_t a, uint32_t v) { c->data_write(a, (uint16_t)v); }
// r0 = DM(r1), zero extended
static void je_dmrd()
{
	j_ubfx(JR1, JR1, 0, 14);
	j_dpi(CAL, JCMP, 1, 0, JR1, 0x2000);
	j_dpr(CAL, JMOV, 0, JR2, 0, JR1, JLSL, 1);
	j_ldrh_r(CLO, JR0, JR7, JR2);
	uint32_t *b1 = j_b(CLO);
	j_movw(JR3, 0x3fe0);
	j_dpr(CAL, JCMP, 1, 0, JR1, JR3);
	j_ldrh_r(CLO, JR0, JR8, JR2);
	uint32_t *b2 = j_b(CLO);
	j_mov(JR0, JR4);
	j_call((uint32_t)(uintptr_t)rax_dm_read_slow);
	j_patch(b1, s_jp);
	j_patch(b2, s_jp);
}
// DM(r1) = r0 (low 16 bits); a control register write may end the slice (m_icount = 0)
static void je_dmwr()
{
	j_ubfx(JR1, JR1, 0, 14);
	j_dpi(CAL, JCMP, 1, 0, JR1, 0x2000);
	j_dpr(CAL, JMOV, 0, JR2, 0, JR1, JLSL, 1);
	j_strh_r(CLO, JR0, JR7, JR2);
	uint32_t *b1 = j_b(CLO);
	j_movw(JR3, 0x3fe0);
	j_dpr(CAL, JCMP, 1, 0, JR1, JR3);
	j_strh_r(CLO, JR0, JR8, JR2);
	uint32_t *b2 = j_b(CLO);
	j_mov(JR2, JR0);
	j_mov(JR0, JR4);
	j_str(JR5, JR4, JO(m_icount));
	j_call((uint32_t)(uintptr_t)rax_dm_write_slow);
	j_ldr(JR5, JR4, JO(m_icount));
	j_patch(b1, s_jp);
	j_patch(b2, s_jp);
}
// PM(r1) = (r0 << 8) | PX (24 bits); r0 = 1 if a block was compiled over that address (the block then leaves)
extern "C" uint32_t rax_pm_write(adsp21xx_device *c, uint32_t a, uint32_t v)
{
	a &= 0x3fff;
	c->program_write(a, (v << 8) | c->m_px);
	return s_jcov[a];
}
// condition 14 (CE): counts CNTR down (pops the counter stack when it runs out)
extern "C" uint32_t rax_cond_ce(adsp21xx_device *c) { return (uint32_t)c->slow_condition(); }
// r0 = PM(r1) >> 8, PX = low 8 bits
static void je_pmrd()
{
	j_ubfx(JR1, JR1, 0, 14);
	j_ldr_rl2(JR0, JR11, JR1);
	j_strb_off(JR0, JR4, JO(m_px));
	j_dpr(CAL, JMOV, 0, JR0, 0, JR0, JLSR, 8);
}

// condition c true -> fall through; false -> the returned branch (patched to the skip point)
static uint32_t *je_cond(int c)
{
	if (c == 14) {
		j_mov(JR0, JR4);
		j_call((uint32_t)(uintptr_t)rax_cond_ce);
		j_dpi(CAL, JCMP, 1, 0, JR0, 0);
		return j_b(CEQ);
	}
	j_ldr(JR0, JR4, JO(m_astat));
	j_dpr(CAL, JADD, 0, JR0, JR9, JR0);
	j_ldrb_off(JR0, JR0, c << 8);
	j_dpi(CAL, JCMP, 1, 0, JR0, 0);
	return j_b(CEQ);
}

static const int32_t j_constants[] = { 0x0001, 0xfffe, 0x0002, 0xfffd, 0x0004, 0xfffb, 0x0008, 0xfff7, 0x0010, 0xffef, 0x0020, 0xffdf,
	0x0040, 0xffbf, 0x0080, 0xff7f, 0x0100, 0xfeff, 0x0200, 0xfdff, 0x0400, 0xfbff, 0x0800, 0xf7ff, 0x1000, 0xefff, 0x2000, 0xdfff,
	0x4000, 0xbfff, 0x8000, 0x7fff };

// ALU functions done here (alu_op_ar / alu_op_af and the constant forms)
static int je_alu_ok(int amf) { return amf == 0 || amf == 1 || amf == 3 || amf == 7 || amf == 8 || amf == 9 || amf == 12 || amf == 13 || amf == 14; }
// ALU op: to AR (af = 0) or AF; cst >= 0: constant y (alu_op_*_const); flags: compute ASTAT
static void je_alu(uint32_t op, int af, int cst, int flags, int store = 1)
{
	const int sat = store && !af && (s_jmstat & 0x08); // AR saturation: needs V and C now
	if (sat)
		flags = 1;
	const int amf = BIT(op, 13, 4), xo = BIT(op, 8, 3), yo = BIT(op, 11, 2);
	const int needx = amf == 3 || amf == 7 || amf == 9 || amf >= 12;
	if (needx) j_ldrh(JR1, JR4, j_alux_off(xo));          // x (r1)
	if (cst >= 0) j_mov32(JR2, (uint32_t)cst);              // y (r2)
	else if (j_aluy_off(yo) < 0) j_movw(JR2, 0);
	else j_ldrh(JR2, JR4, j_aluy_off(yo));
	int sub = 0, v_c = 0; // v_c: 1 NZVC add (x, y), 2 NZVC sub (s, d), 3 special +1/-1
	switch (amf) {
	case 0: j_mov(JR0, JR2); break;
	case 1: j_dpi(CAL, JADD, 0, JR0, JR2, 1); v_c = 3; break;
	case 3: j_dpr(CAL, JADD, 0, JR0, JR1, JR2); v_c = 1; break;
	case 7: j_dpr(CAL, JSUB, 0, JR0, JR1, JR2); v_c = 2; sub = 0; break;
	case 8: j_dpi(CAL, JSUB, 0, JR0, JR2, 1); v_c = 3; break;
	case 9: j_dpr(CAL, JSUB, 0, JR0, JR2, JR1); v_c = 2; sub = 1; break;
	case 12: j_dpr(CAL, JAND, 0, JR0, JR1, JR2); break;
	case 13: j_dpr(CAL, JORR, 0, JR0, JR1, JR2); break;
	case 14: j_dpr(CAL, JEOR, 0, JR0, JR1, JR2); break;
	}
	if (flags) {
		const uint32_t clr = (s_jmstat & 0x04) ? ~(uint32_t)(0x08 | 0x02 | 0x01) : ~(uint32_t)(0x08 | 0x04 | 0x02 | 0x01);
		j_ldr(JR3, JR4, JO(m_astat));
		j_dpi(CAL, JBIC, 0, JR3, JR3, ~clr & 0xff);
		// N: bit 15, Z: low 16 bits zero
		j_dpr(CAL, JMOV, 0, JR12, 0, JR0, JASR, 14);
		j_dpi(CAL, JAND, 0, JR12, JR12, 2);
		j_dpr(CAL, JORR, 0, JR3, JR3, JR12);
		j_dpr(CAL, JMOV, 1, JR12, 0, JR0, JLSL, 16);
		j_dpi(CEQ, JORR, 0, JR3, JR3, 1);
		if (v_c == 1 || v_c == 2) {
			// V: bit 15 of (s ^ d ^ r ^ (r >> 1)); add: s = x, d = y; sub (X-Y): s = x, d = y; (Y-X): s = y, d = x
			j_dpr(CAL, JEOR, 0, JR12, JR1, JR2);
			j_dpr(CAL, JEOR, 0, JR12, JR12, JR0);
			j_dpr(CAL, JEOR, 0, JR12, JR12, JR0, JASR, 1);
			j_dpr(CAL, JMOV, 0, JR12, 0, JR12, JASR, 13);
			j_dpi(CAL, JAND, 0, JR12, JR12, 4);
			j_dpr(CAL, JORR, 0, JR3, JR3, JR12);
			// C: bit 16 of r (add) or of ~r (sub)
			if (v_c == 1) j_dpr(CAL, JMOV, 0, JR12, 0, JR0, JASR, 13);
			else j_dpr(CAL, JMVN, 0, JR12, 0, JR0, JASR, 13);
			j_dpi(CAL, JAND, 0, JR12, JR12, 8);
			j_dpr(CAL, JORR, 0, JR3, JR3, JR12);
		} else if (v_c == 3) {
			// Y + 1: V if y == 7fff, else C if y == ffff; Y - 1: V if y == 8000, else C if y == 0
			// (the two cases cannot both hold, so two separate compares)
			j_movw(JR12, amf == 1 ? 0x7fff : 0x8000);
			j_dpr(CAL, JCMP, 1, 0, JR2, JR12);
			j_dpi(CEQ, JORR, 0, JR3, JR3, 4);
			j_movw(JR12, amf == 1 ? 0xffff : 0);
			j_dpr(CAL, JCMP, 1, 0, JR2, JR12);
			j_dpi(CEQ, JORR, 0, JR3, JR3, 8);
		}
		j_str(JR3, JR4, JO(m_astat));
		if (sat) { // if V: AR = C ? -32768 : 32767
			j_dpi(CAL, JTST, 1, 0, JR3, 4);
			uint32_t *nv = j_b(CEQ);
			j_dpi(CAL, JTST, 1, 0, JR3, 8);
			jw(JC(CEQ) | 0x03000000u | ((0x7fffu >> 12) << 16) | (0u << 12) | (0x7fffu & 0xfff)); // movweq r0, #0x7fff
			jw(JC(CNE) | 0x03000000u | ((0x8000u >> 12) << 16) | (0u << 12) | (0x8000u & 0xfff)); // movwne r0, #0x8000
			j_patch(nv, s_jp);
		}
	}
	(void)sub;
	if (store)
		j_strh(JR0, JR4, af ? JO(m_core.af) : JO(m_core.ar));
}

// MAC op to MR (mf = 0, sets MV when flags) or MF; amf 0 = no-op
static void je_mac(uint32_t op, int mf, int flags)
{
	const int amf = BIT(op, 13, 4), xo = BIT(op, 8, 3), yo = BIT(op, 11, 2);
	if (amf == 0)
		return;
	int xs, ys, base, rnd = 0; // base 0: none, 1: MR +, 2: MR -
	if (amf <= 3) { xs = ys = 1; base = amf == 1 ? 0 : amf == 2 ? 1 : 2; rnd = 1; }
	else { const int m = amf & 3; xs = !(m & 2); ys = !(m & 1); base = amf < 8 ? 0 : amf < 12 ? 1 : 2; }
	if (xs) j_ldrsh(JR0, JR4, j_macx_off(xo)); else j_ldrh(JR0, JR4, j_macx_off(xo));
	if (j_macy_off(yo) < 0) j_movw(JR1, 0);
	else if (ys) j_ldrsh(JR1, JR4, j_macy_off(yo));
	else j_ldrh(JR1, JR4, j_macy_off(yo));
	j_mul(JR2, JR0, JR1);                                   // temp = x * y (32 bits)
	if (!(s_jmstat & 0x10))
		j_dpr(CAL, JMOV, 0, JR2, 0, JR2, JLSL, 1);          // fractional mode
	if (base == 0) {
		j_mov(JR0, JR2);
		j_dpr(CAL, JMOV, 0, JR1, 0, JR2, JASR, 31);
	} else {
		j_ldrd(JR0, JR4, JO(m_core.mr));
		if (base == 1) { j_dpr(CAL, JADD, 1, JR0, JR0, JR2); j_dpr(CAL, JADC, 0, JR1, JR1, JR2, JASR, 31); }
		else { j_dpr(CAL, JSUB, 1, JR0, JR0, JR2); j_dpr(CAL, JSBC, 0, JR1, JR1, JR2, JASR, 31); }
	}
	if (rnd) {
		j_dpi(CAL, JADD, 1, JR0, JR0, 0x8000);
		j_dpi(CAL, JADC, 0, JR1, JR1, 0);
		j_uxth(JR3, JR2);
		j_dpi(CAL, JCMP, 1, 0, JR3, 0x8000);
		j_dpi(CEQ, JBIC, 0, JR0, JR0, 0x10000);
	}
	if (mf) {
		j_dpr(CAL, JMOV, 0, JR0, 0, JR0, JLSR, 16);
		j_strh(JR0, JR4, JO(m_core.mf));
		return;
	}
	j_strd(JR0, JR4, JO(m_core.mr));                       // all 64 bits, as MAME
	if (flags) {
		// MV unless bits 31-39 are all 0 or all 1
		j_dpr(CAL, JMOV, 0, JR2, 0, JR1, JLSL, 24);
		j_dpr(CAL, JORR, 0, JR2, JR2, JR0, JLSR, 8);
		j_dpr(CAL, JMOV, 0, JR2, 0, JR2, JASR, 23);
		j_dpi(CAL, JADD, 0, JR2, JR2, 1);
		j_ldr(JR3, JR4, JO(m_astat));
		j_dpi(CAL, JBIC, 0, JR3, JR3, 0x40);
		j_dpi(CAL, JCMP, 1, 0, JR2, 1);
		j_dpi(CHI, JORR, 0, JR3, JR3, 0x40);
		j_str(JR3, JR4, JO(m_astat));
	}
}

// what an instruction does to the flags: kinds for the liveness scan
enum { JF_NONE = 0, JF_W_NZVC = 1, JF_W_MV = 2, JF_R_ALL = 4 };
static int je_cond_reads(int c) { return c != 15; }
static void j_sbfx(int rd, int rn, int lsb, int w) { jw(0xE7A00050u | ((uint32_t)(w - 1) << 16) | ((uint32_t)rd << 12) | ((uint32_t)lsb << 7) | (uint32_t)rn); }
static int j_shx_off(int x) { static const int o[8] = { JO(m_core.si), JO(m_core.si), JO(m_core.ar), JO(m_core.mr.mrx.mr0), JO(m_core.mr.mrx.mr1), JO(m_core.mr.mrx.mr2), JO(m_core.sr.srx.sr0), JO(m_core.sr.srx.sr1) }; return o[x & 7]; }
// shift_op_imm, modes 0-7 (LSHIFT / ASHIFT, HI / LO, OR); NORM stays with the handler
static void je_shift_imm(uint32_t op)
{
	const int mode = BIT(op, 11, 4), x = BIT(op, 8, 3), sc = (int8_t)op;
	const int arith = mode >= 4, hi = !(mode & 2), orr = mode & 1;
	if (arith) j_ldrsh(JR0, JR4, j_shx_off(x)); else j_ldrh(JR0, JR4, j_shx_off(x));
	if (hi) j_dpr(CAL, JMOV, 0, JR0, 0, JR0, JLSL, 16);
	if (sc > 0) {
		if (sc < 32) j_dpr(CAL, JMOV, 0, JR0, 0, JR0, JLSL, sc);
		else j_movw(JR0, 0);
	} else if (sc < 0) {
		const int r = -sc;
		if (r < 32) j_dpr(CAL, JMOV, 0, JR0, 0, JR0, arith ? JASR : JLSR, r);
		else if (arith) j_dpr(CAL, JMOV, 0, JR0, 0, JR0, JASR, 31);
		else j_movw(JR0, 0);
	}
	if (orr) {
		j_ldr(JR1, JR4, JO(m_core.sr));
		j_dpr(CAL, JORR, 0, JR0, JR0, JR1);
	}
	j_str(JR0, JR4, JO(m_core.sr));
}
// shift_op modes 0-7: as shift_op_imm with sc = (int8)SE; register shifts give 0 (LSL / LSR) or the sign (ASR) from 32
static void je_shift_se(uint32_t op)
{
	const int mode = BIT(op, 11, 4), x = BIT(op, 8, 3);
	const int arith = mode >= 4, hi = !(mode & 2), orr = mode & 1;
	if (arith) j_ldrsh(JR0, JR4, j_shx_off(x)); else j_ldrh(JR0, JR4, j_shx_off(x));
	if (hi) j_dpr(CAL, JMOV, 0, JR0, 0, JR0, JLSL, 16);
	j_ldrh(JR1, JR4, JO(m_core.se));
	j_sxtb(JR1, JR1);
	j_dpi(CAL, JCMP, 1, 0, JR1, 0);
	jw(JC(CGT) | 0x01A00010u | (0u << 12) | (1u << 8) | ((uint32_t)JLSL << 5) | 0u); // movgt r0, r0, lsl r1
	j_dpi(CLE, JRSB, 0, JR1, JR1, 0);                                                // rsble r1, r1, #0
	jw(JC(CLE) | 0x01A00010u | (0u << 12) | (1u << 8) | ((uint32_t)(arith ? JASR : JLSR) << 5) | 0u); // movle r0, r0, lsr/asr r1
	if (orr) {
		j_ldr(JR1, JR4, JO(m_core.sr));
		j_dpr(CAL, JORR, 0, JR0, JR0, JR1);
	}
	j_str(JR0, JR4, JO(m_core.sr));
}
// DAG1 data access in bit-reverse mode: r1 = reversed address
static void je_dag1(int ir, int mr)
{
	je_dag(ir, mr);
	if (s_jmstat & 0x02) {
		j_ubfx(JR1, JR1, 0, 14);
		j_dpr(CAL, JMOV, 0, JR2, 0, JR1, JLSL, 1);
		j_dpr(CAL, JADD, 0, JR2, JR2, JR9);
		j_dpi(CAL, JADD, 0, JR2, JR2, 0x9000);       // m_reverse_table follows m_mask_table (0x1000 + 0x8000)
		j_ldrh(JR1, JR2, 0);
	}
}
// register groups: 0 data, 1 / 2 I M L (index 0-3 / 4-7); group 1 0xC-0xF (overlays) and group 3 stay with handlers
static int je_reg_ok(int g, int r) { return g == 0 || ((g == 1 || g == 2) && r < 12); }
// rd = read_reg<g>(r)
static void je_rdg(int rd, int g, int r)
{
	if (g == 0) { je_rd0(rd, r); return; }
	const int idx = (g == 2 ? 4 : 0) + (r & 3);
	j_ldr(rd, JR4, (r >> 2) == 0 ? JO(m_i) + 4 * idx : (r >> 2) == 1 ? JO(m_m) + 4 * idx : JO(m_l) + 4 * idx);
}
// write_reg<g>(r, rs): I = v & 3FFF (base again), M = 14-bit signed, L = v & 3FFF (mask and base again)
static void je_wrg(int g, int r, int rs)
{
	if (g == 0) { je_wr0(r, rs); return; }
	const int idx = (g == 2 ? 4 : 0) + (r & 3);
	switch (r >> 2) {
	case 0:
		j_ubfx(JR2, rs, 0, 14);
		j_str(JR2, JR4, JO(m_i) + 4 * idx);
		j_ldr(JR3, JR4, JO(m_lmask) + 4 * idx);
		j_dpr(CAL, JAND, 0, JR3, JR2, JR3);
		j_str(JR3, JR4, JO(m_base) + 4 * idx);
		break;
	case 1:
		j_sbfx(JR2, rs, 0, 14);
		j_str(JR2, JR4, JO(m_m) + 4 * idx);
		break;
	case 2:
		j_ubfx(JR2, rs, 0, 14);
		j_str(JR2, JR4, JO(m_l) + 4 * idx);
		j_dpr(CAL, JMOV, 0, JR3, 0, JR2, JLSL, 1);
		j_dpr(CAL, JADD, 0, JR3, JR3, JR9);
		j_dpi(CAL, JADD, 0, JR3, JR3, 0x1000);              // m_mask_table follows m_condition_table
		j_ldrh(JR3, JR3, 0);
		j_str(JR3, JR4, JO(m_lmask) + 4 * idx);
		j_ldr(JR2, JR4, JO(m_i) + 4 * idx);
		j_dpr(CAL, JAND, 0, JR2, JR2, JR3);
		j_str(JR2, JR4, JO(m_base) + 4 * idx);
		break;
	}
}

// the inline form of an instruction; 0 = not handled here
static int je_kind(uint32_t op)
{
	const uint32_t t = op >> 16;
	const int amf = BIT(op, 13, 4);
	if (t == 0x00 || t == 0x09 || (t >= 0x40 && t <= 0x4f) || (t >= 0x80 && t <= 0x83) || (t >= 0x90 && t <= 0x93))
		return 1;
	if (t >= 0xa0 && t <= 0xbf) return 1;
	if (t == 0x0d) { const int sel = BIT(op, 8, 4); return je_reg_ok(sel >> 2, BIT(op, 4, 4)) && je_reg_ok(sel & 3, BIT(op, 0, 4)); }
	if (t == 0x0f) return BIT(op, 11, 4) < 8;
	if (t == 0x0e || t == 0x10 || t == 0x12 || t == 0x13) return BIT(op, 11, 4) < 8;
	if (t >= 0x30 && t <= 0x3b) return je_reg_ok((t >> 2) & 3, BIT(op, 0, 4));
	if (t >= 0x18 && t <= 0x1b) return 1;
	if (t == 0x05) return 1;
	if (t >= 0x84 && t <= 0x8b) return je_reg_ok((t >> 2) & 3, BIT(op, 0, 4));
	if (t >= 0x94 && t <= 0x9b) return je_reg_ok((t >> 2) & 3, BIT(op, 0, 4));
	auto aluok = [&](int alu) { return !alu || je_alu_ok(amf); };
	if (t >= 0x60 && t <= 0x7f) { const int fn = (t >> 1) & 3; return aluok(fn & 1); }
	if (t >= 0x50 && t <= 0x5f) { const int fn = (t >> 1) & 3; return aluok(fn & 1); }
	if (t >= 0xc0) return aluok(BIT(op, 17));
	if (t >= 0x20 && t <= 0x27) {
		const int fn = (t >> 1) & 3;
		if (!(fn & 1) && (op & 0x0018f0) == 0x000010) return 0; // xop forms
		return aluok(fn & 1);
	}
	if (t >= 0x28 && t <= 0x2f) {
		const int fn = (t >> 1) & 3;
		return aluok(fn & 1);
	}
	return 0;
}
// flags effect for the liveness scan (inline forms only; handlers count as reading everything)
static int je_flags(uint32_t op)
{
	const uint32_t t = op >> 16;
	if (!je_kind(op)) return JF_R_ALL;
	int f = 0;
	int fn = -1; // 0 MAC MR, 1 ALU AR, 2 MAC MF, 3 ALU AF
	if (t == 0x05) return JF_R_ALL;
	if ((t >= 0x60 && t <= 0x7f) || (t >= 0x50 && t <= 0x5f) || (t >= 0x20 && t <= 0x2f)) fn = (t >> 1) & 3;
	else if (t >= 0xc0) fn = BIT(op, 17) ? 1 : 0;
	if ((t >= 0x18 && t <= 0x1b) || (t >= 0x20 && t <= 0x27) || t == 0x0e) if (je_cond_reads(op & 15)) f |= JF_R_ALL;
	if (fn == 1 || fn == 3) f |= JF_W_NZVC;
	if (fn == 0 && BIT(op, 13, 4) != 0) f |= JF_W_MV;
	return f;
}

// one instruction (inline); flags_nzvc / flags_mv: its flags are read later
static void je_inst(uint32_t op, uint32_t pc, int fl_nzvc, int fl_mv)
{
	const uint32_t t = op >> 16;
	if (t == 0x00) return;
	if (t == 0x09) { // modify address
		const int ir = BIT(op, 2, 3);
		je_dag(ir, (ir & 4) | (op & 3));
		return;
	}
	if (t >= 0x40 && t <= 0x4f) { j_movw(JR0, BIT(op, 4, 16)); je_wr0(BIT(op, 0, 4), JR0); return; }
	if (t >= 0x80 && t <= 0x83) { j_movw(JR1, BIT(op, 4, 14)); je_dmrd(); je_wr0(BIT(op, 0, 4), JR0); return; }
	if (t >= 0x90 && t <= 0x93) { je_rd0(JR0, BIT(op, 0, 4)); j_movw(JR1, BIT(op, 4, 14)); je_dmwr(); return; }
	if (t >= 0xa0 && t <= 0xbf) { // DM(I, M) = immediate, DAG1 / DAG2
		const int d2 = t >= 0xb0;
		if (d2) je_dag(4 + BIT(op, 2, 2), 4 + BIT(op, 0, 2));
		else je_dag1(BIT(op, 2, 2), BIT(op, 0, 2));
		j_movw(JR0, BIT(op, 4, 16));
		je_dmwr();
		return;
	}
	if (t == 0x0d) { const int sel = BIT(op, 8, 4); je_rdg(JR0, sel & 3, BIT(op, 0, 4)); je_wrg(sel >> 2, BIT(op, 4, 4), JR0); return; }
	if (t == 0x0f) { je_shift_imm(op); return; }
	if (t == 0x0e) {
		uint32_t *skip = (op & 15) != 15 ? je_cond(op & 15) : NULL;
		je_shift_se(op);
		if (skip) j_patch(skip, s_jp);
		return;
	}
	if (t == 0x10) { je_shift_se(op); je_rd0(JR0, BIT(op, 0, 4)); je_wr0(BIT(op, 4, 4), JR0); return; }
	if (t == 0x12 || t == 0x13) {
		auto dag = [&]() { if (t == 0x13) je_dag(4 + BIT(op, 2, 2), 4 + BIT(op, 0, 2)); else je_dag1(BIT(op, 2, 2), BIT(op, 0, 2)); };
		if (BIT(op, 15)) { dag(); je_rd0(JR0, BIT(op, 4, 4)); je_dmwr(); je_shift_se(op); }
		else { je_shift_se(op); dag(); je_dmrd(); je_wr0(BIT(op, 4, 4), JR0); }
		return;
	}
	if (t == 0x05) { // IF MV SAT MR
		j_ldr(JR0, JR4, JO(m_astat));
		j_dpi(CAL, JTST, 1, 0, JR0, 0x40);
		uint32_t *nmv = j_b(CEQ);
		j_ldrh(JR1, JR4, JO(m_core.mr.mrx.mr2));
		j_dpi(CAL, JTST, 1, 0, JR1, 0x80);
		j_movw(JR1, 0xffff); j_movw(JR2, 0x8000); j_movw(JR3, 0x0000);  // negative: FFFF 8000 0000
		uint32_t *neg = j_b(CNE);
		j_movw(JR1, 0x0000); j_movw(JR2, 0x7fff); j_movw(JR3, 0xffff);  // positive: 0000 7FFF FFFF
		j_patch(neg, s_jp);
		j_strh(JR1, JR4, JO(m_core.mr.mrx.mr2));
		j_strh(JR2, JR4, JO(m_core.mr.mrx.mr1));
		j_strh(JR3, JR4, JO(m_core.mr.mrx.mr0));
		j_patch(nmv, s_jp);
		return;
	}
	if (t >= 0x84 && t <= 0x8b) { j_movw(JR1, BIT(op, 4, 14)); je_dmrd(); je_wrg((t >> 2) & 3, BIT(op, 0, 4), JR0); return; }
	if (t >= 0x94 && t <= 0x9b) { je_rdg(JR0, (t >> 2) & 3, BIT(op, 0, 4)); j_movw(JR1, BIT(op, 4, 14)); je_dmwr(); return; }
	if (t >= 0x30 && t <= 0x3b) { j_mov32(JR0, (uint32_t)util::sext(op >> 4, 14)); je_wrg((t >> 2) & 3, BIT(op, 0, 4), JR0); return; }
	if (t >= 0x18 && t <= 0x1b) { // conditional jump: m_pc = target or the next address
		const uint32_t target = BIT(op, 4, 14);
		uint32_t *skip = (op & 15) != 15 ? je_cond(op & 15) : NULL;
		j_movw(JR0, target);
		j_str(JR0, JR4, JO(m_pc));
		if (target == pc) j_movw(JR5, 0); // busy loop: the slice ends (MAME m_icount = 0)
		if (skip) {
			uint32_t *done = j_b(CAL);
			j_patch(skip, s_jp);
			j_movw(JR0, (pc + 1) & 0xffff);
			j_str(JR0, JR4, JO(m_pc));
			j_patch(done, s_jp);
		}
		return;
	}
	auto op_fn = [&](int fn, int cst) {
		if (fn == 0) je_mac(op, 0, fl_mv);
		else if (fn == 1) je_alu(op, 0, cst, fl_nzvc);
		else if (fn == 2) je_mac(op, 1, 0);
		else je_alu(op, 1, cst, fl_nzvc);
	};
	if (t >= 0x60 && t <= 0x7f) {
		const int fn = (t >> 1) & 3, d2 = t >= 0x70, wr = (t >> 3) & 1;
		const int ir = (d2 ? 4 : 0) + BIT(op, 2, 2), mr = (d2 ? 4 : 0) + BIT(op, 0, 2);
		auto dag = [&]() { if (d2) je_dag(ir, mr); else je_dag1(ir, mr); };
		if (wr) {
			dag();
			je_rd0(JR0, BIT(op, 4, 4));
			je_dmwr();
			op_fn(fn, -1);
		} else {
			op_fn(fn, -1);
			dag();
			je_dmrd();
			je_wr0(BIT(op, 4, 4), JR0);
		}
		return;
	}
	if (t >= 0x50 && t <= 0x57) {
		op_fn((t >> 1) & 3, -1);
		je_dag(4 + BIT(op, 2, 2), 4 + BIT(op, 0, 2));
		je_pmrd();
		je_wr0(BIT(op, 4, 4), JR0);
		return;
	}
	if (t >= 0x58 && t <= 0x5f) { // PM(I, M) = reg, then the op; [sp] = 1 when the write hit compiled code
		je_dag(4 + BIT(op, 2, 2), 4 + BIT(op, 0, 2));
		je_rd0(JR2, BIT(op, 4, 4));
		j_mov(JR0, JR4);
		j_call((uint32_t)(uintptr_t)rax_pm_write);
		j_str(JR0, JSP, 0);
		op_fn((t >> 1) & 3, -1);
		return;
	}
	if (t >= 0xc0) {
		op_fn(BIT(op, 17) ? 1 : 0, -1);
		je_dag1(BIT(op, 2, 2), BIT(op, 0, 2));
		je_dmrd();
		static const int xr[4] = { 0x00, 0x01, 0x02, 0x03 }; // AX0, AX1, MX0, MX1 (group 0 numbers)
		j_strh(JR0, JR4, j_reg0_off(xr[BIT(op, 18, 2)]));
		const uint32_t o4 = op >> 4;
		je_dag(4 + BIT(o4, 2, 2), 4 + BIT(o4, 0, 2));
		je_pmrd();
		static const int yr[4] = { 0x04, 0x05, 0x06, 0x07 }; // AY0, AY1, MY0, MY1
		j_strh(JR0, JR4, j_reg0_off(yr[BIT(op, 20, 2)]));
		return;
	}
	if (t >= 0x20 && t <= 0x27) {
		const int fn = (t >> 1) & 3;
		uint32_t *skip = (op & 15) != 15 ? je_cond(op & 15) : NULL;
		const int cst = (fn & 1) && BIT(op, 4) ? j_constants[BIT(op, 5, 3) | BIT(op, 11, 2) << 3] : -1;
		op_fn(fn, cst);
		if (skip) j_patch(skip, s_jp);
		return;
	}
	if ((t == 0x2a || t == 0x2b) && BIT(op, 0, 8) == 0xaa) { // NONE = ALU op: flags only
		je_alu(op, 0, -1, 1, 0);
		return;
	}
	if (t >= 0x28 && t <= 0x2f) {
		je_rd0(JR0, BIT(op, 0, 4));
		j_str(JR0, JSP, 0);      // the moved value waits on the stack (the op uses r0-r3)
		op_fn((t >> 1) & 3, -1);
		j_ldr(JR0, JSP, 0);
		je_wr0(BIT(op, 4, 4), JR0);
		return;
	}
}

extern "C" void rax_loop_end(adsp21xx_device *c, uint32_t a);
// compiles the block at pc0 into [s_jp, s_jend); returns its length in words (0: did not fit).
// Entry checks (else the block leaves at once): the MSTAT mode bits it was made for (m_jn = -1: run() drops it and
// interprets); at least as many cycles left as it has instructions (m_jn = -2: run() interprets this one). At each
// address a DO names as a loop end the block leaves when it is the current one (run() takes the loop end). Inside, only the instructions that may
// end the slice (handlers, data memory writes: a BDMA start) check m_icount; the flags an instruction would set are
// only computed when they can be seen: read later in the block, or at a place the block may leave.
static int raxj_compile(uint32_t pc0)
{
	uint32_t *start = s_jp;
	struct Exit { uint32_t *at; int kind; uint32_t pc; int jn; }; // kind 0: m_pc = pc, 1: m_pc already set
	Exit ex[RAXJ_MAX * 8 + 8];
	int nex = 0;
	s_jfull = 0;
	s_jfb = 0;
	s_jmstat = s_cpu.m_mstat & JMS;
	uint32_t ops[RAXJ_MAX];
	int n = 0;
	for (uint32_t pc = pc0; n < RAXJ_MAX && pc < 0x4000; pc++) {
		s_jcov[pc] = 1; // a program write here drops the blocks
		ops[n++] = s_pram[pc];
		if (raxj_ender(s_pram[pc]))
			break;
	}
	auto inl = [&](uint32_t op) -> int {
		return je_kind(op);
	};
	auto exitpt = [&](uint32_t op) -> int { // the slice may end after it
		const uint32_t t = op >> 16;
		if (!inl(op)) return 1;
		return (t >= 0x90 && t <= 0x9b) || (t >= 0xa0 && t <= 0xbf) || (t >= 0x68 && t <= 0x6f) || (t >= 0x78 && t <= 0x7f) ||
			(t >= 0x18 && t <= 0x1b) || (t >= 0x58 && t <= 0x5f) || ((t == 0x12 || t == 0x13) && BIT(op, 15));
	};
	int need_nzvc[RAXJ_MAX], need_mv[RAXJ_MAX];
	{
		int live_nzvc = 1, live_mv = 1; // after the block: assumed read
		for (int k = n - 1; k >= 0; k--) {
			if (exitpt(ops[k]) || (k > 0 && s_loopend[(pc0 + k) & 0x3fff])) live_nzvc = live_mv = 1;
			const int f = inl(ops[k]) ? je_flags(ops[k]) : JF_R_ALL;
			need_nzvc[k] = live_nzvc;
			need_mv[k] = live_mv;
			if (f & JF_W_NZVC) live_nzvc = 0;
			if (f & JF_W_MV) live_mv = 0;
			if (f & JF_R_ALL) live_nzvc = live_mv = 1;
			if (k > 0 && s_loopend[(pc0 + k) & 0x3fff]) live_nzvc = live_mv = 1; // may leave before k (loop end)
		}
	}
	jw(0xE92D4FF0u);                 // push {r4-r11, lr}
	jw(0xE24DD004u);                 // sub sp, sp, #4 (8-byte alignment; also a slot for 28-2F)
	j_mov(JR4, JR0);
	j_ldr(JR0, JR4, JO(m_mstat));
	j_dpi(CAL, JAND, 0, JR0, JR0, JMS);
	j_dpi(CAL, JCMP, 1, 0, JR0, s_jmstat);
	uint32_t *b_mode = j_b(CNE);
	j_ldr(JR6, JR4, JO(m_loop));
	j_ldr(JR5, JR4, JO(m_icount));
	j_mov32(JR0, (uint32_t)n);
	j_dpr(CAL, JCMP, 1, 0, JR5, JR0);
	uint32_t *b_short = j_b(CLT);
	j_mov32(JR7, (uint32_t)(uintptr_t)&s_dbank); j_ldr(JR7, JR7, 0);
	j_mov32(JR8, (uint32_t)(uintptr_t)s_iram - 0x4000);
	j_mov32(JR9, (uint32_t)(uintptr_t)&s_cpu.m_condition_table[0]);
	j_mov32(JR11, (uint32_t)(uintptr_t)s_pram);
	j_movw(JR10, 0);
	uint32_t *top = s_jp;            // back edge target (a DO loop over this block)
	uint32_t *to_next = NULL;        // loop end path going on with the next instruction
	for (int k = 0; k < n; k++) {
		const uint32_t pc = pc0 + (uint32_t)k, op = ops[k];
		const int ender = raxj_ender(op);
		if (to_next) { j_patch(to_next, s_jp); to_next = NULL; }
		// the instruction itself; looppath: m_pc was set by the loop end step (a handler must not set it again)
		auto body = [&](int looppath) -> int {
			if (inl(op)) {
				je_inst(op, pc, need_nzvc[k], need_mv[k]);
				return (op >> 16) >= 0x18 && (op >> 16) <= 0x1b ? 1 : 0;
			}
			s_jfb++;
			j_mov32(JR0, (uint32_t)(uintptr_t)&rstv_rax_jit_fb); // diag count
			j_ldr(JR1, JR0, 0);
			j_dpi(CAL, JADD, 0, JR1, JR1, 1);
			j_str(JR1, JR0, 0);
#if !defined(__vita__)
			j_mov32(JR0, op);
			j_call((uint32_t)(uintptr_t)rax_jfb_count);
#endif
			if (!looppath) {
				j_movw(JR0, (pc + 1) & 0xFFFF);
				j_str(JR0, JR4, JO(m_pc));
			}
			if (BIT(op, 16, 8) >= 0x18 && BIT(op, 16, 8) <= 0x1b) {
				j_movw(JR0, pc);
				j_str(JR0, JR4, JO(m_ppc));
			}
			j_str(JR5, JR4, JO(m_icount));
			j_mov(JR0, JR4);
			j_mov32(JR1, op);
			j_call((uint32_t)(uintptr_t)s_ins[pc].fn);
			j_ldr(JR5, JR4, JO(m_icount));
			j_mov32(JR7, (uint32_t)(uintptr_t)&s_dbank); j_ldr(JR7, JR7, 0);
			return 1;
		};
		uint32_t *b_normal = NULL;
		if (k > 0 && s_loopend[pc]) {
			// at the loop end: loop step, the instruction, then the loop top (this block: go round), a + 1 or elsewhere
			j_movw(JR0, pc);
			j_dpr(CAL, JCMP, 1, 0, JR6, JR0);
			b_normal = j_b(CNE);
			j_mov(JR0, JR4);
			j_movw(JR1, pc);
			j_call((uint32_t)(uintptr_t)rax_loop_end);
			j_ldr(JR6, JR4, JO(m_loop));
			const int lk = body(1);
			(void)lk;
			j_dpi(CAL, JSUB, 1, JR5, JR5, 1);
			if (inl(op) && (op >> 16) >= 0x58 && (op >> 16) <= 0x5f) { // the write hit compiled code: leave (m_pc set)
				j_ldr(JR0, JSP, 0);
				j_dpi(CAL, JCMP, 1, 0, JR0, 0);
				ex[nex].at = j_b(CNE); ex[nex].kind = 1; ex[nex].pc = 0; ex[nex].jn = k + 1; nex++;
			}
			ex[nex].at = j_b(CLE); ex[nex].kind = 1; ex[nex].pc = 0; ex[nex].jn = k + 1; nex++;
			if (ender) {
				ex[nex].at = j_b(CAL); ex[nex].kind = 1; ex[nex].pc = 0; ex[nex].jn = k + 1; nex++;
			} else {
				j_ldr(JR0, JR4, JO(m_pc));
				j_mov32(JR1, pc0);
				j_dpr(CAL, JCMP, 1, 0, JR0, JR1);
				uint32_t *b_notback = j_b(CNE);
				j_mov32(JR1, (uint32_t)n);
				j_dpr(CAL, JCMP, 1, 0, JR5, JR1);
				ex[nex].at = j_b(CLT); ex[nex].kind = 1; ex[nex].pc = 0; ex[nex].jn = k + 1; nex++;
				j_dpi(CAL, JADD, 0, JR10, JR10, (uint32_t)(k + 1));
				jw(JC(CAL) | 0x0A000000u | ((uint32_t)(top - (s_jp + 2)) & 0x00FFFFFFu)); // b top
				j_patch(b_notback, s_jp);
				j_movw(JR1, (pc + 1) & 0xFFFF);
				j_dpr(CAL, JCMP, 1, 0, JR0, JR1);
				ex[nex].at = j_b(CNE); ex[nex].kind = 1; ex[nex].pc = 0; ex[nex].jn = k + 1; nex++;
				if (k == n - 1) {
					ex[nex].at = j_b(CAL); ex[nex].kind = 1; ex[nex].pc = 0; ex[nex].jn = k + 1; nex++;
				} else
					to_next = j_b(CAL);
			}
			j_patch(b_normal, s_jp);
		}
		const int kind = body(0);
		j_dpi(CAL, JSUB, 1, JR5, JR5, 1);
		if (inl(op) && (op >> 16) >= 0x58 && (op >> 16) <= 0x5f) { // the write hit compiled code: leave
			j_ldr(JR0, JSP, 0);
			j_dpi(CAL, JCMP, 1, 0, JR0, 0);
			ex[nex].at = j_b(CNE); ex[nex].kind = 0; ex[nex].pc = pc + 1; ex[nex].jn = k + 1; nex++;
		}
		if (exitpt(op)) { ex[nex].at = j_b(CLE); ex[nex].kind = kind; ex[nex].pc = pc + 1; ex[nex].jn = k + 1; nex++; }
		if (ender) { ex[nex].at = j_b(CAL); ex[nex].kind = kind; ex[nex].pc = pc + 1; ex[nex].jn = k + 1; nex++; break; }
		if (k == n - 1) { ex[nex].at = j_b(CAL); ex[nex].kind = 0; ex[nex].pc = pc + 1; ex[nex].jn = k + 1; nex++; }
	}
	if (to_next) { // (cannot happen: the last instruction exits) keep the branch valid
		j_patch(to_next, s_jp);
		ex[nex].at = j_b(CAL); ex[nex].kind = 1; ex[nex].pc = 0; ex[nex].jn = n; nex++;
	}
	// leave: r0 = m_pc (kind 0), r10 = instructions run
	uint32_t *leave_pc = s_jp;
	j_str(JR0, JR4, JO(m_pc));
	uint32_t *leave = s_jp;
	j_str(JR5, JR4, JO(m_icount));
	j_str(JR10, JR4, JO(m_jn));
	jw(0xE28DD004u);                 // add sp, sp, #4
	jw(0xE8BD8FF0u);                 // pop {r4-r11, pc}
	uint32_t *t_drop = s_jp;         // made for another mode or loop end: m_jn = -1
	j_dpi(CAL, JMVN, 0, JR0, 0, 0);
	j_str(JR0, JR4, JO(m_jn));
	jw(0xE28DD004u);
	jw(0xE8BD8FF0u);
	uint32_t *t_short = s_jp;        // too few cycles left: m_jn = -2
	j_dpi(CAL, JMVN, 0, JR0, 0, 1);
	j_str(JR0, JR4, JO(m_jn));
	jw(0xE28DD004u);
	jw(0xE8BD8FF0u);
	j_patch(b_mode, t_drop);
	j_patch(b_short, t_short);
	for (int i = 0; i < nex; i++) {
		j_patch(ex[i].at, s_jp);    // stub: r10 = instructions run, r0 = pc (kind 0)
		j_dpi(CAL, JADD, 0, JR10, JR10, (uint32_t)ex[i].jn); // instructions run (r10: earlier rounds of a loop)
		if (ex[i].kind == 1) {
			uint32_t *b = j_b(CAL);
			j_patch(b, leave);
		} else {
			j_movw(JR0, ex[i].pc & 0xFFFF);
			uint32_t *b = j_b(CAL);
			j_patch(b, leave_pc);
		}
	}
	s_jlast_n = n;
	return s_jfull ? 0 : (int)(s_jp - start);
}

static void raxj_newgen() // sound thread: every block dropped (reset, state load); the region is not reused
{
	rstv_rax_jit_gens++;
	memset(s_blk, 0, sizeof(s_blk));
	memset(s_jcov, 0, sizeof(s_jcov));
	memset(s_jpend, 0, sizeof(s_jpend));
	__sync_synchronize();
	s_jgen = s_jgen + 1;
	s_jdirty = 0;
}

// sound thread, once a frame: take the blocks the emu thread installed (if no program write hit them meanwhile)
static void raxj_collect()
{
	if (s_jon <= 0)
		return;
	for (int i = 0; i < RAXJ_SLOTS; i++) {
		RaxSlot *sl = &s_slot[i];
		if (sl->state != RJ_INSTALLED)
			continue;
		__sync_synchronize();
		int ok = sl->gen == s_jgen && sl->code && !s_jdirty && rstv_rax_jit_on != 1; // mode 1: copy only (device check)
		for (int k = 0; ok && k < sl->n; k++)
			if (s_wstamp[(sl->pc + k) & 0x3fff] > sl->stamp)
				ok = 0;
		if (ok) {
			// this core may hold instructions it prefetched from these addresses before they were written (the emu
			// thread's sync covers its own core): sync here too before the first run (ADR-177)
			rstv_jit_code_sync(sl->code, (size_t)sl->len * 4);
			const uint32_t pc = sl->pc;
			int w = s_blk[pc][0] && s_blkt[pc][0] == sl->mode ? 0 : s_blk[pc][1] && s_blkt[pc][1] == sl->mode ? 1 : !s_blk[pc][0] ? 0 : 1;
			s_blk[pc][w] = (RaxBlock)(void *)sl->code;
			s_blkt[pc][w] = (uint8_t)sl->mode;
			s_blk_n[sl->pc] = (uint8_t)sl->n;
			if (++rstv_rax_jit_blocks == 1)
				RAXJ_LOG("raxjit: first block pc %04x at %p len %d", sl->pc, (void *)sl->code, sl->len);
		}
		s_jpend[sl->pc] = 0;
		__sync_synchronize();
		sl->state = RJ_FREE;
	}
}

// the block at pc; NULL: run() interprets this instruction (a block is staged for the next frames)
static RaxBlock raxj_get(uint32_t pc)
{
	if (s_jon <= 0) {
		if (s_jon == 0)
			return NULL;
		s_jon = rstv_rax_jit_on;
#if !defined(__vita__)
		s_jon = getenv("RSTV_RAXJIT") && !getenv("RSTV_RAXHIST") ? 2 : 0; // harness: the interpreter unless asked
#endif
		RAXJ_LOG("raxjit: %s", s_jon ? "on" : "off");
		if (!s_jon)
			return NULL;
	}
	if (s_jdirty)
		raxj_newgen();
	const uint32_t mode = s_cpu.m_mstat & JMS;
	RaxBlock b = s_blk[pc][0] && s_blkt[pc][0] == mode ? s_blk[pc][0] : s_blk[pc][1] && s_blkt[pc][1] == mode ? s_blk[pc][1] : NULL;
	if (b || s_jpend[pc] || s_jregion_full)
		return b;
	for (int i = 0; i < RAXJ_SLOTS; i++) {
		RaxSlot *sl = &s_slot[i];
		if (sl->state != RJ_FREE)
			continue;
		s_jp = sl->w;
		s_jend = sl->w + RAXJ_SLOTW;
		const int len = raxj_compile(pc);
		if (!len)
			return NULL;
		rstv_rax_jit_compiles++;
#if !defined(__vita__)
		{ // diag: RSTV_RAXJITDUMP=pc (hex) writes that block's code to /tmp/raxblk_<pc>.bin
			const char *e = getenv("RSTV_RAXJITDUMP");
			if (e && strtoul(e, NULL, 16) == pc) {
				char fn[64];
				snprintf(fn, sizeof(fn), "/tmp/raxblk_%04x.bin", pc);
				FILE *f = fopen(fn, "wb");
				if (f) { fwrite(sl->w, 4, (size_t)len, f); fclose(f); }
			}
		}
#endif
		sl->pc = pc;
		sl->mode = mode;
		sl->gen = s_jgen;
		sl->len = len;
		sl->n = s_jlast_n;
		sl->stamp = s_wcount;
		sl->code = NULL;
		s_jpend[pc] = 1;
		__sync_synchronize();
		sl->state = RJ_STAGED;
		return NULL;
	}
	return NULL;
}
static inline void raxj_dirty() { s_jdirty = 1; }
// a program write: drop the blocks over a (s_jcov: a was compiled at some time)
static inline void raxj_dirty_at(uint32_t a)
{
	if (!s_jcov[a])
		return;
	s_wstamp[a] = ++s_wcount;
	for (uint32_t p = a >= RAXJ_MAX ? a - RAXJ_MAX + 1 : 0; p <= a; p++)
		if ((s_blk[p][0] || s_blk[p][1]) && p + s_blk_n[p] > a) {
			s_blk[p][0] = s_blk[p][1] = NULL;
			rstv_rax_jit_wdrops++;
		}
}
static inline void raxj_drop(uint32_t pc) { s_blk[pc][0] = s_blk[pc][1] = NULL; }
static inline void raxj_loopend_new(uint32_t a) // a block made without a check at a is dropped
{
	for (uint32_t p = a >= RAXJ_MAX ? a - RAXJ_MAX + 1 : 0; p < a; p++)
		if ((s_blk[p][0] || s_blk[p][1]) && p + s_blk_n[p] > a)
			s_blk[p][0] = s_blk[p][1] = NULL;
	s_jdirty |= 0; // staged blocks over a: their stamp test below catches them
	s_wstamp[a] = ++s_wcount;
}

// emu thread, start of each frame (YabauseExec)
extern "C" void rstv_rax_jit_install(void)
{
	if (s_jon <= 0 || s_jalloc_failed)
		return;
	const uint32_t gen = s_jgen;
	int opened = 0;
	uint32_t *first = NULL;
	size_t words = 0;
	RaxSlot *done[RAXJ_SLOTS];
	int ndone = 0;
	for (int i = 0; i < RAXJ_SLOTS; i++) {
		RaxSlot *sl = &s_slot[i];
		if (sl->state != RJ_STAGED)
			continue;
		__sync_synchronize();
		uint32_t *code = NULL;
		if (sl->gen == gen) {
			if (!s_jcode) {
				s_jcode = (uint32_t *)rstv_jit_code_alloc(RAXJ_CODE);
				RAXJ_LOG("raxjit: code %p (avail %u)", (void *)s_jcode, (unsigned)rstv_jit_code_avail());
				if (!s_jcode) {
					s_jalloc_failed = 1;
					s_jon = 0;
					return;
				}
			}
			if (s_jused + (size_t)sl->len > RAXJ_CODE / 4) {
				if (!s_jregion_full)
					RAXJ_LOG("raxjit: code region full (%u words)", (unsigned)s_jused);
				s_jregion_full = 1;
			} else {
				if (!opened) {
					rstv_jit_code_write_begin();
					opened = 1;
					// each batch on fresh 4 KB pages: writing and syncing a page the sound thread is running code
					// from gave prefetch aborts on the device
					s_jused = (s_jused + 1023) & ~(size_t)1023;
				}
				if (s_jused + (size_t)sl->len > RAXJ_CODE / 4) { // (after the page rounding)
					s_jregion_full = 1;
					sl->code = NULL;
					done[ndone++] = sl;
					continue;
				}
				code = s_jcode + s_jused;
				memcpy(code, sl->w, (size_t)sl->len * 4);
				if (!first)
					first = code;
				words += (size_t)sl->len;
				s_jused += (size_t)sl->len;
			}
		}
		sl->code = code;
		done[ndone++] = sl;
	}
	if (opened && words)
		rstv_jit_code_write_end(first, words * 4);
	// only now (code synced, executable) may the sound thread take them: marking them before the sync let it run a
	// block whose pages were not ready yet (prefetch abort)
	__sync_synchronize();
	for (int i = 0; i < ndone; i++)
		done[i]->state = RJ_INSTALLED;
	{ // device check: progress of the installs
		static unsigned calls, total;
		total += (unsigned)words;
		if (words && (++calls & 63) == 1)
			RAXJ_LOG("raxjit: install %u (%u words so far, used %u)", calls, total, (unsigned)s_jused);
	}
}
#else
unsigned rstv_rax_jit_fb, rstv_rax_jit_compiles, rstv_rax_jit_drops, rstv_rax_jit_wdrops, rstv_rax_jit_blocks;
typedef void (*RaxBlock)(adsp21xx_device *c);
static inline RaxBlock raxj_get(uint32_t) { return NULL; }
static inline void raxj_dirty() {}
static inline void raxj_dirty_at(uint32_t) {}
static inline void raxj_drop(uint32_t) {}
static inline void raxj_loopend_new(uint32_t) {}
static inline void raxj_collect() {}
extern "C" void rstv_rax_jit_install(void) {}
#endif


#if !defined(__vita__) && defined(__arm__)
static int rax_jit_check_on();
static void rax_jit_check(adsp21xx_device *c, RaxBlock b);
#endif
#include "rstv_rax_loops.hxx" // ADR-180
int adsp21xx_device::run(int cycles)
{
	m_icount = cycles;
	m_abort = 0;
	m_skip_pc = ~0u;
	int n = 0;
	check_irqs();
	do
	{
		// ADR-173: the sound driver's wait loops (batmanfr RAX) poll I7, which only the autobuffer timer moves, and
		// the host flags. After one whole idle iteration (13 instructions back at the head, no exit) the rest of the
		// wait until the next event is skipped (the slice is used up, as an IDLE does in MAME)
		if ((m_pc == 0x009a || m_pc == 0x00a1) && rstv_rax_loop_ok(m_pc)) {
			if (m_skip_pc == m_pc && n - m_skip_n == 13) {
				s_idle = 1;
				m_icount = 0;
				break;
			}
			m_skip_pc = m_pc;
			m_skip_n = n;
		}
		if (const int nl = rax_nl_try(this, m_pc)) { // ADR-180: a listed inner loop, natively (one-word loops: top = end)
			n += nl;
			m_icount -= nl;
			continue;
		}
		if (m_pc != m_loop) { // ADR-175: compiled block (the loop end itself is interpreted below)
			const RaxBlock b = raxj_get(m_pc & 0x3fff);
			if (b) {
#if !defined(__vita__) && defined(__arm__)
				if (rax_jit_check_on()) {
					rax_jit_check(this, b);
					if (m_jn >= 0) { n += m_jn; continue; }
				} else
#endif
				b(this);
				if (m_jn >= 0) {
					n += m_jn;
					rstv_rax_jit_insns += (unsigned)m_jn;
					continue;
				}
				if (m_jn == -1) {
					raxj_drop(m_pc & 0x3fff); // made for another mode / loop end: compile again later
					rstv_rax_jit_drops++;
				}
				// -1 / -2: this instruction is interpreted
			}
		}
		m_ppc = m_pc;
#if !defined(__vita__)
		if (s_hist) s_hist[m_pc & 0x3fff]++;
#endif
		const RaxIns in = s_ins[m_pc & 0x3fff];
		if (m_pc != m_loop)
			m_pc++;
		else
		{
			if (condition(m_loop_condition))
				m_pc = pc_stack_top();
			else
			{
				loop_stack_pop();
				pc_stack_pop_val();
				m_pc++;
			}
		}
		n++;

		in.fn(this, in.op);

		m_icount--;
	} while (m_icount > 0);
	m_insns += n;
	return m_abort ? n : cycles;
}

/* ---- board interface ---- */
static void rax_board_reset()
{
	for (int i = 0; i < 32; i++) { // 32 program words (96 bytes) via BDMA
		uint32_t word = s_rom[i * 3 + 0] << 16;
		word |= s_rom[i * 3 + 1] << 8;
		word |= s_rom[i * 3 + 2];
		s_pram[i] = word;
	}
	rax_fn_all();
	s_snd_pf0 = 1;
	s_rom_bank = 0;
	s_size = s_incs = 0;
	s_ireg = 0;
	memset(s_control, 0, sizeof(s_control));
	s_dmovlay_val = 0;
	s_data_bank = 0;
	update_data_ram_bank();
	s_dac_on = 0;
	s_reg_period = 0;
	s_dma_next = 0;
	s_now = 0;
}

extern "C" void rstv_rax_init(const u8 *rom, u32 size)
{
	s_rom = rom;
	s_rom_size = size;
#if !defined(__vita__)
	if (getenv("RSTV_RAXOFFS"))
		fprintf(stderr, "raxoffs pc %u ppc %u loop %u astat %u mstat %u core %u mr %u mf %u i %u m %u l %u base %u px %u "
			"icount %u jn %u read0 %u aluxregs %u cond %u size %u\n",
			(unsigned)offsetof(adsp21xx_device, m_pc), (unsigned)offsetof(adsp21xx_device, m_ppc),
			(unsigned)offsetof(adsp21xx_device, m_loop), (unsigned)offsetof(adsp21xx_device, m_astat),
			(unsigned)offsetof(adsp21xx_device, m_mstat), (unsigned)offsetof(adsp21xx_device, m_core),
			(unsigned)offsetof(adsp21xx_device, m_core.mr), (unsigned)offsetof(adsp21xx_device, m_core.mf),
			(unsigned)offsetof(adsp21xx_device, m_i), (unsigned)offsetof(adsp21xx_device, m_m),
			(unsigned)offsetof(adsp21xx_device, m_l), (unsigned)offsetof(adsp21xx_device, m_base),
			(unsigned)offsetof(adsp21xx_device, m_px), (unsigned)offsetof(adsp21xx_device, m_icount),
			(unsigned)offsetof(adsp21xx_device, m_jn), (unsigned)offsetof(adsp21xx_device, m_read0_ptr),
			(unsigned)offsetof(adsp21xx_device, m_alu_xregs), (unsigned)offsetof(adsp21xx_device, m_condition_table),
			(unsigned)sizeof(adsp21xx_device));
#endif
	s_cpu.init();
	rax_nl_init();
	memset(s_pram, 0, sizeof(s_pram));
	rax_fn_all();
	memset(s_banked, 0, sizeof(s_banked));
	memset(s_iram, 0, sizeof(s_iram));
	rstv_rax_active = 1;
	rstv_rax_reset();
}

static void rax_wait();
static void rax_pend_state_reset();
static int rax_pend_state(FILE *fp, int save);
extern "C" int rstv_rax_state(FILE *fp, int save)
{
	rax_wait(); // ADR-176: the board thread finishes its frame first
#define RAX_ST(x) do { if (save ? fwrite(&(x), sizeof(x), 1, fp) != 1 : fread(&(x), sizeof(x), 1, fp) != 1) return -1; } while (0)
	// the board is saved as raw structs: a tag with their layout comes first, and a save from a build with another
	// layout is not read (1: the caller starts the board again) instead of loading garbage pointers and indexes
	{
		uint32_t tag[2] = { 0x53584152u /* "RAXS" */, (uint32_t)(sizeof(s_cpu) | RAX_QN << 16) ^ 1u /* layout 1 */ };
		uint32_t got[2] = { tag[0], tag[1] };
		RAX_ST(got);
		if (!save && (got[0] != tag[0] || got[1] != tag[1]))
			return 1;
	}
	RAX_ST(s_cpu); // pointer tables are set again below
	RAX_ST(s_pram); RAX_ST(s_banked); RAX_ST(s_iram); RAX_ST(s_control);
	RAX_ST(s_snd_pf0); RAX_ST(s_data_bank); RAX_ST(s_rom_bank); RAX_ST(s_dmovlay_val);
	RAX_ST(s_size); RAX_ST(s_incs); RAX_ST(s_ireg_base); RAX_ST(s_ireg); RAX_ST(s_dac_on); RAX_ST(s_dac_rate);
	RAX_ST(s_now); RAX_ST(s_reg_next); RAX_ST(s_reg_period); RAX_ST(s_dma_next); RAX_ST(s_dma_param);
	RAX_ST(s_data_in); RAX_ST(s_data_out); RAX_ST(s_idle);
	RAX_ST(s_cmd); RAX_ST(s_cmd_r); RAX_ST(s_cmd_w);
	RAX_ST(s_out); RAX_ST(s_out_r); RAX_ST(s_out_w); RAX_ST(s_out_frac);
	if (rax_pend_state(fp, save))
		return -1;
	// words of frames the sound thread has not run yet
	uint32_t nq = s_qw - s_qr;
	RAX_ST(nq);
	if (nq > RAX_QN)
		return -1;
	for (uint32_t i = 0; i < nq; i++) {
		uint32_t v = save ? s_q[(s_qr + i) % RAX_QN] : 0;
		RAX_ST(v);
		if (!save)
			s_q[i] = v;
	}
#undef RAX_ST
#if !defined(__vita__)
	if (getenv("RSTV_RAXLOG"))
		fprintf(stderr, "rax state %s now %llu pc %04x q %u cmd %u/%u reg %llu dma %llu\n", save ? "save" : "load",
			(unsigned long long)s_now, s_cpu.m_pc, nq, s_cmd_r, s_cmd_w, (unsigned long long)s_reg_next,
			(unsigned long long)s_dma_next);
#endif
	if (!save) {
		s_cpu.setup_pointers();
		rax_fn_all();
		update_data_ram_bank();
		s_qr = 0;
		s_qw = nq;
	}
	return 0;
}

#if defined(__vita__)
// diag (ux0:data/restv/raxbench): time per call of the handlers of a few instructions from the decoder loops
extern "C" u64 sceKernelGetProcessTimeWide(void);
static void rax_noop(adsp21xx_device *, uint32_t) {}
extern "C" void rstv_rax_bench(void)
{
	static const uint32_t ops[] = { 0x000000, 0x638609, 0x600009, 0x2278df, 0x12160b, 0x22ea0f, 0x0d047a, 0x83dfd5, 0x680005 };
	adsp21xx_device *c = &s_cpu;
	c->init();
	c->reset();
	for (int k = 0; k < 8; k++) { c->m_i[k] = 0x2100; c->m_l[k] = 0; c->m_m[k] = 0; c->m_lmask[k] = 0x3fff; c->m_base[k] = 0x2100; }
	enum { N = 200000 };
	{
		const u64 t0 = sceKernelGetProcessTimeWide();
		RaxExecFn f = rax_noop;
		for (int i = 0; i < N; i++) f(c, 0);
		RAXJ_LOG("raxbench: empty call %.1f ns", (sceKernelGetProcessTimeWide() - t0) * 1000.0 / N);
	}
	for (unsigned k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
		const RaxExecFn f = s_exec_tab[(ops[k] >> 13) & 0x7ff];
		const u64 t0 = sceKernelGetProcessTimeWide();
		for (int i = 0; i < N; i++) f(c, ops[k]);
		RAXJ_LOG("raxbench: %06x %.1f ns", ops[k], (sceKernelGetProcessTimeWide() - t0) * 1000.0 / N);
	}
	c->init();
}
#endif

extern "C" void rstv_rax_shutdown(void)
{
	rstv_rax_active = 0;
	s_rom = NULL;
}

extern "C" void rstv_rax_reset(void)
{
	rax_wait();
	rax_pend_state_reset();
	if (!s_rom)
		return;
	rax_board_reset();
	s_cpu.reset();
	s_qw = s_qr = 0;
	s_cmd_r = s_cmd_w = 0;
	s_out_r = s_out_w = 0;
	s_out_frac = 0;
	s_data_in = s_data_out = 0;
}

static void q_put(uint32_t v)
{
	const uint32_t w = s_qw;
	if (w - s_qr >= RAX_QN)
		return; // the sound thread stopped: drop
	s_q[w % RAX_QN] = v;
	__sync_synchronize();
	s_qw = w + 1;
}

extern "C" void rstv_rax_data_w(u16 data)
{
	if (rstv_rax_active)
		q_put(data);
}

extern "C" void rstv_rax_frame_end(void)
{
	if (rstv_rax_active)
		q_put(0x10000);
}

// one frame of the board: its SH-2 words, n samples of DSP time, the DAC output resampled into l / r (added)
static void rax_frame(s32 *l, s32 *r, int n)
{
	raxj_collect(); // ADR-175: blocks installed by the emu thread
	// one emulated frame's words, once all of them are there (its end marker)
	{
		const uint32_t w = s_qw;
		__sync_synchronize();
		uint32_t m = s_qr;
		while (m != w && !(s_q[m % RAX_QN] & 0x10000))
			m++;
		if (m != w) {
			for (uint32_t rd = s_qr; rd != m; rd++)
#if !defined(__vita__)
				if (getenv("RSTV_RAXLOG") && fprintf(stderr, "rax cmd %04x at q %u\n", s_q[rd % RAX_QN], rd) < 0) {} else
#endif
				if (s_cmd_w - s_cmd_r < RAX_QN)
					s_cmd[s_cmd_w++ % RAX_QN] = (uint16_t)s_q[rd % RAX_QN];
			__sync_synchronize();
			s_qr = m + 1;
		}
	}
	const u64 t_prof = RSTV_PROF_T();
#if defined(__vita__)
	const u64 c_prof = t_prof ? rax_thread_cpu_us() : 0;
#endif
#if !defined(__vita__)
	struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
#endif
	// one frame of DSP time (n samples at 44.1 kHz)
	const uint64_t end = s_now + (uint64_t)RAX_CLOCK * n / 44100;
	while (s_now < end) {
		uint64_t stop = end;
		if (s_reg_period && s_reg_next < stop)
			stop = s_reg_next;
		if (s_dma_next && s_dma_next < stop)
			stop = s_dma_next;
		if (s_cmd_r != s_cmd_w && !s_cpu.m_irq_state[ADSP2181_IRQL0])
			s_cpu.set_input(ADSP2181_IRQL0, ASSERT_LINE); // data_w
		if (s_idle && !s_cpu.irq_pending()) {
			s_now = stop; // ADR-173: nothing the wait loop polls changes before the next event
		} else if (stop > s_now) {
			s_idle = 0;
			if (stop > s_now + RAX_SLICE)
				stop = s_now + RAX_SLICE;
			s_t0 = s_now;
			s_len = (int)(stop - s_now);
			s_now += s_cpu.run(s_len);
		}
		if (s_reg_period && s_now >= s_reg_next) {
			s_idle = 0;
			adsp_irq0();
			s_reg_next += s_reg_period;
		}
		if (s_dma_next && s_now >= s_dma_next) {
			s_dma_next = 0;
			s_idle = 0;
			dma_timer();
		}
	}
	rstv_rax_insns = s_cpu.m_insns;
	if (t_prof) {
		rstv_rax_us += RSTV_PROF_T() - t_prof;
#if defined(__vita__)
		rstv_rax_cpu_us += rax_thread_cpu_us() - c_prof;
#endif
	}
#if !defined(__vita__)
	{ // diag (harness): RSTV_RAXLOG=1, a line every 60 frames
		static int init, on, fr;
		static unsigned last_insns, last_cmds;
		if (!init) { init = 1; on = getenv("RSTV_RAXLOG") != NULL; }
		if (fr == 0 && getenv("RSTV_RAXHIST")) s_hist = (unsigned *)calloc(0x4000, sizeof(unsigned));
		if (s_hist && fr % 600 == 599) {
			for (int k = 0; k < 400; k++) {
				int best = 0;
				for (int a = 1; a < 0x4000; a++) if (s_hist[a] > s_hist[best]) best = a;
				if (!s_hist[best]) break;
				fprintf(stderr, "raxpc %04x %u %06x\n", best, s_hist[best], s_pram[best]);
				s_hist[best] = 0;
			}
			memset(s_hist, 0, 0x4000 * sizeof(unsigned));
		}
		{ // RSTV_RAXDUMP=file (+ RSTV_RAXDUMPF=frame, default 1800): program memory, then data memory (0x2000 + 0x2000 words)
			static int dumped;
			const char *dp = getenv("RSTV_RAXDUMP");
			const char *df = getenv("RSTV_RAXDUMPF");
			if (dp && !dumped && fr >= (df ? atoi(df) : 1800)) {
				FILE *f = fopen(dp, "wb");
				if (f) {
					fwrite(s_pram, 4, 0x4000, f);
					fwrite(s_dbank, 2, 0x2000, f);
					fwrite(s_iram, 2, 0x2000, f);
					fclose(f);
				}
				dumped = 1;
			}
		}
		static double ns;
		{ struct timespec ts1; clock_gettime(CLOCK_MONOTONIC, &ts1); ns += (ts1.tv_sec - ts0.tv_sec) * 1e9 + (ts1.tv_nsec - ts0.tv_nsec); }
		if (on && fr % 60 == 59) { fprintf(stderr, "rax mix %.3f ms/frame\n", ns / 60 / 1e6); ns = 0; }
		if (on && ++fr % 60 == 0) {
#if defined(__arm__)
			fprintf(stderr, "rax bank switches %u\n", rstv_rax_bank_switches);
			fprintf(stderr, "rax jit gens %u insns %u fb %u compiles %u drops %u wdrops %u\n", rstv_rax_jit_gens, rstv_rax_jit_insns,
				rstv_rax_jit_fb, rstv_rax_jit_compiles, rstv_rax_jit_drops, rstv_rax_jit_wdrops);
			if (fr % 600 == 0) {
				fprintf(stderr, "rax jit fallback kinds:");
				for (int k = 0; k < 256; k++) if (s_jfb_kind[k] > 10000) fprintf(stderr, " %02x:%uk", k, s_jfb_kind[k] / 1000);
				fprintf(stderr, "\n");
			}
#endif
			fprintf(stderr, "rax f%d insns/frame %u cmds %u q %u dac %d rate %u out %u pc %04x\n", fr,
				(s_cpu.m_insns - last_insns) / 60, s_cmd_w - last_cmds, s_qw, s_dac_on, s_dac_rate, s_out_w - s_out_r,
				s_cpu.m_pc);
			last_insns = s_cpu.m_insns;
			last_cmds = s_cmd_w;
		}
	}
#endif
	// DAC rate -> 44.1 kHz (linear)
	if (!s_dac_rate)
		return;
	const uint32_t step = (uint32_t)(((uint64_t)s_dac_rate << 16) / 44100);
	for (int i = 0; i < n; i++) {
		if (s_out_w - s_out_r < 2)
			break;
		const int16_t *a = s_out[s_out_r % RAX_ON], *b = s_out[(s_out_r + 1) % RAX_ON];
		const int32_t f = (int32_t)s_out_frac;
		l[i] += a[0] + (((b[0] - a[0]) * f) >> 16);
		r[i] += a[1] + (((b[1] - a[1]) * f) >> 16);
		s_out_frac += step;
		s_out_r += s_out_frac >> 16;
		s_out_frac &= 0xffff;
	}
	// keep the buffer short (DAC rate rounding): drop what is more than a frame ahead
	const unsigned keep = s_dac_rate / 60 + 2;
	if (s_out_w - s_out_r > 2 * keep)
		s_out_r = s_out_w - keep;
}

/* ADR-176: the board runs one frame behind the sound thread. Each call mixes the output of the previous frame and
   starts the next one: on its own thread on CPU3 (Vita), or right away (harness), so the samples are the same. */
#include "threads.h"
#define RAX_MAXN 1024
static s32 s_pend_l[RAX_MAXN], s_pend_r[RAX_MAXN];
static int s_pend_n;              // samples of the frame run last (0: none)
static int s_job_n;
static volatile int s_busy;       // the board thread is running a frame
static int s_thr = -1;            // -1: not decided, 0: inline, 1: thread
extern "C" { int rstv_rax_thread_cpu; } // 0: on the sound thread (default); 2 / 3: a board thread on that CPU
static YabEventQueue *s_q_go, *s_q_done;

static void rax_job()
{
	memset(s_pend_l, 0, sizeof(s32) * (size_t)s_job_n);
	memset(s_pend_r, 0, sizeof(s32) * (size_t)s_job_n);
	rax_frame(s_pend_l, s_pend_r, s_job_n);
	s_pend_n = s_job_n;
}

static void *rax_thread_main(void *)
{
	for (;;) {
		YabWaitEventQueue(s_q_go);
		rax_job();
		YabAddEventQueue(s_q_done, 0);
	}
	return NULL;
}

// the board thread is idle after this (sound thread, between frames)
static void rax_wait()
{
	if (s_busy) {
		YabWaitEventQueue(s_q_done);
		s_busy = 0;
	}
}

extern "C" void rstv_rax_mix(s32 *l, s32 *r, int n)
{
	if (!rstv_rax_active || !s_rom || n > RAX_MAXN)
		return;
	if (s_thr < 0) {
		s_thr = 0;
#if defined(__vita__) // ADR-176: a board thread only when asked (ux0:data/restv/raxthr = 2 or 3: its CPU)
		if (rstv_rax_thread_cpu == 2) { // CPU3 hit prefetch aborts running JIT code (twice): not offered
			s_q_go = YabThreadCreateQueue(2);
			s_q_done = YabThreadCreateQueue(2);
			if (s_q_go && s_q_done && YabThreadStart(YAB_THREAD_RAX, rax_thread_main, NULL) == 0)
				s_thr = 1;
		}
#endif
	}
	rax_wait();
	for (int i = 0; i < s_pend_n && i < n; i++) {
		l[i] += s_pend_l[i];
		r[i] += s_pend_r[i];
	}
	s_job_n = n;
	if (s_thr == 1) {
		s_busy = 1;
		YabAddEventQueue(s_q_go, 0);
	} else
		rax_job();
}

static void rax_pend_state_reset() { s_pend_n = 0; }
// the output of the frame run last (mixed by the next call)
static int rax_pend_state(FILE *fp, int save)
{
	int nn = s_pend_n;
	if (save ? fwrite(&nn, sizeof(nn), 1, fp) != 1 : fread(&nn, sizeof(nn), 1, fp) != 1)
		return -1;
	if (nn < 0 || nn > RAX_MAXN)
		return -1;
	if (save ? (fwrite(s_pend_l, sizeof(s32), (size_t)nn, fp) != (size_t)nn || fwrite(s_pend_r, sizeof(s32), (size_t)nn, fp) != (size_t)nn)
	         : (fread(s_pend_l, sizeof(s32), (size_t)nn, fp) != (size_t)nn || fread(s_pend_r, sizeof(s32), (size_t)nn, fp) != (size_t)nn))
		return -1;
	s_pend_n = nn;
	return 0;
}

void adsp21xx_device::step1()
{
	const RaxIns in = s_ins[m_pc & 0x3fff];
	m_ppc = m_pc;
	if (m_pc != m_loop)
		m_pc++;
	else {
		if (condition(m_loop_condition))
			m_pc = pc_stack_top();
		else {
			loop_stack_pop();
			pc_stack_pop_val();
			m_pc++;
		}
	}
	in.fn(this, in.op);
	m_icount--;
}

#if !defined(__vita__) && defined(__arm__)
/* ADR-177 check (harness, RSTV_RAXJITCHK=frames): each block is run, then its state is compared with the interpreter
   running the same instructions from the same start (registers and data memory) */
static unsigned s_chk_bad, s_chk_n;
static int rax_jit_check_on()
{
	static int on = -1;
	if (on < 0) on = getenv("RSTV_RAXJITCHK") ? 1 : 0;
	return on;
}
static void rax_jit_check(adsp21xx_device *c, RaxBlock b)
{
	static uint8_t c0[sizeof(adsp21xx_device)], cj[sizeof(adsp21xx_device)];
	static uint16_t m0b[0x2000 * 5], m0i[0x2000], mjb[0x2000 * 5], mji[0x2000];
	const size_t core = offsetof(adsp21xx_device, m_read0_ptr);
	static uint32_t p0[0x4000], pj[0x4000];
	memcpy(c0, c, sizeof(*c)); memcpy(m0b, s_banked, sizeof(s_banked)); memcpy(m0i, s_iram, sizeof(s_iram));
	memcpy(p0, s_pram, sizeof(s_pram));
	const uint64_t now0 = s_now; const uint64_t dma0 = s_dma_next; const uint32_t dp0 = s_dma_param;
	uint16_t ctl0[32]; memcpy(ctl0, s_control, sizeof(ctl0));
	const uint32_t pc0 = c->m_pc;
	b(c);
	if (c->m_jn < 0)
		return;
	const int jn = c->m_jn;
	memcpy(cj, c, sizeof(*c)); memcpy(mjb, s_banked, sizeof(s_banked)); memcpy(mji, s_iram, sizeof(s_iram));
	const uint64_t dmaj = s_dma_next; const uint32_t dpj = s_dma_param;
	memcpy(pj, s_pram, sizeof(s_pram));
	memcpy(c, c0, sizeof(*c)); memcpy(s_banked, m0b, sizeof(s_banked)); memcpy(s_iram, m0i, sizeof(s_iram));
	memcpy(s_pram, p0, sizeof(s_pram)); // (s_ins / blocks are not touched by data-area program writes)
	s_now = now0; s_dma_next = dma0; s_dma_param = dp0;
	uint16_t ctlj[32]; memcpy(ctlj, s_control, sizeof(ctlj)); memcpy(s_control, ctl0, sizeof(ctl0));
	for (int i = 0; i < jn; i++)
		c->step1();
	{ // bookkeeping only the block keeps
		const adsp21xx_device *J = (const adsp21xx_device *)cj;
		c->m_jn = J->m_jn; c->m_insns = J->m_insns; c->m_skip_pc = J->m_skip_pc; c->m_skip_n = J->m_skip_n; c->m_ppc = J->m_ppc;
	}
	s_chk_n++;
	int bad = memcmp(cj, c, core) != 0 || memcmp(mjb, s_banked, sizeof(s_banked)) || memcmp(mji, s_iram, sizeof(s_iram)) ||
		dmaj != s_dma_next || memcmp(ctlj, s_control, sizeof(ctlj)) || memcmp(pj, s_pram, sizeof(s_pram));
	if (bad && s_chk_bad++ < 20) {
		fprintf(stderr, "raxjitchk BAD block %04x n %d:", pc0, jn);
		for (size_t o = 0; o < core; o++)
			if (cj[o] != ((uint8_t *)c)[o]) fprintf(stderr, " c+%u %02x/%02x", (unsigned)o, cj[o], ((uint8_t *)c)[o]);
		for (int a = 0; a < 0x2000 * 5; a++) if (mjb[a] != s_banked[a]) { fprintf(stderr, " bank %04x %04x/%04x", a, mjb[a], s_banked[a]); break; }
		for (int a = 0; a < 0x2000; a++) if (mji[a] != s_iram[a]) { fprintf(stderr, " iram %04x %04x/%04x", a + 0x2000, mji[a], s_iram[a]); break; }
		fprintf(stderr, "\n");
		for (int i = 0; i < jn && i < 48; i++) fprintf(stderr, "  %04x %06x\n", pc0 + i, s_pram[(pc0 + i) & 0x3fff]);
	}
	if ((s_chk_n & 0xffff) == 0)
		fprintf(stderr, "raxjitchk %u blocks, %u bad\n", s_chk_n, s_chk_bad);
	// go on from the interpreter's state
}
#endif

void adsp21xx_device::loop_end_step(uint32_t a)
{
	if (condition(m_loop_condition))
		m_pc = pc_stack_top();
	else {
		loop_stack_pop();
		pc_stack_pop_val();
		m_pc = a + 1;
	}
}
extern "C" void rax_loop_end(adsp21xx_device *c, uint32_t a) { c->loop_end_step(a); }
