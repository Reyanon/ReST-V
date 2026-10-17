#ifndef RSTV_PLATFORM_LOG_H
#define RSTV_PLATFORM_LOG_H

// Built-in UDP logger
// - Port 18194. Receive on the PC with `nc -lu 18194` or `python tools/udp_log.py`.
// - Target address set at build time with RSTV_LOG_HOST. Broadcast if unset.
// - Without RSTV_ENABLE_LOG (release build) every call compiles away.

#define RSTV_LOG_PORT 18194

#ifdef RSTV_ENABLE_LOG

void rstv_log_init(void);
void rstv_log_shutdown(void);
void rstv_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define RSTV_LOG(...) rstv_log_printf(__VA_ARGS__)

#else

#define rstv_log_init()     ((void)0)
#define rstv_log_shutdown() ((void)0)
#define RSTV_LOG(...)       ((void)0)

#endif

#endif
