/*
 * GMK/cpu — GDT: null, code64, data64, user code64, user data64
 */
#include "gdt.h"
#include <stdint.h>

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdtr;

static void set_entry(int i, uint8_t access, uint8_t gran) {
    gdt[i].limit_lo    = 0;
    gdt[i].base_lo     = 0;
    gdt[i].base_mid    = 0;
    gdt[i].access      = access;
    gdt[i].granularity = gran;
    gdt[i].base_hi     = 0;
}

void gdt_init(void) {
    /* 0: null */
    set_entry(0, 0x00, 0x00);

    /* 1: kernel code64 — 0x08 */
    set_entry(1, 0x9A, 0x20);  /* present, code, readable, long mode */

    /* 2: kernel data64 — 0x10 */
    set_entry(2, 0x92, 0x00);  /* present, data, writable */

    /* 3: user code64 — 0x18 */
    set_entry(3, 0xFA, 0x20);  /* present, ring3, code, readable, long mode */

    /* 4: user data64 — 0x20 */
    set_entry(4, 0xF2, 0x00);  /* present, ring3, data, writable */

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;

    __asm__ volatile(
        "lgdt (%0)\n\t"
        /* Reload CS via far return */
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* Reload data segment registers */
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        :
        : "r"(&gdtr)
        : "rax", "memory"
    );
}
