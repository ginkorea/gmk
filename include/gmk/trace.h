/*
 * GMK/cpu — Trace ring buffer + filtering
 *
 * Per-tenant trace rings (SPSC). Level-based filtering.
 * Statistical sampling via fast PRNG.
 */
#ifndef GMK_TRACE_H
#define GMK_TRACE_H

#include "types.h"
#include "ring_spsc.h"

#define GMK_TRACE_RING_CAP  4096  /* per-tenant, power of two */

struct gmk_trace {
    gmk_ring_spsc_t  rings[GMK_MAX_TENANTS];  /* per-tenant trace rings */
    uint32_t          level;                    /* global trace level     */
    float             sample_rate;              /* 0.0..1.0               */
    uint32_t          prng_state;               /* fast PRNG seed         */
    uint32_t          n_tenants;                /* active tenant count    */
    _Atomic(uint64_t) total_events;
    _Atomic(uint64_t) dropped_events;
};

int  gmk_trace_init(gmk_trace_t *t, uint32_t n_tenants);
void gmk_trace_destroy(gmk_trace_t *t);

/* Write a trace event. Checks level + sampling. */
void gmk_trace_write(gmk_trace_t *t, uint16_t tenant, uint32_t ev_type,
                     uint16_t task_type, uint32_t arg0, uint32_t arg1);

/* Force-write a trace event (bypasses level/sampling). */
void gmk_trace_write_force(gmk_trace_t *t, uint16_t tenant, uint32_t ev_type,
                           uint16_t task_type, uint32_t arg0, uint32_t arg1);

/* Read one trace event from tenant's ring. Returns 0 on success, -1 if empty. */
int  gmk_trace_read(gmk_trace_t *t, uint16_t tenant, gmk_trace_ev_t *ev);

/* Set global trace level. */
void gmk_trace_set_level(gmk_trace_t *t, uint32_t level);

/* Set sampling rate (0.0 = none, 1.0 = all). */
void gmk_trace_set_sample_rate(gmk_trace_t *t, float rate);

/* Get total events written. */
uint64_t gmk_trace_total(const gmk_trace_t *t);

/* Get total events dropped (ring full). */
uint64_t gmk_trace_dropped(const gmk_trace_t *t);

#endif /* GMK_TRACE_H */
