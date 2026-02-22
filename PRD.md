# GMK — GPU Microkernel v0.1

A system of microkernels spanning GPU and CPU. The GPU kernel (GMK/gpu) owns compute: scheduling, memory, channels, and module dispatch via persistent CUDA kernels. The CPU kernel (GMK/cpu) owns peripherals: NIC, NVMe, DMA, and the GPU driver interface. Both kernels share the same architectural primitives — tasks, channels, modules, allocators, and observability. They communicate through cross-kernel channels over PCIe DMA. Neither is subordinate. Together they are GMK.

---

## 0. Design Philosophy

Six principles. These are not aspirations — they are constraints that resolve ambiguity when making implementation decisions.

### Everything is a task.

One unit of work. One scheduling primitive. One mental model. A task can represent a simulation tick, a key-value lookup, a log write, a channel message delivery. There is no second concept for "work." This is GMK's equivalent of Unix's "everything is a file" — a simplifying axiom that makes the system composable. When in doubt, make it a task.

### Channels are the composition primitive.

Modules do not call each other. They communicate through named, typed channels. A channel can be point-to-point or fan-out. Channels build systems from modules the same way Unix pipes build systems from programs. But channels are parallel, typed, and GPU-native — not byte streams.

### Two tiers, one system.

The kernel exposes a terse, powerful API (`gmk_emit`, `gmk_alloc`, `gmk_yield`). The module SDK wraps it in guardrails (`gmk_sdk_chan_send`, `gmk_sdk_kv_put`). Both tiers are first-class. The kernel API is for GPU systems programmers. The SDK is for domain users who want to build simulations, ML pipelines, or data processors without learning GPU memory ordering.

### Observe everything.

Traces and metrics are not bolted on. They are wired into every queue operation, every allocation, every dispatch. The system generates a continuous, structured stream of what happened and when. Debugging a GPU kernel should not require printf.

### Fail gracefully.

Tasks fail. Modules fail. The system keeps running. Retry, backoff, poison detection, watchdog reset — these are not error paths, they are the normal execution model. The happy path and the failure path share the same infrastructure.

### GPU-native, not GPU-ported.

This is not Unix on a GPU. The SIMT execution model, the memory hierarchy, the absence of a syscall boundary — these are features, not constraints. Every design decision starts from "what does the GPU want?" not "how did Unix do it?"

### One system, many kernels.

GMK is not a GPU kernel with a host helper. It is a system of peer microkernels. GMK/gpu runs on the GPU. GMK/cpu runs on the CPU. Each has its own scheduler, allocator, modules, and channels. They communicate through cross-kernel channels — same abstraction, different transport. A module does not know which kernel its channel partner lives on. When hardware changes — a second GPU, a remote node, GPUDirect bypassing the CPU — the module code does not change. Only the channel transport does.

---

## 1. Goals and Non-Goals

### Goals

* **System of peer microkernels**: GMK/gpu owns compute (scheduling, memory, channels, dispatch). GMK/cpu owns peripherals (NIC, NVMe, DMA, GPU driver interface). Both share the same architectural primitives.
* **GPU-resident ownership** of task scheduling, memory allocation, channel delivery, module dispatch, and observability within the GPU kernel.
* **Persistent execution**: GPU kernel(s) stay resident; no per-task host launches.
* **Cross-kernel channels**: same channel abstraction spans GPU↔CPU, with transport determined by topology, not by module code.
* **Named channels** as the inter-module and inter-kernel composition primitive.
* **Two-tier API surface**: kernel primitives and module SDK.
* **Structured concurrency**: tasks are queued and executed by workers (GPU thread blocks or CPU threads).
* **SoA-first**: data layout and APIs encourage coalesced access and SIMT efficiency on the GPU.
* **Pluggable modules**: on GPU, compiled device code; on CPU, compiled host code. Same module structure, same registration, same channel interface.

### Non-Goals (v0.1)

* General POSIX compatibility
* Byte-stream I/O (channels are typed, not byte pipes)
* POSIX signal semantics
* Arbitrary C ABI execution
* Full preemption of arbitrary GPU kernels
* Complex device driver model on GPU
* Multi-GPU coherence (single GPU only)

---

## 2. Architecture

### Components

GMK is a system of two peer microkernels:

**GMK/gpu** — the GPU-resident compute kernel:
1. **Persistent Kernel**: worker block loops, never exits.
2. **HBM State**: queues, allocators, channel registry, module registry, metrics, trace buffers.
3. **GPU Modules**: device code containing task handlers registered at init.
4. **GPU Channels**: named, typed, parallel communication paths (ring buffers in HBM).

**GMK/cpu** — the CPU-resident peripheral kernel:
1. **CPU Workers**: threads running the same gather-dispatch loop as GPU workers (adapted for CPU).
2. **Host Memory State**: queues, allocators, channel registry, module registry, metrics, trace buffers — mirrored structure in host memory.
3. **CPU Modules**: host code handling I/O, DMA, and hardware peripherals.
4. **CPU Channels**: named, typed communication paths (ring buffers in host memory).

**Cross-kernel infrastructure**:
1. **Bridge Channels**: channels whose producer and consumer live on different kernels. Transport is PCIe DMA. Same semantics as local channels.
2. **Boot Orchestrator**: runs on CPU, discovers hardware, boots GMK/cpu, then boots GMK/gpu via the `gpu_drv` module.

```
GMK/gpu (GPU)                           GMK/cpu (CPU)
┌───────────────────────────┐          ┌───────────────────────────┐
│ Worker Blocks [0..N-1]    │          │ Worker Threads [0..M-1]   │
│   gather → dispatch → emit│          │   gather → dispatch → emit│
│                           │          │                           │
│ Scheduler (GRQ, LQs, EVQ)│          │ Scheduler (RQ, LQs)      │
│ Allocator (slab, bump)    │          │ Allocator (host arenas)   │
│ Module Registry           │          │ Module Registry           │
│ Channel Registry          │          │ Channel Registry          │
│ Observability             │          │ Observability             │
│                           │          │                           │
│ Local channels:           │          │ Local channels:           │
│   sim.tick, kv.put, ...   │          │   nic.rx, nvme.read, ... │
│                           │          │                           │
│   bridge.tx ─── DMA ──────────────── bridge.rx                  │
│   bridge.rx ─── DMA ──────────────── bridge.tx                  │
└───────────────────────────┘          └───────────────────────────┘
                                       │
                                       │ CPU modules:
                                       │   gpu_drv  (wraps libcuda.so)
                                       │   nic      (network I/O)
                                       │   nvme     (storage I/O)
                                       │   watchdog (heartbeat monitor)
                                       │   cli      (gmk shell)
                                       └───────────────────────────
```

### Kernel Identity

Each microkernel instance has a unique identity:

* **Kernel ID**: `gpu.0`, `cpu.0`, `gpu.1`, etc.
* **Role**: `compute` (GPU) or `peripheral` (CPU).
* **Discovery**: at boot, GMK/cpu starts first, enumerates hardware, then boots each GMK/gpu via the `gpu_drv` module. Kernel IDs are assigned at discovery time.

In v0.1, exactly two kernels: `gpu.0` and `cpu.0`. Multi-GPU (`gpu.0`, `gpu.1`, ...) and multi-node (`cpu.0@host0`, `cpu.0@host1`) are v0.3.

### Worker Granularity

A "worker" is a **thread block**, not a single thread. All threads in a worker block cooperate on the same task (or task batch). This is a deliberate SIMT-aware design choice:

* If individual threads within a warp pulled different task types, the hardware would serialize their execution due to instruction pointer divergence. Performance would collapse.
* A full block executing one handler type means all warps share the same code path, the compiler can optimize register allocation, and shared memory is available for intra-handler cooperation.

For small tasks (e.g., a simple `KV_GET`), dedicating a full block to one task wastes occupancy. The solution is **gather-dispatch batching**:

1. The block's **lead warp** (warp 0) pops a batch of tasks from the LQ or GRQ.
2. The lead warp **sorts the batch by `type`** using a fast warp-level sort.
3. The block dispatches each homogeneous sub-batch to the handler, with all threads in the block cooperating on the batch.
4. For handlers that need only a single warp, multiple sub-batches can execute concurrently across warps within the block.

The batch size is configurable per worker (`GMK_WORKER_BATCH_SIZE`, default 32 — one per warp lane). Handlers declare whether they are **scalar** (one thread per task, batch across lanes), **warp** (one warp per task), or **block** (full block per task) via `GMK_HF_SCALAR`, `GMK_HF_WARP`, `GMK_HF_BLOCK` flags in handler registration.

### Execution Loop

A fixed number of worker blocks are launched and remain resident. Each worker block repeatedly:

1. **Gather**: lead warp pops a batch of tasks from the local queue (LQ). LQ tasks may be mixed-type (from channel delivery), so the lead warp sorts by type.
2. If LQ is empty, pop a batch from a GRQ type-bucket (already homogeneous — no sort needed).
3. Check the event queue (EVQ) for due events.
4. **Dispatch**: execute each homogeneous sub-batch via the handler, all threads cooperating on the same handler type.
5. Handlers execute, may emit follow-on tasks or channel messages, and return.
6. Worker updates metrics and trace, loops.
7. If no work found, spin briefly, then park.

No per-task host launches. All work is expressed as task records.

### Single Enqueue Core

"Everything is a task" must hold at the implementation level, not just the API level. All task scheduling — `gmk_emit`, channel P2P fast-path, channel CDQ drain, yield re-enqueue, EVQ fire, host.rx delivery — must funnel through a single internal `_gmk_enqueue(task, target)` primitive. This primitive handles LQ insertion, GRQ type-bucket insertion, yield watermark logic, and metric updates. No bypass paths. If a channel fast-path or yield overflow takes a shortcut that skips `_gmk_enqueue`, the system has two enqueue models and the "everything is a task" axiom is broken. One enqueue core. No exceptions.

### Channel Flow

When a module emits a task on channel `"sim.tick"`:
1. The task record is written to the channel's ring buffer (atomic tail advance).
2. The channel drain step moves the task to each subscriber's LQ (or GRQ for any-worker subscribers).
3. Fan-out channels duplicate the task header per subscriber; the payload pointer is shared read-only with **atomic reference counting** (see §9).
4. Subscribers receive the task as a normal handler invocation.

---

## 3. Memory Model

### Address Spaces

* **HBM_GLOBAL**: primary runtime state and user data.
* **HBM_ARENA[n]**: arenas for allocations (per subsystem/module/tenant).
* **HBM_CHANNEL**: ring buffers backing GPU-local channels and the GPU side of bridge channels. Separate from arena memory — channel buffers persist for the lifetime of the channel, not the task or tick.
* **HBM_BRIDGE_ALLOC**: reserved pool for payloads arriving from CPU→GPU bridge transfers. `gpu_drv` allocates from this pool when marshaling incoming payloads into HBM.
* **PINNED_HOST**: pinned host buffers used **exclusively** as DMA staging areas by `gpu_drv` for bridge channel transfers. No GPU-local queue or channel touches pinned memory. GPU scheduling (GRQ, LQs, EVQ) runs entirely in HBM.
* **HOST_ARENA[n]**: GMK/cpu's local memory arenas for CPU-side allocations and CPU-local channel ring buffers.

### Allocators

Arena-only allocation. No general malloc.

| Allocator | Use Case | API |
|---|---|---|
| **Slab** | Fixed-size objects (task nodes, events, small structs) | `gmk_alloc(ctx, size)` / `gmk_free(ctx, ptr)` |
| **Bump** | Transient per-tick allocations, reset each tick | `gmk_bump(ctx, size)` / `gmk_bump_reset(ctx)` |
| **Block** | Variable buffers (power-of-two bins) | `gmk_alloc(ctx, size)` (routed by size) |

### Requirements

All runtime allocations must be:
* Bounded
* Non-blocking (or bounded spin)
* Safe under high contention

Every allocator exposes:
* Total bytes
* Free bytes
* High watermark
* Alloc/fail counters

---

## 4. Task Model

### Task Record

All work is represented by a fixed header plus a payload pointer.

```c
typedef struct __align__(16) {
    uint32_t  type;         // handler id / message type
    uint16_t  flags;        // priority, determinism, etc.
    uint16_t  tenant;       // logical partition id (0..N-1)
    uint32_t  channel;      // source channel id (0 = direct submit)
    uint32_t  seq;          // monotonic enqueue sequence
    uint64_t  payload_ptr;  // pointer into HBM arena
    uint32_t  payload_len;  // bytes
    uint16_t  yield_count;  // runtime: incremented on each yield
    uint16_t  _pad;         // alignment
    uint64_t  meta0;        // inline fast arg / continuation state
    uint64_t  meta1;        // inline fast arg
} gmk_task_t;               // 48 bytes, 16-byte aligned
```

`channel` is a uint32 index into the channel registry. A value of 0 means direct submit (not from any channel). `seq` is a monotonic counter assigned at enqueue time, used for deterministic ordering.

### Task Context

Every handler receives a typed context — no more `void*` casting.

```c
typedef struct {
    gmk_task_t*     task;       // the current task
    gmk_alloc_t*    alloc;      // allocator for this tenant's arena
    gmk_chan_t*     chan;        // channel interface
    gmk_trace_t*    trace;      // trace writer
    gmk_metrics_t*  metrics;    // metrics counters
    uint32_t        worker_id;  // which worker block is executing
    uint32_t        tick;       // current logical tick (0 if not in tick mode)
} gmk_ctx_t;
```

Handler signature:

```c
typedef int (*gmk_handler_fn)(gmk_ctx_t* ctx);
```

Returns `GMK_OK`, `GMK_RETRY`, or `GMK_FAIL(code)`.

### Dispatch Model (v0.1)

Since all modules are statically linked in v0.1, the dispatch table is generated at compile time as a `switch(task->type)` statement rather than indirect function pointer calls. This gives the `nvcc` compiler full visibility into the call tree, enabling inter-procedural register optimization and avoiding GPU instruction cache (L1i) thrashing from scattered indirect jumps.

The `gmk_handler_fn` typedef is retained as the logical contract. The build system generates the dispatch switch from the module registry. In v0.2, when dynamic module loading arrives, dispatch falls back to indirect calls through the function pointer table.

### Register Pressure Isolation

A single `switch` over all handler types means `nvcc` sees every code path and allocates registers for the **worst case**. If 19 handlers use 16 registers and one physics handler uses 128, every thread in the block gets 128 registers — killing occupancy for the lightweight handlers.

The fix: the build system marks register-heavy handlers with `__noinline__`. This forces the compiler to isolate their register pressure behind a function call boundary. The call overhead is negligible (one instruction); the occupancy gain is massive.

The build system applies this automatically based on a configurable threshold (`GMK_INLINE_REG_LIMIT`, default 32). Handlers whose standalone register count exceeds the threshold are emitted as `__noinline__` in the generated dispatch code. Handlers below the threshold remain inlinable. The `gmk build` output reports per-handler register counts so developers can tune this:

```
$ gmk build --kernel
  handler kv_put:      16 regs (inline)
  handler kv_get:      14 regs (inline)
  handler phys_step:   96 regs (__noinline__)
  handler ai_forward: 112 regs (__noinline__)
Built: gmk.cubin (8 modules, 23 handlers, max_inline_regs=32)
  lmem: 0B (clean)
  shared_mem: max=32768B (ai_forward), blocks/SM: 2
```

### Local Memory Detection

Register spilling to local memory (a private slice of HBM) is a silent performance catastrophe — every spilled variable access becomes a ~400-cycle round trip. The build system parses `ptxas` output during `gmk build --kernel`. If any handler reports nonzero "Stack frame" or "Lmem" usage, the build emits a warning. If the spill exceeds `GMK_MAX_LMEM_BYTES` (default 0 = fail on any spill), the build fails. This forces developers to reduce register pressure (simplify the handler, split into phases with yield, or mark as `__noinline__`) rather than silently accepting a 10x throughput collapse.

### Shared Memory Budget

Persistent worker blocks cannot resize `__shared__` memory after launch. The build system computes the shared memory allocation as `max(shared_bytes)` across all registered handlers. If a handler declares `shared_bytes = 32768` and all others declare 0, every worker block gets 32KB of shared memory — reducing the number of concurrent blocks per SM.

To mitigate this, handlers with high shared memory requirements should be flagged `GMK_HF_BLOCK` and the scheduler can optionally **dedicate specific worker blocks** to high-SRAM handler types (configured via `gmk boot --dedicated-workers TYPE=N`). Dedicated workers only pull from that type's GRQ bucket, and can be launched with a different shared memory configuration. Non-dedicated workers use the default (smaller) shared memory allocation and are unaffected.

The build system reports per-handler shared memory usage and the computed block-level allocation:

```
$ gmk build --kernel
  ...
  shared_mem: kv_put=0B  phys_step=16384B  ai_forward=32768B
  worker shared_mem: 32768B (limited by ai_forward)
  blocks/SM: 2 (at 32768B shared)  -- would be 4 at 0B
```

### Flags

| Flag | Bits | Meaning |
|---|---|---|
| `GMK_TF_PRIORITY_MASK` | 2 | Priority 0..3 |
| `GMK_TF_DETERMINISTIC` | 1 | Execute in canonical order |
| `GMK_TF_IDEMPOTENT` | 1 | Safe to replay after crash |
| `GMK_TF_EMIT_TRACE` | 1 | Always emit a trace record |
| `GMK_TF_CHANNEL_MSG` | 1 | Delivered via channel (set by runtime) |

### Task Semantics

* Handlers are pure functions with side effects only through `gmk_ctx_t` services (alloc, channels, counters, traces).
* No hidden global mutation.
* Handlers must be bounded in time or yield cooperatively (see §6).

---

## 5. Queueing and Scheduling

### Queues

All in HBM:

1. **Global Ready Queue (GRQ)**: the primary cross-SM task distribution mechanism. The GRQ is **type-bucketed**: an array of MPMC ring buffers, one per registered handler type, with priority ordering within each bucket. When a worker needs work, it picks a type-bucket and drains a full batch of homogeneous tasks — no warp-level sorting needed. This eliminates pathological batch fragmentation (where a mixed-type batch of 32 tasks with 32 different types would serialize dispatch despite the gather-sort phase). Workers rotate across type-buckets to prevent starvation. To handle hot-bucket dominance (where a heavily trafficked type like `"kv.put"` replenishes faster than it drains, starving rare-but-critical types like `"sys.error"`), the scheduler enforces an **aging scan**: every `GMK_BUCKET_SCAN_INTERVAL` pops (default 64), the worker scans all non-empty buckets and selects the one with the highest `(priority * age)` weight, where age increments each time a non-empty bucket is skipped. This ensures cold buckets are eventually serviced regardless of hot-bucket throughput.
2. **Local Queue (LQ)**: per-worker SPSC (single-producer single-consumer) ring buffer. The channel system and the worker itself can enqueue tasks here for locality. Simpler and safer than a work-stealing deque on GPU hardware — avoids cross-SM cache coherence overhead and weak memory ordering hazards.
3. **Event Queue (EVQ)**: tick/time-ordered queue for simulation step progression. v0.1 uses a **bounded binary min-heap** (capacity `GMK_EVQ_SIZE`, default 64K entries). O(log n) insert and pop-min, simple to implement with atomics. This is deliberately kept simple — hierarchical timing wheels and O(1) amortized structures are v0.2 optimizations. The EVQ is checked once per worker loop iteration; only events with `tick <= current_tick` are popped. Per-tick work is bounded by `GMK_EVQ_DRAIN_LIMIT` (default 256) to prevent EVQ storms from starving normal task processing.
4. **Channel Delivery Queues (CDQ)**: per-channel ring buffers that feed tasks into the scheduler. When a task is emitted on a channel, the CDQ buffers it before routing to subscriber LQs or the GRQ.

v0.1 does not implement work-stealing. Each worker drains its own LQ and falls back to the GRQ. This is a deliberate simplification — full Chase-Lev deques require acquire/release semantics that are difficult to implement correctly under the GPU's relaxed memory model, and the cross-SM cache coherence overhead of stealing can stall SMs. Work-stealing is a v0.2 feature, contingent on profiling v0.1 under real workloads.

### Scheduler Policy

Worker block loop, in order:

1. Pop a batch from own LQ (up to `GMK_WORKER_BATCH_SIZE`).
2. If LQ empty, pop batch from GRQ (priority-weighted).
3. Check EVQ for due events.
4. Sort batch by type, dispatch (see §2 Worker Granularity).
5. If no work found, spin briefly, then park (decrement active worker count).

Channel-to-scheduler integration: when a task is emitted on a channel, the channel system enqueues it to the subscriber's LQ if the subscriber is a specific worker, or to the GRQ if the subscriber is any-worker. Fan-out channels duplicate the task header for each subscriber; the payload pointer is shared with reference counting.

### Priorities

4 fixed priority levels (0 = highest). Separate GRQ sub-queues per priority with weighted popping.

### Determinism Mode

When `GMK_TF_DETERMINISTIC` is set:

* Tasks execute in canonical order: stable sort by `(tick, priority, type, seq)`.
* Work stealing disabled for deterministic queues.
* Atomics limited to deterministic primitives (see §7).

---

## 6. Cooperative Yielding

No preemption in v0.1. The runtime uses cooperative yielding.

### Yield API

```c
// Simple yield — re-enqueue the current task
__device__ void gmk_yield(gmk_ctx_t* ctx);

// Yield with explicit phase — for multi-phase handlers
__device__ void gmk_yield_at(gmk_ctx_t* ctx, uint32_t phase);
```

`gmk_yield(ctx)` snapshots `meta0` and `meta1`, re-enqueues the task at current priority.

`gmk_yield_at(ctx, phase)` sets `meta0 = phase` before re-enqueue. The idiomatic pattern:

```c
__device__ int my_handler(gmk_ctx_t* ctx) {
    switch (ctx->task->meta0) {
    case 0:
        // phase 0: setup
        ctx->task->meta1 = partial_result;
        gmk_yield_at(ctx, 1);
        return GMK_OK;
    case 1:
        // phase 1: finish
        uint64_t partial = ctx->task->meta1;
        // ... complete work ...
        return GMK_OK;
    }
    return GMK_OK;
}
```

After calling `gmk_yield` or `gmk_yield_at`, the handler returns `GMK_OK`. The runtime handles re-enqueue.

### Yield Forward Progress Guarantee

Yields must never fail. If a handler calls `gmk_yield`, the task **must** be re-enqueued — otherwise work is silently lost, and under load the system livelocks as all workers spin trying to yield into full queues.

The guarantee is enforced structurally:

* Each LQ reserves a **yield watermark** — the last `GMK_LQ_YIELD_RESERVE` slots (default: 25% of LQ capacity) are exclusively for yielded tasks. Normal enqueue cannot fill past the watermark; yield enqueue can use the full buffer.
* If even the yield reserve is full (the worker is yielding faster than it can drain), the yielded task is promoted to the GRQ's dedicated **yield overflow bucket** — a single MPMC ring shared across all workers, drained at highest priority.
* If the yield overflow is full (system is catastrophically overloaded), the task is routed to `"sys.error"` with a `GMK_ERR_YIELD_OVERFLOW` code and a trace event is emitted. This is a hard failure visible to the host.

In practice, yield overflow should never trigger in a well-behaved system. If it does, it means handlers are yielding faster than the system can make progress — a design bug in the workload, not the runtime.

### Yield Circuit Breaker

A handler that never converges will yield indefinitely, creating a self-induced DoS. The runtime enforces a per-type **max yield count** (`max_yields` in the type registry, default 16). Each yield increments `task->yield_count` (a dedicated field in the task header — never `meta0` or `meta1`, which belong to the handler). When the count exceeds `max_yields`:

* The task is **not** re-enqueued.
* It is routed to `"sys.error"` with `GMK_ERR_YIELD_LIMIT` and a trace event.
* The task type's poison counter is incremented (feeding into the existing poison detection in §11).

This collapses yield storms into the existing fault model: retry → poison → watchdog. A handler that yields 16 times without completing is treated the same as a handler that fails 16 times — it's broken.

### Time Budget

Workers track handler execution time using `clock64()` sampled before dispatch and checked after each yield point. If elapsed cycles exceed `GMK_MAX_HANDLER_CYCLES` (configurable per handler type, default 1M cycles), the runtime forces a yield on the next check. This is best-effort — there is no hardware interrupt to preempt mid-instruction — but `clock64()` provides real cycle-level granularity rather than the soft heuristic of an instruction counter.

The build system reports `clock64()` overhead per handler in test mode so developers can tune budgets to their workload.

---

## 7. Synchronization

### Allowed (v0.1)

* `__syncthreads()` within a block
* Warp primitives (`__shfl*`, ballot, etc.)
* Atomics on 32/64-bit for counters, queue indices, and deterministic reductions

Channel operations are the preferred inter-module synchronization. Direct shared-memory atomics should be reserved for intra-module coordination.

### Deterministic Atomics

* `atomicAdd` on integers for counters.
* Deterministic reductions via two-phase: per-block partials, then deterministic finalize sorted by worker_id.

### Forbidden (v0.1)

* Nondeterministic "winner takes all" atomics in deterministic tasks.
* Global locks that can deadlock. Bounded spinlocks only (64 spins max) for rare paths.

---

## 8. Module System

### Module Definition

```c
typedef struct {
    const char*         name;       // e.g., "kv", "echo", "timer"
    uint32_t            version;    // GMK_VERSION(major, minor, patch)
    gmk_handler_reg_t*  handlers;
    uint32_t            n_handlers;
    gmk_chan_decl_t*    channels;   // channels this module declares
    uint32_t            n_channels;
    int (*init)(gmk_ctx_t* ctx);    // called once at boot
    int (*fini)(gmk_ctx_t* ctx);    // called at shutdown (best effort)
} gmk_module_t;
```

### Handler Registration

```c
typedef struct {
    uint32_t        type;           // handler id / task type
    gmk_handler_fn  fn;
    const char*     name;           // human-readable, e.g., "kv_put"
    uint32_t        flags;          // see below
    uint32_t        shared_bytes;   // shared memory request per block (0 = none)
} gmk_handler_reg_t;
```

Handler flags:

| Flag | Meaning |
|---|---|
| `GMK_HF_SCALAR` | One thread per task. Tasks batched across warp lanes. Default. |
| `GMK_HF_WARP` | One warp per task. For handlers needing warp-level cooperation. |
| `GMK_HF_BLOCK` | Full block per task. For handlers needing shared memory or block-wide sync. |
| `GMK_HF_DETERMINISTIC` | Handler supports deterministic mode. |
| `GMK_HF_NEEDS_SHARED` | Handler uses shared memory (requires `shared_bytes > 0`). |

The granularity flags (`SCALAR`, `WARP`, `BLOCK`) inform the gather-dispatch phase how to bin-pack tasks onto worker threads. Scalar handlers achieve the highest throughput; block handlers have the most resources.

The build system warns if more than `GMK_BLOCK_HANDLER_WARN_PCT` (default 25%) of handlers are marked `GMK_HF_BLOCK`. The architecture thrives on scalar batching — BLOCK handlers reduce throughput by dedicating an entire thread block to a single task. Developers should prefer SCALAR (batch across warp lanes) or WARP (one warp per task) unless the handler genuinely requires block-wide `__syncthreads()` or shared memory cooperation.

### Channel Declarations

Modules declare which channels they produce and consume:

```c
typedef struct {
    const char*     name;       // channel name, e.g., "sim.tick"
    uint32_t        direction;  // GMK_CHAN_PRODUCE | GMK_CHAN_CONSUME
    uint32_t        msg_type;   // expected task type
    uint32_t        mode;       // GMK_CHAN_P2P | GMK_CHAN_FANOUT
    uint32_t        guarantee;  // GMK_CHAN_LOSSY | GMK_CHAN_LOSSLESS
} gmk_chan_decl_t;
```

GMK/cpu validates all channel declarations at boot: type mismatches between producers and consumers are a boot-time error, not a runtime surprise.

### Module Lifecycle

1. **Link**: module object linked into the kernel image (v0.1 static).
2. **Register**: `gmk_module_t` structs are processed. Handlers enter the dispatch table. Channel declarations are validated.
3. **Init**: `module->init(ctx)` is called — allocate persistent state, open channels, set up internals.
4. **Run**: handlers are dispatched as tasks arrive.
5. **Fini**: `module->fini(ctx)` at shutdown. Best effort — if the GPU hangs, fini may not run.

### Registration Macro

```c
GMK_MODULE(mymod) = {
    .name = "mymod",
    .version = GMK_VERSION(0, 1, 0),
    .handlers = (gmk_handler_reg_t[]) { ... },
    .n_handlers = 1,
    .channels = (gmk_chan_decl_t[]) { ... },
    .n_channels = 2,
    .init = mymod_init,
};
```

`GMK_MODULE()` expands to a struct with linker annotations. The kernel scans a linker section for `gmk_module_t` entries at boot.

---

## 9. Channels

### Concept

A channel is a named, typed, parallel communication path between modules. Channels are the primary composition mechanism in GMK.

* **Named**: string names like `"sim.tick"`, `"kv.put"`, `"log.entry"`. Resolved to uint32 IDs at registration.
* **Typed**: each channel carries tasks of a specific handler type. Type mismatches caught at boot (static declarations) or runtime (dynamic opens).
* **Parallel**: lock-free ring buffer. Multiple warps can emit concurrently.
* **Observable**: every channel operation emits a trace event. Channel depth is a standard metric.

### Modes

**Point-to-Point (P2P)**: one consumer. Tasks delivered in FIFO order. Backpressure: `gmk_chan_emit` returns `GMK_CHAN_FULL` when the ring buffer is full.

**Fan-Out**: multiple consumers (up to `GMK_CHAN_MAX_SUBS`, default 32). Each emitted task is delivered to all subscribers. Task header is copied per subscriber; payload is shared read-only. Backpressure behavior depends on the channel's delivery guarantee (see below).

### Delivery Guarantees

Each channel declares a delivery guarantee in its channel declaration:

**`GMK_CHAN_LOSSY`** (default for fan-out): drop-slow semantics. If a subscriber's delivery queue is full, that subscriber's copy is dropped and a `CHAN_DROP` trace event is emitted. Other subscribers still receive the message. Suitable for metrics, logging, sampling, and any case where occasional loss is acceptable.

**`GMK_CHAN_LOSSLESS`**: no drops. If any subscriber's queue is full, `gmk_chan_emit` returns `GMK_CHAN_FULL` and the emitter must yield or retry. Every emitted message carries a monotonic **channel sequence number** (`chan_seq`). Subscribers track the last received `chan_seq` and can detect gaps. If a subscriber falls behind (e.g., after a restart), it can request catch-up from the channel's replay buffer (a configurable tail of recent messages, `GMK_CHAN_REPLAY_SLOTS`, default 0 = no replay).

Lossless channels are required for correctness-critical fan-out like `"sim.tick"` where a missed tick means state divergence. The cost is that a slow subscriber can stall all producers. The channel declaration makes this trade-off explicit and visible at boot time.

Replay buffer eviction is bounded: each subscriber maintains a `last_acked_seq` (advanced when the subscriber completes handling a message). The replay buffer evicts entries where `seq < min(last_acked_seq)` across all subscribers. A subscriber that falls too far behind (replay buffer full and subscriber still hasn't acked) triggers a `CHAN_REPLAY_OVERFLOW` trace event and the channel degrades to lossy for that subscriber until it catches up. This prevents a single slow subscriber from pinning replay memory indefinitely.

### Priority-Aware Backpressure

Channel ring buffers reserve the top `GMK_CHAN_PRIORITY_RESERVE` fraction (default 10%) of slots exclusively for tasks with priority 0 (highest). When the buffer is below the reserve threshold, only high-priority tasks can be enqueued; lower-priority tasks receive `GMK_CHAN_FULL`. This prevents a flood of low-priority work from starving critical system tasks like heartbeats or error signals.

### API

```c
int gmk_chan_open(gmk_ctx_t* ctx, const char* name, uint32_t mode, uint32_t buf_slots);
int gmk_chan_emit(gmk_ctx_t* ctx, uint32_t chan, gmk_task_t* task);
int gmk_chan_sub(gmk_ctx_t* ctx, uint32_t chan);
int gmk_chan_close(gmk_ctx_t* ctx, uint32_t chan);
```

Return codes: `GMK_OK`, `GMK_CHAN_FULL`, `GMK_CHAN_CLOSED`, `GMK_CHAN_TYPE_MISMATCH`, `GMK_CHAN_ALREADY_BOUND` (P2P second subscriber).

### Payload Ownership and Reference Counting

For P2P channels, ownership is simple: the consumer owns the payload after delivery and is responsible for releasing it via `gmk_payload_release(ctx)` (or the SDK does it automatically after the handler returns).

For fan-out channels, the payload is shared read-only across multiple subscribers. The allocator maintains an atomic reference counter in the block header metadata:

1. When a fan-out channel delivers a task, the ref-count is set to the number of successful subscriber deliveries.
2. Each subscriber calls `gmk_payload_release(ctx)` when done (the SDK does this automatically).
3. `gmk_payload_release` performs `atomicSub(&refcount, 1)`. When the count reaches zero, the payload memory is returned to the allocator.
4. If a subscriber's copy is dropped via drop-slow backpressure, the ref-count is decremented at drop time — no dangling references.

This ensures no use-after-free and no leaks, even under partial delivery.

**Retaining payloads beyond handler scope**: if a handler needs to store a `payload_ptr` into persistent state (e.g., KV or module-private memory), it must call `gmk_payload_retain(ctx)` before returning. This increments the ref-count, preventing the SDK's auto-release from freeing the payload. The handler (or its module) is then responsible for calling `gmk_payload_release(ctx)` later when the stored pointer is no longer needed. Failing to retain before stashing a pointer is a use-after-free bug; the build system's test harness (`gmk test`) detects this via a debug-mode canary that poisons freed payloads.

### Dead-Letter Channel

Tasks that are undeliverable — type mismatches at runtime, channel closed mid-flight, or messages dropped by the drop-slow mechanism — are routed to `"sys.dropped"`. This channel is always present and always fan-out. The `log` module subscribes to it by default.

`"sys.dropped"` is distinct from `"sys.error"` (which carries handler failures). Together they provide complete visibility into lost and failed work.

### Implementation

* Each channel backed by a ring buffer in `HBM_CHANNEL`.
* `gmk_chan_emit` writes a task record using atomic tail advance.
* Fan-out channels iterate the subscriber list and copy the task header for each. `payload_ptr` is shared with ref-counting as described above.

### Channel Drain Model (Distributed, Not Centralized)

There is no global "channel maintenance phase." Channel drain is **opportunistic and distributed** — it happens at the edges, driven by the workers and emitters that touch channels:

* **On emit**: for P2P channels with a single known subscriber, `gmk_chan_emit` can push directly to the subscriber's LQ or GRQ type-bucket (fast path, no CDQ buffering needed). For fan-out channels, the emit writes to the CDQ and the emitting warp drains pending entries for that channel immediately.
* **On LQ underflow**: when a worker's LQ is empty, before falling back to GRQ, it checks the CDQs of channels it subscribes to and drains any pending tasks into its own LQ.
* **On GRQ pop**: as a worker pops from a GRQ type-bucket, it opportunistically checks if the corresponding CDQs have pending work and drains them into the bucket.

This ensures drain work is proportional to actual channel activity and distributed across the workers that care about each channel. A system with 1000 channels but only 10 active ones does zero drain work for the idle 990.

**Exception: deterministic mode.** Opportunistic drain is inherently nondeterministic — drain order depends on which worker happens to touch which CDQ first. When `GMK_TF_DETERMINISTIC` tasks are in flight, channels carrying deterministic tasks use **synchronous drain at tick boundaries**:

> **Deterministic drain rule:** at the start of tick T, before any deterministic task is dispatched, all CDQs for deterministic channels are flushed exactly once, in ascending channel ID order. Tasks emitted onto deterministic channels during tick T are buffered and not visible until the drain at the start of tick T+1. There is no mid-tick drain for deterministic channels.

This is slower but guarantees that task delivery order is bit-for-bit reproducible across runs. The trace hash in the determinism acceptance test validates this. Any emission onto a deterministic channel outside of a tick boundary (e.g., from host.rx) is buffered until the next tick drain.

### Naming Convention

Dot-separated: `"namespace.verb"`.

* System: `"sys.tick"`, `"sys.error"`, `"sys.dropped"`, `"sys.trace"`
* Bridge: `"host.rx"`, `"host.tx"` (default GPU↔CPU bridge aliases)
* GPU modules: `"kv.put"`, `"kv.get"`, `"log.entry"`, `"timer.fire"`
* CPU modules: `"net.rx"`, `"net.tx"`, `"disk.read"`, `"disk.write"`, `"cli.cmd"`
* User: `"sim.step"`, `"ml.batch"`, `"data.ingest"`

Channel names are global across the system. A channel named `"sim.results"` is the same channel whether accessed from GMK/gpu or GMK/cpu. The channel registry resolves locality.

### Cross-Kernel Channels

Every channel between GMK/gpu and GMK/cpu is a bridge channel. The legacy names `"host.rx"` and `"host.tx"` are retained as the default bridge pair:
* `"host.rx"` — CPU → GPU (ingress events/data from peripherals)
* `"host.tx"` — GPU → CPU (egress results/requests to peripherals)

But any channel can be cross-kernel. A GPU module producing on `"sim.results"` and a CPU module consuming it — that's a bridge channel too. The channel declaration doesn't change. The transport (DMA vs ring buffer) is resolved at boot.

Modules subscribe to cross-kernel input using the same channel API they use for local communication. One mechanism.

---

## 10. Observability

### Metrics

Per tenant + global:

* Tasks enqueued/dequeued
* Queue depth high watermarks
* Handler success/fail counts by type
* Allocation stats (bytes, fails, fragmentation estimates)
* Tick duration stats
* Channel stats: per-channel depth, emit rate, full/drop counts, subscriber count

### Trace Records

Fixed-size trace events in a ring buffer:

```c
typedef struct __align__(16) {
    uint64_t  tsc;          // GPU clock or logical tick
    uint32_t  ev_type;      // GMK_EV_TASK_START, GMK_EV_TASK_END, etc.
    uint16_t  tenant;
    uint16_t  type;         // task type
    uint32_t  arg0;
    uint32_t  arg1;
} gmk_trace_ev_t;
```

### Trace Event Types

```
GMK_EV_TASK_START     GMK_EV_TASK_END       GMK_EV_TASK_FAIL
GMK_EV_TASK_RETRY     GMK_EV_TASK_YIELD     GMK_EV_ALLOC_FAIL
GMK_EV_CHAN_EMIT      GMK_EV_CHAN_FULL       GMK_EV_CHAN_DROP
GMK_EV_CHAN_DRAIN     GMK_EV_CHAN_OPEN       GMK_EV_CHAN_CLOSE
GMK_EV_WATCHDOG       GMK_EV_WORKER_PARK    GMK_EV_WORKER_WAKE
```

Traces are written to per-tenant ring buffers on each kernel. GMK/cpu collects its own traces locally. GMK/gpu traces are drained via bridge channel (`"sys.trace"`) without stalling the GPU kernel. The `cli` module on GMK/cpu aggregates traces from both kernels for unified display.

### Trace Cost Control

"Observe everything" does not mean "write everything." At 2M tasks/sec, emitting a 24-byte trace record for every `TASK_START` and `TASK_END` would generate ~96 MB/s of trace traffic — significant HBM bandwidth overhead. The observability system provides three levels of cost control:

**1. Per-module trace level** (runtime configurable):
Each module has a trace level: `GMK_TRACE_OFF`, `GMK_TRACE_ERROR`, `GMK_TRACE_WARN`, `GMK_TRACE_INFO`, `GMK_TRACE_ALL`. The default is `GMK_TRACE_WARN`. The `gmk_trace()` call checks the level before writing — a single branch, no memory traffic if filtered. Levels are configurable at runtime via the host CLI (`gmk trace --level INFO --module physics`).

**2. Statistical sampling** (runtime configurable):
When trace level is `GMK_TRACE_ALL`, a sampling rate controls what fraction of events are actually written. `GMK_TRACE_SAMPLE_RATE` (default 1.0 = every event, 0.01 = 1% of events). Sampling uses a fast per-worker PRNG — no atomics. Sampled traces include a `sampled` flag so analysis tools can scale up counts.

**3. Compile-time stripping** (build-time):
`gmk build --strip-trace` removes all `gmk_trace()` calls at compile time via preprocessor macros. This produces a zero-overhead production build with no trace infrastructure at all. Metrics (counters, gauges) are never stripped — they are always available.

Tasks with `GMK_TF_EMIT_TRACE` set bypass all filtering and are always traced regardless of module level or sampling rate. This ensures critical tasks can be individually tracked.

**Metrics are independent of traces.** Metric increments (`gmk_metric_inc`, handler success/fail counters, queue depth updates) execute unconditionally — they are never gated by trace level or sampling. Trace sampling controls what gets written to the trace ring buffer; it does not affect the accuracy of counters, gauges, or histograms. These are separate code paths. Tying a metric increment to a trace emission is a bug.

---

## 11. Fault Model and Recovery

### Error Classes

* **Soft Fail**: handler returns a fail code. Task dropped or routed to `"sys.error"` channel.
* **Retry**: handler returns `GMK_RETRY`. Microkernel re-enqueues with backoff counter.
* **Poison**: repeated failures mark a task type as unhealthy. Poison detection is **rate-based**: a type is flagged when it exceeds `GMK_POISON_RATE` failures per `GMK_POISON_WINDOW` ticks (defaults: 32 failures per 1000 ticks). This prevents transient error spikes from escalating — a burst of 30 failures in one tick followed by smooth operation does not trigger poison. A sustained 5% failure rate does. When a type is poisoned, new tasks of that type are routed directly to `"sys.error"` until the host explicitly clears the flag via `gmk mod reset TYPE`.

### Channel Error Handling

If a handler fails while processing a channel message, the message is routed to the channel's error handler (if registered) or to `"sys.error"`.

```c
int gmk_chan_on_error(gmk_ctx_t* ctx, uint32_t chan, gmk_handler_fn error_fn);
```

### Watchdog

* GPU workers update a per-worker atomic heartbeat counter every N iterations.
* The `watchdog` module on GMK/cpu polls GPU heartbeats via bridge channel at configurable interval (default 100ms).
* Watchdog thresholds are **configurable per handler type** (`GMK_WATCHDOG_CYCLES`, default 10M cycles). Under heavy HBM contention, even fast handlers may take longer than expected — a hard constant risks false positives. Thresholds should be tuned based on observed workload during bring-up. Adaptive thresholds (exponential moving average) are a v0.2 improvement.
* 2 missed heartbeats at the configured threshold = warning trace event.
* 5 missed heartbeats = `watchdog` requests GPU reset via `gpu_drv`, which resets and reboots GMK/gpu. GMK/cpu continues running throughout.

### Replay

Tasks marked `GMK_TF_IDEMPOTENT` may be re-enqueued after restart. Host may persist GRQ/EVQ snapshots for replay.

---

## 12. GMK/cpu — The CPU Kernel

GMK/cpu is a peer microkernel, not a shim. It runs on the CPU and owns all peripheral hardware. It shares the same architectural primitives as GMK/gpu: tasks, channels, modules, an allocator, and observability.

### Structure

GMK/cpu mirrors GMK/gpu's architecture, adapted for CPU hardware:

| Primitive | GMK/gpu | GMK/cpu |
|---|---|---|
| Workers | Thread blocks (CUDA) | OS threads or bare-metal cores |
| Scheduler | Type-bucketed GRQ + SPSC LQs | Simple run queue + per-thread LQs |
| Allocator | Slab/bump/block in HBM | Arena allocators in host memory |
| Channels | Ring buffers in HBM | Ring buffers in host memory |
| Dispatch | Compile-time switch | Function pointer table |
| Modules | Device code (`__device__`) | Host code (C/Rust) |

### Standard CPU Modules

| Module | Purpose |
|---|---|
| `gpu_drv` | GPU driver interface. Wraps `libcuda.so` + `nvidia.ko` (or bare-metal GPU init in Path 3). Handles GPU memory allocation, kernel launch, and DMA transfers. Produces/consumes bridge channels. This is a **driver module** — `libcuda.so` is a dependency of this module, not of the system. |
| `nic` | Network I/O. Manages NIC hardware (or kernel bypass via DPDK/io_uring in Path 1). Produces `"net.rx"`, consumes `"net.tx"`. |
| `nvme` | Storage I/O. Manages NVMe devices. Produces `"disk.read.resp"`, consumes `"disk.write"`, `"disk.read"`. |
| `watchdog` | Monitors GMK/gpu heartbeats via bridge channel. Triggers reset + reload on hang. |
| `cli` | The `gmk` command-line interface. Consumes `"cli.resp"`, produces `"cli.cmd"`. See USERLAND.md. |

### Cross-Kernel Channels (Bridge Channels)

A bridge channel connects a producer on one kernel to a consumer on another. The channel declaration is identical to a local channel — name, type, mode, guarantee. The system determines transport at boot based on producer/consumer kernel locality:

| Locality | Transport |
|---|---|
| Same kernel (GPU↔GPU or CPU↔CPU) | Ring buffer in local memory (HBM or host RAM) |
| GPU↔CPU | PCIe DMA via `gpu_drv` module |
| CPU↔CPU across nodes (v0.3) | Network via `nic` module |

**Modules do not know which transport their channels use.** A GPU module producing on `"sim.results"` uses the same `gmk_chan_emit` call whether the consumer is another GPU module (local ring buffer) or a CPU module (bridge via DMA). The channel registry resolves locality at boot and wires the appropriate transport.

Bridge channel implementation:
* The `gpu_drv` module on GMK/cpu owns the DMA path.
* GPU→CPU: `gpu_drv` polls the GPU-side bridge ring buffer via DMA reads, delivers tasks to GMK/cpu's local channels.
* CPU→GPU: `gpu_drv` writes tasks to the GPU-side bridge ring buffer via DMA writes.
* Bridge channels respect the same delivery guarantees (lossy/lossless) as local channels. DMA transfer failures are retried by `gpu_drv`; persistent failures route to `"sys.error"`.

The legacy `"host.rx"` and `"host.tx"` names are retained as aliases for the default bridge channels between `gpu.0` and `cpu.0`.

### Bridge DMA Batching

PCIe DMA round-trips cost 2–10μs — three orders of magnitude slower than HBM channel operations (~10ns). Transferring one task at a time across the bridge would bottleneck any high-throughput channel. The `gpu_drv` module uses **asynchronous batched DMA**:

* GPU-side bridge ring buffers live in **HBM** (not pinned host memory). GPU modules write to them at full HBM speed. There is no PCIe overhead on the GPU's hot path.
* `gpu_drv` on GMK/cpu runs a DMA poll loop. Each iteration, it issues a single DMA read that pulls **a batch** of pending tasks from the GPU-side ring buffer into a pinned host-memory staging area. Batch size is configurable (`GMK_BRIDGE_BATCH_SIZE`, default 64 tasks).
* The poll fires when either the batch is full or a timeout expires (`GMK_BRIDGE_POLL_US`, default 10μs). This amortizes PCIe latency across many tasks.
* CPU→GPU transfers use the same batching in reverse: tasks accumulate in a host-side staging buffer, then a single DMA write pushes the batch to the GPU-side ring buffer.

The staging areas (pinned host memory) are allocated by `gpu_drv` at boot. Only bridge channels touch pinned memory. All GPU-local channels (GRQ, LQs, CDQs, local ring buffers) remain in HBM — no pinned memory snooping, no PCIe overhead on the GPU's internal scheduling paths.

### Payload Marshaling

A `payload_ptr` on the GPU points to HBM. The CPU cannot dereference it. A `payload_ptr` on the CPU points to host memory. The GPU cannot dereference it. When a task crosses the bridge, the payload must be copied.

This is handled transparently by `gpu_drv`:

* **GPU→CPU**: `gpu_drv` reads the task header via DMA. If `payload_len > 0`, it issues a second DMA read to copy the payload from HBM to host memory, allocates a host-side buffer, and rewrites `payload_ptr` to point to the host copy. The GPU-side payload is released (ref-count decremented).
* **CPU→GPU**: `gpu_drv` allocates a buffer in the GPU's HBM arena (via a reserved bridge allocation pool), DMA-writes the payload, and rewrites `payload_ptr` to the HBM address.
* **Inline payloads** (`meta0`/`meta1` only, `payload_len == 0`): no copy needed. The 16 bytes travel in the task header. This is the fast path — design cross-kernel protocols to use inline payloads where possible.

Modules do not perform these copies. The bridge is transparent. A handler on GMK/cpu receives a task with a valid host-memory `payload_ptr`, just as if the task had been produced locally. The SDK's `GMK_PAYLOAD()` macro works identically on both sides.

**Large payload warning**: bridge channels carrying tasks with large payloads (>64KB) will saturate PCIe bandwidth under high throughput. The build system warns if a type carried on a bridge channel has a payload schema exceeding `GMK_BRIDGE_PAYLOAD_WARN_SIZE` (default 64KB). For bulk data transfer, prefer streaming many small tasks over one large payload — this amortizes DMA setup cost and allows interleaving with other bridge traffic.

### Cross-Kernel Determinism

Opportunistic bridge DMA is nondeterministic — transfer timing depends on poll intervals, PCIe bus contention, and CPU scheduling jitter. When deterministic tasks cross the bridge:

> **Cross-kernel deterministic rule**: at the start of tick T, GMK/gpu does not dispatch any deterministic task until `gpu_drv` on GMK/cpu has acknowledged that all CPU-produced tasks for tick T-1 have been delivered to the GPU-side bridge ring buffer. `gpu_drv` sends an explicit **tick fence** message after flushing each tick's cross-kernel tasks.

This means the GPU waits for the CPU at each tick boundary. The cost is one synchronous DMA round-trip per tick (~5μs). For simulations running at 1000+ ticks/second this adds ~5ms/second of sync overhead — acceptable for determinism guarantees. For non-deterministic workloads, the bridge runs fully asynchronous with no tick fences.

### Boot Sequence

1. **Hardware discovery**: bare-metal init or unikernel starts. PCIe enumeration. GPU(s) detected.
2. **GMK/cpu boot**: CPU kernel initializes — allocator, channel registry, module registry. CPU modules register.
3. **`gpu_drv` init**: loads NVIDIA driver (or bare-metal GPU init). Allocates HBM buffers. Uploads GMK/gpu kernel image.
4. **GMK/gpu launch**: `gpu_drv` launches the persistent GPU kernel. GPU workers start, GPU modules register, GPU channels initialize.
5. **Channel wiring**: both kernels exchange channel declarations. Bridge channels are established. Type mismatches across kernels are boot-time errors.
6. **System ready**: both kernels running, all channels wired, bridge DMA active.

```
$ gmk boot
GMK/cpu.0  booted |  4 workers |  5 modules (gpu_drv, nic, nvme, watchdog, cli)
GMK/gpu.0  booted | 64 workers | 12 modules (kv, echo, timer, log, ...)
bridge channels: 4 (host.rx, host.tx, sys.error, sys.trace)
system ready
```

### Fault Isolation

Each kernel is independently recoverable:
* If GMK/gpu hangs, `watchdog` on GMK/cpu detects it, resets the GPU via `gpu_drv`, and reboots GMK/gpu. GMK/cpu continues running.
* If a CPU module crashes, GMK/cpu's scheduler routes to `"sys.error"` and continues. GPU is unaffected.
* Bridge channel disconnection (e.g., during GPU reset) is visible as `GMK_CHAN_CLOSED` to subscribers on both sides.

---

## 13. Acceptance Tests

1. **Persistent kernel boot**: microkernel launches and stays resident for 10+ minutes under load.

2. **Queue correctness**: GRQ enq/deq under MPMC stress. Per-worker LQ operates correctly under channel-driven enqueue + worker-driven dequeue.

3. **Gather-dispatch**: mixed task types in a single batch are sorted by type and dispatched without warp divergence. Scalar, warp, and block handlers all execute correctly in the same worker.

4. **Allocator correctness**: slab and block allocators survive contention; no leaks in long runs. Stats match observed allocations.

5. **Module dispatch**: register 10+ handlers; dispatch mixed task types at high rate. Handlers receive valid `gmk_ctx_t` with all pointers non-null. Compile-time switch dispatch matches expected handler for every type.

6. **Channel correctness**:
   * P2P: emit 1M tasks, all delivered in order, no loss.
   * Fan-out: 3 subscribers, all receive all messages.
   * Backpressure: fill a channel, verify `GMK_CHAN_FULL`, drain, verify flow resumes.
   * Type mismatch: boot-time error on mismatched producer/consumer types.
   * Ref-counting: fan-out payload freed exactly once, after all subscribers release. No leaks under partial delivery (drop-slow).
   * Dead-letter: dropped and undeliverable messages appear on `"sys.dropped"`.

7. **Determinism mode**: same seed + same input task stream → identical outputs and identical trace hashes.

8. **Cooperative yielding**: multi-phase handler yields 3 times, produces correct final result. Yield under full LQ uses reserved watermark. Yield overflow bucket drains correctly under stress.

9. **Observability**: host drains traces and metrics without stalling the microkernel. Per-module trace levels filter correctly. Sampling produces statistically accurate counts. `--strip-trace` build produces zero trace overhead.

10. **Fault handling**: inject handler failures; retry/backoff works; rate-based poison detection triggers at sustained failure rate but not on transient spikes; watchdog resets on deadlock.

11. **Type-bucketed GRQ**: workers pulling from a single type-bucket receive only homogeneous batches. Aging scan prevents starvation of cold buckets under hot-bucket flood. No pathological fragmentation under mixed workloads.

12. **Yield circuit breaker**: handler exceeding `max_yields` is routed to `"sys.error"`. `yield_count` is tracked in task header, not in `meta0`/`meta1`.

13. **Lossless channels**: emit 1M messages on a lossless fan-out channel with 3 subscribers. All subscribers receive all messages in sequence. Replay buffer evicts correctly when all subscribers advance past a sequence number.

14. **Payload retain/release**: handler that retains payload, stores pointer in KV, and returns — payload survives auto-release. Handler that does not retain — payload is freed after return. Debug-mode canary detects use-after-free.

15. **Cross-kernel channels**: GPU module emits on bridge channel, CPU module receives via DMA. Verify delivery, ordering, and backpressure across the PCIe boundary. Lossy and lossless guarantees hold for bridge channels.

16. **Dual-kernel boot**: GMK/cpu boots, discovers GPU, launches GMK/gpu via `gpu_drv`. Channel wiring validated across kernels. Type mismatches between GPU producer and CPU consumer caught at boot.

17. **GPU fault recovery**: kill GMK/gpu (simulated hang). `watchdog` module on GMK/cpu detects, resets GPU via `gpu_drv`, reboots GMK/gpu. GMK/cpu remains running throughout. Bridge channels reconnect.

18. **Bridge payload marshaling**: GPU module emits task with 4KB payload on bridge channel. CPU module receives task with valid host-memory `payload_ptr` and correct data. Reverse direction: CPU module emits with payload, GPU module receives valid HBM `payload_ptr`. Inline payloads (`meta0`/`meta1` only) cross without copy.

19. **Bridge DMA batching**: emit 100K tasks/sec on a bridge channel. Verify `gpu_drv` batches DMA transfers (observe fewer DMA ops than tasks). Verify end-to-end throughput scales with batch size.

20. **Cross-kernel determinism**: run deterministic simulation with CPU-produced tick events crossing bridge. Verify tick fences synchronize correctly — GPU does not advance tick until CPU ack received. Identical trace hashes across runs.

---

## 14. Known v0.1 Scaling Limits

These are deliberate simplifications. They are acceptable for v0.1 workloads and are addressed in the v0.2/v0.3 roadmap.

* **KV contention**: the global hash map uses per-bucket spinlocks. Under extreme parallelism (80+ SMs) with hot keys or hash skew, bucket contention on L2 cache lines becomes the dominant bottleneck. v0.2 introduces key-space partitioning across independent sub-stores.
* **GRQ L2 atomic pressure**: type-bucketed GRQ distributes contention across N buckets, but at very high SM counts, atomic operations on bucket head/tail pointers still compete for L2 crossbar bandwidth. v0.2 introduces GPC-local sub-queues.
* **Shared memory ceiling**: persistent blocks allocate shared memory for the worst-case handler. One heavy handler can cap blocks/SM for the entire system. Dedicated workers mitigate this but introduce static partitioning. v0.2 introduces dynamic worker flavors.
* **Watchdog granularity**: per-block heartbeats with configurable thresholds. False positives possible under heavy HBM contention. v0.2 introduces adaptive thresholds via exponential moving average.
* **Single GPU only**: v0.1 supports exactly one GMK/gpu and one GMK/cpu. Multi-GPU requires bridge channel multiplexing and cross-GPU task routing (v0.3).
* **Bridge channel latency**: GPU-local channels operate at ~10ns (HBM atomics). Bridge channels operate at ~2-10μs (PCIe DMA round-trip) — a 1000x difference. DMA batching amortizes this for throughput, but latency-sensitive request-reply patterns across the bridge will feel slow. Design cross-kernel protocols to be asynchronous (fire-and-forget or batched-response), not synchronous RPC. v0.2 explores GPUDirect RDMA to reduce bridge latency for NIC/NVMe traffic.
* **Bridge payload copy cost**: every non-inline payload crossing the bridge incurs a DMA copy. A bridge channel carrying 10K tasks/sec with 4KB payloads generates ~40MB/sec of PCIe traffic per direction. PCIe Gen4 x16 has ~25GB/sec bandwidth, so this is fine. But a careless design (e.g., 1MB payloads at 10K/sec = 10GB/sec) will saturate the bus. Prefer inline payloads (`meta0`/`meta1`) for cross-kernel control messages.
* **CPU kernel simplicity**: GMK/cpu uses a simple run queue, not the type-bucketed GRQ. Sufficient for I/O-bound CPU modules in v0.1. CPU-heavy workloads (e.g., complex network protocol processing) may need a more sophisticated CPU scheduler in v0.2.
* **Cross-kernel determinism cost**: tick fence synchronization adds ~5μs per tick. At 1000 ticks/sec this is 5ms/sec (0.5%) overhead. At 10,000 ticks/sec it becomes 50ms/sec (5%) — potentially significant. v0.2 may batch tick fences or use GPUDirect doorbells to reduce sync cost.

---

## 15. Roadmap

### v0.2

* Dynamic module loading (GPU: host loads additional cubins/ptx; CPU: dlopen)
* Function pointer dispatch table on GPU (replaces compile-time switch for dynamic modules)
* Work-stealing between GPU worker LQs (contingent on v0.1 profiling)
* Hierarchical GPC-local sub-queues to reduce L2 atomic contention at 80+ SM scale
* KV key-space partitioning across independent sub-stores
* Dynamic worker flavors (rebalance dedicated workers at runtime)
* Adaptive watchdog thresholds (exponential moving average per handler type)
* Dynamic channel creation (runtime, not just boot-time)
* Channel filters (predicate functions that select which messages a subscriber receives)
* Multi-queue QoS policies (tenant budgets)
* GPUDirect paths (NVMe/NIC) — `gpu_drv` sets up RDMA mappings, GPU modules bypass CPU for I/O
* Time wheel for EVQ
* **Path 3 bare-metal option**: replace unikernel + `libcuda.so` with bare-metal `gpu_drv` using open-source kernel module as reference. Same GMK/cpu interface, different driver implementation.

### v0.3

* Multi-GPU federation: multiple GMK/gpu instances, each with bridge channels to GMK/cpu
* Cross-GPU channels (routed via GMK/cpu or GPUDirect P2P where available)
* Multi-node: GMK/cpu instances on separate hosts, connected via `nic` module channels
* Stronger isolation (capabilities per arena/queue/channel)
* Snapshot + restore for deterministic replay at scale
* Kernel migration: move a module from GPU to CPU or vice versa (channel rewiring, no code change)
