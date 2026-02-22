/*
 * GMK/cpu — Ticket spinlock for freestanding mode
 */
#ifndef GMK_ARCH_SPINLOCK_H
#define GMK_ARCH_SPINLOCK_H

#include <stdint.h>
#include <stdatomic.h>

typedef struct {
    _Atomic(uint32_t) next;
    _Atomic(uint32_t) serving;
} gmk_spinlock_t;

static inline void gmk_spinlock_init(gmk_spinlock_t *l) {
    atomic_store_explicit(&l->next, 0, memory_order_relaxed);
    atomic_store_explicit(&l->serving, 0, memory_order_relaxed);
}

static inline void gmk_spinlock_acquire(gmk_spinlock_t *l) {
    uint32_t ticket = atomic_fetch_add_explicit(&l->next, 1, memory_order_relaxed);
    while (atomic_load_explicit(&l->serving, memory_order_acquire) != ticket) {
        __builtin_ia32_pause();
    }
}

static inline void gmk_spinlock_release(gmk_spinlock_t *l) {
    uint32_t current = atomic_load_explicit(&l->serving, memory_order_relaxed);
    atomic_store_explicit(&l->serving, current + 1, memory_order_release);
}

static inline void gmk_spinlock_destroy(gmk_spinlock_t *l) {
    (void)l; /* nothing to do */
}

#endif /* GMK_ARCH_SPINLOCK_H */
