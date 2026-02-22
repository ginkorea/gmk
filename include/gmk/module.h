/*
 * GMK/cpu — Module registry + dispatch table
 *
 * Explicit registration array for v0.1.
 * Dispatch table: gmk_handler_fn dispatch[256] indexed by type ID.
 * Poison: simple threshold (N failures = poisoned).
 */
#ifndef GMK_MODULE_H
#define GMK_MODULE_H

#include "types.h"
#include "chan.h"

typedef struct {
    gmk_handler_fn   dispatch[GMK_MAX_HANDLERS];
    const char       *handler_names[GMK_MAX_HANDLERS];
    uint32_t          max_yields[GMK_MAX_HANDLERS];
    _Atomic(uint32_t) fail_counts[GMK_MAX_HANDLERS];
    bool              poisoned[GMK_MAX_HANDLERS];

    gmk_module_t     *modules[GMK_MAX_MODULES];
    uint32_t          n_modules;
    uint32_t          n_handlers;

    gmk_chan_reg_t    *chan;     /* for channel declarations */
    gmk_trace_t      *trace;
    gmk_metrics_t    *metrics;
} gmk_module_reg_t;

int  gmk_module_reg_init(gmk_module_reg_t *mr, gmk_chan_reg_t *chan,
                         gmk_trace_t *trace, gmk_metrics_t *metrics);
void gmk_module_reg_destroy(gmk_module_reg_t *mr);

/* Register a module: builds dispatch table entries, validates channel decls. */
int  gmk_module_register(gmk_module_reg_t *mr, gmk_module_t *mod);

/* Initialize all registered modules (call mod->init). */
int  gmk_module_init_all(gmk_module_reg_t *mr, gmk_ctx_t *ctx);

/* Finalize all registered modules (call mod->fini). */
void gmk_module_fini_all(gmk_module_reg_t *mr, gmk_ctx_t *ctx);

/* Dispatch a task: look up handler by type, call it.
   Returns handler return code, or GMK_FAIL if poisoned/not-found. */
int  gmk_module_dispatch(gmk_module_reg_t *mr, gmk_ctx_t *ctx);

/* Record a handler failure. Checks poison threshold. */
void gmk_module_record_fail(gmk_module_reg_t *mr, uint32_t type);

/* Check if a type is poisoned. */
bool gmk_module_is_poisoned(const gmk_module_reg_t *mr, uint32_t type);

/* Reset poison for a type. */
void gmk_module_reset_poison(gmk_module_reg_t *mr, uint32_t type);

#endif /* GMK_MODULE_H */
