#ifndef TALOS_DEBUG_H
#define TALOS_DEBUG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// optional timing + debug logging (no-ops when TALOS_DEBUG is off)

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
#ifdef TALOS_BUILD_DLL
#define TALOS_API __declspec(dllexport)
#else
#define TALOS_API
#endif
#else
#define TALOS_API
#endif

#ifdef TALOS_DEBUG
    TALOS_API void talos_init_debug(void);
    TALOS_API void talos_shutdown_debug(void);
    TALOS_API void talos_set_enabled(bool enabled);
    TALOS_API bool talos_get_enabled(void);

    TALOS_API uint64_t talos_now_us(void);
    TALOS_API uint64_t talos_now_cycles(void);
    TALOS_API void talos_profile_record(const char *name, uint64_t elapsed_us, uint64_t elapsed_cycles);
    TALOS_API void talos_profile_set_thresholds(uint64_t us_threshold, uint64_t cycles_threshold);
    TALOS_API void talos_profile_set_warning_percent(double pct);
    TALOS_API void talos_profile_set_runinfo(uint32_t layers, uint32_t width, uint32_t length, uint32_t seed, uint32_t fulness);

    // quick mode skips per-scope profiling for benchmarking
    TALOS_API void talos_set_quick_mode(bool enabled);
    TALOS_API bool talos_quick_mode_enabled(void);

// scope profiling via gcc/clang cleanup attr
#ifdef __GNUC__
    typedef struct
    {
        const char *name;
        uint64_t start_us;
        uint64_t start_cycles;
    } __talos_profile_scope_t;
    static inline void __talos_profile_scope_end(__talos_profile_scope_t *p)
    {
        if (p && p->name)
            talos_profile_record(p->name, talos_now_us() - p->start_us, talos_now_cycles() - p->start_cycles);
    }
    static inline void __talos_profile_scope_init(__talos_profile_scope_t *p)
    {
        p->name = __func__;
        p->start_us = 0;
        p->start_cycles = 0;
        if (!talos_quick_mode_enabled())
        {
            p->start_us = talos_now_us();
            p->start_cycles = talos_now_cycles();
        }
        else
        {
            p->name = NULL;
        }
    }
#define TALOS_PROFILE_FUNC()                                                                                 \
    __talos_profile_scope_t __talos_prof __attribute__((cleanup(__talos_profile_scope_end))) = {NULL, 0, 0}; \
    __talos_profile_scope_init(&__talos_prof);
#else
#define TALOS_PROFILE_FUNC()                                                               \
    uint64_t __talos_profile_start_us = (talos_quick_mode_enabled() ? 0 : talos_now_us()); \
    uint64_t __talos_profile_start_cycles = (talos_quick_mode_enabled() ? 0 : talos_now_cycles());
#endif

    TALOS_API void talos_log(const char *fmt, ...);

#else
#ifndef TALOS_PROFILE_FUNC
#define TALOS_PROFILE_FUNC() ((void)0)
#endif

// release no-ops
static inline void talos_init_debug(void) {}
static inline void talos_shutdown_debug(void) {}
static inline void talos_set_enabled(bool enabled) { (void)enabled; }
static inline bool talos_get_enabled(void) { return false; }
static inline uint64_t talos_now_us(void) { return 0; }
static inline uint64_t talos_now_cycles(void) { return 0; }
static inline void talos_profile_record(const char *name, uint64_t elapsed_us, uint64_t elapsed_cycles)
{
    (void)name;
    (void)elapsed_us;
    (void)elapsed_cycles;
}
static inline void talos_profile_set_thresholds(uint64_t us_threshold, uint64_t cycles_threshold)
{
    (void)us_threshold;
    (void)cycles_threshold;
}
static inline void talos_profile_set_warning_percent(double pct) { (void)pct; }
static inline void talos_profile_set_runinfo(uint32_t layers, uint32_t width, uint32_t length, uint32_t seed, uint32_t fulness)
{
    (void)layers;
    (void)width;
    (void)length;
    (void)seed;
    (void)fulness;
}
static inline void talos_log(const char *fmt, ...) { (void)fmt; }

#endif // TALOS_DEBUG

#ifdef __cplusplus
}
#endif

#endif // TALOS_DEBUG_H
