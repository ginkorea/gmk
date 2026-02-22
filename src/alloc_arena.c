/*
 * GMK/cpu — Arena init/destroy (aligned_alloc)
 */
#include "gmk/alloc.h"
#include <stdlib.h>
#include <string.h>

int gmk_arena_init(gmk_arena_t *a, size_t size) {
    if (!a || size == 0) return -1;

    a->base = (uint8_t *)aligned_alloc(GMK_CACHE_LINE, size);
    if (!a->base) return -1;

    memset(a->base, 0, size);
    a->size = size;
    return 0;
}

void gmk_arena_destroy(gmk_arena_t *a) {
    if (a && a->base) {
        free(a->base);
        a->base = NULL;
        a->size = 0;
    }
}
