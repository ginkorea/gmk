# GMK — GPU Microkernel

A system of peer microkernels spanning GPU and CPU. The GPU kernel (GMK/gpu) owns compute: scheduling, memory, channels, and module dispatch via persistent CUDA kernels. The CPU kernel (GMK/cpu) owns peripherals: NIC, NVMe, DMA, and the GPU driver interface. Both kernels share the same architectural primitives — tasks, channels, modules, allocators, and observability. They communicate through cross-kernel channels over PCIe DMA. Neither is subordinate. Together they are GMK.

## Status

**GMK/cpu v0.1** — the CPU-side microkernel — is implemented and tested as both a hosted library and a bootable bare-metal x86_64 kernel. The same source files compile in both modes via `#ifdef GMK_FREESTANDING` guards. The hosted build runs on Linux with pthreads; the bare-metal build boots via the Limine protocol and runs on real or emulated x86_64 hardware.

GMK/gpu (CUDA kernel) and cross-kernel bridge channels are planned for future work.

## Architecture

```
GMK/gpu (GPU)                           GMK/cpu (CPU)
┌───────────────────────────┐          ┌───────────────────────────┐
│ Worker Blocks [0..N-1]    │          │ Worker Threads [0..M-1]   │
│   gather → dispatch → emit│          │   gather → dispatch → emit│
│                           │          │                           │
│ Scheduler (GRQ, LQs, EVQ)│          │ Scheduler (RQ, LQs, EVQ) │
│ Allocator (slab, bump)    │          │ Allocator (slab, block,   │
│ Module Registry           │          │   bump)                   │
│ Channel Registry          │          │ Module Registry           │
│ Observability             │          │ Channel Registry          │
│                           │          │ Observability             │
│   bridge.tx ─── DMA ──────────────── bridge.rx                  │
│   bridge.rx ─── DMA ──────────────── bridge.tx                  │
└───────────────────────────┘          └───────────────────────────┘
```

## Design Principles

- **Everything is a task.** One unit of work. One scheduling primitive. One mental model.
- **Channels are the composition primitive.** Modules communicate through named, typed channels — not function calls.
- **Observe everything.** Traces and metrics are wired into every queue operation, every allocation, every dispatch.
- **Fail gracefully.** Retry, backoff, poison detection, watchdog reset — these are the normal execution model.
- **One system, many kernels.** GMK/gpu and GMK/cpu are peers. A module does not know which kernel its channel partner lives on.

## GMK/cpu Subsystems

| Subsystem | Description |
|-----------|-------------|
| **Ring Buffers** | SPSC (local queues, trace) and Vyukov MPMC (ready queues, channels). Lock-free, power-of-two capacity. |
| **Allocator** | Single arena subdivided into task slab (10%), trace slab (2%), block allocator with 12 power-of-two bins (68%), and atomic bump allocator (20%). |
| **Scheduler** | 4-priority weighted ready queue, per-worker local queues with yield watermark, bounded binary min-heap event queue. |
| **Enqueue Core** | Single `_gmk_enqueue` path for all task routing. Cooperative yield with circuit breaker and overflow bucket. |
| **Channels** | Up to 256 named channels. P2P fast-path, fan-out with shared payload, priority-aware backpressure, dead-letter routing. |
| **Modules** | Function pointer dispatch table indexed by type ID. Poison detection via failure threshold. |
| **Workers** | N worker loops running gather-dispatch-park. Hosted: pthreads + `pthread_cond_timedwait`. Bare-metal: per-CPU `sti;hlt;cli` + LAPIC IPI wake. |
| **Boot** | `gmk_boot` initializes arena → scheduler → channels → modules → workers. `gmk_halt` tears down in reverse. |

## Bare-Metal Kernel

GMK/cpu runs as a freestanding x86_64 kernel booted by [Limine v8](https://github.com/limine-bootloader/limine). Each physical CPU becomes a GMK worker — BSP is worker 0, APs are workers 1..N-1.

### Boot Sequence

```
Limine → _kstart → serial(COM1) → GDT → IDT(256 vectors) → HHDM
       → PMM(bitmap) → boot_alloc(8MB bump) → LAPIC(1ms timer)
       → SMP(goto_address) → kmain → gmk_boot → worker loops
```

### Platform Abstraction

| Primitive | Hosted | Bare-Metal |
|-----------|--------|------------|
| Lock | `pthread_mutex` | Ticket spinlock |
| Worker park | `pthread_cond_timedwait` (1ms) | `sti; hlt; cli` (LAPIC timer wakes) |
| Worker wake | `pthread_cond_signal` | `lapic_send_ipi(cpu_id, 0xFE)` |
| Arena memory | `aligned_alloc` | PMM page allocation via HHDM |
| Kernel objects | `calloc` / `free` | Boot bump allocator (never frees) |
| Strings | `<string.h>` | Freestanding `memset`/`memcpy`/`memmove` |

### Arch File Layout

```
arch/x86_64/
  entry.c          _kstart + Limine request structs
  kmain.c          kernel main, test echo handler
  serial.c/.h      COM1 115200 8N1, kprintf
  gdt.c/.h         5-entry GDT, segment reload
  idt.c/.h         256-entry IDT, exception handler, shutdown timer
  idt_stubs.S      ISR assembly thunks (.altmacro generated)
  pmm.c/.h         bitmap page frame allocator
  paging.c/.h      MMIO page mapping into HHDM
  mem.h            phys_to_virt / virt_to_phys
  boot_alloc.c/.h  boot-time bump allocator
  memops.c         freestanding memset/memcpy/memmove/memcmp
  lapic.c/.h       LAPIC init, EOI, IPI, periodic timer
  smp.c/.h         SMP bringup via Limine goto_address
  ctx_switch.S     callee-saved register + RSP swap
  linker.ld        higher-half at 0xffffffff80000000
  limine.conf      bootloader config
  limine/limine.h  vendored Limine v8 protocol header
```

## Building

### Hosted (library + tests)

```
make lib       # build build/libgmk_cpu.a
make test      # build + run all 15 test binaries (1321 assertions)
make clean     # remove build artifacts
```

Requires: GCC with C11 support, pthreads. No external dependencies.

### Bare-metal kernel

```
make kernel    # build build/gmk_kernel.elf (freestanding x86_64)
make iso       # build bootable ISO (requires xorriso + Limine)
make run       # boot in QEMU: 4 CPUs, 256MB, serial on stdio
make run-debug # same as run, but paused for GDB (-S -s)
```

Requires: GCC with C11 support, nasm (for Limine build), xorriso, qemu-system-x86_64.

To build Limine (if not installed system-wide):

```
git clone --depth=1 --branch=v8.6.0 https://github.com/limine-bootloader/limine.git /tmp/limine
cd /tmp/limine && ./bootstrap && ./configure --enable-bios --enable-bios-cd && make -j$(nproc)
```

Then `make iso` uses `LIMINE_DIR=/tmp/limine` by default.

### Grouped test targets

```
make test-ring     # SPSC/MPMC concurrent correctness
make test-alloc    # slab/block/bump alloc + free + stats
make test-sched    # priority pop, yield watermark, EVQ ordering, enqueue
make test-chan     # P2P, fan-out, backpressure, dead-letter
make test-module   # dispatch table, poison detection
make test-worker   # gather-dispatch loop, yield flow
make test-boot     # full boot → execute → halt lifecycle
```

## Quick Start

```c
#include "gmk/gmk.h"

static int my_handler(gmk_ctx_t *ctx) {
    uint64_t value = ctx->task->meta0;
    // ... process task ...
    return GMK_OK;
}

int main(void) {
    gmk_handler_reg_t handlers[] = {
        { .type = 1, .fn = my_handler, .name = "my_handler" },
    };
    gmk_module_t mod = {
        .name = "my_module",
        .version = GMK_VERSION(0, 1, 0),
        .handlers = handlers,
        .n_handlers = 1,
    };
    gmk_module_t *mods[] = { &mod };

    gmk_kernel_t kernel;
    gmk_boot_cfg_t cfg = {
        .arena_size = 64 * 1024 * 1024,  // 64 MB
        .n_workers  = 4,
        .n_tenants  = 1,
    };
    gmk_boot(&kernel, &cfg, mods, 1);

    // Submit tasks
    gmk_task_t task = { .type = 1, .meta0 = 42 };
    gmk_submit(&kernel, &task);

    // ... run workload ...

    gmk_halt(&kernel);
    return 0;
}
```

## Specs

- [PRD.md](PRD.md) — full kernel specification
- [USERLAND.md](USERLAND.md) — runtime libraries, standard modules, toolchain

## License

[MIT](LICENSE)
