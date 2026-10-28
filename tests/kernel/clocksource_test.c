/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../../kernel/src/clocksource.c"

static volatile u64 published;

static void *publish_range(void *argument)
{
    u64 base = (u64)(uintptr_t)argument;
    for (u64 i = 1; i <= 10000; i++)
        (void)clocksource_publish_max(&published, base + i);
    return NULL;
}

int main(void)
{
    clocksource_counter_state_t state = {0};
    u64 extended = 0;
    pthread_t workers[4];

    assert(clocksource_select(true, true, true, true) == CLOCKSOURCE_HPET);
    assert(clocksource_select(true, false, true, true) ==
           CLOCKSOURCE_ACPI_PM_TIMER);
    assert(clocksource_select(true, false, true, false) == CLOCKSOURCE_HPET);
    assert(clocksource_select(false, false, true, false) ==
           CLOCKSOURCE_ACPI_PM_TIMER);
    assert(clocksource_select(false, false, false, false) == CLOCKSOURCE_PIT);

    assert(clocksource_ticks_to_ms(0, 10000000ULL) == 0);
    assert(clocksource_ticks_to_ms(100000, 10000000ULL) == 1);
    assert(clocksource_ticks_to_ms(100000000, 10000000ULL) == 1000);
    assert(clocksource_ticks_to_ms(3579545, 279365115ULL) == 1000);
    assert(clocksource_ticks_to_ms(~(u64)0, ~(u64)0) == ~(u64)0);
    assert(clocksource_ticks_to_ms(5, 0) == 0);

    assert(clocksource_extend_counter(&state, 0xFFFFFEU, 0xFFFFFFU,
                                      &extended));
    assert(extended == 0xFFFFFEU);
    assert(clocksource_extend_counter(&state, 1U, 0xFFFFFFU, &extended));
    assert(extended == 0x1000001ULL);
    assert(clocksource_extend_counter(&state, 7U, 0xFFFFFFU, &extended));
    assert(extended == 0x1000007ULL);
    assert(!clocksource_extend_counter(&state, 7U, 0x00FFFFFEU,
                                       &extended));
    assert(!clocksource_extend_counter(NULL, 0, 0xFFFFFFU, &extended));
    assert(!clocksource_extend_counter(&state, 0, 0, &extended));

    for (uintptr_t i = 0; i < 4; i++)
        assert(!pthread_create(&workers[i], NULL, publish_range,
                               (void *)(i * 100000U)));
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_join(workers[i], NULL));
    assert(published == 310000U);
    assert(clocksource_publish_max(&published, 9U) == 310000U);

    puts("clocksource: selection, conversion, wrap extension and concurrent monotonic publication passed");
    return 0;
}
