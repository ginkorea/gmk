/*
 * GMK/cpu — Local Queue tests
 */
#include "gmk/sched.h"
#include "test_util.h"
#include <string.h>

static gmk_task_t make_task(uint32_t type) {
    gmk_task_t t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    return t;
}

static void test_basic(void) {
    gmk_lq_t lq;
    GMK_ASSERT_EQ(gmk_lq_init(&lq, 16), 0, "init");
    GMK_ASSERT_EQ(gmk_lq_count(&lq), 0, "empty");

    gmk_task_t t = make_task(42);
    GMK_ASSERT_EQ(gmk_lq_push(&lq, &t), 0, "push");
    GMK_ASSERT_EQ(gmk_lq_count(&lq), 1, "count == 1");

    gmk_task_t out;
    GMK_ASSERT_EQ(gmk_lq_pop(&lq, &out), 0, "pop");
    GMK_ASSERT_EQ(out.type, 42, "type preserved");

    gmk_lq_destroy(&lq);
}

static void test_yield_watermark(void) {
    gmk_lq_t lq;
    /* Cap 16, watermark at 75% = 12. Normal push fails after 12. */
    GMK_ASSERT_EQ(gmk_lq_init(&lq, 16), 0, "init");

    gmk_task_t t = make_task(1);
    int normal_count = 0;
    for (int i = 0; i < 16; i++) {
        if (gmk_lq_push(&lq, &t) == 0)
            normal_count++;
        else
            break;
    }
    /* Watermark = 16 - (16*25/100) = 12 */
    GMK_ASSERT_EQ(normal_count, (int)lq.yield_watermark, "normal push stops at watermark");

    /* Yield push can go beyond watermark */
    int yield_count = 0;
    for (int i = normal_count; i < 16; i++) {
        if (gmk_lq_push_yield(&lq, &t) == 0)
            yield_count++;
        else
            break;
    }
    GMK_ASSERT_EQ(yield_count, 16 - normal_count, "yield push fills remaining");

    /* Now it should be truly full */
    GMK_ASSERT_EQ(gmk_lq_push_yield(&lq, &t), -1, "full even for yield");

    gmk_lq_destroy(&lq);
}

static void test_fifo_order(void) {
    gmk_lq_t lq;
    gmk_lq_init(&lq, 32);

    for (uint32_t i = 0; i < 10; i++) {
        gmk_task_t t = make_task(i);
        gmk_lq_push(&lq, &t);
    }

    for (uint32_t i = 0; i < 10; i++) {
        gmk_task_t out;
        GMK_ASSERT_EQ(gmk_lq_pop(&lq, &out), 0, "pop");
        GMK_ASSERT_EQ(out.type, i, "FIFO order");
    }

    gmk_lq_destroy(&lq);
}

int main(void) {
    GMK_TEST_BEGIN("sched_lq");
    GMK_RUN_TEST(test_basic);
    GMK_RUN_TEST(test_yield_watermark);
    GMK_RUN_TEST(test_fifo_order);
    GMK_TEST_END();
    return 0;
}
