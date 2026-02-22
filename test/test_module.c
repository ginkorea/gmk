/*
 * GMK/cpu — Module system tests: dispatch table, poison detection
 */
#include "gmk/gmk.h"
#include "test_util.h"
#include <string.h>

/* ── Test handlers ───────────────────────────────────────────── */
static int echo_handler(gmk_ctx_t *ctx) {
    (void)ctx;
    return GMK_OK;
}

static int fail_handler(gmk_ctx_t *ctx) {
    (void)ctx;
    return GMK_FAIL(1);
}

static int counter_value = 0;
static int counter_handler(gmk_ctx_t *ctx) {
    counter_value += (int)ctx->task->meta0;
    return GMK_OK;
}

/* ── Tests ───────────────────────────────────────────────────── */
static gmk_sched_t sched;
static gmk_trace_t trace;
static gmk_metrics_t metrics;
static gmk_chan_reg_t chan;
static gmk_module_reg_t mr;

static void setup(void) {
    gmk_sched_init(&sched, 2);
    gmk_trace_init(&trace, 1);
    gmk_metrics_init(&metrics, 1);
    gmk_chan_reg_init(&chan, &sched, &trace, &metrics);
    gmk_module_reg_init(&mr, &chan, &trace, &metrics);
}

static void teardown(void) {
    gmk_module_reg_destroy(&mr);
    gmk_chan_reg_destroy(&chan);
    gmk_metrics_destroy(&metrics);
    gmk_trace_destroy(&trace);
    gmk_sched_destroy(&sched);
}

static void test_register_and_dispatch(void) {
    setup();

    gmk_handler_reg_t handlers[] = {
        { .type = 1, .fn = echo_handler, .name = "echo", .flags = 0 },
        { .type = 2, .fn = counter_handler, .name = "counter", .flags = 0 },
    };
    gmk_module_t mod = {
        .name       = "test_mod",
        .version    = GMK_VERSION(0, 1, 0),
        .handlers   = handlers,
        .n_handlers = 2,
        .channels   = NULL,
        .n_channels = 0,
        .init       = NULL,
        .fini       = NULL,
    };
    GMK_ASSERT_EQ(gmk_module_register(&mr, &mod), 0, "register");
    GMK_ASSERT_EQ(mr.n_handlers, 2, "2 handlers");

    /* Dispatch echo handler */
    gmk_task_t task;
    memset(&task, 0, sizeof(task));
    task.type = 1;
    gmk_ctx_t ctx = {
        .task      = &task,
        .sched     = &sched,
        .chan       = &chan,
        .trace     = &trace,
        .metrics   = &metrics,
        .worker_id = 0,
        .tick      = 0,
    };
    int rc = gmk_module_dispatch(&mr, &ctx);
    GMK_ASSERT_EQ(rc, GMK_OK, "echo dispatch OK");

    /* Dispatch counter handler */
    task.type  = 2;
    task.meta0 = 7;
    counter_value = 0;
    rc = gmk_module_dispatch(&mr, &ctx);
    GMK_ASSERT_EQ(rc, GMK_OK, "counter dispatch OK");
    GMK_ASSERT_EQ(counter_value, 7, "counter value == 7");

    teardown();
}

static void test_dispatch_unknown_type(void) {
    setup();

    gmk_task_t task;
    memset(&task, 0, sizeof(task));
    task.type = 255; /* unregistered */
    gmk_ctx_t ctx = { .task = &task };
    int rc = gmk_module_dispatch(&mr, &ctx);
    GMK_ASSERT(rc < 0, "unknown type fails");

    teardown();
}

static void test_duplicate_type(void) {
    setup();

    gmk_handler_reg_t h1[] = {
        { .type = 5, .fn = echo_handler, .name = "h1" },
    };
    gmk_module_t mod1 = { .name = "m1", .handlers = h1, .n_handlers = 1 };
    GMK_ASSERT_EQ(gmk_module_register(&mr, &mod1), 0, "register m1");

    gmk_handler_reg_t h2[] = {
        { .type = 5, .fn = fail_handler, .name = "h2" },
    };
    gmk_module_t mod2 = { .name = "m2", .handlers = h2, .n_handlers = 1 };
    GMK_ASSERT(gmk_module_register(&mr, &mod2) < 0, "duplicate type rejected");

    teardown();
}

static void test_poison(void) {
    setup();

    gmk_handler_reg_t handlers[] = {
        { .type = 10, .fn = fail_handler, .name = "fail_h" },
    };
    gmk_module_t mod = {
        .name = "fail_mod", .handlers = handlers, .n_handlers = 1,
    };
    gmk_module_register(&mr, &mod);

    GMK_ASSERT(!gmk_module_is_poisoned(&mr, 10), "not poisoned initially");

    /* Record failures up to threshold */
    for (uint32_t i = 0; i < GMK_POISON_THRESHOLD; i++) {
        gmk_module_record_fail(&mr, 10);
    }
    GMK_ASSERT(gmk_module_is_poisoned(&mr, 10), "poisoned after threshold");

    /* Dispatch of poisoned type should fail */
    gmk_task_t task;
    memset(&task, 0, sizeof(task));
    task.type = 10;
    gmk_ctx_t ctx = { .task = &task };
    int rc = gmk_module_dispatch(&mr, &ctx);
    GMK_ASSERT(rc < 0, "poisoned dispatch fails");
    GMK_ASSERT_EQ(GMK_ERR_CODE(rc), GMK_ERR_POISONED, "poisoned error code");

    /* Reset poison */
    gmk_module_reset_poison(&mr, 10);
    GMK_ASSERT(!gmk_module_is_poisoned(&mr, 10), "poison reset");

    teardown();
}

static void test_init_fini(void) {
    setup();

    static int init_called = 0, fini_called = 0;

    int test_init(gmk_ctx_t *ctx) { (void)ctx; init_called++; return GMK_OK; }
    int test_fini(gmk_ctx_t *ctx) { (void)ctx; fini_called++; return GMK_OK; }

    gmk_module_t mod = {
        .name = "lifecycle", .init = test_init, .fini = test_fini,
        .handlers = NULL, .n_handlers = 0,
    };
    gmk_module_register(&mr, &mod);

    gmk_ctx_t ctx = { 0 };
    gmk_module_init_all(&mr, &ctx);
    GMK_ASSERT_EQ(init_called, 1, "init called once");

    gmk_module_fini_all(&mr, &ctx);
    GMK_ASSERT_EQ(fini_called, 1, "fini called once");

    teardown();
}

int main(void) {
    GMK_TEST_BEGIN("module");
    GMK_RUN_TEST(test_register_and_dispatch);
    GMK_RUN_TEST(test_dispatch_unknown_type);
    GMK_RUN_TEST(test_duplicate_type);
    GMK_RUN_TEST(test_poison);
    GMK_RUN_TEST(test_init_fini);
    GMK_TEST_END();
    return 0;
}
