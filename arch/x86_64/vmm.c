/*
 * GMK/cpu — Virtual Memory Manager
 *
 * Simple bump-allocator kernel heap with demand paging.
 * Pages are not mapped until first access triggers a page fault.
 * TLB shootdown via IPI 0xFD for cross-CPU invalidation on unmap.
 */
#include "vmm.h"
#include "paging.h"
#include "pmm.h"
#include "mem.h"
#include "lapic.h"
#include "serial.h"
#include "../../include/gmk/arch/spinlock.h"
#include <stdatomic.h>

/* ── Heap state ──────────────────────────────────────────────────── */

static uint64_t heap_next;   /* next virtual address to hand out */
static gmk_spinlock_t heap_lock;

/* ── TLB shootdown state ─────────────────────────────────────────── */

static volatile _Atomic(uint64_t) tlb_shootdown_addr;
static volatile _Atomic(uint32_t) tlb_shootdown_ack;
static uint32_t total_cpus;

/* ── Init ────────────────────────────────────────────────────────── */

void vmm_init(void) {
    heap_next = VMM_HEAP_BASE;
    gmk_spinlock_init(&heap_lock);
    total_cpus = 1; /* only BSP running at init time */
    atomic_store_explicit(&tlb_shootdown_addr, 0, memory_order_relaxed);
    atomic_store_explicit(&tlb_shootdown_ack, 0, memory_order_relaxed);
    kprintf("VMM: heap 0x%lx - 0x%lx (%lu MB)\n",
            VMM_HEAP_BASE, VMM_HEAP_END,
            (unsigned long)(VMM_HEAP_SIZE >> 20));
}

void vmm_set_cpu_count(uint32_t count) {
    total_cpus = count;
}

/* ── Map arbitrary physical range to virtual ──────────────────────── */

void *vmm_map(uint64_t phys, size_t size, uint64_t flags) {
    if (size == 0) return 0;

    size_t pages = (size + 0xFFF) >> 12;
    uint64_t phys_aligned = phys & ~0xFFFULL;

    /* Allocate virtual space from the heap */
    gmk_spinlock_acquire(&heap_lock);
    uint64_t virt = heap_next;
    /* Align to page boundary */
    virt = (virt + 0xFFF) & ~0xFFFULL;
    heap_next = virt + (pages << 12);
    gmk_spinlock_release(&heap_lock);

    if (heap_next > VMM_HEAP_END) {
        PANIC("VMM heap exhausted");
    }

    /* Map each page immediately (not demand-paged — caller has physical addrs) */
    for (size_t i = 0; i < pages; i++) {
        paging_map(virt + (i << 12), phys_aligned + (i << 12), flags);
    }

    /* Return pointer adjusted for sub-page offset */
    return (void *)(virt + (phys & 0xFFF));
}

/* ── Unmap virtual pages and free physical memory ─────────────────── */

void vmm_unmap(void *virt, size_t size) {
    if (!virt || size == 0) return;

    uint64_t addr = (uint64_t)virt & ~0xFFFULL;
    size_t pages = ((uint64_t)virt + size - addr + 0xFFF) >> 12;

    for (size_t i = 0; i < pages; i++) {
        uint64_t page_virt = addr + (i << 12);
        uint64_t phys = paging_unmap(page_virt);
        if (phys) {
            pmm_free_pages(phys, 1);
        }
        /* Shootdown on all CPUs */
        if (total_cpus > 1) {
            vmm_tlb_shootdown(page_virt);
        }
    }
}

/* ── Kernel heap bump allocator ───────────────────────────────────── */

void *kmalloc(size_t size) {
    if (size == 0) return 0;

    /* Round up to 16-byte alignment */
    size = (size + 15) & ~15ULL;

    gmk_spinlock_acquire(&heap_lock);
    uint64_t addr = heap_next;
    /* Align to 16 bytes */
    addr = (addr + 15) & ~15ULL;
    heap_next = addr + size;
    gmk_spinlock_release(&heap_lock);

    if (heap_next > VMM_HEAP_END) {
        PANIC("VMM heap exhausted (kmalloc %lu bytes)", (unsigned long)size);
    }

    /* Pages will be demand-mapped on first access via page fault handler */
    return (void *)addr;
}

void kfree(void *ptr, size_t size) {
    if (!ptr || size == 0) return;

    uint64_t addr = (uint64_t)ptr & ~0xFFFULL;
    uint64_t end  = ((uint64_t)ptr + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t page = addr; page < end; page += 4096) {
        uint64_t phys = paging_unmap(page);
        if (phys) {
            pmm_free_pages(phys, 1);
            if (total_cpus > 1) {
                vmm_tlb_shootdown(page);
            }
        }
    }
    /* Note: virtual address space is not reclaimed (bump allocator) */
}

/* ── Demand paging handler ────────────────────────────────────────── */

int vmm_demand_page(uint64_t fault_addr) {
    /* Only handle faults in the heap range */
    if (fault_addr < VMM_HEAP_BASE || fault_addr >= VMM_HEAP_END)
        return -1;

    uint64_t page_virt = fault_addr & ~0xFFFULL;

    /* Allocate a physical page */
    uint64_t phys = pmm_alloc_pages(1);
    if (!phys) {
        PANIC("VMM: out of physical memory for demand page at 0x%lx", fault_addr);
    }

    /* Zero the page via HHDM (it's not mapped at page_virt yet) */
    uint8_t *tmp = (uint8_t *)phys_to_virt(phys);
    for (int i = 0; i < 4096; i++)
        tmp[i] = 0;

    /* Map it */
    paging_map(page_virt, phys, PTE_PRESENT | PTE_WRITABLE);
    return 0;
}

/* ── TLB shootdown via IPI ────────────────────────────────────────── */

/* ISR handler for vector 0xFD calls this */
void vmm_tlb_shootdown_handler(void) {
    uint64_t addr = atomic_load_explicit(&tlb_shootdown_addr, memory_order_acquire);
    if (addr) {
        paging_invlpg(addr);
    }
    atomic_fetch_add_explicit(&tlb_shootdown_ack, 1, memory_order_release);
}

void vmm_tlb_shootdown(uint64_t virt_addr) {
    if (total_cpus <= 1) return;

    /* Store the address for remote CPUs */
    atomic_store_explicit(&tlb_shootdown_addr, virt_addr, memory_order_release);
    atomic_store_explicit(&tlb_shootdown_ack, 0, memory_order_release);

    /* Send IPI to all other CPUs */
    lapic_send_ipi_all_but_self(IPI_TLB_VECTOR);

    /* Wait for all other CPUs to acknowledge */
    uint32_t expected = total_cpus - 1;
    while (atomic_load_explicit(&tlb_shootdown_ack, memory_order_acquire) < expected) {
        __builtin_ia32_pause();
    }

    /* Clear shootdown address */
    atomic_store_explicit(&tlb_shootdown_addr, 0, memory_order_release);
}
