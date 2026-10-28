/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <user_sync.h>

#include <process.h>
#include <scheduler.h>
#include <spinlock.h>
#include <timer.h>
#include <vmm.h>
#include <panic.h>
#include <stddef.h>

typedef enum {
    USER_SYNC_WAITER_WAITING = 0,
    USER_SYNC_WAITER_WOKEN,
} user_sync_waiter_state_t;

/* A waiter is stored on the blocked syscall's kernel stack. Its syscall
 * admission and VMM lease outlive its registration; the queue lock ensures
 * no waker retains the node after the waiter can return. */
typedef struct user_sync_waiter {
    struct user_sync_waiter *next;
    struct process *process;
    struct kernel_thread *thread;
    page_table_t *address_space;
    uintptr_t address;
    user_sync_waiter_state_t state;
    bool linked;
} user_sync_waiter_t;

static spinlock_t user_sync_lock;
static user_sync_waiter_t *user_sync_waiters;

void user_sync_init(void)
{
    spinlock_init(&user_sync_lock);
    user_sync_waiters = NULL;
}

static void user_sync_remove_locked(user_sync_waiter_t *waiter)
{
    user_sync_waiter_t **cursor = &user_sync_waiters;

    while (*cursor && *cursor != waiter)
        cursor = &(*cursor)->next;
    if (!*cursor)
        panic("user sync: registered waiter missing from queue");
    *cursor = waiter->next;
    waiter->next = NULL;
    waiter->linked = false;
}

static bool user_sync_wait_interrupted(struct process *process,
                                      mg_result_t *out_result)
{
    if (process_shutdown_requested(process)) {
        *out_result = MG_ERR_SHUTDOWN;
        return true;
    }
    if (process_take_interrupt(process)) {
        *out_result = MG_ERR_INTERRUPTED;
        return true;
    }
    return false;
}

static void user_sync_finish_waiter(user_sync_waiter_t *waiter,
                                    vmm_user_range_lease_t *lease)
{
    u64 lock_flags = spin_lock_irqsave(&user_sync_lock);

    if (waiter->linked)
        user_sync_remove_locked(waiter);
    spin_unlock_irqrestore(&user_sync_lock, lock_flags);

    /* Pending event state is a notification, not the durable wake condition;
     * the waiter's state was consumed above. */
    scheduler_clear_event_wake(waiter->thread);
    vmm_user_range_release(lease);
}

mg_result_t user_sync_wait_u32(struct process *process,
                               struct kernel_thread *thread,
                               volatile u32 *word, u32 expected,
                               u64 timeout_ms)
{
    user_sync_waiter_t waiter = {0};
    vmm_user_range_lease_t lease = {0};
    uintptr_t address = (uintptr_t)word;
    u64 deadline = 0;
    bool timed = timeout_ms != USER_SYNC_WAIT_FOREVER;
    mg_result_t result = MG_OK;

    if (!process || !thread || thread->process != process || !word ||
        (address & (sizeof(u32) - 1U)) != 0U)
        return MG_ERR_BAD_ARGUMENT;
    if (!vmm_user_range_lease(process->address_space, (const void *)word,
                              sizeof(*word), false, &lease))
        return MG_ERR_BAD_ARGUMENT;
    if (user_sync_wait_interrupted(process, &result)) {
        vmm_user_range_release(&lease);
        return result;
    }

    waiter.process = process;
    waiter.thread = thread;
    waiter.address_space = process->address_space;
    waiter.address = address;
    waiter.state = USER_SYNC_WAITER_WAITING;

    u64 lock_flags = spin_lock_irqsave(&user_sync_lock);
    if (__atomic_load_n((u32 *)(uintptr_t)word, __ATOMIC_ACQUIRE) != expected) {
        spin_unlock_irqrestore(&user_sync_lock, lock_flags);
        vmm_user_range_release(&lease);
        return MG_ERR_WOULD_BLOCK;
    }
    if (timed && timeout_ms == 0U) {
        spin_unlock_irqrestore(&user_sync_lock, lock_flags);
        vmm_user_range_release(&lease);
        return MG_ERR_TIMEOUT;
    }
    waiter.next = user_sync_waiters;
    user_sync_waiters = &waiter;
    waiter.linked = true;
    spin_unlock_irqrestore(&user_sync_lock, lock_flags);

    if (timed) {
        u64 now = timer_uptime_ms();
        deadline = now > (~(u64)0 - timeout_ms) ? ~(u64)0 : now + timeout_ms;
    }

    for (;;) {
        if (user_sync_wait_interrupted(process, &result)) {
            user_sync_finish_waiter(&waiter, &lease);
            return result;
        }

        u64 now = timed ? timer_uptime_ms() : 0U;
        lock_flags = spin_lock_irqsave(&user_sync_lock);
        if (waiter.state == USER_SYNC_WAITER_WOKEN) {
            if (__atomic_load_n((u32 *)(uintptr_t)word,
                                __ATOMIC_ACQUIRE) != expected) {
                spin_unlock_irqrestore(&user_sync_lock, lock_flags);
                user_sync_finish_waiter(&waiter, &lease);
                return MG_OK;
            }

            /* Wakes are notifications, not proof that the value changed.
             * Re-register under the same lock used by wake so an unchanged
             * word cannot cause a false successful return or lose a later
             * notification. */
            waiter.state = USER_SYNC_WAITER_WAITING;
            waiter.next = user_sync_waiters;
            user_sync_waiters = &waiter;
            waiter.linked = true;
        }
        if (timed && now >= deadline) {
            user_sync_remove_locked(&waiter);
            waiter.linked = false;
            spin_unlock_irqrestore(&user_sync_lock, lock_flags);
            user_sync_finish_waiter(&waiter, &lease);
            return MG_ERR_TIMEOUT;
        }
        spin_unlock_irqrestore(&user_sync_lock, lock_flags);

        bool blocked;
        if (!timed) {
            blocked = scheduler_block();
        } else {
            u64 remaining = deadline - now;
            blocked = scheduler_sleep(remaining);
        }
        if (!blocked) {
            /* A wake may have won while scheduler_block was validating the
             * current context. Resolve that race under the wait-list lock. */
            lock_flags = spin_lock_irqsave(&user_sync_lock);
            bool woken = waiter.state == USER_SYNC_WAITER_WOKEN;
            if (!woken)
                user_sync_remove_locked(&waiter);
            if (!woken)
                waiter.linked = false;
            spin_unlock_irqrestore(&user_sync_lock, lock_flags);
            if (woken) {
                user_sync_finish_waiter(&waiter, &lease);
                return MG_OK;
            }
            scheduler_clear_event_wake(thread);
            vmm_user_range_release(&lease);
            return MG_ERR_BUSY;
        }
    }
}

mg_result_t user_sync_wake_u32(struct process *process,
                               volatile u32 *word, u32 max_waiters)
{
    vmm_user_range_lease_t lease = {0};
    uintptr_t address = (uintptr_t)word;
    u32 woken = 0;

    if (!process || !word || max_waiters == 0U ||
        (address & (sizeof(u32) - 1U)) != 0U)
        return MG_ERR_BAD_ARGUMENT;
    if (!vmm_user_range_lease(process->address_space, (const void *)word,
                              sizeof(*word), false, &lease))
        return MG_ERR_BAD_ARGUMENT;

    u64 lock_flags = spin_lock_irqsave(&user_sync_lock);
    user_sync_waiter_t *waiter = user_sync_waiters;
    while (waiter && woken < max_waiters) {
        user_sync_waiter_t *next = waiter->next;
        if (waiter->address_space == process->address_space &&
            waiter->address == address &&
            waiter->state == USER_SYNC_WAITER_WAITING) {
            user_sync_remove_locked(waiter);
            waiter->state = USER_SYNC_WAITER_WOKEN;
            if (!scheduler_wake_waiter(waiter->thread))
                panic("user sync: failed to wake registered thread");
            woken++;
        }
        waiter = next;
    }
    spin_unlock_irqrestore(&user_sync_lock, lock_flags);
    vmm_user_range_release(&lease);
    return (mg_result_t)woken;
}
