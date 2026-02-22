/*
 * GMK/cpu — Event Queue: bounded binary min-heap
 *
 * Ordered by key = (tick << 32) | (priority << 16) | seq.
 * Lock-protected (heap ops aren't trivially lock-free).
 * Drain limit per check: GMK_EVQ_DRAIN_LIMIT.
 */
#include "gmk/sched.h"
#ifdef GMK_FREESTANDING
#include "../../arch/x86_64/boot_alloc.h"
#else
#include <stdlib.h>
#include <string.h>
#endif

static inline uint64_t evq_key(const gmk_task_t *t, uint32_t seq) {
    uint32_t tick = (uint32_t)t->meta0;  /* meta0 used as tick for EVQ */
    uint32_t prio = GMK_PRIORITY(t->flags);
    return ((uint64_t)tick << 32) | ((uint64_t)prio << 16) | (uint64_t)seq;
}

static void heap_sift_up(gmk_evq_entry_t *heap, uint32_t idx) {
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        if (heap[idx].key < heap[parent].key) {
            gmk_evq_entry_t tmp = heap[idx];
            heap[idx] = heap[parent];
            heap[parent] = tmp;
            idx = parent;
        } else {
            break;
        }
    }
}

static void heap_sift_down(gmk_evq_entry_t *heap, uint32_t count, uint32_t idx) {
    for (;;) {
        uint32_t smallest = idx;
        uint32_t left  = 2 * idx + 1;
        uint32_t right = 2 * idx + 2;

        if (left < count && heap[left].key < heap[smallest].key)
            smallest = left;
        if (right < count && heap[right].key < heap[smallest].key)
            smallest = right;

        if (smallest != idx) {
            gmk_evq_entry_t tmp = heap[idx];
            heap[idx] = heap[smallest];
            heap[smallest] = tmp;
            idx = smallest;
        } else {
            break;
        }
    }
}

int gmk_evq_init(gmk_evq_t *evq, uint32_t cap) {
    if (!evq || cap == 0) return -1;

#ifdef GMK_FREESTANDING
    evq->heap = (gmk_evq_entry_t *)boot_calloc(cap, sizeof(gmk_evq_entry_t));
#else
    evq->heap = (gmk_evq_entry_t *)calloc(cap, sizeof(gmk_evq_entry_t));
#endif
    if (!evq->heap) return -1;

    evq->count    = 0;
    evq->cap      = cap;
    evq->next_seq = 0;
    gmk_lock_init(&evq->lock);
    return 0;
}

void gmk_evq_destroy(gmk_evq_t *evq) {
    if (!evq) return;
#ifdef GMK_FREESTANDING
    boot_free(evq->heap);
#else
    free(evq->heap);
#endif
    evq->heap = NULL;
    gmk_lock_destroy(&evq->lock);
}

int gmk_evq_push(gmk_evq_t *evq, const gmk_task_t *task) {
    if (!evq || !task) return -1;

    gmk_lock_acquire(&evq->lock);

    if (evq->count >= evq->cap) {
        gmk_lock_release(&evq->lock);
        return -1; /* full */
    }

    uint32_t seq = evq->next_seq++;
    uint32_t idx = evq->count++;
    evq->heap[idx].key  = evq_key(task, seq);
    evq->heap[idx].task = *task;
    evq->heap[idx].task.seq = seq;

    heap_sift_up(evq->heap, idx);

    gmk_lock_release(&evq->lock);
    return 0;
}

int gmk_evq_pop_due(gmk_evq_t *evq, uint32_t current_tick, gmk_task_t *task) {
    if (!evq || !task) return -1;

    gmk_lock_acquire(&evq->lock);

    if (evq->count == 0) {
        gmk_lock_release(&evq->lock);
        return -1;
    }

    /* Check if the minimum element is due */
    uint32_t entry_tick = (uint32_t)(evq->heap[0].key >> 32);
    if (entry_tick > current_tick) {
        gmk_lock_release(&evq->lock);
        return -1; /* not due yet */
    }

    *task = evq->heap[0].task;

    /* Remove root: move last element to root and sift down */
    evq->count--;
    if (evq->count > 0) {
        evq->heap[0] = evq->heap[evq->count];
        heap_sift_down(evq->heap, evq->count, 0);
    }

    gmk_lock_release(&evq->lock);
    return 0;
}

uint32_t gmk_evq_count(const gmk_evq_t *evq) {
    if (!evq) return 0;
    return evq->count;
}
