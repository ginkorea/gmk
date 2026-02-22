/*
 * GMK/cpu — Atomic metric counters
 */
#include "gmk/metrics.h"
#ifndef GMK_FREESTANDING
#include <string.h>
#endif

int gmk_metrics_init(gmk_metrics_t *m, uint32_t n_tenants) {
    if (!m || n_tenants == 0 || n_tenants > GMK_MAX_TENANTS)
        return -1;

    memset(m, 0, sizeof(*m));
    m->n_tenants = n_tenants;

    for (uint32_t i = 0; i < GMK_METRIC_COUNT; i++)
        atomic_init(&m->global[i], 0);

    for (uint32_t t = 0; t < n_tenants; t++)
        for (uint32_t i = 0; i < GMK_METRIC_COUNT; i++)
            atomic_init(&m->per_tenant[t][i], 0);

    return 0;
}

void gmk_metrics_destroy(gmk_metrics_t *m) {
    (void)m;
    /* No dynamic resources */
}

void gmk_metric_inc(gmk_metrics_t *m, uint16_t tenant,
                    uint32_t metric_id, uint64_t delta) {
    if (!m || metric_id >= GMK_METRIC_COUNT) return;

    gmk_atomic_add(&m->global[metric_id], delta, memory_order_relaxed);

    if (tenant < m->n_tenants)
        gmk_atomic_add(&m->per_tenant[tenant][metric_id], delta,
                       memory_order_relaxed);
}

uint64_t gmk_metric_get(const gmk_metrics_t *m, uint32_t metric_id) {
    if (!m || metric_id >= GMK_METRIC_COUNT) return 0;
    return gmk_atomic_load(&m->global[metric_id], memory_order_relaxed);
}

uint64_t gmk_metric_get_tenant(const gmk_metrics_t *m, uint16_t tenant,
                               uint32_t metric_id) {
    if (!m || metric_id >= GMK_METRIC_COUNT || tenant >= m->n_tenants)
        return 0;
    return gmk_atomic_load(&m->per_tenant[tenant][metric_id],
                           memory_order_relaxed);
}

void gmk_metrics_reset(gmk_metrics_t *m) {
    if (!m) return;
    for (uint32_t i = 0; i < GMK_METRIC_COUNT; i++)
        gmk_atomic_store(&m->global[i], 0, memory_order_relaxed);
    for (uint32_t t = 0; t < m->n_tenants; t++)
        for (uint32_t i = 0; i < GMK_METRIC_COUNT; i++)
            gmk_atomic_store(&m->per_tenant[t][i], 0, memory_order_relaxed);
}
