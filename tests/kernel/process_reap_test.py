#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production process wait/poll/reap after thread cleanup and stack handoff."""
from pathlib import Path
import sys
import argparse
import os
import re

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/shared"))
from host_fixture import HostFixture
parser = argparse.ArgumentParser()
parser.add_argument('cc', nargs='?', default='cc')
parser.add_argument('--process-source', type=Path,
                    default=ROOT / 'kernel/src/process.c')
parser.add_argument('--scheduler-source', type=Path,
                    default=ROOT / 'kernel/src/scheduler.c')
args = parser.parse_args()


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


source = args.process_source.read_text()
scheduler = args.scheduler_source.read_text()
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t i32;
typedef u64 process_handle_t;
typedef u64 mg_session_id_t;
enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED};
enum {THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
      THREAD_STATE_TERMINATED};
typedef enum {SCHEDULER_WAKE_FAILED=0, SCHEDULER_WAKE_DELIVERED,
               SCHEDULER_WAKE_ALREADY_TERMINATED} scheduler_wake_result_t;
typedef int thread_state_t;
enum {THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
      THREAD_PROCESS_EXIT_REQUESTED, THREAD_PROCESS_HANDOFF_COMPLETE};
enum {MG_OK=0, MG_ERR_BAD_ARGUMENT=-4, MG_ERR_INVALID_HANDLE=-8,
      MG_ERR_BUSY=-14, MG_ERR_SHUTDOWN=-38, MG_ERR_WOULD_BLOCK=-19,
      MG_ERR_NOT_CHILD=-22, OBJECT_TYPE_PROCESS=3};
#define PROCESS_NO_SESSION 0
#define THREAD_CPU_NONE UINT32_MAX
typedef struct process process_t;
typedef struct process_thread_record process_thread_record_t;
typedef struct kernel_thread {
 u32 state, running_cpu; bool context_switch_pending, syscall_active, sleeping;
 bool event_wake_pending;
 bool stack_external;
 uintptr_t preempt_return_rip;
 process_t *process;
 struct kernel_thread *process_next;
 int process_state;
 bool process_initial, process_reference_held;
 volatile bool process_exit_requested;
 i32 process_exit_status;
 bool process_exit_status_valid;
 process_thread_record_t *worker_record;
} kernel_thread_t;
typedef struct {unsigned ref_count; process_t *process; int type;}
 kernel_object_t;
struct process_thread_record {
 kernel_object_t object; process_t *process;
 kernel_thread_t *execution, *join_waiter;
 process_thread_record_t *next;
 void *stack_region, *tls_region;
 bool completed, kernel_stack_released, join_claimed, listed, reclaiming;
};
struct process {
 int state; bool wait_collected, owner_reference_held, shutdown_requested;
 bool exit_requested;
 bool launch_ready, wait_claimed, exit_cleanup_complete,
      exit_cleanup_in_progress,
      thread_reap_in_progress, exit_notification_sent, termination_pending,
      external_termination_in_progress;
 i32 exit_status; u64 session_id; u32 active_syscalls;
 process_t *parent, *waiting_child, *next_all, *first_child, *next_sibling;
 kernel_thread_t *main_thread, *threads, *waiting_thread;
 u32 thread_count, live_thread_count, thread_list_readers;
 u32 scheduler_handoff_references;
 _Atomic u32 scheduler_handoff_releases_pending;
 _Atomic u32 deferred_reference_releases_pending;
 bool deferred_reference_collection_retry;
 u32 worker_reapers;
 process_thread_record_t *thread_records;
 bool worker_reap_pending;
 u32 storage_authorized_operation; u64 storage_authorized_thread_id;
 bool storage_management_session, storage_management_authorizing;
 kernel_object_t object;
 void *address_space, *handle_table;
};
static u32 process_registry_lock, scheduler_lock;
static process_t parent, child, *all_processes;
static kernel_thread_t parent_thread, child_thread;
static unsigned sleep_calls, owner_releases, step;
static bool block_success, sleep_success, request_shutdown;
static bool cancel_initial, expected_panic;
static unsigned destroyed_roots, released_memory, frees, external_terminations;
static unsigned block_calls, exit_notifications, thread_reaps;
static unsigned user_region_metadata_releases;
static jmp_buf panic_environment;
static bool scheduler_thread_exit_complete(const kernel_thread_t *thread);
static void process_scheduler_handoff_complete(kernel_thread_t *thread);
void process_handle_reference_release(kernel_object_t *object);
static bool process_thread_list_read_acquire_locked(process_t *process)
{
 assert(process_registry_lock);
 if(!process||process->state==PROCESS_STATE_TERMINATED||
    !process->threads||!process->thread_count||
    process->thread_list_readers==UINT32_MAX)
  return false;
 process->thread_list_readers++;
 return true;
}
static void process_thread_list_read_release(process_t *process)
{
 assert(process_registry_lock==0&&process&&process->thread_list_readers);
 process->thread_list_readers--;
}
/* This fixture models a safe process context and does not include the
 * deferred cleanup worker. The focused deferred-release fixture exercises
 * the production unsafe-context retry path. */
static bool process_cleanup_context_valid(void){return true;}
static void process_cleanup_worker_request_session_collection_retry(void){}
static u64 spin_lock_irqsave(u32 *lock)
{assert(!*lock);*lock=1;return 1;}
static void spin_unlock_irqrestore(u32 *lock,u64 flags)
{(void)flags;assert(*lock);*lock=0;}
static void spin_lock(u32 *lock){(void)spin_lock_irqsave(lock);}
static void spin_unlock(u32 *lock){spin_unlock_irqrestore(lock,0);}
static u64 process_cli_save(void){return 1;}
static void process_cli_restore(u64 flags){(void)flags;}
static kernel_thread_t *thread_current(void){return &parent_thread;}
static void scheduler_clear_event_wake(kernel_thread_t*t){t->event_wake_pending=false;}
static bool scheduler_wake_waiter(kernel_thread_t *thread)
{if(!thread)return false;thread->event_wake_pending=true;return true;}
static scheduler_wake_result_t scheduler_wake_waiter_result(
    kernel_thread_t *thread)
{if(!thread)return SCHEDULER_WAKE_FAILED;
 if(thread->state==THREAD_STATE_TERMINATED)
  return SCHEDULER_WAKE_ALREADY_TERMINATED;
 return scheduler_wake_waiter(thread)?SCHEDULER_WAKE_DELIVERED:
        SCHEDULER_WAKE_FAILED;}
static bool process_shutdown_requested(const process_t *p)
{return p->shutdown_requested;}
static kernel_object_t *process_handle_lookup_any(process_t *p,
 process_handle_t h,int type,u32 rights)
{(void)rights;assert(p==&parent && h==7 && type==OBJECT_TYPE_PROCESS);
 child.object.ref_count++; return &child.object;}
static process_t *object_process(kernel_object_t *object)
{return object->process;}
static bool object_reference(kernel_object_t *object)
{assert(process_registry_lock);object->ref_count++;return true;}
static void object_release(kernel_object_t *object)
{assert(!process_registry_lock && object);
 /* No collector may release ownership before the child is really done. */
 if(object==&child.object &&
    (!child.owner_reference_held || object->ref_count==1))
   assert(child.thread_count==0 ||
          scheduler_thread_exit_complete(child.main_thread));
 assert(object->ref_count);object->ref_count--;
 if(object==&child.object && !child.owner_reference_held && !owner_releases)
   owner_releases++;
 if(object==&child.object && !object->ref_count)all_processes=NULL;}
static void process_unlink_locked(process_t *p)
{assert(process_registry_lock && p==&child);p->parent=NULL;}
static bool scheduler_block(void)
{assert(!process_registry_lock && !scheduler_lock);
 block_calls++;if(!block_success)return false;
 if(request_shutdown){parent.shutdown_requested=true;return true;}
 if(parent.shutdown_requested)return true;
 child.state=PROCESS_STATE_EXITING;child.exit_cleanup_complete=true;
 child_thread.state=THREAD_STATE_TERMINATED;child_thread.running_cpu=THREAD_CPU_NONE;
 child_thread.context_switch_pending=false;child_thread.sleeping=false;
 child_thread.preempt_return_rip=0;
 child_thread.process_state=THREAD_PROCESS_EXIT_REQUESTED;
 process_scheduler_handoff_complete(&child_thread);
 /* Completion is published only after resource cleanup and stack handoff. */
 parent.waiting_child=NULL;parent.waiting_thread=NULL;
 return true;}
static __attribute__((unused)) bool scheduler_sleep(u64 ticks)
{assert(ticks==1 && !process_registry_lock && !scheduler_lock);
 sleep_calls++;if(!sleep_success)return false;
 if(request_shutdown){parent.shutdown_requested=true;return true;}
 if(step++==0){child_thread.state=THREAD_STATE_TERMINATED;
   child_thread.running_cpu=1;child_thread.context_switch_pending=true;}
 else{child_thread.running_cpu=THREAD_CPU_NONE;
   child_thread.context_switch_pending=false;}
 return true;}
static bool process_terminate_external(process_t *p,i32 status)
{assert(p==&child && status==-1);external_terminations++;
 if(p->state!=PROCESS_STATE_ACTIVE || p->active_syscalls)return false;
 p->state=PROCESS_STATE_EXITING;p->exit_cleanup_complete=true;
 child_thread.state=THREAD_STATE_TERMINATED;child_thread.running_cpu=THREAD_CPU_NONE;
 child_thread.context_switch_pending=false;child_thread.process_state=THREAD_PROCESS_EXIT_REQUESTED;
 process_scheduler_handoff_complete(&child_thread);
 return true;}
static void process_cwd_clear(process_t *p){assert(p==&child);}
static void process_unlink_all(process_t *p){assert(p==&child);all_processes=NULL;}
static bool thread_destroy(kernel_thread_t *t)
{if(cancel_initial)return true;
 if(!t || t->process || t->process_state!=THREAD_PROCESS_UNATTACHED ||
    !scheduler_thread_exit_complete(t))return false;
 thread_reaps++;return true;}
static void process_memory_release_all(process_t *p)
{assert(p==&child);released_memory++;}
static void vmm_destroy_address_space(void *root)
{assert(root==child.address_space);destroyed_roots++;}
static void process_user_region_metadata_release(process_t *p)
{assert(p==&child && destroyed_roots==1);user_region_metadata_releases++;}
static void kfree(void *p){(void)p;frees++;}
static bool kernel_stack_release_context_valid(void){return true;}
static void session_process_exited(process_t *p)
{(void)p;exit_notifications++;}
static __attribute__((unused)) void panic(const char *message)
{assert(expected_panic && !strcmp(message,"process: destruction with owned thread state"));
 longjmp(panic_environment,1);}
'''
# Old-source negative controls supply this predicate only to the fixture's
# assertions. Old production code still calls its original running predicate.
completion = '''static bool scheduler_thread_exit_complete(const kernel_thread_t *t)
{return t && t->state==THREAD_STATE_TERMINATED &&
 t->running_cpu==THREAD_CPU_NONE && !t->context_switch_pending;}\n'''
if re.search(r'^bool scheduler_thread_exit_complete\(', scheduler, re.M):
    completion = function(scheduler, 'scheduler_thread_exit_complete')
    completion = completion.replace('bool scheduler_thread_exit_complete(',
                                     'static bool scheduler_thread_exit_complete(', 1)
handoff_valid = function(scheduler, 'scheduler_thread_handoff_callback_valid')
handoff_valid = handoff_valid.replace(
    'bool scheduler_thread_handoff_callback_valid(',
    'static bool scheduler_thread_handoff_callback_valid(', 1)
running = function(scheduler, 'scheduler_thread_is_running')
running = running.replace('bool scheduler_thread_is_running(',
                          'static __attribute__((unused)) bool scheduler_thread_is_running(', 1)
state_snapshot = function(scheduler, 'scheduler_thread_state_snapshot')
code = prefix + completion + handoff_valid + running + state_snapshot
code += function(source, 'process_storage_authorization_clear_locked')
for name in ('process_thread_list_complete_locked',
             'process_thread_member_locked',
             'process_session_collection_ready_locked',
             'process_worker_records_release_all',
             'process_waiter_wake_locked',
             'process_publish_terminated_locked',
             'process_scheduler_handoff_complete_internal',
             'process_scheduler_handoff_complete',
             'process_detach_thread_owned', 'process_detach_thread',
             'process_finalize_exit',
             'process_wait', 'process_poll', 'process_collect_completed', 'process_reap_session_members',
             'process_handle_reference_release',
             'process_object_destroy'):
    code += function(source, name)
code += r'''
static void reset(bool active)
{
 memset(&parent,0,sizeof(parent));memset(&child,0,sizeof(child));
 parent.state=PROCESS_STATE_ACTIVE;
 child.state=active?PROCESS_STATE_ACTIVE:PROCESS_STATE_TERMINATED;
 child.parent=&parent;child.main_thread=&child_thread;child.threads=&child_thread;
 child.thread_count=1;child.live_thread_count=active?1:0;
 child.exit_cleanup_complete=!active;child.session_id=42;
 child.launch_ready=true;
 child.owner_reference_held=true;child.exit_status=23;
 child.object=(kernel_object_t){.ref_count=2,.process=&child,
                                 .type=OBJECT_TYPE_PROCESS};
 parent.object=(kernel_object_t){.ref_count=2,.process=&parent,
                                  .type=OBJECT_TYPE_PROCESS};
 child_thread=(kernel_thread_t){.state=THREAD_STATE_READY,
   .running_cpu=THREAD_CPU_NONE,.syscall_active=true,.process=&child,
   .process_state=active?THREAD_PROCESS_ATTACHED:
                       THREAD_PROCESS_HANDOFF_COMPLETE,
   .process_initial=true,.process_reference_held=true};
 if(!active)child_thread.state=THREAD_STATE_TERMINATED;
 all_processes=&child;parent_thread=(kernel_thread_t){0};
 process_registry_lock=scheduler_lock=sleep_calls=owner_releases=step=0;
 block_calls=exit_notifications=thread_reaps=0;
 user_region_metadata_releases=0;
 cancel_initial=expected_panic=false;
 destroyed_roots=released_memory=frees=external_terminations=0;
 block_success=sleep_success=true;request_shutdown=false;
 parent.threads=parent.main_thread=&parent_thread;parent.thread_count=1;
 parent_thread.process=&parent;parent_thread.process_reference_held=true;
 parent_thread.process_state=THREAD_PROCESS_ATTACHED;
 parent_thread.state=THREAD_STATE_RUNNING;
}
int main(void)
{
 i32 status=-1;
 assert(!scheduler_thread_exit_complete(NULL));
 reset(true);
 assert(process_wait(&parent,7,&status)==MG_OK && status==23);
 assert(block_calls==1 && owner_releases==1 && child.wait_collected &&
        !child.owner_reference_held && !process_registry_lock && !scheduler_lock);
 reset(false);child_thread.state=THREAD_STATE_BLOCKED;
 assert(process_wait(&parent,7,&status)==MG_ERR_BUSY && block_calls==0);
 reset(false);
 int completed_wait=process_wait(&parent,7,&status);
 assert(completed_wait==MG_OK && !block_calls);

 reset(true);child.object.ref_count++; /* parent handle-table reference */
 status=-1;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK && status==-1);
 child_thread.state=THREAD_STATE_BLOCKED;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK);
 child.state=PROCESS_STATE_EXITING;child.exit_cleanup_complete=true;
 child_thread.state=THREAD_STATE_TERMINATED;child_thread.running_cpu=1;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK);
 child_thread.running_cpu=THREAD_CPU_NONE;child_thread.context_switch_pending=true;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK);
 child_thread.context_switch_pending=false;
 child_thread.sleeping=true;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK);
 child_thread.sleeping=false;child_thread.preempt_return_rip=123;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK);
 child_thread.preempt_return_rip=0;
 child.state=PROCESS_STATE_TERMINATED;child.live_thread_count=0;
 child_thread.process_state=THREAD_PROCESS_HANDOFF_COMPLETE;
 assert(process_poll(&parent,7,&status)==MG_OK && status==23);
 assert(!owner_releases && !child.wait_collected);

 reset(false);child.object.ref_count=1;
 process_reap_session_members(42);assert(!owner_releases);
 child_thread.state=THREAD_STATE_TERMINATED;child_thread.context_switch_pending=true;
 process_reap_session_members(42);assert(!owner_releases);
 child_thread.context_switch_pending=false;
 child.object.ref_count=2; /* process owner plus its retained thread record */
 process_reap_session_members(42);assert(owner_releases==1 && !all_processes);

 reset(true);request_shutdown=true;
 assert(process_wait(&parent,7,&status)==MG_ERR_SHUTDOWN && block_calls==1);
 assert(!owner_releases && !child.wait_collected && child.owner_reference_held);
 reset(true);parent.shutdown_requested=true;
 assert(process_wait(&parent,7,&status)==MG_ERR_SHUTDOWN && !owner_releases);
 assert(!child.wait_collected && child.owner_reference_held);
 reset(true);block_success=false;
 assert(process_wait(&parent,7,&status)==MG_ERR_BUSY && !owner_releases);

 reset(true);
 child.active_syscalls=1;
 assert(!process_terminate_external(&child,-1) && child.owner_reference_held);
 child.active_syscalls=0;
 assert(process_terminate_external(&child,-1));
 process_reap_session_members(42);
 assert(owner_releases==1 &&
        child.state==PROCESS_STATE_TERMINATED && !child.thread_count &&
        !child.owner_reference_held &&
        child_thread.process_state==THREAD_PROCESS_UNATTACHED &&
        !child.object.ref_count && !all_processes);

 /* A missed admission check must fail before retiring page tables. */
 reset(false);expected_panic=true;
 if(!setjmp(panic_environment)) {
   process_object_destroy(&child.object);
   assert(!"live-thread destruction did not fail closed");
 }
 assert(!destroyed_roots && !released_memory && !frees &&
        !user_region_metadata_releases);
 reset(false);
 assert(process_finalize_exit(&child));
 process_object_destroy(&child.object);
 assert(destroyed_roots==1 && released_memory==1 && frees==2 &&
        user_region_metadata_releases==1);

 /* A worker pin may make the first session-collection final-count check
  * defer after final thread removal. The completed empty list must remain a
  * valid candidate on the retry after the pin is released. */
 reset(false);child.address_space=&child;
 assert(process_finalize_exit(&child));
 assert(child.thread_count==0 && child.exit_notification_sent &&
        process_session_collection_ready_locked(&child));
 child.object.ref_count++; /* cleanup worker scan pin */
 process_reap_session_members(42);
 assert(child.owner_reference_held && child.object.ref_count==2 &&
        !owner_releases);
 object_release(&child.object);
 process_reap_session_members(42);
 assert(!child.owner_reference_held && child.object.ref_count==0 &&
        owner_releases==1 && !all_processes);

 /* An open process handle remains an external owner. Closing its slot and
  * then dropping the retained lookup reference automatically retries the
  * completed session collection. */
 reset(false);child.address_space=&child;
 child.object.ref_count=4; /* owner, thread, handle slot, in-flight lookup */
 process_reap_session_members(42);
 assert(child.thread_count==1 && child.owner_reference_held &&
        !owner_releases);
 object_release(&child.object); /* close removes the table-owned reference */
 process_handle_reference_release(&child.object);
 assert(child.thread_count==0 && !child.owner_reference_held &&
        child.object.ref_count==0 && owner_releases==1 && !all_processes);
 puts("process reaping: wake-before-exit, suspended cleanup, remote handoff, poll, session collection, and shutdown cancellation passed");
}
'''
with HostFixture("process-reap") as workspace:
    directory = workspace.path
    path = Path(directory) / 'reap.c'
    path.write_text('#include "' + str(ROOT / "tests/kernel/process_cleanup_fixture.h") + '"\n' + code)
    binary = path.with_suffix('')
    workspace.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', str(binary)],
                   check=True)
    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'detect_leaks=0'
    workspace.run([str(binary)], check=True, env=env)
