/*
 * GMK/cpu — Fixed-size slab allocator with free list
 * Mutex-protected. Index-based free list.
 */
#include "gmk/alloc.h"
#include <stdlib.h>
#include <string.h>

int gmk_slab_init(gmk_slab_t *s, void *mem, size_t mem_size, uint32_t obj_size) {
    if (!s || !mem || obj_size == 0) return -1;

    /* Align object size up to 8 bytes */
    obj_size = (obj_size + 7u) & ~7u;

    /* Reserve space for free list at the end of the memory region */
    uint32_t capacity = (uint32_t)(mem_size / (obj_size + sizeof(int32_t)));
    if (capacity == 0) return -1;

    s->base       = (uint8_t *)mem;
    s->total_size = mem_size;
    s->obj_size   = obj_size;
    s->capacity   = capacity;
    s->high_water = 0;
    atomic_init(&s->alloc_count, 0);

    /* Free list lives after the slab objects */
    s->free_list = (int32_t *)(s->base + (size_t)capacity * obj_size);
    s->free_head = 0;

    /* Initialize free list: each slot points to the next */
    for (uint32_t i = 0; i < capacity - 1; i++) {
        s->free_list[i] = (int32_t)(i + 1);
    }
    s->free_list[capacity - 1] = -1; /* end of list */

    pthread_mutex_init(&s->lock, NULL);
    return 0;
}

void gmk_slab_destroy(gmk_slab_t *s) {
    if (s) {
        pthread_mutex_destroy(&s->lock);
        s->base = NULL;
    }
}

void *gmk_slab_alloc(gmk_slab_t *s) {
    if (!s) return NULL;

    pthread_mutex_lock(&s->lock);

    if (s->free_head < 0) {
        pthread_mutex_unlock(&s->lock);
        return NULL; /* full */
    }

    int32_t idx = s->free_head;
    s->free_head = s->free_list[idx];

    uint32_t count = gmk_atomic_add(&s->alloc_count, 1, memory_order_relaxed) + 1;
    if (count > s->high_water)
        s->high_water = count;

    pthread_mutex_unlock(&s->lock);

    return s->base + (size_t)idx * s->obj_size;
}

void gmk_slab_free(gmk_slab_t *s, void *ptr) {
    if (!s || !ptr) return;

    uint8_t *p = (uint8_t *)ptr;
    if (p < s->base) return;

    size_t offset = (size_t)(p - s->base);
    uint32_t idx = (uint32_t)(offset / s->obj_size);
    if (idx >= s->capacity) return;

    pthread_mutex_lock(&s->lock);

    s->free_list[idx] = s->free_head;
    s->free_head = (int32_t)idx;
    gmk_atomic_sub(&s->alloc_count, 1, memory_order_relaxed);

    pthread_mutex_unlock(&s->lock);
}

uint32_t gmk_slab_used(const gmk_slab_t *s) {
    return gmk_atomic_load(&s->alloc_count, memory_order_relaxed);
}
