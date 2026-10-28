/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <idt.h>
#include <mg/time.h>

#define TIMER_FREQUENCY 1000

typedef struct {
    u64 start_ticks;
    u64 duration_ticks;
    bool expired;
} timer_monotonic_deadline_t;

void timer_init (void);

/* The raw delivered-event count is diagnostic/scheduler state. Uptime and
 * monotonic milliseconds use the selected elapsed-time clocksource. */
u64 timer_ticks(void);
u64 timer_uptime_ms(void);
u64 timer_preemptions(void);
void timer_monotonic_info(mg_monotonic_info_t *out);

/* Initializes an interrupt-independent polling clock after ACPI and the
 * permanent MMIO mapping infrastructure are ready. This clock is for
 * bounded hardware waits and remains usable while CPU IF is clear. */
bool timer_monotonic_init(void);
bool timer_monotonic_ready(void);
bool timer_monotonic_deadline_start(timer_monotonic_deadline_t *deadline,
                                    u64 microseconds);
bool timer_monotonic_deadline_expired(timer_monotonic_deadline_t *deadline);
bool timer_monotonic_delay_us(u64 microseconds);

/* Busy delay using the selected monotonic source. Callers must already have a
 * working runtime clocksource; this is not an early-boot wait. */
void timer_sleep(u64 ms);
void timer_delay(u64 ms);

void timer_interrupt(struct cpu_registers *regs);
void timer_lapic_interrupt(struct cpu_registers *regs);
