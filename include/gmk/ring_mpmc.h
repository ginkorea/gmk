/*
 * GMK/cpu — MPMC lock-free ring buffer (Vyukov bounded queue)
 * Multi-producer, multi-consumer. Used for RQ sub-queues and channel rings.
 * Per-slot sequence numbers. Power-of-two capacity. No ABA problem.
 */
#ifndef GMK_RING_MPMC_H
#define GMK_RING_MPMC_H

#include "platform.h"

/* Per-slot cell: sequence + data */
typedef struct {
    _Atomic(uint32_t) seq;
    uint8_t           data[];  /* flexible array member */
} gmk_mpmc_cell_t;

typedef struct {
    _Atomic(uint32_t) head GMK_ALIGN(GMK_CACHE_LINE);
    _Atomic(uint32_t) tail GMK_ALIGN(GMK_CACHE_LINE);
    uint32_t cap;        /* must be power of two */
    uint32_t mask;       /* cap - 1              */
    uint32_t elem_size;  /* bytes per element    */
    uint32_t cell_size;  /* sizeof(seq) + elem_size, aligned */
    uint8_t *buf;        /* cell storage          */
} gmk_ring_mpmc_t;

/* Initialize ring. cap must be power of two. Returns 0 on success. */
int  gmk_ring_mpmc_init(gmk_ring_mpmc_t *r, uint32_t cap, uint32_t elem_size);
void gmk_ring_mpmc_destroy(gmk_ring_mpmc_t *r);

/* Push one element. Returns 0 on success, -1 if full. */
int  gmk_ring_mpmc_push(gmk_ring_mpmc_t *r, const void *elem);

/* Pop one element. Returns 0 on success, -1 if empty. */
int  gmk_ring_mpmc_pop(gmk_ring_mpmc_t *r, void *elem);

/* Approximate count. */
uint32_t gmk_ring_mpmc_count(const gmk_ring_mpmc_t *r);

/* Initialize ring using pre-allocated buffer (no internal malloc). */
int gmk_ring_mpmc_init_buf(gmk_ring_mpmc_t *r, uint32_t cap,
                            uint32_t elem_size, void *buf, size_t buf_size);

#endif /* GMK_RING_MPMC_H */
