/*
 * GMK/cpu — Kernel entry point (_kstart)
 *
 * Limine loads us into higher-half. We initialize serial, GDT, IDT,
 * memory, then hand off to kmain.
 */
#include "limine/limine.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "mem.h"
#include "pmm.h"
#include "boot_alloc.h"
#include "paging.h"
#include "lapic.h"
#include "smp.h"

/* ── Limine base revision ───────────────────────────────────────── */
LIMINE_BASE_REVISION(3)

/* ── Limine requests ────────────────────────────────────────────── */
LIMINE_REQ
static volatile struct limine_entry_point_request entry_point_req = {
    .id = LIMINE_ENTRY_POINT_REQUEST,
    .revision = 0,
};

LIMINE_REQ
static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

LIMINE_REQ
static volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

LIMINE_REQ
static volatile struct limine_smp_request smp_req = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0,
};

/* ── HHDM offset (global) ──────────────────────────────────────── */
uint64_t hhdm_offset;

/* ── Forward declaration ────────────────────────────────────────── */
extern void kmain(uint32_t cpu_count);

/* ── Boot allocator region size ─────────────────────────────────── */
#define BOOT_ALLOC_SIZE (8ULL * 1024 * 1024) /* 8MB */

/* ── Kernel entry point ─────────────────────────────────────────── */
void _kstart(void) {
    /* 1. Serial console */
    serial_init();
    kprintf("\nGMK/cpu bare-metal kernel starting\n");

    /* 2. Verify Limine base revision */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        kprintf("ERROR: Limine base revision not supported\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* 3. GDT + IDT */
    gdt_init();
    idt_init();
    kprintf("GDT+IDT loaded\n");

    /* 4. HHDM setup */
    if (!hhdm_req.response) {
        kprintf("ERROR: No HHDM response from Limine\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    hhdm_offset = hhdm_req.response->offset;
    kprintf("HHDM offset: 0x%lx\n", hhdm_offset);

    /* 5. Physical memory manager */
    if (!memmap_req.response) {
        kprintf("ERROR: No memmap response from Limine\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    pmm_init((void *)memmap_req.response);

    /* 6. Boot allocator: carve 8MB from PMM */
    uint64_t ba_phys = pmm_alloc_pages(BOOT_ALLOC_SIZE / PMM_PAGE_SIZE);
    if (!ba_phys) {
        kprintf("ERROR: Cannot allocate boot allocator region\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    boot_alloc_init(phys_to_virt(ba_phys), BOOT_ALLOC_SIZE);
    kprintf("Boot alloc: %lu KB at phys 0x%lx\n",
            (unsigned long)(BOOT_ALLOC_SIZE / 1024), ba_phys);

    /* 7. Map LAPIC MMIO into HHDM (not covered by Limine's mapping) */
    map_mmio(0xFEE00000ULL, 0x1000);

    /* 8. LAPIC (BSP) */
    lapic_init();
    lapic_timer_init(1000); /* 1ms periodic */
    kprintf("LAPIC initialized (BSP ID = %u)\n", lapic_id());

    /* 9. SMP detection */
    uint32_t cpu_count = 1;
    if (smp_req.response) {
        cpu_count = smp_init((void *)smp_req.response);
    }

    /* 10. Hand off to kernel main */
    kmain(cpu_count);

    /* Should never reach here */
    kprintf("kmain returned — halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}
