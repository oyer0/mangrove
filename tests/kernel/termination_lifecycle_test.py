#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production scheduler/process termination and session retirement boundaries.

The host fixtures control scheduling, mutex queues, and resource teardown. They
do not emulate a live kernel stack, filesystem I/O, or guest SMP execution.
"""
from pathlib import Path
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
        start = source.index("{", match.end())
        if ";" in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise ValueError(f"missing production function: {path}:{name}")


def run(source, name):
    with tempfile.TemporaryDirectory(prefix="mangrove-termination-") as tmp:
        path = Path(tmp) / f"{name}.c"
        binary = Path(tmp) / name
        path.write_text('#include "' + str(ROOT / "tests/kernel/process_cleanup_fixture.h") + '"\n' + source)
        subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(path), "-o",
                        str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


scheduler_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef size_t usize;
typedef int32_t i32;
typedef u32 process_handle_t;
enum {THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
      THREAD_STATE_TERMINATED};
typedef enum {SCHEDULER_WAKE_FAILED=0, SCHEDULER_WAKE_DELIVERED,
               SCHEDULER_WAKE_ALREADY_TERMINATED} scheduler_wake_result_t;
typedef int thread_state_t;
enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED};
enum {THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
      THREAD_PROCESS_EXIT_REQUESTED, THREAD_PROCESS_HANDOFF_COMPLETE};
#define MG_OK 0
#define MG_ERR_BUSY -14
#define MG_ERR_BAD_ARGUMENT -1
#define MG_ERR_INVALID_HANDLE -3
#define MG_ERR_NOT_CHILD -4
#define MG_SERVICE_SPROUT 1U
#define MG_SERVICE_SESSIOND 2U
#define MG_SERVICE_LOGIND 3U
#define OBJECT_TYPE_PROCESS 2
#define THREAD_CPU_NONE ((u32)-1)
typedef struct kernel_thread kernel_thread_t;
typedef struct process process_t;
typedef struct {bool active;} kernel_stack_t;
typedef struct {process_t *process; u32 references;} kernel_object_t;
typedef struct process_thread_record {
 process_t *process; kernel_thread_t *execution, *join_waiter;
 bool completed, kernel_stack_released, join_claimed, listed;
} process_thread_record_t;
struct process {int state; kernel_thread_t *main_thread, *threads;
 u32 thread_count, live_thread_count, active_syscalls;
 u32 thread_list_readers;
 u32 scheduler_handoff_references;
 _Atomic u32 scheduler_handoff_releases_pending;
 u32 storage_authorized_operation; u64 storage_authorized_thread_id;
 bool storage_management_session, storage_management_authorizing;
 process_thread_record_t *thread_records;
 u32 worker_reapers;
 bool termination_pending, external_termination_in_progress, launch_ready;
 bool thread_reap_in_progress;
 bool worker_reap_pending;
 bool system_service, exit_requested, exit_cleanup_complete,
      exit_cleanup_in_progress; i32 exit_status; u32 service_id;
 process_t *parent, *waiting_child; kernel_thread_t *waiting_thread;
 kernel_object_t object;};
static kernel_object_t target_object;
struct kernel_thread {
 int state; u32 running_cpu, syscall_nesting;
 bool syscall_active, context_switch_pending, queued, sleeping;
 bool entry_started, stack_external;
 uintptr_t preempt_return_rip;
 u64 wakeup_deadline_ms; usize kernel_stack_size; kernel_thread_t *next;
 void *waiting_mutex; uintptr_t kernel_stack_base;
 kernel_stack_t kernel_stack;
 process_t *process;
 kernel_thread_t *process_next;
 int process_state;
 bool process_initial, process_reference_held;
 volatile bool process_exit_requested;
 i32 process_exit_status;
 bool process_exit_status_valid;
 process_thread_record_t *worker_record;
};
typedef struct {kernel_thread_t *scheduler_idle_thread;} cpu_local_t;
static cpu_local_t cpu;
static kernel_thread_t bootstrap_thread, *sleeping_threads;
static struct {u32 blocked_threads, sleeping_threads;} scheduler_stats;
static u32 scheduler_lock, process_registry_lock;
static unsigned removed, cancelled, freed, exited, closed_handles, unmapped;
static unsigned self_dispatches, self_handoffs;
static unsigned worker_reap_calls;
static unsigned lookup_references, lookup_releases;
static bool group_allowed=true;
static process_t *current_process;
static kernel_thread_t *current_thread, *idle_thread;
static kernel_thread_t selected_thread;
static u32 fixture_spinlock_depth;
typedef enum {SCHEDULER_DISPATCH_TERMINATE} scheduler_dispatch_action_t;
static u32 spinlock_depth_current(void) { return fixture_spinlock_depth; }
static bool kernel_stack_release_context_valid(void)
{return fixture_spinlock_depth == 0;}
static bool kernel_stack_release(kernel_stack_t *stack)
{assert(stack && stack->active && !fixture_spinlock_depth); stack->active=false; return true;}
static void panic(const char *reason) {(void)reason; assert(!"unexpected kernel panic");}
static u64 spin_lock_irqsave(u32 *lock) {assert(!*lock); *lock = 1; return 0;}
static void spin_unlock_irqrestore(u32 *lock, u64 flags)
{(void)flags; assert(*lock); *lock = 0;}
static u32 cpu_count(void) {return 1;}
static u32 cpu_current_index(void) {return 0;}
static cpu_local_t *cpu_by_index(u32 index) {assert(index == 0); return &cpu;}
static bool scheduler_prepare_dispatch_locked(
  scheduler_dispatch_action_t action, kernel_thread_t **outgoing,
  kernel_thread_t **target, bool *handoff)
{assert(scheduler_lock && action==SCHEDULER_DISPATCH_TERMINATE);
 assert(current_thread && current_thread->state==THREAD_STATE_TERMINATED);
 *outgoing=current_thread; *target=&selected_thread; *handoff=true;
 self_dispatches++; return true;}
static bool scheduler_handoff(kernel_thread_t *outgoing,
  kernel_thread_t *target, u64 flags)
{assert(scheduler_lock && outgoing==current_thread &&
        outgoing->state==THREAD_STATE_TERMINATED && target==&selected_thread);
 spin_unlock_irqrestore(&scheduler_lock,flags); self_handoffs++; return true;}
static void scheduler_remove_queued_locked(kernel_thread_t *thread)
{assert(scheduler_lock && thread->queued); thread->queued = false; removed++;}
static void mutex_cancel_waiter(kernel_thread_t *thread)
{assert(!scheduler_lock); cancelled++; thread->waiting_mutex = NULL;}
static void kfree(void *pointer) {(void)pointer; freed++;}
static process_t *process_current(void) {return current_process;}
static kernel_object_t *process_handle_lookup_any(process_t *parent,
  process_handle_t handle, int type, u32 rights)
{(void)parent; (void)rights; assert(type==OBJECT_TYPE_PROCESS);
 if(handle!=17)return NULL;
 target_object.references++;
 lookup_references++;
 return &target_object;}
static process_t *object_process(kernel_object_t *object) {return object->process;}
static void object_release(kernel_object_t *object)
{assert(object&&object->references);object->references--;lookup_releases++;}
static void process_handle_reference_release(kernel_object_t *object)
{object_release(object);}
static bool object_reference(kernel_object_t *object)
{assert(object);object->references++;return true;}
static bool scheduler_thread_process_attachable(const kernel_thread_t *thread)
{return thread && thread->state==THREAD_STATE_READY && !thread->queued &&
 thread->running_cpu==THREAD_CPU_NONE && !thread->context_switch_pending &&
 !thread->entry_started && !thread->syscall_active &&
 !thread->syscall_nesting && !thread->sleeping && !thread->preempt_return_rip &&
 !thread->stack_external;}
static bool scheduler_thread_initial_process_attachable(
 const kernel_thread_t *thread)
{return thread && thread==current_thread &&
 thread->state==THREAD_STATE_RUNNING && thread->stack_external;}
static bool scheduler_thread_exit_complete(const kernel_thread_t *thread)
{return thread && thread->state==THREAD_STATE_TERMINATED &&
 thread->running_cpu==THREAD_CPU_NONE && !thread->context_switch_pending &&
 !thread->preempt_return_rip && !thread->sleeping;}
static bool scheduler_thread_handoff_callback_valid(
 const kernel_thread_t *thread)
{return thread && thread->state==THREAD_STATE_TERMINATED &&
 thread->running_cpu==THREAD_CPU_NONE && thread->context_switch_pending &&
 !thread->preempt_return_rip && !thread->sleeping;}
static bool scheduler_terminate_thread_group(kernel_thread_t *head,
                                               usize expected_count);
static bool process_finish_termination(process_t *process);
static void process_cleanup_exit_resources(process_t *process);
static void console_cancel_waiter(kernel_thread_t *thread) {(void)thread;}
static bool scheduler_wake_waiter(kernel_thread_t *thread)
{(void)thread; return true;}
static scheduler_wake_result_t scheduler_wake_waiter_result(
    kernel_thread_t *thread)
{if(!thread)return SCHEDULER_WAKE_FAILED;
 if(thread->state==THREAD_STATE_TERMINATED)
  return SCHEDULER_WAKE_ALREADY_TERMINATED;
 return scheduler_wake_waiter(thread)?SCHEDULER_WAKE_DELIVERED:
        SCHEDULER_WAKE_FAILED;}

static bool process_cleanup_worker_wake(process_t *process)
{assert(process && !process_registry_lock); return true;}
static bool process_exit(process_t *process, i32 status)
{(void)status; assert(process->state == PROCESS_STATE_ACTIVE &&
                      !process->active_syscalls);
 process->state = PROCESS_STATE_TERMINATED;
 process->termination_pending=false;exited++; closed_handles++;
 unmapped++; return true;}
'''
scheduler_functions = "".join(function(path, name) for path, name in (
    ("kernel/src/scheduler.c", "sleeping_remove"),
    ("kernel/src/scheduler.c", "scheduler_terminate_thread"),
    ("kernel/src/scheduler.c", "scheduler_terminate"),
    ("kernel/src/scheduler.c", "thread_destroy"),
    ("kernel/src/process.c", "process_thread_register_locked"),
    ("kernel/src/process.c", "process_thread_list_complete_locked"),
    ("kernel/src/process.c", "process_thread_member_locked"),
    ("kernel/src/process.c", "process_thread_list_read_acquire_locked"),
    ("kernel/src/process.c", "process_thread_list_read_release"),
    ("kernel/src/scheduler.c", "scheduler_thread_state_snapshot"),
    ("kernel/src/process.c", "process_waiter_wake_locked"),
    ("kernel/src/process.c", "process_storage_authorization_clear_locked"),
    ("kernel/src/process.c", "process_publish_terminated_locked"),
    ("kernel/src/process.c", "process_finish_termination"),
    ("kernel/src/process.c", "process_scheduler_handoff_complete_internal"),
    ("kernel/src/process.c", "process_scheduler_handoff_complete"),
    ("kernel/src/process.c", "process_attach_thread"),
    ("kernel/src/process.c", "process_publish_launch"),
    ("kernel/src/process.c", "process_termination_begin"),
    ("kernel/src/process.c", "process_termination_cancel"),
    ("kernel/src/process.c", "process_matches_shutdown_phase"),
    ("kernel/src/process.c", "process_terminate_external"),
    ("kernel/src/process.c", "process_terminate_child"),
))
scheduler_suffix = r'''
static bool scheduler_terminate_thread_group(kernel_thread_t *head,
                                               usize expected_count)
{
 if(!group_allowed || !head || !expected_count)return false;
 assert(head->process && head->process->thread_list_readers==1U);
 u64 flags=spin_lock_irqsave(&scheduler_lock);
 usize count=0;kernel_thread_t *thread=head;
 for(;thread && count<expected_count;thread=thread->process_next,count++) {
   bool done=scheduler_thread_exit_complete(thread);
   if(!thread->process || (!done &&
      ((thread->state!=THREAD_STATE_READY &&
        thread->state!=THREAD_STATE_BLOCKED) ||
       thread->running_cpu!=THREAD_CPU_NONE || thread->context_switch_pending ||
       thread->preempt_return_rip || thread->syscall_active ||
       thread->syscall_nesting || thread->sleeping))) {
       spin_unlock_irqrestore(&scheduler_lock,flags);
       return false;
   }
 }
 if(count!=expected_count || thread) {
   spin_unlock_irqrestore(&scheduler_lock,flags);
   return false;
 }
 for(thread=head;thread;thread=thread->process_next) {
   if(!scheduler_thread_exit_complete(thread)) {
     if(thread->queued) scheduler_remove_queued_locked(thread);
     thread->state=THREAD_STATE_TERMINATED;
     thread->running_cpu=THREAD_CPU_NONE;
     thread->context_switch_pending=false;
     thread->preempt_return_rip=0;
     thread->sleeping=false;
   }
 }
 spin_unlock_irqrestore(&scheduler_lock,flags);
 for(thread=head;thread;thread=thread->process_next) {
   mutex_cancel_waiter(thread);
   process_scheduler_handoff_complete(thread);
 }
 return true;
}
static void process_cleanup_exit_resources(process_t *process)
{
 if(process && process->exit_cleanup_complete)return;
 assert(process && process->state==PROCESS_STATE_EXITING &&
        process_thread_list_complete_locked(process));
 process->exit_cleanup_complete=true;
 exited++;closed_handles++;unmapped++;
 (void)process_finish_termination(process);
}
int main(void) {
 process_t reap_process={.state=PROCESS_STATE_ACTIVE,.thread_count=1,
   .thread_list_readers=1,.worker_reap_pending=true};
 kernel_thread_t reap_thread={0};reap_process.threads=&reap_thread;
 process_thread_list_read_release(&reap_process);
 assert(!reap_process.thread_list_readers && worker_reap_calls==0 &&
        (process_cleanup_fixture_work & PROCESS_CLEANUP_WORKER_REAP));

 process_t caller={.state=PROCESS_STATE_ACTIVE};
 process_t target = {.state=PROCESS_STATE_ACTIVE,.parent=&caller,
   .launch_ready=true,.thread_count=1,.live_thread_count=1};
 target_object.process=&target;
 kernel_thread_t thread = {.state=THREAD_STATE_BLOCKED,
   .running_cpu=THREAD_CPU_NONE, .syscall_active=true,
   .syscall_nesting=1, .entry_started=true,
   .waiting_mutex=&scheduler_lock, .process=&target,
   .process_state=THREAD_PROCESS_ATTACHED,
   .process_initial=true,.process_reference_held=true};
 target.threads=&thread;
 target.main_thread=&thread;
 scheduler_stats.blocked_threads=1;
 target.active_syscalls=1;
 assert(!process_terminate_external(&target, 1) &&
        target.state==PROCESS_STATE_ACTIVE && !target.termination_pending);
 target.active_syscalls=0;
 assert(!process_terminate_external(&target, 1));
 assert(process_terminate_child(&caller,17,1)==MG_ERR_BUSY);

 kernel_thread_t kernel_worker={.state=THREAD_STATE_READY,
   .running_cpu=THREAD_CPU_NONE, .entry_started=true, .queued=true};
 assert(!scheduler_terminate_thread(&kernel_worker));
 assert(!thread_destroy(&kernel_worker));
 assert(kernel_worker.queued && !freed);
 assert(target.state == PROCESS_STATE_ACTIVE && !target.termination_pending &&
        thread.state == THREAD_STATE_BLOCKED &&
        thread.waiting_mutex == &scheduler_lock &&
        scheduler_stats.blocked_threads == 1);
 assert(!removed && !cancelled && !exited && !closed_handles && !unmapped);

 thread.state=THREAD_STATE_READY; thread.queued=true;
 assert(!process_terminate_external(&target, 1));
 assert(thread.queued && !removed && !cancelled);
 thread.syscall_active=false;
 assert(!process_terminate_external(&target, 1)); /* nesting alone */
 thread.syscall_nesting=0;
 thread.running_cpu=0;
 assert(process_terminate_child(&caller,17,1)==MG_ERR_BUSY);
 assert(target.state==PROCESS_STATE_ACTIVE && !target.termination_pending &&
        thread.state==THREAD_STATE_READY &&
        thread.queued && thread.waiting_mutex==&scheduler_lock &&
        !removed && !cancelled && !exited && !closed_handles && !unmapped);
 thread.running_cpu=THREAD_CPU_NONE;
 thread.context_switch_pending=true;
 assert(!process_terminate_external(&target, 1)); /* handoff alone */
 thread.context_switch_pending=false;
 thread.preempt_return_rip=0x1234;
 assert(!process_terminate_external(&target, 1)); /* kernel preemption */
 thread.preempt_return_rip=0;
 thread.syscall_active=true;
 assert(!thread_destroy(&thread)); /* alternate READY destruction */
 thread.syscall_active=false;
 /* A previously started user thread can be terminated at a real safe point. */
 assert(thread.entry_started && thread.process==&target &&
        !thread.syscall_nesting && !thread.context_switch_pending &&
        !thread.preempt_return_rip && thread.running_cpu==THREAD_CPU_NONE);
 assert(process_terminate_child(&caller,17,1)==MG_OK);
 assert(target.state == PROCESS_STATE_TERMINATED &&
        thread.state == THREAD_STATE_TERMINATED && removed == 1 &&
        cancelled == 1 && exited == 1 && closed_handles == 1 &&
        unmapped == 1);
 assert(!target.termination_pending &&
        lookup_references==lookup_releases&&target_object.references==0U);
 assert(target.thread_list_readers==0U);

 /* The final cooperative handoff may complete an exiting process while an
  * external termination group is walking its stable thread list. Final
  * publication must clear the reservation, and the external path must not
  * restore termination_pending after publication. */
 process_t racing_exit={.state=PROCESS_STATE_EXITING,.launch_ready=true,
   .thread_count=1,.live_thread_count=0,.termination_pending=true};
 kernel_thread_t retired_member={.state=THREAD_STATE_TERMINATED,
   .running_cpu=THREAD_CPU_NONE,.process=&racing_exit,
   .process_state=THREAD_PROCESS_HANDOFF_COMPLETE,.process_initial=true,
   .process_reference_held=true};
 racing_exit.threads=&retired_member;racing_exit.main_thread=&retired_member;
 assert(process_terminate_external(&racing_exit,9));
 assert(racing_exit.state==PROCESS_STATE_TERMINATED &&
        !racing_exit.termination_pending &&
        !racing_exit.external_termination_in_progress &&
        racing_exit.exit_cleanup_complete &&
        racing_exit.thread_list_readers==0U);

 assert(!process_terminate_external(&target, 1));
 assert(exited==2 && closed_handles==2 && unmapped==2 && cancelled==2);

 /* A fully retired ordinary kernel thread releases its guarded stack only
  * after scheduler ownership is detached and the lock is no longer held. */
 kernel_thread_t retired={.state=THREAD_STATE_TERMINATED,
   .running_cpu=THREAD_CPU_NONE,.queued=true,
   .kernel_stack={.active=true}};
 fixture_spinlock_depth=1;
 assert(!thread_destroy(&retired) && retired.kernel_stack.active && !freed);
 fixture_spinlock_depth=0;
 assert(thread_destroy(&retired) && !retired.kernel_stack.active && freed==1);

 /* SYSCALL_EXIT publishes process exit, then terminates its own thread.
  * The external guard must not reject this live syscall frame. */
 process_t self_process={.state=PROCESS_STATE_ACTIVE};
 kernel_thread_t self={.state=THREAD_STATE_RUNNING,.running_cpu=0,
   .entry_started=true,.process=&self_process,
   .syscall_active=true,.syscall_nesting=1,
   .process_state=THREAD_PROCESS_ATTACHED};
 self_process.main_thread=&self;
 self_process.threads=&self;self_process.thread_count=1;
 self_process.live_thread_count=1;
 current_process=&self_process; current_thread=&self;
 assert(!process_terminate_external(&self_process,2));
 assert(process_exit(&self_process,2));
 fixture_spinlock_depth=1;
 assert(!scheduler_terminate());
 assert(self.state==THREAD_STATE_RUNNING && !self_dispatches);
 fixture_spinlock_depth=0;
 assert(scheduler_terminate());
 assert(self.state==THREAD_STATE_TERMINATED && self_dispatches==1 &&
        self_handoffs==1 && !scheduler_lock &&
        self.syscall_active && self.syscall_nesting==1 &&
        exited==3 && closed_handles==3 && unmapped==3);

 process_t single = {.state=PROCESS_STATE_ACTIVE};
 kernel_thread_t main_thread={.state=THREAD_STATE_READY,
   .running_cpu=THREAD_CPU_NONE}, extra={.state=THREAD_STATE_READY,
   .running_cpu=THREAD_CPU_NONE};
 single.object.references=1;
 assert(process_attach_thread(&single, &main_thread));
 assert(!process_attach_thread(&single, &main_thread));
 assert(process_attach_thread(&single, &extra));
 assert(single.main_thread == &main_thread && extra.process == &single &&
        single.thread_count==2 && single.live_thread_count==2 &&
        single.object.references==3);

 /* A session/shutdown scan can encounter a process after registry insertion
  * but before its executable, handles and session metadata are complete. It
  * is tracked as in-flight work, but cannot be externally terminated until
  * its creator publishes the fully prepared launch. */
 process_t launching={.state=PROCESS_STATE_ACTIVE,.main_thread=&main_thread,
   .thread_count=1,.live_thread_count=1};
 assert(process_matches_shutdown_phase(&launching,&caller,false));
 assert(process_matches_shutdown_phase(&launching,&caller,true));
 unsigned before_launch_exit=exited;
 assert(!process_terminate_external(&launching,3));
 assert(!launching.termination_pending && exited==before_launch_exit);
 assert(process_publish_launch(&launching));
 assert(!process_matches_shutdown_phase(&launching,&caller,true));
 assert(process_matches_shutdown_phase(&launching,&caller,false));
 launching.system_service=true;
 launching.service_id=10U;
 assert(!process_matches_shutdown_phase(&launching,&caller,false));
 assert(process_matches_shutdown_phase(&launching,&caller,true));
 assert(process_termination_begin(&launching));
 process_termination_cancel(&launching);
 assert(!launching.termination_pending);
 puts("termination: production scheduler/process guards passed");
 return 0;
}
'''

# Session state/retirement is covered by the complete production state machine,
# not the old synchronous allocator/retirement model.
run(scheduler_prefix + scheduler_functions + scheduler_suffix, "scheduler")
subprocess.run([sys.executable, str(ROOT / "tests/kernel/session_lifecycle_test.py"),
                CC], check=True, timeout=90)
