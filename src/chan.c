/*
 * GMK/cpu — Channel open/emit/sub/close/drain
 *
 * Thread safety: per-channel mutex protects subs[]/n_subs mutations.
 * The `open` field is _Atomic(bool) for lock-free checks on the fast path.
 * Subscriber list is snapshotted under lock then used lock-free.
 *
 * Fan-out payloads: tasks with GMK_TF_PAYLOAD_RC get refcount retained
 * once per subscriber copy. Workers release after handler completion.
 */
#include "gmk/chan.h"
#include "gmk/alloc.h"
#include "gmk/trace.h"
#include "gmk/metrics.h"
#include <string.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static inline bool chan_is_open(gmk_chan_entry_t *ch) {
    return gmk_atomic_load(&ch->open, memory_order_acquire);
}

static void init_chan_lock(gmk_chan_entry_t *ch) {
    pthread_mutex_init(&ch->lock, NULL);
}

static void destroy_chan_lock(gmk_chan_entry_t *ch) {
    pthread_mutex_destroy(&ch->lock);
}

/* ── Registry init / destroy ────────────────────────────────────── */

int gmk_chan_reg_init(gmk_chan_reg_t *cr, gmk_sched_t *sched,
                      gmk_alloc_t *alloc, gmk_trace_t *trace,
                      gmk_metrics_t *metrics) {
    if (!cr || !sched) return -1;

    memset(cr, 0, sizeof(*cr));
    cr->sched   = sched;
    cr->alloc   = alloc;
    cr->trace   = trace;
    cr->metrics = metrics;

    /* Reserve channel 0 = direct submit (not used as a real channel) */
    strncpy(cr->channels[0].name, "sys.direct", GMK_MAX_CHAN_NAME - 1);
    cr->channels[0].id = 0;
    atomic_init(&cr->channels[0].open, false);
    init_chan_lock(&cr->channels[0]);

    /* Reserve channel 1 = "sys.dropped" (dead-letter) */
    cr->channels[1].id        = 1;
    cr->channels[1].mode      = GMK_CHAN_FANOUT;
    cr->channels[1].guarantee = GMK_CHAN_LOSSY;
    atomic_init(&cr->channels[1].open, true);
    strncpy(cr->channels[1].name, "sys.dropped", GMK_MAX_CHAN_NAME - 1);
    atomic_init(&cr->channels[1].emit_count, 0);
    atomic_init(&cr->channels[1].drop_count, 0);
    init_chan_lock(&cr->channels[1]);
    if (gmk_ring_mpmc_init(&cr->channels[1].ring, GMK_CHAN_DEFAULT_SLOTS,
                           sizeof(gmk_task_t)) != 0) {
        return -1;
    }
    cr->channels[1].ring_cap = GMK_CHAN_DEFAULT_SLOTS;

    cr->n_channels = 2;
    return 0;
}

void gmk_chan_reg_destroy(gmk_chan_reg_t *cr) {
    if (!cr) return;
    for (uint32_t i = 0; i < cr->n_channels; i++) {
        if (chan_is_open(&cr->channels[i]) || cr->channels[i].ring_cap > 0)
            gmk_ring_mpmc_destroy(&cr->channels[i].ring);
        destroy_chan_lock(&cr->channels[i]);
    }
}

/* ── Open ───────────────────────────────────────────────────────── */

int gmk_chan_open(gmk_chan_reg_t *cr, const char *name, uint32_t mode,
                  uint32_t guarantee, uint32_t msg_type, uint32_t slots) {
    if (!cr || !name) return GMK_FAIL(GMK_ERR_INVALID);
    if (cr->n_channels >= GMK_MAX_CHANNELS) return GMK_FAIL(GMK_ERR_FULL);

    /* Check for duplicate name */
    for (uint32_t i = 0; i < cr->n_channels; i++) {
        if (chan_is_open(&cr->channels[i]) &&
            strncmp(cr->channels[i].name, name, GMK_MAX_CHAN_NAME) == 0) {
            return GMK_FAIL(GMK_ERR_EXISTS);
        }
    }

    /* Ensure slots is power of two */
    if (slots == 0) slots = GMK_CHAN_DEFAULT_SLOTS;
    if (!gmk_is_power_of_two(slots))
        slots = gmk_next_pow2(slots);

    uint32_t id = cr->n_channels;
    gmk_chan_entry_t *ch = &cr->channels[id];

    strncpy(ch->name, name, GMK_MAX_CHAN_NAME - 1);
    ch->name[GMK_MAX_CHAN_NAME - 1] = '\0';
    ch->id        = id;
    ch->mode      = mode;
    ch->guarantee = guarantee;
    ch->msg_type  = msg_type;
    ch->n_subs    = 0;
    ch->ring_cap  = slots;
    atomic_init(&ch->open, true);
    atomic_init(&ch->emit_count, 0);
    atomic_init(&ch->drop_count, 0);
    init_chan_lock(ch);

    if (gmk_ring_mpmc_init(&ch->ring, slots, sizeof(gmk_task_t)) != 0)
        return GMK_FAIL(GMK_ERR_NOMEM);

    cr->n_channels++;

    if (cr->trace)
        gmk_trace_write(cr->trace, 0, GMK_EV_CHAN_OPEN, 0, id, mode);

    return (int)id;
}

/* ── Dead letter ────────────────────────────────────────────────── */

static void route_to_dead_letter(gmk_chan_reg_t *cr, gmk_task_t *task) {
    if (chan_is_open(&cr->channels[GMK_CHAN_SYS_DROPPED])) {
        gmk_ring_mpmc_push(&cr->channels[GMK_CHAN_SYS_DROPPED].ring, task);
    }
}

/* ── Emit ───────────────────────────────────────────────────────── */

int gmk_chan_emit(gmk_chan_reg_t *cr, uint32_t chan_id, gmk_task_t *task) {
    if (!cr || !task || chan_id >= cr->n_channels)
        return GMK_FAIL(GMK_ERR_INVALID);

    gmk_chan_entry_t *ch = &cr->channels[chan_id];
    if (!chan_is_open(ch)) return GMK_CHAN_CLOSED;

    /* Set channel source */
    task->channel = chan_id;
    task->flags |= GMK_TF_CHANNEL_MSG;

    /* Priority-aware backpressure: last 10% reserved for P0 */
    uint32_t count = gmk_ring_mpmc_count(&ch->ring);
    uint32_t reserve_threshold = ch->ring_cap -
                                 (ch->ring_cap * GMK_CHAN_PRIORITY_RESERVE_PCT / 100);
    if (count >= reserve_threshold && GMK_PRIORITY(task->flags) != GMK_PRIO_CRITICAL) {
        if (cr->trace)
            gmk_trace_write(cr->trace, task->tenant, GMK_EV_CHAN_FULL,
                           task->type, chan_id, 0);
        if (cr->metrics)
            gmk_metric_inc(cr->metrics, task->tenant, GMK_METRIC_CHAN_FULL_COUNT, 1);
        return GMK_CHAN_FULL;
    }

    /* P2P fast-path: snapshot subscriber under lock, enqueue outside lock */
    if (ch->mode == GMK_CHAN_P2P) {
        bool has_sub = false;
        int  target_worker = -1;

        pthread_mutex_lock(&ch->lock);
        if (ch->n_subs == 1 && ch->subs[0].active) {
            has_sub = true;
            target_worker = ch->subs[0].worker_id;
        }
        pthread_mutex_unlock(&ch->lock);

        if (has_sub) {
            if (_gmk_enqueue(cr->sched, task, target_worker) == 0) {
                gmk_atomic_add(&ch->emit_count, 1, memory_order_relaxed);
                if (cr->metrics)
                    gmk_metric_inc(cr->metrics, task->tenant,
                                  GMK_METRIC_CHAN_EMITS, 1);
                return GMK_OK;
            }
        }
    }

    /* Buffer in ring */
    if (gmk_ring_mpmc_push(&ch->ring, task) != 0) {
        if (cr->trace)
            gmk_trace_write(cr->trace, task->tenant, GMK_EV_CHAN_FULL,
                           task->type, chan_id, 0);
        if (cr->metrics)
            gmk_metric_inc(cr->metrics, task->tenant, GMK_METRIC_CHAN_FULL_COUNT, 1);
        return GMK_CHAN_FULL;
    }

    gmk_atomic_add(&ch->emit_count, 1, memory_order_relaxed);
    if (cr->metrics)
        gmk_metric_inc(cr->metrics, task->tenant, GMK_METRIC_CHAN_EMITS, 1);

    /* For P2P with subscriber, drain immediately */
    if (ch->mode == GMK_CHAN_P2P) {
        bool has_sub = false;
        pthread_mutex_lock(&ch->lock);
        has_sub = (ch->n_subs > 0);
        pthread_mutex_unlock(&ch->lock);
        if (has_sub)
            gmk_chan_drain(cr, chan_id, 1);
    }

    return GMK_OK;
}

/* ── Subscribe ──────────────────────────────────────────────────── */

int gmk_chan_sub(gmk_chan_reg_t *cr, uint32_t chan_id, uint32_t module_id,
                int worker_id) {
    if (!cr || chan_id >= cr->n_channels)
        return GMK_FAIL(GMK_ERR_INVALID);

    gmk_chan_entry_t *ch = &cr->channels[chan_id];
    if (!chan_is_open(ch)) return GMK_CHAN_CLOSED;

    pthread_mutex_lock(&ch->lock);

    /* P2P: only one subscriber allowed */
    if (ch->mode == GMK_CHAN_P2P && ch->n_subs >= 1) {
        pthread_mutex_unlock(&ch->lock);
        return GMK_CHAN_ALREADY_BOUND;
    }

    if (ch->n_subs >= GMK_MAX_CHAN_SUBS) {
        pthread_mutex_unlock(&ch->lock);
        return GMK_FAIL(GMK_ERR_FULL);
    }

    gmk_chan_sub_t *sub = &ch->subs[ch->n_subs++];
    sub->module_id = module_id;
    sub->worker_id = worker_id;
    sub->active    = true;

    pthread_mutex_unlock(&ch->lock);
    return GMK_OK;
}

/* ── Close ──────────────────────────────────────────────────────── */

int gmk_chan_close(gmk_chan_reg_t *cr, uint32_t chan_id) {
    if (!cr || chan_id >= cr->n_channels)
        return GMK_FAIL(GMK_ERR_INVALID);

    /* Don't close system channels */
    if (chan_id <= GMK_CHAN_SYS_DROPPED)
        return GMK_FAIL(GMK_ERR_INVALID);

    gmk_chan_entry_t *ch = &cr->channels[chan_id];
    gmk_atomic_store(&ch->open, false, memory_order_release);

    if (cr->trace)
        gmk_trace_write(cr->trace, 0, GMK_EV_CHAN_CLOSE, 0, chan_id, 0);

    return GMK_OK;
}

/* ── Drain ──────────────────────────────────────────────────────── */

int gmk_chan_drain(gmk_chan_reg_t *cr, uint32_t chan_id, uint32_t limit) {
    if (!cr || chan_id >= cr->n_channels) return 0;

    gmk_chan_entry_t *ch = &cr->channels[chan_id];

    /* Snapshot subscriber list under lock */
    pthread_mutex_lock(&ch->lock);
    uint32_t n_subs = ch->n_subs;
    gmk_chan_sub_t subs_snap[GMK_MAX_CHAN_SUBS];
    if (n_subs > 0)
        memcpy(subs_snap, ch->subs, n_subs * sizeof(gmk_chan_sub_t));
    pthread_mutex_unlock(&ch->lock);

    if (n_subs == 0) return 0;
    if (limit == 0) limit = UINT32_MAX;

    uint32_t drained = 0;
    gmk_task_t task;

    while (drained < limit && gmk_ring_mpmc_pop(&ch->ring, &task) == 0) {
        if (ch->mode == GMK_CHAN_P2P) {
            /* P2P: route to the single subscriber */
            if (subs_snap[0].active) {
                if (_gmk_enqueue(cr->sched, &task, subs_snap[0].worker_id) != 0) {
                    route_to_dead_letter(cr, &task);
                    gmk_atomic_add(&ch->drop_count, 1, memory_order_relaxed);
                }
            }
        } else {
            /* Fan-out: copy task header to each subscriber.
             * If the task has a refcounted payload, retain for each
             * additional subscriber so the payload lives until all
             * handlers complete. */
            bool has_rc = (task.flags & GMK_TF_PAYLOAD_RC) && task.payload_ptr;

            /* Count active subscribers */
            uint32_t n_active = 0;
            for (uint32_t i = 0; i < n_subs; i++) {
                if (subs_snap[i].active) n_active++;
            }

            /* Retain payload (n_active - 1) times for the extra copies.
             * The original refcount (1) covers the first subscriber. */
            if (has_rc && n_active > 1) {
                for (uint32_t r = 0; r < n_active - 1; r++)
                    gmk_payload_retain((void *)(uintptr_t)task.payload_ptr);
            }

            for (uint32_t i = 0; i < n_subs; i++) {
                if (!subs_snap[i].active) continue;

                gmk_task_t copy = task;
                int rc = _gmk_enqueue(cr->sched, &copy, subs_snap[i].worker_id);
                if (rc != 0) {
                    if (ch->guarantee == GMK_CHAN_LOSSY) {
                        gmk_atomic_add(&ch->drop_count, 1, memory_order_relaxed);
                        if (cr->trace)
                            gmk_trace_write(cr->trace, task.tenant,
                                           GMK_EV_CHAN_DROP, task.type,
                                           chan_id, i);
                        if (cr->metrics)
                            gmk_metric_inc(cr->metrics, task.tenant,
                                          GMK_METRIC_CHAN_DROPS, 1);
                    } else {
                        /* Lossless: route to dead letter */
                        route_to_dead_letter(cr, &copy);
                    }
                    /* Release the ref for this failed/dead-lettered copy */
                    if (has_rc)
                        gmk_payload_release(cr->alloc, (void *)(uintptr_t)copy.payload_ptr);
                }
            }
        }
        drained++;
    }

    if (drained > 0 && cr->trace)
        gmk_trace_write(cr->trace, 0, GMK_EV_CHAN_DRAIN, 0, chan_id, drained);

    return (int)drained;
}

/* ── Find ───────────────────────────────────────────────────────── */

int gmk_chan_find(const gmk_chan_reg_t *cr, const char *name) {
    if (!cr || !name) return -1;
    for (uint32_t i = 0; i < cr->n_channels; i++) {
        if (gmk_atomic_load(&cr->channels[i].open, memory_order_acquire) &&
            strncmp(cr->channels[i].name, name, GMK_MAX_CHAN_NAME) == 0)
            return (int)i;
    }
    return -1;
}
