/*
 * GMK/cpu — _gmk_enqueue + _gmk_yield + gmk_yield/gmk_yield_at
 *
 * Single enqueue core: assigns monotonic seq, routes to LQ or RQ.
 * All scheduling paths funnel through _gmk_enqueue.
 */
#include "gmk/sched.h"

int _gmk_enqueue(gmk_sched_t *s, gmk_task_t *task, int worker_id) {
    if (!s || !task) return -1;

    /* Assign monotonic sequence number */
    task->seq = gmk_atomic_add(&s->next_seq, 1, memory_order_relaxed);

    /* Route: if worker_id specified, try LQ first */
    if (worker_id >= 0 && (uint32_t)worker_id < s->n_workers) {
        if (gmk_lq_push(&s->lqs[worker_id], task) == 0)
            return 0;
    }

    /* Fall back to RQ */
    return gmk_rq_push(&s->rq, task);
}

int _gmk_yield(gmk_sched_t *s, gmk_task_t *task, int worker_id,
               uint32_t max_yields) {
    if (!s || !task) return -1;

    /* Increment yield count */
    task->yield_count++;

    /* Circuit breaker: check max yields */
    if (max_yields == 0) max_yields = GMK_DEFAULT_MAX_YIELDS;
    if (task->yield_count > max_yields) {
        return GMK_FAIL(GMK_ERR_YIELD_LIMIT);
    }

    /* Try LQ yield reserve first */
    if (worker_id >= 0 && (uint32_t)worker_id < s->n_workers) {
        if (gmk_lq_push_yield(&s->lqs[worker_id], task) == 0)
            return 0;
    }

    /* Try overflow bucket */
    if (gmk_ring_mpmc_push(&s->overflow, task) == 0)
        return 0;

    /* System is catastrophically overloaded */
    return GMK_FAIL(GMK_ERR_YIELD_OVERFLOW);
}

void gmk_yield_impl(gmk_sched_t *s, gmk_task_t *task, int worker_id) {
    if (!s || !task) return;
    _gmk_yield(s, task, worker_id, GMK_DEFAULT_MAX_YIELDS);
}

void gmk_yield_at_impl(gmk_sched_t *s, gmk_task_t *task, int worker_id,
                       uint32_t phase) {
    if (!s || !task) return;
    task->meta0 = phase;
    _gmk_yield(s, task, worker_id, GMK_DEFAULT_MAX_YIELDS);
}
