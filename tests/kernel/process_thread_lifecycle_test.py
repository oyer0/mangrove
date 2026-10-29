#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production process/thread ownership and completion lifecycle fixture."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def function(name):
    source = (ROOT / "kernel/src/process.c").read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        start = source.index("{", match.end())
        if ";" in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise ValueError(f"missing production function: {name}")


def function_from(path, name):
    source = (ROOT / path).read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        start = source.index("{", match.end())
        if ";" in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end]
    raise ValueError(f"missing production function: {path}:{name}")


dispatch = function_from("kernel/src/syscall.c", "syscall_dispatch")
lease_release = dispatch.index("syscall_user_lease_release_all(&context)")
syscall_leave = dispatch.index("process_syscall_leave(process)", lease_release)
worker_reap_retry = dispatch.index("process_worker_reclaim_pending(process)",
                                   syscall_leave)
thread_exit_check = dispatch.index(
    "if (tracked && process_thread_exit_requested(thread))", syscall_leave)
assert lease_release < syscall_leave < worker_reap_retry < thread_exit_check
assert "worker_reap_pending" in dispatch[syscall_leave:worker_reap_retry]
assert "user_region_reap_pending" in dispatch[
    syscall_leave:worker_reap_retry]


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef int32_t i32;
typedef size_t usize;
typedef pthread_mutex_t spinlock_t;
enum {THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
      THREAD_STATE_TERMINATED};
enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED};
enum {THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
      THREAD_PROCESS_EXIT_REQUESTED, THREAD_PROCESS_HANDOFF_COMPLETE};
typedef struct process process_t;
typedef struct kernel_thread kernel_thread_t;
typedef struct {u32 ref_count;} kernel_object_t;
typedef struct {u32 unused;} terminal_output_state_t;
typedef struct process_user_region process_user_region_t;
typedef struct process_thread_record process_thread_record_t;
struct kernel_thread {
    u32 state, running_cpu;
    bool context_switch_pending, syscall_active, sleeping, stack_external;
    uintptr_t preempt_return_rip;
    u64 id;
    process_t *process;
    kernel_thread_t *process_next;
    int process_state;
    bool process_initial, process_reference_held;
    volatile bool process_exit_requested;
    i32 process_exit_status;
    bool process_exit_status_valid;
    process_thread_record_t *worker_record;
};
struct process_thread_record {
    kernel_object_t object;
    process_t *process;
    kernel_thread_t *execution, *join_waiter;
    process_user_region_t *stack_region, *tls_region;
    process_thread_record_t *next;
    u64 id, result;
    bool completed, kernel_stack_released, join_claimed, joined, detached,
         listed, reclaiming;
};
struct process {
    u64 pid;
    int state;
    u32 active_syscalls;
    bool termination_pending, external_termination_in_progress;
    bool launch_ready;
    kernel_thread_t *main_thread, *threads;
    u32 thread_count, live_thread_count;
    u32 worker_reapers;
    bool worker_reap_pending;
    process_thread_record_t *thread_records;
    u32 thread_list_readers;
    u32 storage_authorized_operation;
    u64 storage_authorized_thread_id;
    bool storage_management_session, storage_management_authorizing;
    bool exit_requested, exit_cleanup_in_progress, exit_cleanup_complete;
    bool thread_reap_in_progress, exit_notification_sent, wait_claimed;
    bool owner_reference_held;
    i32 exit_status;
    process_t *parent, *waiting_child;
    kernel_thread_t *waiting_thread;
    kernel_object_t object;
    terminal_output_state_t terminal_output;
};
static spinlock_t process_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static spinlock_t scheduler_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local kernel_thread_t *local_thread;
static bool allow_object_reference=true;
static bool stack_release_context=true;
static bool syscall_lease_live;
static unsigned wake_calls, console_cancels, cleanup_calls, freed_threads;
static unsigned terminal_finishes, alternate_leaves, handle_closes, cwd_clears;
static unsigned exit_notifications, unblocks;
static bool fixture_interrupts_enabled=true;
static bool process_finish_termination(process_t *process);
static void process_worker_reclaim_pending(process_t *process);
static bool scheduler_terminate_thread(kernel_thread_t *thread);
void process_worker_exit(struct kernel_thread *thread, u64 result);
static void process_worker_reclaim_pending(process_t *process)
{assert(process);}
static void process_worker_records_release_all(process_t *process)
{assert(process && !process->thread_records && !process->worker_reapers);}
static void process_unlink_locked(process_t *process);
static void process_unlink_all_locked(process_t *process);
static u64 spin_lock_irqsave(spinlock_t *lock)
{assert(pthread_mutex_lock(lock)==0);return 0;}
static void spin_unlock_irqrestore(spinlock_t *lock,u64 flags)
{(void)flags;assert(pthread_mutex_unlock(lock)==0);}
static kernel_thread_t *thread_current(void){return local_thread;}
static bool object_reference(kernel_object_t *object)
{if(!allow_object_reference)return false;
 __atomic_add_fetch(&object->ref_count,1U,__ATOMIC_RELAXED);return true;}
static void object_release(kernel_object_t *object)
{assert(__atomic_load_n(&object->ref_count,__ATOMIC_RELAXED));
 __atomic_sub_fetch(&object->ref_count,1U,__ATOMIC_RELAXED);}
static bool scheduler_thread_process_attachable(const kernel_thread_t *thread)
{bool ok;u64 f=spin_lock_irqsave(&scheduler_lock);
 ok=thread->state==THREAD_STATE_READY && thread->running_cpu==UINT32_MAX &&
   !thread->context_switch_pending && !thread->syscall_active &&
   !thread->sleeping && !thread->preempt_return_rip;
 spin_unlock_irqrestore(&scheduler_lock,f);return ok;}
static bool scheduler_thread_initial_process_attachable(
 const kernel_thread_t *thread)
{bool ok;u64 f=spin_lock_irqsave(&scheduler_lock);
 ok=thread==local_thread && thread->state==THREAD_STATE_RUNNING &&
   thread->running_cpu!=UINT32_MAX && !thread->context_switch_pending;
 spin_unlock_irqrestore(&scheduler_lock,f);return ok;}
static bool scheduler_thread_exit_complete(const kernel_thread_t *thread)
{bool ok;u64 f=spin_lock_irqsave(&scheduler_lock);
 ok=thread->state==THREAD_STATE_TERMINATED &&
   thread->running_cpu==UINT32_MAX && !thread->context_switch_pending &&
   !thread->sleeping && !thread->preempt_return_rip;
 spin_unlock_irqrestore(&scheduler_lock,f);return ok;}
static bool kernel_stack_release_context_valid(void)
{return stack_release_context;}
static void panic(const char *message)
{fprintf(stderr,"unexpected panic: %s\n",message);abort();}
static bool thread_destroy(kernel_thread_t *thread)
{if(!thread || thread->process ||
   thread->process_state!=THREAD_PROCESS_UNATTACHED ||
   !scheduler_thread_exit_complete(thread))return false;
 freed_threads++;return true;}
static bool scheduler_wake_waiter(kernel_thread_t *thread)
{(void)thread;__atomic_add_fetch(&wake_calls,1U,__ATOMIC_RELAXED);return true;}
static bool scheduler_unblock(kernel_thread_t *thread)
{(void)thread;__atomic_add_fetch(&unblocks,1U,__ATOMIC_RELAXED);return true;}
static void console_cancel_waiter(kernel_thread_t *thread)
{(void)thread;__atomic_add_fetch(&console_cancels,1U,__ATOMIC_RELAXED);}
static void terminal_output_finish_for_process(u64 pid,
                                                terminal_output_state_t *state)
{(void)pid;(void)state;terminal_finishes++;}
static bool terminal_alternate_leave_process(u64 pid)
{(void)pid;alternate_leaves++;return true;}
static void terminal_force_end_batch(void){}
static void ipc_process_exit(process_t *process)
{(void)process;assert(fixture_interrupts_enabled);cleanup_calls++;}
static void process_memory_release_all(process_t *process){(void)process;}
static void process_handle_close_all(process_t *process)
{(void)process;handle_closes++;}
static void process_cwd_clear(process_t *process)
{(void)process;cwd_clears++;}
static void session_process_exited(process_t *process)
{(void)process;__atomic_add_fetch(&exit_notifications,1U,__ATOMIC_RELAXED);}
'''

names = (
    "process_storage_authorization_clear_locked",
    "process_storage_authorization_revoke_thread_locked",
    "process_thread_register_locked",
    "process_thread_list_complete_locked",
    "process_thread_list_read_acquire_locked",
    "process_thread_list_read_release",
    "process_publish_terminated_locked",
    "process_attach_thread",
    "process_detach_thread_owned",
    "process_detach_thread",
    "process_cleanup_exit_resources",
    "process_finish_termination",
    "process_exit",
    "process_syscall_enter",
    "process_syscall_leave",
    "process_thread_exit_requested",
    "process_thread_entry_completed",
    "process_worker_exit",
    "process_scheduler_handoff_complete",
    "process_scheduler_post_handoff",
    "process_abort",
    "process_finalize_exit",
)

suffix = r'''
static void thread_running(kernel_thread_t *thread,u32 cpu)
{
    u64 f=spin_lock_irqsave(&scheduler_lock);
    thread->state=THREAD_STATE_RUNNING;thread->running_cpu=cpu;
    spin_unlock_irqrestore(&scheduler_lock,f);
}
static void thread_stopped(kernel_thread_t *thread)
{
    u64 f=spin_lock_irqsave(&scheduler_lock);
    thread->state=THREAD_STATE_TERMINATED;thread->running_cpu=UINT32_MAX;
    thread->context_switch_pending=false;thread->sleeping=false;
    thread->preempt_return_rip=0;
    spin_unlock_irqrestore(&scheduler_lock,f);
}
static bool scheduler_terminate_thread(kernel_thread_t *thread)
{if(!thread || thread->state==THREAD_STATE_RUNNING)return false;
 thread_stopped(thread);return true;}
static void process_unlink_locked(process_t *process)
{process->parent=NULL;}
static void process_unlink_all_locked(process_t *process)
{(void)process;}
static void reset_process(process_t *process,kernel_thread_t *initial,
                          kernel_thread_t *worker)
{
    memset(process,0,sizeof(*process));memset(initial,0,sizeof(*initial));
    memset(worker,0,sizeof(*worker));
    process->state=PROCESS_STATE_ACTIVE;process->pid=41;
    process->launch_ready=true;
    process->owner_reference_held=true;
    process->object.ref_count=1;
    initial->state=worker->state=THREAD_STATE_READY;
    initial->running_cpu=worker->running_cpu=UINT32_MAX;
    local_thread=NULL;allow_object_reference=true;stack_release_context=true;
    wake_calls=console_cancels=cleanup_calls=freed_threads=0;
    terminal_finishes=alternate_leaves=handle_closes=cwd_clears=0;
    exit_notifications=unblocks=0;
    fixture_interrupts_enabled=true;
}
static void finish_thread(process_t *process,kernel_thread_t *thread,
                          i32 status)
{
    (void)process;
    local_thread=thread;
    process_thread_entry_completed(thread,status);
    thread_stopped(thread);
    process_scheduler_handoff_complete(thread);
}
typedef struct {process_t *process;kernel_thread_t *thread;
                pthread_barrier_t *barrier;i32 status;} exit_request_t;
static void *concurrent_exit(void *argument)
{
    exit_request_t *request=argument;local_thread=request->thread;
    int result=pthread_barrier_wait(request->barrier);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(process_exit(request->process,request->status));
    return NULL;
}
typedef struct {process_t *process;pthread_barrier_t *barrier;bool admitted;}
        admission_request_t;
static void *concurrent_admission(void *argument)
{
    admission_request_t *request=argument;
    int result=pthread_barrier_wait(request->barrier);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    request->admitted=process_syscall_enter(request->process);
    return NULL;
}
static void *concurrent_process_exit(void *argument)
{
    exit_request_t *request=argument;local_thread=request->thread;
    int result=pthread_barrier_wait(request->barrier);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(process_exit(request->process,request->status));
    return NULL;
}
int main(void)
{
    process_t process;kernel_thread_t initial,worker,invalid;
    reset_process(&process,&initial,&worker);
    assert(process_attach_thread(&process,&initial));
    assert(process.main_thread==&initial && process.threads==&initial &&
           process.thread_count==1 && process.live_thread_count==1 &&
           process.object.ref_count==2 && initial.process_initial);
    assert(!process_attach_thread(&process,&initial));
    invalid=(kernel_thread_t){.state=THREAD_STATE_RUNNING,.running_cpu=0};
    assert(!process_attach_thread(&process,&invalid));
    assert(process_attach_thread(&process,&worker));
    assert(process.thread_count==2 && process.live_thread_count==2 &&
           process.object.ref_count==3 && worker.process==&process &&
           !worker.process_initial);
    assert(!process_detach_thread(&process,&worker));
    allow_object_reference=false;
    kernel_thread_t failed={.state=THREAD_STATE_READY,.running_cpu=UINT32_MAX};
    assert(!process_attach_thread(&process,&failed));
    assert(!failed.process && !failed.process_reference_held &&
           process.thread_count==2 && process.live_thread_count==2 &&
           process.object.ref_count==3);
    allow_object_reference=true;

    thread_running(&initial,0);
    local_thread=&initial;
    assert(process_syscall_enter(&process));
    syscall_lease_live=true;
    assert(process.active_syscalls==1 && process.object.ref_count==4);
    assert(process_exit(&process,17));
    assert(process.state==PROCESS_STATE_EXITING && process.exit_requested &&
           process.exit_status==17 && process.termination_pending &&
           process_thread_exit_requested(&initial) &&
           process_thread_exit_requested(&worker));
    assert(!process_syscall_enter(&process));
    assert(wake_calls==1 && console_cancels==1 && cleanup_calls==0);
    assert(syscall_lease_live);
    syscall_lease_live=false;
    process_syscall_leave(&process);
    assert(!process.active_syscalls && process.object.ref_count==3 &&
           process.state==PROCESS_STATE_EXITING && cleanup_calls==0);

    finish_thread(&process,&worker,-7);
    assert(worker.process_exit_status_valid &&
           worker.process_exit_status==-7 &&
           worker.process_state==THREAD_PROCESS_HANDOFF_COMPLETE &&
           process.live_thread_count==1 && process.thread_count==2 &&
           process.state==PROCESS_STATE_EXITING && cleanup_calls==0 &&
           worker.process==&process);
    assert(!process_detach_thread(&process,&worker));

    /* Resource cleanup waits for the initial thread's outgoing-stack handoff
     * as well as the worker handoff and syscall lease. */
    u64 reader_flags=spin_lock_irqsave(&process_registry_lock);
    assert(process_thread_list_read_acquire_locked(&process));
    spin_unlock_irqrestore(&process_registry_lock,reader_flags);
    local_thread=&initial;
    process_thread_entry_completed(&initial,17);
    assert(cleanup_calls==0 && !process.exit_cleanup_complete &&
           process.state==PROCESS_STATE_EXITING);
    thread_stopped(&initial);
    fixture_interrupts_enabled=false;
    process_scheduler_handoff_complete(&initial);
    assert(process.state==PROCESS_STATE_EXITING && cleanup_calls==0 &&
           !process.exit_cleanup_complete);
    fixture_interrupts_enabled=true;
    process_scheduler_post_handoff(&process);
    assert(process.state==PROCESS_STATE_EXITING && cleanup_calls==1 &&
           process.exit_cleanup_complete &&
           process.live_thread_count==0 && process.thread_count==2 &&
           process.termination_pending && unblocks==0);
    assert(!process_finalize_exit(&process) && process.thread_count==2 &&
           freed_threads==0);
    process_thread_list_read_release(&process);
    assert(process.state==PROCESS_STATE_TERMINATED &&
           !process.termination_pending && unblocks==0);
    u64 complete_flags=spin_lock_irqsave(&process_registry_lock);
    assert(process_thread_list_complete_locked(&process));
    process.thread_count++;
    assert(!process_thread_list_complete_locked(&process));
    process.thread_count--;
    kernel_thread_t *old_next=worker.process_next;
    worker.process_next=&worker;
    assert(!process_thread_list_complete_locked(&process));
    worker.process_next=old_next;
    spin_unlock_irqrestore(&process_registry_lock,complete_flags);
    stack_release_context=false;
    assert(!process_finalize_exit(&process) && process.thread_count==2 &&
           freed_threads==0);
    stack_release_context=true;
    reader_flags=spin_lock_irqsave(&process_registry_lock);
    assert(!process_thread_list_read_acquire_locked(&process));
    spin_unlock_irqrestore(&process_registry_lock,reader_flags);
    process.thread_reap_in_progress=true;
    assert(!process_detach_thread(&process,&worker));
    process.thread_reap_in_progress=false;
    assert(process_finalize_exit(&process));
    assert(process.thread_count==0 && process.threads==NULL &&
           process.main_thread==NULL && process.object.ref_count==1 &&
           freed_threads==2 && exit_notifications==1);
    assert(process_finalize_exit(&process) && exit_notifications==1);

    /* A worker returning while its process remains active only completes its
     * own scheduler record. The initial thread and process resources remain
     * owned; a kernel join/reap path may then detach that stopped worker. */
    reset_process(&process,&initial,&worker);
    assert(process_attach_thread(&process,&initial));
    assert(process_attach_thread(&process,&worker));
    finish_thread(&process,&worker,23);
    assert(process.state==PROCESS_STATE_ACTIVE && process.main_thread==&initial &&
           process.thread_count==2 && process.live_thread_count==1 &&
           !process.exit_cleanup_complete && cleanup_calls==0 &&
           process.object.ref_count==3 && worker.process==&process);
    assert(process_detach_thread(&process,&worker));
    assert(process.thread_count==1 && process.live_thread_count==1 &&
           process.object.ref_count==2 && worker.process==NULL &&
           thread_destroy(&worker));
    local_thread=&initial;
    process_thread_entry_completed(&initial,0);
    thread_stopped(&initial);
    process_scheduler_handoff_complete(&initial);
    process_scheduler_post_handoff(&process);
    assert(process.state==PROCESS_STATE_TERMINATED && cleanup_calls==1 &&
           process_finalize_exit(&process) && process.thread_count==0 &&
           exit_notifications==1);

    /* Process exit closes syscall admission atomically. A syscall that wins
     * the race retains the process until its last lease and frame unwind. */
    reset_process(&process,&initial,&worker);
    assert(process_attach_thread(&process,&initial));
    assert(process_attach_thread(&process,&worker));
    pthread_barrier_t admission_barrier;
    assert(pthread_barrier_init(&admission_barrier,NULL,3)==0);
    admission_request_t admission={&process,&admission_barrier,false};
    exit_request_t racing_exit={&process,&initial,&admission_barrier,14};
    pthread_t admission_thread, exit_thread;
    assert(pthread_create(&admission_thread,NULL,concurrent_admission,
                          &admission)==0);
    assert(pthread_create(&exit_thread,NULL,concurrent_process_exit,
                          &racing_exit)==0);
    int admission_result=pthread_barrier_wait(&admission_barrier);
    assert(admission_result==0 ||
           admission_result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(pthread_join(admission_thread,NULL)==0 &&
           pthread_join(exit_thread,NULL)==0);
    assert(pthread_barrier_destroy(&admission_barrier)==0);
    assert(process.state==PROCESS_STATE_EXITING &&
           process.active_syscalls==(admission.admitted?1U:0U));
    if (admission.admitted) process_syscall_leave(&process);
    finish_thread(&process,&worker,-1);
    local_thread=&initial;
    process_thread_entry_completed(&initial,14);
    thread_stopped(&initial);
    process_scheduler_handoff_complete(&initial);
    process_scheduler_post_handoff(&process);
    assert(process.state==PROCESS_STATE_TERMINATED && cleanup_calls==1 &&
           process_finalize_exit(&process));

    /* PID 1 adopts the permanent BSP bootstrap record. Final process reap
     * releases process ownership without freeing the scheduler record or its
     * linker-backed stack. */
    reset_process(&process,&initial,&worker);
    local_thread=&initial;
    initial.state=THREAD_STATE_RUNNING;
    initial.running_cpu=0;
    initial.stack_external=true;
    u64 bootstrap_flags=spin_lock_irqsave(&process_registry_lock);
    assert(process_thread_register_locked(&process,&initial,true));
    spin_unlock_irqrestore(&process_registry_lock,bootstrap_flags);
    assert(process.main_thread==&initial && process.thread_count==1 &&
           process.object.ref_count==2);
    process_thread_entry_completed(&initial,0);
    assert(!process.exit_cleanup_complete && cleanup_calls==0 &&
           process.state==PROCESS_STATE_EXITING);
    thread_stopped(&initial);
    process_scheduler_handoff_complete(&initial);
    process_scheduler_post_handoff(&process);
    assert(process.state==PROCESS_STATE_TERMINATED &&
           process_finalize_exit(&process));
    assert(process.thread_count==0 && process.main_thread==NULL &&
           process.object.ref_count==1 && freed_threads==0 &&
           exit_notifications==1 && initial.stack_external);

    /* Unpublished launch rollback owns its resource cleanup. The scheduler
     * handoff hook must not run the published-process cleanup transaction. */
    reset_process(&process,&initial,&worker);
    process.launch_ready=false;
    assert(process_attach_thread(&process,&initial));
    process_abort(&process);
    assert(process.state==PROCESS_STATE_TERMINATED &&
           process.thread_count==0 && process.main_thread==NULL &&
           process.object.ref_count==0 && freed_threads==1 &&
           !process.owner_reference_held &&
           cleanup_calls==1 && terminal_finishes==1 &&
           alternate_leaves==1 && handle_closes==1 && cwd_clears==1 &&
           exit_notifications==0);

    /* Competing exit requests are serialized: the first status is retained,
     * both threads observe the process-wide request, and only one cleanup
     * publication can occur. */
    reset_process(&process,&initial,&worker);
    assert(process_attach_thread(&process,&initial));
    assert(process_attach_thread(&process,&worker));
    thread_running(&initial,0);thread_running(&worker,1);
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier,NULL,3)==0);
    exit_request_t a={&process,&initial,&barrier,31};
    exit_request_t b={&process,&worker,&barrier,32};
    pthread_t first,second;
    assert(pthread_create(&first,NULL,concurrent_exit,&a)==0);
    assert(pthread_create(&second,NULL,concurrent_exit,&b)==0);
    int barrier_result=pthread_barrier_wait(&barrier);
    assert(barrier_result==0 || barrier_result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(pthread_join(first,NULL)==0 && pthread_join(second,NULL)==0);
    assert(pthread_barrier_destroy(&barrier)==0);
    assert(process.state==PROCESS_STATE_EXITING && process.exit_requested &&
           (process.exit_status==31 || process.exit_status==32) &&
           process_thread_exit_requested(&initial) &&
           process_thread_exit_requested(&worker));
    assert(cleanup_calls==0 && process.live_thread_count==2);
    finish_thread(&process,&worker,-1);
    local_thread=&initial;
    process_thread_entry_completed(&initial,process.exit_status);
    thread_stopped(&initial);
    process_scheduler_handoff_complete(&initial);
    process_scheduler_post_handoff(&process);
    assert(process.state==PROCESS_STATE_TERMINATED &&
           process.live_thread_count==0 && cleanup_calls==1);
    assert(process_finalize_exit(&process) && process.thread_count==0 &&
           exit_notifications==1);

    /* A blocked syscall keeps the process in EXITING. Admission remains
     * closed until its lease owner returns, and external finalization cannot
     * detach its thread early. */
    reset_process(&process,&initial,&worker);
    assert(process_attach_thread(&process,&initial));
    thread_running(&initial,0);local_thread=&initial;
    assert(process_syscall_enter(&process));
    assert(process_exit(&process,9));
    assert(process.state==PROCESS_STATE_EXITING &&
           process.active_syscalls==1 && process.live_thread_count==1 &&
           !process_thread_list_complete_locked(&process));
    process_syscall_leave(&process);
    assert(process.state==PROCESS_STATE_EXITING &&
           process.active_syscalls==0);
    /* In the real syscall dispatcher the last admitted call checks the
     * persistent exit request and enters the thread exit boundary. */
    local_thread=&initial;
    process_thread_entry_completed(&initial,9);
    thread_stopped(&initial);
    process_scheduler_handoff_complete(&initial);
    process_scheduler_post_handoff(&process);
    assert(cleanup_calls==1 && process.state==PROCESS_STATE_TERMINATED);
    assert(process_finalize_exit(&process) && process.thread_count==0);

    puts("process threads: ownership, admission, concurrent exit, handoff, cleanup, and reap passed");
    return 0;
}
'''

code = prefix + "".join(function(name) for name in names) + suffix
with tempfile.TemporaryDirectory(prefix="mangrove-process-thread-") as directory:
    path = Path(directory) / "process_thread_lifecycle.c"
    binary = Path(directory) / "process_thread_lifecycle"
    path.write_text(code)
    subprocess.run([CC, "-std=gnu17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-pthread", str(path),
                    "-o", str(binary)], check=True)
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    subprocess.run([str(binary)], check=True, env=env)
