/*
 * GMK/cpu — Atomic metric counters
 *
 * Per-tenant + global atomic counters.
 * Unconditional — never gated by trace level or sampling.
 */
#ifndef GMK_METRICS_H
#define GMK_METRICS_H

#include "types.h"

struct gmk_metrics {
    _Atomic(uint64_t) global[GMK_METRIC_COUNT];
    _Atomic(uint64_t) per_tenant[GMK_MAX_TENANTS][GMK_METRIC_COUNT];
    uint32_t          n_tenants;
};

int  gmk_metrics_init(gmk_metrics_t *m, uint32_t n_tenants);
void gmk_metrics_destroy(gmk_metrics_t *m);

/* Increment a metric counter. */
void gmk_metric_inc(gmk_metrics_t *m, uint16_t tenant,
                    uint32_t metric_id, uint64_t delta);

/* Read global counter. */
uint64_t gmk_metric_get(const gmk_metrics_t *m, uint32_t metric_id);

/* Read per-tenant counter. */
uint64_t gmk_metric_get_tenant(const gmk_metrics_t *m, uint16_t tenant,
                               uint32_t metric_id);

/* Reset all counters to zero. */
void gmk_metrics_reset(gmk_metrics_t *m);

#endif /* GMK_METRICS_H */
