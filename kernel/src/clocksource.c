/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <clocksource.h>

#define FEMTOSECONDS_PER_MILLISECOND 1000000000000ULL

clocksource_id_t clocksource_select(bool hpet_available, bool hpet_64bit,
                                    bool pm_timer_available,
                                    bool pm_timer_32bit)
{
    if (hpet_available && hpet_64bit)
        return CLOCKSOURCE_HPET;
    if (pm_timer_available && pm_timer_32bit)
        return CLOCKSOURCE_ACPI_PM_TIMER;
    if (hpet_available)
        return CLOCKSOURCE_HPET;
    if (pm_timer_available)
        return CLOCKSOURCE_ACPI_PM_TIMER;
    return CLOCKSOURCE_PIT;
}

bool clocksource_extend_counter(clocksource_counter_state_t *state,
                                u64 raw, u64 mask, u64 *out_extended)
{
    u64 modulus;

    if (!state || !out_extended || !mask || (mask & (mask + 1U)) != 0U)
        return false;
    raw &= mask;
    modulus = mask + 1U;
    if (!state->initialized) {
        state->last_low = raw;
        state->initialized = true;
        *out_extended = raw;
        return true;
    }
    if (raw < state->last_low) {
        if (state->high > ~(u64)0 - modulus)
            return false;
        state->high += modulus;
    }
    state->last_low = raw;
    if (state->high > ~(u64)0 - raw)
        return false;
    *out_extended = state->high + raw;
    return true;
}

u64 clocksource_ticks_to_ms(u64 ticks, u64 period_fs)
{
    u64 low;
    u64 high;
    u64 elapsed_ms;
    u64 remainder;
    __uint128_t product;
    register u64 divisor __asm__("rcx") =
        FEMTOSECONDS_PER_MILLISECOND;

    if (!period_fs)
        return 0;

    /* The kernel deliberately does not link compiler runtime arithmetic.
     * x86-64's unsigned multiply/divide gives the exact 128-bit product and
     * quotient without a libgcc helper.  A quotient that does not fit in
     * RAX is saturated before DIVQ, which would otherwise fault. */
    product = (__uint128_t)ticks * period_fs;
    low = (u64)product;
    high = (u64)(product >> 64);
    if (high >= divisor)
        return ~(u64)0;
    __asm__ volatile("divq %%rcx"
                     : "=a"(elapsed_ms), "=d"(remainder)
                     : "a"(low), "d"(high), "c"(divisor));
    (void)remainder;
    return elapsed_ms;
}

u64 clocksource_publish_max(volatile u64 *published, u64 candidate)
{
    u64 observed;

    if (!published)
        return 0;
    observed = __atomic_load_n(published, __ATOMIC_ACQUIRE);
    while (observed < candidate &&
           !__atomic_compare_exchange_n(published, &observed, candidate,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
        /* A competing CPU may have published a later sample. */
    }
    return observed > candidate ? observed : candidate;
}
