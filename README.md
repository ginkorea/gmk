# GMK — GPU Microkernel

A system of peer microkernels spanning GPU and CPU. The GPU kernel (GMK/gpu) owns compute: scheduling, memory, channels, and module dispatch via persistent CUDA kernels. The CPU kernel (GMK/cpu) owns peripherals: NIC, NVMe, DMA, and the GPU driver interface. Both kernels share the same architectural primitives — tasks, channels, modules, allocators, and observability. They communicate through cross-kernel channels over PCIe DMA. Neither is subordinate. Together they are GMK.

## Status

**GMK/cpu v0.1** — the CPU-side microkernel — is implemented and tested. GMK/gpu (CUDA kernel) and cross-kernel bridge channels are planned for future work.

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
| **Workers** | N pthreads running gather-dispatch-park loops. Park via `pthread_cond_timedwait` (1ms). |
| **Boot** | `gmk_boot` initializes arena → scheduler → channels → modules → workers. `gmk_halt` tears down in reverse. |

## Building

```
make lib       # build build/libgmk_cpu.a
make test      # build + run all 15 test binaries
make clean     # remove build artifacts
```

Requires: GCC with C11 support, pthreads. No external dependencies.

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
