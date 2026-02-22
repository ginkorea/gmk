/*
 * GMK/cpu — Page table manipulation
 */
#ifndef GMK_PAGING_H
#define GMK_PAGING_H

#include <stdint.h>
#include <stddef.h>

/* Page table entry flags */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)  /* Page Write-Through */
#define PTE_PCD      (1ULL << 4)  /* Page Cache Disable (for MMIO) */
#define PTE_PS       (1ULL << 7)  /* Page Size (2MB/1GB huge page) */
#define PTE_NX       (1ULL << 63) /* No Execute */

/* Map a range of physical MMIO pages into the HHDM virtual address space.
 * phys must be page-aligned. size is rounded up to page boundary. */
void map_mmio(uint64_t phys, size_t size);

/* Map a single 4KB page: virt → phys with given flags. */
void paging_map(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

/* Unmap a single 4KB page. Returns the physical address that was mapped,
 * or 0 if the page was not present. Does NOT free the physical page. */
uint64_t paging_unmap(uint64_t virt_addr);

/* Read CR3 (page table root physical address). */
uint64_t paging_read_cr3(void);

/* Allocate a zeroed page for use as a page table. */
uint64_t paging_alloc_table(void);

/* Invalidate a single TLB entry on the local CPU. */
static inline void paging_invlpg(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
}

#endif /* GMK_PAGING_H */
