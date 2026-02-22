/*
 * GMK/cpu — Error codes, flags, constants, limits
 */
#ifndef GMK_ERROR_H
#define GMK_ERROR_H

#include <stdint.h>

/* ── Return codes ────────────────────────────────────────────── */
#define GMK_OK              0
#define GMK_RETRY           1
#define GMK_FAIL(code)      (-(int)(code))
#define GMK_ERR_CODE(rc)    ((uint32_t)(-(rc)))

#define GMK_ERR_NOMEM       1
#define GMK_ERR_FULL        2
#define GMK_ERR_EMPTY       3
#define GMK_ERR_INVALID     4
#define GMK_ERR_NOT_FOUND   5
#define GMK_ERR_EXISTS      6
#define GMK_ERR_CLOSED      7
#define GMK_ERR_POISONED    8
#define GMK_ERR_YIELD_OVERFLOW  9
#define GMK_ERR_YIELD_LIMIT    10
#define GMK_ERR_TYPE_MISMATCH  11
#define GMK_ERR_ALREADY_BOUND  12

/* ── Channel return codes ────────────────────────────────────── */
#define GMK_CHAN_FULL           GMK_FAIL(GMK_ERR_FULL)
#define GMK_CHAN_CLOSED         GMK_FAIL(GMK_ERR_CLOSED)
#define GMK_CHAN_TYPE_MISMATCH  GMK_FAIL(GMK_ERR_TYPE_MISMATCH)
#define GMK_CHAN_ALREADY_BOUND  GMK_FAIL(GMK_ERR_ALREADY_BOUND)

/* ── Task flags (bits in gmk_task_t.flags) ───────────────────── */
#define GMK_TF_PRIORITY_MASK   0x0003  /* bits 0-1: priority 0..3 */
#define GMK_TF_PRIORITY_SHIFT  0
#define GMK_TF_DETERMINISTIC   0x0004  /* bit 2 */
#define GMK_TF_IDEMPOTENT      0x0008  /* bit 3 */
#define GMK_TF_EMIT_TRACE      0x0010  /* bit 4 */
#define GMK_TF_CHANNEL_MSG     0x0020  /* bit 5: delivered via channel */
#define GMK_TF_PAYLOAD_RC      0x0040  /* bit 6: payload has refcount header */

#define GMK_PRIORITY(flags)    ((flags) & GMK_TF_PRIORITY_MASK)
#define GMK_SET_PRIORITY(f, p) (((f) & ~GMK_TF_PRIORITY_MASK) | ((p) & 0x3))

/* ── Priority levels ─────────────────────────────────────────── */
#define GMK_PRIORITY_COUNT     4
#define GMK_PRIO_CRITICAL      0
#define GMK_PRIO_HIGH          1
#define GMK_PRIO_NORMAL        2
#define GMK_PRIO_LOW           3

/* ── Handler flags ───────────────────────────────────────────── */
#define GMK_HF_SCALAR          0x0000
#define GMK_HF_WARP            0x0001
#define GMK_HF_BLOCK           0x0002
#define GMK_HF_DETERMINISTIC   0x0004
#define GMK_HF_NEEDS_SHARED    0x0008

/* ── Channel modes ───────────────────────────────────────────── */
#define GMK_CHAN_P2P            0x0001
#define GMK_CHAN_FANOUT         0x0002

/* ── Channel delivery guarantees ─────────────────────────────── */
#define GMK_CHAN_LOSSY          0x0000
#define GMK_CHAN_LOSSLESS       0x0010

/* ── Channel direction (for module declarations) ─────────────── */
#define GMK_CHAN_PRODUCE        0x0100
#define GMK_CHAN_CONSUME        0x0200

/* ── System limits ───────────────────────────────────────────── */
#define GMK_MAX_CHANNELS       256
#define GMK_MAX_MODULES        64
#define GMK_MAX_HANDLERS       256
#define GMK_MAX_WORKERS        32
#define GMK_MAX_TENANTS        16
#define GMK_MAX_CHAN_SUBS      32
#define GMK_MAX_CHAN_NAME      64

/* ── Queue defaults ──────────────────────────────────────────── */
#define GMK_RQ_DEFAULT_CAP     4096
#define GMK_LQ_DEFAULT_CAP     1024
#define GMK_EVQ_DEFAULT_CAP    (64 * 1024)
#define GMK_CHAN_DEFAULT_SLOTS  1024

/* ── Yield / scheduling ──────────────────────────────────────── */
#define GMK_LQ_YIELD_RESERVE_PCT  25   /* 25% of LQ reserved for yields */
#define GMK_DEFAULT_MAX_YIELDS    16
#define GMK_OVERFLOW_CAP          4096

/* ── Priority pop weights ────────────────────────────────────── */
#define GMK_WEIGHT_P0  8
#define GMK_WEIGHT_P1  4
#define GMK_WEIGHT_P2  2
#define GMK_WEIGHT_P3  1

/* ── EVQ ─────────────────────────────────────────────────────── */
#define GMK_EVQ_DRAIN_LIMIT    256

/* ── Channel backpressure ────────────────────────────────────── */
#define GMK_CHAN_PRIORITY_RESERVE_PCT  10  /* last 10% for P0 only */

/* ── Poison detection ────────────────────────────────────────── */
#define GMK_POISON_THRESHOLD   16  /* simple threshold for v0.1 */

/* ── Trace levels ────────────────────────────────────────────── */
#define GMK_TRACE_OFF          0
#define GMK_TRACE_ERROR        1
#define GMK_TRACE_WARN         2
#define GMK_TRACE_INFO         3
#define GMK_TRACE_ALL          4

/* ── Trace event types ───────────────────────────────────────── */
#define GMK_EV_TASK_START      0x0001
#define GMK_EV_TASK_END        0x0002
#define GMK_EV_TASK_FAIL       0x0003
#define GMK_EV_TASK_RETRY      0x0004
#define GMK_EV_TASK_YIELD      0x0005
#define GMK_EV_ALLOC_FAIL      0x0006
#define GMK_EV_CHAN_EMIT       0x0010
#define GMK_EV_CHAN_FULL       0x0011
#define GMK_EV_CHAN_DROP       0x0012
#define GMK_EV_CHAN_DRAIN      0x0013
#define GMK_EV_CHAN_OPEN       0x0014
#define GMK_EV_CHAN_CLOSE      0x0015
#define GMK_EV_WATCHDOG        0x0020
#define GMK_EV_WORKER_PARK     0x0021
#define GMK_EV_WORKER_WAKE     0x0022
#define GMK_EV_YIELD_OVERFLOW  0x0030
#define GMK_EV_YIELD_LIMIT     0x0031
#define GMK_EV_POISON          0x0032
#define GMK_EV_BOOT            0x0040
#define GMK_EV_HALT            0x0041

/* ── Metric IDs ──────────────────────────────────────────────── */
#define GMK_METRIC_TASKS_ENQUEUED    0
#define GMK_METRIC_TASKS_DEQUEUED    1
#define GMK_METRIC_TASKS_DISPATCHED  2
#define GMK_METRIC_TASKS_FAILED      3
#define GMK_METRIC_TASKS_RETRIED     4
#define GMK_METRIC_TASKS_YIELDED     5
#define GMK_METRIC_ALLOC_BYTES       6
#define GMK_METRIC_ALLOC_FAILS       7
#define GMK_METRIC_CHAN_EMITS        8
#define GMK_METRIC_CHAN_DROPS        9
#define GMK_METRIC_CHAN_FULL_COUNT   10
#define GMK_METRIC_WORKER_PARKS     11
#define GMK_METRIC_WORKER_WAKES     12
#define GMK_METRIC_COUNT             16  /* total metric slots */

/* ── Version macro ───────────────────────────────────────────── */
#define GMK_VERSION(major, minor, patch) \
    (((uint32_t)(major) << 16) | ((uint32_t)(minor) << 8) | (uint32_t)(patch))

/* ── System channel IDs ──────────────────────────────────────── */
#define GMK_CHAN_DIRECT        0   /* channel 0 = direct submit */
#define GMK_CHAN_SYS_DROPPED   1   /* "sys.dropped" */

#endif /* GMK_ERROR_H */
