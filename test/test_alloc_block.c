/*
 * GMK/cpu — Block allocator tests
 */
#include "gmk/alloc.h"
#include "test_util.h"
#include <stdlib.h>
#include <string.h>

static void test_various_sizes(void) {
    size_t mem_size = 16 * 1024 * 1024; /* 16 MB — enough for all bins */
    void *mem = aligned_alloc(64, mem_size);
    GMK_ASSERT_NOT_NULL(mem, "aligned_alloc");

    gmk_block_t b;
    GMK_ASSERT_EQ(gmk_block_init(&b, mem, mem_size), 0, "block init");

    /* Allocate various sizes */
    void *p32 = gmk_block_alloc(&b, 32);
    GMK_ASSERT_NOT_NULL(p32, "alloc 32");

    void *p64 = gmk_block_alloc(&b, 64);
    GMK_ASSERT_NOT_NULL(p64, "alloc 64");

    void *p100 = gmk_block_alloc(&b, 100); /* rounds up to 128 */
    GMK_ASSERT_NOT_NULL(p100, "alloc 100 (-> 128)");

    void *p1k = gmk_block_alloc(&b, 1024);
    GMK_ASSERT_NOT_NULL(p1k, "alloc 1024");

    void *p64k = gmk_block_alloc(&b, 65536);
    GMK_ASSERT_NOT_NULL(p64k, "alloc 65536");

    /* Too large should fail */
    void *pbig = gmk_block_alloc(&b, 65537);
    GMK_ASSERT_NULL(pbig, "alloc > 64K fails");

    /* Write and verify data integrity */
    memset(p32, 0xAA, 32);
    memset(p1k, 0xBB, 1024);
    GMK_ASSERT_EQ(((uint8_t *)p32)[0], 0xAA, "p32 data intact");
    GMK_ASSERT_EQ(((uint8_t *)p1k)[1023], 0xBB, "p1k data intact");

    /* Free */
    gmk_block_free(&b, p32, 32);
    gmk_block_free(&b, p64, 64);
    gmk_block_free(&b, p100, 100);
    gmk_block_free(&b, p1k, 1024);
    gmk_block_free(&b, p64k, 65536);

    gmk_block_destroy(&b);
    free(mem);
}

static void test_reuse_after_free(void) {
    size_t mem_size = 64 * 1024;
    void *mem = aligned_alloc(64, mem_size);
    gmk_block_t b;
    GMK_ASSERT_EQ(gmk_block_init(&b, mem, mem_size), 0, "init");

    /* Allocate and free repeatedly */
    for (int round = 0; round < 100; round++) {
        void *p = gmk_block_alloc(&b, 256);
        GMK_ASSERT_NOT_NULL(p, "alloc in loop");
        memset(p, (uint8_t)round, 256);
        gmk_block_free(&b, p, 256);
    }

    gmk_block_destroy(&b);
    free(mem);
}

static void test_small_alloc(void) {
    size_t mem_size = 64 * 1024;
    void *mem = aligned_alloc(64, mem_size);
    gmk_block_t b;
    GMK_ASSERT_EQ(gmk_block_init(&b, mem, mem_size), 0, "init");

    /* Allocate sizes smaller than 32 should go to bin 0 */
    void *p1 = gmk_block_alloc(&b, 1);
    GMK_ASSERT_NOT_NULL(p1, "alloc 1 byte");

    void *p16 = gmk_block_alloc(&b, 16);
    GMK_ASSERT_NOT_NULL(p16, "alloc 16 bytes");

    gmk_block_free(&b, p1, 1);
    gmk_block_free(&b, p16, 16);

    gmk_block_destroy(&b);
    free(mem);
}

int main(void) {
    GMK_TEST_BEGIN("alloc_block");
    GMK_RUN_TEST(test_various_sizes);
    GMK_RUN_TEST(test_reuse_after_free);
    GMK_RUN_TEST(test_small_alloc);
    GMK_TEST_END();
    return 0;
}
