/*
 * GMK/cpu — Core type definitions
 * gmk_task_t, gmk_ctx_t, gmk_module_t, handler/channel declarations
 */
#ifndef GMK_TYPES_H
#define GMK_TYPES_H

#include "platform.h"
#include "error.h"

/* ── Forward declarations ────────────────────────────────────── */
typedef struct gmk_arena     gmk_arena_t;
typedef struct gmk_alloc     gmk_alloc_t;
typedef struct gmk_chan_reg  gmk_chan_reg_t;
typedef struct gmk_trace     gmk_trace_t;
typedef struct gmk_metrics   gmk_metrics_t;
typedef struct gmk_sched     gmk_sched_t;
typedef struct gmk_kernel    gmk_kernel_t;

/* ── Task record (48 bytes, 16-byte aligned) ─────────────────── */
typedef struct GMK_ALIGN(16) {
    uint32_t  type;         /* handler id / message type                */
    uint16_t  flags;        /* priority, determinism, etc.              */
    uint16_t  tenant;       /* logical partition id (0..N-1)            */
    uint32_t  channel;      /* source channel id (0 = direct submit)   */
    uint32_t  seq;          /* monotonic enqueue sequence               */
    uint64_t  payload_ptr;  /* pointer into arena                      */
    uint32_t  payload_len;  /* bytes                                   */
    uint16_t  yield_count;  /* runtime: incremented on each yield      */
    uint16_t  _pad;         /* alignment                               */
    uint64_t  meta0;        /* inline fast arg / continuation state    */
    uint64_t  meta1;        /* inline fast arg                         */
} gmk_task_t;               /* 48 bytes */

_Static_assert(sizeof(gmk_task_t) == 48, "gmk_task_t must be 48 bytes");

/* ── Handler function signature ──────────────────────────────── */
struct gmk_ctx;
typedef int (*gmk_handler_fn)(struct gmk_ctx *ctx);

/* ── Task context (passed to every handler) ──────────────────── */
typedef struct gmk_ctx {
    gmk_task_t     *task;       /* the current task                    */
    gmk_alloc_t    *alloc;      /* allocator for this tenant's arena   */
    gmk_chan_reg_t  *chan;       /* channel registry                    */
    gmk_trace_t    *trace;      /* trace writer                        */
    gmk_metrics_t  *metrics;    /* metrics counters                    */
    gmk_sched_t    *sched;      /* scheduler (for enqueue/yield)       */
    gmk_kernel_t   *kernel;     /* kernel reference                    */
    uint32_t        worker_id;  /* which worker is executing           */
    uint32_t        tick;       /* current logical tick                 */
} gmk_ctx_t;

/* ── Handler registration ────────────────────────────────────── */
typedef struct {
    uint32_t        type;           /* handler id / task type          */
    gmk_handler_fn  fn;             /* handler function                */
    const char     *name;           /* human-readable, e.g., "kv_put" */
    uint32_t        flags;          /* GMK_HF_* flags                  */
    uint32_t        max_yields;     /* yield circuit breaker (0=default) */
} gmk_handler_reg_t;

/* ── Channel declaration (for module registration) ───────────── */
typedef struct {
    const char *name;       /* channel name, e.g., "sim.tick"      */
    uint32_t    direction;  /* GMK_CHAN_PRODUCE | GMK_CHAN_CONSUME  */
    uint32_t    msg_type;   /* expected task type                  */
    uint32_t    mode;       /* GMK_CHAN_P2P | GMK_CHAN_FANOUT      */
    uint32_t    guarantee;  /* GMK_CHAN_LOSSY | GMK_CHAN_LOSSLESS   */
} gmk_chan_decl_t;

/* ── Module definition ───────────────────────────────────────── */
typedef struct {
    const char          *name;       /* e.g., "kv", "echo", "timer"  */
    uint32_t             version;    /* GMK_VERSION(major, minor, patch) */
    gmk_handler_reg_t   *handlers;
    uint32_t             n_handlers;
    gmk_chan_decl_t     *channels;
    uint32_t             n_channels;
    int (*init)(gmk_ctx_t *ctx);    /* called once at boot            */
    int (*fini)(gmk_ctx_t *ctx);    /* called at shutdown             */
} gmk_module_t;

/* ── Trace event record (32 bytes, 16-byte aligned) ──────────── */
typedef struct GMK_ALIGN(16) {
    uint64_t  tsc;      /* monotonic clock / logical tick    */
    uint32_t  ev_type;  /* GMK_EV_* event type               */
    uint16_t  tenant;
    uint16_t  type;     /* task type                         */
    uint32_t  arg0;
    uint32_t  arg1;
    uint64_t  _pad;     /* pad to 32 bytes for ring indexing */
} gmk_trace_ev_t;

_Static_assert(sizeof(gmk_trace_ev_t) == 32, "gmk_trace_ev_t must be 32 bytes");

#endif /* GMK_TYPES_H */
