#ifndef RSTV_PLATFORM_CLOCK_H
#define RSTV_PLATFORM_CLOCK_H

// Raise ARM to 444MHz, bus 222MHz, GPU 222MHz, GPU Xbar 166MHz.
void rstv_clock_boost(void);

// Log current clocks over UDP. If measure is nonzero, also measure the real clock (about 45ms, startup only)
void rstv_clock_log(int measure);

// Estimate the real ARM clock (MHz) with a fixed-cycle loop (about 45-180ms)
int rstv_clock_measure_arm_mhz(void);

#ifdef RSTV_CLOCK_SWEEP
// clock step test for diagnostics (-DRSTV_CLOCK_SWEEP)
void rstv_clock_sweep(void);
#endif

#endif
