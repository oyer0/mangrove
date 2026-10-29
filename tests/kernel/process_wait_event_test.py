#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Process wait uses production event-wake and thread-list lifetime rules."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else 'cc'


def function(source, name):
    for match in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name +
                             r'\s*\(', source, re.M):
        brace = source.index('{', match.end())
        if ';' in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    raise ValueError(name)


prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32; typedef int32_t i32;
typedef uint64_t process_handle_t; typedef unsigned mg_session_id_t;
typedef uintptr_t usize;
enum { PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED };
enum { THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
       THREAD_STATE_TERMINATED };
enum { THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
       THREAD_PROCESS_EXIT_REQUESTED, THREAD_PROCESS_HANDOFF_COMPLETE };
enum { THREAD_PRIORITY_HIGH, THREAD_PRIORITY_NORMAL,
       THREAD_PRIORITY_BACKGROUND };
enum { MG_OK=0, MG_ERR_BAD_ARGUMENT=-4, MG_ERR_INVALID_HANDLE=-8,
       MG_ERR_BUSY=-14, MG_ERR_SHUTDOWN=-38, MG_ERR_WOULD_BLOCK=-19 };
#define OBJECT_TYPE_PROCESS 3
#define THREAD_CPU_NONE UINT32_MAX
#define SCHEDULER_DISPATCH_BLOCK 1
typedef pthread_mutex_t spinlock_t;
typedef u32 thread_priority_t;
typedef struct process process_t;
typedef struct { u32 ref_count; process_t *process; int type; } kernel_object_t;
typedef struct kernel_thread {
    u32 state, running_cpu;
    bool context_switch_pending, syscall_active, sleeping, queued;
    bool wakeup_boosted, event_wake_pending, mutex_wake_pending;
    u32 base_priority, effective_priority;
    u64 wakeup_deadline_ms;
    struct kernel_thread *next, *process_next;
    process_t *process;
    int process_state;
    bool process_initial, process_reference_held;
    volatile bool process_exit_requested;
    i32 process_exit_status;
    bool process_exit_status_valid;
    void *worker_record;
    uintptr_t preempt_return_rip;
} kernel_thread_t;
struct process {
    int state;
    bool wait_collected, owner_reference_held, shutdown_requested;
    bool exit_requested, termination_pending, external_termination_in_progress;
    bool launch_ready, wait_claimed, exit_cleanup_complete;
    bool exit_cleanup_in_progress, thread_reap_in_progress;
    bool exit_notification_sent;
    i32 exit_status; u64 session_id;
    u32 active_syscalls, thread_count, live_thread_count;
    u32 thread_list_readers, worker_reapers;
    process_t *parent, *waiting_child, *first_child, *next_sibling;
    kernel_thread_t *main_thread, *threads, *waiting_thread;
    kernel_object_t object;
    bool storage_management_session, storage_management_authorizing;
    u32 storage_authorized_operation; u64 storage_authorized_thread_id;
    _Atomic bool worker_reap_pending;
};
typedef struct { u64 blocks, wakeups, sleeping_threads, blocked_threads; }
    scheduler_stats_t;
static spinlock_t process_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static spinlock_t scheduler_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scheduler_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static scheduler_stats_t scheduler_stats;
static kernel_thread_t *current_thread, *idle_thread, *sleeping_threads;
static bool preemption_pending;
static process_t parent, child, other_child;
static kernel_thread_t parent_thread, second_parent_thread, child_thread,
                       other_child_thread;
static _Thread_local kernel_thread_t *tls_current_thread;
static _Thread_local bool tls_if = true;
static _Thread_local unsigned tls_lock_depth;
static _Thread_local u64 tls_saved_flags[16];
static _Thread_local spinlock_t *tls_saved_locks[16];
static _Thread_local unsigned tls_saved_count;
static unsigned gate_block_count;
static bool complete_on_registration_unlock;
static bool delay_delivery, delivery_entered, release_delivery;
static bool second_wait_result_ready;
static int second_wait_result;

static bool process_finish_termination(process_t *);
static bool process_cleanup_worker_wake(process_t *);
static void process_worker_reclaim_pending(process_t *);
static bool scheduler_prepare_dispatch_locked(int, kernel_thread_t **,
                                             kernel_thread_t **, bool *);
static bool scheduler_enqueue_locked(kernel_thread_t *);
static void sleeping_remove(kernel_thread_t *);
static void fixture_before_waiter_delivery(process_t *, kernel_thread_t *);
static void complete_child(process_t *, kernel_thread_t *, i32);

static u64 spin_lock_irqsave(spinlock_t *lock)
{
    u64 old = tls_if;
    tls_if = false;
    pthread_mutex_lock(lock);
    tls_lock_depth++;
    return old;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    assert(tls_lock_depth); tls_lock_depth--;
    pthread_mutex_unlock(lock); tls_if = flags != 0;
}
static void spin_lock(spinlock_t *lock)
{
    assert(tls_saved_count < 16);
    tls_saved_locks[tls_saved_count] = lock;
    tls_saved_flags[tls_saved_count++] = spin_lock_irqsave(lock);
}
static void spin_unlock(spinlock_t *lock)
{
    if (tls_saved_count && tls_saved_locks[tls_saved_count - 1] == lock) {
        u64 flags = tls_saved_flags[--tls_saved_count];
        spin_unlock_irqrestore(lock, flags);
    } else {
        /* Production sometimes pairs plain unlock with IRQ-save acquisition
         * inside an already-disabled CLI scope. */
        assert(!tls_if);
        spin_unlock_irqrestore(lock, 0);
    }
    if (lock == &process_registry_lock && complete_on_registration_unlock &&
        parent.waiting_child == &child) {
        complete_on_registration_unlock = false;
        complete_child(&child, &child_thread, 23);
    }
}
static unsigned spinlock_depth_current(void) { return tls_lock_depth; }
static u64 process_cli_save(void) { u64 old=tls_if; tls_if=false; return old; }
static void process_cli_restore(u64 flags) { tls_if=flags != 0; }
static kernel_thread_t *thread_current(void) { return tls_current_thread; }
static bool process_shutdown_requested(const process_t *p)
{ return p && p->shutdown_requested; }
static void panic(const char *s) { fprintf(stderr, "panic: %s\n", s); abort(); }
static bool object_reference(kernel_object_t *o)
{ return __atomic_add_fetch(&o->ref_count, 1, __ATOMIC_RELAXED) != 0; }
static void object_release(kernel_object_t *o)
{ assert(o && __atomic_load_n(&o->ref_count, __ATOMIC_RELAXED));
  __atomic_sub_fetch(&o->ref_count, 1, __ATOMIC_RELAXED); }
static kernel_object_t *process_handle_lookup_any(process_t *p,
        process_handle_t h, int type, u32 rights)
{
    (void)rights; assert(p == &parent && type == OBJECT_TYPE_PROCESS);
    assert(h == 77 || h == 78);
    object_reference(&child.object); return &child.object;
}
static process_t *object_process(kernel_object_t *o) { return o->process; }
static void process_handle_reference_release(kernel_object_t *o)
{ object_release(o); }
static bool process_finalize_exit(process_t *p)
{ return p && p->state == PROCESS_STATE_TERMINATED; }
static void process_unlink_locked(process_t *p)
{ assert(tls_lock_depth && p == &child); p->parent=NULL; parent.first_child=NULL; }
static bool process_cleanup_worker_wake(process_t *p) { (void)p; return true; }
static void process_worker_reclaim_pending(process_t *p) { (void)p; }

/* Host dispatch waits on the same scheduler lock used by the extracted
 * production wake/block functions. The condition variable models the actual
 * context switch; state tests and event-latch consumption remain production. */
static bool scheduler_enqueue_locked(kernel_thread_t *thread)
{
    thread->queued=true;
    pthread_cond_broadcast(&scheduler_cv);
    return true;
}
static u32 cpu_current_index(void) { return 0; }
static bool scheduler_handoff(kernel_thread_t *outgoing,
                              kernel_thread_t *target, u64 flags)
{ (void)outgoing; (void)target; (void)flags; return false; }
static void sleeping_remove(kernel_thread_t *thread)
{ thread->sleeping=false; thread->next=NULL; }
static bool scheduler_prepare_dispatch_locked(int action,
        kernel_thread_t **outgoing, kernel_thread_t **target, bool *handoff)
{
    (void)action;
    *outgoing=current_thread; *target=NULL; *handoff=false;
    pthread_mutex_lock(&gate_lock);
    gate_block_count++;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    while (current_thread->state == THREAD_STATE_BLOCKED)
        pthread_cond_wait(&scheduler_cv, &scheduler_lock);
    assert(current_thread->state == THREAD_STATE_READY &&
           current_thread->queued);
    current_thread->queued=false;
    current_thread->state=THREAD_STATE_RUNNING;
    current_thread->running_cpu=0;
    return true;
}
static void fixture_before_waiter_delivery(process_t *p,
                                           kernel_thread_t *thread)
{
    (void)p; (void)thread;
    pthread_mutex_lock(&gate_lock);
    if (delay_delivery) {
        delivery_entered=true;
        pthread_cond_broadcast(&gate_cv);
        while (!release_delivery) pthread_cond_wait(&gate_cv, &gate_lock);
    }
    pthread_mutex_unlock(&gate_lock);
}
'''

process = (ROOT / 'kernel/src/process.c').read_text()
scheduler = (ROOT / 'kernel/src/scheduler.c').read_text()
code = prefix
for name in ('scheduler_thread_exit_complete', 'scheduler_unblock_locked',
             'scheduler_wake_waiter', 'scheduler_consume_pending_wake_locked',
             'scheduler_block'):
    item = function(scheduler, name)
    if name == 'scheduler_thread_exit_complete':
        item = item.replace('bool scheduler_thread_exit_complete(',
                            'static bool scheduler_thread_exit_complete(', 1)
    if name == 'scheduler_unblock_locked':
        item = item.replace('static bool scheduler_unblock_locked(',
                            'static bool scheduler_unblock_locked(', 1)
    if name in ('scheduler_wake_waiter', 'scheduler_block'):
        item = item.replace('bool ' + name + '(', 'static bool ' + name + '(', 1)
    code += item
for name in ('process_storage_authorization_clear_locked',
             'process_thread_list_complete_locked',
             'process_thread_list_read_acquire_locked',
             'process_thread_list_read_release',
             'process_waiter_wake_release',
             'process_publish_terminated_locked',
             'process_finish_termination', 'process_wait'):
    code += function(process, name)
code = code.replace('woke = scheduler_wake_waiter(waiter);',
                    'fixture_before_waiter_delivery(process, waiter);\n'
                    '    woke = scheduler_wake_waiter(waiter);')

code += r'''
static void complete_child(process_t *p, kernel_thread_t *t, i32 status)
{
    u64 f=spin_lock_irqsave(&process_registry_lock);
    p->state=PROCESS_STATE_EXITING; p->exit_cleanup_complete=true;
    p->exit_status=status; p->live_thread_count=0;
    t->state=THREAD_STATE_TERMINATED; t->running_cpu=THREAD_CPU_NONE;
    t->context_switch_pending=false; t->sleeping=false;
    t->preempt_return_rip=0; t->process_state=THREAD_PROCESS_HANDOFF_COMPLETE;
    spin_unlock_irqrestore(&process_registry_lock,f);
    assert(process_finish_termination(p));
}
static void setup(void)
{
    memset(&parent,0,sizeof(parent)); memset(&child,0,sizeof(child));
    memset(&other_child,0,sizeof(other_child));
    memset(&parent_thread,0,sizeof(parent_thread));
    memset(&second_parent_thread,0,sizeof(second_parent_thread));
    memset(&child_thread,0,sizeof(child_thread));
    memset(&other_child_thread,0,sizeof(other_child_thread));
    parent.state=PROCESS_STATE_ACTIVE; parent.active_syscalls=1;
    parent.threads=parent.main_thread=&parent_thread; parent.thread_count=1;
    parent_thread.process=&parent; parent_thread.process_initial=true;
    parent_thread.process_reference_held=true;
    parent_thread.process_state=THREAD_PROCESS_ATTACHED;
    parent_thread.state=THREAD_STATE_RUNNING; parent_thread.running_cpu=0;
    current_thread=&parent_thread; tls_current_thread=&parent_thread;
    child.state=PROCESS_STATE_ACTIVE; child.parent=&parent;
    child.threads=child.main_thread=&child_thread; child.thread_count=1;
    child.live_thread_count=1; child.object.ref_count=1;
    child.object.process=&child; child.owner_reference_held=true;
    child_thread.process=&child; child_thread.process_initial=true;
    child_thread.process_reference_held=true;
    child_thread.process_state=THREAD_PROCESS_ATTACHED;
    child_thread.state=THREAD_STATE_RUNNING; child_thread.running_cpu=1;
    parent.first_child=&child;
    other_child.state=PROCESS_STATE_ACTIVE; other_child.parent=&parent;
    other_child.threads=other_child.main_thread=&other_child_thread;
    other_child.thread_count=1; other_child.live_thread_count=1;
    other_child.object.ref_count=1; other_child.object.process=&other_child;
    other_child_thread.process=&other_child; other_child_thread.process_initial=true;
    other_child_thread.process_reference_held=true;
    other_child_thread.process_state=THREAD_PROCESS_ATTACHED;
    other_child_thread.state=THREAD_STATE_RUNNING;
    scheduler_stats=(scheduler_stats_t){0}; sleeping_threads=NULL;
    preemption_pending=false; gate_block_count=0;
    complete_on_registration_unlock=false; delay_delivery=false;
    delivery_entered=false; release_delivery=false;
    second_wait_result_ready=false; second_wait_result=0;
    tls_if=true; tls_lock_depth=0; tls_saved_count=0;
}
static void *wait_call(void *unused)
{
    (void)unused; tls_current_thread=&parent_thread;
    i32 status=-1;
    int rc=process_wait(&parent,77,&status);
    u64 packed=(u32)rc | ((u64)(u32)status << 32);
    return (void *)(uintptr_t)packed;
}
static void wait_for_block(unsigned count)
{
    pthread_mutex_lock(&gate_lock);
    while (gate_block_count<count) pthread_cond_wait(&gate_cv,&gate_lock);
    pthread_mutex_unlock(&gate_lock);
}
static void *complete_after_block(void *unused)
{
    (void)unused; wait_for_block(1); complete_child(&child,&child_thread,23);
    return NULL;
}
static void *spurious_then_complete(void *unused)
{
    (void)unused; wait_for_block(1);
    assert(scheduler_wake_waiter(&parent_thread));
    wait_for_block(2);
    complete_child(&other_child,&other_child_thread,91);
    assert(parent_thread.state==THREAD_STATE_BLOCKED);
    assert(!parent_thread.event_wake_pending);
    complete_child(&child,&child_thread,23);
    return NULL;
}
static void *terminate_parent_while_waiting(void *unused)
{
    (void)unused; wait_for_block(1);
    u64 f=spin_lock_irqsave(&process_registry_lock);
    parent.state=PROCESS_STATE_EXITING; parent.exit_requested=true;
    spin_unlock_irqrestore(&process_registry_lock,f);
    assert(scheduler_wake_waiter(&parent_thread));
    return NULL;
}
static void *delayed_completion(void *unused)
{
    (void)unused; wait_for_block(1);
    complete_child(&child,&child_thread,23);
    return NULL;
}
static void *waiter_main(void *unused)
{
    (void)unused; tls_current_thread=&parent_thread;
    i32 status=-1; int rc=process_wait(&parent,77,&status);
    pthread_mutex_lock(&gate_lock);
    second_wait_result=rc; second_wait_result_ready=true;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    return (void *)(intptr_t)status;
}
static void assert_wait_success(void *encoded)
{
    intptr_t v=(intptr_t)encoded;
    int rc=(i32)(u32)(uintptr_t)v;
    i32 status=(i32)((u64)(uintptr_t)v >> 32);
    if (rc!=MG_OK || status!=23 || parent.thread_list_readers ||
        parent.waiting_child || parent.waiting_thread || !child.wait_collected)
        fprintf(stderr,"wait rc=%d status=%d readers=%u child=%p waiter=%p collected=%d blocks=%u\n",
            rc,status,parent.thread_list_readers,(void *)parent.waiting_child,
            (void *)parent.waiting_thread,child.wait_collected,gate_block_count);
    assert(rc==MG_OK && status==23);
    assert(parent.thread_list_readers==0 && !parent.waiting_child &&
           !parent.waiting_thread && child.wait_collected);
}
int main(void)
{
    pthread_t worker, waiter;
    setup();
    /* A child published before registration needs no scheduler event. */
    complete_child(&child,&child_thread,23);
    assert_wait_success(wait_call(NULL));

    setup(); complete_on_registration_unlock=true;
    assert_wait_success(wait_call(NULL));
    assert(gate_block_count==0); /* event latch was consumed before parking */

    setup();
    assert(!pthread_create(&worker,NULL,complete_after_block,NULL));
    assert_wait_success(wait_call(NULL)); pthread_join(worker,NULL);
    assert(gate_block_count==1);

    setup();
    assert(!pthread_create(&worker,NULL,spurious_then_complete,NULL));
    assert_wait_success(wait_call(NULL)); pthread_join(worker,NULL);
    assert(gate_block_count==2 && other_child.state==PROCESS_STATE_TERMINATED);

    /* A second process thread cannot replace the existing process-wide wait
     * slot. Its rejected call must not disturb the first waiter's reader pin. */
    setup(); second_parent_thread.process=&parent;
    second_parent_thread.process_state=THREAD_PROCESS_ATTACHED;
    second_parent_thread.state=THREAD_STATE_RUNNING;
    parent_thread.process_next=&second_parent_thread; parent.thread_count=2;
    assert(!pthread_create(&worker,NULL,complete_after_block,NULL));
    assert(!pthread_create(&waiter,NULL,waiter_main,NULL));
    wait_for_block(1);
    tls_current_thread=&second_parent_thread;
    i32 status=-1;
    assert(process_wait(&parent,78,&status)==MG_ERR_BUSY);
    assert(parent.thread_list_readers==1);
    pthread_join(waiter,NULL); pthread_join(worker,NULL);
    assert(second_wait_result==MG_OK && child.wait_collected &&
           parent.thread_list_readers==0);

    setup();
    assert(!pthread_create(&worker,NULL,terminate_parent_while_waiting,NULL));
    void *cancel_result=wait_call(NULL); pthread_join(worker,NULL);
    assert((i32)(u32)(uintptr_t)cancel_result==MG_ERR_SHUTDOWN);
    assert(parent.thread_list_readers==0 && !parent.waiting_child &&
           child.state==PROCESS_STATE_ACTIVE);

    /* Hold completion between waiter capture and delivery. The waiter is
     * interrupted and unwinds its registration pin first; the publisher's
     * independent pin must keep the thread record/list alive until delivery. */
    setup(); delay_delivery=true;
    assert(!pthread_create(&worker,NULL,delayed_completion,NULL));
    assert(!pthread_create(&waiter,NULL,waiter_main,NULL));
    pthread_mutex_lock(&gate_lock);
    while(!delivery_entered) pthread_cond_wait(&gate_cv,&gate_lock);
    pthread_mutex_unlock(&gate_lock);
    u64 f=spin_lock_irqsave(&process_registry_lock);
    parent.state=PROCESS_STATE_EXITING; parent.exit_requested=true;
    parent.exit_cleanup_complete=true; parent.live_thread_count=0;
    spin_unlock_irqrestore(&process_registry_lock,f);
    assert(scheduler_wake_waiter(&parent_thread));
    pthread_join(waiter,NULL);
    assert(second_wait_result==MG_ERR_SHUTDOWN);
    assert(parent.thread_list_readers==1);
    f=spin_lock_irqsave(&process_registry_lock);
    parent_thread.state=THREAD_STATE_TERMINATED;
    parent_thread.running_cpu=THREAD_CPU_NONE;
    parent_thread.process_state=THREAD_PROCESS_HANDOFF_COMPLETE;
    parent.active_syscalls=0;
    assert(!process_publish_terminated_locked(&parent,NULL));
    assert(parent.state==PROCESS_STATE_EXITING &&
           parent.thread_list_readers==1);
    spin_unlock_irqrestore(&process_registry_lock,f);
    pthread_mutex_lock(&gate_lock); release_delivery=true;
    pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate_lock);
    pthread_join(worker,NULL);
    assert(parent.thread_list_readers==0 &&
           parent.state==PROCESS_STATE_TERMINATED);
    puts("process wait: latched wake, predicate retry, cancellation, and waiter lifetime passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-process-wait-') as directory:
    path = Path(directory) / 'process_wait_event.c'
    binary = Path(directory) / 'process_wait_event'
    path.write_text(code)
    env = os.environ.copy()
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    subprocess.run([CC, '-std=gnu17', '-g', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', str(path),
                    '-o', str(binary)], check=True, env=env)
    try:
        subprocess.run([str(binary)], check=True, env=env, timeout=30)
    except subprocess.CalledProcessError:
        evidence = ROOT / 'build' / 'stage6-process-wait-fix-20261029' / \
            f'process_wait_event-failure-{os.getpid()}.c'
        evidence.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, evidence)
        print(f'failed production-source fixture retained at {evidence}',
              file=sys.stderr)
        raise
