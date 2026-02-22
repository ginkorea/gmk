/*
 * GMK/cpu — Boot-time bump allocator for kernel-lifetime objects
 *
 * Never frees. Initialized with a region carved from PMM.
 * Used in freestanding mode for all calloc/aligned_alloc calls during boot.
 */
#ifndef GMK_BOOT_ALLOC_H
#define GMK_BOOT_ALLOC_H

#include <stdint.h>
#include <stddef.h>

void  boot_alloc_init(void *base, size_t size);
void *boot_aligned_alloc(size_t alignment, size_t size);
void *boot_calloc(size_t count, size_t size);
void  boot_free(void *ptr);   /* no-op */
size_t boot_alloc_used(void);
size_t boot_alloc_total(void);

#endif /* GMK_BOOT_ALLOC_H */
