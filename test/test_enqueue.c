/*
 * GMK/cpu — Enqueue + yield tests
 */
#include "gmk/sched.h"
#include "test_util.h"
#include <string.h>

static gmk_task_t make_task(uint32_t type, uint32_t prio) {
    gmk_task_t t;
    memset(&t, 0, sizeof(t));
    t.type  = type;
    t.flags = GMK_SET_PRIORITY(0, prio);
    return t;
}

static void test_enqueue_to_rq(void) {
    gmk_sched_t s;
    GMK_ASSERT_EQ(gmk_sched_init(&s, 2), 0, "init");

    gmk_task_t t = make_task(1, GMK_PRIO_NORMAL);
    GMK_ASSERT_EQ(_gmk_enqueue(&s, &t, -1), 0, "enqueue to RQ");
    GMK_ASSERT(t.seq == 0, "seq assigned");

    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_rq_pop(&s.rq, &out), 0, "pop from RQ");
    GMK_ASSERT_EQ(out.type, 1, "type preserved");

    gmk_sched_destroy(&s);
}

static void test_enqueue_to_lq(void) {
    gmk_sched_t s;
    gmk_sched_init(&s, 4);

    gmk_task_t t = make_task(2, GMK_PRIO_HIGH);
    GMK_ASSERT_EQ(_gmk_enqueue(&s, &t, 1), 0, "enqueue to LQ[1]");

    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_lq_pop(&s.lqs[1], &out), 0, "pop from LQ[1]");
    GMK_ASSERT_EQ(out.type, 2, "type preserved");

    /* LQ[0] should be empty */
    GMK_ASSERT_EQ(gmk_lq_pop(&s.lqs[0], &out), -1, "LQ[0] empty");

    gmk_sched_destroy(&s);
}

static void test_seq_monotonic(void) {
    gmk_sched_t s;
    gmk_sched_init(&s, 2);

    for (uint32_t i = 0; i < 10; i++) {
        gmk_task_t t = make_task(i, GMK_PRIO_NORMAL);
        _gmk_enqueue(&s, &t, -1);
        GMK_ASSERT_EQ(t.seq, i, "seq monotonically increases");
    }

    gmk_sched_destroy(&s);
}

static void test_yield_basic(void) {
    gmk_sched_t s;
    gmk_sched_init(&s, 2);

    gmk_task_t t = make_task(5, GMK_PRIO_NORMAL);
    t.yield_count = 0;

    int rc = _gmk_yield(&s, &t, 0, GMK_DEFAULT_MAX_YIELDS);
    GMK_ASSERT_EQ(rc, 0, "yield succeeds");
    GMK_ASSERT_EQ(t.yield_count, 1, "yield_count incremented");

    /* Should be in LQ[0] */
    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_lq_pop(&s.lqs[0], &out), 0, "pop yielded task");
    GMK_ASSERT_EQ(out.type, 5, "type preserved");

    gmk_sched_destroy(&s);
}

static void test_yield_circuit_breaker(void) {
    gmk_sched_t s;
    gmk_sched_init(&s, 2);

    gmk_task_t t = make_task(7, GMK_PRIO_NORMAL);
    t.yield_count = GMK_DEFAULT_MAX_YIELDS; /* already at limit */

    int rc = _gmk_yield(&s, &t, 0, GMK_DEFAULT_MAX_YIELDS);
    GMK_ASSERT(rc < 0, "yield fails at circuit breaker");
    GMK_ASSERT_EQ(GMK_ERR_CODE(rc), GMK_ERR_YIELD_LIMIT, "yield limit error");

    gmk_sched_destroy(&s);
}

static void test_yield_overflow(void) {
    gmk_sched_t s;
    gmk_sched_init(&s, 1);

    /* Fill LQ completely so yield goes to overflow */
    gmk_task_t fill;
    memset(&fill, 0, sizeof(fill));
    fill.type = 99;
    for (uint32_t i = 0; i < GMK_LQ_DEFAULT_CAP; i++) {
        gmk_ring_spsc_push(&s.lqs[0].ring, &fill);
    }

    /* Yield should go to overflow bucket */
    gmk_task_t t = make_task(8, GMK_PRIO_NORMAL);
    int rc = _gmk_yield(&s, &t, 0, GMK_DEFAULT_MAX_YIELDS);
    GMK_ASSERT_EQ(rc, 0, "yield to overflow");

    /* Should be in overflow ring */
    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_ring_mpmc_pop(&s.overflow, &out), 0, "pop from overflow");
    GMK_ASSERT_EQ(out.type, 8, "overflow type preserved");

    gmk_sched_destroy(&s);
}

static void test_yield_at(void) {
    gmk_sched_t s;
    gmk_sched_init(&s, 2);

    gmk_task_t t = make_task(9, GMK_PRIO_NORMAL);
    t.meta0 = 0;
    gmk_yield_at_impl(&s, &t, 0, 42);

    gmk_task_t out;
    gmk_lq_pop(&s.lqs[0], &out);
    GMK_ASSERT_EQ(out.meta0, 42, "meta0 set to phase");
    GMK_ASSERT_EQ(out.yield_count, 1, "yield_count incremented");

    gmk_sched_destroy(&s);
}

int main(void) {
    GMK_TEST_BEGIN("enqueue");
    GMK_RUN_TEST(test_enqueue_to_rq);
    GMK_RUN_TEST(test_enqueue_to_lq);
    GMK_RUN_TEST(test_seq_monotonic);
    GMK_RUN_TEST(test_yield_basic);
    GMK_RUN_TEST(test_yield_circuit_breaker);
    GMK_RUN_TEST(test_yield_overflow);
    GMK_RUN_TEST(test_yield_at);
    GMK_TEST_END();
    return 0;
}
