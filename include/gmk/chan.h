/*
 * GMK/cpu — Channel registry + P2P/fan-out
 *
 * Up to 256 channels. ID 0 = direct submit, ID 1 = "sys.dropped".
 * Per channel: MPMC ring buffer, subscriber list, mode, guarantee.
 * P2P fast-path, fan-out with shared payload.
 * Priority-aware backpressure.
 */
#ifndef GMK_CHAN_H
#define GMK_CHAN_H

#include "types.h"
#include "ring_mpmc.h"
#include "sched.h"
#include <pthread.h>

/* ── Channel subscriber ──────────────────────────────────────── */
typedef struct {
    uint32_t module_id;     /* subscribing module              */
    int      worker_id;     /* target worker (-1 = any)        */
    bool     active;
} gmk_chan_sub_t;

/* ── Channel entry ───────────────────────────────────────────── */
typedef struct {
    char              name[GMK_MAX_CHAN_NAME];
    uint32_t          id;
    uint32_t          mode;       /* GMK_CHAN_P2P | GMK_CHAN_FANOUT */
    uint32_t          guarantee;  /* GMK_CHAN_LOSSY | GMK_CHAN_LOSSLESS */
    uint32_t          msg_type;   /* expected task type */
    gmk_ring_mpmc_t   ring;       /* backing ring buffer */
    uint32_t          ring_cap;
    gmk_chan_sub_t     subs[GMK_MAX_CHAN_SUBS];
    uint32_t           n_subs;
    _Atomic(bool)      open;      /* atomic: checked without lock on emit fast-path */
    _Atomic(uint64_t)  emit_count;
    _Atomic(uint64_t)  drop_count;
    pthread_mutex_t    lock;      /* protects subs[], n_subs, open mutations */
} gmk_chan_entry_t;

/* ── Channel registry ────────────────────────────────────────── */
struct gmk_chan_reg {
    gmk_chan_entry_t  channels[GMK_MAX_CHANNELS];
    uint32_t          n_channels;
    gmk_sched_t      *sched;    /* for routing tasks to scheduler */
    gmk_alloc_t      *alloc;    /* for payload refcount release   */
    gmk_trace_t      *trace;    /* for trace events */
    gmk_metrics_t    *metrics;  /* for metric updates */
};

int  gmk_chan_reg_init(gmk_chan_reg_t *cr, gmk_sched_t *sched,
                       gmk_alloc_t *alloc, gmk_trace_t *trace,
                       gmk_metrics_t *metrics);
void gmk_chan_reg_destroy(gmk_chan_reg_t *cr);

/* Open a channel. Returns channel ID or negative error. */
int  gmk_chan_open(gmk_chan_reg_t *cr, const char *name, uint32_t mode,
                   uint32_t guarantee, uint32_t msg_type, uint32_t slots);

/* Emit a task on a channel. */
int  gmk_chan_emit(gmk_chan_reg_t *cr, uint32_t chan_id, gmk_task_t *task);

/* Subscribe to a channel. module_id = subscribing module, worker_id = -1 for any. */
int  gmk_chan_sub(gmk_chan_reg_t *cr, uint32_t chan_id, uint32_t module_id,
                  int worker_id);

/* Close a channel. */
int  gmk_chan_close(gmk_chan_reg_t *cr, uint32_t chan_id);

/* Drain a channel: move buffered tasks to subscribers' queues.
   Returns number of tasks drained. */
int  gmk_chan_drain(gmk_chan_reg_t *cr, uint32_t chan_id, uint32_t limit);

/* Find channel by name. Returns channel ID or -1. */
int  gmk_chan_find(const gmk_chan_reg_t *cr, const char *name);

#endif /* GMK_CHAN_H */
