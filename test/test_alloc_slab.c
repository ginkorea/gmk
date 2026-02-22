/*
 * GMK/cpu — Slab allocator tests
 */
#include "gmk/alloc.h"
#include "test_util.h"
#include <stdlib.h>

static void test_basic_alloc_free(void) {
    size_t mem_size = 4096;
    void *mem = aligned_alloc(64, mem_size);
    GMK_ASSERT_NOT_NULL(mem, "aligned_alloc");

    gmk_slab_t s;
    GMK_ASSERT_EQ(gmk_slab_init(&s, mem, mem_size, 64), 0, "slab init");
    GMK_ASSERT(s.capacity > 0, "capacity > 0");

    void *p1 = gmk_slab_alloc(&s);
    GMK_ASSERT_NOT_NULL(p1, "alloc 1");
    GMK_ASSERT_EQ(gmk_slab_used(&s), 1, "used == 1");

    void *p2 = gmk_slab_alloc(&s);
    GMK_ASSERT_NOT_NULL(p2, "alloc 2");
    GMK_ASSERT_NE((uintptr_t)p1, (uintptr_t)p2, "different pointers");
    GMK_ASSERT_EQ(gmk_slab_used(&s), 2, "used == 2");

    gmk_slab_free(&s, p1);
    GMK_ASSERT_EQ(gmk_slab_used(&s), 1, "used == 1 after free");

    gmk_slab_free(&s, p2);
    GMK_ASSERT_EQ(gmk_slab_used(&s), 0, "used == 0 after free all");

    gmk_slab_destroy(&s);
    free(mem);
}

static void test_exhaust_and_reuse(void) {
    size_t mem_size = 2048;
    void *mem = aligned_alloc(64, mem_size);
    gmk_slab_t s;
    GMK_ASSERT_EQ(gmk_slab_init(&s, mem, mem_size, 128), 0, "init");

    /* Allocate everything */
    void *ptrs[64];
    uint32_t count = 0;
    for (uint32_t i = 0; i < 64; i++) {
        ptrs[i] = gmk_slab_alloc(&s);
        if (!ptrs[i]) break;
        count++;
    }
    GMK_ASSERT(count > 0, "allocated some");
    GMK_ASSERT_EQ(count, s.capacity, "allocated exactly capacity");

    /* One more should fail */
    GMK_ASSERT_NULL(gmk_slab_alloc(&s), "alloc when full returns NULL");

    /* Free one, alloc again */
    gmk_slab_free(&s, ptrs[0]);
    void *p = gmk_slab_alloc(&s);
    GMK_ASSERT_NOT_NULL(p, "re-alloc after free");

    /* Free all */
    for (uint32_t i = 1; i < count; i++)
        gmk_slab_free(&s, ptrs[i]);
    gmk_slab_free(&s, p);
    GMK_ASSERT_EQ(gmk_slab_used(&s), 0, "all freed");

    gmk_slab_destroy(&s);
    free(mem);
}

static void test_high_watermark(void) {
    size_t mem_size = 4096;
    void *mem = aligned_alloc(64, mem_size);
    gmk_slab_t s;
    GMK_ASSERT_EQ(gmk_slab_init(&s, mem, mem_size, 32), 0, "init");

    void *p1 = gmk_slab_alloc(&s);
    void *p2 = gmk_slab_alloc(&s);
    void *p3 = gmk_slab_alloc(&s);
    GMK_ASSERT_EQ(s.high_water, 3, "high water == 3");

    gmk_slab_free(&s, p2);
    GMK_ASSERT_EQ(s.high_water, 3, "high water stays at 3");

    gmk_slab_free(&s, p1);
    gmk_slab_free(&s, p3);
    gmk_slab_destroy(&s);
    free(mem);
}

int main(void) {
    GMK_TEST_BEGIN("alloc_slab");
    GMK_RUN_TEST(test_basic_alloc_free);
    GMK_RUN_TEST(test_exhaust_and_reuse);
    GMK_RUN_TEST(test_high_watermark);
    GMK_TEST_END();
    return 0;
}
