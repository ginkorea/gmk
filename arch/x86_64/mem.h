/*
 * GMK/cpu — HHDM physical/virtual address helpers
 */
#ifndef GMK_MEM_H
#define GMK_MEM_H

#include <stdint.h>

extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

static inline uint64_t virt_to_phys(void *virt) {
    return (uint64_t)virt - hhdm_offset;
}

#endif /* GMK_MEM_H */
