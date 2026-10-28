/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/sync.h>

extern long mg_syscall(unsigned long number, unsigned long arg0,
                       unsigned long arg1, unsigned long arg2);

#define SYSCALL_SYNC_WAIT_U32 106UL
#define SYSCALL_SYNC_WAKE_U32 107UL
#define MUTEX_LOCKED 1U
#define MUTEX_CONTENDED 2U
#define MUTEX_USERS_DESTROYED 0x80000000U
#define MUTEX_USER_MASK 0x7fffffffU
#define CONDITION_DESTROYED 0x80000000U
#define CONDITION_USER_MASK 0x7fffffffU

mg_result_t mg_sync_wait_u32(const volatile u32 *word, u32 expected,
                             u64 timeout_ms)
{
    if (!word || ((uintptr_t)word & (sizeof(u32) - 1U)) != 0U)
        return MG_ERR_BAD_ARGUMENT;
    return (mg_result_t)mg_syscall(SYSCALL_SYNC_WAIT_U32,
                                   (unsigned long)word, expected,
                                   (unsigned long)timeout_ms);
}

mg_result_t mg_sync_wake_u32(const volatile u32 *word, u32 max_waiters)
{
    if (!word || !max_waiters ||
        ((uintptr_t)word & (sizeof(u32) - 1U)) != 0U)
        return MG_ERR_BAD_ARGUMENT;
    return (mg_result_t)mg_syscall(SYSCALL_SYNC_WAKE_U32,
                                   (unsigned long)word, max_waiters, 0);
}

mg_result_t mg_mutex_init(mg_mutex_t *mutex)
{
    if (!mutex) return MG_ERR_BAD_ARGUMENT;
    atomic_init(&mutex->state, 0U);
    atomic_init(&mutex->users, 0U);
    return MG_OK;
}

static mg_result_t mutex_enter(mg_mutex_t *mutex)
{
    unsigned users = atomic_load_explicit(&mutex->users,
                                           memory_order_acquire);
    for (;;) {
        if ((users & MUTEX_USERS_DESTROYED) != 0U)
            return MG_ERR_BAD_ARGUMENT;
        if ((users & MUTEX_USER_MASK) == MUTEX_USER_MASK)
            return MG_ERR_BUSY;
        if (atomic_compare_exchange_weak_explicit(
                &mutex->users, &users, users + 1U,
                memory_order_acq_rel, memory_order_acquire))
            return MG_OK;
    }
}

static void mutex_leave(mg_mutex_t *mutex)
{
    (void)atomic_fetch_sub_explicit(&mutex->users, 1U, memory_order_release);
}

mg_result_t mg_mutex_destroy(mg_mutex_t *mutex)
{
    unsigned expected = 0U;
    if (!mutex) return MG_ERR_BAD_ARGUMENT;
    if (atomic_load_explicit(&mutex->state, memory_order_acquire) != 0U)
        return MG_ERR_BUSY;
    if (atomic_compare_exchange_strong_explicit(
            &mutex->users, &expected, MUTEX_USERS_DESTROYED,
            memory_order_acq_rel, memory_order_acquire))
        return MG_OK;
    return (expected & MUTEX_USERS_DESTROYED) != 0U ? MG_ERR_BAD_ARGUMENT :
                                                       MG_ERR_BUSY;
}

mg_result_t mg_mutex_try_lock(mg_mutex_t *mutex)
{
    unsigned expected = 0U;
    if (!mutex) return MG_ERR_BAD_ARGUMENT;
    mg_result_t result = mutex_enter(mutex);
    if (result != MG_OK) return result;
    if (atomic_compare_exchange_strong_explicit(
            &mutex->state, &expected, MUTEX_LOCKED,
            memory_order_acquire, memory_order_relaxed))
        return MG_OK;
    mutex_leave(mutex);
    return MG_ERR_BUSY;
}

static mg_result_t mg_mutex_lock_internal(mg_mutex_t *mutex,
                                          bool reacquire)
{
    bool contended = false;
    if (!mutex) return MG_ERR_BAD_ARGUMENT;
    mg_result_t enter_result = mutex_enter(mutex);
    if (enter_result != MG_OK) return enter_result;

    for (;;) {
        unsigned state = atomic_load_explicit(&mutex->state,
                                               memory_order_relaxed);
        if (state == 0U) {
            unsigned desired = contended ? MUTEX_CONTENDED : MUTEX_LOCKED;
            if (atomic_compare_exchange_weak_explicit(
                    &mutex->state, &state, desired,
                    memory_order_acquire, memory_order_relaxed))
                return MG_OK;
            continue;
        }

        if (state == MUTEX_LOCKED) {
            contended = true;
            unsigned expected = MUTEX_LOCKED;
            if (!atomic_compare_exchange_weak_explicit(
                    &mutex->state, &expected, MUTEX_CONTENDED,
                    memory_order_relaxed, memory_order_relaxed))
                continue;
        } else if (state != MUTEX_CONTENDED) {
            mutex_leave(mutex);
            return MG_ERR_BAD_ARGUMENT;
        } else {
            contended = true;
        }

        /* A thread that has seen contention acquires an unlocked word in
         * CONTENDED state so the next unlock continues the wake chain. */
        state = atomic_load_explicit(&mutex->state, memory_order_relaxed);
        if (state != MUTEX_CONTENDED)
            continue;

        if (reacquire) {
            /* Ctrl+C cannot abandon the condition mutex reacquire, but system
             * shutdown must be able to unwind this blocked operation. */
            mg_result_t result = mg_sync_wait_u32(
                (const volatile u32 *)&mutex->state, MUTEX_CONTENDED,
                MG_SYNC_WAIT_FOREVER);
            if (result == MG_OK || result == MG_ERR_WOULD_BLOCK ||
                result == MG_ERR_INTERRUPTED)
                continue;
            mutex_leave(mutex);
            return result;
        }

        mg_result_t result = mg_sync_wait_u32(
            (const volatile u32 *)&mutex->state, MUTEX_CONTENDED,
            MG_SYNC_WAIT_FOREVER);
        if (result == MG_OK || result == MG_ERR_WOULD_BLOCK)
            continue;
        mutex_leave(mutex);
        return result;
    }
}

mg_result_t mg_mutex_lock(mg_mutex_t *mutex)
{
    return mg_mutex_lock_internal(mutex, false);
}

mg_result_t mg_mutex_unlock(mg_mutex_t *mutex)
{
    if (!mutex) return MG_ERR_BAD_ARGUMENT;
    for (;;) {
        unsigned state = atomic_load_explicit(&mutex->state,
                                               memory_order_relaxed);
        if (state == 0U)
            return MG_ERR_BAD_ARGUMENT;
        if (state != MUTEX_LOCKED && state != MUTEX_CONTENDED)
            return MG_ERR_BAD_ARGUMENT;
        if (!atomic_compare_exchange_weak_explicit(
                &mutex->state, &state, 0U,
                memory_order_release, memory_order_relaxed))
            continue;
        if (state == MUTEX_CONTENDED) {
            mg_result_t result = mg_sync_wake_u32(
                (const volatile u32 *)&mutex->state, 1U);
            mutex_leave(mutex);
            if (result < 0) return result;
        } else {
            mutex_leave(mutex);
        }
        return MG_OK;
    }
}

static mg_result_t condition_enter(mg_condition_t *condition)
{
    if (!condition) return MG_ERR_BAD_ARGUMENT;
    unsigned state = atomic_load_explicit(&condition->users,
                                           memory_order_acquire);
    for (;;) {
        if ((state & CONDITION_DESTROYED) != 0U)
            return MG_ERR_BAD_ARGUMENT;
        if ((state & CONDITION_USER_MASK) == CONDITION_USER_MASK)
            return MG_ERR_BUSY;
        if (atomic_compare_exchange_weak_explicit(
                &condition->users, &state, state + 1U,
                memory_order_acq_rel, memory_order_acquire))
            return MG_OK;
    }
}

static void condition_leave(mg_condition_t *condition)
{
    (void)atomic_fetch_sub_explicit(&condition->users, 1U,
                                    memory_order_release);
}

mg_result_t mg_condition_init(mg_condition_t *condition)
{
    if (!condition) return MG_ERR_BAD_ARGUMENT;
    atomic_init(&condition->sequence, 0U);
    atomic_init(&condition->users, 0U);
    return MG_OK;
}

mg_result_t mg_condition_destroy(mg_condition_t *condition)
{
    unsigned expected = 0U;
    if (!condition) return MG_ERR_BAD_ARGUMENT;
    if (atomic_compare_exchange_strong_explicit(
            &condition->users, &expected, CONDITION_DESTROYED,
            memory_order_acq_rel, memory_order_acquire))
        return MG_OK;
    return (expected & CONDITION_DESTROYED) != 0U ? MG_ERR_BAD_ARGUMENT :
                                                     MG_ERR_BUSY;
}

mg_result_t mg_condition_wait(mg_condition_t *condition, mg_mutex_t *mutex)
{
    return mg_condition_timed_wait(condition, mutex, MG_SYNC_WAIT_FOREVER);
}

mg_result_t mg_condition_timed_wait(mg_condition_t *condition,
                                    mg_mutex_t *mutex, u64 timeout_ms)
{
    if (!mutex) return MG_ERR_BAD_ARGUMENT;
    mg_result_t result = condition_enter(condition);
    if (result != MG_OK) return result;

    u32 sequence = atomic_load_explicit(&condition->sequence,
                                        memory_order_acquire);
    result = mg_mutex_unlock(mutex);
    if (result != MG_OK) {
        condition_leave(condition);
        return result;
    }

    result = mg_sync_wait_u32((const volatile u32 *)&condition->sequence,
                              sequence, timeout_ms);
    if (result == MG_ERR_WOULD_BLOCK)
        result = MG_OK; /* A notification preceded waiter registration. */
    if (result == MG_ERR_SHUTDOWN) {
        /* System shutdown waits for services to leave active syscalls. Do not
         * block that transition trying to reacquire a mutex held by a sibling
         * that is also shutting down. */
        condition_leave(condition);
        return result;
    }

    mg_result_t lock_result = mg_mutex_lock_internal(mutex, false);
    if (lock_result == MG_ERR_SHUTDOWN) {
        condition_leave(condition);
        return lock_result;
    }
    if (lock_result == MG_ERR_INTERRUPTED)
        lock_result = mg_mutex_lock_internal(mutex, true);
    condition_leave(condition);
    return lock_result == MG_OK ? result : lock_result;
}

static mg_result_t condition_notify(mg_condition_t *condition, u32 count)
{
    mg_result_t result = condition_enter(condition);
    if (result != MG_OK) return result;
    (void)atomic_fetch_add_explicit(&condition->sequence, 1U,
                                    memory_order_release);
    result = mg_sync_wake_u32(
        (const volatile u32 *)&condition->sequence, count);
    condition_leave(condition);
    return result < 0 ? result : MG_OK;
}

mg_result_t mg_condition_signal(mg_condition_t *condition)
{
    return condition_notify(condition, 1U);
}

mg_result_t mg_condition_broadcast(mg_condition_t *condition)
{
    return condition_notify(condition, MG_SYNC_WAKE_ALL);
}
