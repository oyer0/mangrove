/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mutex.h>
#include <scheduler.h>
#include <stddef.h>

static u64 mutex_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void mutex_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static void mutex_waiter_remove_locked(mutex_t *mutex,
                                        kernel_thread_t *thread)
{
    kernel_thread_t *previous = NULL;
    kernel_thread_t *cursor;

    if (!mutex || !thread) return;
    cursor = mutex->wait_head;
    while (cursor && cursor != thread) {
        previous = cursor;
        cursor = cursor->mutex_wait_next;
    }
    if (!cursor) return;
    if (previous)
        previous->mutex_wait_next = cursor->mutex_wait_next;
    else
        mutex->wait_head = cursor->mutex_wait_next;
    if (mutex->wait_tail == cursor)
        mutex->wait_tail = previous;
    cursor->mutex_wait_next = NULL;
}

static void mutex_waiter_append_locked(mutex_t *mutex,
                                       kernel_thread_t *thread)
{
    if (!mutex || !thread) return;
    thread->mutex_wait_next = NULL;
    if (mutex->wait_tail)
        mutex->wait_tail->mutex_wait_next = thread;
    else
        mutex->wait_head = thread;
    mutex->wait_tail = thread;
}

static bool mutex_handoff_locked(mutex_t *mutex)
{
    kernel_thread_t *waiter;

    if (!mutex) return false;
    while ((waiter = mutex->wait_head) != NULL) {
        mutex->wait_head = waiter->mutex_wait_next;
        if (mutex->wait_tail == waiter)
            mutex->wait_tail = NULL;
        waiter->mutex_wait_next = NULL;

        /* A waiter can be terminated by a lifecycle path after it was
         * selected but before it reached scheduler_unblock(). */
        if (waiter->waiting_mutex != mutex) {
            continue;
        }

        /* unlock() may win after the waiter registered itself but before
         * scheduler_block() acquired the scheduler lock.  Leave ownership
         * with that running waiter and publish a pending wakeup; the waiter
         * will consume it without sleeping. */
        if (waiter->state == THREAD_STATE_RUNNING) {
            mutex->owner = waiter;
            __atomic_store_n(&waiter->mutex_wake_pending, true,
                             __ATOMIC_RELEASE);
            return true;
        }

        if (waiter->state != THREAD_STATE_BLOCKED) {
            if (waiter->waiting_mutex == mutex)
                waiter->waiting_mutex = NULL;
            continue;
        }

        /* Ownership is transferred before wakeup.  The waiter recognizes
         * this handoff when scheduler_block() returns, so it cannot race a
         * second unlock into acquiring the same mutex. */
        mutex->owner = waiter;
        if (scheduler_unblock(waiter))
            return true;
        mutex->owner = NULL;
        waiter->waiting_mutex = NULL;
    }
    return false;
}

void mutex_init(mutex_t *mutex)
{
    if (!mutex) return;
    spinlock_init(&mutex->state_lock);
    mutex->owner = NULL;
    mutex->wait_head = NULL;
    mutex->wait_tail = NULL;
}

bool mutex_try_lock(mutex_t *mutex)
{
    kernel_thread_t *self;
    u64 flags;
    bool acquired = false;

    if (!mutex) return false;
    self = thread_current();
    if (!self) return false;

    flags = mutex_irq_save();
    spin_lock(&mutex->state_lock);
    if (!mutex->owner) {
        mutex->owner = self;
        acquired = true;
    }
    spin_unlock(&mutex->state_lock);
    mutex_irq_restore(flags);
    return acquired;
}

bool mutex_lock(mutex_t *mutex)
{
    kernel_thread_t *self;

    if (!mutex) return false;
    self = thread_current();
    if (!self) return false;

    for (;;) {
        u64 flags = mutex_irq_save();
        bool should_block = false;

        spin_lock(&mutex->state_lock);
        if (!mutex->owner) {
            /* An unrelated wakeup may have returned this thread to the
             * scheduler.  Remove its stale queue link before acquiring. */
            if (self->waiting_mutex == mutex) {
                mutex_waiter_remove_locked(mutex, self);
                self->waiting_mutex = NULL;
            }
            __atomic_store_n(&self->mutex_wake_pending, false,
                             __ATOMIC_RELAXED);
            mutex->owner = self;
            spin_unlock(&mutex->state_lock);
            mutex_irq_restore(flags);
            return true;
        }
        if (mutex->owner == self) {
            /* This is the ownership handoff performed by unlock(). */
            if (self->waiting_mutex == mutex) {
                self->waiting_mutex = NULL;
                __atomic_store_n(&self->mutex_wake_pending, false,
                                 __ATOMIC_RELAXED);
                spin_unlock(&mutex->state_lock);
                mutex_irq_restore(flags);
                return true;
            }
            spin_unlock(&mutex->state_lock);
            mutex_irq_restore(flags);
            return false;
        }

        if (self->waiting_mutex && self->waiting_mutex != mutex) {
            spin_unlock(&mutex->state_lock);
            mutex_irq_restore(flags);
            return false;
        }
        if (!self->waiting_mutex) {
            __atomic_store_n(&self->mutex_wake_pending, false,
                             __ATOMIC_RELAXED);
            self->waiting_mutex = mutex;
            mutex_waiter_append_locked(mutex, self);
        }
        spin_unlock(&mutex->state_lock);

        /* IF remains clear from mutex_irq_save().  The waiter is now visible
         * to unlock(), so its check/register/block transition is atomic with
         * respect to the wakeup path. */
        should_block = scheduler_block();
        if (!should_block) {
            spin_lock(&mutex->state_lock);
            mutex_waiter_remove_locked(mutex, self);
            if (self->waiting_mutex == mutex)
                self->waiting_mutex = NULL;
            __atomic_store_n(&self->mutex_wake_pending, false,
                             __ATOMIC_RELAXED);
            spin_unlock(&mutex->state_lock);
            mutex_irq_restore(flags);
            return false;
        }
        mutex_irq_restore(flags);
        /* The normal wake path transferred ownership.  Loop once to consume
         * that handoff; a spurious wake simply registers/block again. */
    }
}

bool mutex_unlock(mutex_t *mutex)
{
    kernel_thread_t *self;
    u64 flags;
    bool owned;

    if (!mutex) return false;
    self = thread_current();
    if (!self) return false;

    flags = mutex_irq_save();
    spin_lock(&mutex->state_lock);
    owned = mutex->owner == self;
    if (owned) {
        mutex->owner = NULL;
        (void)mutex_handoff_locked(mutex);
    }
    spin_unlock(&mutex->state_lock);
    mutex_irq_restore(flags);
    return owned;
}

void mutex_cancel_waiter(struct kernel_thread *thread)
{
    mutex_t *mutex;
    u64 flags;

    if (!thread) return;
    mutex = thread->waiting_mutex;
    if (!mutex) return;

    flags = mutex_irq_save();
    spin_lock(&mutex->state_lock);
    if (thread->waiting_mutex == mutex) {
        __atomic_store_n(&thread->mutex_wake_pending, false,
                         __ATOMIC_RELAXED);
        if (mutex->owner == thread) {
            mutex->owner = NULL;
            thread->waiting_mutex = NULL;
            (void)mutex_handoff_locked(mutex);
        } else {
            mutex_waiter_remove_locked(mutex, thread);
            thread->waiting_mutex = NULL;
        }
    }
    spin_unlock(&mutex->state_lock);
    mutex_irq_restore(flags);
}
