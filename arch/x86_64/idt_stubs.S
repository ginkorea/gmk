/*
 * GMK/cpu — ISR assembly thunks for x86_64
 *
 * Uses .altmacro to properly expand numeric vector arguments in .rept loops.
 */

.altmacro

.section .text

/* Macro for ISR without error code (push dummy 0) */
.macro ISR_NOERR vec
.align 16
isr_stub_\vec:
    pushq $0
    pushq $\vec
    jmp isr_common
.endm

/* Macro for ISR with error code (CPU already pushed it) */
.macro ISR_ERR vec
.align 16
isr_stub_\vec:
    pushq $\vec
    jmp isr_common
.endm

/* Exceptions 0-31 (explicitly listed for error code handling) */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8    /* Double Fault */
ISR_NOERR 9
ISR_ERR   10   /* Invalid TSS */
ISR_ERR   11   /* Segment Not Present */
ISR_ERR   12   /* Stack-Segment Fault */
ISR_ERR   13   /* General Protection Fault */
ISR_ERR   14   /* Page Fault */
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17   /* Alignment Check */
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21   /* Control Protection */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30   /* Security Exception */
ISR_NOERR 31

/* IRQs 32-255: generate with .altmacro expansion */
.macro GEN_ISR_NOERR n
    ISR_NOERR %n
.endm

.set i, 32
.rept 224
    GEN_ISR_NOERR i
    .set i, i + 1
.endr

/* ── Common ISR handler ─────────────────────────────────────────── */
.align 16
isr_common:
    /* Save all general-purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Pass frame pointer as argument (rdi = first arg in SysV ABI) */
    movq %rsp, %rdi
    call isr_handler

    /* Restore all general-purpose registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Remove vector and error code */
    addq $16, %rsp

    iretq

/* ── ISR stub table ─────────────────────────────────────────────── */
.section .data
.global isr_stub_table

/* Generate table entries with .altmacro */
.macro TABLE_ENTRY n
    .quad isr_stub_\n
.endm

isr_stub_table:
.set i, 0
.rept 256
    TABLE_ENTRY %i
    .set i, i + 1
.endr
