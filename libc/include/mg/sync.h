/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>
#include <stdatomic.h>
#include <stdint.h>

/*
 * Synchronization words must be naturally aligned 32-bit C atomic objects.
 * Wait is process/address-space local. It returns MG_ERR_WOULD_BLOCK when the
 * value already differs from expected at entry. A successful wait means a
 * wake occurred and the value was observed to differ; a wake with an unchanged
 * value remains a wait. It returns MG_ERR_TIMEOUT on deadline expiry,
 * MG_ERR_INTERRUPTED for Ctrl+C, and MG_ERR_SHUTDOWN during orderly shutdown.
 * Wake returns the number of waiters selected (zero is success). Callers
 * publish changes with release ordering before waking; wait observes with
 * acquire ordering. The word's mapping must remain alive for each operation.
 */
#define MG_SYNC_WAIT_FOREVER UINT64_MAX
#define MG_SYNC_WAKE_ALL UINT32_MAX

mg_result_t mg_sync_wait_u32(const volatile u32 *word, u32 expected,
                             u64 timeout_ms);
mg_result_t mg_sync_wake_u32(const volatile u32 *word, u32 max_waiters);

/* Static initialization is valid for zero-initialized storage. Destroy returns
 * MG_ERR_BUSY while an owner, waiter, or other API operation is active. After
 * successful destruction, operations return MG_ERR_BAD_ARGUMENT. Reinitialize
 * only after all users have stopped touching the object. */
typedef struct {
    atomic_uint state;
    atomic_uint users;
} mg_mutex_t;

#define MG_MUTEX_INITIALIZER \
    { ATOMIC_VAR_INIT(0U), ATOMIC_VAR_INIT(0U) }

mg_result_t mg_mutex_init(mg_mutex_t *mutex);
mg_result_t mg_mutex_destroy(mg_mutex_t *mutex);
mg_result_t mg_mutex_try_lock(mg_mutex_t *mutex);
mg_result_t mg_mutex_lock(mg_mutex_t *mutex);
mg_result_t mg_mutex_unlock(mg_mutex_t *mutex);

/* Condition waits may return spuriously; always recheck the predicate while
 * holding the mutex. The associated mutex is reacquired before a wait returns
 * normally, on timeout, or on ordinary interruption. Process-wide exit and
 * orderly system shutdown may return MG_ERR_SHUTDOWN without reacquiring it so
 * teardown cannot wait on a sibling that is also stopping.
 * Destruction returns MG_ERR_BUSY while any wait, signal, or broadcast is in
 * progress. Concurrent destruction with new operations is invalid. */
typedef struct {
    atomic_uint sequence;
    atomic_uint users;
} mg_condition_t;

#define MG_CONDITION_INITIALIZER \
    { ATOMIC_VAR_INIT(0U), ATOMIC_VAR_INIT(0U) }

mg_result_t mg_condition_init(mg_condition_t *condition);
mg_result_t mg_condition_destroy(mg_condition_t *condition);
mg_result_t mg_condition_wait(mg_condition_t *condition,
                              mg_mutex_t *mutex);
mg_result_t mg_condition_timed_wait(mg_condition_t *condition,
                                    mg_mutex_t *mutex, u64 timeout_ms);
mg_result_t mg_condition_signal(mg_condition_t *condition);
mg_result_t mg_condition_broadcast(mg_condition_t *condition);
