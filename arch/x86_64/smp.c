/*
 * GMK/cpu — SMP bringup via Limine
 */
#include "smp.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "serial.h"
#include "mem.h"
#include "boot_alloc.h"
#include "limine/limine.h"
#include "../../include/gmk/error.h"
#include "../../include/gmk/arch/thread.h"

static struct limine_smp_response *smp_resp;
static uint32_t cpu_count;
static uint32_t bsp_lapic;

/* AP entry wrapper — called by Limine's goto_address mechanism */
struct ap_entry_ctx {
    void (*fn)(void *);
    void *arg;
    uint32_t cpu_idx;
};

static struct ap_entry_ctx ap_contexts[GMK_MAX_WORKERS] = {{0}};

static void ap_trampoline(struct limine_smp_info *info) {
    /* Set up this CPU's GDT and IDT */
    gdt_init();
    idt_load();

    /* Initialize local APIC */
    lapic_init();
    lapic_timer_init(1000); /* 1ms periodic */

    /* Retrieve our context from extra_argument */
    struct ap_entry_ctx *ctx = (struct ap_entry_ctx *)(uintptr_t)info->extra_argument;

    kprintf("CPU %u (LAPIC %u) ready\n", ctx->cpu_idx, info->lapic_id);

    /* Call the real entry function */
    ctx->fn(ctx->arg);

    /* Should never return */
    for (;;) __asm__ volatile("cli; hlt");
}

uint32_t smp_init(void *smp_response) {
    smp_resp = (struct limine_smp_response *)smp_response;
    if (!smp_resp) return 1;

    cpu_count  = (uint32_t)smp_resp->cpu_count;
    bsp_lapic  = smp_resp->bsp_lapic_id;

    kprintf("SMP: %u CPUs detected, BSP LAPIC ID = %u\n", cpu_count, bsp_lapic);
    return cpu_count;
}

void smp_start_aps(void (*ap_entry)(void *), void *arg) {
    if (!smp_resp) return;

    for (uint64_t i = 0; i < smp_resp->cpu_count; i++) {
        struct limine_smp_info *cpu = smp_resp->cpus[i];

        /* Skip BSP */
        if (cpu->lapic_id == bsp_lapic) continue;

        /* Find the CPU index (BSP is always 0, APs are 1..N-1) */
        uint32_t cpu_idx = 0;
        uint32_t idx = 0;
        for (uint64_t j = 0; j < smp_resp->cpu_count; j++) {
            if (smp_resp->cpus[j]->lapic_id == bsp_lapic) continue;
            idx++;
            if (j == i) { cpu_idx = idx; break; }
        }

        /* Set up context for this AP */
        ap_contexts[cpu_idx].fn      = ap_entry;
        ap_contexts[cpu_idx].arg     = arg;
        ap_contexts[cpu_idx].cpu_idx = cpu_idx;

        /* Pass context via extra_argument */
        cpu->extra_argument = (uint64_t)(uintptr_t)&ap_contexts[cpu_idx];

        /* Write goto_address — this wakes the AP */
        __atomic_store_n(&cpu->goto_address, ap_trampoline, __ATOMIC_SEQ_CST);
    }
}

uint32_t smp_bsp_lapic_id(void) {
    return bsp_lapic;
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}

uint32_t smp_lapic_id(uint32_t cpu_idx) {
    if (!smp_resp || cpu_idx >= cpu_count) return 0;
    return smp_resp->cpus[cpu_idx]->lapic_id;
}
