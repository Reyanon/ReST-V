/*  ReST-V (ADR-136): the slave SH-2 on its own thread. See rstv_slave.h.
    This file is part of ReST-V, based on Yabause. GPL-2.0-or-later. */
#include "rstv_slave.h"

#include "sh2core.h"
#include "yabause.h"

int rstv_slave_cfg = 0;
volatile int rstv_slave_on = 0;
int rstv_slave_lead = 8;
int rstv_slave_spin = 2000; /* busy-wait iterations before the slave thread sleeps (it shares CPU2 with the prep worker) */
unsigned rstv_slave_busy_us, rstv_slave_wait_us, rstv_slave_lockwait_us, rstv_slave_wakes, rstv_slave_compiles;

#if defined(RSTV) && defined(__arm__)

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

#define RING 64
static volatile u32 s_ring[RING];
static volatile u32 s_posted, s_done; // slices posted by the main thread / run by the slave thread
static volatile int s_waiting;        // the slave thread sleeps on s_cond
static volatile int s_idle;           // the slave's last slice ended in an idle loop
static volatile int s_kick, s_stop;
static pthread_t s_thread;
static int s_started;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;

static u64 now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}

/* ---- I/O lock: recursive ticket lock (fair: a thread waiting gets it before the holder can take it again; the main
   thread releases and retakes it around every master slice). Owner = thread pointer register ---- */
static volatile u32 s_next, s_serving;
static volatile u32 s_owner;
static int s_depth;

void rstv_io_lock(void)
{
	const u32 me = rstv_thread_tp();
	if (s_owner == me) {
		s_depth++;
		return;
	}
	const u32 t = __sync_fetch_and_add(&s_next, 1);
	if (s_serving != t) {
		const u64 t0 = now_us();
		unsigned n = 0;
		while (s_serving != t) {
			if (++n > 20000) {
				sched_yield();
				n = 0;
			} else
				__asm__ volatile("" ::: "memory");
		}
		rstv_slave_lockwait_us += (unsigned)(now_us() - t0);
	}
	__sync_synchronize();
	s_owner = me;
	s_depth = 1;
}

void rstv_io_unlock(void)
{
	if (--s_depth == 0) {
		s_owner = 0;
		__sync_synchronize();
		s_serving = s_serving + 1;
	}
}

int rstv_io_held(void)
{
	return s_owner == rstv_thread_tp();
}

/* ---- ADR-138: compile requests of the slave thread ---- */
static void *volatile s_creq_inst, *volatile s_creq_parent, *volatile s_creq_res;
static volatile u32 s_creq_pc;
static volatile int s_creq; /* 0 none, 1 pending, 2 done */

void *rstv_slave_compile_proxy(void *inst, u32 pc, void *parent)
{
	const int held = rstv_io_held(), depth = held ? s_depth : 0;
	if (held) { /* the main thread's compile takes the lock */
		s_depth = 1;
		rstv_io_unlock();
	}
	s_creq_inst = inst;
	s_creq_pc = pc;
	s_creq_parent = parent;
	__sync_synchronize();
	s_creq = 1;
	unsigned n = 0;
	while (s_creq != 2) {
		if (++n > 20000) {
			sched_yield();
			n = 0;
		} else
			__asm__ volatile("" ::: "memory");
	}
	__sync_synchronize();
	void *r = s_creq_res;
	s_creq = 0;
	if (held) {
		rstv_io_lock();
		s_depth = depth;
	}
	rstv_slave_compiles++;
	return r;
}

void rstv_slave_service(void)
{
	if (s_creq != 1 || rstv_thread_tp() == rstv_slave_tp)
		return;
	__sync_synchronize();
	extern void *SH2DynCompileFor(void *inst, u32 pc, void *parent);
	s_creq_res = SH2DynCompileFor(s_creq_inst, s_creq_pc, s_creq_parent);
	__sync_synchronize();
	s_creq = 2;
}

void rstv_slave_set_idle(int idle)
{
	if (idle && !s_idle) {
		// the slave just finished its work: the master may be sleeping in a loop that waits for it (end that sleep)
		extern void SH2DynWake(SH2_struct *context);
		SH2DynWake(MSH2);
	}
	s_idle = idle;
}

static void wake(void)
{
	pthread_mutex_lock(&s_mtx);
	pthread_cond_signal(&s_cond);
	pthread_mutex_unlock(&s_mtx);
	rstv_slave_wakes++;
}

static void *slave_thread(void *arg)
{
	(void)arg;
#ifdef __vita__
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_2);
	sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);
#endif
	rstv_slave_tp = rstv_thread_tp();
	__sync_synchronize();
	for (;;) {
		if (s_stop)
			break;
		const u32 posted = s_posted;
		if (s_done != posted) {
			__sync_synchronize();
			const u32 cycles = s_ring[s_done & (RING - 1)];
			const u64 t0 = now_us();
			SH2Exec(SSH2, cycles);
			rstv_slave_busy_us += (unsigned)(now_us() - t0);
			__sync_synchronize();
			s_done = s_done + 1;
			continue;
		}
		// caught up: spin a little (the next slice usually comes within a line), then sleep
		int got = 0;
		for (int k = 0; k < rstv_slave_spin && !got; k++) {
			__asm__ volatile("" ::: "memory");
			got = s_posted != s_done || s_kick || s_stop;
		}
		if (got) {
			s_kick = 0;
			continue;
		}
		pthread_mutex_lock(&s_mtx);
		s_waiting = 1;
		__sync_synchronize();
		while (s_posted == s_done && !s_kick && !s_stop)
			pthread_cond_wait(&s_cond, &s_mtx);
		s_waiting = 0;
		s_kick = 0;
		pthread_mutex_unlock(&s_mtx);
	}
	return NULL;
}

static void start_thread(void)
{
	if (s_started)
		return;
	s_stop = 0;
	s_posted = s_done = 0;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	if (pthread_create(&s_thread, &attr, slave_thread, NULL) == 0)
		s_started = 1;
	pthread_attr_destroy(&attr);
	while (s_started && !rstv_slave_tp)
		usleep(100);
}

void rstv_slave_kick(void)
{
	if (!rstv_slave_on)
		return;
	s_kick = 1;
	__sync_synchronize();
	if (s_waiting)
		wake();
}

void rstv_slave_post(u32 cycles)
{
	rstv_slave_service();
	s_ring[s_posted & (RING - 1)] = cycles;
	__sync_synchronize();
	s_posted = s_posted + 1;
	__sync_synchronize();
	const u32 lag = s_posted - s_done;
	if (s_waiting && (!s_idle || lag >= (u32)rstv_slave_lead))
		wake();
	if (lag > (u32)rstv_slave_lead) {
		const u64 t0 = now_us();
		rstv_io_unlock(); // the slave may need a device to finish its slice
		while (s_posted - s_done > (u32)rstv_slave_lead) {
			rstv_slave_service();
			if (s_waiting)
				wake();
			__asm__ volatile("" ::: "memory");
		}
		rstv_io_lock();
		rstv_slave_wait_us += (unsigned)(now_us() - t0);
	}
}

void rstv_slave_drain(void)
{
	if (!rstv_slave_on || s_done == s_posted)
		return;
	if (rstv_thread_tp() == rstv_slave_tp) {
#ifndef __vita__
		static int warned;
		if (!warned++) { fprintf(stderr, "rstv_slave: drain from the slave thread%c", 10); }
#endif
		return; // the slave itself (a device it wrote wants it idle): it is between instructions anyway
	}
	const u64 t0 = now_us();
	const int held = rstv_io_held(), depth = held ? s_depth : 0;
	if (held) { // release every level (a device handler may hold it inside another holder)
		s_depth = 1;
		rstv_io_unlock();
	}
	while (s_done != s_posted) {
		rstv_slave_service();
		if (s_waiting)
			wake();
		__asm__ volatile("" ::: "memory");
	}
	__sync_synchronize();
	if (held) {
		rstv_io_lock();
		s_depth = depth;
	}
	rstv_slave_wait_us += (unsigned)(now_us() - t0);
}

void rstv_slave_frame_begin(void)
{
	if (rstv_slave_cfg && !rstv_slave_on && SH2Core && SH2Core->id == 3 /* SH2CORE_DYNAMIC */) {
		extern int SH2DynUseOwnBlocks(SH2_struct *context);
		// ADR-136: the slave compiles into its own block cache from now on
		if (!SH2DynUseOwnBlocks(SSH2)) {
			rstv_slave_cfg = 0;
			return;
		}
		start_thread();
		if (s_started)
			rstv_slave_on = 1;
	}
	if (rstv_slave_on)
		rstv_io_lock();
}

void rstv_slave_frame_end(void)
{
	if (!rstv_slave_on)
		return;
	rstv_slave_drain();
	rstv_io_unlock();
}

void rstv_slave_debug(char *out, int size)
{
	snprintf(out, size, "posted %u done %u waiting %d idle %d kick %d lock %u/%u owner %08x (slave %08x) depth %d",
		(unsigned)s_posted, (unsigned)s_done, s_waiting, s_idle, s_kick, (unsigned)s_next, (unsigned)s_serving, (unsigned)s_owner, (unsigned)rstv_slave_tp, s_depth);
}

void rstv_slave_shutdown(void)
{
	if (!s_started)
		return;
	rstv_slave_drain();
	rstv_slave_on = 0;
	s_stop = 1;
	__sync_synchronize();
	wake();
	pthread_join(s_thread, NULL);
	s_started = 0;
	rstv_slave_tp = 0;
}

#else /* one thread only (x86 harness) */

void rstv_slave_frame_begin(void) {}
void rstv_slave_frame_end(void) {}
void rstv_slave_post(u32 cycles) { (void)cycles; }
void rstv_slave_drain(void) {}
void rstv_slave_kick(void) {}
void rstv_slave_shutdown(void) {}
void rstv_slave_set_idle(int idle) { (void)idle; }
void rstv_io_lock(void) {}
void *rstv_slave_compile_proxy(void *inst, u32 pc, void *parent) { (void)inst; (void)pc; (void)parent; return 0; }
void rstv_slave_service(void) {}
void rstv_io_unlock(void) {}
void rstv_slave_debug(char *out, int size) { if (size) out[0] = 0; }
int rstv_io_held(void) { return 0; }

#endif
