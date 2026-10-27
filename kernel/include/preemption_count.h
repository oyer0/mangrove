/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* These counters are diagnostic accounting only. Relaxed atomics make each
 * increment and sample indivisible without imposing ordering on scheduler or
 * timer state that is published through other synchronization. */
static inline void preemption_count_increment(u64 *counter)
{
    __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

static inline u64 preemption_count_snapshot(const u64 *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}
