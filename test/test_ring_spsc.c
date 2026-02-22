/*
 * GMK/cpu — SPSC ring buffer tests
 */
#include "gmk/ring_spsc.h"
#include "test_util.h"
#include <pthread.h>

static void test_basic_push_pop(void) {
    gmk_ring_spsc_t r;
    GMK_ASSERT_EQ(gmk_ring_spsc_init(&r, 8, sizeof(uint32_t)), 0, "init");

    uint32_t val = 42;
    GMK_ASSERT_EQ(gmk_ring_spsc_push(&r, &val), 0, "push 42");
    val = 99;
    GMK_ASSERT_EQ(gmk_ring_spsc_push(&r, &val), 0, "push 99");
    GMK_ASSERT_EQ(gmk_ring_spsc_count(&r), 2, "count == 2");

    uint32_t out;
    GMK_ASSERT_EQ(gmk_ring_spsc_pop(&r, &out), 0, "pop");
    GMK_ASSERT_EQ(out, 42, "first pop == 42");
    GMK_ASSERT_EQ(gmk_ring_spsc_pop(&r, &out), 0, "pop");
    GMK_ASSERT_EQ(out, 99, "second pop == 99");
    GMK_ASSERT(gmk_ring_spsc_empty(&r), "ring empty after draining");

    gmk_ring_spsc_destroy(&r);
}

static void test_full_and_empty(void) {
    gmk_ring_spsc_t r;
    GMK_ASSERT_EQ(gmk_ring_spsc_init(&r, 4, sizeof(uint32_t)), 0, "init");

    /* Fill to capacity */
    for (uint32_t i = 0; i < 4; i++) {
        GMK_ASSERT_EQ(gmk_ring_spsc_push(&r, &i), 0, "push fill");
    }
    GMK_ASSERT(gmk_ring_spsc_full(&r), "ring full");

    uint32_t val = 100;
    GMK_ASSERT_EQ(gmk_ring_spsc_push(&r, &val), -1, "push when full fails");

    /* Drain */
    uint32_t out;
    for (uint32_t i = 0; i < 4; i++) {
        GMK_ASSERT_EQ(gmk_ring_spsc_pop(&r, &out), 0, "pop drain");
        GMK_ASSERT_EQ(out, i, "pop order correct");
    }
    GMK_ASSERT(gmk_ring_spsc_empty(&r), "ring empty");
    GMK_ASSERT_EQ(gmk_ring_spsc_pop(&r, &out), -1, "pop when empty fails");

    gmk_ring_spsc_destroy(&r);
}

static void test_wraparound(void) {
    gmk_ring_spsc_t r;
    GMK_ASSERT_EQ(gmk_ring_spsc_init(&r, 4, sizeof(uint32_t)), 0, "init");

    /* Push/pop many times to exercise wraparound */
    for (uint32_t round = 0; round < 100; round++) {
        uint32_t val = round;
        GMK_ASSERT_EQ(gmk_ring_spsc_push(&r, &val), 0, "push wrap");
        uint32_t out;
        GMK_ASSERT_EQ(gmk_ring_spsc_pop(&r, &out), 0, "pop wrap");
        GMK_ASSERT_EQ(out, round, "wraparound value correct");
    }

    gmk_ring_spsc_destroy(&r);
}

static void test_large_elements(void) {
    gmk_ring_spsc_t r;
    typedef struct { uint64_t a; uint64_t b; uint64_t c; } big_t;
    GMK_ASSERT_EQ(gmk_ring_spsc_init(&r, 8, sizeof(big_t)), 0, "init large");

    big_t in = { .a = 111, .b = 222, .c = 333 };
    GMK_ASSERT_EQ(gmk_ring_spsc_push(&r, &in), 0, "push large");

    big_t out;
    GMK_ASSERT_EQ(gmk_ring_spsc_pop(&r, &out), 0, "pop large");
    GMK_ASSERT_EQ(out.a, 111, "large.a");
    GMK_ASSERT_EQ(out.b, 222, "large.b");
    GMK_ASSERT_EQ(out.c, 333, "large.c");

    gmk_ring_spsc_destroy(&r);
}

/* ── Concurrent test ─────────────────────────────────────────── */
#define CONC_COUNT 100000
static gmk_ring_spsc_t conc_ring;

static void *producer_fn(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < CONC_COUNT; i++) {
        while (gmk_ring_spsc_push(&conc_ring, &i) != 0) {
            /* spin */
        }
    }
    return NULL;
}

static void *consumer_fn(void *arg) {
    uint64_t *sum = (uint64_t *)arg;
    *sum = 0;
    uint32_t count = 0;
    while (count < CONC_COUNT) {
        uint32_t val;
        if (gmk_ring_spsc_pop(&conc_ring, &val) == 0) {
            *sum += val;
            count++;
        }
    }
    return NULL;
}

static void test_concurrent(void) {
    GMK_ASSERT_EQ(gmk_ring_spsc_init(&conc_ring, 1024, sizeof(uint32_t)),
                  0, "init concurrent");

    pthread_t prod, cons;
    uint64_t sum = 0;
    pthread_create(&prod, NULL, producer_fn, NULL);
    pthread_create(&cons, NULL, consumer_fn, &sum);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    /* Sum of 0..99999 */
    uint64_t expected = (uint64_t)(CONC_COUNT - 1) * CONC_COUNT / 2;
    GMK_ASSERT_EQ(sum, expected, "concurrent sum correct");
    GMK_ASSERT(gmk_ring_spsc_empty(&conc_ring), "ring empty after concurrent");

    gmk_ring_spsc_destroy(&conc_ring);
}

int main(void) {
    GMK_TEST_BEGIN("ring_spsc");
    GMK_RUN_TEST(test_basic_push_pop);
    GMK_RUN_TEST(test_full_and_empty);
    GMK_RUN_TEST(test_wraparound);
    GMK_RUN_TEST(test_large_elements);
    GMK_RUN_TEST(test_concurrent);
    GMK_TEST_END();
    return 0;
}
