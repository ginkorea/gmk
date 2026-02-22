/*
 * GMK/cpu — Local APIC: init, EOI, IPI, periodic timer
 *
 * LAPIC is memory-mapped at 0xFEE00000 (accessed via HHDM).
 */
#include "lapic.h"
#include "mem.h"
#include "serial.h"

#define LAPIC_PHYS_BASE  0xFEE00000ULL

/* LAPIC register offsets */
#define LAPIC_ID         0x020
#define LAPIC_VERSION    0x030
#define LAPIC_TPR        0x080
#define LAPIC_EOI        0x0B0
#define LAPIC_SPURIOUS   0x0F0
#define LAPIC_ICR_LO     0x300
#define LAPIC_ICR_HI     0x310
#define LAPIC_TIMER_LVT  0x320
#define LAPIC_TIMER_ICR  0x380
#define LAPIC_TIMER_CCR  0x390
#define LAPIC_TIMER_DCR  0x3E0

/* Delivery status bit */
#define ICR_DELIVERY_PENDING (1 << 12)

/* PIT (8254) ports for calibration */
#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43
#define PIT_GATE      0x61
#define PIT_FREQ      1193182  /* Hz */

static volatile uint32_t *lapic_base;
static uint32_t lapic_ticks_per_ms;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

void lapic_init(void) {
    lapic_base = (volatile uint32_t *)phys_to_virt(LAPIC_PHYS_BASE);

    /* Enable LAPIC: set spurious interrupt vector + software enable bit */
    lapic_write(LAPIC_SPURIOUS, 0xFF | (1 << 8));

    /* Set task priority to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* Send EOI to clear any pending state */
    lapic_eoi();
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    /* Wait for previous IPI to complete */
    while (lapic_read(LAPIC_ICR_LO) & ICR_DELIVERY_PENDING)
        __builtin_ia32_pause();

    /* Set destination APIC ID */
    lapic_write(LAPIC_ICR_HI, apic_id << 24);

    /* Send: fixed delivery, vector */
    lapic_write(LAPIC_ICR_LO, (uint32_t)vector);
}

/*
 * Calibrate LAPIC timer using PIT channel 2 as a reference.
 * PIT channel 2 is the "speaker" channel — we can gate it on/off
 * and poll its output bit without needing interrupts.
 */
static uint32_t calibrate_lapic_timer(void) {
    /* Disable interrupts during calibration to avoid skewing the measurement */
    __asm__ volatile("cli");

    /* Set LAPIC divisor to 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);

    /* Mask LAPIC timer (disable interrupts from it during calibration) */
    lapic_write(LAPIC_TIMER_LVT, (1 << 16)); /* masked */

    /* Program PIT channel 2: mode 0 (one-shot), binary, count = 0xFFFF
     * This gives ~54.925 ms at 1.193182 MHz */
    uint8_t gate = inb(PIT_GATE);
    gate &= ~0x02;  /* gate off (disable speaker) */
    gate |=  0x01;   /* enable channel 2 gate */
    outb(PIT_GATE, gate);

    /* Channel 2, lobyte/hibyte, mode 0, binary */
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2_DATA, 0xFF); /* count low */
    outb(PIT_CH2_DATA, 0xFF); /* count high */

    /* Start LAPIC timer with max count (one-shot) */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* Gate PIT channel 2 on: toggle bit 0 off then on to restart */
    gate = inb(PIT_GATE);
    gate &= ~0x01;
    outb(PIT_GATE, gate);
    gate |= 0x01;
    outb(PIT_GATE, gate);

    /* Wait for PIT output (bit 5 of port 0x61) to go high */
    while (!(inb(PIT_GATE) & 0x20))
        __builtin_ia32_pause();

    /* Read LAPIC current count */
    uint32_t lapic_current = lapic_read(LAPIC_TIMER_CCR);
    uint32_t elapsed = 0xFFFFFFFF - lapic_current;

    /* PIT ran for 65535 ticks at 1193182 Hz = ~54.925 ms
     * ticks_per_ms = elapsed / 54.925
     * = elapsed * PIT_FREQ / (65535 * 1000) */
    uint32_t ticks_ms = (uint32_t)((uint64_t)elapsed * PIT_FREQ / (65535ULL * 1000));

    if (ticks_ms == 0) ticks_ms = 1;

    return ticks_ms;
}

void lapic_timer_init(uint32_t hz) {
    /* Calibrate using PIT */
    lapic_ticks_per_ms = calibrate_lapic_timer();

    kprintf("LAPIC timer: %u ticks/ms (divisor 16)\n", lapic_ticks_per_ms);

    /* Set divide value to 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);

    /* Set timer LVT: periodic mode, vector 32 */
    lapic_write(LAPIC_TIMER_LVT, 32 | (1 << 17)); /* bit 17 = periodic */

    /* Compute count for requested frequency */
    uint32_t count = lapic_ticks_per_ms * (1000 / hz);
    if (count == 0) count = 1;

    lapic_write(LAPIC_TIMER_ICR, count);
}

void lapic_send_ipi_all_but_self(uint8_t vector) {
    /* Wait for previous IPI to complete */
    while (lapic_read(LAPIC_ICR_LO) & ICR_DELIVERY_PENDING)
        __builtin_ia32_pause();

    /* Shorthand: all excluding self (bits 19:18 = 11), fixed delivery */
    lapic_write(LAPIC_ICR_LO, (uint32_t)vector | (3 << 18));
}

uint32_t lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

uint32_t lapic_get_ticks_per_ms(void) {
    return lapic_ticks_per_ms;
}
