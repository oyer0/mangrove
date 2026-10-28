/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native Ring 3 qualification for Mangrove's worker lifecycle API. */
#include <mangrove.h>
#include <mg/thread.h>
#include <mg/sync.h>
#include <mg/inspection.h>
#include <mg/memory.h>
#include <mg/process_status.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKERS 2U
#define ITERATIONS 2000U
#define REPEAT_WORKERS 24U
#define REQUIRE(condition, stage) do { if (!(condition)) { \
    printf("THREADS1 FAIL %s\n", stage); return 1; } } while (0)

typedef struct {
    unsigned index;
    mg_thread_id_t id;
    int observed_errno;
    unsigned observed_tls;
    mg_thread_t self_handle;
    mg_result_t self_join_result;
} worker_arg_t;

static mg_mutex_t gate_mutex = MG_MUTEX_INITIALIZER;
static mg_condition_t gate_condition = MG_CONDITION_INITIALIZER;
static unsigned arrived;
static bool release_workers;
static unsigned mutex_total;
static atomic_uint atomic_total;
static atomic_uint cpu_overlap_seen;
static atomic_uint cpu_witness_workers;
static atomic_uint wait_ready;
static atomic_uint termination_wait_ready;
static _Thread_local unsigned worker_tls;

extern u64 threadtest_syscall_with_rsp(uintptr_t selected_rsp,
                                      u64 *out_rflags);
extern void threadtest_syscall_then_touch_rsp(uintptr_t selected_rsp);

#define SYSCALL_PROBE_UNMAPPED_RSP ((uintptr_t)0x0000700000000000ULL)

static int save_log(const char *text)
{
    mg_handle_t handle = 0;
    usize length = strlen(text);
    mg_result_t result = file_create_contents("/temp/thread-evidence.txt", text,
                                               length, &handle);
    if (result < 0) return (int)result;
    return handle_close(handle) == MG_OK ? 0 : 1;
}

static bool own_process_is_running_on_multiple_cpus(void)
{
    mg_process_info_t entries[MG_PROCESS_SNAPSHOT_PAGE_MAX];
    u64 pid = process_current_pid();
    u32 offset = 0, count = 0, total = 0;
    do {
        mg_process_snapshot_request_t request = {
            .offset = offset,
            .result_capacity = MG_PROCESS_SNAPSHOT_PAGE_MAX,
            .result = entries,
            .out_count = &count,
            .out_total = &total,
        };
        if (process_snapshot(&request) != MG_OK) return false;
        for (u32 i = 0; i < count; i++)
            if (entries[i].pid == pid)
                return entries[i].running_cpu == MG_PROCESS_CPU_MULTIPLE;
        offset += count;
    } while (count && offset < total);
    return false;
}

static bool own_process_memory_bytes(u64 *out_bytes, u32 *out_flags)
{
    if (!out_bytes) return false;
    mg_process_info_t entries[MG_PROCESS_SNAPSHOT_PAGE_MAX];
    u64 pid = process_current_pid();
    u32 offset = 0, count = 0, total = 0;
    do {
        mg_process_snapshot_request_t request = {
            .offset = offset,
            .result_capacity = MG_PROCESS_SNAPSHOT_PAGE_MAX,
            .result = entries,
            .out_count = &count,
            .out_total = &total,
        };
        if (process_snapshot(&request) != MG_OK) return false;
        for (u32 i = 0; i < count; i++) {
            if (entries[i].pid == pid) {
                *out_bytes = entries[i].memory_bytes;
                if (out_flags) *out_flags = entries[i].flags;
                return true;
            }
        }
        offset += count;
    } while (count && offset < total);
    return false;
}

/* A process snapshot reports address-space size before its syscall epilogue.
 * That epilogue may finish a worker reap, so use consecutive observations
 * rather than treating one possibly stale sample as a stable baseline. */
static bool own_process_memory_stable(u64 deadline, u64 *out_bytes)
{
    u64 previous = 0;
    unsigned consecutive = 0;

    if (!out_bytes) return false;
    while (uptime_ms() < deadline) {
        u64 current = 0;
        if (own_process_memory_bytes(&current, NULL)) {
            if (consecutive && current == previous) {
                if (++consecutive == 2U) {
                    *out_bytes = current;
                    return true;
                }
            } else {
                previous = current;
                consecutive = 1U;
            }
        }
        (void)process_yield();
    }
    return false;
}

/* Reaping is asynchronous with respect to the worker's user-level return and
 * join completion. A pair of identical non-baseline snapshots is not proof
 * that stack/TLS mappings were reclaimed; wait for the exact pre-worker
 * mapping baseline instead. Snapshot syscalls also provide opportunities for
 * the kernel's deferred reaper to run after releasing their own user leases. */
static bool own_process_memory_wait_for(u64 expected, u64 deadline,
                                        u64 *out_last, u32 *out_flags,
                                        u32 *out_samples)
{
    u64 current = 0;
    u32 samples = 0;
    u32 flags = 0;
    bool sampled = false;

    while (uptime_ms() < deadline) {
        if (own_process_memory_bytes(&current, &flags)) {
            sampled = true;
            samples++;
            if (current == expected &&
                !(flags & MG_PROCESS_FLAG_REAP_STATE_MASK)) {
                if (out_last) *out_last = current;
                if (out_flags) *out_flags = flags;
                if (out_samples) *out_samples = samples;
                return true;
            }
        }
        (void)process_yield();
    }
    if (out_last) *out_last = sampled ? current : 0;
    if (out_flags) *out_flags = sampled ? flags : 0;
    if (out_samples) *out_samples = samples;
    return false;
}

static mg_thread_result_t worker(void *raw)
{
    worker_arg_t *argument = raw;
    mg_thread_id_t id = 0;
    worker_tls = 0x51a70000U + argument->index;
    errno = 31 + (int)argument->index;
    if (mg_thread_self(&id) != MG_OK) return 0x100U;
    argument->id = id;
    argument->observed_tls = worker_tls;
    argument->observed_errno = errno;
    /* The creator initializes this field after mg_thread_create() returns.
     * Hold the rendezvous mutex from before creation so this worker cannot
     * inspect it until the creator has stored the returned handle. */
    if (mg_mutex_lock(&gate_mutex) != MG_OK) return 0x101U;
    mg_thread_result_t self_result = 0;
    argument->self_join_result = mg_thread_join(argument->self_handle,
                                                 &self_result);
    arrived++;
    (void)mg_condition_broadcast(&gate_condition);
    while (!release_workers) {
        if (mg_condition_wait(&gate_condition, &gate_mutex) != MG_OK) {
            (void)mg_mutex_unlock(&gate_mutex);
            return 0x102U;
        }
    }
    (void)mg_mutex_unlock(&gate_mutex);

    (void)atomic_fetch_add_explicit(&cpu_witness_workers, 1U,
                                    memory_order_release);
    u64 witness_deadline = uptime_ms() + 5000U;
    while (atomic_load_explicit(&cpu_witness_workers,
                                memory_order_acquire) != WORKERS) {
        if (uptime_ms() >= witness_deadline) return 0x104U + argument->index;
        __asm__ volatile("pause" ::: "memory");
    }

    /* Keep both workers runnable while either samples the scheduler's
     * migration-safe process snapshot. Main blocks in join after this gate,
     * leaving two worker contexts to occupy the two CPUs. The monotonic limit
     * is a failure bound, not a sleep used to manufacture overlap. */
    witness_deadline = uptime_ms() + 5000U;
    while (!atomic_load_explicit(&cpu_overlap_seen, memory_order_acquire)) {
        u64 flags = 0;
        uintptr_t selected_rsp = SYSCALL_PROBE_UNMAPPED_RSP +
                                 (uintptr_t)argument->index * 0x1000U;
        if ((i64)threadtest_syscall_with_rsp(selected_rsp, &flags) < 0 ||
            !(flags & (1ULL << 9)) || (flags & (1ULL << 10)) ||
            (flags & (1ULL << 18)))
            return 0x105U + argument->index;
        if (errno != 31 + (int)argument->index)
            return 0x107U + argument->index;
        if (own_process_is_running_on_multiple_cpus())
            atomic_store_explicit(&cpu_overlap_seen, 1U,
                                  memory_order_release);
        else if (uptime_ms() >= witness_deadline)
            return 0x104U + argument->index;
        else
            __asm__ volatile("pause" ::: "memory");
    }

    for (unsigned i = 0; i < ITERATIONS; i++) {
        (void)atomic_fetch_add_explicit(&atomic_total, 1U,
                                        memory_order_relaxed);
        if (mg_mutex_lock(&gate_mutex) != MG_OK) return 0x103U;
        mutex_total++;
        (void)mg_mutex_unlock(&gate_mutex);
    }
    return 0xabc00000U + argument->index;
}

static mg_thread_result_t explicit_exit_worker(void *raw)
{
    mg_thread_result_t result = (mg_thread_result_t)(uintptr_t)raw;
    (void)mg_thread_exit(result);
    for (;;) (void)process_yield();
}

static mg_thread_result_t wait_worker(void *raw)
{
    atomic_uint *word = raw;
    atomic_store_explicit(&wait_ready, 1U, memory_order_release);
    mg_result_t result = mg_sync_wait_u32((const volatile u32 *)word, 0U,
                                          MG_SYNC_WAIT_FOREVER);
    return result == MG_OK ? 0x55U : (mg_thread_result_t)(i64)result;
}

static mg_thread_result_t termination_condition_worker(void *raw)
{
    atomic_uint *ready = raw;
    if (mg_mutex_lock(&gate_mutex) != MG_OK) return 0x56U;
    atomic_store_explicit(ready, 1U, memory_order_release);
    mg_result_t result = mg_condition_wait(&gate_condition, &gate_mutex);
    (void)mg_mutex_unlock(&gate_mutex);
    return result == MG_ERR_SHUTDOWN ? 0x57U : 0x58U;
}

static mg_thread_result_t repeat_worker(void *raw)
{
    return (mg_thread_result_t)(uintptr_t)raw + 0x7000U;
}

static mg_thread_result_t detached_worker(void *raw)
{
    atomic_uint *completed = raw;
    (void)atomic_fetch_add_explicit(completed, 1U, memory_order_release);
    return 0;
}

static mg_thread_result_t spin_worker(void *raw)
{
    atomic_uint *stop = raw;
    while (!atomic_load_explicit(stop, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");
    return 0x44U;
}

static mg_thread_result_t fault_worker(void *raw)
{
    (void)raw;
    volatile const u64 *invalid = (volatile const u64 *)0;
    return *invalid;
}

static int child_mode(const char *mode)
{
    mg_thread_created_t thread;
    mg_thread_result_t result = 0;
    if (!strcmp(mode, "exit-spin")) {
        static atomic_uint stop;
        atomic_init(&stop, 0U);
        if (mg_thread_create(&thread, spin_worker, &stop) != MG_OK) return 41;
        process_exit(23);
    }
    if (!strcmp(mode, "exit-wait")) {
        atomic_init(&termination_wait_ready, 0U);
        if (mg_thread_create(&thread, termination_condition_worker,
                             &termination_wait_ready) != MG_OK) return 42;
        while (!atomic_load_explicit(&termination_wait_ready,
                                     memory_order_acquire))
            (void)process_yield();
        process_exit(24);
    }
    if (!strcmp(mode, "fault")) {
        if (mg_thread_create(&thread, fault_worker, NULL) != MG_OK) return 43;
        (void)mg_thread_join(thread.handle, &result);
        for (;;) (void)process_yield();
    }
    if (!strcmp(mode, "bad-rsp")) {
        /* SYSRET must not run with a supervisor stack pointer. Correct entry
         * code terminates this child as a user-context fault before SYSRET. */
        u64 flags = 0;
        (void)threadtest_syscall_with_rsp(
            (uintptr_t)0xffff800000000000ULL, &flags);
        return 44;
    }
    if (!strcmp(mode, "exhausted-rsp")) {
        /* Entry and return tolerate an unmapped lower-half RSP, but the
         * first real stack write must fault in Ring 3, not corrupt the kernel. */
        threadtest_syscall_then_touch_rsp(SYSCALL_PROBE_UNMAPPED_RSP);
        return 45;
    }
    return 40;
}

static int verify_process_termination(void)
{
    const char *spin_argv[] = {"/bin/threadtest", "exit-spin"};
    const char *wait_argv[] = {"/bin/threadtest", "exit-wait"};
    const char *fault_argv[] = {"/bin/threadtest", "fault"};
    const char *bad_rsp_argv[] = {"/bin/threadtest", "bad-rsp"};
    const char *exhausted_rsp_argv[] = {
        "/bin/threadtest", "exhausted-rsp"
    };
    const char *const *commands[] = {
        spin_argv, wait_argv, fault_argv, bad_rsp_argv, exhausted_rsp_argv
    };
    const i32 expected[] = {
        23, 24, MG_PROCESS_STATUS_CRASHED, MG_PROCESS_STATUS_CRASHED,
        MG_PROCESS_STATUS_CRASHED
    };
    for (unsigned i = 0; i < 5; i++) {
        mg_result_t spawned = process_spawn_argv(commands[i], 2U);
        if (spawned <= 0) {
            printf("THREADS1 FAIL termination spawn_case=%u result=%lld\n",
                   i, (long long)spawned);
            return 50 + (int)i;
        }
        i32 status = 0;
        mg_handle_t handle = (mg_handle_t)spawned;
        mg_result_t waited = process_wait(handle, &status);
        mg_result_t closed = handle_close(handle);
        if (waited != MG_OK || closed != MG_OK || status != expected[i]) {
            printf("THREADS1 FAIL termination case=%u wait=%lld close=%lld "
                   "status=%d expected=%d\n", i, (long long)waited,
                   (long long)closed, status, expected[i]);
            return 53 + (int)i;
        }
    }
    return 0;
}

static int verify_syscall_entry_stack(void)
{
    const usize bytes = 4096U;
    const usize marker_bytes = 128U;
    u8 *mapping = NULL;
    u64 observed_flags = 0;

    if (memory_map(bytes, (void **)&mapping) != MG_OK || !mapping)
        return 1;
    for (usize index = 0; index < marker_bytes; index++)
        mapping[bytes - marker_bytes + index] = (u8)(0x5aU ^ index);

    /* The writable mapping makes the prior entry's user-stack pushes
     * observable as deterministic corruption of these bytes. */
    if ((i64)threadtest_syscall_with_rsp((uintptr_t)(mapping + bytes),
                                        &observed_flags) < 0) {
        (void)memory_unmap(mapping);
        return 2;
    }
    for (usize index = 0; index < marker_bytes; index++) {
        if (mapping[bytes - marker_bytes + index] != (u8)(0x5aU ^ index)) {
            (void)memory_unmap(mapping);
            return 3;
        }
    }
    if (!(observed_flags & (1ULL << 9)) ||
        (observed_flags & ((1ULL << 10) | (1ULL << 18)))) {
        (void)memory_unmap(mapping);
        return 4;
    }

    /* Text is user-readable but not writable; using it as RSP proves entry
     * does not attempt to save any register through the caller's stack. */
    if ((i64)threadtest_syscall_with_rsp(
            (uintptr_t)threadtest_syscall_with_rsp,
            &observed_flags) < 0) {
        (void)memory_unmap(mapping);
        return 5;
    }

    /* A stack pointer at the low edge of a mapped region behaves like an
     * exhausted stack adjacent to an unmapped guard page. */
    if ((i64)threadtest_syscall_with_rsp((uintptr_t)mapping,
                                        &observed_flags) < 0) {
        (void)memory_unmap(mapping);
        return 6;
    }

    /* A pointer immediately below the mapping is a guard-adjacent candidate;
     * entry itself must not touch it even when it is not mapped. */
    if ((i64)threadtest_syscall_with_rsp((uintptr_t)mapping - bytes,
                                        &observed_flags) < 0) {
        (void)memory_unmap(mapping);
        return 7;
    }

    /* An unmapped, lower-half user RSP remains safe when the caller restores
     * its real stack without touching the selected pointer after SYSRET. */
    if ((i64)threadtest_syscall_with_rsp(SYSCALL_PROBE_UNMAPPED_RSP,
                                        &observed_flags) < 0) {
        (void)memory_unmap(mapping);
        return 8;
    }
    if (memory_unmap(mapping) != MG_OK) return 9;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2) return child_mode(argv[1]);
    REQUIRE(argc == 1, "arguments");
    REQUIRE(verify_syscall_entry_stack() == 0,
            "trusted syscall stack, user-memory isolation, and flags");
    worker_arg_t arguments[WORKERS] = {{.index = 0U}, {.index = 1U}};
    mg_thread_created_t threads[WORKERS];
    mg_thread_id_t initial_id = 0;
    REQUIRE(mg_thread_self(&initial_id) == MG_OK && initial_id != 0,
            "initial identity");
    REQUIRE(mg_mutex_init(&gate_mutex) == MG_OK, "mutex init");
    REQUIRE(mg_condition_init(&gate_condition) == MG_OK, "condition init");
    atomic_init(&atomic_total, 0U);
    atomic_init(&cpu_overlap_seen, 0U);
    atomic_init(&cpu_witness_workers, 0U);
    atomic_init(&wait_ready, 0U);
    errno = 79;
    u64 persistent_user_bytes = 0;
    REQUIRE(own_process_memory_stable(uptime_ms() + 5000U,
                                      &persistent_user_bytes),
            "initial process memory snapshot");

    REQUIRE(mg_mutex_lock(&gate_mutex) == MG_OK, "main startup lock");
    for (unsigned i = 0; i < WORKERS; i++)
        REQUIRE(mg_thread_create(&threads[i], worker, &arguments[i]) == MG_OK,
                "worker create");
    for (unsigned i = 0; i < WORKERS; i++)
        arguments[i].self_handle = threads[i].handle;
    REQUIRE(mg_mutex_unlock(&gate_mutex) == MG_OK, "main startup unlock");
    REQUIRE(mg_mutex_lock(&gate_mutex) == MG_OK, "main lock");
    while (arrived != WORKERS)
        REQUIRE(mg_condition_wait(&gate_condition, &gate_mutex) == MG_OK,
                "condition rendezvous");
    release_workers = true;
    REQUIRE(mg_condition_broadcast(&gate_condition) == MG_OK,
            "condition broadcast");
    REQUIRE(mg_mutex_unlock(&gate_mutex) == MG_OK, "main unlock");

    for (unsigned i = 0; i < WORKERS; i++) {
        mg_thread_result_t result = 0;
        REQUIRE(mg_thread_join(threads[i].handle, &result) == MG_OK,
                "worker join");
        REQUIRE(result == 0xabc00000U + i, "worker return result");
        REQUIRE(arguments[i].id == threads[i].id &&
                arguments[i].id != initial_id, "worker identity");
        REQUIRE(arguments[i].observed_tls == 0x51a70000U + i,
                "worker TLS isolation");
        REQUIRE(arguments[i].observed_errno == 31 + (int)i,
                "worker errno isolation");
        REQUIRE(arguments[i].self_join_result == MG_ERR_BAD_ARGUMENT,
                "self join rejection");
    }
    REQUIRE(errno == 79, "initial errno isolation");
    REQUIRE(atomic_load_explicit(&atomic_total, memory_order_relaxed) ==
            WORKERS * ITERATIONS, "shared atomic updates");
    REQUIRE(mutex_total == WORKERS * ITERATIONS, "mutex contention");
    REQUIRE(atomic_load_explicit(&cpu_overlap_seen, memory_order_acquire),
            "simultaneous two-CPU execution");
    u64 settled_user_bytes = 0;
    u32 joined_reclaim_samples = 0;
    u32 joined_reclaim_flags = 0;
    u64 joined_reclaim_start = uptime_ms();
    bool joined_memory_reclaimed = own_process_memory_wait_for(
        persistent_user_bytes, joined_reclaim_start + 5000U,
        &settled_user_bytes, &joined_reclaim_flags,
        &joined_reclaim_samples);
    if (!joined_memory_reclaimed) {
        u64 excess = settled_user_bytes > persistent_user_bytes
            ? settled_user_bytes - persistent_user_bytes : 0;
        printf("THREADS1 FAIL joined reclaim expected=%llu current=%llu "
               "excess=%llu excess_pages=%llu workers=%u joined=%u "
               "reap_flags=0x%08x worker_reap=%u region_reap=%u "
               "reaper_active=%u samples=%u wait_ms=%llu\n",
               (unsigned long long)persistent_user_bytes,
               (unsigned long long)settled_user_bytes,
               (unsigned long long)excess,
               (unsigned long long)(excess / 4096U), WORKERS, WORKERS,
               joined_reclaim_flags,
               (unsigned)!!(joined_reclaim_flags &
                    MG_PROCESS_FLAG_WORKER_REAP_PENDING),
               (unsigned)!!(joined_reclaim_flags &
                    MG_PROCESS_FLAG_USER_REGION_REAP_PENDING),
               (unsigned)!!(joined_reclaim_flags &
                    MG_PROCESS_FLAG_WORKER_REAPER_ACTIVE),
               joined_reclaim_samples,
               (unsigned long long)(uptime_ms() - joined_reclaim_start));
        return 1;
    }
    REQUIRE(mg_condition_destroy(&gate_condition) == MG_OK,
            "condition destroy");
    REQUIRE(mg_mutex_destroy(&gate_mutex) == MG_OK, "mutex destroy");

    mg_thread_created_t explicit_thread;
    mg_thread_result_t explicit_result = 0;
    REQUIRE(mg_thread_create(&explicit_thread, explicit_exit_worker,
            (void *)(uintptr_t)0x11223344U) == MG_OK, "explicit create");
    REQUIRE(mg_thread_join(explicit_thread.handle, &explicit_result) == MG_OK &&
            explicit_result == 0x11223344U, "explicit exit and join");

    atomic_uint word;
    atomic_init(&word, 0U);
    mg_thread_created_t waiter;
    mg_thread_result_t wait_result = 0;
    REQUIRE(mg_thread_create(&waiter, wait_worker, &word) == MG_OK,
            "wait worker create");
    while (!atomic_load_explicit(&wait_ready, memory_order_acquire))
        (void)process_yield();
    /* Repeated wake probes establish that the waiter is actually registered;
     * a wake before registration is harmless and cannot lose the later store. */
    mg_result_t wake_result = 0;
    while (wake_result == 0) {
        wake_result = mg_sync_wake_u32((const volatile u32 *)&word,
                                       MG_SYNC_WAKE_ALL);
        if (wake_result < 0) break;
        if (wake_result == 0) (void)process_yield();
    }
    REQUIRE(wake_result > 0, "waiter registration");
    atomic_store_explicit(&word, 1U, memory_order_release);
    (void)mg_sync_wake_u32((const volatile u32 *)&word, MG_SYNC_WAKE_ALL);
    REQUIRE(mg_thread_join(waiter.handle, &wait_result) == MG_OK &&
            wait_result == 0x55U,
            "wait/wake result");

    for (unsigned i = 0; i < REPEAT_WORKERS; i++) {
        mg_thread_created_t repeat;
        mg_thread_result_t result = 0;
        REQUIRE(mg_thread_create(&repeat, repeat_worker,
                (void *)(uintptr_t)i) == MG_OK, "repeat create");
        REQUIRE(mg_thread_join(repeat.handle, &result) == MG_OK &&
                result == 0x7000U + i, "repeat join and reap");
    }
    atomic_uint detached_count;
    atomic_init(&detached_count, 0U);
    u64 baseline_user_bytes = settled_user_bytes;
    for (unsigned i = 0; i < REPEAT_WORKERS; i++) {
        mg_thread_created_t detached;
        REQUIRE(mg_thread_create(&detached, detached_worker,
                                &detached_count) == MG_OK,
                "detached create");
        REQUIRE(mg_thread_detach(detached.handle) == MG_OK,
                "detach");
    }
    while (atomic_load_explicit(&detached_count, memory_order_acquire) !=
           REPEAT_WORKERS)
        (void)process_yield();
    /* Completion in user code precedes scheduler handoff and reclamation.
     * Wait for process-owned memory to return to baseline, proving detached
     * worker stacks and TLS mappings were released. */
    u64 reap_start = uptime_ms();
    u64 current_user_bytes = 0;
    u32 detached_reclaim_samples = 0;
    u32 detached_reclaim_flags = 0;
    bool detached_memory_reclaimed = own_process_memory_wait_for(
        baseline_user_bytes, reap_start + 5000U, &current_user_bytes,
        &detached_reclaim_flags, &detached_reclaim_samples);
    if (!detached_memory_reclaimed) {
        u64 excess = current_user_bytes > baseline_user_bytes
            ? current_user_bytes - baseline_user_bytes : 0;
        printf("THREADS1 FAIL detached reclaim expected=%llu current=%llu "
               "excess=%llu excess_pages=%llu workers=%u completed=%u "
               "reap_flags=0x%08x worker_reap=%u region_reap=%u "
               "reaper_active=%u samples=%u wait_ms=%llu\n",
               (unsigned long long)baseline_user_bytes,
               (unsigned long long)current_user_bytes,
               (unsigned long long)excess,
               (unsigned long long)(excess / 4096U), REPEAT_WORKERS,
               atomic_load_explicit(&detached_count, memory_order_acquire),
               detached_reclaim_flags,
               (unsigned)!!(detached_reclaim_flags &
                    MG_PROCESS_FLAG_WORKER_REAP_PENDING),
               (unsigned)!!(detached_reclaim_flags &
                    MG_PROCESS_FLAG_USER_REGION_REAP_PENDING),
               (unsigned)!!(detached_reclaim_flags &
                    MG_PROCESS_FLAG_WORKER_REAPER_ACTIVE),
               detached_reclaim_samples,
               (unsigned long long)(uptime_ms() - reap_start));
        return 1;
    }

    REQUIRE(verify_process_termination() == 0, "process termination and fault");
    static char transcript[512];
    int length = snprintf(transcript, sizeof(transcript),
        "THREADS1 PASS cpus=2 workers=%u iterations=%u atomic=%u mutex=%u "
        "tls=isolated return=join explicit-exit=pass detached=%u "
        "termination=spin,wait,fault\n",
        WORKERS, ITERATIONS, WORKERS * ITERATIONS,
        WORKERS * ITERATIONS, REPEAT_WORKERS);
    REQUIRE(length > 0 && (usize)length < sizeof(transcript), "format log");
    REQUIRE(save_log(transcript) == 0, "save transcript");
    puts(transcript);
    return 0;
}
