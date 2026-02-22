/*
 * GMK/cpu — Worker thread pool tests
 */
#include "gmk/gmk.h"
#include "test_util.h"
#include <string.h>
#include <unistd.h>

/* ── Test handler that increments a counter ──────────────────── */
static _Atomic(int) work_counter;

static int inc_handler(gmk_ctx_t *ctx) {
    (void)ctx;
    gmk_atomic_add(&work_counter, 1, memory_order_relaxed);
    return GMK_OK;
}

static void test_basic_dispatch(void) {
    atomic_init(&work_counter, 0);

    gmk_alloc_t alloc;
    gmk_trace_t trace;
    gmk_metrics_t metrics;
    gmk_sched_t sched;
    gmk_chan_reg_t chan;
    gmk_module_reg_t modules;

    GMK_ASSERT_EQ(gmk_alloc_init(&alloc, 1024 * 1024), 0, "alloc init");
    GMK_ASSERT_EQ(gmk_trace_init(&trace, 1), 0, "trace init");
    GMK_ASSERT_EQ(gmk_metrics_init(&metrics, 1), 0, "metrics init");
    GMK_ASSERT_EQ(gmk_sched_init(&sched, 2), 0, "sched init");
    GMK_ASSERT_EQ(gmk_chan_reg_init(&chan, &sched, &alloc, &trace, &metrics), 0, "chan init");
    GMK_ASSERT_EQ(gmk_module_reg_init(&modules, &chan, &trace, &metrics), 0, "mod init");

    /* Register handler */
    gmk_handler_reg_t handlers[] = {
        { .type = 1, .fn = inc_handler, .name = "inc" },
    };
    gmk_module_t mod = {
        .name = "test", .handlers = handlers, .n_handlers = 1,
    };
    gmk_module_register(&modules, &mod);

    /* Start worker pool */
    gmk_worker_pool_t pool;
    GMK_ASSERT_EQ(gmk_worker_pool_init(&pool, 2, &sched, &modules,
                                        &alloc, &chan, &trace, &metrics, NULL),
                  0, "pool init");
    GMK_ASSERT_EQ(gmk_worker_pool_start(&pool), 0, "pool start");

    /* Enqueue 100 tasks */
    for (int i = 0; i < 100; i++) {
        gmk_task_t t;
        memset(&t, 0, sizeof(t));
        t.type = 1;
        _gmk_enqueue(&sched, &t, -1);
    }

    /* Wake workers */
    gmk_worker_wake_all(&pool);

    /* Wait for processing */
    for (int wait = 0; wait < 100; wait++) {
        if (gmk_atomic_load(&work_counter, memory_order_relaxed) >= 100)
            break;
        usleep(10000); /* 10ms */
    }

    int count = gmk_atomic_load(&work_counter, memory_order_relaxed);
    GMK_ASSERT_EQ(count, 100, "all 100 tasks dispatched");

    gmk_worker_pool_stop(&pool);
    gmk_worker_pool_destroy(&pool);
    gmk_module_reg_destroy(&modules);
    gmk_chan_reg_destroy(&chan);
    gmk_sched_destroy(&sched);
    gmk_metrics_destroy(&metrics);
    gmk_trace_destroy(&trace);
    gmk_alloc_destroy(&alloc);
}

/* ── Test yield within handler ───────────────────────────────── */
static _Atomic(int) phase_counter;

static int yield_handler(gmk_ctx_t *ctx) {
    if (ctx->task->meta0 == 0) {
        /* Phase 0: yield to phase 1 */
        gmk_yield_at_impl(ctx->sched, ctx->task, (int)ctx->worker_id, 1);
        return GMK_OK;
    } else {
        /* Phase 1: done */
        gmk_atomic_add(&phase_counter, 1, memory_order_relaxed);
        return GMK_OK;
    }
}

static void test_yield_flow(void) {
    atomic_init(&phase_counter, 0);

    gmk_alloc_t alloc;
    gmk_trace_t trace;
    gmk_metrics_t metrics;
    gmk_sched_t sched;
    gmk_chan_reg_t chan;
    gmk_module_reg_t modules;

    gmk_alloc_init(&alloc, 1024 * 1024);
    gmk_trace_init(&trace, 1);
    gmk_metrics_init(&metrics, 1);
    gmk_sched_init(&sched, 2);
    gmk_chan_reg_init(&chan, &sched, &alloc, &trace, &metrics);
    gmk_module_reg_init(&modules, &chan, &trace, &metrics);

    gmk_handler_reg_t handlers[] = {
        { .type = 2, .fn = yield_handler, .name = "yield_h" },
    };
    gmk_module_t mod = {
        .name = "yield_test", .handlers = handlers, .n_handlers = 1,
    };
    gmk_module_register(&modules, &mod);

    gmk_worker_pool_t pool;
    gmk_worker_pool_init(&pool, 2, &sched, &modules,
                         &alloc, &chan, &trace, &metrics, NULL);
    gmk_worker_pool_start(&pool);

    /* Enqueue 10 tasks that will yield once then complete */
    for (int i = 0; i < 10; i++) {
        gmk_task_t t;
        memset(&t, 0, sizeof(t));
        t.type  = 2;
        t.meta0 = 0; /* start at phase 0 */
        _gmk_enqueue(&sched, &t, -1);
    }
    gmk_worker_wake_all(&pool);

    for (int wait = 0; wait < 100; wait++) {
        if (gmk_atomic_load(&phase_counter, memory_order_relaxed) >= 10)
            break;
        usleep(10000);
    }

    int completed = gmk_atomic_load(&phase_counter, memory_order_relaxed);
    GMK_ASSERT_EQ(completed, 10, "all 10 tasks completed after yield");

    gmk_worker_pool_stop(&pool);
    gmk_worker_pool_destroy(&pool);
    gmk_module_reg_destroy(&modules);
    gmk_chan_reg_destroy(&chan);
    gmk_sched_destroy(&sched);
    gmk_metrics_destroy(&metrics);
    gmk_trace_destroy(&trace);
    gmk_alloc_destroy(&alloc);
}

int main(void) {
    GMK_TEST_BEGIN("worker");
    GMK_RUN_TEST(test_basic_dispatch);
    GMK_RUN_TEST(test_yield_flow);
    GMK_TEST_END();
    return 0;
}
