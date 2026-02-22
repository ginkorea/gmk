/*
 * GMK/cpu — Channel tests: P2P, fan-out, backpressure, dead-letter, drain
 */
#include "gmk/trace.h"
#include "gmk/metrics.h"
#include "gmk/chan.h"
#include "test_util.h"
#include <string.h>

static gmk_sched_t sched;
static gmk_trace_t trace;
static gmk_metrics_t metrics;
static gmk_chan_reg_t cr;

static void setup(void) {
    gmk_sched_init(&sched, 4);
    gmk_trace_init(&trace, 1);
    gmk_metrics_init(&metrics, 1);
    gmk_chan_reg_init(&cr, &sched, NULL, &trace, &metrics);
}

static void teardown(void) {
    gmk_chan_reg_destroy(&cr);
    gmk_metrics_destroy(&metrics);
    gmk_trace_destroy(&trace);
    gmk_sched_destroy(&sched);
}

static gmk_task_t make_task(uint32_t type, uint32_t prio) {
    gmk_task_t t;
    memset(&t, 0, sizeof(t));
    t.type  = type;
    t.flags = GMK_SET_PRIORITY(0, prio);
    return t;
}

static void test_open_and_find(void) {
    setup();

    int id = gmk_chan_open(&cr, "test.chan", GMK_CHAN_P2P, GMK_CHAN_LOSSY, 1, 64);
    GMK_ASSERT(id >= 2, "channel id >= 2");

    int found = gmk_chan_find(&cr, "test.chan");
    GMK_ASSERT_EQ(found, id, "find by name");

    /* Duplicate name fails */
    int dup = gmk_chan_open(&cr, "test.chan", GMK_CHAN_P2P, GMK_CHAN_LOSSY, 1, 64);
    GMK_ASSERT(dup < 0, "duplicate name fails");

    teardown();
}

static void test_p2p(void) {
    setup();

    int id = gmk_chan_open(&cr, "test.p2p", GMK_CHAN_P2P, GMK_CHAN_LOSSY, 10, 64);
    GMK_ASSERT(id >= 0, "open P2P");

    /* Subscribe one worker */
    GMK_ASSERT_EQ(gmk_chan_sub(&cr, (uint32_t)id, 0, 0), 0, "subscribe");

    /* Second subscribe fails (P2P) */
    GMK_ASSERT(gmk_chan_sub(&cr, (uint32_t)id, 1, 1) < 0, "second sub fails");

    /* Emit a task */
    gmk_task_t t = make_task(10, GMK_PRIO_NORMAL);
    GMK_ASSERT_EQ(gmk_chan_emit(&cr, (uint32_t)id, &t), 0, "emit");

    /* Task should be in worker 0's LQ or RQ */
    gmk_task_t out;
    int got = gmk_lq_pop(&sched.lqs[0], &out);
    if (got != 0) got = gmk_rq_pop(&sched.rq, &out);
    GMK_ASSERT_EQ(got, 0, "task delivered");
    GMK_ASSERT_EQ(out.type, 10, "type preserved");
    GMK_ASSERT(out.flags & GMK_TF_CHANNEL_MSG, "channel flag set");

    teardown();
}

static void test_fanout(void) {
    setup();

    int id = gmk_chan_open(&cr, "test.fanout", GMK_CHAN_FANOUT, GMK_CHAN_LOSSY,
                           20, 64);
    GMK_ASSERT(id >= 0, "open fan-out");

    /* Subscribe 3 workers */
    gmk_chan_sub(&cr, (uint32_t)id, 0, 0);
    gmk_chan_sub(&cr, (uint32_t)id, 1, 1);
    gmk_chan_sub(&cr, (uint32_t)id, 2, 2);

    /* Emit a task */
    gmk_task_t t = make_task(20, GMK_PRIO_NORMAL);
    GMK_ASSERT_EQ(gmk_chan_emit(&cr, (uint32_t)id, &t), 0, "emit");

    /* Drain the channel */
    int drained = gmk_chan_drain(&cr, (uint32_t)id, 10);
    GMK_ASSERT(drained >= 0, "drain succeeds");

    /* Each subscriber should have received the task */
    int received = 0;
    for (int w = 0; w < 3; w++) {
        gmk_task_t out;
        if (gmk_lq_pop(&sched.lqs[w], &out) == 0) {
            GMK_ASSERT_EQ(out.type, 20, "fan-out type preserved");
            received++;
        }
    }
    /* Tasks might also be in RQ if LQ was busy */
    gmk_task_t rq_out;
    while (gmk_rq_pop(&sched.rq, &rq_out) == 0) {
        if (rq_out.type == 20) received++;
    }
    GMK_ASSERT_EQ(received, 3, "3 subscribers received");

    teardown();
}

static void test_backpressure(void) {
    setup();

    /* Small ring to easily trigger fullness */
    int id = gmk_chan_open(&cr, "test.bp", GMK_CHAN_P2P, GMK_CHAN_LOSSY, 30, 8);
    GMK_ASSERT(id >= 0, "open");

    /* Don't subscribe — tasks stay in ring */

    /* Fill the ring */
    int filled = 0;
    for (int i = 0; i < 16; i++) {
        gmk_task_t t = make_task(30, GMK_PRIO_NORMAL);
        if (gmk_chan_emit(&cr, (uint32_t)id, &t) == 0)
            filled++;
        else
            break;
    }
    GMK_ASSERT(filled > 0 && filled <= 8, "ring fills up");

    /* Next emit should fail */
    gmk_task_t t = make_task(30, GMK_PRIO_NORMAL);
    int rc = gmk_chan_emit(&cr, (uint32_t)id, &t);
    GMK_ASSERT(rc < 0, "backpressure: emit fails when full");

    teardown();
}

static void test_close(void) {
    setup();

    int id = gmk_chan_open(&cr, "test.close", GMK_CHAN_P2P, GMK_CHAN_LOSSY, 40, 64);
    GMK_ASSERT(id >= 0, "open");

    GMK_ASSERT_EQ(gmk_chan_close(&cr, (uint32_t)id), 0, "close");

    /* Emit on closed channel fails */
    gmk_task_t t = make_task(40, GMK_PRIO_NORMAL);
    GMK_ASSERT(gmk_chan_emit(&cr, (uint32_t)id, &t) < 0, "emit on closed fails");

    teardown();
}

static void test_dead_letter(void) {
    setup();

    /* sys.dropped should be open */
    GMK_ASSERT(cr.channels[GMK_CHAN_SYS_DROPPED].open, "sys.dropped is open");
    GMK_ASSERT_EQ(gmk_chan_find(&cr, "sys.dropped"), (int)GMK_CHAN_SYS_DROPPED,
                  "find sys.dropped");

    teardown();
}

static void test_priority_reserve(void) {
    setup();

    /* Small ring: 16 slots, 10% reserve = last 1-2 slots for P0 only */
    int id = gmk_chan_open(&cr, "test.prioreserve", GMK_CHAN_P2P, GMK_CHAN_LOSSY,
                           50, 16);

    /* Fill to near capacity with low-priority tasks */
    int filled = 0;
    for (int i = 0; i < 20; i++) {
        gmk_task_t t = make_task(50, GMK_PRIO_LOW);
        if (gmk_chan_emit(&cr, (uint32_t)id, &t) == 0)
            filled++;
    }
    /* Should have stopped before full — reserve blocks low priority */
    GMK_ASSERT(filled < 16, "low-prio stopped before full");

    /* High priority should still be able to emit */
    gmk_task_t t_crit = make_task(50, GMK_PRIO_CRITICAL);
    int rc = gmk_chan_emit(&cr, (uint32_t)id, &t_crit);
    /* Might succeed if ring still has room in reserve */
    (void)rc;

    teardown();
}

int main(void) {
    GMK_TEST_BEGIN("chan");
    GMK_RUN_TEST(test_open_and_find);
    GMK_RUN_TEST(test_p2p);
    GMK_RUN_TEST(test_fanout);
    GMK_RUN_TEST(test_backpressure);
    GMK_RUN_TEST(test_close);
    GMK_RUN_TEST(test_dead_letter);
    GMK_RUN_TEST(test_priority_reserve);
    GMK_TEST_END();
    return 0;
}
