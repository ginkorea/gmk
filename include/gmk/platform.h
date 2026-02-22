/*
 * GMK/cpu — Platform abstractions
 * Cache line size, atomics, TSC, alignment macros.
 */
#ifndef GMK_PLATFORM_H
#define GMK_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

/* ── Cache line ──────────────────────────────────────────────── */
#define GMK_CACHE_LINE 64

/* ── Alignment ───────────────────────────────────────────────── */
#define GMK_ALIGN(n) __attribute__((aligned(n)))
#define GMK_ALIGNED_STRUCT(n) struct GMK_ALIGN(n)

/* ── Padding to fill a cache line ────────────────────────────── */
#define GMK_PAD_TO_CACHE_LINE(used) \
    char _pad[GMK_CACHE_LINE - ((used) % GMK_CACHE_LINE)]

/* ── Atomic helpers (wrapping C11 stdatomic) ─────────────────── */
#define gmk_atomic_load(p, order)          atomic_load_explicit(p, order)
#define gmk_atomic_store(p, val, order)    atomic_store_explicit(p, val, order)
#define gmk_atomic_add(p, val, order)      atomic_fetch_add_explicit(p, val, order)
#define gmk_atomic_sub(p, val, order)      atomic_fetch_sub_explicit(p, val, order)
#define gmk_atomic_cas_weak(p, exp, des, succ, fail) \
    atomic_compare_exchange_weak_explicit(p, exp, des, succ, fail)
#define gmk_atomic_cas_strong(p, exp, des, succ, fail) \
    atomic_compare_exchange_strong_explicit(p, exp, des, succ, fail)

/* ── TSC / monotonic clock ───────────────────────────────────── */
static inline uint64_t gmk_tsc(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ── Power-of-two helpers ────────────────────────────────────── */
static inline bool gmk_is_power_of_two(uint32_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

static inline uint32_t gmk_next_pow2(uint32_t x) {
    if (x <= 1) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

/* ── Compiler hints ──────────────────────────────────────────── */
#define gmk_likely(x)   __builtin_expect(!!(x), 1)
#define gmk_unlikely(x) __builtin_expect(!!(x), 0)

#define GMK_UNUSED __attribute__((unused))

#endif /* GMK_PLATFORM_H */
