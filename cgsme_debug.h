#ifndef __cgsme_DEBUG_H__
#define __cgsme_DEBUG_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// optional timing + debug logging (no-ops when cgsme_DEBUG is off)

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
#ifdef cgsme_BUILD_DLL
#define CGSME_API __declspec(dllexport)
#else
#define CGSME_API
#endif
#else
#define CGSME_API
#endif

#ifdef cgsme_DEBUG
    CGSME_API void cgsme_init_debug(void);
    CGSME_API void cgsme_shutdown_debug(void);
    CGSME_API void cgsme_set_enabled(bool enabled);
    CGSME_API bool cgsme_get_enabled(void);

    CGSME_API uint64_t cgsme_now_us(void);
    CGSME_API uint64_t cgsme_now_cycles(void);
    CGSME_API void cgsme_profile_record(const char *name, uint64_t elapsed_us, uint64_t elapsed_cycles);
    CGSME_API void cgsme_profile_set_thresholds(uint64_t us_threshold, uint64_t cycles_threshold);
    CGSME_API void cgsme_profile_set_warning_percent(double pct);
    CGSME_API void cgsme_profile_set_runinfo(uint32_t layers, uint32_t width, uint32_t length, uint32_t seed, uint32_t fulness);

    // quick mode skips per-scope profiling for benchmarking
    CGSME_API void cgsme_set_quick_mode(bool enabled);
    CGSME_API bool cgsme_quick_mode_enabled(void);

// scope profiling via gcc/clang cleanup attr
#ifdef __GNUC__
    typedef struct
    {
        const char *name;
        uint64_t start_us;
        uint64_t start_cycles;
    } __cgsme_profile_scope_t;
    static inline void __cgsme_profile_scope_end(__cgsme_profile_scope_t *p)
    {
        if (p && p->name)
            cgsme_profile_record(p->name, cgsme_now_us() - p->start_us, cgsme_now_cycles() - p->start_cycles);
    }
    static inline void __cgsme_profile_scope_init(__cgsme_profile_scope_t *p)
    {
        p->name = __func__;
        p->start_us = 0;
        p->start_cycles = 0;
        if (!cgsme_quick_mode_enabled())
        {
            p->start_us = cgsme_now_us();
            p->start_cycles = cgsme_now_cycles();
        }
        else
        {
            p->name = NULL;
        }
    }
#define CGSME_PROFILE_FUNC()                                                                                 \
    __cgsme_profile_scope_t __cgsme_prof __attribute__((cleanup(__cgsme_profile_scope_end))) = {NULL, 0, 0}; \
    __cgsme_profile_scope_init(&__cgsme_prof);
#else
#define CGSME_PROFILE_FUNC()                                                               \
    uint64_t __cgsme_profile_start_us = (cgsme_quick_mode_enabled() ? 0 : cgsme_now_us()); \
    uint64_t __cgsme_profile_start_cycles = (cgsme_quick_mode_enabled() ? 0 : cgsme_now_cycles());
#endif

    CGSME_API void cgsme_log(const char *fmt, ...);

#else
#ifndef CGSME_PROFILE_FUNC
#define CGSME_PROFILE_FUNC() ((void)0)
#endif

// release no-ops
static inline void cgsme_init_debug(void) {}
static inline void cgsme_shutdown_debug(void) {}
static inline void cgsme_set_enabled(bool enabled) { (void)enabled; }
static inline bool cgsme_get_enabled(void) { return false; }
static inline uint64_t cgsme_now_us(void) { return 0; }
static inline uint64_t cgsme_now_cycles(void) { return 0; }
static inline void cgsme_profile_record(const char *name, uint64_t elapsed_us, uint64_t elapsed_cycles)
{
    (void)name;
    (void)elapsed_us;
    (void)elapsed_cycles;
}
static inline void cgsme_profile_set_thresholds(uint64_t us_threshold, uint64_t cycles_threshold)
{
    (void)us_threshold;
    (void)cycles_threshold;
}
static inline void cgsme_profile_set_warning_percent(double pct) { (void)pct; }
static inline void cgsme_profile_set_runinfo(uint32_t layers, uint32_t width, uint32_t length, uint32_t seed, uint32_t fulness)
{
    (void)layers;
    (void)width;
    (void)length;
    (void)seed;
    (void)fulness;
}
static inline void cgsme_log(const char *fmt, ...) { (void)fmt; }

#endif // cgsme_DEBUG

#ifdef __cplusplus
}
#endif

#endif // __cgsme_DEBUG_H__
