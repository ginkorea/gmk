/*
 * GMK/cpu — Metrics tests
 */
#include "gmk/metrics.h"
#include "test_util.h"
#include <pthread.h>

static void test_basic_inc_get(void) {
    gmk_metrics_t m;
    GMK_ASSERT_EQ(gmk_metrics_init(&m, 2), 0, "init");

    gmk_metric_inc(&m, 0, GMK_METRIC_TASKS_ENQUEUED, 5);
    GMK_ASSERT_EQ(gmk_metric_get(&m, GMK_METRIC_TASKS_ENQUEUED), 5, "global == 5");
    GMK_ASSERT_EQ(gmk_metric_get_tenant(&m, 0, GMK_METRIC_TASKS_ENQUEUED), 5,
                  "tenant 0 == 5");
    GMK_ASSERT_EQ(gmk_metric_get_tenant(&m, 1, GMK_METRIC_TASKS_ENQUEUED), 0,
                  "tenant 1 == 0");

    gmk_metric_inc(&m, 1, GMK_METRIC_TASKS_ENQUEUED, 3);
    GMK_ASSERT_EQ(gmk_metric_get(&m, GMK_METRIC_TASKS_ENQUEUED), 8, "global == 8");
    GMK_ASSERT_EQ(gmk_metric_get_tenant(&m, 1, GMK_METRIC_TASKS_ENQUEUED), 3,
                  "tenant 1 == 3");

    gmk_metrics_destroy(&m);
}

static void test_multiple_metrics(void) {
    gmk_metrics_t m;
    gmk_metrics_init(&m, 1);

    gmk_metric_inc(&m, 0, GMK_METRIC_TASKS_ENQUEUED, 100);
    gmk_metric_inc(&m, 0, GMK_METRIC_TASKS_DISPATCHED, 90);
    gmk_metric_inc(&m, 0, GMK_METRIC_TASKS_FAILED, 10);
    gmk_metric_inc(&m, 0, GMK_METRIC_ALLOC_BYTES, 4096);

    GMK_ASSERT_EQ(gmk_metric_get(&m, GMK_METRIC_TASKS_ENQUEUED), 100, "enqueued");
    GMK_ASSERT_EQ(gmk_metric_get(&m, GMK_METRIC_TASKS_DISPATCHED), 90, "dispatched");
    GMK_ASSERT_EQ(gmk_metric_get(&m, GMK_METRIC_TASKS_FAILED), 10, "failed");
    GMK_ASSERT_EQ(gmk_metric_get(&m, GMK_METRIC_ALLOC_BYTES), 4096, "alloc bytes");

    gmk_metrics_destroy(&m);
}

static void test_reset(void) {
    gmk_metrics_t m;
    gmk_metrics_init(&m, 2);

    gmk_metric_inc(&m, 0, GMK_METRIC_TASKS_ENQUEUED, 50);
    gmk_metric_inc(&m, 1, GMK_METRIC_TASKS_ENQUEUED, 30);
    gmk_metrics_reset(&m);

    GMK_ASSERT_EQ(gmk_metric_get(&m, GMK_METRIC_TASKS_ENQUEUED), 0, "global reset");
    GMK_ASSERT_EQ(gmk_metric_get_tenant(&m, 0, GMK_METRIC_TASKS_ENQUEUED), 0,
                  "tenant 0 reset");
    GMK_ASSERT_EQ(gmk_metric_get_tenant(&m, 1, GMK_METRIC_TASKS_ENQUEUED), 0,
                  "tenant 1 reset");

    gmk_metrics_destroy(&m);
}

/* ── Concurrent test ─────────────────────────────────────────── */
#define METRIC_THREADS 4
#define METRIC_ITERS   10000

static gmk_metrics_t conc_metrics;

static void *metric_thread(void *arg) {
    uint16_t tenant = (uint16_t)(uintptr_t)arg;
    for (int i = 0; i < METRIC_ITERS; i++) {
        gmk_metric_inc(&conc_metrics, tenant, GMK_METRIC_TASKS_ENQUEUED, 1);
    }
    return NULL;
}

static void test_concurrent(void) {
    gmk_metrics_init(&conc_metrics, METRIC_THREADS);

    pthread_t threads[METRIC_THREADS];
    for (int i = 0; i < METRIC_THREADS; i++)
        pthread_create(&threads[i], NULL, metric_thread, (void *)(uintptr_t)i);
    for (int i = 0; i < METRIC_THREADS; i++)
        pthread_join(threads[i], NULL);

    uint64_t global = gmk_metric_get(&conc_metrics, GMK_METRIC_TASKS_ENQUEUED);
    GMK_ASSERT_EQ(global, (uint64_t)METRIC_THREADS * METRIC_ITERS,
                  "concurrent global sum");

    for (int i = 0; i < METRIC_THREADS; i++) {
        uint64_t t = gmk_metric_get_tenant(&conc_metrics, (uint16_t)i,
                                            GMK_METRIC_TASKS_ENQUEUED);
        GMK_ASSERT_EQ(t, METRIC_ITERS, "concurrent per-tenant");
    }

    gmk_metrics_destroy(&conc_metrics);
}

int main(void) {
    GMK_TEST_BEGIN("metrics");
    GMK_RUN_TEST(test_basic_inc_get);
    GMK_RUN_TEST(test_multiple_metrics);
    GMK_RUN_TEST(test_reset);
    GMK_RUN_TEST(test_concurrent);
    GMK_TEST_END();
    return 0;
}
