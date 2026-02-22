/*
 * GMK/cpu — Ready Queue tests
 */
#include "gmk/sched.h"
#include "test_util.h"
#include <string.h>

static gmk_task_t make_task(uint32_t type, uint32_t priority) {
    gmk_task_t t;
    memset(&t, 0, sizeof(t));
    t.type  = type;
    t.flags = GMK_SET_PRIORITY(0, priority);
    return t;
}

static void test_basic_push_pop(void) {
    gmk_rq_t rq;
    GMK_ASSERT_EQ(gmk_rq_init(&rq, 64), 0, "init");

    gmk_task_t t = make_task(1, GMK_PRIO_NORMAL);
    GMK_ASSERT_EQ(gmk_rq_push(&rq, &t), 0, "push");
    GMK_ASSERT_EQ(gmk_rq_count(&rq), 1, "count == 1");

    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_rq_pop(&rq, &out), 0, "pop");
    GMK_ASSERT_EQ(out.type, 1, "type preserved");
    GMK_ASSERT_EQ(gmk_rq_count(&rq), 0, "count == 0");

    gmk_rq_destroy(&rq);
}

static void test_priority_ordering(void) {
    gmk_rq_t rq;
    gmk_rq_init(&rq, 64);

    /* Push tasks at different priorities */
    gmk_task_t t_low = make_task(10, GMK_PRIO_LOW);
    gmk_task_t t_norm = make_task(20, GMK_PRIO_NORMAL);
    gmk_task_t t_high = make_task(30, GMK_PRIO_HIGH);
    gmk_task_t t_crit = make_task(40, GMK_PRIO_CRITICAL);

    gmk_rq_push(&rq, &t_low);
    gmk_rq_push(&rq, &t_norm);
    gmk_rq_push(&rq, &t_high);
    gmk_rq_push(&rq, &t_crit);

    /* Pop should favor higher priority (lower number) */
    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_rq_pop(&rq, &out), 0, "pop 1");
    GMK_ASSERT_EQ(out.type, 40, "first pop is P0 (critical)");

    GMK_ASSERT_EQ(gmk_rq_pop(&rq, &out), 0, "pop 2");
    GMK_ASSERT_EQ(out.type, 30, "second pop is P1 (high)");

    GMK_ASSERT_EQ(gmk_rq_pop(&rq, &out), 0, "pop 3");
    GMK_ASSERT_EQ(out.type, 20, "third pop is P2 (normal)");

    GMK_ASSERT_EQ(gmk_rq_pop(&rq, &out), 0, "pop 4");
    GMK_ASSERT_EQ(out.type, 10, "fourth pop is P3 (low)");

    gmk_rq_destroy(&rq);
}

static void test_weighted_pop(void) {
    gmk_rq_t rq;
    gmk_rq_init(&rq, 256);

    /* Fill P0 with 20 tasks, P3 with 20 tasks */
    for (int i = 0; i < 20; i++) {
        gmk_task_t t0 = make_task(100, GMK_PRIO_CRITICAL);
        gmk_task_t t3 = make_task(200, GMK_PRIO_LOW);
        gmk_rq_push(&rq, &t0);
        gmk_rq_push(&rq, &t3);
    }

    /* Pop first 8 — should all be P0 (weight 8) */
    int p0_count = 0, p3_count = 0;
    for (int i = 0; i < 9; i++) {
        gmk_task_t out;
        gmk_rq_pop(&rq, &out);
        if (out.type == 100) p0_count++;
        else p3_count++;
    }

    /* First 8 should be P0, then P3 should start getting served */
    GMK_ASSERT(p0_count >= 8, "at least 8 P0 tasks popped first");

    gmk_rq_destroy(&rq);
}

static void test_empty_pop(void) {
    gmk_rq_t rq;
    gmk_rq_init(&rq, 64);

    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_rq_pop(&rq, &out), -1, "pop from empty");

    gmk_rq_destroy(&rq);
}

int main(void) {
    GMK_TEST_BEGIN("sched_rq");
    GMK_RUN_TEST(test_basic_push_pop);
    GMK_RUN_TEST(test_priority_ordering);
    GMK_RUN_TEST(test_weighted_pop);
    GMK_RUN_TEST(test_empty_pop);
    GMK_TEST_END();
    return 0;
}
