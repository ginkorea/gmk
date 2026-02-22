# GMK Userland v0.1

The runtime libraries, standard modules, toolchain, and shell for the GMK system of microkernels. Covers both GMK/gpu (compute modules) and GMK/cpu (peripheral and I/O modules).

---

## 0. Design Philosophy

The kernel's six principles apply here. The userland adds four more.

### Modules are small and composable.

A module does one thing. `kv` stores data. `timer` emits ticks. `log` writes logs. Combine them with channels to build systems. The standard modules are 50–200 lines each. If your module is longer, it is probably two modules.

### Channels replace function calls.

In traditional programming, you call a function. In GMK, you emit a task on a channel. This is not indirection for its own sake — it decouples modules, enables fan-out, and makes the system observable. Every inter-module interaction is visible in the trace stream.

### The SDK is honest.

The module SDK does not hide the GPU. It abstracts the kernel API's sharp edges — raw pointers, manual sequence numbers, atomic ordering — but it does not pretend you are writing CPU code. You are writing GPU code. The SDK makes it pleasant, not invisible.

### Ship the coreutils.

A bare kernel is useless. GMK ships with standard modules that solve common problems: key-value storage, logging, timing, synchronization, routing. These are not examples — they are production building blocks.

### Modules don't know where they live.

A module talks to channels. It doesn't know if the channel partner is on the same GPU, on the CPU, or on another machine. The channel abstraction hides transport. This means a module written for GMK/gpu can communicate with a module on GMK/cpu without code changes — only channel wiring.

---

## 1. Overview and Layering

GMK is a system of two peer microkernels connected by bridge channels:

```
GMK/gpu (GPU)                              GMK/cpu (CPU)
┌────────────────────────────────┐        ┌────────────────────────────────┐
│ Your GPU Modules (domain logic)│        │ Your CPU Modules (custom I/O)  │
├────────────────────────────────┤        ├────────────────────────────────┤
│ Standard GPU Modules           │        │ Standard CPU Modules           │
│   kv, echo, reduce, timer, ...│        │   gpu_drv, nic, nvme,          │
│                                │        │   watchdog, cli                │
├────────────────────────────────┤        ├────────────────────────────────┤
│ libgmk Tier 2: Module SDK     │        │ libgmk_cpu Tier 2: Module SDK  │
│   gmk_sdk_chan_send, ...       │        │   gmk_sdk_chan_send, ...       │
├────────────────────────────────┤        ├────────────────────────────────┤
│ libgmk Tier 1: Kernel API     │        │ libgmk_cpu Tier 1: Kernel API  │
│   gmk_emit, gmk_alloc, ...    │        │   gmk_emit, gmk_alloc, ...    │
├────────────────────────────────┤        ├────────────────────────────────┤
│ GMK/gpu Microkernel            │        │ GMK/cpu Microkernel            │
│   Scheduler, Allocator,       │        │   Scheduler, Allocator,        │
│   Channels, Modules           │        │   Channels, Modules            │
└───────────┬───────────────────┘        └───────────┬────────────────────┘
            │                                         │
            └──── Bridge Channels (PCIe DMA) ─────────┘
```

You can use any layer. Tier 1 is for when you need full control. Tier 2 is for when you want to move fast. Standard modules are for when someone already solved your problem. The layers compose — a Tier 2 SDK call eventually becomes Tier 1 kernel calls.

The API surface is the same on both kernels. `gmk_emit`, `gmk_chan_emit`, `gmk_alloc` — same function names, same semantics. GPU modules are CUDA C device code. CPU modules are C or Rust host code. The channel interface is identical.

**GPU module authors write CUDA C device code. CPU module authors write C or Rust.** GMK is not a managed runtime. There is no interpreter, no garbage collector, no Python/JS bridge. Domain users who want higher-level abstractions write them as modules using the SDK, or use the standard modules (kv, timer, reduce, gate, nic, nvme) as building blocks. GPU-native DSLs or restricted bytecode interpreters could be implemented as modules in the future, but they are not part of v0.1.

---

## 2. Runtime Libraries

Two libraries, same API surface:

* **libgmk** — GPU-side runtime. CUDA device code. Linked into the GMK/gpu kernel image.
* **libgmk_cpu** — CPU-side runtime. C/Rust host code. Linked into the GMK/cpu binary.

Both expose the same function signatures (`gmk_emit`, `gmk_alloc`, `gmk_chan_emit`, etc.). A module written against the SDK compiles for either target. The implementations differ — libgmk uses HBM atomics and CUDA intrinsics; libgmk_cpu uses pthreads and host memory — but the interface is identical.

### 2.1 Tier 1: Kernel API

The raw kernel interface. Terse, powerful, no guardrails. Every function takes `gmk_ctx_t*` as its first argument. The signatures below apply to both libgmk (GPU) and libgmk_cpu (CPU). On GPU, these are `__device__` functions. On CPU, these are regular C functions.

#### Tasks

```c
int   gmk_emit(gmk_ctx_t* ctx, gmk_task_t* task);
void  gmk_yield(gmk_ctx_t* ctx);
void  gmk_yield_at(gmk_ctx_t* ctx, uint32_t phase);
```

#### Memory

```c
void* gmk_alloc(gmk_ctx_t* ctx, uint32_t size);
void  gmk_free(gmk_ctx_t* ctx, void* ptr);
void* gmk_bump(gmk_ctx_t* ctx, uint32_t size);
void  gmk_bump_reset(gmk_ctx_t* ctx);
void  gmk_payload_release(gmk_ctx_t* ctx);   // release current task's payload (ref-counted)
```

`gmk_payload_release` decrements the atomic reference counter on the current task's payload. For P2P channels, this frees immediately. For fan-out channels, the payload is freed when all subscribers have released. The SDK calls this automatically after handler return — Tier 1 users must call it explicitly if they want early release.

```c
void  gmk_payload_retain(gmk_ctx_t* ctx);    // increment refcount (keep payload alive)
```

`gmk_payload_retain` increments the ref-count, preventing auto-release from freeing the payload. **You must call this if you store a `payload_ptr` beyond handler scope** — into KV, module-private state, or any persistent data structure. The module is then responsible for calling `gmk_payload_release` when the stored pointer is no longer needed. The test harness (`gmk test`) runs in debug mode with payload poisoning to catch retain/release mismatches.

#### Channels

```c
int   gmk_chan_open(gmk_ctx_t* ctx, const char* name, uint32_t mode, uint32_t slots);
int   gmk_chan_emit(gmk_ctx_t* ctx, uint32_t chan, gmk_task_t* task);
int   gmk_chan_sub(gmk_ctx_t* ctx, uint32_t chan);
int   gmk_chan_close(gmk_ctx_t* ctx, uint32_t chan);
```

#### Observability

```c
void  gmk_trace(gmk_ctx_t* ctx, uint32_t ev, uint32_t a0, uint32_t a1);
void  gmk_metric_inc(gmk_ctx_t* ctx, uint32_t metric_id, uint32_t delta);
```

#### Sync

```c
int   gmk_atomic_add(uint32_t* addr, uint32_t val);
int   gmk_reduce_sum(gmk_ctx_t* ctx, int* partial, int count, int* result);
```

#### Identity

```c
uint32_t gmk_self_tenant(gmk_ctx_t* ctx);
uint32_t gmk_self_worker(gmk_ctx_t* ctx);
uint32_t gmk_tick(gmk_ctx_t* ctx);
```

---

### 2.2 Tier 2: Module SDK

Higher-level, safer. Every function starts with `gmk_sdk_`. These wrap Tier 1 with validation, error handling, and convenience.

#### Channels

```c
// Open with automatic type registration
uint32_t gmk_sdk_chan_open(gmk_ctx_t* ctx, const char* name,
                            uint32_t mode, uint32_t msg_type);

// Emit with backpressure handling (yields if full, retries)
int gmk_sdk_chan_send(gmk_ctx_t* ctx, uint32_t chan,
                      uint32_t type, void* payload, uint32_t len);

// Emit with inline payload (no alloc, uses meta0/meta1)
int gmk_sdk_chan_send_inline(gmk_ctx_t* ctx, uint32_t chan,
                              uint32_t type, uint64_t val0, uint64_t val1);
```

`gmk_sdk_chan_send` allocates a payload buffer, fills a `gmk_task_t`, calls `gmk_chan_emit`, and handles `GMK_CHAN_FULL` by calling `gmk_yield` and retrying. You can always drop to Tier 1 if this behavior isn't what you want.

`gmk_sdk_chan_send_inline` exists because many messages are small — two uint64 values. Allocating a payload buffer for 16 bytes is wasteful. Inline messages use `meta0` and `meta1` directly.

#### Request-Reply

```c
// Send a request and encode reply channel in meta0
int gmk_sdk_request(gmk_ctx_t* ctx, uint32_t chan,
                     gmk_task_t* req, uint32_t reply_chan);

// Send a reply (reads reply channel from the original task's meta0)
int gmk_sdk_reply(gmk_ctx_t* ctx, gmk_task_t* response);
```

#### KV Store

```c
int gmk_sdk_kv_put(gmk_ctx_t* ctx, uint64_t key, void* val, uint32_t len);
int gmk_sdk_kv_get(gmk_ctx_t* ctx, uint64_t key, void* buf, uint32_t* len);
int gmk_sdk_kv_del(gmk_ctx_t* ctx, uint64_t key);
```

#### Logging

```c
void gmk_sdk_log(gmk_ctx_t* ctx, uint32_t level, const char* msg);
void gmk_sdk_logf(gmk_ctx_t* ctx, uint32_t level, const char* fmt, ...);
// Levels: GMK_LOG_DEBUG, GMK_LOG_INFO, GMK_LOG_WARN, GMK_LOG_ERROR
```

`gmk_sdk_logf` supports limited printf-style formatting: integers, pointers, fixed-width strings. No floating-point formatting in v0.1 — format floats on the host side from raw trace data.

#### Typed Payload Access

Since `types.toml` defines payload schemas, the code generator produces strongly-typed accessor macros. These eliminate raw `void*` casting in handler code:

```c
// Generated from types.toml by gmk typegen:
GMK_PAYLOAD_DEF(kv_put_req, { uint64_t key; gmk_bytes_t value; });
GMK_PAYLOAD_DEF(kv_get_resp, { uint64_t key; gmk_bytes_t value; uint32_t found; });

// Usage in a handler:
__device__ int my_handler(gmk_ctx_t* ctx) {
    kv_put_req_t* req = GMK_PAYLOAD(ctx, kv_put_req);
    // req->key and req->value are directly accessible, correctly typed
    ...
}
```

`GMK_PAYLOAD(ctx, type)` casts `ctx->task->payload_ptr` to the generated struct type with a compile-time size check against `ctx->task->payload_len`. This catches payload mismatches at development time rather than as silent memory corruption at runtime.

#### Timers

```c
int gmk_sdk_timer_once(gmk_ctx_t* ctx, uint32_t delay,
                        uint32_t chan, uint32_t type);
int gmk_sdk_timer_repeat(gmk_ctx_t* ctx, uint32_t interval,
                          uint32_t chan, uint32_t type);
int gmk_sdk_timer_cancel(gmk_ctx_t* ctx, uint32_t timer_id);
```

#### Barriers

```c
int gmk_sdk_gate_create(gmk_ctx_t* ctx, uint32_t gate_id,
                          uint32_t n_expected, uint32_t out_chan);
int gmk_sdk_gate_arrive(gmk_ctx_t* ctx, uint32_t gate_id);
```

#### Stats

```c
void gmk_sdk_stats_inc(gmk_ctx_t* ctx, uint32_t metric_id, uint32_t delta);
void gmk_sdk_stats_gauge(gmk_ctx_t* ctx, uint32_t metric_id, uint32_t value);
void gmk_sdk_stats_histogram(gmk_ctx_t* ctx, uint32_t metric_id, uint32_t sample);
```

---

## 3. Channel System

### 3.1 Concepts

A channel is a named, typed, parallel conduit. Modules produce tasks onto channels. Other modules subscribe and receive those tasks as handler invocations. Channels are the pipes of GMK.

Properties:
* **Named**: unique string name, resolved to `uint32_t` ID at registration.
* **Typed**: carries tasks of a specific handler type. Mismatches caught at boot.
* **Parallel**: multiple warps emit concurrently via lock-free ring buffer.
* **Observable**: every operation emits a trace event. Depth is a standard metric.

### 3.2 Naming and Types

**Convention**: `"namespace.verb"` or `"namespace.noun"`.

| Namespace | Examples |
|---|---|
| System | `"sys.tick"`, `"sys.error"`, `"sys.dropped"`, `"sys.trace"` |
| Bridge (GPU↔CPU) | `"host.rx"`, `"host.tx"` (default bridge aliases) |
| GPU modules | `"kv.put"`, `"kv.get"`, `"log.entry"`, `"timer.fire"` |
| CPU modules | `"net.rx"`, `"net.tx"`, `"disk.read"`, `"disk.write"`, `"cli.cmd"` |
| User modules | `"sim.step"`, `"ml.batch"`, `"data.ingest"` |

Channel names are **global across the system**. A channel named `"sim.results"` is the same channel whether the producer is on GMK/gpu and the consumer is on GMK/cpu. The channel registry resolves locality and transport at boot.

**Type registry**: maps type IDs to names and payload schemas. In v0.1 this is a static table defined in `types.toml`:

```toml
[[type]]
id = 10
name = "KV_PUT_REQ"
payload = { key = "u64", value = "bytes" }

[[type]]
id = 11
name = "KV_GET_REQ"
payload = { key = "u64" }

[[type]]
id = 12
name = "KV_GET_RESP"
payload = { key = "u64", value = "bytes", found = "bool" }
```

A code generator produces C type definitions and serialization code from the registry. This keeps `types.toml` as the single source of truth and enables the host CLI to serialize JSON payloads without hand-written parsers.

### 3.3 Modes and Backpressure

**Point-to-Point (P2P)**:
* One consumer. First subscriber wins. Second gets `GMK_CHAN_ALREADY_BOUND`.
* FIFO delivery.
* Backpressure: `gmk_chan_emit` returns `GMK_CHAN_FULL`. SDK tier auto-yields and retries.

**Fan-Out**:
* Multiple consumers (up to `GMK_CHAN_MAX_SUBS`, default 32).
* Task header copied per subscriber; payload shared read-only.
* Backpressure: **drop-slow** — if a subscriber's queue is full, that subscriber's copy is dropped and a `CHAN_DROP` trace event is emitted. Other subscribers still receive the message.

**Delivery Guarantees**:

Each channel declares `GMK_CHAN_LOSSY` (default for fan-out) or `GMK_CHAN_LOSSLESS`:

* **Lossy**: drop-slow semantics. If a subscriber is slow, their copy is dropped and a `CHAN_DROP` trace event is emitted. Other subscribers are unaffected. Use for metrics, logging, sampling — anywhere occasional loss is acceptable.
* **Lossless**: no drops. If any subscriber's queue is full, `gmk_chan_emit` returns `GMK_CHAN_FULL` and the emitter must yield or retry. Messages carry a monotonic channel sequence number so subscribers can detect gaps. Use for correctness-critical broadcast like `"sim.tick"` where a missed message means state divergence.

The trade-off is explicit: lossy channels are fast and non-blocking; lossless channels guarantee delivery but a slow subscriber can stall producers. The channel declaration makes this visible at boot time.

Channel ring buffers also reserve 10% of slots for high-priority tasks (priority 0), preventing low-priority floods from starving critical system messages.

### 3.4 Composition Patterns

#### Pipeline

```
ingest → transform → store

Module A produces on "data.raw"
Module B subscribes to "data.raw", produces on "data.clean"
Module C subscribes to "data.clean", stores to kv
```

#### Broadcast

```
tick → [physics, ai, render]

Timer produces on "sim.tick" (fan-out)
Physics, AI, Render all subscribe to "sim.tick"
Each receives every tick independently
```

#### Scatter-Gather

```
splitter → [workers] → reducer

Splitter produces N tasks on "work.chunk" (fan-out to N workers)
Workers produce results on "work.result" (P2P to reducer)
Reducer aggregates when all N results arrive (via gate)
```

#### Request-Reply

```
requester → service → requester

Requester emits on "kv.get" with reply channel in meta0
KV module processes and emits response on the reply channel
```

#### Error Routing

```
any module → "sys.error" → error handler

Any module emits on "sys.error" when it encounters a failure
Error handler subscribes, logs, alerts, or retries
```

#### Dead-Letter Routing

```
dropped/undeliverable tasks → "sys.dropped" → diagnostic handler

Channel full + drop-slow → copy routed to "sys.dropped"
Type mismatch at runtime → routed to "sys.dropped"
Channel closed mid-flight → routed to "sys.dropped"
```

`"sys.dropped"` is always present and always fan-out. The `log` module subscribes by default. This makes the drop-slow backpressure model safe — you never silently lose work.

#### Cross-Kernel Pipeline

```
NIC (CPU) → GPU processing → storage (CPU)

nic module on GMK/cpu produces on "net.rx"       (bridge → GPU)
ingest module on GMK/gpu subscribes to "net.rx"
ingest produces on "data.clean"                   (local GPU channel)
store module on GMK/gpu produces on "disk.write"  (bridge → CPU)
nvme module on GMK/cpu subscribes to "disk.write"
```

The modules don't know they're crossing kernel boundaries. `nic` produces on `"net.rx"` the same way `timer` produces on `"timer.fire"`. The bridge transport is invisible.

#### GPU↔CPU Offload

```
Heavy compute on GPU, results consumed on CPU:

sim module on GMK/gpu produces on "sim.results"
analytics module on GMK/cpu subscribes to "sim.results"
analytics produces on "net.tx" (to send results over network)
```

Any channel can cross the kernel boundary. There is no special API for cross-kernel communication. The `gpu_drv` module handles DMA batching and payload marshaling transparently.

#### Bridge Performance Guidance

Bridge channels are transparent but not free. GPU-local channels run at HBM speed (~10ns). Bridge channels run at PCIe DMA speed (~2-10μs). The system hides this behind batching, but module authors should be aware:

* **Prefer inline payloads** (`meta0`/`meta1`) for cross-kernel control messages. No DMA copy needed.
* **Design async, not RPC.** Don't emit a request on a bridge channel and spin-wait for a response. Emit the request, yield, and handle the response as a separate task when it arrives.
* **Batch where possible.** One task carrying a batch of 100 records is faster across the bridge than 100 tasks carrying one record each.

These are performance guidelines, not API constraints. The bridge works correctly with any payload size and any pattern. But a module author who understands the latency difference will write faster cross-kernel protocols.

---

## 4. Standard Modules

Each module ships with GMK. These are production building blocks, not examples.

Standard modules are split across the two kernels:

| Kernel | Modules |
|---|---|
| GMK/gpu | `kv`, `echo`, `reduce`, `timer`, `log`, `gate`, `stats` |
| GMK/cpu | `gpu_drv`, `nic`, `nvme`, `watchdog`, `cli` |

---

### GPU Modules

---

### 4.1 `kv` — Key-Value Store

GPU-resident hash map for fast key-value lookups within and across modules.

**Channels**:
* Consumes: `"kv.put"`, `"kv.get"`, `"kv.del"`
* Produces: `"kv.result"` (async get responses)

**SDK API**:
```c
int gmk_sdk_kv_put(gmk_ctx_t* ctx, uint64_t key, void* val, uint32_t len);
int gmk_sdk_kv_get(gmk_ctx_t* ctx, uint64_t key, void* buf, uint32_t* len);
int gmk_sdk_kv_del(gmk_ctx_t* ctx, uint64_t key);
```

**Implementation**: open-addressing hash table in HBM arena. 64-bit keys, variable-length values in a slab allocator. Concurrent reads are lock-free. Writes use per-bucket spinlocks (bounded, 64 spins max). No resize in v0.1 — table size fixed at init.

**Config**: `GMK_KV_BUCKETS` (default 65536), `GMK_KV_MAX_VAL_SIZE` (default 256 bytes).

---

### 4.2 `echo` — Task Router

Receives tasks on one channel and re-emits them to one or more output channels.

**Channels**: configurable input and output(s).

**SDK API**:
```c
int gmk_sdk_echo_route(gmk_ctx_t* ctx, uint32_t in_chan,
                        uint32_t* out_chans, uint32_t n_out);
```

**Use cases**:
* **Tap**: see every `"sim.tick"` in the log without modifying simulation modules.
* **Load distribution**: round-robin to multiple consumers.
* **Protocol bridging**: change task type between channels.

---

### 4.3 `reduce` — Parallel Aggregation

Collects N partial results and produces one aggregated result.

**Channels**: configurable input (partials) and output (aggregated result).

**SDK API**:
```c
int gmk_sdk_reduce_start(gmk_ctx_t* ctx, uint32_t reduce_id,
                          uint32_t n_expected, uint32_t out_chan,
                          gmk_reduce_op_t op);

int gmk_sdk_reduce_submit(gmk_ctx_t* ctx, uint32_t reduce_id,
                           void* partial, uint32_t len);
```

**Operations**: `GMK_REDUCE_SUM`, `GMK_REDUCE_MIN`, `GMK_REDUCE_MAX`, `GMK_REDUCE_CONCAT`, `GMK_REDUCE_CUSTOM` (user function pointer).

**Implementation**: per-reduction state in kv. Atomic counter tracks arrivals. When count reaches `n_expected`, finalization runs and result is emitted. For determinism: partials sorted by `seq` before finalization.

---

### 4.4 `timer` — Scheduled Emission

Emits tasks at specified tick intervals or after delays.

**Channels**: produces on configurable output or `"timer.fire"`.

**SDK API**:
```c
int gmk_sdk_timer_once(gmk_ctx_t* ctx, uint32_t delay,
                        uint32_t chan, uint32_t type);
int gmk_sdk_timer_repeat(gmk_ctx_t* ctx, uint32_t interval,
                          uint32_t chan, uint32_t type);
int gmk_sdk_timer_cancel(gmk_ctx_t* ctx, uint32_t timer_id);
```

**Implementation**: wraps the EVQ. Timers are EVQ entries that emit a task on the specified channel when they fire. Repeating timers re-enqueue themselves.

---

### 4.5 `log` — Structured Logging

Receives log entries and writes them to the trace ring buffer and host TX queue.

**Channels**:
* Consumes: `"log.entry"`, `"sys.dropped"` (by default)
* Produces: `"host.tx"` (bridge channel — delivers log records to GMK/cpu for display)

**SDK API**:
```c
void gmk_sdk_log(gmk_ctx_t* ctx, uint32_t level, const char* msg);
void gmk_sdk_logf(gmk_ctx_t* ctx, uint32_t level, const char* fmt, ...);
```

**Implementation**: log entries are tasks emitted on `"log.entry"`. The handler writes a trace record and emits to `"host.tx"` (a bridge channel). The `cli` module on GMK/cpu receives and renders log records. Level filtering at emit time — the SDK checks configured level before allocating. The `log` module also subscribes to `"sys.dropped"` by default, ensuring dead-letter events are always visible in `gmk log` output.

---

### 4.6 `gate` — Barrier Synchronization

Blocks progress until N tasks arrive, then emits a "gate open" task.

**Channels**: configurable input (arrivals) and output (gate-open signal).

**SDK API**:
```c
int gmk_sdk_gate_create(gmk_ctx_t* ctx, uint32_t gate_id,
                          uint32_t n_expected, uint32_t out_chan);
int gmk_sdk_gate_arrive(gmk_ctx_t* ctx, uint32_t gate_id);
```

**Implementation**: atomic counter per gate. When count reaches `n_expected`, a task is emitted on the output channel. Gate state stored in kv.

**Use case**: "Wait for all 64 physics partitions to finish before advancing the simulation tick." Each partition calls `gmk_sdk_gate_arrive`. When all 64 arrive, the gate emits on `"sim.advance"`.

---

### 4.7 `stats` — Metrics Aggregation

Collects, aggregates, and exposes runtime metrics.

**Channels**:
* Consumes: `"stats.sample"`
* Produces: `"host.tx"` (metric snapshots)

**SDK API**:
```c
void gmk_sdk_stats_inc(gmk_ctx_t* ctx, uint32_t metric_id, uint32_t delta);
void gmk_sdk_stats_gauge(gmk_ctx_t* ctx, uint32_t metric_id, uint32_t value);
void gmk_sdk_stats_histogram(gmk_ctx_t* ctx, uint32_t metric_id, uint32_t sample);
```

**Implementation**: three metric types — counters (monotonic), gauges (point-in-time), histograms (distribution). Stored as atomic counters in a fixed-size HBM array. The stats module periodically snapshots all metrics and emits them on `"host.tx"` (bridge channel). The `cli` module on GMK/cpu receives and aggregates metrics from both kernels.

---

### CPU Modules

---

### 4.8 `gpu_drv` — GPU Driver Interface

Manages the GPU hardware. Wraps `libcuda.so` + `nvidia.ko` (Path 1) or bare-metal GPU initialization (Path 3). This module is a **driver** — `libcuda.so` is a dependency of this module, not of the GMK system.

**Channels**:
* Produces: `"host.rx"` (bridge: delivers host/peripheral data to GPU)
* Consumes: `"host.tx"` (bridge: receives GPU results for peripherals)
* Internal: manages all bridge channel DMA transfers

**Responsibilities**:
* GPU memory allocation (HBM buffers for GMK/gpu state)
* Persistent kernel launch
* Bridge channel DMA: polls GPU-side ring buffers, transfers tasks bidirectionally
* GPU reset and reboot (on request from `watchdog`)

**Implementation**: at boot, `gpu_drv` loads the NVIDIA driver, allocates HBM (including the bridge allocation pool), uploads the GMK/gpu kernel image, and launches the persistent kernel. At runtime, it runs an asynchronous batched DMA loop:

* Polls GPU-side bridge ring buffers, pulling batches of up to `GMK_BRIDGE_BATCH_SIZE` (default 64) tasks per DMA read.
* Marshals payloads transparently: copies non-inline payloads from HBM to host memory (GPU→CPU) or from host memory to HBM (CPU→GPU). Inline payloads (`meta0`/`meta1` only) cross without copy.
* Fires when batch is full or timeout expires (`GMK_BRIDGE_POLL_US`, default 10μs).
* For deterministic channels, sends tick fence acknowledgments at tick boundaries.

---

### 4.9 `nic` — Network I/O

Manages network interface hardware. Provides packet-level I/O to the GMK system.

**Channels**:
* Produces: `"net.rx"` (incoming packets/messages)
* Consumes: `"net.tx"` (outgoing packets/messages)

**Implementation**: in Path 1 (unikernel), wraps kernel bypass I/O (DPDK, io_uring, or AF_XDP). In Path 3 (bare metal), drives the NIC directly via MMIO + DMA. Either way, the channel interface is the same. GPU modules that need network data subscribe to `"net.rx"` — the bridge delivers it transparently.

---

### 4.10 `nvme` — Storage I/O

Manages NVMe storage devices.

**Channels**:
* Produces: `"disk.read.resp"` (completed read results)
* Consumes: `"disk.write"`, `"disk.read"` (storage requests)

**Implementation**: submits NVMe commands via submission/completion queues. In Path 1, wraps Linux NVMe or SPDK. In Path 3, drives NVMe hardware directly. Supports async I/O — read requests are tasks, completions are tasks emitted on the response channel.

---

### 4.11 `watchdog` — Health Monitor

Monitors GMK/gpu health from the CPU side.

**Channels**:
* Consumes: `"sys.heartbeat"` (bridge: GPU worker heartbeats)
* Produces: `"sys.error"` (on timeout/hang detection)

**Implementation**: GPU workers emit periodic heartbeat tasks on `"sys.heartbeat"` (a bridge channel). `watchdog` tracks per-worker heartbeat timestamps. Missed heartbeats trigger escalation: 2 missed = warning trace event, 5 missed = GPU reset request to `gpu_drv`. The `watchdog` module runs entirely on GMK/cpu and is unaffected by GPU hangs.

---

### 4.12 `cli` — Command-Line Interface

The `gmk` shell. Runs on GMK/cpu and interacts with both kernels.

**Channels**:
* Produces: `"cli.cmd"` (user commands routed to appropriate kernel)
* Consumes: `"cli.resp"` (responses from both kernels)
* Subscribes: `"sys.trace"`, `"sys.error"` (for `gmk log` and `gmk trace`)

**Implementation**: see §5 Host Shell for command details. The `cli` module translates user commands into tasks, emits them on the appropriate channels (local for CPU queries, bridge for GPU queries), and renders responses. It aggregates traces and metrics from both kernels for unified display.

---

## 5. Host Shell (`gmk` CLI)

The human face of GMK. Terse, composable, discoverable. The `cli` module runs on GMK/cpu and talks to both kernels through channels.

### 5.1 Commands

```
gmk boot [config]            boot the system (GMK/cpu, then GMK/gpu)
gmk halt                     graceful shutdown of all kernels

gmk ps                       list all kernels, modules, and worker status
gmk top                      live dashboard (both kernels)
gmk log [--level LEVEL]      stream log output (both kernels)
gmk trace [--filter EXPR] [--level LEVEL] [--module NAME]
                             stream raw trace events (both kernels)
gmk stats [metric]           show metrics (both kernels)

gmk emit TYPE [--payload JSON] [--chan NAME]
                             submit a task (routed to correct kernel)

gmk chan ls                  list all channels (local + bridge)
gmk chan inspect NAME        show channel state

gmk mod ls                   list all modules (GPU + CPU)
gmk mod info NAME            show module details
gmk mod reset TYPE           clear poison flag for a handler type

gmk kern ls                  list kernels and status
gmk kern info KERNEL_ID      show kernel details

gmk kv get KEY               read from kv store
gmk kv put KEY VALUE         write to kv store
gmk kv ls                    list keys

gmk new NAME [--target gpu|cpu]
                             scaffold a new module (GPU or CPU)
gmk build                    compile modules
gmk test [NAME]              run module tests
```

No subcommand nesting deeper than two levels. The most common operations are one word: `gmk ps`, `gmk top`, `gmk log`.

### 5.2 Live Monitoring

**`gmk top`** — auto-refreshing dashboard showing both kernels:

```
GMK v0.1 | gpu.0 + cpu.0 | tick 48,291 | uptime 00:14:32
──────────────────────────────────────────────────────────

GPU.0  workers: 58 active / 6 idle / 0 stalled
  QUEUES   grq: 142 (12 buckets active)  lq_avg: 8  evq: 23
  MEMORY   used: 847M / 2048M  alloc/s: 12K  fails: 0

CPU.0  workers: 4 active / 0 idle / 0 stalled
  QUEUES   rq: 8  lq_avg: 2
  MEMORY   used: 128M / 512M

CHANNELS                                            KERNEL
  sim.tick      fanout  depth: 0    emit/s: 1.2K    gpu.0
  kv.put        p2p     depth: 34   emit/s: 8.4K    gpu.0
  log.entry     p2p     depth: 2    emit/s: 3.1K    gpu.0
  net.rx        p2p     depth: 0    emit/s: 4.2K    cpu.0
  host.rx       bridge  depth: 1    emit/s: 4.2K    gpu↔cpu
  host.tx       bridge  depth: 3    emit/s: 8.1K    gpu↔cpu

MODULES                                             KERNEL
  kv       tasks/s: 8.4K   fails: 0    handlers: 3  gpu.0
  timer    tasks/s: 1.2K   fails: 0    handlers: 2  gpu.0
  physics  tasks/s: 24K    fails: 12   handlers: 4  gpu.0
  gpu_drv  tasks/s: 12K    fails: 0    handlers: 2  cpu.0
  nic      tasks/s: 4.2K   fails: 0    handlers: 2  cpu.0
```

Updates every 500ms by aggregating metrics from both kernels. Does not stall the GPU.

**`gmk log`** — live log stream:

```
$ gmk log --level warn
[00:14:33.421] WARN  physics  partition 17: CFL exceeded, reducing dt
[00:14:33.422] WARN  physics  partition 23: CFL exceeded, reducing dt
[00:14:34.001] WARN  kv       bucket 4091 contention: 48 spins
```

**`gmk trace`** — raw trace stream:

```
$ gmk trace --filter "type=TASK_START,module=physics"
[tick 48291] TASK_START  type=PHYS_STEP  worker=12  tenant=0  seq=9481023
[tick 48291] TASK_START  type=PHYS_STEP  worker=14  tenant=0  seq=9481024
```

### 5.3 Channel Inspection

```
$ gmk chan ls
NAME          MODE    DEPTH  EMIT/s  SUBS  PRODUCER     KERNEL
sim.tick      fanout  0      1.2K    4     timer        gpu.0
kv.put        p2p     34     8.4K    1     (multiple)   gpu.0
log.entry     p2p     2      3.1K    1     (multiple)   gpu.0
net.rx        p2p     0      4.2K    1     nic          cpu.0
host.rx       bridge  1      4.2K    1     gpu_drv      gpu↔cpu
host.tx       bridge  3      8.1K    1     (multiple)   gpu↔cpu

$ gmk chan inspect kv.put
Channel: kv.put
  Kernel: gpu.0
  Mode: p2p
  Type: KV_PUT_REQ (id=10)
  Buffer: 4096 slots, 34 used (0.8%)
  Emit rate: 8.4K/s
  Full events: 0 (lifetime)
  Subscriber: kv (handler KV_PUT)
  Producers: physics, ai, data_ingest

$ gmk chan inspect host.rx
Channel: host.rx (bridge)
  Transport: PCIe DMA via gpu_drv
  Producer: gpu_drv (cpu.0) → consumer: (multiple) (gpu.0)
  Mode: p2p
  Buffer: 8192 slots, 1 used (0.01%)
  DMA poll: 10μs interval
  Emit rate: 4.2K/s
```

### 5.4 Module Inspection

```
$ gmk mod ls
NAME      VERSION  HANDLERS  CHANNELS  STATUS   KERNEL
kv        0.1.0    3         3         running  gpu.0
echo      0.1.0    1         2         running  gpu.0
timer     0.1.0    2         1         running  gpu.0
log       0.1.0    1         2         running  gpu.0
physics   0.1.0    4         2         running  gpu.0
gpu_drv   0.1.0    2         2         running  cpu.0
nic       0.1.0    2         2         running  cpu.0
nvme      0.1.0    3         3         running  cpu.0
watchdog  0.1.0    1         2         running  cpu.0
cli       0.1.0    2         3         running  cpu.0

$ gmk mod info kv
Module: kv v0.1.0 (gpu.0)
Handlers:
  KV_PUT  (type=10)  deterministic=no  shared=0B
  KV_GET  (type=11)  deterministic=no  shared=0B
  KV_DEL  (type=12)  deterministic=no  shared=0B
Channels:
  kv.put    consumes  type=KV_PUT_REQ   mode=p2p
  kv.get    consumes  type=KV_GET_REQ   mode=p2p
  kv.result produces  type=KV_GET_RESP  mode=p2p
Stats:
  puts/s: 8.4K   gets/s: 12.1K   bucket_util: 72%

$ gmk kern ls
KERNEL  ROLE        WORKERS  MODULES  STATUS
gpu.0   compute     64       7        running
cpu.0   peripheral  4        5        running
```

### 5.5 Task Submission

```
$ gmk emit SIM_START --payload '{"seed": 42, "steps": 1000}'
task submitted: seq=9481025 type=SIM_START

$ gmk emit SIM_START --chan sim.tick
task submitted: seq=9481026 type=SIM_START channel=sim.tick
```

`gmk emit` supports both direct submission (to GRQ) and channel emission (with `--chan`). JSON payloads are serialized by the `cli` module on GMK/cpu using the type registry.

---

## 6. Module SDK and Toolchain

### 6.1 Scaffold

```
$ gmk new counter
Created module: counter/ (target: gpu)
  counter/counter.cu        module source
  counter/counter.h         public header
  counter/counter_test.cu   test harness
  counter/module.toml       manifest

$ gmk new my_nic_filter --target cpu
Created module: my_nic_filter/ (target: cpu)
  my_nic_filter/my_nic_filter.c        module source
  my_nic_filter/my_nic_filter.h        public header
  my_nic_filter/my_nic_filter_test.c   test harness
  my_nic_filter/module.toml            manifest
```

GPU modules default to `.cu` files. CPU modules default to `.c` files. The `module.toml` declares the target kernel.

**module.toml**:

```toml
[module]
name = "counter"
version = "0.1.0"
target = "gpu"          # "gpu" (default) or "cpu"

[[handler]]
name = "COUNTER_INC"
type = 100
deterministic = false
granularity = "scalar"  # "scalar" (default), "warp", or "block" (GPU only)

[[channel]]
name = "counter.in"
direction = "consume"
msg_type = "COUNTER_INC_REQ"
mode = "p2p"
guarantee = "lossy"

[[channel]]
name = "counter.out"
direction = "produce"
msg_type = "COUNTER_INC_RESP"
mode = "p2p"
guarantee = "lossy"
```

**Generated source** (`counter.cu`):

```c
#include <gmk.h>
#include <gmk_sdk.h>
#include "counter.h"

__device__ int counter_inc(gmk_ctx_t* ctx) {
    // your handler logic here
    // access the task:  ctx->task
    // allocate memory:  gmk_alloc(ctx, size)
    // emit on channel:  gmk_chan_emit(ctx, chan_id, &task)
    // log:              gmk_sdk_log(ctx, GMK_LOG_INFO, "done")
    return GMK_OK;
}

__device__ int counter_init(gmk_ctx_t* ctx) {
    gmk_sdk_log(ctx, GMK_LOG_INFO, "counter initialized");
    return GMK_OK;
}

GMK_MODULE(counter) = {
    .name = "counter",
    .version = GMK_VERSION(0, 1, 0),
    .handlers = (gmk_handler_reg_t[]) {
        { .type = COUNTER_INC, .fn = counter_inc, .name = "counter_inc" },
    },
    .n_handlers = 1,
    .channels = (gmk_chan_decl_t[]) {
        { .name = "counter.in",  .direction = GMK_CHAN_CONSUME,
          .msg_type = COUNTER_INC_REQ },
        { .name = "counter.out", .direction = GMK_CHAN_PRODUCE,
          .msg_type = COUNTER_INC_RESP },
    },
    .n_channels = 2,
    .init = counter_init,
};
```

`GMK_MODULE()` expands to a struct with linker annotations. The kernel scans a linker section for these at boot.

### 6.2 Build

```
$ gmk build
[gpu] nvcc -dc counter/counter.cu -o counter/counter.o -I$GMK_INCLUDE
[gpu] Compiled: counter/counter.o
[cpu] cc -c my_nic_filter/my_nic_filter.c -o my_nic_filter/my_nic_filter.o
[cpu] Compiled: my_nic_filter/my_nic_filter.o

$ gmk build --kernel
[gpu] nvcc gmk_kernel.cu gpu_modules/*.o -o gmk_gpu.cubin
[gpu] Built: gmk_gpu.cubin (12.4 MB, 8 modules, 23 handlers)
[cpu] cc gmk_cpu_main.c cpu_modules/*.o -o gmk_cpu -lgmk_cpu -lpthread
[cpu] Built: gmk_cpu (2.1 MB, 5 modules, 12 handlers)
```

The build system reads `module.toml` for each module and routes to the correct compiler. GPU modules use `nvcc`. CPU modules use the system C compiler (or Rust toolchain). Module authors do not need to manage cross-compilation.

Build flags:

* `--strip-trace` — remove all `gmk_trace()` calls at compile time for zero-overhead production builds. Metrics are never stripped.
* `--inline-reg-limit N` — override the register threshold for `__noinline__` isolation (default 32). Handlers exceeding N registers are compiled as `__noinline__` to prevent one heavy handler from destroying occupancy for all lightweight handlers.

`gmk build --kernel` reports per-handler register counts:

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

If `ptxas` reports any register spilling to local memory (`Lmem > 0`), the build fails by default. Local memory is a private slice of HBM — every spilled access is a ~400-cycle round trip. Fix by simplifying the handler, splitting into yield phases, or adjusting `--inline-reg-limit`.

### 6.3 Test Harness

```
$ gmk test counter
Running tests for counter...
  [PASS] test_inc_basic            (0.3ms, 1 task)
  [PASS] test_inc_zero             (0.2ms, 1 task)
  [PASS] test_inc_yield_resume     (0.8ms, 3 tasks)
  [FAIL] test_inc_overflow         expected GMK_FAIL, got GMK_OK
3/4 passed, 1 failed
```

Tests run on the GPU in a sandboxed environment — single worker block, mock channels, mock allocator:

```c
#include <gmk_test.h>
#include "counter.h"

GMK_TEST(test_inc_basic) {
    gmk_test_ctx_t* t = gmk_test_init();

    gmk_task_t task = gmk_test_task(COUNTER_INC, NULL, 0);
    task.meta0 = 41;

    int rc = gmk_test_run(t, counter_inc, &task);

    gmk_test_assert_eq(rc, GMK_OK);
    gmk_test_assert_emitted(t, "counter.out", 1);

    gmk_test_fini(t);
}
```

`gmk_test_ctx_t` provides a sandboxed `gmk_ctx_t` that captures emitted tasks, trace events, and allocations for assertion.

### 6.4 Type Registry

`types.toml` is the single source of truth. A code generator produces C headers and host-side JSON serializers.

```toml
[[type]]
id = 100
name = "COUNTER_INC_REQ"
payload = { delta = "u32" }
max_yields = 8
deterministic = false
lossy = true

[[type]]
id = 101
name = "COUNTER_INC_RESP"
payload = { result = "u64" }
max_yields = 0           # responses should never yield
deterministic = false
lossy = true
```

### Type Policy Fields

Each type in the registry declares behavioral policies that are validated at boot and enforced at runtime:

| Field | Default | Meaning |
|---|---|---|
| `max_yields` | 16 | Max times a task of this type can yield before circuit-breaker routes it to `"sys.error"` |
| `deterministic` | false | Whether this type participates in deterministic mode. If true, channels carrying this type use synchronous drain at tick boundaries |
| `lossy` | true | Whether channels carrying this type default to `GMK_CHAN_LOSSY`. Set to false for correctness-critical types like simulation ticks |

These policies collapse runtime traps into boot-time and build-time validation. GMK/cpu's boot orchestrator checks that lossless types are only carried on lossless channels, that deterministic types are only handled by `GMK_HF_DETERMINISTIC` handlers, and that `max_yields` is compatible with the handler's expected phase count.

```
$ gmk typegen types.toml -o include/gmk_types.h
Generated: include/gmk_types.h (42 types)
```

---

## 7. Developer Experience

### 7.1 Your First Module

From zero to running in 5 minutes.

**Step 1**: scaffold.
```
$ gmk new counter
```

**Step 2**: write the handler (~15 lines).
```c
__device__ int counter_inc(gmk_ctx_t* ctx) {
    uint64_t val = ctx->task->meta0;
    val++;

    gmk_task_t out;
    out.type = COUNTER_INC_RESP;
    out.meta0 = val;
    out.payload_ptr = 0;
    out.payload_len = 0;

    gmk_chan_emit(ctx, counter_out_chan, &out);
    gmk_sdk_logf(ctx, GMK_LOG_INFO, "counter: %lu -> %lu", val - 1, val);
    return GMK_OK;
}
```

**Step 3**: build.
```
$ gmk build && gmk build --kernel
```

**Step 4**: boot and test.
```
$ gmk boot
GMK/cpu.0  booted |  4 workers |  5 modules
GMK/gpu.0  booted | 64 workers |  9 modules
bridge channels: 4
system ready

$ gmk emit COUNTER_INC --meta0 41
task submitted: seq=1 type=COUNTER_INC kernel=gpu.0

$ gmk log
[00:00:01.203] INFO  counter  counter: 41 -> 42
```

**Step 5**: inspect.
```
$ gmk chan inspect counter.out
Channel: counter.out
  Mode: p2p
  Type: COUNTER_INC_RESP (id=101)
  Buffer: 1024 slots, 1 used
  Emit rate: 1/s
```

That's it. One handler, two channels, a working module.

### 7.2 Building a Pipeline

A three-stage data processor: ingest → transform → store.

**ingest.cu**: subscribes to `"host.rx"`, parses incoming data, emits on `"data.parsed"`.

```c
__device__ int ingest_handler(gmk_ctx_t* ctx) {
    void* raw = (void*)ctx->task->payload_ptr;
    uint32_t len = ctx->task->payload_len;

    // parse raw data into structured record
    record_t* rec = (record_t*)gmk_alloc(ctx, sizeof(record_t));
    parse(raw, len, rec);

    gmk_sdk_chan_send(ctx, data_parsed_chan, DATA_PARSED, rec, sizeof(record_t));
    return GMK_OK;
}
```

**transform.cu**: subscribes to `"data.parsed"`, applies computation, emits on `"data.clean"`.

```c
__device__ int transform_handler(gmk_ctx_t* ctx) {
    record_t* rec = (record_t*)ctx->task->payload_ptr;

    rec->value = normalize(rec->value);
    rec->flags |= FLAG_PROCESSED;

    gmk_sdk_chan_send(ctx, data_clean_chan, DATA_CLEAN, rec, sizeof(record_t));
    return GMK_OK;
}
```

**store.cu**: subscribes to `"data.clean"`, writes to kv.

```c
__device__ int store_handler(gmk_ctx_t* ctx) {
    record_t* rec = (record_t*)ctx->task->payload_ptr;
    gmk_sdk_kv_put(ctx, rec->id, rec, sizeof(record_t));
    return GMK_OK;
}
```

Notice that `ingest`, `transform`, and `store` never reference each other. They communicate only through channels. You can replace `transform` with a different implementation, add a second `transform` in parallel, or tap `"data.parsed"` for debugging — all without modifying the other modules.

Running the pipeline:

```
$ gmk top
CHANNELS                                            KERNEL
  host.rx       bridge  depth: 3    emit/s: 10K     gpu↔cpu
  data.parsed   p2p     depth: 12   emit/s: 9.8K    gpu.0
  data.clean    p2p     depth: 5    emit/s: 9.7K    gpu.0
```

### 7.3 Debugging and Profiling

**"My handler is slow"**: check task throughput and handler duration.
```
$ gmk stats --module physics
  tasks/s: 24K    avg_duration: 4.2us    p99: 12us    yields: 340/s
```

**"Messages are being dropped"**: check channel backpressure.
```
$ gmk chan inspect sim.tick
  Full events: 1,203 (lifetime)
  Drop events: 47
```
Increase buffer slots or optimize the slow subscriber.

**"Traces are too noisy / too expensive"**: control trace cost.
```
$ gmk trace --level INFO --module physics   # only INFO+ from physics
$ gmk trace --sample 0.01                   # 1% sampling, all modules
$ gmk build --kernel --strip-trace          # zero-overhead production build
```

**"The system is stuck"**: check for stalled workers.
```
$ gmk top
GPU.0  workers: 58 active / 0 idle / 6 stalled   <-- problem
CPU.0  workers: 4 active / 0 idle / 0 stalled
```
```
$ gmk trace --filter "ev=WATCHDOG"
[tick 48291] WATCHDOG  kernel=gpu.0  worker=12  missed=3
[tick 48291] WATCHDOG  kernel=gpu.0  worker=17  missed=4
```
A handler is not yielding. Check which task type is running on stalled workers.

---

## 8. Conventions and Style Guide

### Naming

| Layer | Convention | Examples |
|---|---|---|
| Kernel API | `gmk_<verb>` | `gmk_emit`, `gmk_alloc`, `gmk_yield` |
| SDK API | `gmk_sdk_<noun>_<verb>` | `gmk_sdk_kv_put`, `gmk_sdk_chan_send` |
| CLI | `gmk <noun> [verb]` | `gmk ps`, `gmk log`, `gmk chan ls` |
| Channels | `"namespace.verb"` | `"kv.put"`, `"sim.tick"`, `"log.entry"` |
| Types | `UPPER_SNAKE` | `KV_PUT_REQ`, `SIM_STEP`, `COUNTER_INC` |
| Constants | `GMK_` prefix | `GMK_OK`, `GMK_FAIL`, `GMK_CHAN_FULL` |

### Error Codes

```c
GMK_OK              =    0   // success
GMK_RETRY           =    1   // transient failure, re-enqueue
GMK_FAIL            =   -1   // permanent failure, to error queue
GMK_CHAN_FULL        = -100   // channel backpressure
GMK_CHAN_CLOSED      = -101   // channel no longer exists
GMK_CHAN_TYPE_MISMATCH = -102 // wrong task type for channel
GMK_CHAN_ALREADY_BOUND = -103 // P2P channel already has a subscriber
GMK_ALLOC_FAIL       = -200  // out of memory
```

### Module Discipline

* Target 50–200 lines per module. If it's bigger, split it.
* All inter-module communication goes through channels. Never share raw pointers between modules.
* Modules don't know which kernel their channel partner lives on. Never assume locality.
* If you need shared state, use `kv`.
* Every module declares its channels and target kernel in `module.toml`. No hidden dependencies.
* Handlers must yield or complete within budget. No infinite loops.
* CPU modules follow the same conventions as GPU modules: same naming, same channel discipline, same error codes.
