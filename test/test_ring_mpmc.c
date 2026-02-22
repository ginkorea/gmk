/*
 * GMK/cpu — MPMC ring buffer tests (concurrent correctness)
 */
#include "gmk/ring_mpmc.h"
#include "test_util.h"
#include <pthread.h>

static void test_basic_push_pop(void) {
    gmk_ring_mpmc_t r;
    GMK_ASSERT_EQ(gmk_ring_mpmc_init(&r, 8, sizeof(uint32_t)), 0, "init");

    uint32_t val = 42;
    GMK_ASSERT_EQ(gmk_ring_mpmc_push(&r, &val), 0, "push 42");
    val = 99;
    GMK_ASSERT_EQ(gmk_ring_mpmc_push(&r, &val), 0, "push 99");
    GMK_ASSERT_EQ(gmk_ring_mpmc_count(&r), 2, "count == 2");

    uint32_t out;
    GMK_ASSERT_EQ(gmk_ring_mpmc_pop(&r, &out), 0, "pop");
    GMK_ASSERT_EQ(out, 42, "first pop == 42");
    GMK_ASSERT_EQ(gmk_ring_mpmc_pop(&r, &out), 0, "pop");
    GMK_ASSERT_EQ(out, 99, "second pop == 99");

    gmk_ring_mpmc_destroy(&r);
}

static void test_full_and_empty(void) {
    gmk_ring_mpmc_t r;
    GMK_ASSERT_EQ(gmk_ring_mpmc_init(&r, 4, sizeof(uint32_t)), 0, "init");

    for (uint32_t i = 0; i < 4; i++) {
        GMK_ASSERT_EQ(gmk_ring_mpmc_push(&r, &i), 0, "push fill");
    }
    uint32_t val = 100;
    GMK_ASSERT_EQ(gmk_ring_mpmc_push(&r, &val), -1, "push when full fails");

    uint32_t out;
    for (uint32_t i = 0; i < 4; i++) {
        GMK_ASSERT_EQ(gmk_ring_mpmc_pop(&r, &out), 0, "pop drain");
        GMK_ASSERT_EQ(out, i, "pop order");
    }
    GMK_ASSERT_EQ(gmk_ring_mpmc_pop(&r, &out), -1, "pop when empty fails");

    gmk_ring_mpmc_destroy(&r);
}

static void test_wraparound(void) {
    gmk_ring_mpmc_t r;
    GMK_ASSERT_EQ(gmk_ring_mpmc_init(&r, 4, sizeof(uint32_t)), 0, "init");

    for (uint32_t round = 0; round < 200; round++) {
        uint32_t val = round;
        GMK_ASSERT_EQ(gmk_ring_mpmc_push(&r, &val), 0, "push wrap");
        uint32_t out;
        GMK_ASSERT_EQ(gmk_ring_mpmc_pop(&r, &out), 0, "pop wrap");
        GMK_ASSERT_EQ(out, round, "wraparound value");
    }

    gmk_ring_mpmc_destroy(&r);
}

/* ── Multi-producer concurrent test ──────────────────────────── */
#define NUM_PRODUCERS 4
#define NUM_CONSUMERS 4
#define ITEMS_PER_THREAD 25000

typedef struct {
    gmk_ring_mpmc_t *ring;
    uint32_t         thread_id;
} thread_arg_t;

static _Atomic(uint64_t) producer_sum;
static _Atomic(uint64_t) consumer_sum;

static void *mpmc_producer(void *arg) {
    thread_arg_t *a = (thread_arg_t *)arg;
    uint64_t local_sum = 0;
    for (uint32_t i = 0; i < ITEMS_PER_THREAD; i++) {
        uint32_t val = a->thread_id * ITEMS_PER_THREAD + i;
        while (gmk_ring_mpmc_push(a->ring, &val) != 0) {
            /* spin-wait */
        }
        local_sum += val;
    }
    gmk_atomic_add(&producer_sum, local_sum, memory_order_relaxed);
    return NULL;
}

static _Atomic(uint32_t) total_consumed;

static void *mpmc_consumer(void *arg) {
    thread_arg_t *a = (thread_arg_t *)arg;
    uint64_t local_sum = 0;
    uint32_t total = NUM_PRODUCERS * ITEMS_PER_THREAD;
    while (gmk_atomic_load(&total_consumed, memory_order_relaxed) < total) {
        uint32_t val;
        if (gmk_ring_mpmc_pop(a->ring, &val) == 0) {
            local_sum += val;
            gmk_atomic_add(&total_consumed, 1, memory_order_relaxed);
        }
    }
    gmk_atomic_add(&consumer_sum, local_sum, memory_order_relaxed);
    return NULL;
}

static void test_mpmc_concurrent(void) {
    gmk_ring_mpmc_t r;
    GMK_ASSERT_EQ(gmk_ring_mpmc_init(&r, 1024, sizeof(uint32_t)), 0, "init");

    atomic_init(&producer_sum, 0);
    atomic_init(&consumer_sum, 0);
    atomic_init(&total_consumed, 0);

    pthread_t prods[NUM_PRODUCERS], cons[NUM_CONSUMERS];
    thread_arg_t pargs[NUM_PRODUCERS], cargs[NUM_CONSUMERS];

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        cargs[i] = (thread_arg_t){ .ring = &r, .thread_id = (uint32_t)i };
        pthread_create(&cons[i], NULL, mpmc_consumer, &cargs[i]);
    }
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pargs[i] = (thread_arg_t){ .ring = &r, .thread_id = (uint32_t)i };
        pthread_create(&prods[i], NULL, mpmc_producer, &pargs[i]);
    }

    for (int i = 0; i < NUM_PRODUCERS; i++)
        pthread_join(prods[i], NULL);
    for (int i = 0; i < NUM_CONSUMERS; i++)
        pthread_join(cons[i], NULL);

    uint64_t ps = gmk_atomic_load(&producer_sum, memory_order_relaxed);
    uint64_t cs = gmk_atomic_load(&consumer_sum, memory_order_relaxed);
    GMK_ASSERT_EQ(ps, cs, "producer sum == consumer sum");

    uint32_t tc = gmk_atomic_load(&total_consumed, memory_order_relaxed);
    GMK_ASSERT_EQ(tc, NUM_PRODUCERS * ITEMS_PER_THREAD, "all items consumed");

    gmk_ring_mpmc_destroy(&r);
}

/* ── Task-sized element test ─────────────────────────────────── */
static void test_task_sized(void) {
    /* Simulate gmk_task_t-sized elements (48 bytes) */
    gmk_ring_mpmc_t r;
    GMK_ASSERT_EQ(gmk_ring_mpmc_init(&r, 16, 48), 0, "init task-sized");

    uint8_t task[48];
    memset(task, 0xAB, 48);
    *(uint32_t *)task = 7; /* type field */

    GMK_ASSERT_EQ(gmk_ring_mpmc_push(&r, task), 0, "push task");

    uint8_t out[48];
    GMK_ASSERT_EQ(gmk_ring_mpmc_pop(&r, out), 0, "pop task");
    GMK_ASSERT_EQ(*(uint32_t *)out, 7, "type field preserved");
    GMK_ASSERT_EQ(out[47], 0xAB, "last byte preserved");

    gmk_ring_mpmc_destroy(&r);
}

int main(void) {
    GMK_TEST_BEGIN("ring_mpmc");
    GMK_RUN_TEST(test_basic_push_pop);
    GMK_RUN_TEST(test_full_and_empty);
    GMK_RUN_TEST(test_wraparound);
    GMK_RUN_TEST(test_task_sized);
    GMK_RUN_TEST(test_mpmc_concurrent);
    GMK_TEST_END();
    return 0;
}
