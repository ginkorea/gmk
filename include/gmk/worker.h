/*
 * GMK/cpu — Worker thread pool
 *
 * N pthreads, each running: pop LQ → pop overflow → pop RQ → check EVQ
 *   → dispatch → park.
 * Park: pthread_cond_timedwait with 1ms timeout.
 * Wake: pthread_cond_signal.
 */
#ifndef GMK_WORKER_H
#define GMK_WORKER_H

#include "types.h"
#include "sched.h"
#include "module.h"
#include <pthread.h>

typedef struct {
    uint32_t        id;
    pthread_t       thread;
    gmk_sched_t    *sched;
    gmk_module_reg_t *modules;
    gmk_alloc_t    *alloc;
    gmk_chan_reg_t  *chan;
    gmk_trace_t    *trace;
    gmk_metrics_t  *metrics;
    gmk_kernel_t   *kernel;

    _Atomic(bool)   running;
    _Atomic(bool)   parked;
    pthread_mutex_t park_mutex;
    pthread_cond_t  park_cond;

    _Atomic(uint64_t) tasks_dispatched;
    _Atomic(uint32_t) tick;
} gmk_worker_t;

typedef struct {
    gmk_worker_t   *workers;
    uint32_t         n_workers;
    gmk_sched_t    *sched;
    gmk_module_reg_t *modules;
    gmk_alloc_t    *alloc;
    gmk_chan_reg_t  *chan;
    gmk_trace_t    *trace;
    gmk_metrics_t  *metrics;
    gmk_kernel_t   *kernel;
} gmk_worker_pool_t;

int  gmk_worker_pool_init(gmk_worker_pool_t *pool, uint32_t n_workers,
                          gmk_sched_t *sched, gmk_module_reg_t *modules,
                          gmk_alloc_t *alloc, gmk_chan_reg_t *chan,
                          gmk_trace_t *trace, gmk_metrics_t *metrics,
                          gmk_kernel_t *kernel);
int  gmk_worker_pool_start(gmk_worker_pool_t *pool);
void gmk_worker_pool_stop(gmk_worker_pool_t *pool);
void gmk_worker_pool_destroy(gmk_worker_pool_t *pool);
void gmk_worker_wake(gmk_worker_t *w);
void gmk_worker_wake_all(gmk_worker_pool_t *pool);

#endif /* GMK_WORKER_H */
