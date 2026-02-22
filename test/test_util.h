/*
 * GMK/cpu — Test utilities
 * Assertion macros and test harness helpers.
 */
#ifndef GMK_TEST_UTIL_H
#define GMK_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gmk_test_count = 0;
static int gmk_test_pass  = 0;
static int gmk_test_fail  = 0;

#define GMK_TEST_BEGIN(name) \
    do { \
        printf("=== TEST: %s ===\n", name); \
        gmk_test_count = 0; \
        gmk_test_pass  = 0; \
        gmk_test_fail  = 0; \
    } while (0)

#define GMK_TEST_END() \
    do { \
        printf("--- Results: %d/%d passed", gmk_test_pass, gmk_test_count); \
        if (gmk_test_fail > 0) \
            printf(" (%d FAILED)", gmk_test_fail); \
        printf(" ---\n\n"); \
        if (gmk_test_fail > 0) return 1; \
    } while (0)

#define GMK_ASSERT(cond, msg) \
    do { \
        gmk_test_count++; \
        if (!(cond)) { \
            printf("  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
            gmk_test_fail++; \
        } else { \
            gmk_test_pass++; \
        } \
    } while (0)

#define GMK_ASSERT_EQ(a, b, msg) \
    do { \
        gmk_test_count++; \
        if ((a) != (b)) { \
            printf("  FAIL [%s:%d]: %s (got %lld, expected %lld)\n", \
                   __FILE__, __LINE__, msg, \
                   (long long)(a), (long long)(b)); \
            gmk_test_fail++; \
        } else { \
            gmk_test_pass++; \
        } \
    } while (0)

#define GMK_ASSERT_NE(a, b, msg) \
    do { \
        gmk_test_count++; \
        if ((a) == (b)) { \
            printf("  FAIL [%s:%d]: %s (values are equal: %lld)\n", \
                   __FILE__, __LINE__, msg, (long long)(a)); \
            gmk_test_fail++; \
        } else { \
            gmk_test_pass++; \
        } \
    } while (0)

#define GMK_ASSERT_NULL(p, msg) \
    do { \
        gmk_test_count++; \
        if ((p) != NULL) { \
            printf("  FAIL [%s:%d]: %s (expected NULL)\n", \
                   __FILE__, __LINE__, msg); \
            gmk_test_fail++; \
        } else { \
            gmk_test_pass++; \
        } \
    } while (0)

#define GMK_ASSERT_NOT_NULL(p, msg) \
    do { \
        gmk_test_count++; \
        if ((p) == NULL) { \
            printf("  FAIL [%s:%d]: %s (got NULL)\n", \
                   __FILE__, __LINE__, msg); \
            gmk_test_fail++; \
        } else { \
            gmk_test_pass++; \
        } \
    } while (0)

#define GMK_RUN_TEST(fn) \
    do { \
        printf("  [%s]\n", #fn); \
        fn(); \
    } while (0)

#endif /* GMK_TEST_UTIL_H */
