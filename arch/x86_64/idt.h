/*
 * GMK/cpu — IDT setup + fault handler
 */
#ifndef GMK_IDT_H
#define GMK_IDT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* Interrupt frame pushed by isr_common stub */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

void idt_init(void);
void idt_load(void);

/* Set a timer-based shutdown: after `ticks` LAPIC timer interrupts,
 * *flag is set to false (triggers worker loop exit). */
void idt_set_shutdown_timer(uint32_t ticks, _Atomic(bool) *flag);

#endif /* GMK_IDT_H */
