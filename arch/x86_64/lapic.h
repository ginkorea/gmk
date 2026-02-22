/*
 * GMK/cpu — Local APIC init, EOI, IPI, periodic timer
 */
#ifndef GMK_LAPIC_H
#define GMK_LAPIC_H

#include <stdint.h>

#define IPI_WAKE_VECTOR    0xFE
#define IPI_TLB_VECTOR     0xFD

void     lapic_init(void);
void     lapic_eoi(void);
void     lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void     lapic_send_ipi_all_but_self(uint8_t vector);
void     lapic_timer_init(uint32_t hz);
uint32_t lapic_id(void);
uint32_t lapic_get_ticks_per_ms(void);

#endif /* GMK_LAPIC_H */
