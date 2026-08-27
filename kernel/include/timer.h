#pragma once

#include <types.h>
#include <idt.h>

#define TIMER_FREQUENCY 1000

typedef struct {
    u64 start_ticks;
    u64 duration_ticks;
    bool expired;
} timer_monotonic_deadline_t;

void timer_init (void);

/* PIT-backed runtime clock. These values advance from IRQ0 and are suitable
 * for scheduler/runtime policy after CPU interrupt delivery is enabled. */
u64 timer_ticks(void);
u64 timer_uptime_ms(void);
u64 timer_preemptions(void);

/* Initializes an interrupt-independent polling clock after ACPI and the
 * permanent MMIO mapping infrastructure are ready. This clock is for
 * bounded hardware waits and remains usable while CPU IF is clear. */
bool timer_monotonic_init(void);
bool timer_monotonic_ready(void);
bool timer_monotonic_deadline_start(timer_monotonic_deadline_t *deadline,
                                    u64 microseconds);
bool timer_monotonic_deadline_expired(timer_monotonic_deadline_t *deadline);
bool timer_monotonic_delay_us(u64 microseconds);

/* Busy delay using the PIT-backed runtime clock. Callers must already have a
 * working runtime interrupt source; this is not an early-boot wait. */
void timer_sleep(u64 ms);
void timer_delay(u64 ms);

void timer_interrupt(struct cpu_registers *regs);
