/*
 * GMK/cpu — Freestanding string operations
 */
#ifndef GMK_ARCH_STRING_H
#define GMK_ARCH_STRING_H

#include <stdint.h>
#include <stddef.h>

/* Provided by arch/x86_64/memops.c */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);

static inline char *gmk_strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

static inline int gmk_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

/* Provide strncpy/strncmp as macros so existing code compiles */
#define strncpy gmk_strncpy
#define strncmp gmk_strncmp

#endif /* GMK_ARCH_STRING_H */
