/*
 * GMK/cpu — Power-of-two bin allocator
 * 12 bins: 32, 64, 128, 256, 512, 1K, 2K, 4K, 8K, 16K, 32K, 64K.
 * Each bin is a slab allocator.
 */
#include "gmk/alloc.h"
#ifndef GMK_FREESTANDING
#include <string.h>
#endif

static int bin_index(uint32_t size) {
    if (size <= GMK_BLOCK_MIN_SIZE) return 0;
    if (size > GMK_BLOCK_MAX_SIZE) return -1;

    /* Find the bin: round up to next power of two, then log2 - 5 */
    uint32_t rounded = gmk_next_pow2(size);
    int idx = __builtin_ctz(rounded) - 5; /* 32 = 2^5 → bin 0 */
    if (idx < 0) idx = 0;
    if (idx >= GMK_BLOCK_BINS) return -1;
    return idx;
}

int gmk_block_init(gmk_block_t *b, void *mem, size_t mem_size) {
    if (!b || !mem || mem_size == 0) return -1;

    memset(b, 0, sizeof(*b));
    b->base       = (uint8_t *)mem;
    b->total_size = mem_size;

    /*
     * Distribute memory across bins. Smaller bins get more objects
     * because they're used more frequently.
     */
    uint32_t weights[GMK_BLOCK_BINS] = {16, 12, 8, 6, 4, 2, 2, 2, 2, 2, 2, 2};
    uint32_t total_weight = 0;
    for (int i = 0; i < GMK_BLOCK_BINS; i++)
        total_weight += weights[i];

    uint8_t *ptr = b->base;
    size_t remaining = mem_size;

    for (int i = 0; i < GMK_BLOCK_BINS; i++) {
        size_t bin_mem = (mem_size * weights[i]) / total_weight;
        if (i == GMK_BLOCK_BINS - 1) bin_mem = remaining;
        if (bin_mem > remaining) bin_mem = remaining;

        uint32_t obj_size = GMK_BLOCK_MIN_SIZE << i;
        uint32_t aligned_obj = (obj_size + 7u) & ~7u;

        /* Check if bin has enough memory for at least 1 object */
        if (bin_mem < (size_t)(aligned_obj + sizeof(int32_t))) {
            /* Not enough memory for this bin — mark as empty */
            memset(&b->bins[i], 0, sizeof(b->bins[i]));
            b->bins[i].capacity = 0;
            b->bins[i].free_head = -1;
            gmk_lock_init(&b->bins[i].lock);
        } else {
            if (gmk_slab_init(&b->bins[i], ptr, bin_mem, obj_size) != 0) {
                /* Treat as empty bin, not a fatal error */
                memset(&b->bins[i], 0, sizeof(b->bins[i]));
                b->bins[i].capacity = 0;
                b->bins[i].free_head = -1;
                gmk_lock_init(&b->bins[i].lock);
            }
        }

        ptr += bin_mem;
        remaining -= bin_mem;
    }

    return 0;
}

void gmk_block_destroy(gmk_block_t *b) {
    if (!b) return;
    for (int i = 0; i < GMK_BLOCK_BINS; i++) {
        if (b->bins[i].capacity > 0)
            gmk_slab_destroy(&b->bins[i]);
        else
            gmk_lock_destroy(&b->bins[i].lock);
    }
    b->base = NULL;
}

void *gmk_block_alloc(gmk_block_t *b, uint32_t size) {
    if (!b || size == 0) return NULL;
    int idx = bin_index(size);
    if (idx < 0) return NULL;
    if (b->bins[idx].capacity == 0) return NULL;
    return gmk_slab_alloc(&b->bins[idx]);
}

void gmk_block_free(gmk_block_t *b, void *ptr, uint32_t size) {
    if (!b || !ptr || size == 0) return;
    int idx = bin_index(size);
    if (idx < 0) return;
    if (b->bins[idx].capacity == 0) return;
    gmk_slab_free(&b->bins[idx], ptr);
}
