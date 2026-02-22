/*
 * GMK/cpu — Local Queue: SPSC per worker with yield watermark
 *
 * Normal push fails past 75% fill. Yield push uses full capacity.
 */
#include "gmk/sched.h"

int gmk_lq_init(gmk_lq_t *lq, uint32_t cap) {
    if (!lq) return -1;
    lq->cap = cap;
    lq->yield_watermark = cap - (cap * GMK_LQ_YIELD_RESERVE_PCT / 100);

    return gmk_ring_spsc_init(&lq->ring, cap, sizeof(gmk_task_t));
}

void gmk_lq_destroy(gmk_lq_t *lq) {
    if (!lq) return;
    gmk_ring_spsc_destroy(&lq->ring);
}

int gmk_lq_push(gmk_lq_t *lq, const gmk_task_t *task) {
    if (!lq || !task) return -1;

    /* Normal push: respect yield watermark */
    if (gmk_ring_spsc_count(&lq->ring) >= lq->yield_watermark)
        return -1;

    return gmk_ring_spsc_push(&lq->ring, task);
}

int gmk_lq_push_yield(gmk_lq_t *lq, const gmk_task_t *task) {
    if (!lq || !task) return -1;

    /* Yield push: use full capacity */
    return gmk_ring_spsc_push(&lq->ring, task);
}

int gmk_lq_pop(gmk_lq_t *lq, gmk_task_t *task) {
    if (!lq || !task) return -1;
    return gmk_ring_spsc_pop(&lq->ring, task);
}

uint32_t gmk_lq_count(const gmk_lq_t *lq) {
    if (!lq) return 0;
    return gmk_ring_spsc_count(&lq->ring);
}
