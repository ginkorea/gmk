/*
 * GMK/cpu — Scheduler init/destroy
 */
#include "gmk/sched.h"
#include <stdlib.h>
#include <string.h>

int gmk_sched_init(gmk_sched_t *s, uint32_t n_workers) {
    if (!s || n_workers == 0 || n_workers > GMK_MAX_WORKERS)
        return -1;

    memset(s, 0, sizeof(*s));
    s->n_workers = n_workers;
    atomic_init(&s->next_seq, 0);

    /* Initialize RQ */
    if (gmk_rq_init(&s->rq, GMK_RQ_DEFAULT_CAP) != 0)
        return -1;

    /* Initialize per-worker LQs */
    s->lqs = (gmk_lq_t *)calloc(n_workers, sizeof(gmk_lq_t));
    if (!s->lqs) {
        gmk_rq_destroy(&s->rq);
        return -1;
    }
    for (uint32_t i = 0; i < n_workers; i++) {
        if (gmk_lq_init(&s->lqs[i], GMK_LQ_DEFAULT_CAP) != 0) {
            for (uint32_t j = 0; j < i; j++)
                gmk_lq_destroy(&s->lqs[j]);
            free(s->lqs);
            gmk_rq_destroy(&s->rq);
            return -1;
        }
    }

    /* Initialize EVQ */
    if (gmk_evq_init(&s->evq, GMK_EVQ_DEFAULT_CAP) != 0) {
        for (uint32_t i = 0; i < n_workers; i++)
            gmk_lq_destroy(&s->lqs[i]);
        free(s->lqs);
        gmk_rq_destroy(&s->rq);
        return -1;
    }

    /* Initialize overflow bucket */
    if (gmk_ring_mpmc_init(&s->overflow, GMK_OVERFLOW_CAP,
                           sizeof(gmk_task_t)) != 0) {
        gmk_evq_destroy(&s->evq);
        for (uint32_t i = 0; i < n_workers; i++)
            gmk_lq_destroy(&s->lqs[i]);
        free(s->lqs);
        gmk_rq_destroy(&s->rq);
        return -1;
    }

    return 0;
}

void gmk_sched_destroy(gmk_sched_t *s) {
    if (!s) return;
    gmk_rq_destroy(&s->rq);
    if (s->lqs) {
        for (uint32_t i = 0; i < s->n_workers; i++)
            gmk_lq_destroy(&s->lqs[i]);
        free(s->lqs);
        s->lqs = NULL;
    }
    gmk_evq_destroy(&s->evq);
    gmk_ring_mpmc_destroy(&s->overflow);
}
