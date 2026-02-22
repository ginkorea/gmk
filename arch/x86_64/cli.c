/*
 * GMK/cpu — Kernel monitor CLI over serial
 *
 * BSP runs this loop instead of gmk_worker_loop.
 * Polls COM1, processes commands, prints to serial via kprintf.
 * All buffers are static — no heap allocation.
 */
#include "cli.h"
#include "serial.h"
#include "idt.h"
#include "vmm.h"
#include "paging.h"
#include "pmm.h"
#include "pci.h"
#include "smp.h"
#include "lapic.h"
#include "mem.h"
#include "../../include/gmk/worker.h"
#include "../../include/gmk/metrics.h"
#include "../../include/gmk/module.h"
#include "../../drivers/virtio/virtio_blk.h"

/* ── Line buffer ─────────────────────────────────────────────────── */

#define CLI_LINE_MAX  256
#define CLI_ARGC_MAX  8

static char     cli_line[CLI_LINE_MAX];
static uint32_t cli_pos;
static int      cli_last_was_cr;

/* Scratch page for blkread/blkwrite DMA */
static uint8_t *blk_scratch;

/* Kernel pointer (set by cli_run) */
static gmk_kernel_t *cli_kernel;

/* ── Argument parsing helpers ────────────────────────────────────── */

static uint64_t parse_u64(const char *s, int *ok) {
    uint64_t val = 0;
    *ok = 0;
    if (!s || !*s) return 0;

    /* Hex prefix */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return 0;
        while (*s) {
            char c = *s++;
            uint64_t d;
            if (c >= '0' && c <= '9')      d = (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f')  d = (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')  d = (uint64_t)(c - 'A' + 10);
            else return 0;
            val = (val << 4) | d;
        }
        *ok = 1;
        return val;
    }

    /* Decimal */
    while (*s) {
        char c = *s++;
        if (c < '0' || c > '9') return 0;
        val = val * 10 + (uint64_t)(c - '0');
    }
    *ok = 1;
    return val;
}

/* ── String helpers (freestanding) ───────────────────────────────── */

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a++ != *b++) return 0;
    }
    return *a == *b;
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Print string left-padded with spaces to fill `width` columns */
static void print_padded(const char *s, int width) {
    kprintf("%s", s);
    int len = str_len(s);
    for (int i = len; i < width; i++)
        serial_putc(' ');
}

/* ── Command handlers ────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *help;
    void (*fn)(int argc, char **argv);
} cli_cmd_t;

/* Forward declarations */
static void cmd_help(int argc, char **argv);
static void cmd_cpu(int argc, char **argv);
static void cmd_mem(int argc, char **argv);
static void cmd_heap(int argc, char **argv);
static void cmd_map(int argc, char **argv);
static void cmd_pci(int argc, char **argv);
static void cmd_blkcap(int argc, char **argv);
static void cmd_blkread(int argc, char **argv);
static void cmd_blkwrite(int argc, char **argv);
static void cmd_tasks(int argc, char **argv);
static void cmd_mod(int argc, char **argv);
static void cmd_metrics(int argc, char **argv);
static void cmd_uptime(int argc, char **argv);
static void cmd_halt(int argc, char **argv);
static void cmd_reboot(int argc, char **argv);

static volatile int cli_running;

static const cli_cmd_t commands[] = {
    { "help",     "Show this help",              cmd_help     },
    { "cpu",      "CPU and worker status",       cmd_cpu      },
    { "mem",      "Physical memory usage",       cmd_mem      },
    { "heap",     "Kernel heap status",          cmd_heap     },
    { "map",      "Walk page tables for address", cmd_map     },
    { "pci",      "List PCI devices",            cmd_pci      },
    { "blkcap",   "Block device capacity",       cmd_blkcap   },
    { "blkread",  "Read and hex-dump sector",    cmd_blkread  },
    { "blkwrite", "Write pattern to sector",     cmd_blkwrite },
    { "tasks",    "Task dispatch statistics",    cmd_tasks    },
    { "mod",      "List modules and handlers",   cmd_mod      },
    { "metrics",  "Global metric counters",      cmd_metrics  },
    { "uptime",   "System uptime",               cmd_uptime   },
    { "halt",     "Shutdown kernel",             cmd_halt     },
    { "reboot",   "Reboot system",               cmd_reboot   },
};

#define N_COMMANDS (sizeof(commands) / sizeof(commands[0]))

/* ── help ────────────────────────────────────────────────────────── */

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    for (uint32_t i = 0; i < N_COMMANDS; i++) {
        kprintf("  ");
        print_padded(commands[i].name, 12);
        kprintf("%s\n", commands[i].help);
    }
}

/* ── cpu ─────────────────────────────────────────────────────────── */

static void cmd_cpu(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t n = cli_kernel->pool.n_workers;
    kprintf("CPUs: %u online  BSP LAPIC=%u (console)\n",
            n, smp_bsp_lapic_id());
    for (uint32_t i = 1; i < n; i++) {
        gmk_worker_t *w = &cli_kernel->pool.workers[i];
        uint64_t disp = gmk_atomic_load(&w->tasks_dispatched,
                                         memory_order_relaxed);
        bool parked = gmk_atomic_load(&w->parked, memory_order_relaxed);
        kprintf("  worker %u  LAPIC %u  dispatched %lu  %s\n",
                i, w->cpu_id, (unsigned long)disp,
                parked ? "parked" : "active");
    }
}

/* ── mem ─────────────────────────────────────────────────────────── */

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    size_t usable = pmm_usable_count();
    size_t free   = pmm_free_count();
    size_t used   = usable - free;
    kprintf("Physical memory:\n");
    kprintf("  Usable: %lu pages (%lu MB)\n",
            (unsigned long)usable, (unsigned long)(usable * 4 / 1024));
    kprintf("  Free:   %lu pages (%lu MB)\n",
            (unsigned long)free, (unsigned long)(free * 4 / 1024));
    kprintf("  Used:   %lu pages (%lu MB)\n",
            (unsigned long)used, (unsigned long)(used * 4 / 1024));
}

/* ── heap ────────────────────────────────────────────────────────── */

static void cmd_heap(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t used = vmm_get_heap_used();
    uint64_t free_bytes = VMM_HEAP_SIZE - used;
    kprintf("Kernel heap:\n");
    kprintf("  Range: 0x%lx - 0x%lx\n",
            (unsigned long)VMM_HEAP_BASE, (unsigned long)VMM_HEAP_END);
    kprintf("  Used:  %lu bytes\n", (unsigned long)used);
    kprintf("  Free:  %lu bytes\n", (unsigned long)free_bytes);
}

/* ── map ─────────────────────────────────────────────────────────── */

static void cmd_map(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: map <addr>\n");
        return;
    }
    int ok;
    uint64_t addr = parse_u64(argv[1], &ok);
    if (!ok) {
        kprintf("Invalid address: %s\n", argv[1]);
        return;
    }

    uint64_t pml4e, pdpe, pde, pte;
    int depth = paging_walk(addr, &pml4e, &pdpe, &pde, &pte);

    kprintf("Page table walk for 0x%lx:\n", (unsigned long)addr);
    kprintf("  PML4E[%u]: 0x%lx %s\n",
            (unsigned)((addr >> 39) & 0x1FF), (unsigned long)pml4e,
            (pml4e & PTE_PRESENT) ? "" : "(not present)");

    if (depth < 2) return;

    kprintf("  PDPE[%u]:  0x%lx %s%s\n",
            (unsigned)((addr >> 30) & 0x1FF), (unsigned long)pdpe,
            (pdpe & PTE_PRESENT) ? "" : "(not present)",
            (pdpe & PTE_PS) ? " [1GB page]" : "");

    if (depth < 3 || (pdpe & PTE_PS)) return;

    kprintf("  PDE[%u]:   0x%lx %s%s\n",
            (unsigned)((addr >> 21) & 0x1FF), (unsigned long)pde,
            (pde & PTE_PRESENT) ? "" : "(not present)",
            (pde & PTE_PS) ? " [2MB page]" : "");

    if (depth < 4 || (pde & PTE_PS)) return;

    kprintf("  PTE[%u]:   0x%lx %s\n",
            (unsigned)((addr >> 12) & 0x1FF), (unsigned long)pte,
            (pte & PTE_PRESENT) ? "" : "(not present)");

    if (pte & PTE_PRESENT) {
        kprintf("  -> phys 0x%lx  flags:%s%s%s\n",
                (unsigned long)(pte & ~0xFFFULL),
                (pte & PTE_WRITABLE) ? " W" : " R",
                (pte & PTE_USER) ? " U" : " S",
                (pte & PTE_NX) ? " NX" : "");
    }
}

/* ── pci ─────────────────────────────────────────────────────────── */

static void cmd_pci(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t count = pci_device_count();
    kprintf("PCI devices: %u\n", count);
    for (uint32_t i = 0; i < count; i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        kprintf("  %u:%u.%u  %x:%x  class %x:%x",
                d->bus, d->dev, d->func,
                d->vendor_id, d->device_id,
                d->class_code, d->subclass);
        if (d->bar[0])
            kprintf("  BAR0=0x%x", d->bar[0]);
        kprintf("\n");
    }
}

/* ── blkcap ──────────────────────────────────────────────────────── */

static void cmd_blkcap(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t cap = virtio_blk_capacity();
    if (cap == 0) {
        kprintf("No block device\n");
        return;
    }
    kprintf("Block device: %lu sectors (%lu KB)\n",
            (unsigned long)cap, (unsigned long)(cap / 2));
}

/* ── blkread ─────────────────────────────────────────────────────── */

static void cmd_blkread(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: blkread <lba>\n");
        return;
    }
    if (!blk_scratch) {
        kprintf("No block device\n");
        return;
    }

    int ok;
    uint64_t lba = parse_u64(argv[1], &ok);
    if (!ok) {
        kprintf("Invalid LBA: %s\n", argv[1]);
        return;
    }

    uint64_t cap = virtio_blk_capacity();
    if (lba >= cap) {
        kprintf("LBA %lu out of range (capacity %lu)\n",
                (unsigned long)lba, (unsigned long)cap);
        return;
    }

    int rc = virtio_blk_read(lba, blk_scratch);
    if (rc != 0) {
        kprintf("Read failed\n");
        return;
    }

    kprintf("Sector %lu (512 bytes):\n", (unsigned long)lba);
    for (int row = 0; row < 4; row++) {
        int off = row * 16;
        kprintf("  %x: ", (unsigned)(off & 0xFFFF));
        for (int col = 0; col < 16; col++)
            kprintf("%x%x ", blk_scratch[off + col] >> 4,
                    blk_scratch[off + col] & 0xF);
        kprintf("\n");
    }
}

/* ── blkwrite ────────────────────────────────────────────────────── */

static void cmd_blkwrite(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: blkwrite <lba> <byte>\n");
        return;
    }
    if (!blk_scratch) {
        kprintf("No block device\n");
        return;
    }

    int ok;
    uint64_t lba = parse_u64(argv[1], &ok);
    if (!ok) {
        kprintf("Invalid LBA: %s\n", argv[1]);
        return;
    }

    uint64_t cap = virtio_blk_capacity();
    if (lba >= cap) {
        kprintf("LBA %lu out of range (capacity %lu)\n",
                (unsigned long)lba, (unsigned long)cap);
        return;
    }

    uint64_t byte_val = parse_u64(argv[2], &ok);
    if (!ok || byte_val > 255) {
        kprintf("Invalid byte: %s (must be 0-255)\n", argv[2]);
        return;
    }

    /* Fill scratch buffer with pattern */
    for (int i = 0; i < 512; i++)
        blk_scratch[i] = (uint8_t)byte_val;

    int rc = virtio_blk_write(lba, blk_scratch);
    if (rc != 0) {
        kprintf("Write failed\n");
        return;
    }

    /* Verify: read back */
    uint8_t verify_buf[512];
    rc = virtio_blk_read(lba, verify_buf);
    if (rc != 0) {
        kprintf("Write OK, verify read failed\n");
        return;
    }

    int match = 1;
    for (int i = 0; i < 512; i++) {
        if (verify_buf[i] != (uint8_t)byte_val) { match = 0; break; }
    }
    kprintf("Sector %lu: write 0x%x, verify %s\n",
            (unsigned long)lba, (unsigned)byte_val,
            match ? "PASS" : "FAIL");
}

/* ── tasks ───────────────────────────────────────────────────────── */

static void cmd_tasks(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t total = 0;
    for (uint32_t i = 1; i < cli_kernel->pool.n_workers; i++) {
        uint64_t count = gmk_atomic_load(
            &cli_kernel->pool.workers[i].tasks_dispatched,
            memory_order_relaxed);
        total += count;
        kprintf("  worker %u: %lu tasks\n", i, (unsigned long)count);
    }
    kprintf("Total: %lu dispatched\n", (unsigned long)total);
}

/* ── mod ─────────────────────────────────────────────────────────── */

static void cmd_mod(int argc, char **argv) {
    (void)argc; (void)argv;
    gmk_module_reg_t *mr = &cli_kernel->modules;
    kprintf("Modules: %u registered\n", mr->n_modules);
    for (uint32_t i = 0; i < mr->n_modules; i++) {
        gmk_module_t *m = mr->modules[i];
        if (!m) continue;
        kprintf("  [%u] %s  v%u.%u.%u  handlers=%u  channels=%u\n",
                i, m->name,
                (m->version >> 16) & 0xFF,
                (m->version >> 8) & 0xFF,
                m->version & 0xFF,
                m->n_handlers, m->n_channels);
        for (uint32_t j = 0; j < m->n_handlers; j++) {
            kprintf("      type %u: %s\n",
                    m->handlers[j].type,
                    m->handlers[j].name ? m->handlers[j].name : "?");
        }
    }
}

/* ── metrics ─────────────────────────────────────────────────────── */

static const char *metric_names[] = {
    "tasks_enqueued", "tasks_dequeued", "tasks_dispatched",
    "tasks_failed",   "tasks_retried",  "tasks_yielded",
    "alloc_bytes",    "alloc_fails",    "chan_emits",
    "chan_drops",      "chan_full",       "worker_parks",
    "worker_wakes",
};

static void cmd_metrics(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Global metrics:\n");
    for (uint32_t i = 0; i < 13; i++) {
        uint64_t val = gmk_metric_get(&cli_kernel->metrics, i);
        kprintf("  ");
        print_padded(metric_names[i], 20);
        kprintf("%lu\n", (unsigned long)val);
    }
}

/* ── uptime ──────────────────────────────────────────────────────── */

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t ticks = idt_get_timer_count();
    uint64_t secs = ticks / 1000;
    uint64_t ms   = ticks % 1000;
    /* Manual zero-pad for ms (kprintf lacks width specifiers) */
    kprintf("Uptime: %lu.", (unsigned long)secs);
    if (ms < 100) serial_putc('0');
    if (ms < 10)  serial_putc('0');
    kprintf("%lu s (%lu ticks)\n", (unsigned long)ms, (unsigned long)ticks);
}

/* ── halt ────────────────────────────────────────────────────────── */

static void cmd_halt(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Stopping workers...\n");

    /* Stop all AP workers */
    for (uint32_t i = 1; i < cli_kernel->pool.n_workers; i++)
        gmk_atomic_store(&cli_kernel->pool.workers[i].running, false,
                         memory_order_release);

    /* Wake APs so they see the flag */
    for (uint32_t i = 1; i < cli_kernel->pool.n_workers; i++)
        gmk_worker_wake(&cli_kernel->pool.workers[i]);

    /* Brief delay for APs to exit */
    for (volatile uint32_t d = 0; d < 1000000; d++)
        __asm__ volatile("pause");

    /* Print results */
    uint64_t total = 0;
    for (uint32_t i = 1; i < cli_kernel->pool.n_workers; i++) {
        uint64_t count = gmk_atomic_load(
            &cli_kernel->pool.workers[i].tasks_dispatched,
            memory_order_relaxed);
        total += count;
        kprintf("  worker %u: %lu tasks\n", i, (unsigned long)count);
    }
    kprintf("Total: %lu dispatched\n", (unsigned long)total);

    cli_running = 0;
}

/* ── reboot ──────────────────────────────────────────────────────── */

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Rebooting...\n");
    /* Triple-fault: load null IDT, trigger interrupt */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed))
        null_idt = { 0, 0 };
    __asm__ volatile("lidt (%0); int $0" :: "r"(&null_idt));
    /* Should not reach here */
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── Parser ──────────────────────────────────────────────────────── */

static void cli_dispatch(void) {
    /* Null-terminate the line */
    cli_line[cli_pos] = '\0';

    /* Skip empty lines */
    if (cli_pos == 0) return;

    /* Tokenize */
    char *argv[CLI_ARGC_MAX];
    int argc = 0;
    int in_token = 0;

    for (uint32_t i = 0; i < cli_pos && argc < CLI_ARGC_MAX; i++) {
        if (cli_line[i] == ' ' || cli_line[i] == '\t') {
            cli_line[i] = '\0';
            in_token = 0;
        } else if (!in_token) {
            argv[argc++] = &cli_line[i];
            in_token = 1;
        }
    }

    if (argc == 0) return;

    /* Look up command */
    for (uint32_t i = 0; i < N_COMMANDS; i++) {
        if (str_eq(argv[0], commands[i].name)) {
            commands[i].fn(argc, argv);
            return;
        }
    }

    kprintf("Unknown command: %s (type 'help')\n", argv[0]);
}

/* ── CLI loop ────────────────────────────────────────────────────── */

static void cli_prompt(void) {
    kprintf("gmk> ");
}

void cli_run(gmk_kernel_t *kernel) {
    cli_kernel = kernel;
    cli_running = 1;
    cli_pos = 0;
    cli_last_was_cr = 0;

    /* Allocate PMM scratch page for block I/O */
    if (virtio_blk_capacity() > 0) {
        uint64_t phys = pmm_alloc_pages(1);
        if (phys)
            blk_scratch = (uint8_t *)phys_to_virt(phys);
    }

    kprintf("\n=== GMK Kernel Monitor ===\n");
    kprintf("Type 'help' for available commands.\n\n");
    cli_prompt();

    while (cli_running) {
        if (serial_data_ready()) {
            int ch = serial_getc();
            if (ch < 0) continue;

            /* CR/LF normalization: skip \n after \r */
            if (ch == '\n' && cli_last_was_cr) {
                cli_last_was_cr = 0;
                continue;
            }
            cli_last_was_cr = (ch == '\r');

            if (ch == '\r' || ch == '\n') {
                kprintf("\n");
                cli_dispatch();
                cli_pos = 0;
                if (cli_running)
                    cli_prompt();
            } else if (ch == 0x08 || ch == 0x7F) {
                /* Backspace (BS) or Delete (DEL) */
                if (cli_pos > 0) {
                    cli_pos--;
                    kprintf("\b \b");
                }
            } else if (ch >= 0x20 && ch < 0x7F) {
                /* Printable character */
                if (cli_pos < CLI_LINE_MAX - 1) {
                    cli_line[cli_pos++] = (char)ch;
                    serial_putc((char)ch);
                }
            }
        } else {
            /* Idle: wait for interrupt (LAPIC timer wakes every ~1ms) */
            __asm__ volatile("sti; hlt");
        }
    }
}
