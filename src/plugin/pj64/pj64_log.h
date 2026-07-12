#ifndef PJ64_LOG_H
#define PJ64_LOG_H

#include <stdbool.h>

#ifndef SOFTRDP_ENABLE_LOG
#define SOFTRDP_ENABLE_LOG 0
#endif

#ifndef SOFTRDP_ENABLE_PERF_LOG
#define SOFTRDP_ENABLE_PERF_LOG 0
#endif

#ifndef SOFTRDP_ENABLE_PERF_OVERLAY
#define SOFTRDP_ENABLE_PERF_OVERLAY 0
#endif

/* Auto-enable main logging if performance logging is requested */
#if SOFTRDP_ENABLE_PERF_LOG && !SOFTRDP_ENABLE_LOG
#undef SOFTRDP_ENABLE_LOG
#define SOFTRDP_ENABLE_LOG 1
#endif

#if SOFTRDP_ENABLE_LOG
#define PJ64_LOG_ENABLED 1
void pj64_log_open(void);
void pj64_log_close(void);
void pj64_log_printf(const char *fmt, ...);
bool pj64_log_is_open(void);
#else
#define PJ64_LOG_ENABLED 0
static inline void pj64_log_open(void) {}
static inline void pj64_log_close(void) {}
static inline bool pj64_log_is_open(void) { return false; }
#define pj64_log_printf(...) ((void)0)
#endif

#endif
