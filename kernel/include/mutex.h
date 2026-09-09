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
 * file offset mutex -> superblock operation mutex -> block I/O gate; directory
 * cursors use their cursor mutex -> superblock operation mutex.  The metadata
 * lock is never held across blocking work.  Callers must not sleep while
 * holding a spinlock, and must not hold unrelated subsystem locks across a
 * superblock operation.  A mutex is non-recursive.
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

/* Remove a thread that is being terminated from a mutex wait chain. */
void mutex_cancel_waiter(struct kernel_thread *thread);
