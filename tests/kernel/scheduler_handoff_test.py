#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production scheduler handoff functions with deterministic CPU interleavings.
Hardware/context assembly is verified separately by native build and QEMU.
An optional repository argument tests the same safety assertions on old code.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/scheduler.c').read_text()


def function(name):
    for m in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', source, re.M):
        b = source.index('{', m.end())
        if ';' in source[m.end():b]:
            continue
        end, depth = b + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[m.start():end] + '\n'
    raise ValueError(name)

pre = r'''
#define KERNEL_STACK_DIAGNOSTICS 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t u64; typedef unsigned u32; typedef unsigned thread_priority_t;
#define MSR_IA32_FS_BASE 0xc0000100U
enum {THREAD_PRIORITY_HIGH,THREAD_PRIORITY_NORMAL,THREAD_PRIORITY_BACKGROUND};
enum {THREAD_STATE_READY,THREAD_STATE_RUNNING,THREAD_STATE_BLOCKED,THREAD_STATE_TERMINATED};
enum {KERNEL_STACK_USAGE_BOOTSTRAP};
#define THREAD_CPU_NONE ((u32)-1)
typedef struct {bool active;} kernel_stack_t;
typedef struct {unsigned references;} kernel_object_t;
typedef struct process {kernel_object_t object;unsigned handoffs;} process_t;
typedef struct kernel_thread {
 struct kernel_thread *next; uintptr_t saved_stack_pointer;
 unsigned state,running_cpu,effective_priority,base_priority,last_selected_priority,ready_wait_ticks;
 bool queued,context_switch_pending,saved_context_valid,wakeup_boosted,last_selection_was_wakeup_boost;
 bool stack_external; uintptr_t kernel_stack_base; size_t kernel_stack_size;
 kernel_stack_t kernel_stack;
 process_t *process;
 void *worker_record;
 unsigned fpu_state;
 uintptr_t user_fs_base;
} kernel_thread_t;
typedef struct {kernel_thread_t *scheduler_idle_thread;
 process_t *scheduler_post_handoff_process;} cpu_local_t;
static cpu_local_t cpu; static kernel_thread_t bootstrap_thread, *current_thread, *idle_thread;
typedef struct {kernel_thread_t *head;} thread_ready_queue_t;
static thread_ready_queue_t ready_queues[3];
static struct {unsigned dispatches[3];} scheduler_stats;
static unsigned scheduler_lock, scheduler_context_switch_in_progress;
static unsigned sampled_kernel_stacks;
static uintptr_t fake_fs_base;
static u64 rdmsr(u32 msr){assert(msr==MSR_IA32_FS_BASE);return fake_fs_base;}
static void wrmsr(u32 msr,u64 value)
{assert(msr==MSR_IA32_FS_BASE);fake_fs_base=(uintptr_t)value;}
static void kernel_stack_sample_rsp(kernel_stack_t *stack, uintptr_t rsp)
{assert(stack && stack->active && rsp); sampled_kernel_stacks++;}
static void kernel_stack_usage_sample(unsigned kind, uintptr_t top, uintptr_t rsp)
{assert(kind == KERNEL_STACK_USAGE_BOOTSTRAP && top && rsp); sampled_kernel_stacks++;}
static u64 spin_lock_irqsave(unsigned *p){assert(!*p);*p=1;return 0;}
static void spin_unlock_irqrestore(unsigned *p,u64 f){(void)f;assert(*p);*p=0;}
static void panic(const char *s){puts(s);abort();}
static bool object_reference(kernel_object_t *object)
{if(!object||object->references==~0U)return false;object->references++;return true;}
static void process_scheduler_handoff_complete(kernel_thread_t *thread)
{assert(thread && thread->process && !scheduler_lock &&
        !scheduler_context_switch_in_progress);thread->process->handoffs++;}
static cpu_local_t *scheduler_cpu_local(void){return &cpu;}
static void fpu_state_save(unsigned *s){(void)s;}
static void fpu_state_restore(unsigned *s){(void)s;}
static bool thread_saved_stack_valid(const kernel_thread_t *t){return t->saved_stack_pointer!=0;}
static bool scheduler_validate_locked(void){return true;}
static bool scheduler_thread_eligible(const kernel_thread_t *t){(void)t;return true;}
static unsigned cpu_current_index(void){return 0;}
static bool scheduler_enqueue_locked(kernel_thread_t *t){assert(!t->queued);t->queued=true;t->next=ready_queues[0].head;ready_queues[0].head=t;return true;}
static void scheduler_remove_queued_locked(kernel_thread_t *t){assert(ready_queues[0].head==t);ready_queues[0].head=t->next;t->queued=false;}
'''
post = r'''
int main(void){
 process_t process={0};
 kernel_thread_t outgoing={.state=THREAD_STATE_READY,.running_cpu=0,.context_switch_pending=true,.saved_stack_pointer=0x1000,.stack_external=true,.kernel_stack_base=0x4000,.kernel_stack_size=0x4000};
 outgoing.process=&process;
 kernel_thread_t incoming={.state=THREAD_STATE_RUNNING,.running_cpu=0,.saved_context_valid=true,.saved_stack_pointer=0x2000};
 outgoing.user_fs_base=0;
 incoming.user_fs_base=0x56780000;
 fake_fs_base=0x12340000;
 current_thread=&incoming;scheduler_context_switch_in_progress=1;
 scheduler_context_switch_saved(&outgoing.saved_stack_pointer,incoming.saved_stack_pointer);
 assert(outgoing.user_fs_base==0x12340000);
 assert(sampled_kernel_stacks==1);
 /* CPU 1 tries selection before CPU 0 has left outgoing's stack. */
 assert(scheduler_select_next_locked()==NULL);
 assert(outgoing.running_cpu==0 && !outgoing.queued && outgoing.context_switch_pending);
 assert(scheduler_context_switch_in_progress==1 && !incoming.saved_context_valid);
 scheduler_context_switch_complete(&outgoing.saved_stack_pointer);
 assert(fake_fs_base==incoming.user_fs_base);
 assert(!outgoing.context_switch_pending && outgoing.running_cpu==THREAD_CPU_NONE);
 assert(scheduler_context_switch_in_progress==0);
 assert(scheduler_select_next_locked()==&outgoing);
 /* A wakeup may queue a BLOCKED outgoing context before handoff completes. */
 outgoing.state=THREAD_STATE_READY;outgoing.running_cpu=THREAD_CPU_NONE;
 outgoing.context_switch_pending=true;outgoing.saved_context_valid=true;
 scheduler_enqueue_locked(&outgoing);
 assert(scheduler_select_next_locked()==NULL);
 scheduler_context_switch_complete(&outgoing.saved_stack_pointer);
 assert(scheduler_select_next_locked()==&outgoing);
 /* Completion releases blocked/terminated stacks without scheduling them. */
 for(unsigned state=THREAD_STATE_BLOCKED;state<=THREAD_STATE_TERMINATED;state++){
  outgoing.state=state;outgoing.running_cpu=0;outgoing.context_switch_pending=true;
  scheduler_context_switch_complete(&outgoing.saved_stack_pointer);
  assert(!outgoing.queued && outgoing.running_cpu==THREAD_CPU_NONE && !outgoing.context_switch_pending);
 }
 /* Terminating any process-owned thread stages its process for post-RFLAGS
  * cleanup, even when it is the initial thread rather than a worker. */
 assert(cpu.scheduler_post_handoff_process==&process &&
        process.object.references==1);
 assert(process.handoffs==1);
 kernel_thread_t guarded={.state=THREAD_STATE_READY,.running_cpu=0,
   .saved_stack_pointer=0x3000,.kernel_stack={.active=true}};
 incoming.saved_stack_pointer=0x3000;current_thread=&incoming;
 scheduler_context_switch_saved(&guarded.saved_stack_pointer,
                                incoming.saved_stack_pointer);
 assert(sampled_kernel_stacks==2);
 puts("scheduler handoff: production publication/selection interleavings passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-scheduler-') as d:
    c = Path(d) / 'test.c'
    c.write_text(pre + ''.join(function(n) for n in ('thread_context_ready', 'scheduler_context_switch_saved', 'scheduler_context_switch_complete', 'scheduler_select_next_locked')) + post)
    exe = Path(d) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
