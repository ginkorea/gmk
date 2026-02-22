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

static volatile uint32_t *lapic_base;

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

void lapic_timer_init(uint32_t hz) {
    /* Calibrate timer against a known delay.
     * Simple approach: use a large initial count and estimate from there.
     * For QEMU, we use a reasonable default divisor. */

    /* Set divide value to 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);

    /* Set timer LVT: periodic mode, vector 32 (IRQ0) */
    lapic_write(LAPIC_TIMER_LVT, 32 | (1 << 17)); /* bit 17 = periodic */

    /* Rough calibration: assume ~1GHz bus clock / 16 divisor = ~62.5MHz tick.
     * For 1000 Hz (1ms): count = 62500000 / 1000 = 62500
     * QEMU's LAPIC timer is quite variable, use a reasonable value. */
    uint32_t count = 62500000 / hz / 16;
    if (count == 0) count = 1;

    /* More reliable: just use a value that works well with QEMU */
    count = 100000; /* ~1ms in QEMU at typical settings */
    (void)hz;

    lapic_write(LAPIC_TIMER_ICR, count);
}

uint32_t lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}
