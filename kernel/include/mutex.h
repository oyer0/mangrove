/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <spinlock.h>
#include <types.h>

struct kernel_thread;

/*
 * A mutex protects a critical section that may sleep.  Its internal lock is
 * held only while changing ownership or the waiter chain; it must never be
 * held across scheduler_block(), block I/O, or other blocking work.
 *
 * The VFS metadata lock is a short lookup/lifetime lock and is released
 * before acquiring a blocking mutex.  File operations order their locks as
 * file offset mutex -> optional logical operation guard -> superblock operation
 * mutex -> block I/O gate; directory cursors substitute their cursor mutex
 * for the offset mutex. Resolved pin finish retains the logical guard and
 * reacquires only the raw operation mutex.  The metadata
 * lock is never held across blocking work.  Callers must not sleep while
 * holding a spinlock, and must not hold unrelated subsystem locks across a
 * superblock operation. Native device inspection instead takes its own
 * outer snapshot gate, then short registry/VFS metadata locks or block I/O.
 * No VFS, management, discovery or removal path acquires that snapshot gate;
 * callers must enter inspection without those locks.
 * Mutex state_lock may acquire scheduler_lock only
 * for wakeup; scheduler lifecycle paths drop scheduler_lock before mutex
 * cancellation. A mutex is non-recursive.
 */
typedef struct kernel_mutex {
    spinlock_t state_lock;
    struct kernel_thread *owner;
    struct kernel_thread *wait_head;
    struct kernel_thread *wait_tail;
} mutex_t;

void mutex_init(mutex_t *mutex);
bool mutex_try_lock(mutex_t *mutex);
bool mutex_lock(mutex_t *mutex);
bool mutex_unlock(mutex_t *mutex);

/* Nonblocking FIFO admission for a persistent kernel service thread. Returns
 * -1 for invalid/recursive admission, 0 while queued, 1 when ownership has
 * been acquired/consumed. While queued the caller may deliver completions,
 * but must not acquire another sleeping mutex. Poll only on a wake/condition
 * change. scheduler_block() consumes an early handoff wake without parking.
 * Cancel an unconsumed request with mutex_cancel_waiter(); after consumption
 * only the owning thread may mutex_unlock(). One request per thread. */
int mutex_lock_deferred(mutex_t *mutex);

/* Remove a thread that is being terminated from a mutex wait chain. */
void mutex_cancel_waiter(struct kernel_thread *thread);
