/*
 * GMK/cpu — Minimal page table manipulation for MMIO mapping
 *
 * Limine's HHDM only covers physical RAM. MMIO regions (e.g., LAPIC at
 * 0xFEE00000) must be manually mapped into the existing page tables.
 */
#include "paging.h"
#include "mem.h"
#include "pmm.h"
#include "serial.h"

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_PWT      (1ULL << 3)  /* Page Write-Through */
#define PTE_PCD      (1ULL << 4)  /* Page Cache Disable (for MMIO) */
#define PTE_PS       (1ULL << 7)  /* Page Size (2MB/1GB huge page) */

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

/* Allocate a zeroed page table page from the PMM */
static uint64_t alloc_pt_page(void) {
    uint64_t phys = pmm_alloc_pages(1);
    if (!phys) {
        kprintf("FATAL: out of memory for page table\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    uint64_t *virt = (uint64_t *)phys_to_virt(phys);
    for (int i = 0; i < 512; i++)
        virt[i] = 0;
    return phys;
}

/* Map a single 4KB page: virt → phys with given flags */
static void map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t cr3 = read_cr3() & ~0xFFFULL;

    uint16_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    uint16_t pdp_idx  = (virt_addr >> 30) & 0x1FF;
    uint16_t pd_idx   = (virt_addr >> 21) & 0x1FF;
    uint16_t pt_idx   = (virt_addr >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(cr3);

    /* Ensure PDP table exists */
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t page = alloc_pt_page();
        pml4[pml4_idx] = page | PTE_PRESENT | PTE_WRITABLE;
    }

    uint64_t *pdp = (uint64_t *)phys_to_virt(pml4[pml4_idx] & ~0xFFFULL);

    /* If PDP entry is a 1GB huge page, the region is already mapped */
    if ((pdp[pdp_idx] & PTE_PRESENT) && (pdp[pdp_idx] & PTE_PS))
        return;

    /* Ensure PD table exists */
    if (!(pdp[pdp_idx] & PTE_PRESENT)) {
        uint64_t page = alloc_pt_page();
        pdp[pdp_idx] = page | PTE_PRESENT | PTE_WRITABLE;
    }

    uint64_t *pd = (uint64_t *)phys_to_virt(pdp[pdp_idx] & ~0xFFFULL);

    /* If PD entry is a 2MB huge page, the region is already mapped */
    if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_PS))
        return;

    /* Ensure PT exists */
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t page = alloc_pt_page();
        pd[pd_idx] = page | PTE_PRESENT | PTE_WRITABLE;
    }

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & ~0xFFFULL);

    /* Set the page table entry */
    pt[pt_idx] = (phys_addr & ~0xFFFULL) | flags;

    /* Invalidate TLB for this virtual address */
    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
}

void map_mmio(uint64_t phys, size_t size) {
    uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT;
    uint64_t end = (phys + size + 0xFFF) & ~0xFFFULL;
    uint64_t start = phys & ~0xFFFULL;

    for (uint64_t addr = start; addr < end; addr += 4096) {
        uint64_t virt = (uint64_t)phys_to_virt(addr);
        map_page(virt, addr, flags);
    }
}
