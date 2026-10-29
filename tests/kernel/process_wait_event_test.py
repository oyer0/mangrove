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
typedef enum { SCHEDULER_WAKE_FAILED=0, SCHEDULER_WAKE_DELIVERED,
               SCHEDULER_WAKE_ALREADY_TERMINATED } scheduler_wake_result_t;
enum { THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
       THREAD_PROCESS_EXIT_REQUESTED, THREAD_PROCESS_HANDOFF_COMPLETE };
enum { THREAD_PRIORITY_HIGH, THREAD_PRIORITY_NORMAL,
       THREAD_PRIORITY_BACKGROUND };
enum { MG_OK=0, MG_ERR_BAD_ARGUMENT=-4, MG_ERR_INVALID_HANDLE=-8,
       MG_ERR_BUSY=-14, MG_ERR_SHUTDOWN=-38, MG_ERR_WOULD_BLOCK=-19 };
#define OBJECT_TYPE_PROCESS 3
#define PROCESS_NO_SESSION 0U
#define THREAD_CPU_NONE UINT32_MAX
#define SCHEDULER_DISPATCH_BLOCK 1
#define SCHEDULER_DISPATCH_TERMINATE 2
typedef u32 thread_state_t;
typedef pthread_mutex_t spinlock_t;
typedef u32 thread_priority_t;
typedef struct process process_t;
typedef struct kernel_object kernel_object_t;
struct kernel_object {
    u32 ref_count;
    process_t *process;
    int type;
    void (*destroy)(kernel_object_t *);
};
typedef struct kernel_thread {
    u32 state, running_cpu;
    bool context_switch_pending, syscall_active, sleeping, queued;
    bool wakeup_boosted, event_wake_pending, mutex_wake_pending;
    bool waiting_mutex;
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
typedef struct process_thread_record {
    process_t *process;
    kernel_thread_t *execution;
    kernel_thread_t *join_waiter;
    bool join_claimed, listed;
    bool completed, kernel_stack_released;
} process_thread_record_t;
struct process {
    int state;
    bool wait_collected, owner_reference_held, shutdown_requested;
    bool exit_requested, termination_pending, external_termination_in_progress;
    bool launch_ready, wait_claimed, exit_cleanup_complete;
    bool exit_cleanup_in_progress, thread_reap_in_progress;
    bool exit_notification_sent;
    bool deferred_reference_collection_retry;
    i32 exit_status; u64 session_id;
    u32 active_syscalls, thread_count, live_thread_count;
    u32 thread_list_readers, worker_reapers;
    u32 scheduler_handoff_references;
    _Atomic u32 deferred_reference_releases_pending;
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
static bool scheduler_enqueue_fails;
static bool finalize_exit_available;
static process_t parent, child, other_child;
static kernel_thread_t parent_thread, second_parent_thread, child_thread,
                       other_child_thread, worker_wait_thread;
static _Thread_local kernel_thread_t *tls_current_thread;
static _Thread_local bool tls_if = true;
static _Thread_local unsigned tls_lock_depth;
static unsigned registry_if_violations;
static unsigned gate_block_count;
static bool complete_on_registration_unlock;
static bool delay_delivery, delivery_entered, release_delivery;
static bool second_wait_result_ready;
static int second_wait_result;
static kernel_thread_t *selected_waiter;
static process_t *collection_interleave_process;
static unsigned dynamic_process_destroyed;
static u32 dynamic_parent_refs_after_concurrent_owner_drop;

static bool process_finish_termination(process_t *);
static bool process_cleanup_worker_wake(process_t *);
static void process_worker_reclaim_pending(process_t *);
static void process_cleanup_worker_request_session_collection_retry(void);
static void process_reap_session_members(mg_session_id_t);
static bool process_cleanup_context_valid(void);
static bool object_reference(kernel_object_t *);
static void object_release(kernel_object_t *);
static void process_handle_reference_release(kernel_object_t *);
static bool scheduler_prepare_dispatch_locked(int, kernel_thread_t **,
                                             kernel_thread_t **, bool *);
static bool scheduler_terminate(void);
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
    pthread_mutex_lock(lock);
    tls_lock_depth++;
    if (lock == &process_registry_lock && tls_if)
        __atomic_add_fetch(&registry_if_violations,1U,__ATOMIC_RELAXED);
}
static void spin_unlock(spinlock_t *lock)
{
    assert(tls_lock_depth); tls_lock_depth--;
    pthread_mutex_unlock(lock);
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
static void kfree(void *p) { free(p); }
static kernel_object_t *process_handle_lookup_any(process_t *p,
        process_handle_t h, int type, u32 rights)
{
    (void)rights; assert(p == &parent && type == OBJECT_TYPE_PROCESS);
    assert(h == 77 || h == 78);
    object_reference(&child.object); return &child.object;
}
static process_t *object_process(kernel_object_t *o) { return o->process; }
static bool process_finalize_exit(process_t *p)
{ return finalize_exit_available && p &&
         p->state == PROCESS_STATE_TERMINATED; }
static void process_unlink_locked(process_t *p)
{ assert(tls_lock_depth && p == &child); p->parent=NULL; parent.first_child=NULL; }
static bool process_cleanup_worker_wake(process_t *p) { (void)p; return true; }
static void process_worker_reclaim_pending(process_t *p) { (void)p; }
static void process_cleanup_worker_request_session_collection_retry(void) {}
static void process_reap_session_members(mg_session_id_t id) { assert(!id); }
static bool process_cleanup_context_valid(void)
{ return tls_current_thread && !tls_current_thread->waiting_mutex &&
         !tls_lock_depth && tls_if; }
static void fixture_object_destroy_noop(kernel_object_t *object)
{ (void)object; }
static void fixture_dynamic_process_destroy(kernel_object_t *object)
{
    process_t *p = object->process;
    assert(p && !p->threads && !p->thread_count && !p->main_thread);
    dynamic_process_destroyed++;
    free(p);
}
static void fixture_collect_after_reader_drop(process_t *p)
{
    if (p != collection_interleave_process) return;
    collection_interleave_process = NULL;
    assert(!p->thread_list_readers && p->owner_reference_held &&
           p->thread_count == 1 && p->threads);
    /* Model a concurrent, already-authorized final collection in the exact
     * gap after the last reader count is dropped and before its follow-up. */
    p->state = PROCESS_STATE_TERMINATED;
    p->owner_reference_held = false;
    p->thread_count = p->live_thread_count = 0;
    p->threads = p->main_thread = NULL;
    object_release(&p->object); /* process owner */
    object_release(&p->object); /* attached-thread ownership */
    dynamic_parent_refs_after_concurrent_owner_drop =
        __atomic_load_n(&p->object.ref_count, __ATOMIC_ACQUIRE);
}

/* Host dispatch waits on the same scheduler lock used by the extracted
 * production wake/block functions. The condition variable models the actual
 * context switch; state tests and event-latch consumption remain production. */
static bool scheduler_enqueue_locked(kernel_thread_t *thread)
{
    if (scheduler_enqueue_fails) return false;
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
    *outgoing=current_thread; *target=NULL; *handoff=false;
    if (action == SCHEDULER_DISPATCH_TERMINATE) {
        assert(current_thread &&
               current_thread->state==THREAD_STATE_TERMINATED);
        current_thread->running_cpu=THREAD_CPU_NONE;
        return true;
    }
    assert(action == SCHEDULER_DISPATCH_BLOCK);
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
objects = (ROOT / 'kernel/src/object.c').read_text()
code = prefix
for name in ('scheduler_thread_exit_complete',
             'scheduler_thread_handoff_callback_valid',
             'scheduler_thread_state_snapshot',
             'scheduler_unblock_locked',
             'scheduler_wake_waiter_result', 'scheduler_wake_waiter',
             'scheduler_consume_pending_wake_locked',
             'scheduler_block', 'scheduler_terminate'):
    item = function(scheduler, name)
    if name == 'scheduler_thread_exit_complete':
        item = item.replace('bool scheduler_thread_exit_complete(',
                            'static bool scheduler_thread_exit_complete(', 1)
    if name == 'scheduler_unblock_locked':
        item = item.replace('static bool scheduler_unblock_locked(',
                            'static bool scheduler_unblock_locked(', 1)
    if name in ('scheduler_wake_waiter_result', 'scheduler_wake_waiter',
                'scheduler_block',
                'scheduler_terminate'):
        return_type = ('scheduler_wake_result_t' if
                       name == 'scheduler_wake_waiter_result' else 'bool')
        item = item.replace(return_type + ' ' + name + '(',
                            'static ' + return_type + ' ' + name + '(', 1)
    code += item
for name in ('object_reference', 'object_release'):
    item = function(objects, name)
    if name == 'object_reference':
        item = item.replace('bool object_reference(',
                            'static bool object_reference(', 1)
    else:
        item = item.replace('void object_release(',
                            'static void object_release(', 1)
    code += item
for name in ('process_storage_authorization_clear_locked',
             'process_thread_list_complete_locked',
             'process_thread_member_locked',
             'process_thread_list_read_acquire_locked',
             'process_thread_list_read_release',
             'process_waiter_wake_release',
             'process_publish_terminated_locked',
             'process_finish_termination', 'process_handle_reference_release',
             'process_scheduler_handoff_complete_internal',
             'process_scheduler_handoff_complete', 'process_wait'):
    item = function(process, name)
    if name == 'process_thread_list_read_release':
        marker = ('spin_unlock_irqrestore(&process_registry_lock, flags);\n'
                  '    if (reap_workers)')
        item = item.replace(marker,
            'spin_unlock_irqrestore(&process_registry_lock, flags);\n'
            '    fixture_collect_after_reader_drop(process);\n'
            '    if (reap_workers)', 1)
    code += item
code = code.replace('woke = scheduler_wake_waiter(waiter);',
                    'fixture_before_waiter_delivery(process, waiter);\n'
                    '    woke = scheduler_wake_waiter(waiter);')

code += r'''
static void complete_child(process_t *p, kernel_thread_t *t, i32 status)
{
    kernel_thread_t *saved_current = tls_current_thread;
    bool saved_if = tls_if;
    tls_current_thread = t;
    tls_if = true;
    u64 f=spin_lock_irqsave(&process_registry_lock);
    p->state=PROCESS_STATE_EXITING; p->exit_cleanup_complete=true;
    p->exit_status=status; p->live_thread_count=0;
    t->state=THREAD_STATE_TERMINATED; t->running_cpu=THREAD_CPU_NONE;
    t->context_switch_pending=false; t->sleeping=false;
    t->preempt_return_rip=0; t->process_state=THREAD_PROCESS_HANDOFF_COMPLETE;
    spin_unlock_irqrestore(&process_registry_lock,f);
    assert(process_finish_termination(p));
    tls_current_thread = saved_current;
    tls_if = saved_if;
}
static void setup(void)
{
    memset(&parent,0,sizeof(parent)); memset(&child,0,sizeof(child));
    memset(&other_child,0,sizeof(other_child));
    memset(&parent_thread,0,sizeof(parent_thread));
    memset(&second_parent_thread,0,sizeof(second_parent_thread));
    memset(&child_thread,0,sizeof(child_thread));
    memset(&other_child_thread,0,sizeof(other_child_thread));
    memset(&worker_wait_thread,0,sizeof(worker_wait_thread));
    parent.state=PROCESS_STATE_ACTIVE; parent.active_syscalls=1;
    parent.threads=parent.main_thread=&parent_thread; parent.thread_count=1;
    parent.object=(kernel_object_t){.ref_count=2,.process=&parent,
        .type=OBJECT_TYPE_PROCESS,.destroy=fixture_object_destroy_noop};
    parent_thread.process=&parent; parent_thread.process_initial=true;
    parent_thread.process_reference_held=true;
    parent_thread.process_state=THREAD_PROCESS_ATTACHED;
    parent_thread.state=THREAD_STATE_RUNNING; parent_thread.running_cpu=0;
    current_thread=&parent_thread; tls_current_thread=&parent_thread;
    child.state=PROCESS_STATE_ACTIVE; child.parent=&parent;
    child.threads=child.main_thread=&child_thread; child.thread_count=1;
    child.live_thread_count=1; child.object.ref_count=1;
    child.object.process=&child; child.object.type=OBJECT_TYPE_PROCESS;
    child.object.destroy=fixture_object_destroy_noop;
    child.owner_reference_held=true;
    child_thread.process=&child; child_thread.process_initial=true;
    child_thread.process_reference_held=true;
    child_thread.process_state=THREAD_PROCESS_ATTACHED;
    child_thread.state=THREAD_STATE_RUNNING; child_thread.running_cpu=1;
    parent.first_child=&child;
    other_child.state=PROCESS_STATE_ACTIVE; other_child.parent=&parent;
    other_child.threads=other_child.main_thread=&other_child_thread;
    other_child.thread_count=1; other_child.live_thread_count=1;
    other_child.object.ref_count=1; other_child.object.process=&other_child;
    other_child.object.type=OBJECT_TYPE_PROCESS;
    other_child.object.destroy=fixture_object_destroy_noop;
    other_child_thread.process=&other_child; other_child_thread.process_initial=true;
    other_child_thread.process_reference_held=true;
    other_child_thread.process_state=THREAD_PROCESS_ATTACHED;
    other_child_thread.state=THREAD_STATE_RUNNING;
    scheduler_stats=(scheduler_stats_t){0}; sleeping_threads=NULL;
    preemption_pending=false; gate_block_count=0;
    scheduler_enqueue_fails=false;
    finalize_exit_available=true;
    complete_on_registration_unlock=false; delay_delivery=false;
    delivery_entered=false; release_delivery=false;
    second_wait_result_ready=false; second_wait_result=0;
    selected_waiter=&parent_thread; collection_interleave_process=NULL;
    registry_if_violations=0; dynamic_parent_refs_after_concurrent_owner_drop=0;
    tls_if=true; tls_lock_depth=0;
}
static void *wait_call(void *unused)
{
    (void)unused; tls_current_thread=selected_waiter;
    i32 status=-1;
    int rc=process_wait(&parent,77,&status);
    if (!tls_if || tls_lock_depth)
        fprintf(stderr,"wait exit state rc=%d IF=%d depth=%u blocks=%u\n",
            rc,tls_if,tls_lock_depth,gate_block_count);
    assert(tls_if && tls_lock_depth==0);
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
    (void)unused; tls_current_thread=selected_waiter;
    i32 status=-1; int rc=process_wait(&parent,77,&status);
    assert(tls_if && tls_lock_depth==0);
    pthread_mutex_lock(&gate_lock);
    second_wait_result=rc; second_wait_result_ready=true;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    return (void *)(intptr_t)status;
}
static void *retire_waiter_after_wait(void *unused)
{
    (void)unused;
    tls_current_thread=selected_waiter;
    i32 status=-1;
    int rc=process_wait(&parent,77,&status);
    assert(rc==MG_OK && status==23);
    assert(tls_if && tls_lock_depth==0);
    current_thread=selected_waiter;
    assert(scheduler_terminate());
    process_scheduler_handoff_complete(selected_waiter);
    assert(selected_waiter->process_state==THREAD_PROCESS_HANDOFF_COMPLETE);
    pthread_mutex_lock(&gate_lock);
    second_wait_result=rc; second_wait_result_ready=true;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    return NULL;
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
    assert(tls_if && tls_lock_depth==0 && registry_if_violations==0);
}
static void run_parent_object_lifetime_interleaving(void)
{
    process_t *dynamic_parent;
    kernel_thread_t *waiter=&parent_thread;
    kernel_thread_t *published_waiter;
    process_t *delivery_process=NULL;
    setup();
    dynamic_parent=calloc(1,sizeof(*dynamic_parent));
    assert(dynamic_parent);
    dynamic_process_destroyed=0;
    dynamic_parent_refs_after_concurrent_owner_drop=0;
    dynamic_parent->state=PROCESS_STATE_ACTIVE;
    dynamic_parent->owner_reference_held=true;
    dynamic_parent->launch_ready=true;
    dynamic_parent->thread_count=1;
    dynamic_parent->live_thread_count=1;
    dynamic_parent->threads=dynamic_parent->main_thread=waiter;
    dynamic_parent->object=(kernel_object_t){.ref_count=2,
        .process=dynamic_parent,.type=OBJECT_TYPE_PROCESS,
        .destroy=fixture_dynamic_process_destroy};
    waiter->process=dynamic_parent;
    waiter->process_state=THREAD_PROCESS_ATTACHED;
    waiter->process_initial=true;
    waiter->process_reference_held=true;
    waiter->state=THREAD_STATE_RUNNING;
    waiter->running_cpu=0;
    dynamic_parent->waiting_child=&child;
    dynamic_parent->waiting_thread=waiter;
    child.parent=dynamic_parent;
    child.state=PROCESS_STATE_EXITING;
    child.exit_cleanup_complete=true;
    child.live_thread_count=0;
    child_thread.state=THREAD_STATE_TERMINATED;
    child_thread.running_cpu=THREAD_CPU_NONE;
    child_thread.context_switch_pending=false;
    child_thread.sleeping=false;
    child_thread.preempt_return_rip=0;
    child_thread.process_state=THREAD_PROCESS_HANDOFF_COMPLETE;

    u64 f=spin_lock_irqsave(&process_registry_lock);
    published_waiter=process_publish_terminated_locked(&child,
                                                        &delivery_process);
    spin_unlock_irqrestore(&process_registry_lock,f);
    assert(published_waiter==waiter && delivery_process==dynamic_parent);
    assert(dynamic_parent->thread_list_readers==1 &&
           dynamic_parent->object.ref_count==3);

    /* Final session collection overlaps the last reader's unlocked follow-up.
     * The hook drops owner/thread refs after the reader reaches zero but
     * before process_finish_termination() resumes. */
    dynamic_parent->state=PROCESS_STATE_EXITING;
    dynamic_parent->exit_cleanup_complete=true;
    collection_interleave_process=dynamic_parent;
    tls_current_thread=waiter; tls_if=true; tls_lock_depth=0;
    process_waiter_wake_release(delivery_process,published_waiter);
    assert(!collection_interleave_process);
    assert(dynamic_parent_refs_after_concurrent_owner_drop==1);
    assert(dynamic_process_destroyed==1);
}

static void run_invalid_waiter_ownership(void)
{
    setup();
    parent.thread_list_readers=1;
    parent_thread.state=THREAD_STATE_TERMINATED;
    parent_thread.running_cpu=THREAD_CPU_NONE;
    parent_thread.process_state=THREAD_PROCESS_HANDOFF_COMPLETE;
    parent_thread.process_next=NULL;
    parent.threads=&second_parent_thread;
    second_parent_thread.process=&parent;
    second_parent_thread.process_state=THREAD_PROCESS_ATTACHED;
    assert(object_reference(&parent.object));
    tls_current_thread=&parent_thread; tls_if=true; tls_lock_depth=0;
    process_waiter_wake_release(&parent,&parent_thread);
    panic("invalid waiter ownership was not rejected");
}

static void run_live_waiter_wake_failure(void)
{
    setup();
    parent.thread_list_readers=1;
    parent_thread.state=THREAD_STATE_BLOCKED;
    parent_thread.running_cpu=THREAD_CPU_NONE;
    parent_thread.queued=false;
    assert(object_reference(&parent.object));
    scheduler_enqueue_fails=true;
    tls_current_thread=&child_thread; tls_if=true; tls_lock_depth=0;
    process_waiter_wake_release(&parent,&parent_thread);
    panic("live waiter wake failure was not rejected");
}

static void run_terminated_worker_delivery(void)
{
    pthread_t completer, waiter;
    setup();
    selected_waiter=&worker_wait_thread;
    parent.threads=&worker_wait_thread;
    worker_wait_thread.process_next=&parent_thread;
    parent.thread_count=2; parent.live_thread_count=2;
    parent.object.ref_count=3; /* owner and two attached-thread refs */
    worker_wait_thread.process=&parent;
    worker_wait_thread.process_state=THREAD_PROCESS_ATTACHED;
    worker_wait_thread.process_reference_held=true;
    worker_wait_thread.state=THREAD_STATE_RUNNING;
    worker_wait_thread.running_cpu=1;
    delay_delivery=true;
    current_thread=&worker_wait_thread;
    assert(!pthread_create(&completer,NULL,delayed_completion,NULL));
    assert(!pthread_create(&waiter,NULL,retire_waiter_after_wait,NULL));
    pthread_mutex_lock(&gate_lock);
    while(!delivery_entered) pthread_cond_wait(&gate_cv,&gate_lock);
    pthread_mutex_unlock(&gate_lock);

    /* An unrelated event lets the worker consume the completed child result
     * and return before the completion publisher sends its delayed wake. */
    assert(scheduler_wake_waiter(&worker_wait_thread));
    pthread_join(waiter,NULL);
    assert(second_wait_result==MG_OK && child.wait_collected);
    assert(worker_wait_thread.state==THREAD_STATE_TERMINATED &&
           worker_wait_thread.process_state==THREAD_PROCESS_HANDOFF_COMPLETE);
    assert(parent.state==PROCESS_STATE_ACTIVE && parent.thread_list_readers==1);

    pthread_mutex_lock(&gate_lock); release_delivery=true;
    pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate_lock);
    pthread_join(completer,NULL);
    assert(parent.thread_list_readers==0 && parent.object.ref_count==3);
    assert(parent.thread_count==2 && parent.live_thread_count==1);
}

static void run_wait_return_interrupt_state(void)
{
    i32 status=-1;
    setup();
    parent.shutdown_requested=true;
    assert(process_wait(&parent,77,&status)==MG_ERR_SHUTDOWN);
    assert(tls_if && tls_lock_depth==0);

    setup();
    complete_child(&child,&child_thread,23);
    child.wait_claimed=true;
    assert(process_wait(&parent,77,&status)==MG_ERR_BUSY);
    assert(tls_if && tls_lock_depth==0 && child.wait_claimed);

    /* A scheduler refusal after registration must remove the process-wide
     * waiter slot and reader pin, preserving either original IF state. */
    setup();
    current_thread=NULL;
    assert(process_wait(&parent,77,&status)==MG_ERR_BUSY);
    assert(tls_if && tls_lock_depth==0 && !parent.waiting_thread &&
           !parent.waiting_child && !parent.thread_list_readers);

    setup();
    current_thread=NULL;
    tls_if=false;
    assert(process_wait(&parent,77,&status)==MG_ERR_BUSY);
    assert(!tls_if && tls_lock_depth==0 && !parent.waiting_thread &&
           !parent.waiting_child && !parent.thread_list_readers);

    /* A failed safe finalization releases the claim and returns with the
     * caller's original interrupt state rather than leaving IF masked. */
    setup();
    complete_child(&child,&child_thread,23);
    finalize_exit_available=false;
    assert(process_wait(&parent,77,&status)==MG_ERR_BUSY);
    assert(tls_if && tls_lock_depth==0 && !child.wait_claimed &&
           !child.wait_collected);
}

int main(int argc, char **argv)
{
    pthread_t worker, waiter;
    if (argc > 1 && !strcmp(argv[1], "invalid-waiter-owner"))
        run_invalid_waiter_ownership();
    if (argc > 1 && !strcmp(argv[1], "live-wake-failure"))
        run_live_waiter_wake_failure();
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
    assert(tls_if && tls_lock_depth==0);
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
    assert(parent.object.ref_count==2);

    run_parent_object_lifetime_interleaving();
    run_terminated_worker_delivery();
    run_wait_return_interrupt_state();
    assert(registry_if_violations==0);
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
        command = [str(binary)]
        completed = subprocess.run(command, check=False, env=env, timeout=30,
                                   text=True, capture_output=True)
        if completed.returncode:
            raise subprocess.CalledProcessError(
                completed.returncode, command, completed.stdout,
                completed.stderr)
        sys.stdout.write(completed.stdout)
        invalid = subprocess.run([str(binary), 'invalid-waiter-owner'],
                                 env=env, text=True, capture_output=True,
                                 timeout=30)
        if invalid.returncode == 0 or (
                'child waiter lost process ownership' not in
                invalid.stderr):
            raise RuntimeError(
                'invalid retained waiter ownership was not rejected: '
                f'exit={invalid.returncode}; stderr={invalid.stderr!r}')
        live_failure = subprocess.run([str(binary), 'live-wake-failure'],
                                      env=env, text=True, capture_output=True,
                                      timeout=30)
        if live_failure.returncode == 0 or (
                'live child waiter could not be woken' not in
                live_failure.stderr):
            raise RuntimeError(
                'live waiter wake failure was not rejected: '
                f'exit={live_failure.returncode}; '
                f'stderr={live_failure.stderr!r}')
    except (subprocess.CalledProcessError, RuntimeError,
            subprocess.TimeoutExpired) as error:
        evidence_dir = ROOT / 'build' / 'stage6-process-wait-repair-20261029'
        evidence_dir.mkdir(parents=True, exist_ok=True)
        evidence = evidence_dir / \
            f'process_wait_event-failure-{os.getpid()}.c'
        shutil.copy2(path, evidence)
        (evidence_dir / f'process_wait_event-failure-{os.getpid()}.log').write_text(
            f'{error!r}\n')
        print(f'failed production-source fixture retained at {evidence}',
              file=sys.stderr)
        raise
