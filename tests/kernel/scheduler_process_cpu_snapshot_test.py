#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production atomic process-CPU snapshot helper."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
source = (ROOT / "kernel/src/scheduler.c").read_text()
name = "scheduler_process_running_cpu_snapshot"
pattern = r"^u32 " + name + r"\s*\("
match = re.search(pattern, source, re.M)
if not match:
    raise SystemExit(f"missing production function: {name}")
start = source.index("{", match.end())
end, depth = start + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[match.start():end]

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define THREAD_CPU_NONE (~(u32)0)
typedef struct kernel_thread {
    u32 running_cpu;
    struct kernel_thread *process_next;
} kernel_thread_t;
static pthread_mutex_t scheduler_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 spin_lock_irqsave(pthread_mutex_t *lock)
{ assert(pthread_mutex_lock(lock) == 0); return 0; }
static void spin_unlock_irqrestore(pthread_mutex_t *lock, u64 flags)
{ (void)flags; assert(pthread_mutex_unlock(lock) == 0); }
''' + function + r'''
int main(void)
{
    kernel_thread_t a = { .running_cpu = THREAD_CPU_NONE };
    kernel_thread_t b = { .running_cpu = THREAD_CPU_NONE };
    kernel_thread_t c = { .running_cpu = THREAD_CPU_NONE };
    a.process_next = &b; b.process_next = &c;
    assert(scheduler_process_running_cpu_snapshot(&a) == THREAD_CPU_NONE);
    a.running_cpu = 0;
    assert(scheduler_process_running_cpu_snapshot(&a) == 0);
    b.running_cpu = 0;
    assert(scheduler_process_running_cpu_snapshot(&a) == 0);
    b.running_cpu = 1;
    assert(scheduler_process_running_cpu_snapshot(&a) == ~(u32)1U);
    b.running_cpu = THREAD_CPU_NONE;
    c.running_cpu = 1;
    assert(scheduler_process_running_cpu_snapshot(&a) == ~(u32)1U);
    c.running_cpu = 0;
    assert(scheduler_process_running_cpu_snapshot(&a) == 0);
    puts("scheduler process CPU snapshot: none, single CPU, and simultaneous distinct-CPU states passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="mangrove-scheduler-cpu-") as directory:
    c_path = Path(directory) / "snapshot.c"
    binary = Path(directory) / "snapshot"
    c_path.write_text(fixture)
    subprocess.run([CC, "-std=c17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-pthread", str(c_path),
                    "-o", str(binary)], check=True)
    import os
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    subprocess.run([str(binary)], check=True, env=env)
