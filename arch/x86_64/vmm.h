/*
 * GMK/cpu — Virtual Memory Manager
 *
 * Provides a kernel heap with demand paging and TLB shootdown.
 */
#ifndef GMK_VMM_H
#define GMK_VMM_H

#include <stdint.h>
#include <stddef.h>

/* Kernel heap range: 128 MB starting at 0xffffffff88000000 */
#define VMM_HEAP_BASE  0xffffffff88000000ULL
#define VMM_HEAP_SIZE  (128ULL * 1024 * 1024)
#define VMM_HEAP_END   (VMM_HEAP_BASE + VMM_HEAP_SIZE)

void  vmm_init(void);

/* Update the active CPU count (call after APs are verified ready). */
void  vmm_set_cpu_count(uint32_t count);

/* Map physical pages at an arbitrary virtual address.
 * Returns the virtual address, or NULL on failure. */
void *vmm_map(uint64_t phys, size_t size, uint64_t flags);

/* Unmap virtual pages and free the underlying physical memory. */
void  vmm_unmap(void *virt, size_t size);

/* Kernel heap allocator (bump). Pages are demand-mapped on first access. */
void *kmalloc(size_t size);

/* Free kernel heap memory. Unmaps pages and returns them to PMM. */
void  kfree(void *ptr, size_t size);

/* Called from page fault handler for demand paging in the heap range.
 * Returns 0 on success (page mapped), -1 if fault is not in heap range. */
int   vmm_demand_page(uint64_t fault_addr);

/* TLB shootdown: invalidate a page on all CPUs. */
void  vmm_tlb_shootdown(uint64_t virt_addr);

#endif /* GMK_VMM_H */
