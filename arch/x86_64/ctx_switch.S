/*
 * GMK/cpu — Context switch: save/restore callee-saved registers + rsp
 *
 * void gmk_ctx_switch(gmk_cpu_ctx_t *old, gmk_cpu_ctx_t *new);
 *
 * gmk_cpu_ctx_t layout (from arch/thread.h):
 *   offset 0:  rsp
 *   offset 8:  rbx
 *   offset 16: rbp
 *   offset 24: r12
 *   offset 32: r13
 *   offset 40: r14
 *   offset 48: r15
 */

.section .text
.global gmk_ctx_switch

gmk_ctx_switch:
    /* Save callee-saved registers to old context (rdi) */
    movq %rbx,  8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    movq %rsp,  0(%rdi)

    /* Restore callee-saved registers from new context (rsi) */
    movq  0(%rsi), %rsp
    movq  8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15

    ret
