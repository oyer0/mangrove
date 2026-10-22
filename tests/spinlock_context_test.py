#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production spinlock depth and scheduler preemption guards."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
TEMP_ROOT = ROOT / "build/stage4.5-spinlock-tmp"
TEMP_ROOT.mkdir(parents=True, exist_ok=True)


def function(path, name):
    source = (ROOT / path).read_text()
    pattern = re.compile(
        r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + re.escape(name) + r"\s*\(",
        re.M,
    )
    for match in pattern.finditer(source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(f"definition not found: {name}")


prelude = r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef struct { volatile u32 value; } spinlock_t;
typedef struct cpu_local {
    volatile u32 spinlock_depth;
    struct kernel_thread *scheduler_current_thread;
    struct kernel_thread *scheduler_idle_thread;
    volatile bool scheduler_preemption_pending;
    volatile bool scheduler_context_switching;
} cpu_local_t;
typedef struct kernel_thread {
    unsigned state;
    bool queued, syscall_active, preempt_from_user, mutex_wake_pending;
    bool event_wake_pending;
    bool sleeping;
    u32 running_cpu;
    uintptr_t saved_stack_pointer, preempt_return_rip, preempt_return_rsp;
    u64 preempt_return_rflags, preempt_return_cs, preempt_return_ss;
    u64 wakeup_tick;
    struct kernel_thread *next;
} kernel_thread_t;
typedef struct cpu_registers {
    u64 rip, rflags, cs, ss;
    uintptr_t interrupted_rsp, interrupted_ss;
    bool from_user;
} cpu_registers_t;
enum {
    THREAD_STATE_READY = 0U,
    THREAD_STATE_RUNNING = 1U,
    THREAD_STATE_BLOCKED = 2U,
    THREAD_STATE_TERMINATED = 3U,
    SCHEDULER_DISPATCH_BLOCK = 1U,
    THREAD_CPU_NONE = UINT32_MAX,
};
typedef struct {
    u64 blocks, blocked_threads, timer_preemptions, sleeping_threads;
    u64 voluntary_yields;
}
    scheduler_stats_t;
typedef struct { volatile u32 value; } scheduler_lock_t;
static _Thread_local cpu_local_t local_cpu;
static scheduler_lock_t scheduler_lock;
static scheduler_stats_t scheduler_stats;
static kernel_thread_t *sleeping_threads;
static u64 scheduler_tick_count;
static _Atomic u32 scheduler_lock_calls;
static bool test_interrupts_enabled = true;
static u64 spin_irq_save(void);
static void spin_irq_restore(u64 flags);
static cpu_local_t *scheduler_cpu_local(void) { return &local_cpu; }
#define current_thread (scheduler_cpu_local()->scheduler_current_thread)
#define idle_thread (scheduler_cpu_local()->scheduler_idle_thread)
#define preemption_pending (scheduler_cpu_local()->scheduler_preemption_pending)
#define scheduler_context_switch_in_progress (scheduler_cpu_local()->scheduler_context_switching)
static cpu_local_t *cpu_current(void) { return &local_cpu; }
static u64 spin_irq_save(void) {
    bool enabled = test_interrupts_enabled;
    test_interrupts_enabled = false;
    return enabled ? (1ULL << 9) : 0U;
}
static void spin_irq_restore(u64 flags) {
    test_interrupts_enabled = (flags & (1ULL << 9)) != 0U;
}
static u32 cpu_current_index(void) { return 0; }
static u64 spin_lock_irqsave(scheduler_lock_t *lock) {
    (void)lock; atomic_fetch_add(&scheduler_lock_calls, 1U); return 0;
}
static void spin_unlock_irqrestore(scheduler_lock_t *lock, u64 flags) {
    (void)lock; (void)flags;
}
static bool scheduler_prepare_dispatch_locked(unsigned action,
        kernel_thread_t **outgoing, kernel_thread_t **target, bool *handoff) {
    (void)action; (void)outgoing; (void)target; (void)handoff; return false;
}
static bool scheduler_handoff(kernel_thread_t *outgoing,
        kernel_thread_t *target, u64 flags) {
    (void)outgoing; (void)target; (void)flags; return false;
}
static void sleeping_remove(kernel_thread_t *thread) { (void)thread; }
static void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }
static void panic(const char *reason) { fprintf(stderr, "%s\n", reason); abort(); }
static bool thread_saved_stack_valid(const kernel_thread_t *thread) {
    return thread && thread->saved_stack_pointer != 0;
}
static bool cpu_registers_has_privilege_stack(const cpu_registers_t *regs) {
    return regs && regs->from_user;
}
static uintptr_t cpu_registers_interrupted_rsp(const cpu_registers_t *regs) {
    return regs->interrupted_rsp;
}
static u64 cpu_registers_interrupted_ss(const cpu_registers_t *regs) {
    return regs->interrupted_ss;
}
static void thread_interrupt_return_trampoline(void) { }
static bool scheduler_reschedule(void) { return true; }
"""

spinlock_code = "".join(
    function("kernel/src/spinlock.c", name)
    for name in (
        "spinlock_depth_enter", "spinlock_depth_leave",
        "spinlock_depth_current", "spinlock_init", "spin_try_lock",
        "spin_try_lock_irqsave", "spin_lock", "spin_unlock",
    )
)
scheduler_code = function(
    "kernel/src/scheduler.c", "scheduler_prepare_preemption"
)
block_code = function("kernel/src/scheduler.c", "scheduler_block")
sleep_code = function("kernel/src/scheduler.c", "scheduler_sleep")
pending_wake_code = function(
    "kernel/src/scheduler.c", "scheduler_consume_pending_wake_locked"
)

test = r"""
static spinlock_t shared;
static _Atomic bool contender_acquired;
static _Atomic bool contender_started;
static void *contender(void *unused) {
    (void)unused;
    atomic_store_explicit(&contender_started, true, memory_order_release);
    spin_lock(&shared);
    assert(spinlock_depth_current() == 1U);
    atomic_store_explicit(&contender_acquired, true, memory_order_release);
    spin_unlock(&shared);
    assert(spinlock_depth_current() == 0U);
    return NULL;
}

int main(void) {
    spinlock_t nested;
    u64 irq_flags = 0U;
    spinlock_init(&shared);
    spinlock_init(&nested);
    assert(spinlock_depth_current() == 0U);
    assert(spin_try_lock(&shared));
    assert(spinlock_depth_current() == 1U);
    assert(!spin_try_lock(&shared));
    assert(spinlock_depth_current() == 1U);
    spin_lock(&nested);
    assert(spinlock_depth_current() == 2U);
    spin_unlock(&nested);
    assert(spinlock_depth_current() == 1U);
    spin_unlock(&shared);
    assert(spinlock_depth_current() == 0U);

    test_interrupts_enabled = true;
    assert(spin_try_lock_irqsave(&shared, &irq_flags));
    assert((irq_flags & (1U << 9)) && !test_interrupts_enabled &&
           spinlock_depth_current() == 1U);
    spin_unlock(&shared);
    spin_irq_restore(irq_flags);
    assert(test_interrupts_enabled && spinlock_depth_current() == 0U);
    spin_lock(&shared);
    test_interrupts_enabled = true;
    assert(!spin_try_lock_irqsave(&shared, &irq_flags));
    assert(test_interrupts_enabled && spinlock_depth_current() == 1U);
    spin_unlock(&shared);

    spin_lock(&shared);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, contender, NULL) == 0);
    while (!atomic_load_explicit(&contender_started, memory_order_acquire))
        sched_yield();
    assert(!spin_try_lock(&shared));
    assert(spinlock_depth_current() == 1U);
    spin_unlock(&shared);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load_explicit(&contender_acquired, memory_order_acquire));
    assert(spinlock_depth_current() == 0U);

    kernel_thread_t thread = {
        .state = THREAD_STATE_RUNNING,
        .saved_stack_pointer = 0x1000U,
    };
    thread.mutex_wake_pending = true;
    thread.event_wake_pending = true;
    assert(scheduler_consume_pending_wake_locked(&thread, false));
    assert(thread.mutex_wake_pending && !thread.event_wake_pending);
    assert(scheduler_consume_pending_wake_locked(&thread, true));
    assert(!thread.mutex_wake_pending && !thread.event_wake_pending);
    cpu_registers_t regs = {
        .rip = 0x2000U,
        .rflags = 1U << 9,
        .cs = 0x08U,
        .ss = 0x10U,
    };
    local_cpu.scheduler_current_thread = &thread;
    local_cpu.scheduler_preemption_pending = true;
    local_cpu.spinlock_depth = 1U;
    assert(scheduler_sleep(0U));
    assert(!scheduler_sleep(1U));
    assert(!scheduler_block());
    assert(atomic_load(&scheduler_lock_calls) == 0U &&
           thread.state == THREAD_STATE_RUNNING && !thread.queued &&
           scheduler_stats.blocks == 0U && scheduler_stats.blocked_threads == 0U);
    assert(!scheduler_prepare_preemption(&regs));
    assert(local_cpu.scheduler_preemption_pending && regs.rip == 0x2000U &&
           scheduler_stats.timer_preemptions == 0U);

    /* The unheld control must take the existing path. This fails against the
     * old scheduler, which switches while the synthetic spinlock is held. */
    local_cpu.spinlock_depth = 0U;
    assert(scheduler_prepare_preemption(&regs));
    assert(!local_cpu.scheduler_preemption_pending &&
           regs.rip == (u64)(uintptr_t)&thread_interrupt_return_trampoline &&
           !(regs.rflags & (1U << 9)) &&
           scheduler_stats.timer_preemptions == 1U);
    puts("spinlock context: IRQ-safe try-lock, depth, contention, blocking rejection, and deferred preemption passed");
}
"""

with tempfile.TemporaryDirectory(prefix="mangrove-spinlock-context-",
                                 dir=TEMP_ROOT) as temp:
    source = Path(temp) / "test.c"
    source.write_text(prelude + spinlock_code + pending_wake_code +
                      block_code + sleep_code +
                      scheduler_code + test)
    executable = Path(temp) / "test"
    subprocess.run(
        [CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-fsanitize=address,undefined", str(source), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True, timeout=30)
