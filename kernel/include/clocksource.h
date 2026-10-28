/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

typedef enum {
    CLOCKSOURCE_NONE = 0,
    CLOCKSOURCE_HPET = 1,
    CLOCKSOURCE_ACPI_PM_TIMER = 2,
    CLOCKSOURCE_PIT = 3,
} clocksource_id_t;

typedef struct {
    u64 high;
    u64 last_low;
    bool initialized;
} clocksource_counter_state_t;

/* Selects the first source with the strongest wrap behavior. The invariant
 * TSC and KVM pvclock are intentionally not candidates until their frequency,
 * SMP, and pause semantics are qualified for this kernel. */
clocksource_id_t clocksource_select(bool hpet_available, bool hpet_64bit,
                                    bool pm_timer_available,
                                    bool pm_timer_32bit);

/* Extends a narrow free-running counter across an observed wrap. `mask` must
 * be a contiguous low-bit mask. Multiple unobserved wraps cannot be inferred
 * from a narrow hardware counter and are reported as a source limitation. */
bool clocksource_extend_counter(clocksource_counter_state_t *state,
                                u64 raw, u64 mask, u64 *out_extended);

/* Convert femtosecond-period counter ticks to elapsed milliseconds without
 * overflowing intermediate 64-bit arithmetic. Saturates on result overflow. */
u64 clocksource_ticks_to_ms(u64 ticks, u64 period_fs);

/* Publish a sampled clock value as a nondecreasing shared value under SMP. */
u64 clocksource_publish_max(volatile u64 *published, u64 candidate);
