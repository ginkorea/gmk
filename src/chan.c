/*
 * GMK/cpu — Channel open/emit/sub/close/drain
 */
#include "gmk/chan.h"
#include "gmk/trace.h"
#include "gmk/metrics.h"
#include <string.h>

int gmk_chan_reg_init(gmk_chan_reg_t *cr, gmk_sched_t *sched,
                      gmk_trace_t *trace, gmk_metrics_t *metrics) {
    if (!cr || !sched) return -1;

    memset(cr, 0, sizeof(*cr));
    cr->sched   = sched;
    cr->trace   = trace;
    cr->metrics = metrics;

    /* Reserve channel 0 = direct submit (not used as a real channel) */
    strncpy(cr->channels[0].name, "sys.direct", GMK_MAX_CHAN_NAME - 1);
    cr->channels[0].id   = 0;
    cr->channels[0].open = false; /* marker channel, not usable */

    /* Reserve channel 1 = "sys.dropped" (dead-letter) */
    cr->channels[1].id        = 1;
    cr->channels[1].mode      = GMK_CHAN_FANOUT;
    cr->channels[1].guarantee = GMK_CHAN_LOSSY;
    cr->channels[1].open      = true;
    strncpy(cr->channels[1].name, "sys.dropped", GMK_MAX_CHAN_NAME - 1);
    atomic_init(&cr->channels[1].emit_count, 0);
    atomic_init(&cr->channels[1].drop_count, 0);
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
    for (uint32_t i = 1; i < cr->n_channels; i++) {
        if (cr->channels[i].open || cr->channels[i].ring_cap > 0)
            gmk_ring_mpmc_destroy(&cr->channels[i].ring);
    }
}

int gmk_chan_open(gmk_chan_reg_t *cr, const char *name, uint32_t mode,
                  uint32_t guarantee, uint32_t msg_type, uint32_t slots) {
    if (!cr || !name) return GMK_FAIL(GMK_ERR_INVALID);
    if (cr->n_channels >= GMK_MAX_CHANNELS) return GMK_FAIL(GMK_ERR_FULL);

    /* Check for duplicate name */
    for (uint32_t i = 0; i < cr->n_channels; i++) {
        if (cr->channels[i].open &&
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
    ch->open      = true;
    ch->ring_cap  = slots;
    atomic_init(&ch->emit_count, 0);
    atomic_init(&ch->drop_count, 0);

    if (gmk_ring_mpmc_init(&ch->ring, slots, sizeof(gmk_task_t)) != 0)
        return GMK_FAIL(GMK_ERR_NOMEM);

    cr->n_channels++;

    if (cr->trace)
        gmk_trace_write(cr->trace, 0, GMK_EV_CHAN_OPEN, 0, id, mode);

    return (int)id;
}

static void route_to_dead_letter(gmk_chan_reg_t *cr, gmk_task_t *task) {
    if (cr->channels[GMK_CHAN_SYS_DROPPED].open) {
        gmk_ring_mpmc_push(&cr->channels[GMK_CHAN_SYS_DROPPED].ring, task);
    }
}

int gmk_chan_emit(gmk_chan_reg_t *cr, uint32_t chan_id, gmk_task_t *task) {
    if (!cr || !task || chan_id >= cr->n_channels)
        return GMK_FAIL(GMK_ERR_INVALID);

    gmk_chan_entry_t *ch = &cr->channels[chan_id];
    if (!ch->open) return GMK_CHAN_CLOSED;

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

    /* P2P fast-path: if exactly 1 subscriber, route directly */
    if (ch->mode == GMK_CHAN_P2P && ch->n_subs == 1 && ch->subs[0].active) {
        if (_gmk_enqueue(cr->sched, task, ch->subs[0].worker_id) == 0) {
            gmk_atomic_add(&ch->emit_count, 1, memory_order_relaxed);
            if (cr->metrics)
                gmk_metric_inc(cr->metrics, task->tenant, GMK_METRIC_CHAN_EMITS, 1);
            return GMK_OK;
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
    if (ch->mode == GMK_CHAN_P2P && ch->n_subs > 0) {
        gmk_chan_drain(cr, chan_id, 1);
    }

    return GMK_OK;
}

int gmk_chan_sub(gmk_chan_reg_t *cr, uint32_t chan_id, uint32_t module_id,
                int worker_id) {
    if (!cr || chan_id >= cr->n_channels)
        return GMK_FAIL(GMK_ERR_INVALID);

    gmk_chan_entry_t *ch = &cr->channels[chan_id];
    if (!ch->open) return GMK_CHAN_CLOSED;

    /* P2P: only one subscriber allowed */
    if (ch->mode == GMK_CHAN_P2P && ch->n_subs >= 1)
        return GMK_CHAN_ALREADY_BOUND;

    if (ch->n_subs >= GMK_MAX_CHAN_SUBS)
        return GMK_FAIL(GMK_ERR_FULL);

    gmk_chan_sub_t *sub = &ch->subs[ch->n_subs++];
    sub->module_id = module_id;
    sub->worker_id = worker_id;
    sub->active    = true;
    return GMK_OK;
}

int gmk_chan_close(gmk_chan_reg_t *cr, uint32_t chan_id) {
    if (!cr || chan_id >= cr->n_channels)
        return GMK_FAIL(GMK_ERR_INVALID);

    /* Don't close system channels */
    if (chan_id <= GMK_CHAN_SYS_DROPPED)
        return GMK_FAIL(GMK_ERR_INVALID);

    gmk_chan_entry_t *ch = &cr->channels[chan_id];
    ch->open = false;

    if (cr->trace)
        gmk_trace_write(cr->trace, 0, GMK_EV_CHAN_CLOSE, 0, chan_id, 0);

    return GMK_OK;
}

int gmk_chan_drain(gmk_chan_reg_t *cr, uint32_t chan_id, uint32_t limit) {
    if (!cr || chan_id >= cr->n_channels) return 0;

    gmk_chan_entry_t *ch = &cr->channels[chan_id];
    if (ch->n_subs == 0) return 0;
    if (limit == 0) limit = UINT32_MAX;

    uint32_t drained = 0;
    gmk_task_t task;

    while (drained < limit && gmk_ring_mpmc_pop(&ch->ring, &task) == 0) {
        if (ch->mode == GMK_CHAN_P2P) {
            /* P2P: route to the single subscriber */
            if (ch->subs[0].active) {
                if (_gmk_enqueue(cr->sched, &task, ch->subs[0].worker_id) != 0) {
                    route_to_dead_letter(cr, &task);
                    gmk_atomic_add(&ch->drop_count, 1, memory_order_relaxed);
                }
            }
        } else {
            /* Fan-out: copy task header to each subscriber */
            for (uint32_t i = 0; i < ch->n_subs; i++) {
                if (!ch->subs[i].active) continue;

                gmk_task_t copy = task;
                /* Shared payload_ptr — fan-out shares read-only */
                int rc = _gmk_enqueue(cr->sched, &copy, ch->subs[i].worker_id);
                if (rc != 0) {
                    if (ch->guarantee == GMK_CHAN_LOSSY) {
                        /* Drop-slow: drop this subscriber's copy */
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
                }
            }
        }
        drained++;
    }

    if (drained > 0 && cr->trace)
        gmk_trace_write(cr->trace, 0, GMK_EV_CHAN_DRAIN, 0, chan_id, drained);

    return (int)drained;
}

int gmk_chan_find(const gmk_chan_reg_t *cr, const char *name) {
    if (!cr || !name) return -1;
    for (uint32_t i = 0; i < cr->n_channels; i++) {
        if (cr->channels[i].open &&
            strncmp(cr->channels[i].name, name, GMK_MAX_CHAN_NAME) == 0)
            return (int)i;
    }
    return -1;
}
