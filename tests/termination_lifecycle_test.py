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

ROOT = Path(__file__).resolve().parents[1]
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
        path.write_text(source)
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
typedef int32_t i32;
typedef u32 process_handle_t;
enum {THREAD_STATE_READY, THREAD_STATE_RUNNING, THREAD_STATE_BLOCKED,
      THREAD_STATE_TERMINATED};
enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_TERMINATED};
#define MG_OK 0
#define MG_ERR_BUSY -14
#define MG_ERR_BAD_ARGUMENT -1
#define MG_ERR_INVALID_HANDLE -3
#define MG_ERR_NOT_CHILD -4
#define OBJECT_TYPE_PROCESS 2
#define THREAD_CPU_NONE ((u32)-1)
typedef struct kernel_thread kernel_thread_t;
typedef struct process process_t;
struct process {int state; kernel_thread_t *main_thread; process_t *parent;};
typedef struct {process_t *process;} kernel_object_t;
static kernel_object_t target_object;
struct kernel_thread {
 int state; u32 running_cpu, syscall_nesting;
 bool syscall_active, context_switch_pending, queued, sleeping;
 bool entry_started;
 uintptr_t preempt_return_rip;
 u64 wakeup_tick; kernel_thread_t *next;
 void *waiting_mutex; uintptr_t kernel_stack_base;
 process_t *process;
};
typedef struct {kernel_thread_t *scheduler_idle_thread;} cpu_local_t;
static cpu_local_t cpu;
static kernel_thread_t bootstrap_thread, *sleeping_threads;
static struct {u32 blocked_threads, sleeping_threads;} scheduler_stats;
static u32 scheduler_lock, process_registry_lock;
static unsigned removed, cancelled, freed, exited, closed_handles, unmapped;
static unsigned self_dispatches, self_handoffs;
static process_t *current_process;
static kernel_thread_t *current_thread, *idle_thread;
static kernel_thread_t selected_thread;
static u32 fixture_spinlock_depth;
typedef enum {SCHEDULER_DISPATCH_TERMINATE} scheduler_dispatch_action_t;
static u32 spinlock_depth_current(void) { return fixture_spinlock_depth; }
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
 return handle==17 ? &target_object : NULL;}
static process_t *object_process(kernel_object_t *object) {return object->process;}
static void console_cancel_waiter(kernel_thread_t *thread) {(void)thread;}
static bool process_exit(process_t *process, i32 status)
{(void)status; assert(process->state == PROCESS_STATE_ACTIVE);
 process->state = PROCESS_STATE_TERMINATED; exited++; closed_handles++;
 unmapped++; return true;}
'''
scheduler_functions = "".join(function(path, name) for path, name in (
    ("kernel/src/scheduler.c", "sleeping_remove"),
    ("kernel/src/scheduler.c", "scheduler_terminate_thread"),
    ("kernel/src/scheduler.c", "scheduler_terminate"),
    ("kernel/src/scheduler.c", "thread_destroy"),
    ("kernel/src/process.c", "process_attach_thread"),
    ("kernel/src/process.c", "process_terminate_external"),
    ("kernel/src/process.c", "process_terminate_child"),
))
scheduler_suffix = r'''
int main(void) {
 process_t caller={.state=PROCESS_STATE_ACTIVE};
 process_t target = {.state=PROCESS_STATE_ACTIVE,.parent=&caller};
 target_object.process=&target;
 kernel_thread_t thread = {.state=THREAD_STATE_BLOCKED,
   .running_cpu=THREAD_CPU_NONE, .syscall_active=true,
   .syscall_nesting=1, .entry_started=true,
   .waiting_mutex=&scheduler_lock, .process=&target};
 target.main_thread=&thread;
 scheduler_stats.blocked_threads=1;
 assert(!process_terminate_external(&target, 1));
 assert(process_terminate_child(&caller,17,1)==MG_ERR_BUSY);

 kernel_thread_t kernel_worker={.state=THREAD_STATE_READY,
   .running_cpu=THREAD_CPU_NONE, .entry_started=true, .queued=true};
 assert(!scheduler_terminate_thread(&kernel_worker));
 assert(!thread_destroy(&kernel_worker));
 assert(kernel_worker.queued && !freed);
 assert(target.state == PROCESS_STATE_ACTIVE &&
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
 assert(target.state==PROCESS_STATE_ACTIVE && thread.state==THREAD_STATE_READY &&
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
 assert(!process_terminate_external(&target, 1));
 assert(exited==1 && closed_handles==1 && unmapped==1 && cancelled==1);

 /* SYSCALL_EXIT publishes process exit, then terminates its own thread.
  * The external guard must not reject this live syscall frame. */
 process_t self_process={.state=PROCESS_STATE_ACTIVE};
 kernel_thread_t self={.state=THREAD_STATE_RUNNING,.running_cpu=0,
   .entry_started=true,.process=&self_process,
   .syscall_active=true,.syscall_nesting=1};
 self_process.main_thread=&self;
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
        exited==2 && closed_handles==2 && unmapped==2);

 process_t single = {.state=PROCESS_STATE_ACTIVE};
 kernel_thread_t main_thread={0}, extra={0};
 assert(process_attach_thread(&single, &main_thread));
 assert(process_attach_thread(&single, &main_thread));
 assert(!process_attach_thread(&single, &extra));
 assert(single.main_thread == &main_thread && !extra.process);
 puts("termination: production scheduler/process guards passed");
 return 0;
}
'''

session_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint64_t mg_session_id_t;
typedef uint32_t u32;
enum {MG_SESSION_STATE_CREATING=1, MG_SESSION_STATE_ACTIVE,
      MG_SESSION_STATE_ENDING, MG_SESSION_STATE_ENDED};
enum {MG_SESSION_ACTIVITY_INACTIVE};
enum {MG_SERVICE_SESSIOND=1};
enum {MG_OK=0, MG_ERR_BUSY=-14, MG_ERR_NOT_FOUND=-2,
      MG_ERR_PRIVILEGE_REQUIRED=-25};
#define SESSION_MAX_ACTIVE 8U
typedef struct {u32 uid, role; char username[32];} user_identity_t;
typedef struct {mg_session_id_t id; u32 state;} mg_session_status_t;
typedef struct process {u64 pid; bool system_service; u32 service_id;} process_t;
typedef struct {
 mg_session_id_t id; u64 shell_pid, owner_pid;
 user_identity_t identity; u32 state, activity; bool active;
 bool retirement_in_progress;
} session_record_t;
static session_record_t session_records[SESSION_MAX_ACTIVE];
static mg_session_id_t next_session_id=1;
static u32 session_lock;
static bool can_terminate;
static bool reenter_retirement;
static unsigned termination_calls, reaps, overlapping_rejections;
static bool session_try_retire(mg_session_id_t id);
static u64 spin_lock_irqsave(u32 *lock) {assert(!*lock); *lock=1; return 0;}
static void spin_unlock_irqrestore(u32 *lock,u64 flags)
{(void)flags; assert(*lock); *lock=0;}
static bool session_backend_authorized(process_t *requester)
{return requester && requester->system_service &&
        requester->service_id == MG_SERVICE_SESSIOND;}
static bool process_terminate_session_members(mg_session_id_t id)
{assert(id && !session_lock); termination_calls++;
 if (reenter_retirement) {
   reenter_retirement=false;
   assert(!session_try_retire(id)); overlapping_rejections++;
 }
 return can_terminate;}
static void process_reap_session_members(mg_session_id_t id)
{assert(id && !session_lock); reaps++;}
static void session_status_from_record(const session_record_t *record,
                                       mg_session_status_t *status)
{status->id=record->id; status->state=record->state;}
'''
session_functions = "".join(function("kernel/src/session.c", name) for name in (
    "session_find", "session_allocate", "session_release_id",
    "session_try_retire", "session_end_process", "session_process_exited",
    "session_list_process",
))
session_suffix = r'''
int main(void) {
 process_t frontend={.pid=77}, backend={.pid=88,.system_service=true,
                         .service_id=MG_SERVICE_SESSIOND};
 session_record_t *first=session_allocate();
 assert(first); first->owner_pid=frontend.pid; first->state=MG_SESSION_STATE_ACTIVE;
 mg_session_id_t id=first->id;
 session_process_exited(&frontend);
 assert(first->active && first->id==id &&
        first->state==MG_SESSION_STATE_ENDING && !reaps);
 mg_session_status_t list[SESSION_MAX_ACTIVE]={0}; u32 count=0,total=0;
 assert(session_list_process(&backend,0,list,SESSION_MAX_ACTIVE,&count,&total)==MG_OK);
 assert(count==1 && total==1 && list[0].id==id &&
        list[0].state==MG_SESSION_STATE_ENDING);
 session_record_t *second=session_allocate();
 assert(second && second!=first && second->id!=id);
 reenter_retirement=true;
 assert(session_end_process(&backend,id)==MG_ERR_BUSY && first->active);
 assert(overlapping_rejections==1 && !first->retirement_in_progress);
 can_terminate=true;
 assert(session_end_process(&backend,id)==MG_OK && !first->active &&
        reaps==1 && termination_calls==3);
 assert(session_end_process(&backend,id)==MG_ERR_NOT_FOUND);

 session_record_t *third=session_allocate();
 assert(third==first); third->state=MG_SESSION_STATE_ACTIVE;
 mg_session_id_t backend_id=third->id;
 can_terminate=false; reaps=0;
 session_process_exited(&backend);
 assert(third->active && third->id==backend_id &&
        third->state==MG_SESSION_STATE_ENDING && !reaps);
 assert(session_list_process(&backend,0,list,SESSION_MAX_ACTIVE,&count,&total)==MG_OK);
 assert(total==2);
 can_terminate=true;
 assert(session_end_process(&backend,backend_id)==MG_OK);
 assert(!third->active && reaps==1);
 puts("termination: production session busy/retry lifecycle passed");
 return 0;
}
'''

run(scheduler_prefix + scheduler_functions + scheduler_suffix, "scheduler")
run(session_prefix + session_functions + session_suffix, "session")
