/*
 * GMK/cpu — Event Queue tests
 */
#include "gmk/sched.h"
#include "test_util.h"
#include <string.h>

static gmk_task_t make_evq_task(uint32_t type, uint32_t tick, uint32_t prio) {
    gmk_task_t t;
    memset(&t, 0, sizeof(t));
    t.type  = type;
    t.meta0 = tick;  /* EVQ uses meta0 as tick */
    t.flags = GMK_SET_PRIORITY(0, prio);
    return t;
}

static void test_basic(void) {
    gmk_evq_t evq;
    GMK_ASSERT_EQ(gmk_evq_init(&evq, 64), 0, "init");
    GMK_ASSERT_EQ(gmk_evq_count(&evq), 0, "empty");

    gmk_task_t t = make_evq_task(1, 10, GMK_PRIO_NORMAL);
    GMK_ASSERT_EQ(gmk_evq_push(&evq, &t), 0, "push");
    GMK_ASSERT_EQ(gmk_evq_count(&evq), 1, "count == 1");

    /* Not due yet at tick 5 */
    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 5, &out), -1, "not due at tick 5");

    /* Due at tick 10 */
    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 10, &out), 0, "due at tick 10");
    GMK_ASSERT_EQ(out.type, 1, "type preserved");

    gmk_evq_destroy(&evq);
}

static void test_ordering(void) {
    gmk_evq_t evq;
    gmk_evq_init(&evq, 64);

    /* Push events at different ticks (out of order) */
    gmk_task_t t3 = make_evq_task(30, 30, GMK_PRIO_NORMAL);
    gmk_task_t t1 = make_evq_task(10, 10, GMK_PRIO_NORMAL);
    gmk_task_t t2 = make_evq_task(20, 20, GMK_PRIO_NORMAL);

    gmk_evq_push(&evq, &t3);
    gmk_evq_push(&evq, &t1);
    gmk_evq_push(&evq, &t2);

    /* Pop in tick order */
    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 100, &out), 0, "pop 1");
    GMK_ASSERT_EQ(out.type, 10, "tick 10 first");

    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 100, &out), 0, "pop 2");
    GMK_ASSERT_EQ(out.type, 20, "tick 20 second");

    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 100, &out), 0, "pop 3");
    GMK_ASSERT_EQ(out.type, 30, "tick 30 third");

    gmk_evq_destroy(&evq);
}

static void test_priority_within_tick(void) {
    gmk_evq_t evq;
    gmk_evq_init(&evq, 64);

    /* Same tick, different priorities */
    gmk_task_t t_low  = make_evq_task(10, 5, GMK_PRIO_LOW);
    gmk_task_t t_crit = make_evq_task(40, 5, GMK_PRIO_CRITICAL);
    gmk_task_t t_norm = make_evq_task(20, 5, GMK_PRIO_NORMAL);

    gmk_evq_push(&evq, &t_low);
    gmk_evq_push(&evq, &t_crit);
    gmk_evq_push(&evq, &t_norm);

    /* Within same tick, higher priority (lower number) should come first */
    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 5, &out), 0, "pop 1");
    GMK_ASSERT_EQ(out.type, 40, "P0 first");

    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 5, &out), 0, "pop 2");
    GMK_ASSERT_EQ(out.type, 20, "P2 second");

    GMK_ASSERT_EQ(gmk_evq_pop_due(&evq, 5, &out), 0, "pop 3");
    GMK_ASSERT_EQ(out.type, 10, "P3 third");

    gmk_evq_destroy(&evq);
}

static void test_capacity(void) {
    gmk_evq_t evq;
    gmk_evq_init(&evq, 8);

    /* Fill to capacity */
    for (uint32_t i = 0; i < 8; i++) {
        gmk_task_t t = make_evq_task(i, i + 1, GMK_PRIO_NORMAL);
        GMK_ASSERT_EQ(gmk_evq_push(&evq, &t), 0, "push fill");
    }

    /* Overfill should fail */
    gmk_task_t t = make_evq_task(99, 99, GMK_PRIO_NORMAL);
    GMK_ASSERT_EQ(gmk_evq_push(&evq, &t), -1, "push when full");

    gmk_evq_destroy(&evq);
}

int main(void) {
    GMK_TEST_BEGIN("sched_evq");
    GMK_RUN_TEST(test_basic);
    GMK_RUN_TEST(test_ordering);
    GMK_RUN_TEST(test_priority_within_tick);
    GMK_RUN_TEST(test_capacity);
    GMK_TEST_END();
    return 0;
}
