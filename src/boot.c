/*
 * GMK/cpu — gmk_boot/gmk_halt sequence
 */
#include "gmk/boot.h"
#include <string.h>

int gmk_boot(gmk_kernel_t *k, const gmk_boot_cfg_t *cfg,
             gmk_module_t **modules_arr, uint32_t n_modules) {
    if (!k) return -1;

    memset(k, 0, sizeof(*k));
    atomic_init(&k->running, false);
    atomic_init(&k->tick, 0);

    /* Apply config with defaults */
    if (cfg) {
        k->cfg = *cfg;
    } else {
        k->cfg.arena_size = GMK_DEFAULT_ARENA_SIZE;
        k->cfg.n_workers  = GMK_DEFAULT_WORKERS;
        k->cfg.n_tenants  = GMK_DEFAULT_TENANTS;
    }
    if (k->cfg.arena_size == 0) k->cfg.arena_size = GMK_DEFAULT_ARENA_SIZE;
    if (k->cfg.n_workers == 0)  k->cfg.n_workers  = GMK_DEFAULT_WORKERS;
    if (k->cfg.n_tenants == 0)  k->cfg.n_tenants  = GMK_DEFAULT_TENANTS;

    /* 1. Arena + allocator */
    if (gmk_alloc_init(&k->alloc, k->cfg.arena_size) != 0)
        goto fail_alloc;

    /* 2. Trace */
    if (gmk_trace_init(&k->trace, k->cfg.n_tenants) != 0)
        goto fail_trace;

    /* 3. Metrics */
    if (gmk_metrics_init(&k->metrics, k->cfg.n_tenants) != 0)
        goto fail_metrics;

    /* 4. Scheduler */
    if (gmk_sched_init(&k->sched, k->cfg.n_workers) != 0)
        goto fail_sched;

    /* 5. Channel registry */
    if (gmk_chan_reg_init(&k->chan, &k->sched, &k->alloc, &k->trace, &k->metrics) != 0)
        goto fail_chan;

    /* 6. Module registry */
    if (gmk_module_reg_init(&k->modules, &k->chan, &k->trace, &k->metrics) != 0)
        goto fail_mod;

    /* 7. Register modules */
    for (uint32_t i = 0; i < n_modules; i++) {
        if (modules_arr && modules_arr[i]) {
            int rc = gmk_module_register(&k->modules, modules_arr[i]);
            if (rc != 0) goto fail_register;
        }
    }

    /* 8. Initialize modules */
    gmk_ctx_t boot_ctx = {
        .task      = NULL,
        .alloc     = &k->alloc,
        .chan       = &k->chan,
        .trace     = &k->trace,
        .metrics   = &k->metrics,
        .sched     = &k->sched,
        .kernel    = k,
        .worker_id = UINT32_MAX,
        .tick      = 0,
    };
    if (gmk_module_init_all(&k->modules, &boot_ctx) != 0)
        goto fail_init;

    /* 9. Worker pool */
    if (gmk_worker_pool_init(&k->pool, k->cfg.n_workers, &k->sched,
                             &k->modules, &k->alloc, &k->chan,
                             &k->trace, &k->metrics, k) != 0)
        goto fail_pool;

    /* 10. Start workers */
    if (gmk_worker_pool_start(&k->pool) != 0)
        goto fail_start;

    gmk_atomic_store(&k->running, true, memory_order_release);

    /* Trace boot event */
    gmk_trace_write_force(&k->trace, 0, GMK_EV_BOOT, 0,
                          k->cfg.n_workers, (uint32_t)(k->cfg.arena_size >> 20));

    return 0;

fail_start:
    gmk_worker_pool_destroy(&k->pool);
fail_pool:
    gmk_module_fini_all(&k->modules, &boot_ctx);
fail_init:
fail_register:
    gmk_module_reg_destroy(&k->modules);
fail_mod:
    gmk_chan_reg_destroy(&k->chan);
fail_chan:
    gmk_sched_destroy(&k->sched);
fail_sched:
    gmk_metrics_destroy(&k->metrics);
fail_metrics:
    gmk_trace_destroy(&k->trace);
fail_trace:
    gmk_alloc_destroy(&k->alloc);
fail_alloc:
    return -1;
}

void gmk_halt(gmk_kernel_t *k) {
    if (!k) return;

    gmk_atomic_store(&k->running, false, memory_order_release);

    /* Trace halt event */
    gmk_trace_write_force(&k->trace, 0, GMK_EV_HALT, 0, 0, 0);

    /* 1. Stop workers */
    gmk_worker_pool_stop(&k->pool);
    gmk_worker_pool_destroy(&k->pool);

    /* 2. Finalize modules */
    gmk_ctx_t halt_ctx = {
        .task      = NULL,
        .alloc     = &k->alloc,
        .chan       = &k->chan,
        .trace     = &k->trace,
        .metrics   = &k->metrics,
        .sched     = &k->sched,
        .kernel    = k,
        .worker_id = UINT32_MAX,
        .tick      = 0,
    };
    gmk_module_fini_all(&k->modules, &halt_ctx);

    /* 3. Cleanup */
    gmk_module_reg_destroy(&k->modules);
    gmk_chan_reg_destroy(&k->chan);
    gmk_sched_destroy(&k->sched);
    gmk_metrics_destroy(&k->metrics);
    gmk_trace_destroy(&k->trace);
    gmk_alloc_destroy(&k->alloc);
}

int gmk_submit(gmk_kernel_t *k, gmk_task_t *task) {
    if (!k || !task) return -1;
    if (!gmk_atomic_load(&k->running, memory_order_acquire))
        return GMK_FAIL(GMK_ERR_CLOSED);

    int rc = _gmk_enqueue(&k->sched, task, -1);
    if (rc == 0) {
        gmk_metric_inc(&k->metrics, task->tenant,
                      GMK_METRIC_TASKS_ENQUEUED, 1);
        /* Wake a worker */
        for (uint32_t i = 0; i < k->pool.n_workers; i++) {
            if (gmk_atomic_load(&k->pool.workers[i].parked,
                               memory_order_acquire)) {
                gmk_worker_wake(&k->pool.workers[i]);
                break;
            }
        }
    }
    return rc;
}

void gmk_tick_advance(gmk_kernel_t *k) {
    if (!k) return;
    uint32_t tick = gmk_atomic_add(&k->tick, 1, memory_order_release) + 1;
    for (uint32_t i = 0; i < k->pool.n_workers; i++)
        gmk_atomic_store(&k->pool.workers[i].tick, tick, memory_order_release);
}
