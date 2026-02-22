/*
 * GMK/cpu — Scheduler: RQ, LQ, EVQ, _gmk_enqueue
 *
 * RQ: 4 MPMC sub-queues (one per priority). Weighted pop.
 * LQ: SPSC per worker. Yield watermark at 75%.
 * EVQ: bounded binary min-heap, lock-protected.
 * Overflow: MPMC ring for yield overflow.
 */
#ifndef GMK_SCHED_H
#define GMK_SCHED_H

#include "types.h"
#include "ring_spsc.h"
#include "ring_mpmc.h"
#include "lock.h"

/* ── Ready Queue (RQ): 4 priority sub-queues ─────────────────── */
typedef struct {
    gmk_ring_mpmc_t queues[GMK_PRIORITY_COUNT];
    uint32_t        pop_counters[GMK_PRIORITY_COUNT]; /* for weighted pop */
} gmk_rq_t;

int  gmk_rq_init(gmk_rq_t *rq, uint32_t cap_per_queue);
void gmk_rq_destroy(gmk_rq_t *rq);
int  gmk_rq_push(gmk_rq_t *rq, const gmk_task_t *task);
int  gmk_rq_pop(gmk_rq_t *rq, gmk_task_t *task);
uint32_t gmk_rq_count(const gmk_rq_t *rq);

/* ── Local Queue (LQ): per-worker SPSC ───────────────────────── */
typedef struct {
    gmk_ring_spsc_t ring;
    uint32_t        yield_watermark;  /* normal push limit (75% of cap) */
    uint32_t        cap;
} gmk_lq_t;

int  gmk_lq_init(gmk_lq_t *lq, uint32_t cap);
void gmk_lq_destroy(gmk_lq_t *lq);
int  gmk_lq_push(gmk_lq_t *lq, const gmk_task_t *task);        /* normal */
int  gmk_lq_push_yield(gmk_lq_t *lq, const gmk_task_t *task);  /* yield reserve */
int  gmk_lq_pop(gmk_lq_t *lq, gmk_task_t *task);
uint32_t gmk_lq_count(const gmk_lq_t *lq);

/* ── Event Queue (EVQ): bounded binary min-heap ──────────────── */
typedef struct {
    uint64_t      key;   /* (meta0 as tick << 32) | (priority << 16) | seq */
    gmk_task_t    task;
} gmk_evq_entry_t;

typedef struct {
    gmk_evq_entry_t *heap;
    uint32_t          count;
    uint32_t          cap;
    uint32_t          next_seq;
    gmk_lock_t        lock;
} gmk_evq_t;

int  gmk_evq_init(gmk_evq_t *evq, uint32_t cap);
void gmk_evq_destroy(gmk_evq_t *evq);
int  gmk_evq_push(gmk_evq_t *evq, const gmk_task_t *task);
int  gmk_evq_pop_due(gmk_evq_t *evq, uint32_t current_tick, gmk_task_t *task);
uint32_t gmk_evq_count(const gmk_evq_t *evq);

/* ── Scheduler aggregate ─────────────────────────────────────── */
struct gmk_sched {
    gmk_rq_t        rq;
    gmk_lq_t       *lqs;            /* array of LQs, one per worker */
    gmk_evq_t       evq;
    gmk_ring_mpmc_t overflow;        /* yield overflow bucket        */
    uint32_t         n_workers;
    _Atomic(uint32_t) next_seq;      /* monotonic sequence counter   */
};

int  gmk_sched_init(gmk_sched_t *s, uint32_t n_workers);
void gmk_sched_destroy(gmk_sched_t *s);

/* Core enqueue: assigns seq, routes to LQ (if worker_id >= 0) or RQ. */
int  _gmk_enqueue(gmk_sched_t *s, gmk_task_t *task, int worker_id);

/* Yield: increment yield_count, circuit breaker, try LQ → overflow → error. */
int  _gmk_yield(gmk_sched_t *s, gmk_task_t *task, int worker_id,
                uint32_t max_yields);

/* Public yield wrappers. */
void gmk_yield_impl(gmk_sched_t *s, gmk_task_t *task, int worker_id);
void gmk_yield_at_impl(gmk_sched_t *s, gmk_task_t *task, int worker_id,
                       uint32_t phase);

#endif /* GMK_SCHED_H */
