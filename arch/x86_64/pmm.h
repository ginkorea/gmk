/*
 * GMK/cpu — Physical page frame allocator (bitmap)
 */
#ifndef GMK_PMM_H
#define GMK_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PMM_PAGE_SIZE 4096

void     pmm_init(void *memmap_response);
uint64_t pmm_alloc_pages(size_t count);
void     pmm_free_pages(uint64_t addr, size_t count);
size_t   pmm_free_count(void);
size_t   pmm_total_count(void);

/* Find the largest contiguous usable region >= min_size bytes.
   Returns physical address, or 0 on failure. Sets *out_size. */
uint64_t pmm_find_contiguous(size_t min_size, size_t *out_size);

#endif /* GMK_PMM_H */
