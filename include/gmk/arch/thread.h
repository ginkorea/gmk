/*
 * GMK/cpu — Kernel thread types for freestanding SMP
 */
#ifndef GMK_ARCH_THREAD_H
#define GMK_ARCH_THREAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* Saved CPU context for context switching (callee-saved + rsp) */
typedef struct {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} gmk_cpu_ctx_t;

/* Kernel thread state */
typedef enum {
    GMK_THREAD_IDLE = 0,
    GMK_THREAD_RUNNING,
    GMK_THREAD_STOPPED,
} gmk_thread_state_t;

/* Per-CPU kernel thread descriptor */
typedef struct {
    uint32_t          cpu_id;
    uint32_t          lapic_id;
    gmk_cpu_ctx_t     ctx;
    void             *stack_base;
    size_t            stack_size;
    gmk_thread_state_t state;
} gmk_kthread_t;

#define GMK_KTHREAD_STACK_SIZE (16 * 1024) /* 16KB per CPU */

#endif /* GMK_ARCH_THREAD_H */
