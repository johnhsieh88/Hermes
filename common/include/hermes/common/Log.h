/* Log.h — the Hermes logging macros. Format-compatible with TradingAlpha's
 * utils/Logger.hpp so both projects' logs read identically:
 *
 *   YYYYMMDD HH:MM:SS:uuuuuu pid tid file:line     <LEVEL> msg
 *   20260720 13:45:01:123456 812 0x1f4a3 abox_graph.c:53   <DEBUG> >> src ch=2
 *
 * Fixed widths (file 12 · line 4), ANSI colors (file blue,
 * DEBUG cyan / INFO green / WARN yellow / ERROR red). C11-compatible (usable from the
 * pure-C abox library AND every C++ module — this is the ONE logging surface; new code
 * must use these macros, not raw fprintf).
 *
 * Level filter: HERMES_LOG_LEVEL = DEBUG|INFO|WARN|ERROR (default DEBUG), read once.
 *
 * RT caution: formatting + stderr I/O is NOT RT-safe (NFR-8). On RT paths these may
 * only appear behind an explicit dev gate (e.g. HERMES_ABOX_TRACE) — never unconditionally.
 */
#ifndef HERMES_COMMON_LOG_H
#define HERMES_COMMON_LOG_H

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HM_LOG_DEBUG_LVL = 0, HM_LOG_INFO_LVL = 1, HM_LOG_WARN_LVL = 2, HM_LOG_ERROR_LVL = 3 };

static inline int hm_log_min_level(void) {
    static int lvl = -1;                       /* read HERMES_LOG_LEVEL once */
    if (lvl < 0) {
        const char* e = getenv("HERMES_LOG_LEVEL");
        lvl = HM_LOG_DEBUG_LVL;
        if (e) {
            if (!strcmp(e, "INFO"))  lvl = HM_LOG_INFO_LVL;
            if (!strcmp(e, "WARN") || !strcmp(e, "WARNING")) lvl = HM_LOG_WARN_LVL;
            if (!strcmp(e, "ERROR")) lvl = HM_LOG_ERROR_LVL;
        }
    }
    return lvl;
}

static inline void hm_log_impl(int level, const char* file, int line,
                               const char* fmt, ...) {
    if (level < hm_log_min_level()) return;

    /* wall clock, TradingAlpha format: YYYYMMDD HH:MM:SS:uuuuuu (localtime_r — reentrant) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmb;
    localtime_r(&ts.tv_sec, &tmb);

    /* file basename, truncated/padded to 12 like the C++ Logger */
    const char* base = strrchr(file, '/');
    base = base ? base + 1 : file;

    static const char* kName[]  = { "DEBUG", "INFO", "WARN", "ERROR" };
    static const char* kColor[] = { "\033[36m", "\033[32m", "\033[33m", "\033[31m" };

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* one fprintf → one write: lines from different threads stay whole (no mutex) */
    fprintf(stderr,
            "%04d%02d%02d %02d:%02d:%02d:%06ld %d %p "
            "\033[34m%-12.12s:%-4d\033[0m %s<%s> \033[0m%s\n",
            tmb.tm_year + 1900, tmb.tm_mon + 1, tmb.tm_mday,
            tmb.tm_hour, tmb.tm_min, tmb.tm_sec, (long)(ts.tv_nsec / 1000),
            (int)getpid(), (void*)pthread_self(),
            base, line, kColor[level], kName[level], msg);
}

#define HM_LOG_DEBUG(...) hm_log_impl(HM_LOG_DEBUG_LVL, __FILE__, __LINE__, __VA_ARGS__)
#define HM_LOG_INFO(...)  hm_log_impl(HM_LOG_INFO_LVL,  __FILE__, __LINE__, __VA_ARGS__)
#define HM_LOG_WARN(...)  hm_log_impl(HM_LOG_WARN_LVL,  __FILE__, __LINE__, __VA_ARGS__)
#define HM_LOG_ERROR(...) hm_log_impl(HM_LOG_ERROR_LVL, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* HERMES_COMMON_LOG_H */
