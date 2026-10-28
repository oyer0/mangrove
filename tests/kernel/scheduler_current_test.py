#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production current-thread operations across a controlled CPU migration.

The fixture interrupts the CPU-local pointer lookup before its dereference,
then resumes the caller on CPU 1 while CPU 0 runs a different thread. No host
threads or timing assumptions are used. Hardware switching is tested natively.
An optional scheduler.c argument runs the same assertions against old source.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else (ROOT / 'kernel/src/scheduler.c').read_text()


def function(name):
    for match in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', source, re.M):
        start = source.index('{', match.end())
        if ';' in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    raise ValueError(name)

prefix = r'''
#define _GNU_SOURCE
#define KERNEL_STACK_DIAGNOSTICS 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#ifdef __linux__
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#define KERNEL_BOOT_DEBUG_LOG(...) ((void)printf(__VA_ARGS__))
typedef uint64_t u64; typedef unsigned u32;
enum { THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED, THREAD_STATE_TERMINATED };
enum { THREAD_PRIORITY_HIGH=0, THREAD_PRIORITY_NORMAL=1,
       THREAD_PRIORITY_BACKGROUND=2 };
typedef enum {SCHEDULER_DISPATCH_REQUEUE, SCHEDULER_DISPATCH_BLOCK,
              SCHEDULER_DISPATCH_TERMINATE} scheduler_dispatch_action_t;
#define THREAD_CPU_NONE ((u32)-1)
#define scheduler_trace(...) ((void)0)
typedef struct {bool active;} kernel_stack_t;
typedef struct kernel_thread {
 unsigned state, running_cpu, effective_priority, base_priority;
 bool queued, sleeping, entry_started, wakeup_boosted;
 void *process; const char *name; void (*entry)(void *); void *entry_argument;
 uintptr_t saved_stack_pointer; u64 wakeup_deadline_ms; struct kernel_thread *next;
 kernel_stack_t kernel_stack; bool process_initial;
} kernel_thread_t;
typedef struct {
 unsigned index; bool bsp;
 kernel_thread_t *scheduler_current_thread, *scheduler_idle_thread;
 unsigned scheduler_context_switching; bool scheduler_preemption_pending;
 u64 kernel_thread_runs;
} cpu_local_t;
static cpu_local_t cpus[2];
static kernel_thread_t caller, other, idle[2], target, bootstrap_thread;
static kernel_thread_t *sleeping_threads, *next_target;
static struct {unsigned blocks, blocked_threads, sleeping_threads, wakeups, dispatches[3];} scheduler_stats;
static unsigned active_cpu, scheduler_lock, migrations, handoffs;
static bool enqueue_allowed;
static u64 scheduler_tick_count;
static u64 uptime_ms;
static bool interrupts_enabled, migrate_armed;
static jmp_buf entry_return;
extern char __stack_guard_bottom[], __stack_bottom[];
__asm__(".pushsection .rodata\n"
        ".globl __stack_guard_bottom\n"
        "__stack_guard_bottom:\n"
        ".space 4096\n"
        ".globl __stack_bottom\n"
        "__stack_bottom:\n"
        ".space 1\n"
        ".popsection\n");
static unsigned stack_samples;
static unsigned process_completion_calls;
static int process_completion_status;
static void kernel_stack_sample_rsp(kernel_stack_t *stack, uintptr_t rsp)
{assert(stack && stack->active && rsp); stack_samples++;}
static bool kernel_stack_guard_contains(const kernel_stack_t *stack,
                                       uintptr_t address)
{return stack && stack->active && address == 0xdeadU;}
static void set_gs(void) {
#ifdef __linux__
 assert(syscall(SYS_arch_prctl,ARCH_SET_GS,&cpus[active_cpu])==0);
#endif
}
static void migrate(void) {
 assert(interrupts_enabled && active_cpu==0);
 cpus[0].scheduler_current_thread=&other;
 cpus[1].scheduler_current_thread=&caller;
 caller.running_cpu=1; other.running_cpu=0;
 active_cpu=1; migrate_armed=false; migrations++;set_gs();
}
static cpu_local_t *cpu_current(void) {
 cpu_local_t *snapshot=&cpus[active_cpu];
 /* Timer preemption after GS lookup but before the caller dereferences it. */
 if (interrupts_enabled && migrate_armed) migrate();
 return snapshot;
}
static unsigned cpu_current_index(void) {return active_cpu;}
static u64 timer_uptime_ms(void) {return uptime_ms;}
static unsigned spinlock_depth_current(void) {return scheduler_lock;}
static u64 spin_lock_irqsave(unsigned *lock) {
 bool enabled=interrupts_enabled;
 /* A pending interrupt can run before CLI, but never after it. */
 if (enabled && migrate_armed) migrate();
 interrupts_enabled=false; assert(!*lock); *lock=1; return enabled;
}
static void spin_unlock_irqrestore(unsigned *lock,u64 flags) {
 assert(*lock); *lock=0; interrupts_enabled=flags;
}
static bool thread_saved_stack_valid(const kernel_thread_t *t) {return t->saved_stack_pointer!=0;}
static bool thread_context_ready(const kernel_thread_t *t) {return thread_saved_stack_valid(t);}
static kernel_thread_t *scheduler_select_next_locked(void) {assert(scheduler_lock);return next_target;}
static bool scheduler_consume_pending_wake_locked(kernel_thread_t *t,bool block) {(void)t;(void)block;return false;}
static void sleeping_remove(kernel_thread_t *t) {assert(sleeping_threads==t);sleeping_threads=t->next;t->sleeping=false;}
static bool scheduler_enqueue_locked(kernel_thread_t *t) {assert(scheduler_lock);if(!enqueue_allowed)return false;t->queued=true;return true;}
static bool scheduler_age_ready_threads_locked(void) {assert(scheduler_lock);return false;}
static void panic(const char *s) {(void)s;assert(!"unexpected scheduler panic");}
static void process_thread_entry_completed(kernel_thread_t *thread,int status)
{assert(thread && thread->process);process_completion_calls++;
 process_completion_status=status;}
static bool scheduler_handoff(kernel_thread_t *out,kernel_thread_t *in,u64 flags) {
 assert(out==&caller && in==&target && active_cpu==1);
 handoffs++; spin_unlock_irqrestore(&scheduler_lock,flags);return true;
}
'''
macros = r'''
#define current_thread (scheduler_cpu_local()->scheduler_current_thread)
#define idle_thread (scheduler_cpu_local()->scheduler_idle_thread)
#define preemption_pending (scheduler_cpu_local()->scheduler_preemption_pending)
#define scheduler_context_switch_in_progress (scheduler_cpu_local()->scheduler_context_switching)
'''
suffix = r'''
static void reset(bool success) {
 memset(cpus,0,sizeof(cpus));memset(&scheduler_stats,0,sizeof(scheduler_stats));
 caller=(kernel_thread_t){.state=THREAD_STATE_RUNNING,.running_cpu=0,.saved_stack_pointer=0x1000};
 other=(kernel_thread_t){.state=THREAD_STATE_RUNNING,.running_cpu=0,.saved_stack_pointer=0x2000};
 target=(kernel_thread_t){.state=THREAD_STATE_READY,.running_cpu=THREAD_CPU_NONE,.saved_stack_pointer=0x3000};
 for(unsigned i=0;i<2;i++){cpus[i].index=i;cpus[i].bsp=i==0;cpus[i].scheduler_idle_thread=&idle[i];}
 cpus[0].scheduler_current_thread=&caller;
 active_cpu=0;scheduler_lock=0;migrations=0;handoffs=0;
 scheduler_tick_count=0;enqueue_allowed=true;
 uptime_ms=100;
 sleeping_threads=NULL;next_target=success?&target:NULL;
 interrupts_enabled=true;migrate_armed=true;set_gs();
}
static void entered(void *argument) {
 assert(argument==&caller && caller.entry_started && !other.entry_started);
 assert(other.state==THREAD_STATE_RUNNING && other.running_cpu==0);
 assert(stack_samples==1);
 assert(!scheduler_lock && interrupts_enabled);
 longjmp(entry_return,1);
}
static void returning(void *argument) {(void)argument;}
static void check_entry_completion(bool initial,int expected_status)
{
 int process_identity=1;
 reset(true);
 caller.process=&process_identity;caller.process_initial=initial;
 caller.entry=returning;caller.entry_argument=NULL;
 process_completion_calls=0;process_completion_status=99;
 thread_entry_trampoline();
 assert(process_completion_calls==1 &&
        process_completion_status==expected_status &&
        caller.state==THREAD_STATE_TERMINATED &&
        caller.running_cpu==THREAD_CPU_NONE);
}
int main(int argc,char **argv) {
 assert(argc==2);
#ifdef __linux__
 unsigned long original_gs;
 assert(syscall(SYS_arch_prctl,ARCH_GET_GS,&original_gs)==0);
#endif
 if(!strcmp(argv[1],"snapshot")) {
#ifdef __linux__
  reset(false);
  assert(scheduler_current_thread()==&caller);
  assert(interrupts_enabled && !scheduler_lock);
  reset(false);migrate();
  assert(scheduler_current_thread()==&caller);
  cpus[active_cpu].scheduler_current_thread=NULL;
  assert(scheduler_current_thread()==NULL);
  assert(scheduler_current_stack_guard_fault(
      (uintptr_t)__stack_guard_bottom + 1U));
#else
  puts("GS snapshot runtime check requires Linux x86-64; target build checks ABI");
#endif
 } else if(!strcmp(argv[1],"entry")) {
  reset(false);caller.entry=entered;caller.entry_argument=&caller;
  caller.kernel_stack.active=true;
  other.entry=entered;other.entry_argument=&other;
  caller.name="caller";other.name="other";
  if(!setjmp(entry_return))thread_entry_trampoline();
 } else if(!strcmp(argv[1],"entry-completion")) {
  check_entry_completion(true,-1);
  check_entry_completion(false,0);
 } else if(!strcmp(argv[1],"deadline")) {
  reset(false);migrate_armed=false;
  cpus[0].scheduler_current_thread=&idle[0];
  target=(kernel_thread_t){.state=THREAD_STATE_BLOCKED,
   .running_cpu=THREAD_CPU_NONE,.effective_priority=THREAD_PRIORITY_NORMAL,
   .base_priority=THREAD_PRIORITY_NORMAL,.sleeping=true,
   .wakeup_deadline_ms=105};
  sleeping_threads=&target;scheduler_stats.sleeping_threads=1;
  scheduler_stats.blocked_threads=1;uptime_ms=104;
  assert(!scheduler_global_tick());
  assert(target.state==THREAD_STATE_BLOCKED && target.sleeping && !target.queued);
  assert(scheduler_stats.sleeping_threads==1 && scheduler_stats.wakeups==0);
  /* A delayed scheduler notification observes elapsed time directly; it does
   * not need one delivered tick for each millisecond of the requested sleep. */
  uptime_ms=110;
  assert(scheduler_global_tick());
  assert(target.state==THREAD_STATE_READY && !target.sleeping && target.queued);
  assert(scheduler_stats.sleeping_threads==0 &&
         scheduler_stats.blocked_threads==0 && scheduler_stats.wakeups==1);
 } else for(unsigned success=0;success<2;success++) {
  reset(success);
  bool result;
  if(!strcmp(argv[1],"block"))result=scheduler_block();
  else if(!strcmp(argv[1],"sleep"))result=scheduler_sleep(5);
  else result=scheduler_terminate();
  assert(result==(bool)success && migrations==1 && !scheduler_lock && interrupts_enabled);
  /* CPU 0's unrelated running thread must never be changed by this caller. */
  assert(cpus[0].scheduler_current_thread==&other);
  assert(other.state==THREAD_STATE_RUNNING && other.running_cpu==0);
  assert(cpus[1].scheduler_current_thread==(success?&target:&caller));
  assert(caller.running_cpu==(success?THREAD_CPU_NONE:1));
  if(!strcmp(argv[1],"sleep"))
   assert(caller.wakeup_deadline_ms==105);
  assert(handoffs==success);
 }
#ifdef __linux__
 assert(syscall(SYS_arch_prctl,ARCH_SET_GS,original_gs)==0);
#endif
 puts("scheduler current-thread migration and rollback passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-scheduler-current-') as tmp:
    path = Path(tmp) / 'test.c'
    path.write_text(prefix + function('scheduler_cpu_local') + macros + ''.join(function(name) for name in (
        'scheduler_prepare_dispatch_locked', 'scheduler_block', 'scheduler_terminate', 'scheduler_sleep',
        'scheduler_current_thread', 'scheduler_current_stack_guard_fault',
        'scheduler_global_tick', 'thread_entry_trampoline')) + suffix)
    binary = Path(tmp) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    results = [subprocess.run([str(binary), case]).returncode
               for case in ('block', 'sleep', 'terminate', 'snapshot', 'entry',
                            'entry-completion', 'deadline')]
    if any(results):
        raise SystemExit(f'scheduler migration regressions failed: {results}')
