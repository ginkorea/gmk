/*
 * GMK/cpu — Minimal page table manipulation for MMIO mapping
 */
#ifndef GMK_PAGING_H
#define GMK_PAGING_H

#include <stdint.h>
#include <stddef.h>

/* Map a range of physical MMIO pages into the HHDM virtual address space.
 * phys must be page-aligned. size is rounded up to page boundary. */
void map_mmio(uint64_t phys, size_t size);

#endif /* GMK_PAGING_H */
