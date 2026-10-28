/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <mg/sync.h>
#include <user_sync.h>

#define MUTEX_ITERATIONS 2000U

static _Thread_local process_t *current_process;
static _Thread_local kernel_thread_t *current_thread;
static pthread_mutex_t park_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t park_cond = PTHREAD_COND_INITIALIZER;
static unsigned park_calls;
static unsigned parked_threads;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool gate_enabled;
static bool gate_entered;
static bool gate_released;
static struct timespec clock_origin;

static void fail_pthread(int result)
{
    if (result != 0) {
        fprintf(stderr, "pthread failure: %d\n", result);
        abort();
    }
}

void spinlock_init(spinlock_t *lock)
{
    fail_pthread(pthread_mutex_init(lock, NULL));
}

u64 spin_lock_irqsave(spinlock_t *lock)
{
    fail_pthread(pthread_mutex_lock(lock));
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    fail_pthread(pthread_mutex_unlock(lock));
}

void panic(const char *message)
{
    fprintf(stderr, "unexpected kernel panic: %s\n", message);
    abort();
}

static void park_begin(void)
{
    fail_pthread(pthread_mutex_lock(&park_lock));
    park_calls++;
    parked_threads++;
    fail_pthread(pthread_cond_broadcast(&park_cond));
    fail_pthread(pthread_mutex_unlock(&park_lock));
}

static void park_end(void)
{
    fail_pthread(pthread_mutex_lock(&park_lock));
    assert(parked_threads > 0U);
    parked_threads--;
    fail_pthread(pthread_cond_broadcast(&park_cond));
    fail_pthread(pthread_mutex_unlock(&park_lock));
}

bool scheduler_block(void)
{
    kernel_thread_t *thread = current_thread;
    fail_pthread(pthread_mutex_lock(&thread->event_lock));
    if (thread->event_pending) {
        thread->event_pending = false;
        fail_pthread(pthread_mutex_unlock(&thread->event_lock));
        return true;
    }
    park_begin();
    while (!thread->event_pending)
        fail_pthread(pthread_cond_wait(&thread->event_cond,
                                       &thread->event_lock));
    thread->event_pending = false;
    fail_pthread(pthread_mutex_unlock(&thread->event_lock));
    park_end();
    return true;
}

bool scheduler_sleep(u64 milliseconds)
{
    kernel_thread_t *thread = current_thread;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(milliseconds / 1000U);
    deadline.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    fail_pthread(pthread_mutex_lock(&thread->event_lock));
    if (thread->event_pending) {
        thread->event_pending = false;
        fail_pthread(pthread_mutex_unlock(&thread->event_lock));
        return true;
    }
    park_begin();
    int wait_result = 0;
    while (!thread->event_pending && wait_result != ETIMEDOUT)
        wait_result = pthread_cond_timedwait(&thread->event_cond,
                                             &thread->event_lock, &deadline);
    assert(wait_result == 0 || wait_result == ETIMEDOUT);
    thread->event_pending = false;
    fail_pthread(pthread_mutex_unlock(&thread->event_lock));
    park_end();
    return true;
}

bool scheduler_wake_waiter(kernel_thread_t *thread)
{
    fail_pthread(pthread_mutex_lock(&thread->event_lock));
    thread->event_pending = true;
    fail_pthread(pthread_cond_signal(&thread->event_cond));
    fail_pthread(pthread_mutex_unlock(&thread->event_lock));
    return true;
}

void scheduler_clear_event_wake(kernel_thread_t *thread)
{
    fail_pthread(pthread_mutex_lock(&thread->event_lock));
    thread->event_pending = false;
    fail_pthread(pthread_mutex_unlock(&thread->event_lock));
}

u64 timer_uptime_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    u64 sec = (u64)(now.tv_sec - clock_origin.tv_sec);
    i64 ns = (i64)now.tv_nsec - (i64)clock_origin.tv_nsec;
    if (ns < 0) {
        sec--;
        ns += 1000000000LL;
    }
    return sec * 1000U + (u64)ns / 1000000U;
}

bool process_shutdown_requested(const process_t *process)
{
    return __atomic_load_n(&process->shutdown_requested, __ATOMIC_ACQUIRE);
}

bool process_take_interrupt(process_t *process)
{
    if (__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE))
        return true;
    return __atomic_exchange_n(&process->interrupt_requested, false,
                               __ATOMIC_ACQ_REL);
}

bool vmm_user_range_lease(page_table_t *address_space, const void *address,
                          usize length, bool writable,
                          vmm_user_range_lease_t *out_lease)
{
    (void)writable;
    if (!address_space || !address || !out_lease || length != sizeof(u32))
        return false;
    process_t *process = address_space->owner;
    fail_pthread(pthread_mutex_lock(&process->mapping_lock));
    bool valid = process->mapped && (uintptr_t)address != 0xdead0000U;
    if (valid) {
        process->active_leases++;
        out_lease->address_space = address_space;
        out_lease->active = true;
    }
    fail_pthread(pthread_mutex_unlock(&process->mapping_lock));
    return valid;
}

void vmm_user_range_release(vmm_user_range_lease_t *lease)
{
    if (!lease || !lease->active) return;
    process_t *process = lease->address_space->owner;
    fail_pthread(pthread_mutex_lock(&process->mapping_lock));
    assert(process->active_leases > 0U);
    process->active_leases--;
    lease->active = false;
    fail_pthread(pthread_mutex_unlock(&process->mapping_lock));
}

static bool fixture_unmap(process_t *process)
{
    fail_pthread(pthread_mutex_lock(&process->mapping_lock));
    bool unmapped = process->active_leases == 0U;
    if (unmapped) process->mapped = false;
    fail_pthread(pthread_mutex_unlock(&process->mapping_lock));
    return unmapped;
}

static void process_fixture_init(process_t *process, volatile u32 *word)
{
    *process = (process_t){0};
    process->address_space = &process->address_space_record;
    process->address_space_record.owner = process;
    process->mapped_word = word;
    process->mapped = true;
    fail_pthread(pthread_mutex_init(&process->mapping_lock, NULL));
}

static void thread_fixture_init(kernel_thread_t *thread, process_t *process)
{
    *thread = (kernel_thread_t){.process = process};
    fail_pthread(pthread_mutex_init(&thread->event_lock, NULL));
    pthread_condattr_t attr;
    fail_pthread(pthread_condattr_init(&attr));
    fail_pthread(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    fail_pthread(pthread_cond_init(&thread->event_cond, &attr));
    fail_pthread(pthread_condattr_destroy(&attr));
}

static void thread_fixture_destroy(kernel_thread_t *thread)
{
    fail_pthread(pthread_cond_destroy(&thread->event_cond));
    fail_pthread(pthread_mutex_destroy(&thread->event_lock));
}

static void wait_for_park_calls(unsigned target)
{
    fail_pthread(pthread_mutex_lock(&park_lock));
    while (park_calls < target)
        fail_pthread(pthread_cond_wait(&park_cond, &park_lock));
    fail_pthread(pthread_mutex_unlock(&park_lock));
}

static void wait_for_parked_threads(unsigned target)
{
    fail_pthread(pthread_mutex_lock(&park_lock));
    while (parked_threads < target)
        fail_pthread(pthread_cond_wait(&park_cond, &park_lock));
    fail_pthread(pthread_mutex_unlock(&park_lock));
}

static void current_context(process_t *process, kernel_thread_t *thread)
{
    current_process = process;
    current_thread = thread;
}

long mg_syscall(unsigned long number, unsigned long arg0,
                unsigned long arg1, unsigned long arg2)
{
    if (number == 106UL) {
        fail_pthread(pthread_mutex_lock(&gate_lock));
        if (gate_enabled) {
            gate_entered = true;
            fail_pthread(pthread_cond_broadcast(&gate_cond));
            while (!gate_released)
                fail_pthread(pthread_cond_wait(&gate_cond, &gate_lock));
            gate_enabled = false;
        }
        fail_pthread(pthread_mutex_unlock(&gate_lock));
        return (long)user_sync_wait_u32(
            current_process, current_thread, (volatile u32 *)(uintptr_t)arg0,
            (u32)arg1, (u64)arg2);
    }
    if (number == 107UL)
        return (long)user_sync_wake_u32(
            current_process, (volatile u32 *)(uintptr_t)arg0, (u32)arg1);
    abort();
}

mg_result_t process_yield(void)
{
    sched_yield();
    return MG_OK;
}

typedef struct {
    process_t *process;
    kernel_thread_t thread;
    const volatile u32 *word;
    u32 expected;
    u64 timeout;
    mg_result_t result;
    atomic_bool done;
} wait_task_t;

static void *wait_task_entry(void *opaque)
{
    wait_task_t *task = opaque;
    current_context(task->process, &task->thread);
    task->result = mg_sync_wait_u32(task->word, task->expected,
                                    task->timeout);
    atomic_store_explicit(&task->done, true, memory_order_release);
    return NULL;
}

static void wait_task_init(wait_task_t *task, process_t *process,
                           const volatile u32 *word, u32 expected,
                           u64 timeout)
{
    *task = (wait_task_t){.process = process, .word = word,
                          .expected = expected, .timeout = timeout};
    atomic_init(&task->done, false);
    thread_fixture_init(&task->thread, process);
}

static void wait_task_destroy(wait_task_t *task)
{
    assert(task->process->active_leases == 0U);
    thread_fixture_destroy(&task->thread);
}

static void test_address_wait_contract(void)
{
    _Atomic u32 word = 4U;
    process_t process;
    process_fixture_init(&process, (volatile u32 *)&word);
    kernel_thread_t main_thread;
    thread_fixture_init(&main_thread, &process);
    current_context(&process, &main_thread);

    assert(mg_sync_wait_u32((volatile u32 *)&word, 5U, 5U) ==
           MG_ERR_WOULD_BLOCK);
    assert(mg_sync_wait_u32((volatile u32 *)&word, 4U, 0U) ==
           MG_ERR_TIMEOUT);
    assert(mg_sync_wait_u32((volatile u32 *)((uintptr_t)&word + 1U), 4U,
                            0U) == MG_ERR_BAD_ARGUMENT);
    assert(mg_sync_wait_u32((volatile u32 *)(uintptr_t)0xdead0000U, 4U,
                            0U) == MG_ERR_BAD_ARGUMENT);
    assert(mg_sync_wake_u32((volatile u32 *)&word, 0U) ==
           MG_ERR_BAD_ARGUMENT);
    assert(process.active_leases == 0U);
    assert(mg_sync_wake_u32((volatile u32 *)&word, 1U) == 0);

    /* A wake before registration is not queued. The changed value makes the
     * following expected-value check fail instead of sleeping indefinitely. */
    atomic_store_explicit(&word, 9U, memory_order_release);
    assert(mg_sync_wake_u32((volatile u32 *)&word, 1U) == 0);
    assert(mg_sync_wait_u32((volatile u32 *)&word, 4U,
                            MG_SYNC_WAIT_FOREVER) == MG_ERR_WOULD_BLOCK);

    thread_fixture_destroy(&main_thread);
    fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
}

static void test_wait_wake_and_lease(void)
{
    _Atomic u32 word = 17U;
    process_t process;
    process_fixture_init(&process, (volatile u32 *)&word);
    wait_task_t task;
    wait_task_init(&task, &process, (volatile u32 *)&word, 17U,
                   MG_SYNC_WAIT_FOREVER);
    pthread_t host_thread;
    unsigned calls_before = park_calls;
    fail_pthread(pthread_create(&host_thread, NULL, wait_task_entry, &task));
    wait_for_park_calls(calls_before + 1U);
    wait_for_parked_threads(1U);
    assert(process.active_leases == 1U);
    assert(!fixture_unmap(&process));

    /* Explicit wake is only a notification. If the word is unchanged, the
     * kernel must atomically register the waiter again rather than reporting
     * a false value change. */
    current_context(&process, NULL);
    assert(mg_sync_wake_u32((volatile u32 *)&word, 1U) == 1);
    wait_for_park_calls(calls_before + 2U);
    assert(!atomic_load_explicit(&task.done, memory_order_acquire));

    atomic_store_explicit(&word, 18U, memory_order_release);
    assert(mg_sync_wake_u32((volatile u32 *)&word, 1U) == 1);
    fail_pthread(pthread_join(host_thread, NULL));
    assert(task.result == MG_OK && atomic_load(&task.done));
    assert(process.active_leases == 0U);
    assert(fixture_unmap(&process));
    wait_task_destroy(&task);
    fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
}

static void test_multiple_waiters_and_address_space_key(void)
{
    _Atomic u32 word = 1U;
    process_t first, second;
    process_fixture_init(&first, (volatile u32 *)&word);
    process_fixture_init(&second, (volatile u32 *)&word);
    wait_task_t tasks[3];
    pthread_t host_threads[3];
    unsigned calls_before = park_calls;
    for (unsigned i = 0; i < 3U; i++) {
        wait_task_init(&tasks[i], i == 2U ? &second : &first,
                       (volatile u32 *)&word, 1U, MG_SYNC_WAIT_FOREVER);
        fail_pthread(pthread_create(&host_threads[i], NULL,
                                    wait_task_entry, &tasks[i]));
    }
    wait_for_park_calls(calls_before + 3U);
    wait_for_parked_threads(3U);
    atomic_store_explicit(&word, 2U, memory_order_release);
    current_context(&first, NULL);
    assert(mg_sync_wake_u32((volatile u32 *)&word, 1U) == 1);

    /* The same virtual address in another address space is a distinct key. */
    current_context(&second, NULL);
    assert(mg_sync_wake_u32((volatile u32 *)&word, 1U) == 1);
    current_context(&first, NULL);
    assert(mg_sync_wake_u32((volatile u32 *)&word, MG_SYNC_WAKE_ALL) == 1);
    for (unsigned i = 0; i < 3U; i++) {
        fail_pthread(pthread_join(host_threads[i], NULL));
        assert(tasks[i].result == MG_OK);
        wait_task_destroy(&tasks[i]);
    }
    assert(first.active_leases == 0U && second.active_leases == 0U);
    fail_pthread(pthread_mutex_destroy(&first.mapping_lock));
    fail_pthread(pthread_mutex_destroy(&second.mapping_lock));
}

static void test_spurious_timeout_and_interruption(void)
{
    _Atomic u32 word = 3U;
    process_t process;
    process_fixture_init(&process, (volatile u32 *)&word);
    wait_task_t task;
    wait_task_init(&task, &process, (volatile u32 *)&word, 3U,
                   MG_SYNC_WAIT_FOREVER);
    pthread_t host_thread;
    unsigned calls_before = park_calls;
    fail_pthread(pthread_create(&host_thread, NULL, wait_task_entry, &task));
    wait_for_park_calls(calls_before + 1U);
    wait_for_parked_threads(1U);
    assert(scheduler_wake_waiter(&task.thread)); /* spurious scheduler event */
    wait_for_park_calls(calls_before + 2U);
    assert(!atomic_load_explicit(&task.done, memory_order_acquire));
    __atomic_store_n(&process.interrupt_requested, true, __ATOMIC_RELEASE);
    assert(scheduler_wake_waiter(&task.thread));
    fail_pthread(pthread_join(host_thread, NULL));
    assert(task.result == MG_ERR_INTERRUPTED);
    assert(process.active_leases == 0U);
    wait_task_destroy(&task);

    wait_task_t timed;
    wait_task_init(&timed, &process, (volatile u32 *)&word, 3U, 15U);
    current_context(&process, &timed.thread);
    assert(mg_sync_wait_u32((volatile u32 *)&word, 3U, 10U) ==
           MG_ERR_TIMEOUT);
    assert(process.active_leases == 0U);
    wait_task_destroy(&timed);

    wait_task_t shutdown;
    wait_task_init(&shutdown, &process, (volatile u32 *)&word, 3U,
                   MG_SYNC_WAIT_FOREVER);
    calls_before = park_calls;
    fail_pthread(pthread_create(&host_thread, NULL, wait_task_entry,
                                &shutdown));
    wait_for_park_calls(calls_before + 1U);
    wait_for_parked_threads(1U);
    __atomic_store_n(&process.shutdown_requested, true, __ATOMIC_RELEASE);
    assert(scheduler_wake_waiter(&shutdown.thread));
    fail_pthread(pthread_join(host_thread, NULL));
    assert(shutdown.result == MG_ERR_SHUTDOWN);
    assert(process.active_leases == 0U);
    wait_task_destroy(&shutdown);

    wait_task_t exiting;
    wait_task_init(&exiting, &process, (volatile u32 *)&word, 3U,
                   MG_SYNC_WAIT_FOREVER);
    __atomic_store_n(&process.shutdown_requested, false, __ATOMIC_RELEASE);
    calls_before = park_calls;
    fail_pthread(pthread_create(&host_thread, NULL, wait_task_entry, &exiting));
    wait_for_park_calls(calls_before + 1U);
    wait_for_parked_threads(1U);
    __atomic_store_n(&process.exit_requested, true, __ATOMIC_RELEASE);
    assert(scheduler_wake_waiter(&exiting.thread));
    fail_pthread(pthread_join(host_thread, NULL));
    assert(exiting.result == MG_ERR_INTERRUPTED);
    assert(process.active_leases == 0U);
    wait_task_destroy(&exiting);
    fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
}

typedef struct {
    process_t *process;
    kernel_thread_t thread;
    mg_mutex_t *mutex;
    unsigned *counter;
} mutex_task_t;

static void *mutex_worker(void *opaque)
{
    mutex_task_t *task = opaque;
    current_context(task->process, &task->thread);
    for (unsigned i = 0; i < MUTEX_ITERATIONS; i++) {
        assert(mg_mutex_lock(task->mutex) == MG_OK);
        (*task->counter)++;
        assert(mg_mutex_unlock(task->mutex) == MG_OK);
    }
    return NULL;
}

static void test_mutex_contention(void)
{
    enum { WORKER_COUNT = 5U };
    _Atomic u32 word = 0U;
    process_t process;
    process_fixture_init(&process, (volatile u32 *)&word);
    kernel_thread_t main_thread;
    thread_fixture_init(&main_thread, &process);
    current_context(&process, &main_thread);
    mg_mutex_t mutex = MG_MUTEX_INITIALIZER;
    unsigned counter = 0U;
    assert(mg_mutex_try_lock(&mutex) == MG_OK);
    assert(mg_mutex_try_lock(&mutex) == MG_ERR_BUSY);
    assert(mg_mutex_destroy(&mutex) == MG_ERR_BUSY);

    mutex_task_t tasks[WORKER_COUNT];
    pthread_t host_threads[WORKER_COUNT];
    unsigned calls_before = park_calls;
    for (unsigned i = 0; i < WORKER_COUNT; i++) {
        tasks[i] = (mutex_task_t){.process = &process, .mutex = &mutex,
                                  .counter = &counter};
        thread_fixture_init(&tasks[i].thread, &process);
        fail_pthread(pthread_create(&host_threads[i], NULL,
                                    mutex_worker, &tasks[i]));
    }
    wait_for_park_calls(calls_before + WORKER_COUNT);
    wait_for_parked_threads(WORKER_COUNT);
    assert(mg_mutex_destroy(&mutex) == MG_ERR_BUSY);
    assert(mg_mutex_unlock(&mutex) == MG_OK);
    for (unsigned i = 0; i < WORKER_COUNT; i++) {
        fail_pthread(pthread_join(host_threads[i], NULL));
        thread_fixture_destroy(&tasks[i].thread);
    }
    assert(counter == WORKER_COUNT * MUTEX_ITERATIONS);
    assert(mg_mutex_destroy(&mutex) == MG_OK);
    assert(mg_mutex_lock(&mutex) == MG_ERR_BAD_ARGUMENT);
    thread_fixture_destroy(&main_thread);
    assert(process.active_leases == 0U);
    fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
}

typedef struct {
    process_t *process;
    kernel_thread_t thread;
    mg_mutex_t *mutex;
    mg_condition_t *condition;
    bool *predicate;
    mg_result_t result;
    bool mutex_reacquired;
} condition_task_t;

static void *condition_worker(void *opaque)
{
    condition_task_t *task = opaque;
    current_context(task->process, &task->thread);
    assert(mg_mutex_lock(task->mutex) == MG_OK);
    while (!*task->predicate) {
        task->result = mg_condition_wait(task->condition, task->mutex);
        assert(task->result == MG_OK);
    }
    task->mutex_reacquired = mg_mutex_try_lock(task->mutex) == MG_ERR_BUSY;
    assert(mg_mutex_unlock(task->mutex) == MG_OK);
    return NULL;
}

static void *shutdown_condition_worker(void *opaque)
{
    condition_task_t *task = opaque;
    current_context(task->process, &task->thread);
    assert(mg_mutex_lock(task->mutex) == MG_OK);
    task->result = mg_condition_wait(task->condition, task->mutex);
    mg_result_t probe = mg_mutex_try_lock(task->mutex);
    task->mutex_reacquired = probe == MG_ERR_BUSY;
    if (probe == MG_OK)
        assert(mg_mutex_unlock(task->mutex) == MG_OK);
    else
        assert(probe == MG_ERR_BUSY);
    return NULL;
}

static void test_condition_notification_gap(void)
{
    _Atomic u32 word = 0U;
    process_t process;
    process_fixture_init(&process, (volatile u32 *)&word);
    kernel_thread_t main_thread;
    thread_fixture_init(&main_thread, &process);
    current_context(&process, &main_thread);
    mg_mutex_t mutex = MG_MUTEX_INITIALIZER;
    mg_condition_t condition = MG_CONDITION_INITIALIZER;
    bool predicate = false;
    condition_task_t task = {.process = &process, .mutex = &mutex,
                             .condition = &condition,
                             .predicate = &predicate};
    thread_fixture_init(&task.thread, &process);
    pthread_t host_thread;

    fail_pthread(pthread_mutex_lock(&gate_lock));
    gate_enabled = true;
    gate_entered = false;
    gate_released = false;
    fail_pthread(pthread_mutex_unlock(&gate_lock));
    fail_pthread(pthread_create(&host_thread, NULL, condition_worker, &task));

    fail_pthread(pthread_mutex_lock(&gate_lock));
    while (!gate_entered)
        fail_pthread(pthread_cond_wait(&gate_cond, &gate_lock));
    fail_pthread(pthread_mutex_unlock(&gate_lock));

    /* The waiter has released the mutex but has not entered the kernel wait.
     * This notification must be observed by the sequence check, not lost. */
    assert(mg_mutex_lock(&mutex) == MG_OK);
    predicate = true;
    assert(mg_condition_signal(&condition) == MG_OK);
    assert(mg_mutex_unlock(&mutex) == MG_OK);
    fail_pthread(pthread_mutex_lock(&gate_lock));
    gate_released = true;
    fail_pthread(pthread_cond_broadcast(&gate_cond));
    fail_pthread(pthread_mutex_unlock(&gate_lock));

    fail_pthread(pthread_join(host_thread, NULL));
    assert(task.result == MG_OK && task.mutex_reacquired);
    assert(mg_condition_destroy(&condition) == MG_OK);
    assert(mg_condition_signal(&condition) == MG_ERR_BAD_ARGUMENT);
    assert(mg_mutex_destroy(&mutex) == MG_OK);
    thread_fixture_destroy(&task.thread);
    thread_fixture_destroy(&main_thread);
    fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
}

typedef struct {
    process_t *process;
    kernel_thread_t thread;
    mg_mutex_t *mutex;
    mg_condition_t *condition;
    bool *predicate;
    mg_result_t result;
    bool reacquired;
} timed_condition_task_t;

static void *timed_condition_worker(void *opaque)
{
    timed_condition_task_t *task = opaque;
    current_context(task->process, &task->thread);
    assert(mg_mutex_lock(task->mutex) == MG_OK);
    task->result = mg_condition_timed_wait(task->condition, task->mutex, 15U);
    task->reacquired = mg_mutex_try_lock(task->mutex) == MG_ERR_BUSY;
    assert(mg_mutex_unlock(task->mutex) == MG_OK);
    return NULL;
}

static void test_condition_timeout_and_broadcast(void)
{
    _Atomic u32 word = 0U;
    process_t process;
    process_fixture_init(&process, (volatile u32 *)&word);
    kernel_thread_t main_thread;
    thread_fixture_init(&main_thread, &process);
    current_context(&process, &main_thread);
    mg_mutex_t mutex = MG_MUTEX_INITIALIZER;
    mg_condition_t condition = MG_CONDITION_INITIALIZER;
    bool predicate = false;
    timed_condition_task_t timed = {.process = &process, .mutex = &mutex,
                                    .condition = &condition,
                                    .predicate = &predicate};
    thread_fixture_init(&timed.thread, &process);
    pthread_t host_thread;
    fail_pthread(pthread_create(&host_thread, NULL,
                                timed_condition_worker, &timed));
    fail_pthread(pthread_join(host_thread, NULL));
    assert(timed.result == MG_ERR_TIMEOUT && timed.reacquired);
    thread_fixture_destroy(&timed.thread);

    enum { WAITERS = 3U };
    condition_task_t tasks[WAITERS];
    pthread_t waiters[WAITERS];
    unsigned calls_before = park_calls;
    for (unsigned i = 0; i < WAITERS; i++) {
        tasks[i] = (condition_task_t){.process = &process, .mutex = &mutex,
                    .condition = &condition, .predicate = &predicate};
        thread_fixture_init(&tasks[i].thread, &process);
        fail_pthread(pthread_create(&waiters[i], NULL,
                                    condition_worker, &tasks[i]));
    }
    wait_for_park_calls(calls_before + WAITERS);
    wait_for_parked_threads(WAITERS);
    assert(mg_condition_destroy(&condition) == MG_ERR_BUSY);
    assert(mg_mutex_lock(&mutex) == MG_OK);
    predicate = true;
    assert(mg_condition_broadcast(&condition) == MG_OK);
    assert(mg_mutex_unlock(&mutex) == MG_OK);
    for (unsigned i = 0; i < WAITERS; i++) {
        fail_pthread(pthread_join(waiters[i], NULL));
        assert(tasks[i].result == MG_OK && tasks[i].mutex_reacquired);
        thread_fixture_destroy(&tasks[i].thread);
    }
    assert(mg_condition_destroy(&condition) == MG_OK);
    assert(mg_condition_broadcast(&condition) == MG_ERR_BAD_ARGUMENT);
    assert(mg_mutex_destroy(&mutex) == MG_OK);
    thread_fixture_destroy(&main_thread);
    assert(process.active_leases == 0U);
    fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
}

static void test_condition_shutdown_unwinds(void)
{
    _Atomic u32 word = 0U;
    process_t process;
    process_fixture_init(&process, (volatile u32 *)&word);
    mg_mutex_t mutex = MG_MUTEX_INITIALIZER;
    mg_condition_t condition = MG_CONDITION_INITIALIZER;
    condition_task_t task = {.process = &process, .mutex = &mutex,
                             .condition = &condition};
    thread_fixture_init(&task.thread, &process);
    pthread_t host_thread;
    unsigned calls_before = park_calls;
    fail_pthread(pthread_create(&host_thread, NULL,
                                shutdown_condition_worker, &task));
    wait_for_park_calls(calls_before + 1U);
    wait_for_parked_threads(1U);

    __atomic_store_n(&process.shutdown_requested, true, __ATOMIC_RELEASE);
    assert(scheduler_wake_waiter(&task.thread));
    fail_pthread(pthread_join(host_thread, NULL));
    assert(task.result == MG_ERR_SHUTDOWN);
    assert(!task.mutex_reacquired);
    assert(process.active_leases == 0U);
    assert(mg_condition_destroy(&condition) == MG_OK);
    assert(mg_mutex_destroy(&mutex) == MG_OK);
    thread_fixture_destroy(&task.thread);
    fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
}

static void test_concurrent_timeout_wake_races(void)
{
    for (unsigned iteration = 0; iteration < 64U; iteration++) {
        _Atomic u32 word = 1U;
        process_t process;
        process_fixture_init(&process, (volatile u32 *)&word);
        wait_task_t task;
        wait_task_init(&task, &process, (volatile u32 *)&word, 1U, 2U);
        pthread_t host_thread;
        fail_pthread(pthread_create(&host_thread, NULL,
                                    wait_task_entry, &task));
        /* A simultaneous wake can win or lose the monotonic deadline. */
        atomic_store_explicit(&word, 2U, memory_order_release);
        current_context(&process, NULL);
        (void)mg_sync_wake_u32((volatile u32 *)&word, 1U);
        fail_pthread(pthread_join(host_thread, NULL));
        assert(task.result == MG_OK || task.result == MG_ERR_TIMEOUT ||
               task.result == MG_ERR_WOULD_BLOCK);
        assert(process.active_leases == 0U);
        wait_task_destroy(&task);
        fail_pthread(pthread_mutex_destroy(&process.mapping_lock));
    }
}

int main(void)
{
    clock_gettime(CLOCK_MONOTONIC, &clock_origin);
    user_sync_init();
    test_address_wait_contract();
    test_wait_wake_and_lease();
    test_multiple_waiters_and_address_space_key();
    test_spurious_timeout_and_interruption();
    test_mutex_contention();
    test_condition_notification_gap();
    test_condition_timeout_and_broadcast();
    test_condition_shutdown_unwinds();
    test_concurrent_timeout_wake_races();
    puts("Mangrove userspace sync production-source tests passed");
    return 0;
}
