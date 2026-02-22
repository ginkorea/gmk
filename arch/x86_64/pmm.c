/*
 * GMK/cpu — Physical page frame allocator (bitmap over Limine memmap)
 */
#include "pmm.h"
#include "mem.h"
#include "serial.h"
#include "limine/limine.h"

#define MAX_PAGES (1024 * 1024) /* up to 4GB */

static uint64_t bitmap[MAX_PAGES / 64];
static size_t   total_pages;
static size_t   free_pages;
static uint64_t highest_addr;

static inline void bitmap_set(size_t page) {
    bitmap[page / 64] |= (1ULL << (page % 64));
}

static inline void bitmap_clear(size_t page) {
    bitmap[page / 64] &= ~(1ULL << (page % 64));
}

static inline int bitmap_test(size_t page) {
    return (bitmap[page / 64] >> (page % 64)) & 1;
}

void pmm_init(void *memmap_resp) {
    struct limine_memmap_response *resp =
        (struct limine_memmap_response *)memmap_resp;

    if (!resp) return;

    /* First pass: find highest address to determine bitmap size */
    highest_addr = 0;
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        uint64_t top = e->base + e->length;
        if (top > highest_addr) highest_addr = top;
    }

    total_pages = highest_addr / PMM_PAGE_SIZE;
    if (total_pages > MAX_PAGES) total_pages = MAX_PAGES;

    /* Mark all pages as used initially */
    for (size_t i = 0; i < total_pages / 64 + 1; i++)
        bitmap[i] = ~0ULL;
    free_pages = 0;

    /* Second pass: mark usable regions as free */
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = (e->base + PMM_PAGE_SIZE - 1) & ~(uint64_t)(PMM_PAGE_SIZE - 1);
        uint64_t top  = (e->base + e->length) & ~(uint64_t)(PMM_PAGE_SIZE - 1);

        for (uint64_t addr = base; addr < top; addr += PMM_PAGE_SIZE) {
            size_t page = addr / PMM_PAGE_SIZE;
            if (page < total_pages) {
                bitmap_clear(page);
                free_pages++;
            }
        }
    }

    kprintf("PMM: %lu KB free (%lu pages of %lu total)\n",
            (unsigned long)(free_pages * 4),
            (unsigned long)free_pages,
            (unsigned long)total_pages);
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0 || free_pages < count) return 0;

    /* Simple first-fit search for contiguous pages */
    size_t run = 0;
    size_t start = 0;

    for (size_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) {
            run = 0;
            continue;
        }
        if (run == 0) start = i;
        run++;
        if (run == count) {
            /* Mark as allocated */
            for (size_t j = start; j < start + count; j++)
                bitmap_set(j);
            free_pages -= count;
            return start * PMM_PAGE_SIZE;
        }
    }

    return 0; /* not enough contiguous pages */
}

void pmm_free_pages(uint64_t addr, size_t count) {
    size_t start = addr / PMM_PAGE_SIZE;
    for (size_t i = start; i < start + count && i < total_pages; i++) {
        if (bitmap_test(i)) {
            bitmap_clear(i);
            free_pages++;
        }
    }
}

size_t pmm_free_count(void) {
    return free_pages;
}

size_t pmm_total_count(void) {
    return total_pages;
}

uint64_t pmm_find_contiguous(size_t min_size, size_t *out_size) {
    size_t min_pages = (min_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    size_t best_start = 0;
    size_t best_len = 0;
    size_t run = 0;
    size_t start = 0;

    for (size_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) {
            if (run > best_len) {
                best_start = start;
                best_len = run;
            }
            run = 0;
            continue;
        }
        if (run == 0) start = i;
        run++;
    }
    if (run > best_len) {
        best_start = start;
        best_len = run;
    }

    if (best_len < min_pages) return 0;

    if (out_size) *out_size = best_len * PMM_PAGE_SIZE;
    return best_start * PMM_PAGE_SIZE;
}
