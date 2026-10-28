/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the production timer_uptime_ms() path with controlled HPET reads. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../../kernel/src/timer.c"

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile u64 fake_hpet[64] __attribute__((aligned(8)));

u32 inl(u16 port) { (void)port; return 0; }

u64 spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock == &monotonic_counter_lock);
    assert(pthread_mutex_lock(&counter_lock) == 0);
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    assert(lock == &monotonic_counter_lock && flags == 0);
    assert(pthread_mutex_unlock(&counter_lock) == 0);
}

static void *concurrent_reader(void *argument)
{
    (void)argument;
    u64 previous = 0;
    for (unsigned i = 0; i < 10000; i++) {
        u64 current = timer_uptime_ms();
        assert(current >= previous);
        previous = current;
    }
    return NULL;
}

int main(void)
{
    pthread_t readers[4];
    monotonic_hpet = (volatile u8 *)fake_hpet;
    monotonic_source = CLOCKSOURCE_HPET;
    monotonic_counter_mask = ~(u64)0;
    monotonic_period_fs = 10000000ULL;
    monotonic_counter_origin = 1000;
    monotonic_base_ms = 7;
    __atomic_store_n(&ticks, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&monotonic_published_ms, 7, __ATOMIC_RELAXED);
    monotonic_is_ready = true;
    fake_hpet[HPET_MAIN_COUNTER / sizeof(u64)] = 1000;
    assert(timer_uptime_ms() == 7);

    /* No PIT interrupt is delivered, but the elapsed hardware counter still
     * accounts for the full interval instead of losing/coarsening it. */
    fake_hpet[HPET_MAIN_COUNTER / sizeof(u64)] = 100001000ULL;
    assert(timer_ticks() == 0);
    assert(timer_uptime_ms() == 1007);
    assert(timer_ticks() == 0);

    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&readers[i], NULL, concurrent_reader, NULL));
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_join(readers[i], NULL));

    /* A narrow counter is extended from an observed wrap by the production
     * path. The counter read and state update share the wrap lock. */
    monotonic_counter_mask = 0xFFFFFFFFULL;
    monotonic_period_fs = 10000000000ULL;
    monotonic_counter_origin = 0xFFFFFFF0ULL;
    monotonic_base_ms = 0;
    memset(&monotonic_counter_state, 0, sizeof(monotonic_counter_state));
    assert(clocksource_extend_counter(&monotonic_counter_state,
        monotonic_counter_origin, monotonic_counter_mask,
        &monotonic_extended_origin));
    __atomic_store_n(&monotonic_published_ms, 0, __ATOMIC_RELAXED);
    *(volatile u32 *)((volatile u8 *)fake_hpet + HPET_MAIN_COUNTER) = 0xB0U;
    assert(timer_uptime_ms() == 1);

    puts("timer clocksource: elapsed time independent of PIT delivery, monotonic publication and wrapped-counter integration passed");
    return 0;
}
