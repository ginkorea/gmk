/*
 * GMK/cpu — Boot-time bump allocator (never frees)
 */
#include "boot_alloc.h"

static uint8_t *ba_base;
static size_t   ba_size;
static size_t   ba_offset;

void boot_alloc_init(void *base, size_t size) {
    ba_base   = (uint8_t *)base;
    ba_size   = size;
    ba_offset = 0;
}

void *boot_aligned_alloc(size_t alignment, size_t size) {
    if (!ba_base || size == 0) return (void *)0;

    /* Align offset up */
    size_t aligned = (ba_offset + alignment - 1) & ~(alignment - 1);
    if (aligned + size > ba_size) return (void *)0;

    void *ptr = ba_base + aligned;
    ba_offset = aligned + size;

    /* Zero-initialize */
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < size; i++) p[i] = 0;

    return ptr;
}

void *boot_calloc(size_t count, size_t size) {
    return boot_aligned_alloc(8, count * size);
}

void boot_free(void *ptr) {
    (void)ptr; /* no-op: kernel-lifetime allocations */
}

size_t boot_alloc_used(void) {
    return ba_offset;
}

size_t boot_alloc_total(void) {
    return ba_size;
}
