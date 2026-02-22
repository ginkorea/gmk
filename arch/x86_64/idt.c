/*
 * GMK/cpu — IDT: 256 entries, exception handlers, IRQ stubs
 */
#include "idt.h"
#include "lapic.h"
#include "serial.h"
#include "vmm.h"

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtr;

/* Assembly ISR stubs (defined in idt_stubs.S) */
extern void *isr_stub_table[256];

static void set_gate(int vec, void *handler, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[vec].offset_lo  = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector   = 0x08;  /* kernel code segment */
    idt[vec].ist        = 0;
    idt[vec].type_attr  = type_attr;
    idt[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_hi  = (uint32_t)(addr >> 32);
    idt[vec].reserved   = 0;
}

static const char *exception_names[] = {
    "Division Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FP Exception", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved",
};

/* Timer-based kernel shutdown hook.
 * kmain sets this pointer; after shutdown_ticks timer interrupts, *shutdown_flag
 * is set to false, causing all worker loops to exit. */
static _Atomic(bool) *shutdown_flag = 0;
static volatile uint32_t shutdown_ticks = 0;
static volatile uint32_t timer_count = 0;

void idt_set_shutdown_timer(uint32_t ticks, _Atomic(bool) *flag) {
    shutdown_flag = flag;
    shutdown_ticks = ticks;
    timer_count = 0;
}

/* Forward declaration for TLB shootdown handler */
extern void vmm_tlb_shootdown_handler(void);

/* C handler called from isr_common */
void isr_handler(interrupt_frame_t *frame) {
    uint64_t vec = frame->vector;

    if (vec < 32) {
        /* Page fault: try demand paging first */
        if (vec == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

            /* Not-present fault (bit 0 of error_code = 0) in heap range? */
            if (!(frame->error_code & 1) && vmm_demand_page(cr2) == 0) {
                return; /* page mapped, resume execution */
            }

            /* Not a demand-page fault — fall through to crash dump */
            kprintf("\n!!! PAGE FAULT !!!\n");
            kprintf("  CR2: 0x%lx  Error: 0x%lx\n", cr2, frame->error_code);
            kprintf("  RIP: 0x%lx  RSP: 0x%lx\n", frame->rip, frame->rsp);
            kprintf("  SYSTEM HALTED.\n");
            for (;;) __asm__ volatile("cli; hlt");
        }

        /* All other exceptions: dump and halt */
        kprintf("\n!!! EXCEPTION %lu: %s\n", vec,
                vec < 32 ? exception_names[vec] : "Unknown");
        kprintf("  Error code: 0x%lx\n", frame->error_code);
        kprintf("  RIP: 0x%lx  RSP: 0x%lx\n", frame->rip, frame->rsp);
        kprintf("  CS:  0x%lx  SS:  0x%lx\n", frame->cs, frame->ss);
        kprintf("  RAX: 0x%lx  RBX: 0x%lx\n", frame->rax, frame->rbx);
        kprintf("  RCX: 0x%lx  RDX: 0x%lx\n", frame->rcx, frame->rdx);
        kprintf("  RSI: 0x%lx  RDI: 0x%lx\n", frame->rsi, frame->rdi);
        kprintf("  RBP: 0x%lx  RFLAGS: 0x%lx\n", frame->rbp, frame->rflags);
        kprintf("  SYSTEM HALTED.\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Timer tick (vector 32): check shutdown countdown */
    if (vec == 32) {
        timer_count++;
        if (shutdown_flag && shutdown_ticks > 0 && timer_count >= shutdown_ticks) {
            __atomic_store_n(shutdown_flag, 0, __ATOMIC_RELEASE);
            shutdown_ticks = 0; /* one-shot */
        }
    }

    /* TLB shootdown IPI (vector 0xFD) */
    if (vec == 0xFD) {
        vmm_tlb_shootdown_handler();
    }

    /* IRQs 32-255: send EOI to LAPIC */
    if (vec >= 32) {
        lapic_eoi();
    }
}

void idt_init(void) {
    /* Install all 256 ISR stubs */
    for (int i = 0; i < 256; i++) {
        /* 0x8E = present, ring 0, interrupt gate */
        set_gate(i, isr_stub_table[i], 0x8E);
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    idt_load();
}

void idt_load(void) {
    __asm__ volatile("lidt (%0)" : : "r"(&idtr) : "memory");
}
