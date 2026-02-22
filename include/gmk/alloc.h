/*
 * GMK/cpu — Arena, slab, block, bump allocators
 *
 * One large aligned_alloc arena at boot, subdivided:
 *   10% task slab, 2% trace slab, 68% block allocator, 20% bump.
 *
 * Slab: index-based free list, mutex-protected.
 * Block: 12 bins (32B to 64KB), each bin is a slab.
 * Bump: atomic offset, gmk_bump_reset sets offset to 0.
 */
#ifndef GMK_ALLOC_H
#define GMK_ALLOC_H

#include "platform.h"
#include <pthread.h>

/* ── Forward typedefs ────────────────────────────────────────── */
typedef struct gmk_arena gmk_arena_t;
typedef struct gmk_alloc gmk_alloc_t;

/* ── Arena: owns the big allocation ──────────────────────────── */
struct gmk_arena {
    uint8_t *base;
    size_t   size;
};

int  gmk_arena_init(gmk_arena_t *a, size_t size);
void gmk_arena_destroy(gmk_arena_t *a);

/* ── Slab allocator: fixed-size objects with free list ───────── */
typedef struct {
    uint8_t        *base;       /* start of slab memory  */
    size_t          total_size; /* total bytes available  */
    uint32_t        obj_size;   /* size of each object   */
    uint32_t        capacity;   /* max objects           */
    _Atomic(uint32_t) alloc_count; /* currently allocated */
    uint32_t        high_water; /* peak alloc_count      */
    int32_t        *free_list;  /* free list indices (-1 = end) */
    int32_t         free_head;  /* index of first free slot */
    pthread_mutex_t lock;
} gmk_slab_t;

int    gmk_slab_init(gmk_slab_t *s, void *mem, size_t mem_size, uint32_t obj_size);
void   gmk_slab_destroy(gmk_slab_t *s);
void  *gmk_slab_alloc(gmk_slab_t *s);
void   gmk_slab_free(gmk_slab_t *s, void *ptr);
uint32_t gmk_slab_used(const gmk_slab_t *s);

/* ── Block allocator: power-of-two bins ──────────────────────── */
#define GMK_BLOCK_BINS     12   /* 32, 64, 128, ..., 65536 */
#define GMK_BLOCK_MIN_SIZE 32
#define GMK_BLOCK_MAX_SIZE 65536

typedef struct {
    gmk_slab_t bins[GMK_BLOCK_BINS];
    uint8_t   *base;
    size_t     total_size;
} gmk_block_t;

int    gmk_block_init(gmk_block_t *b, void *mem, size_t mem_size);
void   gmk_block_destroy(gmk_block_t *b);
void  *gmk_block_alloc(gmk_block_t *b, uint32_t size);
void   gmk_block_free(gmk_block_t *b, void *ptr, uint32_t size);

/* ── Bump allocator: atomic offset ───────────────────────────── */
typedef struct {
    uint8_t            *base;
    size_t              size;
    _Atomic(uint32_t)   offset;
    uint32_t            high_water;
} gmk_bump_t;

int    gmk_bump_init(gmk_bump_t *b, void *mem, size_t mem_size);
void  *gmk_bump_alloc(gmk_bump_t *b, uint32_t size);
void   gmk_bump_reset(gmk_bump_t *b);
uint32_t gmk_bump_used(const gmk_bump_t *b);

/* ── Payload refcount header (hidden before payload data) ────── */
typedef struct {
    _Atomic(uint32_t) refcount;
    uint32_t          size;     /* payload data size (excl. header) */
} gmk_payload_hdr_t;

/* Allocate a refcounted payload (refcount=1). Returns pointer past header. */
void  *gmk_payload_alloc(gmk_alloc_t *a, uint32_t size);
/* Increment refcount. */
void   gmk_payload_retain(void *payload);
/* Decrement refcount. Frees when it reaches 0. Returns 1 if freed, 0 if still live. */
int    gmk_payload_release(gmk_alloc_t *a, void *payload);

/* ── Unified allocator ───────────────────────────────────────── */
struct gmk_alloc {
    gmk_arena_t  arena;
    gmk_slab_t   task_slab;    /* for gmk_task_t-sized objects */
    gmk_slab_t   trace_slab;   /* for trace events             */
    gmk_block_t  block;        /* variable-size allocations    */
    gmk_bump_t   bump;         /* transient per-tick           */
    _Atomic(uint64_t) total_alloc_bytes;
    _Atomic(uint64_t) total_alloc_fails;
};

int    gmk_alloc_init(gmk_alloc_t *a, size_t arena_size);
void   gmk_alloc_destroy(gmk_alloc_t *a);
void  *gmk_alloc(gmk_alloc_t *a, uint32_t size);
void   gmk_free(gmk_alloc_t *a, void *ptr, uint32_t size);
void  *gmk_bump(gmk_alloc_t *a, uint32_t size);
void   gmk_bump_reset_all(gmk_alloc_t *a);

#endif /* GMK_ALLOC_H */
