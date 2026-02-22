/*
 * GMK/cpu — Bump allocator tests
 */
#include "gmk/alloc.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static void test_basic(void) {
    size_t mem_size = 4096;
    void *mem = aligned_alloc(64, mem_size);
    gmk_bump_t b;
    GMK_ASSERT_EQ(gmk_bump_init(&b, mem, mem_size), 0, "init");
    GMK_ASSERT_EQ(gmk_bump_used(&b), 0, "used == 0");

    void *p1 = gmk_bump_alloc(&b, 100);
    GMK_ASSERT_NOT_NULL(p1, "alloc 100");
    /* 100 rounds up to 104 (8-byte aligned) */
    GMK_ASSERT(gmk_bump_used(&b) >= 100, "used >= 100");

    void *p2 = gmk_bump_alloc(&b, 200);
    GMK_ASSERT_NOT_NULL(p2, "alloc 200");
    GMK_ASSERT_NE((uintptr_t)p1, (uintptr_t)p2, "different pointers");

    /* Write and verify */
    memset(p1, 0xCC, 100);
    memset(p2, 0xDD, 200);
    GMK_ASSERT_EQ(((uint8_t *)p1)[0], 0xCC, "p1 data");
    GMK_ASSERT_EQ(((uint8_t *)p2)[199], 0xDD, "p2 data");

    free(mem);
}

static void test_reset(void) {
    size_t mem_size = 4096;
    void *mem = aligned_alloc(64, mem_size);
    gmk_bump_t b;
    gmk_bump_init(&b, mem, mem_size);

    gmk_bump_alloc(&b, 1000);
    gmk_bump_alloc(&b, 1000);
    GMK_ASSERT(gmk_bump_used(&b) >= 2000, "used >= 2000");

    gmk_bump_reset(&b);
    GMK_ASSERT_EQ(gmk_bump_used(&b), 0, "used == 0 after reset");

    /* Can allocate again after reset */
    void *p = gmk_bump_alloc(&b, 500);
    GMK_ASSERT_NOT_NULL(p, "alloc after reset");

    free(mem);
}

static void test_exhaustion(void) {
    size_t mem_size = 256;
    void *mem = aligned_alloc(64, mem_size);
    gmk_bump_t b;
    gmk_bump_init(&b, mem, mem_size);

    void *p = gmk_bump_alloc(&b, 200);
    GMK_ASSERT_NOT_NULL(p, "first alloc");

    void *p2 = gmk_bump_alloc(&b, 200);
    GMK_ASSERT_NULL(p2, "second alloc fails (exhausted)");

    free(mem);
}

/* ── Concurrent test ─────────────────────────────────────────── */
#define BUMP_THREADS 4
#define BUMP_ALLOCS  1000

static gmk_bump_t conc_bump;

static void *bump_thread_fn(void *arg) {
    (void)arg;
    int success = 0;
    for (int i = 0; i < BUMP_ALLOCS; i++) {
        void *p = gmk_bump_alloc(&conc_bump, 8);
        if (p) success++;
    }
    return (void *)(intptr_t)success;
}

static void test_concurrent(void) {
    size_t mem_size = BUMP_THREADS * BUMP_ALLOCS * 8 + 1024;
    void *mem = aligned_alloc(64, mem_size);
    gmk_bump_init(&conc_bump, mem, mem_size);

    pthread_t threads[BUMP_THREADS];
    for (int i = 0; i < BUMP_THREADS; i++)
        pthread_create(&threads[i], NULL, bump_thread_fn, NULL);

    int total_success = 0;
    for (int i = 0; i < BUMP_THREADS; i++) {
        void *ret;
        pthread_join(threads[i], &ret);
        total_success += (int)(intptr_t)ret;
    }

    GMK_ASSERT_EQ(total_success, BUMP_THREADS * BUMP_ALLOCS,
                  "all concurrent allocs succeed");

    free(mem);
}

int main(void) {
    GMK_TEST_BEGIN("alloc_bump");
    GMK_RUN_TEST(test_basic);
    GMK_RUN_TEST(test_reset);
    GMK_RUN_TEST(test_exhaustion);
    GMK_RUN_TEST(test_concurrent);
    GMK_TEST_END();
    return 0;
}
