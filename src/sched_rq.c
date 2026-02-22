/*
 * GMK/cpu — Ready Queue: 4 priority sub-queues with weighted pop
 *
 * Weights: P0=8, P1=4, P2=2, P3=1.
 * Each weight-batch pops that many tasks before moving to the next priority.
 */
#include "gmk/sched.h"
#include <string.h>

int gmk_rq_init(gmk_rq_t *rq, uint32_t cap_per_queue) {
    if (!rq) return -1;
    memset(rq, 0, sizeof(*rq));

    for (int i = 0; i < GMK_PRIORITY_COUNT; i++) {
        if (gmk_ring_mpmc_init(&rq->queues[i], cap_per_queue,
                               sizeof(gmk_task_t)) != 0) {
            for (int j = 0; j < i; j++)
                gmk_ring_mpmc_destroy(&rq->queues[j]);
            return -1;
        }
        rq->pop_counters[i] = 0;
    }
    return 0;
}

void gmk_rq_destroy(gmk_rq_t *rq) {
    if (!rq) return;
    for (int i = 0; i < GMK_PRIORITY_COUNT; i++)
        gmk_ring_mpmc_destroy(&rq->queues[i]);
}

int gmk_rq_push(gmk_rq_t *rq, const gmk_task_t *task) {
    if (!rq || !task) return -1;
    uint32_t prio = GMK_PRIORITY(task->flags);
    if (prio >= GMK_PRIORITY_COUNT) prio = GMK_PRIO_LOW;
    return gmk_ring_mpmc_push(&rq->queues[prio], task);
}

int gmk_rq_pop(gmk_rq_t *rq, gmk_task_t *task) {
    if (!rq || !task) return -1;

    /* Weighted pop: try priorities in order with their weights */
    static const uint32_t weights[GMK_PRIORITY_COUNT] = {
        GMK_WEIGHT_P0, GMK_WEIGHT_P1, GMK_WEIGHT_P2, GMK_WEIGHT_P3
    };

    for (int prio = 0; prio < GMK_PRIORITY_COUNT; prio++) {
        if (rq->pop_counters[prio] < weights[prio]) {
            if (gmk_ring_mpmc_pop(&rq->queues[prio], task) == 0) {
                rq->pop_counters[prio]++;
                return 0;
            }
        }
    }

    /* Reset counters and try again from the top */
    for (int prio = 0; prio < GMK_PRIORITY_COUNT; prio++)
        rq->pop_counters[prio] = 0;

    /* Second pass: try any non-empty queue */
    for (int prio = 0; prio < GMK_PRIORITY_COUNT; prio++) {
        if (gmk_ring_mpmc_pop(&rq->queues[prio], task) == 0) {
            rq->pop_counters[prio]++;
            return 0;
        }
    }

    return -1; /* all queues empty */
}

uint32_t gmk_rq_count(const gmk_rq_t *rq) {
    if (!rq) return 0;
    uint32_t total = 0;
    for (int i = 0; i < GMK_PRIORITY_COUNT; i++)
        total += gmk_ring_mpmc_count(&rq->queues[i]);
    return total;
}
