/*
 * GMK/cpu — Trace tests
 */
#include "gmk/trace.h"
#include "test_util.h"

static void test_basic_write_read(void) {
    gmk_trace_t t;
    GMK_ASSERT_EQ(gmk_trace_init(&t, 2), 0, "init");
    gmk_trace_set_level(&t, GMK_TRACE_ALL);

    gmk_trace_write(&t, 0, GMK_EV_TASK_START, 1, 100, 200);
    gmk_trace_write(&t, 0, GMK_EV_TASK_END, 1, 100, 0);

    GMK_ASSERT_EQ(gmk_trace_total(&t), 2, "total == 2");

    gmk_trace_ev_t ev;
    GMK_ASSERT_EQ(gmk_trace_read(&t, 0, &ev), 0, "read 1");
    GMK_ASSERT_EQ(ev.ev_type, GMK_EV_TASK_START, "ev_type");
    GMK_ASSERT_EQ(ev.type, 1, "task type");
    GMK_ASSERT_EQ(ev.arg0, 100, "arg0");
    GMK_ASSERT_EQ(ev.arg1, 200, "arg1");
    GMK_ASSERT(ev.tsc > 0, "tsc > 0");

    GMK_ASSERT_EQ(gmk_trace_read(&t, 0, &ev), 0, "read 2");
    GMK_ASSERT_EQ(ev.ev_type, GMK_EV_TASK_END, "ev_type 2");

    GMK_ASSERT_EQ(gmk_trace_read(&t, 0, &ev), -1, "read empty");

    gmk_trace_destroy(&t);
}

static void test_level_filtering(void) {
    gmk_trace_t t;
    gmk_trace_init(&t, 1);

    /* Default level is WARN — ERROR and WARN events should pass */
    gmk_trace_write(&t, 0, GMK_EV_TASK_FAIL, 1, 0, 0);    /* ERROR → pass */
    gmk_trace_write(&t, 0, GMK_EV_CHAN_FULL, 1, 0, 0);     /* WARN → pass */
    gmk_trace_write(&t, 0, GMK_EV_TASK_START, 1, 0, 0);    /* INFO → filter */
    GMK_ASSERT_EQ(gmk_trace_total(&t), 2, "2 events (INFO filtered)");

    /* Set to ERROR only */
    gmk_trace_set_level(&t, GMK_TRACE_ERROR);
    gmk_trace_write(&t, 0, GMK_EV_TASK_FAIL, 2, 0, 0);     /* ERROR → pass */
    gmk_trace_write(&t, 0, GMK_EV_CHAN_FULL, 2, 0, 0);      /* WARN → filter */
    GMK_ASSERT_EQ(gmk_trace_total(&t), 3, "3 events total");

    /* Set to OFF */
    gmk_trace_set_level(&t, GMK_TRACE_OFF);
    gmk_trace_write(&t, 0, GMK_EV_TASK_FAIL, 3, 0, 0);     /* filter */
    GMK_ASSERT_EQ(gmk_trace_total(&t), 3, "still 3 (OFF filters all)");

    gmk_trace_destroy(&t);
}

static void test_force_write(void) {
    gmk_trace_t t;
    gmk_trace_init(&t, 1);
    gmk_trace_set_level(&t, GMK_TRACE_OFF);

    /* Force write bypasses filtering */
    gmk_trace_write_force(&t, 0, GMK_EV_BOOT, 0, 0, 0);
    GMK_ASSERT_EQ(gmk_trace_total(&t), 1, "force write bypasses level");

    gmk_trace_destroy(&t);
}

static void test_sampling(void) {
    gmk_trace_t t;
    gmk_trace_init(&t, 1);
    gmk_trace_set_level(&t, GMK_TRACE_ALL);
    gmk_trace_set_sample_rate(&t, 0.5f);

    /* Write many events, expect roughly 50% to pass */
    for (int i = 0; i < 10000; i++)
        gmk_trace_write(&t, 0, GMK_EV_TASK_START, 1, (uint32_t)i, 0);

    uint64_t total = gmk_trace_total(&t);
    /* With 50% sampling, expect 3000..7000 out of 10000 */
    GMK_ASSERT(total > 2000, "sampling: total > 2000");
    GMK_ASSERT(total < 8000, "sampling: total < 8000");

    gmk_trace_destroy(&t);
}

static void test_per_tenant(void) {
    gmk_trace_t t;
    gmk_trace_init(&t, 4);
    gmk_trace_set_level(&t, GMK_TRACE_ALL);

    gmk_trace_write(&t, 0, GMK_EV_TASK_START, 1, 0, 0);
    gmk_trace_write(&t, 1, GMK_EV_TASK_START, 2, 0, 0);
    gmk_trace_write(&t, 2, GMK_EV_TASK_START, 3, 0, 0);

    gmk_trace_ev_t ev;
    GMK_ASSERT_EQ(gmk_trace_read(&t, 0, &ev), 0, "read tenant 0");
    GMK_ASSERT_EQ(ev.type, 1, "tenant 0 type");

    GMK_ASSERT_EQ(gmk_trace_read(&t, 1, &ev), 0, "read tenant 1");
    GMK_ASSERT_EQ(ev.type, 2, "tenant 1 type");

    GMK_ASSERT_EQ(gmk_trace_read(&t, 2, &ev), 0, "read tenant 2");
    GMK_ASSERT_EQ(ev.type, 3, "tenant 2 type");

    /* Tenant 3 has no events */
    GMK_ASSERT_EQ(gmk_trace_read(&t, 3, &ev), -1, "tenant 3 empty");

    gmk_trace_destroy(&t);
}

int main(void) {
    GMK_TEST_BEGIN("trace");
    GMK_RUN_TEST(test_basic_write_read);
    GMK_RUN_TEST(test_level_filtering);
    GMK_RUN_TEST(test_force_write);
    GMK_RUN_TEST(test_sampling);
    GMK_RUN_TEST(test_per_tenant);
    GMK_TEST_END();
    return 0;
}
