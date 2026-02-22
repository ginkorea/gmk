/*
 * GMK/cpu — MPMC ring buffer implementation (Vyukov bounded queue)
 * Each slot has a sequence number. Producers and consumers use CAS on
 * head/tail, checking the slot's sequence to ensure correctness.
 */
#include "gmk/ring_mpmc.h"
#include <stdlib.h>
#include <string.h>

static inline uint32_t cell_stride(uint32_t elem_size) {
    /* cell = 4 bytes (seq) + elem_size, rounded up to 8-byte alignment */
    uint32_t raw = sizeof(_Atomic(uint32_t)) + elem_size;
    return (raw + 7u) & ~7u;
}

static inline gmk_mpmc_cell_t *cell_at(const gmk_ring_mpmc_t *r, uint32_t idx) {
    return (gmk_mpmc_cell_t *)(r->buf + (size_t)idx * r->cell_size);
}

int gmk_ring_mpmc_init(gmk_ring_mpmc_t *r, uint32_t cap, uint32_t elem_size) {
    if (!r || !gmk_is_power_of_two(cap) || elem_size == 0)
        return -1;

    r->cap       = cap;
    r->mask      = cap - 1;
    r->elem_size = elem_size;
    r->cell_size = cell_stride(elem_size);

    size_t buf_bytes = (size_t)cap * r->cell_size;
    r->buf = (uint8_t *)aligned_alloc(GMK_CACHE_LINE, buf_bytes);
    if (!r->buf) return -1;
    memset(r->buf, 0, buf_bytes);

    /* Initialize per-slot sequence numbers */
    for (uint32_t i = 0; i < cap; i++) {
        gmk_mpmc_cell_t *c = cell_at(r, i);
        atomic_init(&c->seq, i);
    }

    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return 0;
}

int gmk_ring_mpmc_init_buf(gmk_ring_mpmc_t *r, uint32_t cap,
                            uint32_t elem_size, void *buf, size_t buf_size) {
    if (!r || !gmk_is_power_of_two(cap) || elem_size == 0 || !buf)
        return -1;

    r->cap       = cap;
    r->mask      = cap - 1;
    r->elem_size = elem_size;
    r->cell_size = cell_stride(elem_size);

    if (buf_size < (size_t)cap * r->cell_size)
        return -1;

    r->buf = (uint8_t *)buf;
    memset(r->buf, 0, (size_t)cap * r->cell_size);

    for (uint32_t i = 0; i < cap; i++) {
        gmk_mpmc_cell_t *c = cell_at(r, i);
        atomic_init(&c->seq, i);
    }

    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return 0;
}

void gmk_ring_mpmc_destroy(gmk_ring_mpmc_t *r) {
    if (r && r->buf) {
        free(r->buf);
        r->buf = NULL;
    }
}

int gmk_ring_mpmc_push(gmk_ring_mpmc_t *r, const void *elem) {
    uint32_t tail;
    gmk_mpmc_cell_t *c;

    tail = gmk_atomic_load(&r->tail, memory_order_relaxed);
    for (;;) {
        c = cell_at(r, tail & r->mask);
        uint32_t seq = gmk_atomic_load(&c->seq, memory_order_acquire);
        int32_t diff = (int32_t)seq - (int32_t)tail;

        if (diff == 0) {
            /* Slot is ready for writing. Try to claim it. */
            if (gmk_atomic_cas_weak(&r->tail, &tail, tail + 1,
                                     memory_order_relaxed,
                                     memory_order_relaxed))
                break;
        } else if (diff < 0) {
            /* Queue is full */
            return -1;
        } else {
            /* Another producer claimed this slot, reload tail */
            tail = gmk_atomic_load(&r->tail, memory_order_relaxed);
        }
    }

    /* Write data and publish */
    memcpy(c->data, elem, r->elem_size);
    gmk_atomic_store(&c->seq, tail + 1, memory_order_release);
    return 0;
}

int gmk_ring_mpmc_pop(gmk_ring_mpmc_t *r, void *elem) {
    uint32_t head;
    gmk_mpmc_cell_t *c;

    head = gmk_atomic_load(&r->head, memory_order_relaxed);
    for (;;) {
        c = cell_at(r, head & r->mask);
        uint32_t seq = gmk_atomic_load(&c->seq, memory_order_acquire);
        int32_t diff = (int32_t)seq - (int32_t)(head + 1);

        if (diff == 0) {
            /* Slot has data. Try to claim it. */
            if (gmk_atomic_cas_weak(&r->head, &head, head + 1,
                                     memory_order_relaxed,
                                     memory_order_relaxed))
                break;
        } else if (diff < 0) {
            /* Queue is empty */
            return -1;
        } else {
            /* Another consumer claimed this slot, reload head */
            head = gmk_atomic_load(&r->head, memory_order_relaxed);
        }
    }

    /* Read data and release slot */
    memcpy(elem, c->data, r->elem_size);
    gmk_atomic_store(&c->seq, head + r->cap, memory_order_release);
    return 0;
}

uint32_t gmk_ring_mpmc_count(const gmk_ring_mpmc_t *r) {
    uint32_t tail = gmk_atomic_load(&r->tail, memory_order_acquire);
    uint32_t head = gmk_atomic_load(&r->head, memory_order_acquire);
    return tail - head;
}
