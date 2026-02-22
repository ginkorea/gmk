/*
 * GMK/cpu — Kernel boot/halt + gmk_kernel_t
 *
 * gmk_boot: init arena → init sched → init channels → register modules
 *           → init modules → start workers.
 * gmk_halt: stop workers → fini modules → cleanup.
 */
#ifndef GMK_BOOT_H
#define GMK_BOOT_H

#include "types.h"
#include "alloc.h"
#include "trace.h"
#include "metrics.h"
#include "sched.h"
#include "chan.h"
#include "module.h"
#include "worker.h"

/* ── Boot configuration ──────────────────────────────────────── */
typedef struct {
    size_t      arena_size;   /* total arena bytes (default 64MB) */
    uint32_t    n_workers;    /* worker thread count (default 4)  */
    uint32_t    n_tenants;    /* tenant count (default 1)         */
} gmk_boot_cfg_t;

#define GMK_DEFAULT_ARENA_SIZE  (64ULL * 1024 * 1024)
#define GMK_DEFAULT_WORKERS     4
#define GMK_DEFAULT_TENANTS     1

/* ── Kernel state ────────────────────────────────────────────── */
struct gmk_kernel {
    gmk_alloc_t       alloc;
    gmk_trace_t       trace;
    gmk_metrics_t     metrics;
    gmk_sched_t       sched;
    gmk_chan_reg_t     chan;
    gmk_module_reg_t   modules;
    gmk_worker_pool_t pool;
    gmk_boot_cfg_t    cfg;
    _Atomic(bool)      running;
    _Atomic(uint32_t)  tick;
};

/* Boot the kernel. modules_arr is an array of modules to register.
   Returns 0 on success. */
int  gmk_boot(gmk_kernel_t *k, const gmk_boot_cfg_t *cfg,
              gmk_module_t **modules_arr, uint32_t n_modules);

/* Halt the kernel gracefully. */
void gmk_halt(gmk_kernel_t *k);

/* Submit a task to the kernel (from external code). */
int  gmk_submit(gmk_kernel_t *k, gmk_task_t *task);

/* Advance the kernel tick (for simulation/event-driven mode). */
void gmk_tick_advance(gmk_kernel_t *k);

#endif /* GMK_BOOT_H */
