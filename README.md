# GGMK — Gompert GPU Microkernel

A system of peer microkernels spanning GPU and CPU. The GPU kernel (GGMK/gpu) owns compute: scheduling, memory, channels, and module dispatch via persistent CUDA kernels. The CPU kernel (GGMK/cpu) owns peripherals: NIC, NVMe, DMA, and the GPU driver interface. Both kernels share the same architectural primitives — tasks, channels, modules, allocators, and observability. They communicate through cross-kernel channels over PCIe DMA. Neither is subordinate. Together they are GGMK.

## Status

**GGMK/cpu v0.2** — the CPU-side microkernel — is implemented and tested as both a hosted library and a bootable bare-metal x86_64 kernel. The same core source files compile in both modes with zero platform guards — a Hardware Abstraction Layer (`include/ggmk/hal.h`) is the single point of platform selection. The hosted build links the Linux HAL (pthreads, libc); the bare-metal build links the x86 baremetal HAL (spinlocks, LAPIC IPI, PMM) and boots via the Limine protocol.

The bare-metal kernel now owns real peripherals: PCI bus enumeration, virtio-blk block device I/O, a virtual memory manager with demand paging and TLB shootdown, PIT-calibrated LAPIC timer, and IRQ-safe serial output. All 4 CPUs participate in work stealing with deterministic SMP synchronization.

GGMK/gpu (CUDA kernel) and cross-kernel bridge channels are planned for future work.

## Architecture

```
GGMK/gpu (GPU)                           GGMK/cpu (CPU)
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
- **One system, many kernels.** GGMK/gpu and GGMK/cpu are peers. A module does not know which kernel its channel partner lives on.

## GGMK/cpu Subsystems

| Subsystem | Description |
|-----------|-------------|
| **Ring Buffers** | SPSC (local queues, trace) and Vyukov MPMC (ready queues, channels). Lock-free, power-of-two capacity. |
| **Allocator** | Single arena subdivided into task slab (10%), trace slab (2%), block allocator with 12 power-of-two bins (68%), and atomic bump allocator (20%). |
| **Scheduler** | 4-priority weighted ready queue, per-worker local queues with yield watermark, bounded binary min-heap event queue. |
| **Enqueue Core** | Single `_gmk_enqueue` path for all task routing. Cooperative yield with circuit breaker and overflow bucket. |
| **Channels** | Up to 256 named channels. P2P fast-path, fan-out with shared payload, priority-aware backpressure, dead-letter routing. |
| **Modules** | Function pointer dispatch table indexed by type ID. Poison detection via failure threshold. |
| **Workers** | N worker loops running gather-dispatch-park. Platform-specific parking/waking delegated to HAL (Linux: condvar; bare-metal: `sti;hlt;cli` + LAPIC IPI). |
| **HAL** | Hardware Abstraction Layer. One `#ifdef` in `hal.h` selects platform types. Linux HAL: pthreads, libc, clock_gettime. Baremetal HAL: spinlocks, LAPIC IPI, PMM, boot allocator. |
| **Boot** | `gmk_boot` initializes arena → scheduler → channels → modules → workers. `gmk_halt` tears down in reverse. |
| **PCI** | Legacy I/O port (0xCF8/0xCFC) bus 0 enumeration with multi-function support. BAR decode, device lookup by vendor/device ID. |
| **VMM** | Kernel heap (128 MB virtual range) with bump allocator, demand paging via page fault handler, and cross-CPU TLB shootdown via IPI. |
| **Virtio** | Legacy PCI transport (I/O BAR), split virtqueue setup, and virtio-blk driver for synchronous single-sector read/write with DMA. |

## Bare-Metal Kernel

GGMK/cpu runs as a freestanding x86_64 kernel booted by [Limine v8](https://github.com/limine-bootloader/limine). Each physical CPU becomes a GGMK worker — BSP is worker 0, APs are workers 1..N-1.

### Boot Sequence

```
Limine → _kstart → serial(COM1, IRQ-safe lock) → GDT → IDT(256 vectors)
       → HHDM → PMM(bitmap) → boot_alloc(8MB bump)
       → LAPIC(PIT-calibrated periodic timer) → VMM(128MB heap, demand paging)
       → PCI(bus 0 scan) → SMP(goto_address, per-AP ready flags)
       → kmain → virtio-blk init → gmk_boot → worker loops
```

### HAL (Hardware Abstraction Layer)

Core source files (`src/`, `include/ggmk/`) contain zero `#ifdef GMK_FREESTANDING` guards. All platform differences are isolated behind `include/ggmk/hal.h`, which selects the appropriate HAL implementation at compile time.

| HAL Function | Linux HAL (`hal/linux/`) | Baremetal HAL (`hal/x86_baremetal/`) |
|--------------|--------------------------|--------------------------------------|
| `gmk_hal_lock_*` | `pthread_mutex` | Ticket spinlock |
| `gmk_hal_park_wait` | `pthread_cond_timedwait` (CLOCK_MONOTONIC, 1ms) | `sti; hlt; cli` (LAPIC timer wakes) |
| `gmk_hal_park_wake` | `pthread_cond_signal` | `lapic_send_ipi(cpu_id, 0xFE)` |
| `gmk_hal_thread_create` | `pthread_create` | no-op (APs pre-started by SMP) |
| `gmk_hal_page_alloc` | `aligned_alloc` + memset | PMM page allocation via HHDM |
| `gmk_hal_calloc` | `calloc` | Boot bump allocator (never frees) |
| `gmk_hal_now_ns` | `clock_gettime(MONOTONIC_RAW)` | `idt_get_timer_count() * 1000000` |
| `gmk_hal_memset/memcpy` | libc | arch `memops.c` |

Additional bare-metal platform features (not behind HAL):

| Primitive | Implementation |
|-----------|---------------|
| Block I/O | Virtio-blk via PCI legacy transport |
| Timer calibration | PIT channel 2 → LAPIC ticks/ms |
| TLB management | `invlpg` + IPI 0xFD shootdown |
| Crash reporting | `PANIC()` → serial dump + halt |
| Kernel heap | VMM bump allocator + demand paging |

### File Layout

```
include/ggmk/
  hal.h            HAL API — the ONE #ifdef selecting platform types
  lock.h           gmk_lock_t (delegates to HAL)
  worker.h         worker pool (uses HAL thread/park types)
  platform.h       cache line, atomics, power-of-two helpers
  types.h          gmk_task_t, gmk_ctx_t, gmk_module_t
  ...              (alloc.h, boot.h, chan.h, error.h, sched.h, etc.)

hal/linux/
  hal_types.h      pthread-based type definitions
  thread.c         pthread_create/join
  lock.c           pthread_mutex
  park.c           condvar (CLOCK_MONOTONIC)
  time.c           clock_gettime(MONOTONIC_RAW)
  mem.c            aligned_alloc, calloc, free, memset, memcpy

hal/x86_baremetal/
  hal_types.h      spinlock/cpu_id type definitions
  thread.c         no-op (APs pre-started by SMP)
  lock.c           ticket spinlock
  park.c           sti;hlt;cli + LAPIC IPI
  time.c           LAPIC timer tick count
  mem.c            PMM page alloc, boot bump allocator

arch/x86_64/
  entry.c          _kstart + Limine request structs
  kmain.c          kernel main, test echo handler, smoke tests
  serial.c/.h      COM1 115200 8N1, kprintf (IRQ-safe lock), panic()
  gdt.c/.h         5-entry GDT, segment reload
  idt.c/.h         256-entry IDT, exception/fault handlers, shutdown timer
  idt_stubs.S      ISR assembly thunks (.altmacro generated)
  pmm.c/.h         bitmap page frame allocator
  paging.c/.h      4-level page table map/unmap, MMIO mapping
  vmm.c/.h         kernel heap (128 MB), demand paging, TLB shootdown
  pci.c/.h         PCI bus 0 enumeration via I/O ports 0xCF8/0xCFC
  mem.h            phys_to_virt / virt_to_phys
  boot_alloc.c/.h  boot-time bump allocator
  memops.c         freestanding memset/memcpy/memmove/memcmp/strncpy/strncmp
  lapic.c/.h       LAPIC init, PIT-calibrated timer, EOI, IPI
  smp.c/.h         SMP bringup via Limine goto_address
  ctx_switch.S     callee-saved register + RSP swap
  linker.ld        higher-half at 0xffffffff80000000
  limine.conf      bootloader config
  limine/limine.h  vendored Limine v8 protocol header

drivers/virtio/
  virtio.h         vring structs, constants, size helpers
  virtio_pci.c/.h  legacy I/O BAR transport, virtqueue setup
  virtio_blk.c/.h  block device driver (single-sector sync I/O)
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
make run       # boot in QEMU: 4 CPUs, 256MB, virtio-blk, serial on stdio
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
#include "ggmk/ggmk.h"

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
