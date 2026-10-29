#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production terminal foreground-process interrupt selection."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/process.c').read_text()


def function(name):
    for match in re.finditer(
            r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(',
            source, re.M):
        brace = source.index('{', match.end())
        if ';' in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    raise ValueError(name)


prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef size_t usize;
enum { PROCESS_STATE_ACTIVE = 0, PROCESS_STATE_EXITING,
       PROCESS_STATE_TERMINATED };
typedef struct process process_t;
typedef struct { u32 references; } kernel_object_t;
typedef struct kernel_thread {
    unsigned wakes;
    struct kernel_thread *process_next;
} kernel_thread_t;
struct process {
    u64 session_id;
    int state;
    bool session_shell, launch_ready;
    volatile bool interrupt_requested, exit_requested;
    process_t *waiting_child, *parent, *next_all;
    kernel_thread_t *main_thread, *threads;
    u32 thread_count;
    u32 thread_list_readers;
    bool thread_reap_in_progress, exit_cleanup_complete;
    bool worker_reap_pending;
    kernel_object_t object;
};
static int process_registry_lock;
static process_t *all_processes;
static unsigned cleanup_wakes;
static u64 spin_lock_irqsave(int *lock) { assert(!*lock); *lock = 1; return 1; }
static void spin_unlock_irqrestore(int *lock, u64 flags)
{ (void)flags; assert(*lock); *lock = 0; }
static bool object_reference(kernel_object_t *object)
{ assert(process_registry_lock); object->references++; return true; }
static void object_release(kernel_object_t *object)
{ assert(!process_registry_lock && object->references > 1U); object->references--; }
static void panic(const char *message)
{ (void)message; assert(!"unexpected process panic"); }
static bool process_finish_termination(process_t *process)
{ (void)process; return false; }
static void process_worker_reclaim_pending(process_t *process)
{ (void)process; assert(!process_registry_lock); }
static void process_cleanup_worker_wake(process_t *process)
{ assert(process && !process_registry_lock); cleanup_wakes++; }
static bool scheduler_wake_waiter(kernel_thread_t *thread)
{ assert(thread); thread->wakes++; return true; }
'''

body = (function('process_thread_list_read_acquire_locked') +
        function('process_thread_list_read_release') +
        function('process_take_interrupt') +
        function('process_interrupt_foreground_session'))
postlude = r'''
static void clear_fixture(void)
{
    all_processes = NULL;
    process_registry_lock = 0;
}
int main(void)
{
    process_t shell = {.state=PROCESS_STATE_ACTIVE, .session_shell=true,
                       .session_id=4, .launch_ready=true, .object={1}};
    process_t wrapper = {.state=PROCESS_STATE_ACTIVE, .session_id=4,
                         .parent=&shell, .launch_ready=true, .object={1}};
    process_t child = {.state=PROCESS_STATE_ACTIVE, .session_id=4,
                       .parent=&wrapper, .launch_ready=true, .object={1}};
    process_t unrelated = {.state=PROCESS_STATE_ACTIVE, .session_id=9,
                           .launch_ready=true, .object={1}};
    kernel_thread_t wrapper_thread={0}, child_thread={0},
                    child_worker_thread={0};

    shell.waiting_child = &wrapper;
    shell.next_all = &wrapper;
    wrapper.waiting_child = &child;
    wrapper.main_thread = &wrapper_thread;
    wrapper.next_all = &child;
    child.main_thread = &child_thread;
    child.threads = &child_thread;
    child.thread_count = 2;
    child_thread.process_next = &child_worker_thread;
    child_worker_thread.process_next = NULL;
    child.next_all = &unrelated;
    all_processes = &shell;

    assert(process_interrupt_foreground_session());
    assert(!shell.interrupt_requested && !wrapper.interrupt_requested);
    assert(child.interrupt_requested && child_thread.wakes == 1U &&
           child_worker_thread.wakes == 1U);
    assert(child.object.references == 1U);
    assert(process_take_interrupt(&child));
    assert(!process_take_interrupt(&child));
    child.exit_requested = true;
    assert(process_take_interrupt(&child));
    child.exit_requested = false;

    /* An ambiguous console session is rejected rather than interrupting a
       process belonging to an arbitrary second session. */
    process_t other_shell = {.state=PROCESS_STATE_ACTIVE, .session_shell=true,
                             .session_id=9, .launch_ready=true, .object={1}};
    process_t other_child = {.state=PROCESS_STATE_ACTIVE, .session_id=9,
                             .parent=&other_shell, .main_thread=&wrapper_thread,
                             .launch_ready=true, .object={1}};
    other_shell.waiting_child = &other_child;
    unrelated.next_all = &other_shell;
    other_shell.next_all = &other_child;
    assert(!process_interrupt_foreground_session());
    assert(!child.interrupt_requested && !other_child.interrupt_requested);

    clear_fixture();
    shell.next_all = &wrapper;
    wrapper.next_all = &child;
    child.next_all = NULL;
    unrelated.next_all = NULL;
    wrapper.session_id = 8;
    all_processes = &shell;
    assert(!process_interrupt_foreground_session());
    assert(!child.interrupt_requested);

    puts("foreground interruption targets the deepest active child only");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-process-interrupt-') as temp:
    cfile = Path(temp) / 'process_interrupt.c'
    binary = Path(temp) / 'process_interrupt'
    cfile.write_text(prelude + body + postlude)
    compiler = sys.argv[1] if len(sys.argv) > 1 else 'cc'
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(cfile), '-o', str(binary)],
                   check=True)
    environment = os.environ.copy()
    environment.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    subprocess.run([str(binary)], check=True, env=environment)
