/*
 * GMK/cpu — Per-tenant trace rings, level check, sampling
 *
 * Sampling uses fixed-point uint32_t threshold (no floating point).
 */
#include "gmk/trace.h"
#ifndef GMK_FREESTANDING
#include <string.h>
#endif

/* Fast xorshift PRNG */
static inline uint32_t prng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Map event type to required trace level */
static inline uint32_t ev_level(uint32_t ev_type) {
    switch (ev_type) {
    case GMK_EV_TASK_FAIL:
    case GMK_EV_ALLOC_FAIL:
    case GMK_EV_YIELD_OVERFLOW:
    case GMK_EV_YIELD_LIMIT:
    case GMK_EV_POISON:
        return GMK_TRACE_ERROR;
    case GMK_EV_CHAN_FULL:
    case GMK_EV_CHAN_DROP:
    case GMK_EV_WATCHDOG:
        return GMK_TRACE_WARN;
    case GMK_EV_TASK_START:
    case GMK_EV_TASK_END:
    case GMK_EV_TASK_RETRY:
    case GMK_EV_TASK_YIELD:
    case GMK_EV_CHAN_EMIT:
    case GMK_EV_CHAN_DRAIN:
    case GMK_EV_CHAN_OPEN:
    case GMK_EV_CHAN_CLOSE:
    case GMK_EV_WORKER_PARK:
    case GMK_EV_WORKER_WAKE:
    case GMK_EV_BOOT:
    case GMK_EV_HALT:
        return GMK_TRACE_INFO;
    default:
        return GMK_TRACE_ALL;
    }
}

int gmk_trace_init(gmk_trace_t *t, uint32_t n_tenants) {
    if (!t || n_tenants == 0 || n_tenants > GMK_MAX_TENANTS)
        return -1;

    memset(t, 0, sizeof(*t));
    t->level            = GMK_TRACE_WARN;
    t->sample_threshold = UINT32_MAX;  /* 1.0 = trace all */
    t->prng_state       = 0xDEADBEEF;
    t->n_tenants        = n_tenants;
    atomic_init(&t->total_events, 0);
    atomic_init(&t->dropped_events, 0);

    for (uint32_t i = 0; i < n_tenants; i++) {
        if (gmk_ring_spsc_init(&t->rings[i], GMK_TRACE_RING_CAP,
                               sizeof(gmk_trace_ev_t)) != 0)
        {
            /* Clean up previously initialized rings */
            for (uint32_t j = 0; j < i; j++)
                gmk_ring_spsc_destroy(&t->rings[j]);
            return -1;
        }
    }
    return 0;
}

void gmk_trace_destroy(gmk_trace_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->n_tenants; i++)
        gmk_ring_spsc_destroy(&t->rings[i]);
}

static void trace_emit(gmk_trace_t *t, uint16_t tenant, uint32_t ev_type,
                       uint16_t task_type, uint32_t arg0, uint32_t arg1) {
    if (tenant >= t->n_tenants) return;

    gmk_trace_ev_t ev = {
        .tsc     = gmk_tsc(),
        .ev_type = ev_type,
        .tenant  = tenant,
        .type    = task_type,
        .arg0    = arg0,
        .arg1    = arg1,
        ._pad    = 0,
    };

    if (gmk_ring_spsc_push(&t->rings[tenant], &ev) == 0) {
        gmk_atomic_add(&t->total_events, 1, memory_order_relaxed);
    } else {
        gmk_atomic_add(&t->dropped_events, 1, memory_order_relaxed);
    }
}

void gmk_trace_write(gmk_trace_t *t, uint16_t tenant, uint32_t ev_type,
                     uint16_t task_type, uint32_t arg0, uint32_t arg1) {
    if (!t) return;

    /* Level check */
    uint32_t required = ev_level(ev_type);
    if (required > t->level) return;

    /* Sampling check (only for TRACE_ALL level events, fixed-point) */
    if (t->level == GMK_TRACE_ALL && t->sample_threshold < UINT32_MAX) {
        uint32_t r = prng_next(&t->prng_state);
        if (r > t->sample_threshold) return;
    }

    trace_emit(t, tenant, ev_type, task_type, arg0, arg1);
}

void gmk_trace_write_force(gmk_trace_t *t, uint16_t tenant, uint32_t ev_type,
                           uint16_t task_type, uint32_t arg0, uint32_t arg1) {
    if (!t) return;
    trace_emit(t, tenant, ev_type, task_type, arg0, arg1);
}

int gmk_trace_read(gmk_trace_t *t, uint16_t tenant, gmk_trace_ev_t *ev) {
    if (!t || tenant >= t->n_tenants || !ev) return -1;
    return gmk_ring_spsc_pop(&t->rings[tenant], ev);
}

void gmk_trace_set_level(gmk_trace_t *t, uint32_t level) {
    if (t) t->level = level;
}

void gmk_trace_set_sample_rate(gmk_trace_t *t, float rate) {
    if (!t) return;
    if (rate <= 0.0f) {
        t->sample_threshold = 0;
    } else if (rate >= 1.0f) {
        t->sample_threshold = UINT32_MAX;
    } else {
        /* Convert float 0.0..1.0 to uint32_t 0..UINT32_MAX.
         * This is the only float operation, used only via the hosted API.
         * The kernel path (sampling check) is pure integer. */
        t->sample_threshold = (uint32_t)(rate * (double)UINT32_MAX);
    }
}

uint64_t gmk_trace_total(const gmk_trace_t *t) {
    return gmk_atomic_load(&t->total_events, memory_order_relaxed);
}

uint64_t gmk_trace_dropped(const gmk_trace_t *t) {
    return gmk_atomic_load(&t->dropped_events, memory_order_relaxed);
}
