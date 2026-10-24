/*  ReST-V (ADR-136): the slave SH-2 on its own thread.

    The main thread posts each SH-2 slice's cycle count and runs the master; the slave thread runs the same slices for the
    slave, at most rstv_slave_lead slices behind (never ahead). The main thread waits only when the slave falls further
    behind, and drains it at the end of every frame and before it touches the slave's state (reset, start/stop).
    Devices are not thread safe: everything outside work RAM goes through one recursive lock (rstv_io_lock), which the
    main thread holds all frame except while the master executes. Off by default; one thread then (unchanged).

    This file is part of ReST-V, based on Yabause. GPL-2.0-or-later. */
#ifndef RSTV_SLAVE_H
#define RSTV_SLAVE_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int rstv_slave_cfg;     /* wanted (frontend / harness option) */
extern volatile int rstv_slave_on; /* the slave thread runs the slave now */
extern int rstv_slave_lead;    /* slices the slave may lag */
extern int rstv_slave_spin;    /* busy-wait iterations before the slave thread sleeps */

void rstv_slave_frame_begin(void); /* main: start of a frame (starts the thread when wanted, takes the I/O lock) */
void rstv_slave_frame_end(void);   /* main: end of a frame (drains, releases the I/O lock) */
void rstv_slave_post(u32 cycles);  /* main: one slice for the slave */
void rstv_slave_drain(void);       /* main: wait until the slave ran every posted slice */
void rstv_slave_kick(void);        /* wake the slave thread if it sleeps (an interrupt for it) */
void rstv_slave_shutdown(void);    /* stop the thread (exit, reset) */
void rstv_slave_set_idle(int idle);/* slave thread: the slice ended in an idle loop */

/* ADR-138: the slave thread's block compiles run on the main thread (the only writer of the code region) */
void *rstv_slave_compile_proxy(void *inst, u32 pc, void *parent); /* slave thread: compile and wait */
void rstv_slave_service(void);    /* main: run a pending compile request */
extern unsigned rstv_slave_compiles;

void rstv_io_lock(void);
void rstv_io_unlock(void);
int rstv_io_held(void);           /* this thread holds the I/O lock */

/* statistics (performance line): slave busy time, waits of the main thread, lock waits (us since the last read) */
extern unsigned rstv_slave_busy_us, rstv_slave_wait_us, rstv_slave_lockwait_us, rstv_slave_wakes;

#define RSTV_IO_LOCK() do { if (rstv_slave_on) rstv_io_lock(); } while (0)
#define RSTV_IO_UNLOCK() do { if (rstv_slave_on) rstv_io_unlock(); } while (0)

#ifdef __cplusplus
}
#endif

#endif
