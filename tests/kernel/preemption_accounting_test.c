/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include <preemption_count.h>

#define WRITER_COUNT 6U
#define INCREMENTS_PER_WRITER 200000U

static u64 timer_interrupt_preemptions;
static u64 scheduler_timer_preemptions;
static _Atomic u32 ready_writers;
static _Atomic u32 completed_writers;
static _Atomic bool begin_writes;

static void *writer(void *unused)
{
    (void)unused;
    atomic_fetch_add_explicit(&ready_writers, 1U, memory_order_release);
    while (!atomic_load_explicit(&begin_writes, memory_order_acquire))
        sched_yield();

    for (u32 index = 0; index < INCREMENTS_PER_WRITER; index++) {
        preemption_count_increment(&timer_interrupt_preemptions);
        preemption_count_increment(&scheduler_timer_preemptions);
    }
    atomic_fetch_add_explicit(&completed_writers, 1U, memory_order_release);
    return NULL;
}

int main(void)
{
    pthread_t writers[WRITER_COUNT];
    u64 last_timer = 0;
    u64 last_scheduler = 0;

    for (u32 index = 0; index < WRITER_COUNT; index++)
        assert(pthread_create(&writers[index], NULL, writer, NULL) == 0);
    while (atomic_load_explicit(&ready_writers, memory_order_acquire) !=
           WRITER_COUNT)
        sched_yield();
    atomic_store_explicit(&begin_writes, true, memory_order_release);

    while (atomic_load_explicit(&completed_writers, memory_order_acquire) !=
           WRITER_COUNT) {
        u64 timer_sample =
            preemption_count_snapshot(&timer_interrupt_preemptions);
        u64 scheduler_sample =
            preemption_count_snapshot(&scheduler_timer_preemptions);
        assert(timer_sample >= last_timer);
        assert(scheduler_sample >= last_scheduler);
        assert(timer_sample <=
               (u64)WRITER_COUNT * INCREMENTS_PER_WRITER);
        assert(scheduler_sample <=
               (u64)WRITER_COUNT * INCREMENTS_PER_WRITER);
        last_timer = timer_sample;
        last_scheduler = scheduler_sample;
    }

    for (u32 index = 0; index < WRITER_COUNT; index++)
        assert(pthread_join(writers[index], NULL) == 0);

    assert(preemption_count_snapshot(&timer_interrupt_preemptions) ==
           (u64)WRITER_COUNT * INCREMENTS_PER_WRITER);
    assert(preemption_count_snapshot(&scheduler_timer_preemptions) ==
           (u64)WRITER_COUNT * INCREMENTS_PER_WRITER);
    puts("preemption accounting: concurrent increments and snapshots passed");
    return 0;
}
