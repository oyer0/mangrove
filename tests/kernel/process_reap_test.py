#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production process collection across a controlled child exit/handoff.

The blocked parent wakes after process termination is published, while the
child still owns a resumable kernel cleanup frame on the other CPU. Optional
source arguments run the same assertions against an earlier implementation.
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
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
enum {PROCESS_STATE_ACTIVE=1, PROCESS_STATE_TERMINATED};
enum {THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
      THREAD_STATE_TERMINATED};
enum {MG_OK=0, MG_ERR_BAD_ARGUMENT=-4, MG_ERR_INVALID_HANDLE=-8,
      MG_ERR_BUSY=-14, MG_ERR_SHUTDOWN=-38, MG_ERR_WOULD_BLOCK=-19,
      MG_ERR_NOT_CHILD=-22, OBJECT_TYPE_PROCESS=3};
#define PROCESS_NO_SESSION 0
#define THREAD_CPU_NONE UINT32_MAX
typedef struct kernel_thread {
 u32 state, running_cpu; bool context_switch_pending, syscall_active, sleeping;
 uintptr_t preempt_return_rip;
} kernel_thread_t;
typedef struct process process_t;
typedef struct {unsigned ref_count; process_t *process;} kernel_object_t;
struct process {
 int state; bool wait_collected, owner_reference_held, shutdown_requested;
 bool launch_ready;
 i32 exit_status; u64 session_id; u32 active_syscalls;
 process_t *parent, *waiting_child, *next_all, *first_child, *next_sibling;
 kernel_thread_t *main_thread, *waiting_thread; kernel_object_t object;
 void *address_space, *handle_table;
};
static u32 process_registry_lock, scheduler_lock;
static process_t parent, child, *all_processes;
static kernel_thread_t parent_thread, child_thread;
static unsigned sleep_calls, owner_releases, step;
static bool block_success, sleep_success, request_shutdown;
static bool cancel_initial, expected_panic;
static unsigned destroyed_roots, released_memory, frees, external_terminations;
static jmp_buf panic_environment;
static bool scheduler_thread_exit_complete(const kernel_thread_t *thread);
static u64 spin_lock_irqsave(u32 *lock)
{assert(!*lock);*lock=1;return 1;}
static void spin_unlock_irqrestore(u32 *lock,u64 flags)
{(void)flags;assert(*lock);*lock=0;}
static void spin_lock(u32 *lock){(void)spin_lock_irqsave(lock);}
static void spin_unlock(u32 *lock){spin_unlock_irqrestore(lock,0);}
static u64 process_cli_save(void){return 1;}
static void process_cli_restore(u64 flags){(void)flags;}
static kernel_thread_t *thread_current(void){return &parent_thread;}
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
{assert(!process_registry_lock && object==&child.object);
 /* No collector may release ownership before the child is really done. */
 if(!child.owner_reference_held || object->ref_count==1)
   assert(scheduler_thread_exit_complete(child.main_thread));
 assert(object->ref_count);object->ref_count--;
 if(!child.owner_reference_held && !owner_releases)owner_releases++;
 if(!object->ref_count)all_processes=NULL;}
static void process_unlink_locked(process_t *p)
{assert(process_registry_lock && p==&child);p->parent=NULL;}
static bool scheduler_block(void)
{assert(!process_registry_lock && !scheduler_lock);
 if(!block_success)return false;
 child.state=PROCESS_STATE_TERMINATED;
 child_thread.state=THREAD_STATE_READY;child_thread.running_cpu=THREAD_CPU_NONE;
 child_thread.syscall_active=true;
 /* process_exit() woke the parent before the child's scheduler_terminate(). */
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
 if(p->state!=PROCESS_STATE_ACTIVE)return false;
 p->state=PROCESS_STATE_TERMINATED;
 child_thread.state=THREAD_STATE_TERMINATED;return true;}
static void process_cwd_clear(process_t *p){assert(p==&child);}
static void process_unlink_all(process_t *p){assert(p==&child);all_processes=NULL;}
static bool thread_destroy(kernel_thread_t *t)
{return scheduler_thread_exit_complete(t) || cancel_initial;}
static void process_memory_release_all(process_t *p)
{assert(p==&child);released_memory++;}
static void vmm_destroy_address_space(void *root)
{assert(root==child.address_space);destroyed_roots++;}
static void kfree(void *p){(void)p;frees++;}
static __attribute__((unused)) void panic(const char *message)
{assert(expected_panic && !strcmp(message,"process: destruction before main thread exit"));
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
running = function(scheduler, 'scheduler_thread_is_running')
running = running.replace('bool scheduler_thread_is_running(',
                          'static __attribute__((unused)) bool scheduler_thread_is_running(', 1)
code = prefix + completion + running
for name in ('process_wait', 'process_poll', 'process_reap_session_members',
             'process_terminate_session_members', 'process_object_destroy'):
    code += function(source, name)
code += r'''
static void reset(bool active)
{
 memset(&parent,0,sizeof(parent));memset(&child,0,sizeof(child));
 parent.state=PROCESS_STATE_ACTIVE;
 child.state=active?PROCESS_STATE_ACTIVE:PROCESS_STATE_TERMINATED;
 child.parent=&parent;child.main_thread=&child_thread;child.session_id=42;
 child.launch_ready=true;
 child.owner_reference_held=true;child.exit_status=23;
 child.object=(kernel_object_t){.ref_count=2,.process=&child};
 child_thread=(kernel_thread_t){.state=THREAD_STATE_READY,
   .running_cpu=THREAD_CPU_NONE,.syscall_active=true};
 all_processes=&child;parent_thread=(kernel_thread_t){0};
 process_registry_lock=scheduler_lock=sleep_calls=owner_releases=step=0;
 cancel_initial=expected_panic=false;
 destroyed_roots=released_memory=frees=external_terminations=0;
 block_success=sleep_success=true;request_shutdown=false;
}
int main(void)
{
 i32 status=-1;
 assert(!scheduler_thread_exit_complete(NULL));
 reset(true);
 assert(process_wait(&parent,7,&status)==MG_OK && status==23);
 assert(sleep_calls==2 && owner_releases==1 && child.wait_collected &&
        !child.owner_reference_held && !process_registry_lock && !scheduler_lock);
 reset(false);child_thread.state=THREAD_STATE_BLOCKED;
 assert(process_wait(&parent,7,&status)==MG_OK && sleep_calls==2);
 reset(false);child_thread.state=THREAD_STATE_TERMINATED;
 assert(process_wait(&parent,7,&status)==MG_OK && !sleep_calls);

 reset(false);status=-1;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK && status==-1);
 child_thread.state=THREAD_STATE_BLOCKED;
 assert(process_poll(&parent,7,&status)==MG_ERR_WOULD_BLOCK);
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
 assert(process_poll(&parent,7,&status)==MG_OK && status==23);
 assert(!owner_releases && !child.wait_collected);

 reset(false);child.object.ref_count=1;
 process_reap_session_members(42);assert(!owner_releases);
 child_thread.state=THREAD_STATE_TERMINATED;child_thread.context_switch_pending=true;
 process_reap_session_members(42);assert(!owner_releases);
 child_thread.context_switch_pending=false;
 process_reap_session_members(42);assert(owner_releases==1 && !all_processes);

 reset(false);request_shutdown=true;
 assert(process_wait(&parent,7,&status)==MG_ERR_SHUTDOWN && sleep_calls==1);
 assert(!owner_releases && !child.wait_collected && child.owner_reference_held);
 reset(true);parent.shutdown_requested=true;
 assert(process_wait(&parent,7,&status)==MG_ERR_SHUTDOWN && !owner_releases);
 assert(!child.wait_collected && child.owner_reference_held);
 reset(true);block_success=false;
 assert(process_wait(&parent,7,&status)==MG_ERR_BUSY && !owner_releases);
 reset(false);sleep_success=false;
 assert(process_wait(&parent,7,&status)==MG_ERR_BUSY && !owner_releases);

 reset(false);
 assert(!process_terminate_session_members(42) && child.owner_reference_held);
 child_thread.state=THREAD_STATE_TERMINATED;
 assert(process_terminate_session_members(42) && !owner_releases);

 /* A missed admission check must fail before retiring page tables. */
 reset(false);expected_panic=true;
 if(!setjmp(panic_environment)) {
   process_object_destroy(&child.object);
   assert(!"live-thread destruction did not fail closed");
 }
 assert(!destroyed_roots && !released_memory && !frees);
 reset(false);child_thread.state=THREAD_STATE_TERMINATED;
 process_object_destroy(&child.object);
 assert(destroyed_roots==1 && released_memory==1 && frees==2);
 /* Unpublished process creation rollback can cancel an initial READY frame. */
 reset(false);cancel_initial=true;
 process_object_destroy(&child.object);
 assert(destroyed_roots==1 && frees==2);
 puts("process reaping: wake-before-exit, suspended cleanup, remote handoff, poll, session collection, and shutdown cancellation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-process-reap-') as directory:
    path = Path(directory) / 'reap.c'
    path.write_text(code)
    binary = path.with_suffix('')
    subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', str(binary)],
                   check=True)
    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'detect_leaks=0'
    subprocess.run([str(binary)], check=True, env=env)
