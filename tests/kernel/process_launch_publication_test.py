#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Launch publication reasserts cleanup work for an early-exiting child."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def function(path, name):
    source = (ROOT / path).read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        brace = source.index("{", match.end())
        if ";" in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise ValueError(f"missing production function {path}:{name}")


prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t i32;
typedef int thread_state_t;
typedef uint64_t mg_session_id_t;
#define THREAD_CPU_NONE ((unsigned)-1)
enum { PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED };
enum { THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
       THREAD_STATE_TERMINATED };
enum { THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
       THREAD_PROCESS_EXIT_REQUESTED, THREAD_PROCESS_HANDOFF_COMPLETE };
enum { SCHEDULER_WAKE_FAILED, SCHEDULER_WAKE_DELIVERED,
       SCHEDULER_WAKE_ALREADY_TERMINATED };
enum { SCHEDULER_DISPATCH_BLOCK };
typedef int scheduler_wake_result_t;
typedef pthread_mutex_t spinlock_t;
typedef struct process process_t;
typedef struct kernel_thread kernel_thread_t;
typedef struct kernel_object {
    u32 ref_count;
    void (*destroy)(struct kernel_object *);
} kernel_object_t;
struct kernel_thread {
    int state, process_state;
    unsigned running_cpu;
    bool queued, context_switch_pending, event_wake_pending, sleeping;
    bool mutex_wake_pending, process_reference_held, process_initial;
    uintptr_t preempt_return_rip;
    volatile bool process_exit_requested;
    kernel_thread_t *process_next;
    process_t *process;
};
struct process {
    int state;
    bool launch_ready, exit_cleanup_complete, exit_cleanup_in_progress;
    bool termination_pending, external_termination_in_progress;
    volatile bool exit_requested;
    i32 exit_status;
    u32 active_syscalls, thread_count, live_thread_count, worker_reapers;
    kernel_thread_t *main_thread, *threads;
    process_t *next_all;
    kernel_object_t object;
};
static spinlock_t process_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static spinlock_t scheduler_lock = PTHREAD_MUTEX_INITIALIZER;
static kernel_thread_t cleanup_worker;
static kernel_thread_t *process_cleanup_worker = &cleanup_worker;
static kernel_thread_t *current_thread = &cleanup_worker, *idle_thread;
static process_t *all_processes;
static _Thread_local bool irq_enabled = true;
static _Thread_local unsigned held_spinlocks;
static struct { unsigned blocks, blocked_threads; } scheduler_stats;
static unsigned scheduler_unblocks, cleanup_completions;
static unsigned process_destructions;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static pthread_t publication_tid, exit_tid, scan_tid;
static bool publication_tid_valid, exit_tid_valid, scan_tid_valid;
static bool hold_publication_unlock, publication_at_unlock;
static bool release_publication, exit_lock_attempted;
static bool hold_scan_unlock, scan_at_unlock, release_scan;
static bool publication_lock_attempted;
static process_t *concurrent_process;
static kernel_thread_t *concurrent_thread;
static u64 spin_lock_irqsave(spinlock_t *lock) {
    u64 old = irq_enabled;
    irq_enabled = false;
    if (lock == &process_registry_lock) {
        pthread_mutex_lock(&gate_lock);
        if (exit_tid_valid && pthread_equal(pthread_self(), exit_tid)) {
            exit_lock_attempted = true;
            pthread_cond_broadcast(&gate_cond);
        }
        if (publication_tid_valid &&
            pthread_equal(pthread_self(), publication_tid)) {
            publication_lock_attempted = true;
            pthread_cond_broadcast(&gate_cond);
        }
        pthread_mutex_unlock(&gate_lock);
    }
    assert(pthread_mutex_lock(lock) == 0);
    held_spinlocks++;
    return old;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 old) {
    assert(held_spinlocks);
    if (lock == &process_registry_lock) {
        pthread_mutex_lock(&gate_lock);
        if (hold_publication_unlock && publication_tid_valid &&
            pthread_equal(pthread_self(), publication_tid) &&
            concurrent_process && concurrent_process->launch_ready) {
            publication_at_unlock = true;
            pthread_cond_broadcast(&gate_cond);
            while (!release_publication)
                pthread_cond_wait(&gate_cond, &gate_lock);
        }
        if (hold_scan_unlock && scan_tid_valid &&
            pthread_equal(pthread_self(), scan_tid)) {
            scan_at_unlock = true;
            pthread_cond_broadcast(&gate_cond);
            while (!release_scan)
                pthread_cond_wait(&gate_cond, &gate_lock);
        }
        pthread_mutex_unlock(&gate_lock);
    }
    held_spinlocks--;
    assert(pthread_mutex_unlock(lock) == 0);
    irq_enabled = old != 0;
}
static unsigned spinlock_depth_current(void) { return held_spinlocks; }
static u64 cpu_current_index(void) { return 0; }
bool scheduler_thread_exit_complete(const kernel_thread_t *thread);
static bool scheduler_wake_waiter(kernel_thread_t *thread);
static bool scheduler_unblock_locked(kernel_thread_t *thread) {
    assert(held_spinlocks && thread->state == THREAD_STATE_BLOCKED);
    thread->state = THREAD_STATE_READY;
    scheduler_unblocks++;
    return true;
}
static bool scheduler_prepare_dispatch_locked(int reason,
        kernel_thread_t **outgoing, kernel_thread_t **target, bool *handoff) {
    (void)reason; *outgoing = current_thread; *target = NULL;
    *handoff = false; return true;
}
static bool scheduler_handoff(kernel_thread_t *outgoing,
        kernel_thread_t *target, u64 flags) {
    (void)outgoing; (void)target; (void)flags; abort();
}
static void test_process_destroy(kernel_object_t *object) {
    assert(object && !object->ref_count);
    process_destructions++;
}
static void kfree(void *pointer) { (void)pointer; abort(); }
bool object_reference(kernel_object_t *object);
void object_release(kernel_object_t *object);
static void panic(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
static void process_storage_authorization_clear_locked(process_t *process) {
    (void)process;
}
static kernel_thread_t *thread_current(void) { return current_thread; }
static void console_cancel_waiter(kernel_thread_t *thread) { (void)thread; }
static bool process_cleanup_worker_wake(process_t *hint);
'''

object_functions = "".join(function("kernel/src/object.c", name) for name in (
    "object_reference", "object_release",
))

process_functions = "".join(function("kernel/src/process.c", name) for name in (
    "process_thread_list_complete_locked",
    "process_cleanup_candidate_locked",
    "process_cleanup_worker_wake",
    "process_publish_launch",
    "process_exit",
))

scheduler_functions = "".join(function("kernel/src/scheduler.c", name) for name in (
    "scheduler_wake_waiter_result", "scheduler_wake_waiter",
    "scheduler_clear_event_wake", "scheduler_consume_pending_wake_locked",
    "scheduler_block", "scheduler_thread_exit_complete",
))

suffix = r'''
/* One deterministic worker pass uses the production candidate predicate. The
 * cleanup body is represented by a counter; actual switch/CR3 execution is not
 * simulated by this host fixture. */
static bool worker_scan_once(void) {
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    process_t *candidate = process_cleanup_candidate_locked();
    if (candidate) {
        candidate->state = PROCESS_STATE_TERMINATED;
        candidate->exit_cleanup_complete = true;
        cleanup_completions++;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (candidate) object_release(&candidate->object);
    return candidate != NULL;
}
static bool concurrent_scan_result;
static void *worker_scan_thread_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&gate_lock);
    scan_tid = pthread_self(); scan_tid_valid = true;
    pthread_mutex_unlock(&gate_lock);
    concurrent_scan_result = worker_scan_once();
    return NULL;
}
static void worker_scan_until_idle(void) {
    current_thread = &cleanup_worker;
    while (worker_scan_once()) {}
}
static void init_process(process_t *p, kernel_thread_t *thread, int state,
                         bool published) {
    memset(p, 0, sizeof(*p)); memset(thread, 0, sizeof(*thread));
    p->state = state; p->launch_ready = published; p->main_thread = thread;
    p->threads = thread; p->thread_count = 1;
    p->live_thread_count = state == PROCESS_STATE_ACTIVE ? 1U : 0U;
    p->object.ref_count = 2;
    p->object.destroy = test_process_destroy;
    thread->process = p; thread->process_state = THREAD_PROCESS_HANDOFF_COMPLETE;
    thread->process_reference_held = true; thread->process_initial = true;
    thread->state = THREAD_STATE_TERMINATED; thread->running_cpu = (unsigned)-1;
}
static void test_consumed_early_event(void) {
    process_t p; kernel_thread_t thread;
    init_process(&p, &thread, PROCESS_STATE_EXITING, false);
    p.live_thread_count = 0; all_processes = &p;
    cleanup_worker.state = THREAD_STATE_RUNNING; current_thread = &cleanup_worker;
    unsigned initial_refs = p.object.ref_count;

    /* Handoff wake is consumed by a worker scan which correctly skips the
     * unpublished process; the worker then parks. */
    assert(scheduler_wake_waiter(&cleanup_worker));
    scheduler_clear_event_wake(&cleanup_worker);
    assert(!worker_scan_once());
    assert(scheduler_block() && cleanup_worker.state == THREAD_STATE_BLOCKED);
    assert(!p.launch_ready && !cleanup_completions);

    assert(process_publish_launch(&p));
    assert(p.launch_ready && cleanup_worker.state == THREAD_STATE_READY);
    assert(scheduler_unblocks == 1 && p.object.ref_count == initial_refs);
    cleanup_worker.state = THREAD_STATE_RUNNING;
    worker_scan_until_idle();
    assert(p.state == PROCESS_STATE_TERMINATED &&
           p.exit_cleanup_complete && cleanup_completions == 1);
    assert(!worker_scan_once() && cleanup_completions == 1);
    assert(p.object.ref_count == initial_refs);
    object_release(&p.object); /* process owner */
    object_release(&p.object); /* initial thread ownership */
    assert(!p.object.ref_count && process_destructions == 1);
}
static void test_publication_first_and_invalid_launch(void) {
    process_t active, invalid; kernel_thread_t a, missing;
    init_process(&active, &a, PROCESS_STATE_ACTIVE, false);
    init_process(&invalid, &missing, PROCESS_STATE_ACTIVE, false);
    invalid.main_thread = NULL;
    cleanup_worker.state = THREAD_STATE_RUNNING;
    unsigned refs = active.object.ref_count;
    assert(process_publish_launch(&active));
    assert(active.launch_ready && !cleanup_worker.event_wake_pending &&
           active.object.ref_count == refs);
    /* Exit after publication retains the existing final-handoff wake path. */
    active.state = PROCESS_STATE_EXITING;
    assert(scheduler_wake_waiter(&cleanup_worker));
    assert(cleanup_worker.event_wake_pending);
    assert(!process_publish_launch(&active));
    assert(!process_publish_launch(&invalid));
    assert(!invalid.launch_ready);
}
static void test_exit_and_publication_orders(void) {
    process_t p; kernel_thread_t thread;
    cleanup_worker.event_wake_pending = false;
    init_process(&p, &thread, PROCESS_STATE_ACTIVE, false);
    thread.state = THREAD_STATE_RUNNING; thread.running_cpu = 0;
    current_thread = &thread; cleanup_worker.state = THREAD_STATE_RUNNING;
    assert(process_publish_launch(&p));
    assert(!cleanup_worker.event_wake_pending);
    assert(process_exit(&p, 31));
    /* The production exit path wakes siblings, while the final handoff is the
     * existing cleanup event when publication won the registry lock. */
    assert(scheduler_wake_waiter(&cleanup_worker));
    assert(cleanup_worker.event_wake_pending);

    cleanup_worker.event_wake_pending = false;
    init_process(&p, &thread, PROCESS_STATE_ACTIVE, false);
    thread.state = THREAD_STATE_RUNNING; thread.running_cpu = 0;
    current_thread = &thread; cleanup_worker.state = THREAD_STATE_RUNNING;
    assert(process_exit(&p, 32));
    assert(p.state == PROCESS_STATE_EXITING);
    assert(process_publish_launch(&p));
    assert(cleanup_worker.event_wake_pending);
    current_thread = &cleanup_worker;
}
static void *publish_thread_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&gate_lock);
    publication_tid = pthread_self();
    publication_tid_valid = true;
    pthread_mutex_unlock(&gate_lock);
    assert(process_publish_launch(concurrent_process));
    return NULL;
}
static void *exit_thread_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&gate_lock);
    exit_tid = pthread_self();
    exit_tid_valid = true;
    pthread_mutex_unlock(&gate_lock);
    current_thread = concurrent_thread;
    assert(process_exit(concurrent_process, 37));
    /* Model the existing final scheduler-handoff notification after the
     * process transition has released the registry lock. */
    assert(scheduler_wake_waiter(&cleanup_worker));
    return NULL;
}
static void test_exit_attempted_during_publication(void) {
    process_t p; kernel_thread_t thread;
    pthread_t publisher, exiter;
    init_process(&p, &thread, PROCESS_STATE_ACTIVE, false);
    cleanup_worker.state = THREAD_STATE_RUNNING;
    cleanup_worker.event_wake_pending = false;
    concurrent_process = &p; concurrent_thread = &thread;
    publication_tid_valid = exit_tid_valid = false;
    publication_at_unlock = release_publication = exit_lock_attempted = false;
    hold_publication_unlock = true;

    assert(pthread_create(&publisher, NULL, publish_thread_main, NULL) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!publication_at_unlock)
        pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    assert(pthread_create(&exiter, NULL, exit_thread_main, NULL) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!exit_lock_attempted)
        pthread_cond_wait(&gate_cond, &gate_lock);
    release_publication = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);

    assert(pthread_join(publisher, NULL) == 0);
    assert(pthread_join(exiter, NULL) == 0);
    hold_publication_unlock = false;
    assert(p.launch_ready && p.state == PROCESS_STATE_EXITING &&
           cleanup_worker.event_wake_pending);
    object_release(&p.object);
    object_release(&p.object);
    assert(!p.object.ref_count && process_destructions == 2);
    current_thread = &cleanup_worker;
    concurrent_process = NULL;
    concurrent_thread = NULL;
}
static void test_publication_during_worker_scan(void) {
    process_t p; kernel_thread_t thread;
    pthread_t scanner, publisher;
    unsigned completed_before = cleanup_completions;
    unsigned destroyed_before = process_destructions;
    init_process(&p, &thread, PROCESS_STATE_EXITING, false);
    p.live_thread_count = 0; all_processes = &p;
    concurrent_process = &p; concurrent_thread = &thread;
    cleanup_worker.state = THREAD_STATE_RUNNING;
    scheduler_clear_event_wake(&cleanup_worker);
    hold_scan_unlock = true; scan_at_unlock = release_scan = false;
    publication_tid_valid = false; publication_lock_attempted = false;
    assert(pthread_create(&scanner, NULL, worker_scan_thread_main, NULL) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!scan_at_unlock) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    assert(pthread_create(&publisher, NULL, publish_thread_main, NULL) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!publication_lock_attempted)
        pthread_cond_wait(&gate_cond, &gate_lock);
    release_scan = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    assert(pthread_join(scanner, NULL) == 0);
    assert(pthread_join(publisher, NULL) == 0);
    hold_scan_unlock = false; scan_tid_valid = false;

    assert(!concurrent_scan_result && p.launch_ready &&
           cleanup_worker.event_wake_pending);
    current_thread = &cleanup_worker;
    assert(scheduler_block() && cleanup_worker.state == THREAD_STATE_RUNNING);
    worker_scan_until_idle();
    assert(p.state == PROCESS_STATE_TERMINATED &&
           cleanup_completions == completed_before + 1);
    object_release(&p.object);
    object_release(&p.object);
    assert(!p.object.ref_count && process_destructions == destroyed_before + 1);
    concurrent_process = NULL; concurrent_thread = NULL;
}
static void test_scan_to_park_latch_and_multiple_children(void) {
    process_t p[2]; kernel_thread_t t[2];
    unsigned destroyed_before = process_destructions;
    cleanup_completions = 0;
    init_process(&p[0], &t[0], PROCESS_STATE_EXITING, false);
    init_process(&p[1], &t[1], PROCESS_STATE_EXITING, false);
    p[0].next_all = &p[1]; all_processes = &p[0];
    p[0].live_thread_count = p[1].live_thread_count = 0;
    cleanup_worker.state = THREAD_STATE_RUNNING;
    scheduler_clear_event_wake(&cleanup_worker);
    assert(!worker_scan_once()); /* scan before publication */
    assert(process_publish_launch(&p[0])); /* wake after scan, before block */
    assert(process_publish_launch(&p[1])); /* coalesced second wake */
    assert(cleanup_worker.event_wake_pending);
    assert(scheduler_block() && cleanup_worker.state == THREAD_STATE_RUNNING);
    worker_scan_until_idle();
    assert(p[0].state == PROCESS_STATE_TERMINATED &&
           p[1].state == PROCESS_STATE_TERMINATED && cleanup_completions == 2);
    for (unsigned i = 0; i < 2; i++) {
        assert(p[i].object.ref_count == 2);
        object_release(&p[i].object);
        object_release(&p[i].object);
    }
    assert(process_destructions == destroyed_before + 2);
}
int main(void) {
    test_consumed_early_event();
    test_publication_first_and_invalid_launch();
    test_exit_and_publication_orders();
    test_exit_attempted_during_publication();
    test_publication_during_worker_scan();
    test_scan_to_park_latch_and_multiple_children();
    assert(irq_enabled && !held_spinlocks);
    puts("launch publication: early exit, concurrent publication, worker scan/park "
         "latch, coalesced children, invalid publication, object ref balance PASS");
    return 0;
}
'''

code = prefix + object_functions + process_functions + scheduler_functions + suffix
workspace_parent = Path(os.environ.get("TMPDIR") or
                        ROOT / "build" / "test-workspaces")
workspace_parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="mangrove-launch-publication-",
                                 dir=workspace_parent) as tmp:
    source = Path(tmp) / "launch_publication.c"
    binary = Path(tmp) / "launch_publication"
    source.write_text(code)
    subprocess.run([CC, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", "-pthread",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, cwd=tmp,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
