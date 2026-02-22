/*
 * GMK/cpu — SMP bringup via Limine goto_address
 */
#ifndef GMK_SMP_H
#define GMK_SMP_H

#include <stdint.h>

/* Initialize SMP: read Limine SMP response, prepare AP stacks.
   Returns number of CPUs available. */
uint32_t smp_init(void *smp_response);

/* Start APs: write goto_address for each AP, they enter the given entry fn.
   The entry function receives the worker pool pointer via extra_argument. */
void smp_start_aps(void (*ap_entry)(void *), void *arg);

/* Get the BSP's LAPIC ID */
uint32_t smp_bsp_lapic_id(void);

/* Get CPU count */
uint32_t smp_cpu_count(void);

/* Get LAPIC ID for CPU index */
uint32_t smp_lapic_id(uint32_t cpu_idx);

#endif /* GMK_SMP_H */
