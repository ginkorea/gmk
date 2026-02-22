/*
 * GMK/cpu — Worker thread pool
 *
 * Hosted: N pthreads, park via pthread_cond_timedwait.
 * Freestanding: N CPUs, park via sti;hlt, wake via LAPIC IPI.
 */
#ifndef GMK_WORKER_H
#define GMK_WORKER_H

#include "types.h"
#include "sched.h"
#include "module.h"

#ifdef GMK_FREESTANDING
#include "arch/thread.h"
#else
#include <pthread.h>
#endif

typedef struct {
    uint32_t        id;
#ifdef GMK_FREESTANDING
    uint32_t        cpu_id;     /* LAPIC ID for this worker's CPU */
    gmk_kthread_t   kthread;    /* kernel thread state */
#else
    pthread_t       thread;
#endif
    gmk_sched_t    *sched;
    gmk_module_reg_t *modules;
    gmk_alloc_t    *alloc;
    gmk_chan_reg_t  *chan;
    gmk_trace_t    *trace;
    gmk_metrics_t  *metrics;
    gmk_kernel_t   *kernel;

    _Atomic(bool)   running;
    _Atomic(bool)   parked;
#ifndef GMK_FREESTANDING
    pthread_mutex_t park_mutex;
    pthread_cond_t  park_cond;
#endif

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

#ifdef GMK_FREESTANDING
/* Exported worker loop for bare-metal entry (BSP/APs call directly) */
void *gmk_worker_loop(void *arg);
#endif

#endif /* GMK_WORKER_H */
