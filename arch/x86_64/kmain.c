/*
 * GMK/cpu — Kernel main: boot GMK, submit test tasks, enter worker loop
 */
#include "serial.h"
#include "lapic.h"
#include "smp.h"
#include "idt.h"
#include "boot_alloc.h"
#include "pmm.h"
#include "mem.h"

#include "../../include/gmk/boot.h"
#include "../../include/gmk/worker.h"

/* ── Echo handler: prints task info to serial ───────────────────── */
static int echo_handler(gmk_ctx_t *ctx) {
    kprintf("[worker %u] echo: type=%u meta0=%lu\n",
            ctx->worker_id, ctx->task->type,
            (unsigned long)ctx->task->meta0);
    return GMK_OK;
}

/* ── Echo module definition ─────────────────────────────────────── */
static gmk_handler_reg_t echo_handlers[] = {
    {
        .type       = 1,
        .fn         = echo_handler,
        .name       = "echo",
        .flags      = GMK_HF_SCALAR,
        .max_yields = 0,
    },
};

static gmk_module_t echo_module = {
    .name       = "echo",
    .version    = GMK_VERSION(0, 1, 0),
    .handlers   = echo_handlers,
    .n_handlers = 1,
    .channels   = 0,
    .n_channels = 0,
    .init       = 0,
    .fini       = 0,
};

/* ── Kernel state (global so APs can access it) ─────────────────── */
static gmk_kernel_t kernel;

/* ── AP entry: find our worker and run the worker loop ──────────── */
static void ap_worker_entry(void *arg) {
    (void)arg;

    /* Find our worker by matching LAPIC ID */
    uint32_t my_lapic = lapic_id();
    gmk_worker_t *w = 0;
    for (uint32_t i = 0; i < kernel.pool.n_workers; i++) {
        if (kernel.pool.workers[i].cpu_id == my_lapic) {
            w = &kernel.pool.workers[i];
            break;
        }
    }

    if (w) {
        kprintf("CPU %u entering worker loop (LAPIC %u)\n", w->id, my_lapic);
        gmk_worker_loop(w);
    }

    /* Should not return; halt if it does */
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── Kernel main ────────────────────────────────────────────────── */
void kmain(uint32_t cpu_count) {
    kprintf("\n=== GMK/cpu kernel main ===\n");

    /* Configure boot */
    gmk_boot_cfg_t cfg = {
        .arena_size = GMK_DEFAULT_ARENA_SIZE,
        .n_workers  = cpu_count > GMK_MAX_WORKERS ? GMK_MAX_WORKERS : cpu_count,
        .n_tenants  = GMK_DEFAULT_TENANTS,
    };

    /* Register modules */
    gmk_module_t *modules[] = { &echo_module };

    kprintf("Booting GMK kernel: %u workers, %lu MB arena\n",
            cfg.n_workers, (unsigned long)(cfg.arena_size >> 20));

    /* Boot the kernel subsystems */
    int rc = gmk_boot(&kernel, &cfg, modules, 1);
    if (rc != 0) {
        kprintf("ERROR: gmk_boot failed (rc=%d)\n", rc);
        for (;;) __asm__ volatile("cli; hlt");
    }

    kprintf("GMK kernel booted successfully\n");

    /* Assign LAPIC IDs to workers for IPI wake */
    kernel.pool.workers[0].cpu_id = smp_bsp_lapic_id();
    for (uint32_t i = 1; i < cfg.n_workers; i++) {
        kernel.pool.workers[i].cpu_id = smp_lapic_id(i);
    }

    /* Submit 16 echo tasks */
    kprintf("Submitting 16 tasks...\n");
    for (uint32_t i = 0; i < 16; i++) {
        gmk_task_t task = {0};
        task.type  = 1; /* echo handler */
        task.flags = GMK_SET_PRIORITY(0, GMK_PRIO_NORMAL);
        task.meta0 = i;
        task.meta1 = 0;

        rc = gmk_submit(&kernel, &task);
        if (rc != 0) {
            kprintf("  submit %u failed: %d\n", i, rc);
        }
    }
    kprintf("Submitted 16 tasks.\n");

    /* Set shutdown timer: after 500 LAPIC timer ticks (~500ms),
     * BSP's running flag is cleared, causing it to exit the worker loop. */
    idt_set_shutdown_timer(500, &kernel.pool.workers[0].running);

    /* Start APs — each AP finds its worker and enters gmk_worker_loop */
    if (cpu_count > 1) {
        smp_start_aps(ap_worker_entry, &kernel.pool);
    }

    /* BSP becomes worker 0 and enters the worker loop */
    kprintf("BSP entering worker loop as worker 0\n");
    gmk_worker_loop(&kernel.pool.workers[0]);

    /* BSP exited — now stop all other workers */
    for (uint32_t i = 1; i < cfg.n_workers; i++)
        gmk_atomic_store(&kernel.pool.workers[i].running, false,
                        memory_order_release);
    gmk_worker_wake_all(&kernel.pool);

    /* Brief delay for APs to exit */
    for (volatile uint32_t d = 0; d < 1000000; d++)
        __asm__ volatile("pause");

    /* Report results */
    kprintf("\n=== Results ===\n");
    uint64_t total = 0;
    for (uint32_t i = 0; i < kernel.pool.n_workers; i++) {
        uint64_t count = gmk_atomic_load(
            &kernel.pool.workers[i].tasks_dispatched, memory_order_relaxed);
        total += count;
        kprintf("  worker %u dispatched %lu tasks\n", i, (unsigned long)count);
    }
    kprintf("Total: %lu / 16 tasks dispatched.\n", (unsigned long)total);

    kprintf("\nGMK kernel halted. System idle.\n");
    for (;;) __asm__ volatile("sti; hlt");
}
