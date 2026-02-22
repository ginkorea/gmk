/*
 * GMK/cpu — SPSC lock-free ring buffer
 * Single-producer, single-consumer. Used for LQ and trace rings.
 * Power-of-two capacity, mask-based indexing, elements copied via memcpy.
 */
#ifndef GMK_RING_SPSC_H
#define GMK_RING_SPSC_H

#include "platform.h"

typedef struct {
    _Atomic(uint32_t) head GMK_ALIGN(GMK_CACHE_LINE);
    _Atomic(uint32_t) tail GMK_ALIGN(GMK_CACHE_LINE);
    uint32_t cap;       /* must be power of two */
    uint32_t mask;      /* cap - 1              */
    uint32_t elem_size; /* bytes per element    */
    uint8_t *buf;       /* element storage      */
} gmk_ring_spsc_t;

/* Initialize ring. cap must be power of two. Returns 0 on success. */
int  gmk_ring_spsc_init(gmk_ring_spsc_t *r, uint32_t cap, uint32_t elem_size);
void gmk_ring_spsc_destroy(gmk_ring_spsc_t *r);

/* Push one element. Returns 0 on success, -1 if full. */
int  gmk_ring_spsc_push(gmk_ring_spsc_t *r, const void *elem);

/* Pop one element. Returns 0 on success, -1 if empty. */
int  gmk_ring_spsc_pop(gmk_ring_spsc_t *r, void *elem);

/* Current count of elements in the ring. */
uint32_t gmk_ring_spsc_count(const gmk_ring_spsc_t *r);

/* Is the ring full? */
bool gmk_ring_spsc_full(const gmk_ring_spsc_t *r);

/* Is the ring empty? */
bool gmk_ring_spsc_empty(const gmk_ring_spsc_t *r);

/* Initialize ring using pre-allocated buffer (no internal malloc). */
int gmk_ring_spsc_init_buf(gmk_ring_spsc_t *r, uint32_t cap,
                            uint32_t elem_size, void *buf);

#endif /* GMK_RING_SPSC_H */
