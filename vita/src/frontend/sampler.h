// Sampling profiler (for measurement). See sampler.c
#ifndef RSTV_SAMPLER_H
#define RSTV_SAMPLER_H

void rstv_sampler_start(void);
// histogram since the last report on one line (trailing newline)
void rstv_sampler_report(char *out, int size);

#endif
