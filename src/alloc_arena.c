/*
 * GMK/cpu — Arena init/destroy
 *
 * Hosted: aligned_alloc from libc.
 * Freestanding: PMM allocation (large contiguous physical region via HHDM).
 */
#include "gmk/alloc.h"

#ifdef GMK_FREESTANDING
#include "../../arch/x86_64/pmm.h"
#include "../../arch/x86_64/mem.h"
#else
#include <stdlib.h>
#include <string.h>
#endif

int gmk_arena_init(gmk_arena_t *a, size_t size) {
    if (!a || size == 0) return -1;

#ifdef GMK_FREESTANDING
    /* Allocate arena directly from PMM (large contiguous region) */
    size_t pages = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return -1;
    a->base = (uint8_t *)phys_to_virt(phys);
#else
    a->base = (uint8_t *)aligned_alloc(GMK_CACHE_LINE, size);
#endif
    if (!a->base) return -1;

    memset(a->base, 0, size);
    a->size = size;
    return 0;
}

void gmk_arena_destroy(gmk_arena_t *a) {
    if (a && a->base) {
#ifdef GMK_FREESTANDING
        size_t pages = (a->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        pmm_free_pages(virt_to_phys(a->base), pages);
#else
        free(a->base);
#endif
        a->base = NULL;
        a->size = 0;
    }
}
