/*
 * GMK/cpu — Bump allocator (atomic offset)
 * Fast, lock-free. Reset sets offset to 0.
 */
#include "gmk/alloc.h"

int gmk_bump_init(gmk_bump_t *b, void *mem, size_t mem_size) {
    if (!b || !mem || mem_size == 0) return -1;
    b->base = (uint8_t *)mem;
    b->size = mem_size;
    b->high_water = 0;
    atomic_init(&b->offset, 0);
    return 0;
}

void *gmk_bump_alloc(gmk_bump_t *b, uint32_t size) {
    if (!b || size == 0) return NULL;

    /* Align to 8 bytes */
    size = (size + 7u) & ~7u;

    uint32_t off = gmk_atomic_add(&b->offset, size, memory_order_relaxed);
    if (off + size > (uint32_t)b->size) {
        /* Roll back */
        gmk_atomic_sub(&b->offset, size, memory_order_relaxed);
        return NULL;
    }

    /* Update high water (relaxed, best-effort) */
    uint32_t new_off = off + size;
    if (new_off > b->high_water)
        b->high_water = new_off;

    return b->base + off;
}

void gmk_bump_reset(gmk_bump_t *b) {
    if (b) {
        gmk_atomic_store(&b->offset, 0, memory_order_release);
    }
}

uint32_t gmk_bump_used(const gmk_bump_t *b) {
    return gmk_atomic_load(&b->offset, memory_order_relaxed);
}
