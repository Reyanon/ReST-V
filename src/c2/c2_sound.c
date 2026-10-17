/*
 * c2_sound.c - Sega System C / C2 sound glue: clocks, resampling to 44100 Hz, mixing,
 * output ring and save state for YM3438 (c2_ym.cpp) + PSG (sn76489.c) + uPD7759 (upd7759.c).
 *
 * Chip clocks, routing and mix levels follow MAME 0.264 src/mame/sega/segac2.cpp
 * (BSD-3-Clause, copyright-holders: David Haywood, Aaron Giles).
 * This file is part of ReST-V. GPL-2.0-or-later.
 *
 * Time base: XL2 master clocks. Per chip:
 *   YM3438  one sample every 7*144 = 1008 clocks; linear interpolation (1 YM sample latency)
 *   PSG     one divided tick every 15*16 = 240 clocks; box average per output sample
 *   uPD7759 640 kHz via a fractional accumulator; time-weighted average per output sample
 * The main loop advances in chunks that end exactly on 44100 Hz output sample boundaries.
 */
#include "c2_sound.h"
#include "c2_ym.h"
#include "sn76489.h"
#include "upd7759.h"

#include <string.h>

#define XL2             C2SND_XL2_CLOCK
#define OUT_RATE        C2SND_OUTPUT_RATE
#define PSG_TICK        (15u * 16u)
#define YM_PERIOD       C2YM_SAMPLE_PERIOD
#define RING_MASK       (C2SND_RING_FRAMES - 1)

/* Saved mixer/resampler state: fixed-width fields only. */
typedef struct mix_state {
	uint32_t out_acc;    /* output phase, units of master*OUT_RATE, < XL2 */
	uint32_t ym_cnt;     /* master clocks since the last YM sample */
	int32_t  ym_prev, ym_cur;
	uint32_t psg_cnt;    /* master clocks into the current PSG tick */
	int32_t  psg_sum, psg_n;
	uint32_t upd_acc;    /* uPD phase, units of master*640000, < XL2 */
	int32_t  upd_sum;    /* sum(sample * clocks) since last output */
	uint32_t upd_clk;
	int32_t  dc_x;       /* DC blocker: previous input */
	int32_t  dc_y;       /* DC blocker: previous output, Q8 */
} mix_state;

static mix_state m;
/* ADR-107: the YM sound is made on a worker (another core). The emulation thread keeps the timer / status chip and
   logs, per frame, every YM write and every c2snd_advance(); the worker replays the log on the sound-only chip with the
   same chunking, so the result does not depend on thread timing. The frame is mixed one frame later. */
#define LOG_MAX 16384
#define PU_MAX  1024
#define LOG_WRITE 0x80000000u
typedef struct c2frame {
	uint32_t log[LOG_MAX];
	int nlog;
	int32_t ps[PU_MAX], up[PU_MAX], ym[PU_MAX]; /* per output sample: PSG, uPD (emulation thread), YM (worker) */
	int npu, nym;
} c2frame;
static c2frame fr[2];
static int fr_cur;           /* frame being filled by the emulation thread */
static int fr_job = -1;      /* frame handed to the worker, mixed at the next frame end */
typedef struct ym_state {    /* worker-side YM resampler */
	uint32_t out_acc, ym_cnt;
	int32_t ym_prev, ym_cur;
} ym_state;
static ym_state wy;
void (*c2snd_kick)(void);    /* frontend: run c2snd_worker() on the worker thread (NULL = run it here) */
void (*c2snd_wait)(void);    /* frontend: wait until that run is done */
static sn_psg    psg;
static upd_chip  upd;
static int       g_ym = 128, g_psg = 128, g_upd = 128;

static int16_t   ring[C2SND_RING_FRAMES];
static uint32_t  ring_head, ring_tail;  /* producer writes head, consumer writes tail */

/* ---- ring (SPSC) ---- */
static void ring_push(int16_t v)
{
	uint32_t h = ring_head;
	uint32_t t = __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE);
	if (h - t >= C2SND_RING_FRAMES) return;     /* full: drop newest */
	ring[h & RING_MASK] = v;
	__atomic_store_n(&ring_head, h + 1, __ATOMIC_RELEASE);
}

int c2snd_available(void)
{
	return (int)(__atomic_load_n(&ring_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE));
}

int c2snd_fetch(int16_t *out, int max_frames)
{
	uint32_t t = ring_tail;
	uint32_t h = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
	int n = 0;
	while (n < max_frames && t != h) {
		int16_t v = ring[t & RING_MASK];
		out[2 * n] = v;
		out[2 * n + 1] = v;
		t++; n++;
	}
	__atomic_store_n(&ring_tail, t, __ATOMIC_RELEASE);
	return n;
}

/* ---- worker ---- */
static void log_put(uint32_t v)
{
	c2frame *f = &fr[fr_cur];
	if (f->nlog < LOG_MAX)
		f->log[f->nlog++] = v;
}

void c2snd_worker(void)
{
	c2frame *f = &fr[fr_job];
	f->nym = 0;
	for (int i = 0; i < f->nlog; i++) {
		const uint32_t v = f->log[i];
		if (v & LOG_WRITE) {
			c2ym_gen_write((int)((v >> 8) & 3), (uint8_t)v);
			continue;
		}
		uint32_t n = v;
		while (n) { /* same chunks as c2snd_advance: they end on output sample boundaries */
			const uint32_t need = (XL2 - wy.out_acc + OUT_RATE - 1) / OUT_RATE;
			const uint32_t step = n < need ? n : need;
			wy.ym_cnt += step;
			while (wy.ym_cnt >= YM_PERIOD) {
				wy.ym_cnt -= YM_PERIOD;
				wy.ym_prev = wy.ym_cur;
				wy.ym_cur = c2ym_gen_sample();
			}
			wy.out_acc += step * OUT_RATE;
			n -= step;
			if (wy.out_acc >= XL2) {
				wy.out_acc -= XL2;
				if (f->nym < PU_MAX)
					f->ym[f->nym++] = wy.ym_prev +
						(int32_t)(((int64_t)(wy.ym_cur - wy.ym_prev) * (int32_t)wy.ym_cnt) / (int32_t)YM_PERIOD);
			}
		}
	}
}

static void mix_frame(const c2frame *f)
{
	for (int i = 0; i < f->npu; i++) {
		const int32_t ym = i < f->nym ? f->ym[i] : 0;
		int32_t x = (ym * g_ym + f->ps[i] * g_psg + f->up[i] * g_upd) >> 8, acc;
		/* one-pole DC blocker, pole 1 - 1/1024 (~7 Hz at 44.1 kHz), Q8 */
		acc = ((x - m.dc_x) * 256) + m.dc_y - (m.dc_y >> 10);
		m.dc_x = x;
		m.dc_y = acc;
		x = acc >> 8;
		if (x > 32767) x = 32767;
		else if (x < -32768) x = -32768;
		ring_push((int16_t)x);
	}
}

/* finish the frame on the worker (if any) and mix it */
void c2snd_sync(void)
{
	if (fr_job < 0)
		return;
	if (c2snd_kick && c2snd_wait)
		c2snd_wait();
	mix_frame(&fr[fr_job]);
	fr_job = -1;
}

void c2snd_frame_end(void)
{
	c2snd_sync();
	fr_job = fr_cur;
	fr_cur ^= 1;
	fr[fr_cur].nlog = fr[fr_cur].npu = fr[fr_cur].nym = 0;
	if (c2snd_kick && c2snd_wait)
		c2snd_kick();
	else
		c2snd_worker();
}

/* ---- init / reset ---- */
static void mix_clear(void)
{
	c2snd_sync();
	memset(&m, 0, sizeof(m));
	memset(&wy, 0, sizeof(wy));
	fr_cur = 0;
	fr_job = -1;
	fr[0].nlog = fr[0].npu = fr[0].nym = 0;
	fr[1].nlog = fr[1].npu = fr[1].nym = 0;
}

void c2snd_init(void)
{
	mix_clear();
	c2ym_init();
	sn_init(&psg);
	upd_init(&upd);
	ring_head = ring_tail = 0;
}

void c2snd_reset(void)
{
	mix_clear();
	c2ym_reset();
	sn_init(&psg);
	upd_device_reset(&upd);
}

void c2snd_set_gains(int ym, int psg_gain, int upd_gain)
{
	g_ym = ym; g_psg = psg_gain; g_upd = upd_gain;
}

/* ---- chip access ---- */
void    c2snd_ym_write(int a, uint8_t d) { c2ym_write(a, d); log_put(LOG_WRITE | ((uint32_t)(a & 3) << 8) | d); }
uint8_t c2snd_ym_read(int a)             { return c2ym_read(a); }
int     c2snd_ym_irq(void)               { return c2ym_irq(); }
void    c2snd_ym_set_busy_emulation(int on) { c2ym_set_busy_emulation(on); }
void    c2snd_sn_write(uint8_t d)        { sn_write(&psg, d); }

void c2snd_upd_set_rom(const uint8_t *rom, uint32_t size) { upd_set_rom(&upd, rom, size); }
void c2snd_upd_set_bank(int bank)        { upd_set_bank(&upd, bank); }
void c2snd_upd_set_start_delay(int d)    { upd.s.start_delay = d < 0 ? 0u : (uint32_t)d; }
void c2snd_upd_port(uint8_t d)           { upd_port_w(&upd, d); }
void c2snd_upd_start(int line)           { upd_start_w(&upd, line); }
void c2snd_upd_reset(int line)           { upd_reset_w(&upd, line); }
int  c2snd_upd_busy(void)                { return upd.rom ? upd_busy_r(&upd) : 1; }

/* ---- timing ---- */
extern uint64_t (*c2_clock_us)(void);
extern int c2vdp_prof_on;
unsigned c2snd_prof_us[4]; /* psg, ym, upd, ym time (c2vdp_prof_on only) */
static void run_chips(uint32_t step)  /* step <= ~1218 master clocks */
{
	const int prof = c2vdp_prof_on && c2_clock_us;
	uint64_t t0 = prof ? c2_clock_us() : 0, t1 = 0, t2 = 0, t3 = 0;
	m.psg_cnt += step;
	while (m.psg_cnt >= PSG_TICK) {
		m.psg_cnt -= PSG_TICK;
		m.psg_sum += sn_tick(&psg);
		m.psg_n++;
	}

	if (prof) t1 = c2_clock_us(); /* YM sound: on the worker (c2snd_worker); the timer chip only counts the period */
	m.ym_cnt += step;
	while (m.ym_cnt >= YM_PERIOD) {
		m.ym_cnt -= YM_PERIOD;
		c2ym_count_tick();
	}

	if (prof) t2 = c2_clock_us();
	if (upd.rom) {
		uint32_t n;
		m.upd_acc += step * C2SND_XL1_CLOCK;
		n = m.upd_acc / XL2;
		m.upd_acc -= n * XL2;
		m.upd_sum += (int32_t)upd_run(&upd, n);
		m.upd_clk += n;
	}

	if (prof) t3 = c2_clock_us();
	c2ym_advance_time(step);
	if (prof) {
		const uint64_t t4 = c2_clock_us();
		c2snd_prof_us[0] += (unsigned)(t1 - t0);
		c2snd_prof_us[1] += (unsigned)(t2 - t1);
		c2snd_prof_us[2] += (unsigned)(t3 - t2);
		c2snd_prof_us[3] += (unsigned)(t4 - t3);
	}
}

static void emit_sample(void)
{
	c2frame *f = &fr[fr_cur];
	const int32_t ps = m.psg_n ? m.psg_sum / m.psg_n : psg.out;
	const int32_t up = m.upd_clk ? (m.upd_sum * 128) / (int32_t)m.upd_clk : 0;
	m.psg_sum = 0; m.psg_n = 0;
	m.upd_sum = 0; m.upd_clk = 0;
	if (f->npu < PU_MAX) {
		f->ps[f->npu] = ps;
		f->up[f->npu] = up;
		f->npu++;
	}
}

void c2snd_advance(uint32_t xl2_clocks)
{
	if (xl2_clocks)
		log_put(xl2_clocks & ~LOG_WRITE);
	while (xl2_clocks) {
		uint32_t need = (XL2 - m.out_acc + OUT_RATE - 1) / OUT_RATE; /* >= 1 */
		uint32_t step = xl2_clocks < need ? xl2_clocks : need;
		run_chips(step);
		m.out_acc += step * OUT_RATE;
		xl2_clocks -= step;
		if (m.out_acc >= XL2) {
			m.out_acc -= XL2;
			emit_sample();
		}
	}
}

/* ---- save state ---- */
#define STATE_MAGIC   0x4E533243u   /* "C2SN" */
#define STATE_VERSION 2u /* 2: worker YM resampler, second YM chip (ADR-107) */

static int wr_u32(FILE *fp, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	return fwrite(b, 1, 4, fp) == 4;
}

static int rd_u32(FILE *fp, uint32_t *v)
{
	uint8_t b[4];
	if (fread(b, 1, 4, fp) != 4) return 0;
	*v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 1;
}

/* blocks are size-prefixed so a layout change is detected instead of misread */
static int blk(FILE *fp, void *p, uint32_t size, int save)
{
	uint32_t s;
	if (save) return wr_u32(fp, size) && fwrite(p, 1, size, fp) == size;
	return rd_u32(fp, &s) && s == size && fread(p, 1, size, fp) == size;
}

int c2snd_state(FILE *fp, int save)
{
	uint32_t magic = STATE_MAGIC, ver = STATE_VERSION;
	mix_state tm;
	sn_psg tp;
	upd_state tu;

	if (!fp) return -1;
	c2snd_sync(); /* between frames: the frame being filled is empty, the worker's one is mixed now */
	if (save) {
		if (!wr_u32(fp, magic) || !wr_u32(fp, ver)) return -1;
		if (!blk(fp, &wy, sizeof(wy), 1)) return -1;
		if (!blk(fp, &m, sizeof(m), 1) || !blk(fp, &psg, sizeof(psg), 1)
		    || !blk(fp, &upd.s, sizeof(upd.s), 1))
			return -1;
		return c2ym_state(fp, 1);
	}

	if (!rd_u32(fp, &magic) || !rd_u32(fp, &ver) || magic != STATE_MAGIC || ver != STATE_VERSION)
		return -1;
	/* read into temporaries so a short file leaves the running state intact (except YM) */
	ym_state twy;
	if (!blk(fp, &twy, sizeof(twy), 0)) return -1;
	if (!blk(fp, &tm, sizeof(tm), 0) || !blk(fp, &tp, sizeof(tp), 0) || !blk(fp, &tu, sizeof(tu), 0))
		return -1;
	if (c2ym_state(fp, 0) != 0) return -1;
	m = tm;
	wy = twy;
	fr[fr_cur].nlog = fr[fr_cur].npu = fr[fr_cur].nym = 0;
	psg = tp;
	upd.s = tu;
	upd_set_bank(&upd, upd.s.bank);
	__atomic_store_n(&ring_tail, __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
	return 0;
}
