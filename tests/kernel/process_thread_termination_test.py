#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production scheduler group termination preflight and all-or-none commit."""
from pathlib import Path
import os
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
    raise ValueError(name)


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef size_t usize;
typedef u32 spinlock_t;
enum {THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
      THREAD_STATE_TERMINATED};
enum {THREAD_PRIORITY_HIGH, THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_BACKGROUND};
enum {THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
      THREAD_PROCESS_EXIT_REQUESTED, THREAD_PROCESS_HANDOFF_COMPLETE};
#define THREAD_CPU_NONE (~(u32)0)
typedef struct process process_t;
typedef struct kernel_thread kernel_thread_t;
struct kernel_thread {
    int state, effective_priority;
    u32 running_cpu, syscall_nesting;
    bool context_switch_pending;
    uintptr_t preempt_return_rip;
    bool syscall_active, sleeping, queued;
    u64 wakeup_deadline_ms;
    process_t *process;
    kernel_thread_t *process_next, *next, *previous;
    int process_state;
    void *waiting_mutex;
};
struct process {u32 live_thread_count;};
typedef struct {kernel_thread_t *scheduler_idle_thread;} cpu_local_t;
typedef struct {kernel_thread_t *head,*tail;u32 count;} thread_ready_queue_t;
typedef struct {u32 sleeping_threads,blocked_threads;} scheduler_stats_t;
static spinlock_t scheduler_lock;
static kernel_thread_t bootstrap_thread,*sleeping_threads;
static cpu_local_t cpus[2];
static thread_ready_queue_t ready_queues[3];
static scheduler_stats_t scheduler_stats;
static unsigned canceled,completed;
static u64 spin_lock_irqsave(spinlock_t *lock)
{assert(!*lock);*lock=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *lock,u64 flags)
{(void)flags;assert(*lock);*lock=0;}
static u32 cpu_count(void){return 2;}
static cpu_local_t *cpu_by_index(u32 index)
{return index<2?&cpus[index]:NULL;}
static bool thread_priority_valid(int priority)
{return priority>=THREAD_PRIORITY_HIGH &&
       priority<=THREAD_PRIORITY_BACKGROUND;}
static void mutex_cancel_waiter(kernel_thread_t *thread)
{assert(!scheduler_lock);canceled++;thread->waiting_mutex=NULL;}
static void process_scheduler_handoff_complete(kernel_thread_t *thread)
{assert(!scheduler_lock && thread->process);
 if(thread->process_state!=THREAD_PROCESS_HANDOFF_COMPLETE) {
   assert(thread->process->live_thread_count);
   thread->process->live_thread_count--;
   thread->process_state=THREAD_PROCESS_HANDOFF_COMPLETE;
 }
 completed++;}
'''

names = (
    ("kernel/src/scheduler.c", "sleeping_remove"),
    ("kernel/src/scheduler.c", "scheduler_remove_queued_locked"),
    ("kernel/src/scheduler.c", "scheduler_terminate_thread_group"),
)

suffix = r'''
int main(void)
{
    process_t process={.live_thread_count=2};
    kernel_thread_t ready={.state=THREAD_STATE_READY,.running_cpu=THREAD_CPU_NONE,
      .effective_priority=THREAD_PRIORITY_NORMAL,.queued=true,.process=&process,
      .process_state=THREAD_PROCESS_ATTACHED};
    kernel_thread_t blocked={.state=THREAD_STATE_BLOCKED,
      .running_cpu=THREAD_CPU_NONE,.process=&process,
      .process_state=THREAD_PROCESS_ATTACHED,.waiting_mutex=(void*)1};
    ready.process_next=&blocked;
    ready_queues[THREAD_PRIORITY_NORMAL]=
        (thread_ready_queue_t){&ready,&ready,1};
    scheduler_stats.blocked_threads=1;

    blocked.syscall_active=true;
    assert(!scheduler_terminate_thread_group(&ready,2));
    assert(ready.state==THREAD_STATE_READY && ready.queued &&
           ready_queues[THREAD_PRIORITY_NORMAL].count==1 &&
           blocked.state==THREAD_STATE_BLOCKED &&
           scheduler_stats.blocked_threads==1 && canceled==0 && completed==0 &&
           process.live_thread_count==2);

    blocked.syscall_active=false;blocked.context_switch_pending=true;
    assert(!scheduler_terminate_thread_group(&ready,2));
    assert(ready.queued && blocked.state==THREAD_STATE_BLOCKED &&
           process.live_thread_count==2);
    blocked.context_switch_pending=false;
    assert(!scheduler_terminate_thread_group(&ready,1)); /* incomplete list */
    assert(ready.queued && blocked.state==THREAD_STATE_BLOCKED);

    assert(scheduler_terminate_thread_group(&ready,2));
    assert(ready.state==THREAD_STATE_TERMINATED && !ready.queued &&
           !ready.next && !ready.previous &&
           ready_queues[THREAD_PRIORITY_NORMAL].count==0 &&
           !ready_queues[THREAD_PRIORITY_NORMAL].head &&
           blocked.state==THREAD_STATE_TERMINATED &&
           scheduler_stats.blocked_threads==0 && canceled==2 &&
           completed==2 && process.live_thread_count==0 &&
           ready.process_state==THREAD_PROCESS_HANDOFF_COMPLETE &&
           blocked.process_state==THREAD_PROCESS_HANDOFF_COMPLETE);

    /* An already completed member is idempotent and does not underflow the
     * process's live count when a later group request sees it. */
    assert(scheduler_terminate_thread_group(&ready,2));
    assert(process.live_thread_count==0 && completed==4 && canceled==4);

    kernel_thread_t first={.state=THREAD_STATE_READY,
      .running_cpu=THREAD_CPU_NONE,.process=&process,
      .process_state=THREAD_PROCESS_ATTACHED};
    kernel_thread_t second={.state=THREAD_STATE_READY,
      .running_cpu=0,.process=&process,
      .process_state=THREAD_PROCESS_ATTACHED};
    first.process_next=&second;process.live_thread_count=2;
    assert(!scheduler_terminate_thread_group(&first,2));
    assert(first.state==THREAD_STATE_READY &&
           second.state==THREAD_STATE_READY && process.live_thread_count==2);
    second.running_cpu=THREAD_CPU_NONE;second.preempt_return_rip=0x1234;
    assert(!scheduler_terminate_thread_group(&first,2));
    assert(first.state==THREAD_STATE_READY &&
           second.state==THREAD_STATE_READY && process.live_thread_count==2);

    puts("process thread termination: complete preflight, safe group commit, and handoff passed");
    return 0;
}
'''

code = prefix + "".join(function(path, name) for path, name in names) + suffix
with tempfile.TemporaryDirectory(prefix="mangrove-process-thread-terminate-") as directory:
    path = Path(directory) / "process_thread_termination.c"
    binary = Path(directory) / "process_thread_termination"
    path.write_text(code)
    subprocess.run([CC, "-std=gnu17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path), "-o",
                    str(binary)], check=True)
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    subprocess.run([str(binary)], check=True, env=env)
