/*
 * GMK/cpu — Full boot test: boot → execute task → halt
 */
#include "gmk/gmk.h"
#include "test_util.h"
#include <string.h>
#include <unistd.h>

/* ── Simple echo handler for boot test ───────────────────────── */
static _Atomic(int) echo_count;

static int echo_handler(gmk_ctx_t *ctx) {
    (void)ctx;
    gmk_atomic_add(&echo_count, 1, memory_order_relaxed);
    return GMK_OK;
}

/* ── Multi-phase handler ─────────────────────────────────────── */
static _Atomic(int) multi_done;

static int multi_handler(gmk_ctx_t *ctx) {
    switch (ctx->task->meta0) {
    case 0:
        ctx->task->meta1 = 42;
        gmk_yield_at_impl(ctx->sched, ctx->task, (int)ctx->worker_id, 1);
        return GMK_OK;
    case 1:
        if (ctx->task->meta1 == 42)
            gmk_atomic_add(&multi_done, 1, memory_order_relaxed);
        return GMK_OK;
    default:
        return GMK_OK;
    }
}

static void test_boot_halt(void) {
    gmk_kernel_t kernel;
    gmk_boot_cfg_t cfg = {
        .arena_size = 4 * 1024 * 1024,  /* 4 MB */
        .n_workers  = 2,
        .n_tenants  = 1,
    };

    int rc = gmk_boot(&kernel, &cfg, NULL, 0);
    GMK_ASSERT_EQ(rc, 0, "boot with no modules");

    gmk_halt(&kernel);
}

static void test_boot_with_handler(void) {
    atomic_init(&echo_count, 0);

    gmk_handler_reg_t handlers[] = {
        { .type = 1, .fn = echo_handler, .name = "echo" },
    };
    gmk_module_t mod = {
        .name = "echo_mod", .version = GMK_VERSION(0, 1, 0),
        .handlers = handlers, .n_handlers = 1,
    };
    gmk_module_t *mods[] = { &mod };

    gmk_kernel_t kernel;
    gmk_boot_cfg_t cfg = {
        .arena_size = 4 * 1024 * 1024,
        .n_workers  = 2,
        .n_tenants  = 1,
    };

    int rc = gmk_boot(&kernel, &cfg, mods, 1);
    GMK_ASSERT_EQ(rc, 0, "boot with echo module");

    /* Submit tasks */
    for (int i = 0; i < 50; i++) {
        gmk_task_t t;
        memset(&t, 0, sizeof(t));
        t.type = 1;
        gmk_submit(&kernel, &t);
    }

    /* Wait for processing */
    for (int wait = 0; wait < 200; wait++) {
        if (gmk_atomic_load(&echo_count, memory_order_relaxed) >= 50)
            break;
        usleep(5000);
    }

    int count = gmk_atomic_load(&echo_count, memory_order_relaxed);
    GMK_ASSERT_EQ(count, 50, "50 tasks processed");

    /* Verify metrics */
    uint64_t enqueued = gmk_metric_get(&kernel.metrics, GMK_METRIC_TASKS_ENQUEUED);
    GMK_ASSERT(enqueued >= 50, "metrics: >= 50 enqueued");

    uint64_t dispatched = gmk_metric_get(&kernel.metrics, GMK_METRIC_TASKS_DISPATCHED);
    GMK_ASSERT(dispatched >= 50, "metrics: >= 50 dispatched");

    gmk_halt(&kernel);
}

static void test_multi_phase(void) {
    atomic_init(&multi_done, 0);

    gmk_handler_reg_t handlers[] = {
        { .type = 10, .fn = multi_handler, .name = "multi" },
    };
    gmk_module_t mod = {
        .name = "multi_mod", .handlers = handlers, .n_handlers = 1,
    };
    gmk_module_t *mods[] = { &mod };

    gmk_kernel_t kernel;
    gmk_boot_cfg_t cfg = {
        .arena_size = 4 * 1024 * 1024,
        .n_workers  = 2,
        .n_tenants  = 1,
    };

    gmk_boot(&kernel, &cfg, mods, 1);

    /* Submit multi-phase tasks */
    for (int i = 0; i < 20; i++) {
        gmk_task_t t;
        memset(&t, 0, sizeof(t));
        t.type  = 10;
        t.meta0 = 0;
        gmk_submit(&kernel, &t);
    }

    for (int wait = 0; wait < 200; wait++) {
        if (gmk_atomic_load(&multi_done, memory_order_relaxed) >= 20)
            break;
        usleep(5000);
    }

    int done = gmk_atomic_load(&multi_done, memory_order_relaxed);
    GMK_ASSERT_EQ(done, 20, "20 multi-phase tasks completed");

    gmk_halt(&kernel);
}

static void test_channel_integration(void) {
    atomic_init(&echo_count, 0);

    gmk_handler_reg_t handlers[] = {
        { .type = 1, .fn = echo_handler, .name = "echo" },
    };
    gmk_module_t mod = {
        .name = "chan_echo", .handlers = handlers, .n_handlers = 1,
    };
    gmk_module_t *mods[] = { &mod };

    gmk_kernel_t kernel;
    gmk_boot_cfg_t cfg = {
        .arena_size = 4 * 1024 * 1024,
        .n_workers  = 2,
        .n_tenants  = 1,
    };

    gmk_boot(&kernel, &cfg, mods, 1);

    /* Open a channel and subscribe */
    int ch = gmk_chan_open(&kernel.chan, "test.echo", GMK_CHAN_P2P,
                           GMK_CHAN_LOSSY, 1, 64);
    GMK_ASSERT(ch >= 0, "channel open");
    GMK_ASSERT_EQ(gmk_chan_sub(&kernel.chan, (uint32_t)ch, 0, -1), 0, "subscribe");

    /* Emit tasks through channel */
    for (int i = 0; i < 30; i++) {
        gmk_task_t t;
        memset(&t, 0, sizeof(t));
        t.type = 1;
        gmk_chan_emit(&kernel.chan, (uint32_t)ch, &t);
    }

    for (int wait = 0; wait < 200; wait++) {
        if (gmk_atomic_load(&echo_count, memory_order_relaxed) >= 30)
            break;
        usleep(5000);
    }

    int count = gmk_atomic_load(&echo_count, memory_order_relaxed);
    GMK_ASSERT_EQ(count, 30, "30 channel tasks processed");

    gmk_halt(&kernel);
}

int main(void) {
    GMK_TEST_BEGIN("boot");
    GMK_RUN_TEST(test_boot_halt);
    GMK_RUN_TEST(test_boot_with_handler);
    GMK_RUN_TEST(test_multi_phase);
    GMK_RUN_TEST(test_channel_integration);
    GMK_TEST_END();
    return 0;
}
