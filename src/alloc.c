/*
 * GMK/cpu — Unified allocator routing
 *
 * Arena subdivision:
 *   10% task slab (48-byte objects)
 *    2% trace slab (32-byte objects)
 *   68% block allocator (power-of-two bins)
 *   20% bump allocator
 */
#include "gmk/alloc.h"
#include "gmk/types.h"
#include <string.h>

int gmk_alloc_init(gmk_alloc_t *a, size_t arena_size) {
    if (!a || arena_size < 4096) return -1;

    memset(a, 0, sizeof(*a));

    if (gmk_arena_init(&a->arena, arena_size) != 0)
        return -1;

    atomic_init(&a->total_alloc_bytes, 0);
    atomic_init(&a->total_alloc_fails, 0);

    uint8_t *base = a->arena.base;
    size_t task_size  = (arena_size * 10) / 100;
    size_t trace_size = (arena_size *  2) / 100;
    size_t bump_size  = (arena_size * 20) / 100;
    size_t block_size = arena_size - task_size - trace_size - bump_size;

    uint8_t *task_mem  = base;
    uint8_t *trace_mem = task_mem  + task_size;
    uint8_t *block_mem = trace_mem + trace_size;
    uint8_t *bump_mem  = block_mem + block_size;

    if (gmk_slab_init(&a->task_slab, task_mem, task_size, sizeof(gmk_task_t)) != 0)
        goto fail;
    if (gmk_slab_init(&a->trace_slab, trace_mem, trace_size, sizeof(gmk_trace_ev_t)) != 0)
        goto fail;
    if (gmk_block_init(&a->block, block_mem, block_size) != 0)
        goto fail;
    if (gmk_bump_init(&a->bump, bump_mem, bump_size) != 0)
        goto fail;

    return 0;

fail:
    gmk_arena_destroy(&a->arena);
    return -1;
}

void gmk_alloc_destroy(gmk_alloc_t *a) {
    if (!a) return;
    gmk_slab_destroy(&a->task_slab);
    gmk_slab_destroy(&a->trace_slab);
    gmk_block_destroy(&a->block);
    /* bump has no destroy */
    gmk_arena_destroy(&a->arena);
}

void *gmk_alloc(gmk_alloc_t *a, uint32_t size) {
    if (!a || size == 0) return NULL;

    void *ptr = NULL;

    /* Route: task-sized → slab, small/medium → block, else fail */
    if (size <= sizeof(gmk_task_t) && size > 0) {
        /* Try task slab first for task-sized allocations */
        if (size == sizeof(gmk_task_t)) {
            ptr = gmk_slab_alloc(&a->task_slab);
        }
        if (!ptr) {
            ptr = gmk_block_alloc(&a->block, size);
        }
    } else if (size <= GMK_BLOCK_MAX_SIZE) {
        ptr = gmk_block_alloc(&a->block, size);
    }

    if (ptr) {
        gmk_atomic_add(&a->total_alloc_bytes, size, memory_order_relaxed);
    } else {
        gmk_atomic_add(&a->total_alloc_fails, 1, memory_order_relaxed);
    }
    return ptr;
}

void gmk_free(gmk_alloc_t *a, void *ptr, uint32_t size) {
    if (!a || !ptr || size == 0) return;

    uint8_t *p = (uint8_t *)ptr;

    /* Determine which region this pointer belongs to */
    uint8_t *task_base  = a->task_slab.base;
    uint8_t *trace_base = a->trace_slab.base;
    uint8_t *block_base = a->block.base;

    if (p >= task_base && p < task_base + a->task_slab.total_size) {
        gmk_slab_free(&a->task_slab, ptr);
    } else if (p >= trace_base && p < trace_base + a->trace_slab.total_size) {
        gmk_slab_free(&a->trace_slab, ptr);
    } else if (p >= block_base && p < block_base + a->block.total_size) {
        gmk_block_free(&a->block, ptr, size);
    }
    /* Bump allocator has no individual free */
}

void *gmk_bump(gmk_alloc_t *a, uint32_t size) {
    if (!a) return NULL;
    return gmk_bump_alloc(&a->bump, size);
}

void gmk_bump_reset_all(gmk_alloc_t *a) {
    if (!a) return;
    gmk_bump_reset(&a->bump);
}

/* ── Payload refcounting ────────────────────────────────────── */

void *gmk_payload_alloc(gmk_alloc_t *a, uint32_t size) {
    if (!a || size == 0) return NULL;

    uint32_t total = (uint32_t)sizeof(gmk_payload_hdr_t) + size;
    void *mem = gmk_alloc(a, total);
    if (!mem) return NULL;

    gmk_payload_hdr_t *hdr = (gmk_payload_hdr_t *)mem;
    atomic_init(&hdr->refcount, 1);
    hdr->size = size;
    return (uint8_t *)mem + sizeof(gmk_payload_hdr_t);
}

void gmk_payload_retain(void *payload) {
    if (!payload) return;
    gmk_payload_hdr_t *hdr = (gmk_payload_hdr_t *)
        ((uint8_t *)payload - sizeof(gmk_payload_hdr_t));
    gmk_atomic_add(&hdr->refcount, 1, memory_order_relaxed);
}

int gmk_payload_release(gmk_alloc_t *a, void *payload) {
    if (!payload) return 0;
    gmk_payload_hdr_t *hdr = (gmk_payload_hdr_t *)
        ((uint8_t *)payload - sizeof(gmk_payload_hdr_t));
    uint32_t old = atomic_fetch_sub_explicit(&hdr->refcount, 1,
                                             memory_order_acq_rel);
    if (old == 1) {
        /* Last reference — free the backing allocation */
        gmk_free(a, hdr, (uint32_t)sizeof(gmk_payload_hdr_t) + hdr->size);
        return 1;
    }
    return 0;
}
