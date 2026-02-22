/*
 * GMK/cpu — Module register/dispatch/poison
 */
#include "gmk/module.h"
#include "gmk/trace.h"
#include "gmk/metrics.h"
#ifndef GMK_FREESTANDING
#include <string.h>
#endif

int gmk_module_reg_init(gmk_module_reg_t *mr, gmk_chan_reg_t *chan,
                        gmk_trace_t *trace, gmk_metrics_t *metrics) {
    if (!mr) return -1;

    memset(mr, 0, sizeof(*mr));
    mr->chan    = chan;
    mr->trace  = trace;
    mr->metrics = metrics;

    for (uint32_t i = 0; i < GMK_MAX_HANDLERS; i++)
        atomic_init(&mr->fail_counts[i], 0);

    return 0;
}

void gmk_module_reg_destroy(gmk_module_reg_t *mr) {
    (void)mr;
}

int gmk_module_register(gmk_module_reg_t *mr, gmk_module_t *mod) {
    if (!mr || !mod) return -1;
    if (mr->n_modules >= GMK_MAX_MODULES) return GMK_FAIL(GMK_ERR_FULL);

    /* Register handlers into dispatch table */
    for (uint32_t i = 0; i < mod->n_handlers; i++) {
        gmk_handler_reg_t *h = &mod->handlers[i];
        if (h->type >= GMK_MAX_HANDLERS)
            return GMK_FAIL(GMK_ERR_INVALID);

        if (mr->dispatch[h->type] != NULL)
            return GMK_FAIL(GMK_ERR_EXISTS); /* duplicate type */

        mr->dispatch[h->type]      = h->fn;
        mr->handler_names[h->type] = h->name;
        mr->max_yields[h->type]    = h->max_yields > 0 ? h->max_yields
                                                        : GMK_DEFAULT_MAX_YIELDS;
        mr->n_handlers++;
    }

    mr->modules[mr->n_modules++] = mod;
    return 0;
}

int gmk_module_init_all(gmk_module_reg_t *mr, gmk_ctx_t *ctx) {
    if (!mr || !ctx) return -1;

    for (uint32_t i = 0; i < mr->n_modules; i++) {
        if (mr->modules[i]->init) {
            int rc = mr->modules[i]->init(ctx);
            if (rc != GMK_OK) return rc;
        }
    }
    return 0;
}

void gmk_module_fini_all(gmk_module_reg_t *mr, gmk_ctx_t *ctx) {
    if (!mr || !ctx) return;

    /* Finalize in reverse order */
    for (int i = (int)mr->n_modules - 1; i >= 0; i--) {
        if (mr->modules[i]->fini) {
            mr->modules[i]->fini(ctx);
        }
    }
}

int gmk_module_dispatch(gmk_module_reg_t *mr, gmk_ctx_t *ctx) {
    if (!mr || !ctx || !ctx->task) return GMK_FAIL(GMK_ERR_INVALID);

    uint32_t type = ctx->task->type;
    if (type >= GMK_MAX_HANDLERS || mr->dispatch[type] == NULL)
        return GMK_FAIL(GMK_ERR_NOT_FOUND);

    /* Check poison */
    if (mr->poisoned[type]) {
        if (mr->trace)
            gmk_trace_write(mr->trace, ctx->task->tenant,
                           GMK_EV_POISON, (uint16_t)type, type, 0);
        return GMK_FAIL(GMK_ERR_POISONED);
    }

    /* Dispatch */
    if (mr->trace)
        gmk_trace_write(mr->trace, ctx->task->tenant,
                       GMK_EV_TASK_START, (uint16_t)type, ctx->task->seq, 0);

    int rc = mr->dispatch[type](ctx);

    if (mr->trace)
        gmk_trace_write(mr->trace, ctx->task->tenant,
                       GMK_EV_TASK_END, (uint16_t)type, ctx->task->seq,
                       (uint32_t)rc);

    return rc;
}

void gmk_module_record_fail(gmk_module_reg_t *mr, uint32_t type) {
    if (!mr || type >= GMK_MAX_HANDLERS) return;

    uint32_t count = gmk_atomic_add(&mr->fail_counts[type], 1,
                                    memory_order_relaxed) + 1;

    if (count >= GMK_POISON_THRESHOLD && !mr->poisoned[type]) {
        mr->poisoned[type] = true;
        if (mr->trace)
            gmk_trace_write(mr->trace, 0, GMK_EV_POISON, (uint16_t)type,
                           type, count);
    }
}

bool gmk_module_is_poisoned(const gmk_module_reg_t *mr, uint32_t type) {
    if (!mr || type >= GMK_MAX_HANDLERS) return false;
    return mr->poisoned[type];
}

void gmk_module_reset_poison(gmk_module_reg_t *mr, uint32_t type) {
    if (!mr || type >= GMK_MAX_HANDLERS) return;
    mr->poisoned[type] = false;
    gmk_atomic_store(&mr->fail_counts[type], 0, memory_order_relaxed);
}
