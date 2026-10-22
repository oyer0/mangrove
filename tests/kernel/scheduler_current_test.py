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
enum { THREAD_PRIORITY_BACKGROUND=2 };
typedef enum {SCHEDULER_DISPATCH_REQUEUE, SCHEDULER_DISPATCH_BLOCK,
              SCHEDULER_DISPATCH_TERMINATE} scheduler_dispatch_action_t;
#define THREAD_CPU_NONE ((u32)-1)
#define scheduler_trace(...) ((void)0)
typedef struct kernel_thread {
 unsigned state, running_cpu; bool queued, sleeping, entry_started;
 void *process; const char *name; void (*entry)(void *); void *entry_argument;
 uintptr_t saved_stack_pointer; u64 wakeup_tick; struct kernel_thread *next;
} kernel_thread_t;
typedef struct {
 unsigned index; bool bsp;
 kernel_thread_t *scheduler_current_thread, *scheduler_idle_thread;
 unsigned scheduler_context_switching; u64 kernel_thread_runs;
} cpu_local_t;
static cpu_local_t cpus[2];
static kernel_thread_t caller, other, idle[2], target, bootstrap_thread;
static kernel_thread_t *sleeping_threads, *next_target;
static struct {unsigned blocks, blocked_threads, sleeping_threads, dispatches[3];} scheduler_stats;
static unsigned active_cpu, scheduler_lock, migrations, handoffs;
static u64 scheduler_tick_count;
static bool interrupts_enabled, migrate_armed;
static jmp_buf entry_return;
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
static void panic(const char *s) {(void)s;assert(!"unexpected scheduler panic");}
static bool scheduler_handoff(kernel_thread_t *out,kernel_thread_t *in,u64 flags) {
 assert(out==&caller && in==&target && active_cpu==1);
 handoffs++; spin_unlock_irqrestore(&scheduler_lock,flags);return true;
}
'''
macros = r'''
#define current_thread (scheduler_cpu_local()->scheduler_current_thread)
#define idle_thread (scheduler_cpu_local()->scheduler_idle_thread)
#define scheduler_context_switch_in_progress (scheduler_cpu_local()->scheduler_context_switching)
'''
suffix = r'''
static void reset(bool success) {
 memset(cpus,0,sizeof(cpus));memset(&scheduler_stats,0,sizeof(scheduler_stats));
 caller=(kernel_thread_t){.state=THREAD_STATE_RUNNING,.running_cpu=0,.saved_stack_pointer=0x1000};
 other=(kernel_thread_t){.state=THREAD_STATE_RUNNING,.running_cpu=0,.saved_stack_pointer=0x2000};
 target=(kernel_thread_t){.state=THREAD_STATE_READY,.running_cpu=THREAD_CPU_NONE,.saved_stack_pointer=0x3000};
 for(unsigned i=0;i<2;i++){cpus[i].index=i;cpus[i].scheduler_idle_thread=&idle[i];}
 cpus[0].scheduler_current_thread=&caller;
 active_cpu=0;scheduler_lock=0;migrations=0;handoffs=0;
 sleeping_threads=NULL;next_target=success?&target:NULL;
 interrupts_enabled=true;migrate_armed=true;set_gs();
}
static void entered(void *argument) {
 assert(argument==&caller && caller.entry_started && !other.entry_started);
 assert(other.state==THREAD_STATE_RUNNING && other.running_cpu==0);
 assert(!scheduler_lock && interrupts_enabled);
 longjmp(entry_return,1);
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
#else
  puts("GS snapshot runtime check requires Linux x86-64; target build checks ABI");
#endif
 } else if(!strcmp(argv[1],"entry")) {
  reset(false);caller.entry=entered;caller.entry_argument=&caller;
  other.entry=entered;other.entry_argument=&other;
  caller.name="caller";other.name="other";
  if(!setjmp(entry_return))thread_entry_trampoline();
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
        'scheduler_current_thread', 'thread_entry_trampoline')) + suffix)
    binary = Path(tmp) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    results = [subprocess.run([str(binary), case]).returncode
               for case in ('block', 'sleep', 'terminate', 'snapshot', 'entry')]
    if any(results):
        raise SystemExit(f'scheduler migration regressions failed: {results}')
