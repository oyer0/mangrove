/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <scheduler.h>
#include <preemption_count.h>
#include <cpu.h>
#include <idt.h>
#include <heap.h>
#include <kernel_stack.h>
#include <string.h>
#include <kprint.h>
#include <panic.h>
#include <process.h>
#include <vmm.h>
#include <gdt.h>
#include <smp.h>
#include <timer.h>
#include <terminal.h>
#include <spinlock.h>
#include <mutex.h>
#include <fpu.h>
#include <msr.h>


#ifndef NULL
#define NULL ((void *)0)
#endif

extern char __stack_bottom[];
extern char __stack_top[];
extern char __stack_guard_bottom[];

static kernel_thread_t bootstrap_thread;
static u64 next_thread_id = 2;
static u64 scheduler_tick_count;
static kernel_thread_t *sleeping_threads;
static scheduler_stats_t scheduler_stats;
static spinlock_t scheduler_lock;

#define THREAD_CPU_NONE (~(u32)0)

/* These names retain the scheduler's existing local vocabulary while making
 * their storage explicitly CPU-local.  Every caller resolves through the
 * executing CPU's GS-backed record. */
static cpu_local_t *scheduler_cpu_local(void)
{
    return cpu_current();
}

#define current_thread \
    (scheduler_cpu_local()->scheduler_current_thread)
#define idle_thread \
    (scheduler_cpu_local()->scheduler_idle_thread)
#define preemption_pending \
    (scheduler_cpu_local()->scheduler_preemption_pending)
#define scheduler_context_switch_in_progress \
    (scheduler_cpu_local()->scheduler_context_switching)

typedef struct {
    kernel_thread_t *head;
    kernel_thread_t *tail;
    u32 count;
} thread_ready_queue_t;

/* Three FIFO queues are ordered by effective priority.  Sleeping threads are
 * kept separately and re-enter a queue only when their wake tick arrives.
 * base_priority is immutable; effective_priority carries one-shot wakeup or
 * starvation rescue boosts and is restored when a thread is selected. */
static thread_ready_queue_t ready_queues[3];


extern void thread_context_switch(uintptr_t *outgoing_stack_pointer,
                                  uintptr_t incoming_stack_pointer,
                                  u64 saved_flags);
extern void thread_context_enter(uintptr_t incoming_stack_pointer);
extern void thread_interrupt_return_trampoline(void);

typedef enum {
    SCHEDULER_DISPATCH_REQUEUE = 0,
    SCHEDULER_DISPATCH_BLOCK,
    SCHEDULER_DISPATCH_TERMINATE,
} scheduler_dispatch_action_t;

static bool scheduler_dispatch(scheduler_dispatch_action_t action);
static bool scheduler_prepare_dispatch_locked(
    scheduler_dispatch_action_t action, kernel_thread_t **outgoing,
    kernel_thread_t **target, bool *handoff);
static bool scheduler_enqueue_locked(kernel_thread_t *thread);

/* A cooperative scheduler context must carry ordinary kernel flags.  The
 * arithmetic/status bits are intentionally preserved by context_switch, but
 * firmware/debug/virtualization control bits must never be imported into a
 * kernel thread's saved frame. */
#define SCHEDULER_UNSAFE_CONTEXT_FLAGS \
    ((1ULL << 8)  | /* TF */ \
     (1ULL << 10) | /* DF */ \
     (3ULL << 12) | /* IOPL */ \
     (1ULL << 14) | /* NT */ \
     (1ULL << 17) | /* VM */ \
     (1ULL << 18) | /* AC */ \
     (3ULL << 19))  /* VIF/VIP */

/* Diagnostic provenance checks were removed after the scheduler context
 * lifecycle fix was validated.  Keep this mask as a permanent invariant. */

static bool scheduler_interrupts_enabled(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

static uintptr_t thread_kernel_stack_top(const kernel_thread_t *thread)
{
    uintptr_t top;

    if (!thread || !thread->kernel_stack_base || !thread->kernel_stack_size) {
        return 0;
    }
    top = thread->kernel_stack_base + thread->kernel_stack_size;
    return top > thread->kernel_stack_base ? top : 0;
}

static void scheduler_activate_thread_context(kernel_thread_t *thread)
{
    page_table_t *address_space = vmm_get_kernel_pml4();
    uintptr_t stack_top = thread_kernel_stack_top(thread);

    if (thread && thread->process && thread->process->address_space) {
        address_space = thread->process->address_space;
    }
    /* RSP0 is CPU state, not process memory state.  It must follow the
     * scheduled thread so an IRQ arriving from Ring 3 cannot overwrite a
     * different thread's suspended syscall continuation. */
    gdt_set_kernel_stack(stack_top);
    if (!vmm_switch_address_space(address_space))
        panic("scheduler: selected address space is unavailable");
}

static void scheduler_update_runnable_peak(void)
{
    u64 runnable = ready_queues[THREAD_PRIORITY_HIGH].count +
        ready_queues[THREAD_PRIORITY_NORMAL].count +
        ready_queues[THREAD_PRIORITY_BACKGROUND].count;

    if (current_thread && current_thread != idle_thread &&
        current_thread->state == THREAD_STATE_RUNNING) {
        runnable++;
    }
    if (runnable > scheduler_stats.peak_runnable_threads) {
        scheduler_stats.peak_runnable_threads = runnable;
    }
}

/*
 * Phase 13.3 context-switch ABI:
 *
 *     pushfq
 *     push r15, r14, r13, r12, rbx, rbp
 *     save/restore RSP
 *     pop rbp, rbx, r12, r13, r14, r15
 *     popfq
 *     ret
 *
 * A prepared thread stack therefore contains (from low to high addresses)
 * rbp, rbx, r12, r13, r14, r15, RFLAGS, and the trampoline return address.
 */
static void thread_entry_trampoline(void)
{
#if defined(KERNEL_STACK_DIAGNOSTICS)
    uintptr_t stack_pointer;
#endif
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    kernel_thread_t *thread = current_thread;
    cpu_local_t *cpu = scheduler_cpu_local();
    bool first_kernel_run = false;
    u32 cpu_index = cpu ? cpu->index : THREAD_CPU_NONE;

    if (thread)
        thread->entry_started = true;
    if (thread && !thread->process && cpu)
        first_kernel_run = __atomic_add_fetch(&cpu->kernel_thread_runs, 1,
                                              __ATOMIC_RELAXED) == 1;
    spin_unlock_irqrestore(&scheduler_lock, flags);

#if defined(KERNEL_STACK_DIAGNOSTICS)
    __asm__ volatile("movq %%rsp, %0" : "=r"(stack_pointer));
    if (thread && thread->kernel_stack.active)
        kernel_stack_sample_rsp(&thread->kernel_stack, stack_pointer);
#endif

    if (first_kernel_run) {
        KERNEL_BOOT_DEBUG_LOG("[SCHED] kernel thread '%s' first ran on CPU %u\n",
                              thread->name, cpu_index);
    }
    if (thread && thread->entry) {
        thread->entry(thread->entry_argument);
    }

    /* A process's initial entry requests process-wide exit; a future
     * process-owned worker entry retires only itself. Process lifecycle state
     * is published before the scheduler switches away from this stack. */
    if (thread && thread->process)
        process_thread_entry_completed(thread,
            thread->process_initial ? -1 : 0);

    /* Termination uses the same dispatch path as yield and future preemption. */
    if (thread && scheduler_terminate()) {
        return;
    }

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void idle_thread_entry(void *argument)
{
    (void)argument;
    {
        kernel_thread_t *thread = scheduler_current_thread();
#if defined(KERNEL_STACK_DIAGNOSTICS)
        uintptr_t stack_pointer;
        __asm__ volatile("movq %%rsp, %0" : "=r"(stack_pointer));
        if (thread && thread->kernel_stack.active)
            kernel_stack_sample_rsp(&thread->kernel_stack, stack_pointer);
#endif
        if (thread && !cpu_current()->bsp)
            smp_ap_idle_entered(cpu_current_index());
    }
    for (;;) {
        cpu_local_t *cpu = scheduler_cpu_local();
        if (cpu && cpu->bsp)
            terminal_cursor_blink_poll();
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

static bool thread_saved_stack_valid(const kernel_thread_t *thread)
{
    uintptr_t stack_end;

    if (!thread) {
        return false;
    }

    /* Before its first outgoing switch, bootstrap has a live external stack
     * but no resumable cooperative frame.  It may be the current outgoing
     * thread, never an incoming target. */
    if (!thread->saved_stack_pointer) {
        return thread->stack_external && thread == current_thread;
    }

    if (!thread->kernel_stack_base || !thread->kernel_stack_size) {
        return false;
    }

    stack_end = thread->kernel_stack_base + thread->kernel_stack_size;
    if (stack_end <= thread->kernel_stack_base ||
        stack_end - thread->kernel_stack_base < 8 * sizeof(u64)) {
        return false;
    }

    /* The assembly ABI consumes rbp, rbx, r12-r15, RFLAGS and RIP. */
    return thread->saved_stack_pointer >= thread->kernel_stack_base &&
        thread->saved_stack_pointer <=
        stack_end - 8 * sizeof(u64);
}

static bool thread_context_ready(const kernel_thread_t *thread)
{
    return thread && !thread->context_switch_pending &&
        thread->saved_context_valid &&
        thread_saved_stack_valid(thread);
}

/* The assembly switch consumes the incoming frame and creates the outgoing
 * frame.  Publish those ownership transitions at the exact point where the
 * outgoing RSP has been stored; doing it in the C caller is too early because
 * an interrupt can observe a not-yet-saved stack as resumable. */
void scheduler_context_switch_saved(uintptr_t *outgoing_rsp_slot,
                                    uintptr_t incoming_rsp)
{
    kernel_thread_t *outgoing = NULL;
#if defined(KERNEL_STACK_DIAGNOSTICS)
    uintptr_t stack_pointer;
#endif
    u64 flags;

    if (outgoing_rsp_slot) {
        outgoing = (kernel_thread_t *)((uintptr_t)outgoing_rsp_slot -
            __builtin_offsetof(kernel_thread_t, saved_stack_pointer));
    }
    if (outgoing)
        fpu_state_save(&outgoing->fpu_state);
    /* FS_BASE is not part of the integer interrupt frame. Capture it while
     * this CPU still owns the outgoing context so migration restores the
     * same userspace TLS pointer on whichever CPU resumes it. */
    if (outgoing)
        outgoing->user_fs_base = (uintptr_t)rdmsr(MSR_IA32_FS_BASE);
#if defined(KERNEL_STACK_DIAGNOSTICS)
    __asm__ volatile("movq %%rsp, %0" : "=r"(stack_pointer));
    if (outgoing && outgoing->kernel_stack.active) {
        kernel_stack_sample_rsp(&outgoing->kernel_stack, stack_pointer);
    } else if (outgoing && outgoing->stack_external &&
               outgoing->kernel_stack_size) {
        kernel_stack_usage_sample(KERNEL_STACK_USAGE_BOOTSTRAP,
            outgoing->kernel_stack_base + outgoing->kernel_stack_size,
            stack_pointer);
    }
#endif
    flags = spin_lock_irqsave(&scheduler_lock);
    if (outgoing) {
        outgoing->saved_context_valid = true;
        /* This CPU still executes on the outgoing stack. Saving its frame
         * does not release ownership; only the incoming-stack helper may
         * publish it to other CPUs or permit reclamation. */
    }

    /* current_thread is the target selected by scheduler_dispatch().  Its
     * frame has just been consumed by the pops below, so it is not a valid
     * incoming context again until a later outgoing save recreates it. */
    if (current_thread && current_thread->saved_stack_pointer == incoming_rsp) {
        current_thread->saved_context_valid = false;
    }

    if (current_thread)
        fpu_state_restore(&current_thread->fpu_state);

    spin_unlock_irqrestore(&scheduler_lock, flags);
}

/* thread_context_enter() has no outgoing thread to save.  Restore the
 * selected context before it starts executing its prepared stack. */
void scheduler_context_restore_current(void)
{
    if (!current_thread)
        panic("scheduler: no current thread for FPU restore");
    fpu_state_restore(&current_thread->fpu_state);
    wrmsr(MSR_IA32_FS_BASE, current_thread->user_fs_base);
}

/* The saved outgoing frame is complete when scheduler_context_switch_saved()
 * runs, but that helper still returns through the outgoing stack.  Keep the
 * thread pinned until assembly has switched to the incoming stack, then clear
 * this final reclamation barrier from the target stack. */
process_t *scheduler_context_switch_complete(uintptr_t *outgoing_rsp_slot)
{
    kernel_thread_t *outgoing = NULL;
    bool terminated = false;
    process_t *handoff_process = NULL;
    u64 flags;

    if (outgoing_rsp_slot) {
        outgoing = (kernel_thread_t *)((uintptr_t)outgoing_rsp_slot -
            __builtin_offsetof(kernel_thread_t, saved_stack_pointer));
    }
    flags = spin_lock_irqsave(&scheduler_lock);
    if (outgoing) {
        outgoing->running_cpu = THREAD_CPU_NONE;
        terminated = outgoing->state == THREAD_STATE_TERMINATED;
        /* Keep the descriptor barrier set while process completion still
         * consumes fields from this record. External collectors use the same
         * scheduler predicate before detaching or destroying it. */
        if (terminated && outgoing->process) {
            handoff_process = outgoing->process;
            if (!object_reference(&handoff_process->object)) {
                spin_unlock_irqrestore(&scheduler_lock, flags);
                panic("scheduler: failed to retain process handoff owner");
            }
        } else {
            outgoing->context_switch_pending = false;
        }
        if (outgoing->state == THREAD_STATE_READY && !outgoing->queued &&
            outgoing != &bootstrap_thread &&
            outgoing != scheduler_cpu_local()->scheduler_idle_thread &&
            !scheduler_enqueue_locked(outgoing)) {
            spin_unlock_irqrestore(&scheduler_lock, flags);
            panic("scheduler: failed to publish completed outgoing context");
        }
    }
    /* Assembly calls here on the incoming stack with IF still clear. */
    scheduler_context_switch_in_progress = 0;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    /* This helper runs on the incoming stack, after ownership of the
     * outgoing stack has transferred. Restore architectural thread state
     * here, before the saved syscall/interrupt frame can return to Ring 3. */
    if (current_thread)
        wrmsr(MSR_IA32_FS_BASE, current_thread->user_fs_base);
    if (handoff_process) {
        if (!process_scheduler_context_handoff_complete(outgoing))
            panic("scheduler: process handoff callback rejected owner");
        /* This is the final access to outgoing. Until this publication the
         * process collector sees context_switch_pending and cannot detach
         * the scheduler record. */
        flags = spin_lock_irqsave(&scheduler_lock);
        if (outgoing->state != THREAD_STATE_TERMINATED ||
            outgoing->running_cpu != THREAD_CPU_NONE ||
            !outgoing->context_switch_pending) {
            spin_unlock_irqrestore(&scheduler_lock, flags);
            panic("scheduler: outgoing handoff ownership changed");
        }
        outgoing->context_switch_pending = false;
        spin_unlock_irqrestore(&scheduler_lock, flags);
    }
    return handoff_process;
}

void scheduler_context_switch_post_complete(process_t *process)
{
    if (!process) return;
    /* This runs before the suspended mutex_lock continuation consumes its
     * handoff marker. It is deliberately limited to an event wake; the
     * process cleanup worker performs mutex and VMM operations on its own
     * ordinary kernel stack. A process pointer is carried by this stack
     * frame, so nested/preempted context switches cannot collide in a CPU
     * singleton slot. */
    process_scheduler_post_handoff(process);
    object_release(&process->object);
}

static void scheduler_validate_saved_context(const kernel_thread_t *thread)
{
    const u64 *frame;
    u64 flags;

    if (!thread || !thread_context_ready(thread)) {
        return;
    }

    /* saved_stack_pointer points at rbp; RFLAGS is the seventh qword. */
    frame = (const u64 *)(uintptr_t)thread->saved_stack_pointer;
    flags = frame[6];
    if (!(flags & (1ULL << 1)) ||
        (flags & SCHEDULER_UNSAFE_CONTEXT_FLAGS)) {
        kprint("scheduler: unsafe saved flags thread=%s id=%llu rsp=%p flags=%p\n",
               thread->name, thread->id, thread->saved_stack_pointer, flags);
        panic("scheduler: invalid saved RFLAGS");
    }
}

static bool thread_priority_valid(thread_priority_t priority)
{
    return priority >= THREAD_PRIORITY_HIGH &&
        priority <= THREAD_PRIORITY_BACKGROUND;
}

static bool thread_priority_state_valid(const kernel_thread_t *thread)
{
    if (!thread || !thread_priority_valid(thread->base_priority) ||
        !thread_priority_valid(thread->effective_priority) ||
        thread->effective_priority > thread->base_priority) {
        return false;
    }

    /* Numeric priority decreases as urgency increases.  This permits NORMAL
     * to receive a temporary HIGH turn and BACKGROUND to pass temporarily
     * through NORMAL and HIGH.  A wakeup boost itself is specifically the
     * BACKGROUND-to-NORMAL stage. */
    if (thread->wakeup_boosted &&
        (thread->base_priority != THREAD_PRIORITY_BACKGROUND ||
         thread->effective_priority != THREAD_PRIORITY_NORMAL)) {
        return false;
    }
    return true;
}

static u64 thread_default_time_slice(thread_priority_t priority)
{
    switch (priority) {
        case THREAD_PRIORITY_HIGH:       return THREAD_TIME_SLICE_HIGH;
        case THREAD_PRIORITY_NORMAL:     return THREAD_TIME_SLICE_NORMAL;
        case THREAD_PRIORITY_BACKGROUND: return THREAD_TIME_SLICE_BACKGROUND;
        default:                         return 0;
    }
}

static bool scheduler_validate_locked(void)
{
    thread_ready_queue_t *queue;
    kernel_thread_t *thread;
    kernel_thread_t *previous;
    kernel_thread_t *other;
    kernel_thread_t *slow;
    kernel_thread_t *fast;
    thread_priority_t priority;
    u32 seen;

    if (current_thread && (current_thread->queued ||
                           !thread_priority_state_valid(current_thread))) {
        return false;
    }
    if (idle_thread && (idle_thread->queued ||
        idle_thread->base_priority != THREAD_PRIORITY_BACKGROUND ||
        idle_thread->effective_priority != THREAD_PRIORITY_BACKGROUND)) {
        return false;
    }

    for (u32 cpu_index = 0; cpu_index < cpu_count(); cpu_index++) {
        cpu_local_t *cpu = cpu_by_index(cpu_index);
        if (!cpu || !cpu->online)
            continue;
        if (!cpu->scheduler_context_switching) {
            if (!cpu->scheduler_current_thread ||
                cpu->scheduler_current_thread->state != THREAD_STATE_RUNNING ||
                cpu->scheduler_current_thread->running_cpu != cpu_index ||
                cpu->scheduler_current_thread->queued)
                return false;
        } else if (cpu->scheduler_current_thread &&
                   (cpu->scheduler_current_thread->running_cpu != cpu_index ||
                    cpu->scheduler_current_thread->queued)) {
            return false;
        }
        if (!cpu->scheduler_idle_thread || cpu->scheduler_idle_thread->queued ||
            (cpu->scheduler_idle_thread->running_cpu == THREAD_CPU_NONE &&
             cpu->scheduler_idle_thread->state == THREAD_STATE_RUNNING))
            return false;
    }

    slow = sleeping_threads;
    fast = sleeping_threads;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return false;
        }
    }

    for (priority = THREAD_PRIORITY_HIGH;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        queue = &ready_queues[priority];
        previous = NULL;
        seen = 0;
        for (thread = queue->head; thread; thread = thread->next) {
            if (++seen > queue->count || !thread->queued ||
                thread->state != THREAD_STATE_READY ||
                thread->effective_priority != priority ||
                !thread_priority_state_valid(thread) ||
                thread->sleeping || thread->running_cpu != THREAD_CPU_NONE ||
                thread == idle_thread ||
                thread->previous != previous) {
                return false;
            }
            previous = thread;
        }
        if (seen != queue->count || queue->tail != previous ||
            (queue->head == NULL && queue->tail != NULL) ||
            (queue->head != NULL && queue->tail == NULL)) {
            return false;
        }
    }

    for (priority = THREAD_PRIORITY_HIGH;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        thread_priority_t other_priority;
        for (thread = ready_queues[priority].head; thread;
             thread = thread->next) {
            for (other_priority = priority + 1;
                 other_priority <= THREAD_PRIORITY_BACKGROUND;
                 other_priority++) {
                for (other = ready_queues[other_priority].head;
                     other; other = other->next) {
                    if (thread == other) {
                        return false;
                    }
                }
            }
        }
    }

    for (thread = sleeping_threads; thread; thread = thread->next) {
        if (thread->state != THREAD_STATE_BLOCKED || thread->queued ||
            !thread_priority_state_valid(thread) ||
            !thread->sleeping || thread->running_cpu != THREAD_CPU_NONE ||
            thread == idle_thread) {
            return false;
        }
        for (priority = THREAD_PRIORITY_HIGH;
             priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
            for (other = ready_queues[priority].head; other;
                 other = other->next) {
                if (thread == other) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool scheduler_validate(void)
{
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    bool result = scheduler_validate_locked();
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

bool scheduler_validate_state(void)
{
    return scheduler_validate();
}

u32 scheduler_ready_count(thread_priority_t priority)
{
    u64 flags;
    u32 count;
    if (!thread_priority_valid(priority)) {
        return 0;
    }
    flags = spin_lock_irqsave(&scheduler_lock);
    count = ready_queues[priority].count;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return count;
}

void scheduler_get_stats(scheduler_stats_t *stats)
{
    if (stats) {
        u64 flags = spin_lock_irqsave(&scheduler_lock);
        stats->context_switches = scheduler_stats.context_switches;
        stats->timer_preemptions = preemption_count_snapshot(
            &scheduler_stats.timer_preemptions);
        stats->voluntary_yields = scheduler_stats.voluntary_yields;
        stats->blocks = scheduler_stats.blocks;
        stats->wakeups = scheduler_stats.wakeups;
        stats->sleeping_threads = scheduler_stats.sleeping_threads;
        stats->blocked_threads = scheduler_stats.blocked_threads;
        stats->peak_runnable_threads =
            scheduler_stats.peak_runnable_threads;
        stats->idle_runtime_ticks = scheduler_stats.idle_runtime_ticks;
        for (u32 priority = 0; priority < 3U; priority++)
            stats->dispatches[priority] =
                scheduler_stats.dispatches[priority];
        spin_unlock_irqrestore(&scheduler_lock, flags);
    }
}

void scheduler_dump(void)
{
    thread_priority_t priority;
    kernel_thread_t *thread;
    u32 seen;
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    bool valid = scheduler_validate_locked();

    kprint("Scheduler: tick=%llu current=%s(%llu) idle=%s(%llu) valid=%s\n",
           scheduler_tick_count,
           current_thread ? current_thread->name : "none",
           current_thread ? current_thread->id : 0,
           idle_thread ? idle_thread->name : "none",
           idle_thread ? idle_thread->id : 0,
           valid ? "yes" : "no");
    for (priority = THREAD_PRIORITY_HIGH;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        kprint("  %s[%u]:", thread_priority_name(priority),
               ready_queues[priority].count);
        seen = 0;
        for (thread = ready_queues[priority].head;
             thread && seen++ < ready_queues[priority].count;
             thread = thread->next) {
            kprint(" %s(%llu)", thread->name, thread->id);
        }
        kprint("\n");
    }
    kprint("  sleeping=%llu blocked=%llu:",
           scheduler_stats.sleeping_threads,
           scheduler_stats.blocked_threads);
    seen = 0;
    for (thread = sleeping_threads;
         thread && seen++ < scheduler_stats.sleeping_threads;
         thread = thread->next) {
        kprint(" %s(%llu@%llu)", thread->name, thread->id,
               thread->wakeup_deadline_ms);
    }
    kprint("\n  switches=%llu preemptions=%llu yields=%llu blocks=%llu wakes=%llu\n",
           scheduler_stats.context_switches,
           preemption_count_snapshot(&scheduler_stats.timer_preemptions),
           scheduler_stats.voluntary_yields,
           scheduler_stats.blocks,
           scheduler_stats.wakeups);
    kprint("  peak-runnable=%llu idle-ticks=%llu dispatches=%llu/%llu/%llu\n",
           scheduler_stats.peak_runnable_threads,
           scheduler_stats.idle_runtime_ticks,
           scheduler_stats.dispatches[THREAD_PRIORITY_HIGH],
           scheduler_stats.dispatches[THREAD_PRIORITY_NORMAL],
           scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]);
    spin_unlock_irqrestore(&scheduler_lock, flags);
    process_dump();
}

static void scheduler_remove_queued_locked(kernel_thread_t *thread);

static void scheduler_remove_queued_locked(kernel_thread_t *thread)
{
    thread_ready_queue_t *queue;

    if (!thread || !thread->queued || !thread_priority_valid(thread->effective_priority)) {
        return;
    }

    queue = &ready_queues[thread->effective_priority];
    if (thread->previous) {
        thread->previous->next = thread->next;
    } else {
        queue->head = thread->next;
    }
    if (thread->next) {
        thread->next->previous = thread->previous;
    } else {
        queue->tail = thread->previous;
    }
    if (queue->count) {
        queue->count--;
    }
    thread->next = NULL;
    thread->previous = NULL;
    thread->queued = false;
}

static bool scheduler_enqueue_locked(kernel_thread_t *thread)
{
    thread_ready_queue_t *queue;
    bool result = false;

    if (!thread || thread->state != THREAD_STATE_READY || thread->queued ||
        thread->running_cpu != THREAD_CPU_NONE ||
        !thread_priority_valid(thread->effective_priority)) {
        goto out;
    }

    queue = &ready_queues[thread->effective_priority];
    thread->ready_wait_ticks = 0;
    thread->previous = queue->tail;
    thread->next = NULL;
    thread->queued = true;
    if (queue->tail) {
        queue->tail->next = thread;
    } else {
        queue->head = thread;
    }
    queue->tail = thread;
    queue->count++;
    if (!scheduler_validate_locked()) {
        scheduler_remove_queued_locked(thread);
        goto out;
    }
    scheduler_update_runnable_peak();
    result = true;

out:
    return result;
}

bool scheduler_enqueue(kernel_thread_t *thread)
{
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    bool result = scheduler_enqueue_locked(thread);
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

static bool scheduler_thread_eligible(const kernel_thread_t *thread)
{
    cpu_local_t *cpu = scheduler_cpu_local();

    if (!cpu || !thread)
        return false;
    /* PID 1 begins life in the BSP's bootstrap context.  It has no saved
     * migratable context, so retain this one bootstrap-only exception while
     * ordinary process-backed threads are eligible on every online CPU. */
    return thread != &bootstrap_thread || cpu->bsp;
}

static kernel_thread_t *scheduler_select_next_locked(void)
{
    thread_ready_queue_t *queue;
    kernel_thread_t *thread;
    thread_priority_t priority;

    if (!scheduler_validate_locked()) {
        return NULL;
    }

    for (priority = THREAD_PRIORITY_HIGH;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        queue = &ready_queues[priority];
        for (thread = queue->head; thread; thread = thread->next) {
            if (thread->state != THREAD_STATE_READY ||
                thread->running_cpu != THREAD_CPU_NONE ||
                !scheduler_thread_eligible(thread)) {
                continue;
            }
            if (thread_context_ready(thread)) {
                scheduler_remove_queued_locked(thread);
                thread->last_selected_priority = thread->effective_priority;
                thread->last_selection_was_wakeup_boost =
                    thread->wakeup_boosted;
                scheduler_stats.dispatches[priority]++;
                if (thread->effective_priority != thread->base_priority) {
                    thread->effective_priority = thread->base_priority;
                }
                thread->wakeup_boosted = false;
                thread->ready_wait_ticks = 0;
                return thread;
            }
        }
    }
    if (idle_thread && idle_thread->state == THREAD_STATE_READY &&
        !idle_thread->queued && thread_context_ready(idle_thread) &&
        (idle_thread->running_cpu == THREAD_CPU_NONE ||
         idle_thread->running_cpu == cpu_current_index())) {
        scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]++;
        return idle_thread;
    }
    return NULL;
}

kernel_thread_t *scheduler_select_next(void)
{
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    kernel_thread_t *thread = scheduler_select_next_locked();
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return thread;
}

static bool scheduler_age_ready_threads_locked(void)
{
    kernel_thread_t *thread;
    kernel_thread_t *next;
    thread_priority_t priority;
    thread_priority_t old_priority;
    bool promoted = false;

    for (priority = THREAD_PRIORITY_NORMAL;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        thread = ready_queues[priority].head;
        while (thread) {
            next = thread->next;
            if (thread->ready_wait_ticks != ~(u64)0) {
                thread->ready_wait_ticks++;
            }
            if (thread->effective_priority != thread->base_priority) {
                /* A BACKGROUND thread can arrive at effective NORMAL either
                 * through a wakeup boost or its first starvation rescue.  If
                 * it remains queued, preserve that stage's wait counter and
                 * eventually grant one HIGH turn. */
                if (thread->base_priority == THREAD_PRIORITY_BACKGROUND &&
                    thread->effective_priority == THREAD_PRIORITY_NORMAL &&
                    thread->ready_wait_ticks >=
                        (thread->wakeup_boosted ?
                         BACKGROUND_STARVATION_THRESHOLD :
                         BACKGROUND_HIGH_RESCUE_THRESHOLD)) {
                    old_priority = thread->effective_priority;
                    scheduler_remove_queued_locked(thread);
                    thread->effective_priority = THREAD_PRIORITY_HIGH;
                    thread->wakeup_boosted = false;
                    thread->ready_wait_ticks = 0;
                    promoted = true;
                    if (!scheduler_enqueue_locked(thread)) {
                        thread->effective_priority = old_priority;
                        (void)scheduler_enqueue_locked(thread);
                    }
                } else if (thread->base_priority !=
                               THREAD_PRIORITY_BACKGROUND ||
                           thread->effective_priority !=
                               THREAD_PRIORITY_NORMAL) {
                    thread->ready_wait_ticks = 0;
                }
                thread = next;
                continue;
            }
            if ((thread->base_priority == THREAD_PRIORITY_BACKGROUND &&
                 thread->ready_wait_ticks >= BACKGROUND_STARVATION_THRESHOLD) ||
                (thread->base_priority == THREAD_PRIORITY_NORMAL &&
                 thread->ready_wait_ticks >= NORMAL_STARVATION_THRESHOLD)) {
                old_priority = thread->effective_priority;
                scheduler_remove_queued_locked(thread);
                thread->effective_priority = old_priority - 1;
                thread->ready_wait_ticks = 0;
                promoted = true;
                if (!scheduler_enqueue_locked(thread)) {
                    thread->effective_priority = old_priority;
                    (void)scheduler_enqueue_locked(thread);
                }
            }
            thread = next;
        }
    }
    return promoted;
}

static bool thread_prepare_context(kernel_thread_t *thread)
{
    uintptr_t stack_top;
    uintptr_t stack_pointer;

    if (!thread || !thread->kernel_stack_base ||
        thread->kernel_stack_size < 8 * sizeof(u64)) {
        return false;
    }

    fpu_state_init(&thread->fpu_state);

    stack_top = thread->kernel_stack_base + thread->kernel_stack_size;
    stack_top &= ~(uintptr_t)0x0f;
    if (stack_top < thread->kernel_stack_base + 9 * sizeof(u64)) {
        return false;
    }

    /* Leave RSP % 16 == 8 after ret enters the C trampoline. */
    stack_pointer = stack_top - 2 * sizeof(u64);
    *(u64 *)stack_pointer = (u64)(uintptr_t)thread_entry_trampoline;

    stack_pointer -= sizeof(u64); /* RFLAGS: reserved bit and IF */
    *(u64 *)stack_pointer = (1ULL << 1) | (1ULL << 9);
    stack_pointer -= sizeof(u64); /* r15 */
    *(u64 *)stack_pointer = 0;
    stack_pointer -= sizeof(u64); /* r14 */
    *(u64 *)stack_pointer = 0;
    stack_pointer -= sizeof(u64); /* r13 */
    *(u64 *)stack_pointer = 0;
    stack_pointer -= sizeof(u64); /* r12 */
    *(u64 *)stack_pointer = 0;
    stack_pointer -= sizeof(u64); /* rbx */
    *(u64 *)stack_pointer = 0;
    stack_pointer -= sizeof(u64); /* rbp */
    *(u64 *)stack_pointer = 0;

    thread->saved_stack_pointer = stack_pointer;
    thread->saved_context_valid = true;
    return true;
}

bool scheduler_init(void)
{
    if (!scheduler_cpu_local())
        return false;

    spinlock_init(&scheduler_lock);
    memset(ready_queues, 0, sizeof(ready_queues));
    next_thread_id = 2;
    scheduler_tick_count = 0;
    sleeping_threads = NULL;
    memset(&scheduler_stats, 0, sizeof(scheduler_stats));
    idle_thread = NULL;
    preemption_pending = false;
    scheduler_context_switch_in_progress = 0;
    memset(&bootstrap_thread, 0, sizeof(bootstrap_thread));
    bootstrap_thread.id = 1;
    bootstrap_thread.state = THREAD_STATE_RUNNING;
    bootstrap_thread.effective_priority = THREAD_PRIORITY_NORMAL;
    bootstrap_thread.base_priority = THREAD_PRIORITY_NORMAL;
    bootstrap_thread.default_time_slice =
        thread_default_time_slice(bootstrap_thread.effective_priority);
    bootstrap_thread.remaining_time_slice = bootstrap_thread.default_time_slice;
    bootstrap_thread.saved_stack_pointer = 0;
    bootstrap_thread.saved_context_valid = false;
    bootstrap_thread.running_cpu = cpu_current_index();
    fpu_state_init(&bootstrap_thread.fpu_state);
    bootstrap_thread.stack_external = true;
    bootstrap_thread.kernel_stack_base = (uintptr_t)__stack_bottom;
    bootstrap_thread.kernel_stack_size =
        (usize)((uintptr_t)__stack_top - (uintptr_t)__stack_bottom);
    strncpy(bootstrap_thread.name, "bootstrap", sizeof(bootstrap_thread.name) - 1);
    bootstrap_thread.name[sizeof(bootstrap_thread.name) - 1] = '\0';

    current_thread = &bootstrap_thread;

    /* Keep the idle context out of the ordinary worker queues. */
    idle_thread = thread_create_with_priority("idle", idle_thread_entry,
                                              NULL, THREAD_PRIORITY_BACKGROUND);
    if (!idle_thread) {
        current_thread = NULL;
        return false;
    }
    {
        u64 flags = spin_lock_irqsave(&scheduler_lock);
        scheduler_remove_queued_locked(idle_thread);
        spin_unlock_irqrestore(&scheduler_lock, flags);
    }
    idle_thread->state = THREAD_STATE_READY;
    idle_thread->running_cpu = THREAD_CPU_NONE;
    return true;
}

bool scheduler_prepare_idle_cpu(struct cpu_local *cpu,
                                kernel_stack_t *stack)
{
    kernel_thread_t *idle;
    u64 flags;

    if (!cpu || !cpu->present || cpu->bsp || cpu->scheduler_idle_thread ||
        !stack || !stack->active ||
        stack->size < 8 * sizeof(u64)) {
        return false;
    }

    idle = (kernel_thread_t *)kmalloc(sizeof(*idle));
    if (!idle)
        return false;
    memset(idle, 0, sizeof(*idle));
    idle->state = THREAD_STATE_READY;
    idle->base_priority = THREAD_PRIORITY_BACKGROUND;
    idle->effective_priority = THREAD_PRIORITY_BACKGROUND;
    idle->default_time_slice = thread_default_time_slice(
        THREAD_PRIORITY_BACKGROUND);
    idle->remaining_time_slice = idle->default_time_slice;
    idle->kernel_stack_base = stack->base;
    idle->kernel_stack_size = stack->size;
    idle->entry = idle_thread_entry;
    strncpy(idle->name, "idle", sizeof(idle->name) - 1);
    idle->name[sizeof(idle->name) - 1] = '\0';
    idle->running_cpu = THREAD_CPU_NONE;
    if (!thread_prepare_context(idle)) {
        kfree(idle);
        return false;
    }

    flags = spin_lock_irqsave(&scheduler_lock);
    if (next_thread_id == 0 || cpu->scheduler_idle_thread) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        kfree(idle);
        return false;
    }
    idle->id = next_thread_id++;
    idle->kernel_stack = *stack;
    memset(stack, 0, sizeof(*stack));
    cpu->scheduler_idle_thread = idle;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return true;
}

bool scheduler_start_cpu(void)
{
    cpu_local_t *cpu = scheduler_cpu_local();
    kernel_thread_t *idle;
    uintptr_t incoming_rsp;
    u64 flags;

    if (!cpu || cpu->bsp || !cpu->scheduler_idle_thread ||
        cpu->scheduler_current_thread)
        return false;
    idle = cpu->scheduler_idle_thread;
    flags = spin_lock_irqsave(&scheduler_lock);
    if (idle->state != THREAD_STATE_READY || idle->queued ||
        idle->running_cpu != THREAD_CPU_NONE ||
        !thread_context_ready(idle)) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return false;
    }
    idle->state = THREAD_STATE_RUNNING;
    idle->running_cpu = cpu->index;
    cpu->scheduler_current_thread = idle;
    cpu_mark_online(cpu);
    cpu->kernel_thread_runs = 0;
    incoming_rsp = idle->saved_stack_pointer;
    idle->saved_context_valid = false;
    spin_unlock_irqrestore(&scheduler_lock, flags);

    scheduler_activate_thread_context(idle);
    thread_context_enter(incoming_rsp);
    return false;
}

kernel_thread_t *scheduler_current_thread(void)
{
    kernel_thread_t *thread;

    /* Resolve GS and the thread field in one instruction. A CPU-local
     * pointer retained across preemption may belong to the previous CPU
     * when this thread resumes after migration. */
    __asm__ volatile("movq %%gs:%c1, %0" : "=r"(thread)
                     : "i"(__builtin_offsetof(cpu_local_t,
                                              scheduler_current_thread))
                     : "memory");
    return thread;
}

kernel_thread_t *thread_current(void)
{
    return scheduler_current_thread();
}

bool scheduler_current_stack_guard_fault(uintptr_t fault_address)
{
    kernel_thread_t *thread;

    /* The BSP can hit its linker-reserved guard before scheduler_init has
     * published a current thread. Classify that fixed range independently
     * so early boot overflows are still identified as stack faults. */
    if (fault_address >= (uintptr_t)__stack_guard_bottom &&
        fault_address < (uintptr_t)__stack_bottom)
        return true;

    thread = scheduler_current_thread();
    if (!thread)
        return false;
    if (thread->kernel_stack.active &&
        kernel_stack_guard_contains(&thread->kernel_stack, fault_address))
        return true;
    return false;
}

kernel_thread_t *scheduler_idle_thread(void)
{
    return idle_thread;
}

kernel_thread_t *thread_create_with_priority(const char *name,
                                             thread_entry_t entry,
                                             void *argument,
                                             thread_priority_t priority)
{
    kernel_thread_t *thread;

    if (!name || !entry || !current_thread ||
        !thread_priority_valid(priority)) {
        return NULL;
    }

    thread = (kernel_thread_t *)kmalloc(sizeof(*thread));
    if (!thread) {
        return NULL;
    }

    memset(thread, 0, sizeof(*thread));
    if (!kernel_stack_allocate(&thread->kernel_stack,
                               THREAD_KERNEL_STACK_SIZE,
                               KERNEL_STACK_USAGE_THREAD)) {
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_base = thread->kernel_stack.base;

    thread->running_cpu = THREAD_CPU_NONE;
    thread->state = THREAD_STATE_READY;
    thread->effective_priority = priority;
    thread->base_priority = priority;
    thread->default_time_slice = thread_default_time_slice(priority);
    thread->remaining_time_slice = thread->default_time_slice;
    thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
    thread->entry = entry;
    thread->entry_argument = argument;
    strncpy(thread->name, name, sizeof(thread->name) - 1);
    thread->name[sizeof(thread->name) - 1] = '\0';

    if (!thread_prepare_context(thread)) {
        if (!kernel_stack_release(&thread->kernel_stack))
            panic("scheduler: failed to release an unqueued thread stack");
        kfree(thread);
        return NULL;
    }

    u64 flags = spin_lock_irqsave(&scheduler_lock);
    if (next_thread_id == 0) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        if (!kernel_stack_release(&thread->kernel_stack))
            panic("scheduler: failed to release an unqueued thread stack");
        kfree(thread);
        return NULL;
    }
    thread->id = next_thread_id++;
    if (!scheduler_enqueue_locked(thread)) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        if (!kernel_stack_release(&thread->kernel_stack))
            panic("scheduler: failed to release an unenqueued thread stack");
        kfree(thread);
        return NULL;
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);

    return thread;
}

kernel_thread_t *thread_create_suspended_with_priority(const char *name,
                                                       thread_entry_t entry,
                                                       void *argument,
                                                       thread_priority_t priority)
{
    kernel_thread_t *thread;

    if (!name || !entry || !current_thread ||
        !thread_priority_valid(priority)) {
        return NULL;
    }

    thread = (kernel_thread_t *)kmalloc(sizeof(*thread));
    if (!thread) {
        return NULL;
    }

    memset(thread, 0, sizeof(*thread));
    if (!kernel_stack_allocate(&thread->kernel_stack,
                               THREAD_KERNEL_STACK_SIZE,
                               KERNEL_STACK_USAGE_THREAD)) {
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_base = thread->kernel_stack.base;

    thread->running_cpu = THREAD_CPU_NONE;
    thread->state = THREAD_STATE_READY;
    thread->effective_priority = priority;
    thread->base_priority = priority;
    thread->default_time_slice = thread_default_time_slice(priority);
    thread->remaining_time_slice = thread->default_time_slice;
    thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
    thread->entry = entry;
    thread->entry_argument = argument;
    strncpy(thread->name, name, sizeof(thread->name) - 1);
    thread->name[sizeof(thread->name) - 1] = '\0';

    if (!thread_prepare_context(thread)) {
        if (!kernel_stack_release(&thread->kernel_stack))
            panic("scheduler: failed to release a suspended thread stack");
        kfree(thread);
        return NULL;
    }

    {
        u64 flags = spin_lock_irqsave(&scheduler_lock);
        if (next_thread_id == 0) {
            spin_unlock_irqrestore(&scheduler_lock, flags);
            if (!kernel_stack_release(&thread->kernel_stack))
                panic("scheduler: failed to release a suspended thread stack");
            kfree(thread);
            return NULL;
        }
        thread->id = next_thread_id++;
        spin_unlock_irqrestore(&scheduler_lock, flags);
    }

    return thread;
}

kernel_thread_t *thread_create_suspended(const char *name, thread_entry_t entry,
                                         void *argument)
{
    return thread_create_suspended_with_priority(name, entry, argument, THREAD_PRIORITY_NORMAL);
}

kernel_thread_t *thread_create(const char *name, thread_entry_t entry,
                               void *argument)
{
    return thread_create_with_priority(name, entry, argument,
                                       THREAD_PRIORITY_NORMAL);
}

bool thread_destroy(kernel_thread_t *thread)
{
    u64 flags;

    if (!thread || thread->process ||
        thread->process_state != THREAD_PROCESS_UNATTACHED ||
        thread->process_reference_held)
        return false;

    /* Do not unlink a thread if the VMM's synchronous SMP teardown cannot
     * safely run in the caller's context. */
    if (!thread->stack_external && thread->kernel_stack.active &&
        !kernel_stack_release_context_valid())
        return false;

    flags = spin_lock_irqsave(&scheduler_lock);
    if (thread == &bootstrap_thread || thread->sleeping ||
        thread->running_cpu != THREAD_CPU_NONE ||
        thread->context_switch_pending ||
        thread->preempt_return_rip ||
        (thread->state == THREAD_STATE_READY &&
         (thread->syscall_active || thread->syscall_nesting ||
          (thread->entry_started && !thread->process))) ||
        (thread->state != THREAD_STATE_READY &&
         thread->state != THREAD_STATE_TERMINATED)) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return false;
    }
    for (u32 i = 0; i < cpu_count(); i++) {
        cpu_local_t *cpu = cpu_by_index(i);
        if (cpu && cpu->scheduler_idle_thread == thread) {
            spin_unlock_irqrestore(&scheduler_lock, flags);
            return false;
        }
    }
    scheduler_remove_queued_locked(thread);
    spin_unlock_irqrestore(&scheduler_lock, flags);
    mutex_cancel_waiter(thread);
    if (!thread->stack_external) {
        if (thread->kernel_stack.active) {
            if (!kernel_stack_release(&thread->kernel_stack))
                panic("scheduler: thread stack could not be safely reclaimed");
        } else if (thread->kernel_stack_base || thread->kernel_stack_size) {
            return false;
        }
    }
    kfree(thread);
    return true;
}

bool thread_release_terminated_stack(kernel_thread_t *thread)
{
    u64 flags;

    if (!thread || thread->stack_external ||
        !kernel_stack_release_context_valid() ||
        !scheduler_thread_exit_complete(thread))
        return false;
    if (!thread->kernel_stack.active)
        return thread->kernel_stack_base == 0U &&
               thread->kernel_stack_size == 0U;
    if (!kernel_stack_release(&thread->kernel_stack)) return false;

    flags = spin_lock_irqsave(&scheduler_lock);
    if (thread->state != THREAD_STATE_TERMINATED ||
        thread->running_cpu != THREAD_CPU_NONE ||
        thread->context_switch_pending) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        panic("scheduler: terminated stack reacquired during release");
    }
    thread->kernel_stack_base = 0;
    thread->kernel_stack_size = 0;
    thread->saved_stack_pointer = 0;
    thread->saved_context_valid = false;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return true;
}

static bool scheduler_handoff(kernel_thread_t *outgoing,
                              kernel_thread_t *target, u64 saved_flags)
{
    scheduler_validate_saved_context(target);
    scheduler_activate_thread_context(target);
    outgoing->context_switch_pending = true;
    scheduler_stats.context_switches++;
    /* Keep IF clear until assembly has saved the outgoing frame. */
    spin_unlock(&scheduler_lock);
    thread_context_switch(&outgoing->saved_stack_pointer,
                          target->saved_stack_pointer, saved_flags);
    return true;
}

static bool scheduler_prepare_dispatch_locked(
    scheduler_dispatch_action_t action, kernel_thread_t **outgoing_out,
    kernel_thread_t **target_out, bool *handoff)
{
    cpu_local_t *cpu = scheduler_cpu_local();
    kernel_thread_t *outgoing = current_thread;
    kernel_thread_t *target;
    bool requeue_current = action == SCHEDULER_DISPATCH_REQUEUE;

    if (handoff)
        *handoff = false;
    if (!cpu || !outgoing ||
        (requeue_current && outgoing->state != THREAD_STATE_RUNNING) ||
        (action == SCHEDULER_DISPATCH_BLOCK &&
         outgoing->state != THREAD_STATE_BLOCKED) ||
        (action == SCHEDULER_DISPATCH_TERMINATE &&
         outgoing->state != THREAD_STATE_TERMINATED) ||
        outgoing->queued || !thread_saved_stack_valid(outgoing)) {
        return false;
    }

    scheduler_context_switch_in_progress = 1;
    if (requeue_current) {
        /* Keep ownership until the assembly helper has saved this live stack.
         * Other CPUs therefore cannot select the outgoing thread in the
         * handoff window. */
        outgoing->state = THREAD_STATE_READY;
    } else {
        outgoing->running_cpu = THREAD_CPU_NONE;
        current_thread = NULL;
    }

    target = scheduler_select_next_locked();

    /* The bootstrap context remains a BSP-only special case.  Process-backed
     * threads are eligible on every online CPU once their address space is
     * activated by scheduler_activate_thread_context(). */
    if (target == idle_thread && cpu->bsp &&
        outgoing != &bootstrap_thread &&
        bootstrap_thread.state == THREAD_STATE_READY &&
        bootstrap_thread.running_cpu == THREAD_CPU_NONE &&
        !bootstrap_thread.queued &&
        thread_context_ready(&bootstrap_thread)) {
        if (scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND])
            scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]--;
        target = &bootstrap_thread;
    }

    if (target == idle_thread && action == SCHEDULER_DISPATCH_TERMINATE &&
        cpu->bsp && outgoing != &bootstrap_thread &&
        bootstrap_thread.state == THREAD_STATE_READY &&
        bootstrap_thread.running_cpu == THREAD_CPU_NONE &&
        !bootstrap_thread.queued && thread_context_ready(&bootstrap_thread) &&
        scheduler_stats.blocked_threads == 0) {
        target = &bootstrap_thread;
    }

    /* A requeue request with no other eligible context keeps the live thread
     * running.  In particular, an AP must not fall back to a userspace
     * thread left in the global queue. */
    if (target == idle_thread && requeue_current && outgoing != idle_thread) {
        if (scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND])
            scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]--;
        target = outgoing;
    }

    if (!target) {
        if (requeue_current) {
            outgoing->state = THREAD_STATE_RUNNING;
            current_thread = outgoing;
        } else {
            current_thread = outgoing;
            outgoing->running_cpu = cpu->index;
        }
        scheduler_context_switch_in_progress = 0;
        return false;
    }

    if (target == outgoing) {
        target->state = THREAD_STATE_RUNNING;
        target->running_cpu = cpu->index;
        current_thread = target;
        scheduler_context_switch_in_progress = 0;
        return true;
    }

    if (!thread_context_ready(target))
        panic("scheduler: selected thread has no saved context");
    target->state = THREAD_STATE_RUNNING;
    target->running_cpu = cpu->index;
    current_thread = target;
    if (outgoing_out)
        *outgoing_out = outgoing;
    if (target_out)
        *target_out = target;
    if (handoff)
        *handoff = true;
    return true;
}

bool thread_switch_to(kernel_thread_t *target)
{
    kernel_thread_t *outgoing;

    if (spinlock_depth_current() != 0U)
        return false;
    u64 saved_flags = spin_lock_irqsave(&scheduler_lock);

    if (!target || target == current_thread ||
        target->state != THREAD_STATE_READY ||
        target->running_cpu != THREAD_CPU_NONE ||
        !scheduler_thread_eligible(target) || !thread_context_ready(target)) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }

    if (target != &bootstrap_thread && !target->queued) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }

    outgoing = current_thread;
    if (!outgoing || outgoing->state != THREAD_STATE_RUNNING ||
        outgoing->queued || !thread_saved_stack_valid(outgoing)) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }

    scheduler_remove_queued_locked(target);
    if (target->effective_priority != target->base_priority) {
        target->effective_priority = target->base_priority;
    }
    target->ready_wait_ticks = 0;
    outgoing->state = THREAD_STATE_READY;
    target->state = THREAD_STATE_RUNNING;
    target->running_cpu = cpu_current_index();
    scheduler_context_switch_in_progress = 1;
    current_thread = target;
    return scheduler_handoff(outgoing, target, saved_flags);
}

static bool scheduler_dispatch(scheduler_dispatch_action_t action)
{
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool handoff;
    u64 saved_flags = spin_lock_irqsave(&scheduler_lock);

    if (!scheduler_prepare_dispatch_locked(action, &outgoing, &target,
                                           &handoff)) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }
    if (!handoff) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return true;
    }
    return scheduler_handoff(outgoing, target, saved_flags);
}

bool scheduler_reschedule(void)
{
    if (spinlock_depth_current() != 0U)
        return false;
    return scheduler_dispatch(SCHEDULER_DISPATCH_REQUEUE);
}

bool scheduler_yield(void)
{
    if (spinlock_depth_current() != 0U)
        return false;
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    scheduler_stats.voluntary_yields++;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return scheduler_reschedule();
}

static void sleeping_remove(kernel_thread_t *thread)
{
    kernel_thread_t **cursor;

    if (!thread || !thread->sleeping) {
        return;
    }
    cursor = &sleeping_threads;
    while (*cursor && *cursor != thread) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == thread) {
        *cursor = thread->next;
    }
    thread->next = NULL;
    thread->sleeping = false;
    thread->wakeup_deadline_ms = 0;
}

bool scheduler_terminate_thread(kernel_thread_t *thread)
{
    u64 saved_flags;

    if (!thread)
        return false;
    saved_flags = spin_lock_irqsave(&scheduler_lock);
    /* A blocked or READY syscall still owns its kernel C stack. Cancelling
     * its waiter cannot unwind locks, object references or storage pins. */
    if (thread->state == THREAD_STATE_TERMINATED ||
        thread->running_cpu != THREAD_CPU_NONE ||
        thread->context_switch_pending || thread->preempt_return_rip ||
        thread->syscall_active ||
        thread->syscall_nesting ||
        (thread->entry_started && !thread->process)) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }
    for (u32 i = 0; i < cpu_count(); i++) {
        cpu_local_t *cpu = cpu_by_index(i);
        if (cpu && cpu->scheduler_idle_thread == thread) {
            spin_unlock_irqrestore(&scheduler_lock, saved_flags);
            return false;
        }
    }
    if (thread->queued)
        scheduler_remove_queued_locked(thread);
    if (thread->sleeping) {
        sleeping_remove(thread);
        if (scheduler_stats.sleeping_threads)
            scheduler_stats.sleeping_threads--;
    }
    if (thread->state == THREAD_STATE_BLOCKED &&
        scheduler_stats.blocked_threads) {
        scheduler_stats.blocked_threads--;
    }
    thread->state = THREAD_STATE_TERMINATED;
    spin_unlock_irqrestore(&scheduler_lock, saved_flags);
    mutex_cancel_waiter(thread);
    return true;
}

bool scheduler_terminate_thread_group(kernel_thread_t *head,
                                      usize expected_count)
{
    kernel_thread_t *thread;
    usize count = 0;
    u64 saved_flags;

    if (!head || !expected_count) return false;
    saved_flags = spin_lock_irqsave(&scheduler_lock);

    /* Preflight the complete stable process-owned list before mutating any
     * member. In particular, one running syscall or pending handoff rejects
     * the whole external termination request. */
    for (thread = head; thread && count < expected_count;
         thread = thread->process_next, count++) {
        bool already_stopped = thread->state == THREAD_STATE_TERMINATED &&
            thread->running_cpu == THREAD_CPU_NONE &&
            !thread->context_switch_pending && !thread->preempt_return_rip &&
            !thread->sleeping;
        if (!thread->process ||
            (!already_stopped &&
             (thread->state == THREAD_STATE_TERMINATED ||
              (thread->state != THREAD_STATE_READY &&
               thread->state != THREAD_STATE_BLOCKED) ||
              thread->running_cpu != THREAD_CPU_NONE ||
              thread->context_switch_pending || thread->preempt_return_rip ||
              thread->syscall_active || thread->syscall_nesting ||
              thread->sleeping)) ||
            thread == &bootstrap_thread) {
            spin_unlock_irqrestore(&scheduler_lock, saved_flags);
            return false;
        }
        for (u32 i = 0; i < cpu_count(); i++) {
            cpu_local_t *cpu = cpu_by_index(i);
            if (cpu && cpu->scheduler_idle_thread == thread) {
                spin_unlock_irqrestore(&scheduler_lock, saved_flags);
                return false;
            }
        }
    }
    if (count != expected_count || thread != NULL) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }

    for (thread = head; thread; thread = thread->process_next) {
        if (thread->state == THREAD_STATE_TERMINATED) continue;
        if (thread->queued) scheduler_remove_queued_locked(thread);
        if (thread->sleeping) {
            sleeping_remove(thread);
            if (scheduler_stats.sleeping_threads)
                scheduler_stats.sleeping_threads--;
        }
        if (thread->state == THREAD_STATE_BLOCKED &&
            scheduler_stats.blocked_threads)
            scheduler_stats.blocked_threads--;
        thread->state = THREAD_STATE_TERMINATED;
    }
    spin_unlock_irqrestore(&scheduler_lock, saved_flags);

    /* Waiter removal and process accounting can take their own locks, so they
     * run only after scheduler_lock is released. */
    for (thread = head; thread; thread = thread->process_next) {
        mutex_cancel_waiter(thread);
        if (thread->process)
            process_scheduler_handoff_complete(thread);
    }
    return true;
}

static bool scheduler_unblock_locked(kernel_thread_t *thread)
{
    bool was_sleeping;
    bool old_wakeup_boosted;
    thread_priority_t old_effective_priority;
    u64 old_wakeup_deadline_ms;
    u32 old_running_cpu;

    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_BLOCKED || thread->queued) {
        return false;
    }

    old_running_cpu = thread->running_cpu;
    was_sleeping = thread->sleeping;
    old_wakeup_boosted = thread->wakeup_boosted;
    old_effective_priority = thread->effective_priority;
    old_wakeup_deadline_ms = thread->wakeup_deadline_ms;
    sleeping_remove(thread);
    thread->effective_priority = thread->base_priority ==
        THREAD_PRIORITY_BACKGROUND ? THREAD_PRIORITY_NORMAL :
        thread->base_priority;
    thread->wakeup_boosted = thread->base_priority == THREAD_PRIORITY_BACKGROUND;
    thread->state = THREAD_STATE_READY;
    thread->running_cpu = THREAD_CPU_NONE;
    if (!scheduler_enqueue_locked(thread)) {
        thread->state = THREAD_STATE_BLOCKED;
        thread->running_cpu = old_running_cpu;
        thread->wakeup_boosted = old_wakeup_boosted;
        thread->effective_priority = old_effective_priority;
        if (was_sleeping) {
            thread->wakeup_deadline_ms = old_wakeup_deadline_ms;
            thread->next = sleeping_threads;
            sleeping_threads = thread;
            thread->sleeping = true;
        }
        return false;
    }
    if (scheduler_stats.blocked_threads) {
        scheduler_stats.blocked_threads--;
    }
    if (was_sleeping && scheduler_stats.sleeping_threads) {
        scheduler_stats.sleeping_threads--;
    }
    scheduler_stats.wakeups++;
    /* Keyboard/input wakeups arrive in IRQ context.  The IRQ exit path will
     * perform the deferred switch when the idle thread was running. */
    if (current_thread == idle_thread) {
        preemption_pending = true;
    }
    return true;
}

bool scheduler_unblock(kernel_thread_t *thread)
{
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    bool result = scheduler_unblock_locked(thread);
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

/* Lock order: mutex state_lock -> scheduler_lock. Lifecycle paths release
 * scheduler_lock before mutex_cancel_waiter(). The state test and pending
 * publication share the lock used by scheduler_block(), so another CPU
 * cannot park between them. Ownership has already been assigned by caller. */
bool scheduler_mutex_wake(kernel_thread_t *thread)
{
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    bool result = false;

    if (thread && thread->state == THREAD_STATE_BLOCKED) {
        result = scheduler_unblock_locked(thread);
    } else if (thread && (thread->state == THREAD_STATE_RUNNING ||
                          thread->state == THREAD_STATE_READY)) {
        __atomic_store_n(&thread->mutex_wake_pending, true, __ATOMIC_RELEASE);
        result = true;
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

/* Event sources publish waiter state under their own lock, then take
 * scheduler_lock here. A RUNNING waiter may not yet have called its blocking
 * primitive; publish a pending wake so it cannot park after the event. */
bool scheduler_wake_waiter(kernel_thread_t *thread)
{
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    bool result = false;

    if (thread && thread->state == THREAD_STATE_BLOCKED) {
        result = scheduler_unblock_locked(thread);
    } else if (thread && (thread->state == THREAD_STATE_RUNNING ||
                          thread->state == THREAD_STATE_READY)) {
        __atomic_store_n(&thread->event_wake_pending, true,
                         __ATOMIC_RELEASE);
        result = true;
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

void scheduler_clear_event_wake(kernel_thread_t *thread)
{
    u64 flags;

    if (!thread) return;
    flags = spin_lock_irqsave(&scheduler_lock);
    __atomic_store_n(&thread->event_wake_pending, false, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&scheduler_lock, flags);
}

/* Called with scheduler_lock held. Timed sleeps consume event wakes but must
 * not accidentally consume a mutex handoff intended for scheduler_block(). */
static bool scheduler_consume_pending_wake_locked(kernel_thread_t *thread,
                                                   bool consume_mutex_wake)
{
    bool mutex_wake = false;
    bool event_wake;

    if (!thread) return false;
    if (consume_mutex_wake)
        mutex_wake = __atomic_exchange_n(&thread->mutex_wake_pending, false,
                                         __ATOMIC_ACQ_REL);
    event_wake = __atomic_exchange_n(&thread->event_wake_pending, false,
                                     __ATOMIC_ACQ_REL);
    return mutex_wake || event_wake;
}

bool scheduler_thread_is_running(const kernel_thread_t *thread)
{
    bool running;
    u64 flags;

    if (!thread)
        return false;
    flags = spin_lock_irqsave(&scheduler_lock);
    running = thread->running_cpu != THREAD_CPU_NONE ||
        thread->context_switch_pending;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return running;
}

bool scheduler_thread_exit_complete(const kernel_thread_t *thread)
{
    bool complete;
    u64 flags;

    if (!thread) return false;
    flags = spin_lock_irqsave(&scheduler_lock);
    complete = thread->state == THREAD_STATE_TERMINATED &&
        thread->running_cpu == THREAD_CPU_NONE &&
        !thread->context_switch_pending && !thread->sleeping &&
        !thread->preempt_return_rip;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return complete;
}

bool scheduler_thread_handoff_callback_valid(const kernel_thread_t *thread)
{
    bool valid;
    u64 flags;

    if (!thread) return false;
    flags = spin_lock_irqsave(&scheduler_lock);
    valid = thread->state == THREAD_STATE_TERMINATED &&
        thread->running_cpu == THREAD_CPU_NONE &&
        thread->context_switch_pending && !thread->sleeping &&
        !thread->preempt_return_rip;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return valid;
}

bool scheduler_thread_process_attachable(const kernel_thread_t *thread)
{
    bool attachable;
    u64 flags;

    if (!thread) return false;
    flags = spin_lock_irqsave(&scheduler_lock);
    attachable = thread->state == THREAD_STATE_READY && !thread->queued &&
        thread->running_cpu == THREAD_CPU_NONE &&
        !thread->context_switch_pending && !thread->entry_started &&
        !thread->syscall_active && !thread->syscall_nesting &&
        !thread->sleeping && !thread->preempt_return_rip &&
        !thread->stack_external;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return attachable;
}

bool scheduler_thread_initial_process_attachable(
    const kernel_thread_t *thread)
{
    bool attachable;
    u64 flags;

    if (!thread) return false;
    flags = spin_lock_irqsave(&scheduler_lock);
    attachable = thread == current_thread &&
        thread->state == THREAD_STATE_RUNNING && !thread->queued &&
        !thread->context_switch_pending && !thread->syscall_active &&
        !thread->syscall_nesting && !thread->preempt_return_rip &&
        thread->stack_external;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return attachable;
}

u32 scheduler_thread_running_cpu(const kernel_thread_t *thread)
{
    u32 cpu_index = THREAD_CPU_NONE;
    u64 flags;

    if (!thread)
        return cpu_index;
    flags = spin_lock_irqsave(&scheduler_lock);
    cpu_index = thread->running_cpu;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return cpu_index;
}

u32 scheduler_process_running_cpu_snapshot(const kernel_thread_t *threads)
{
    u32 cpu_index = THREAD_CPU_NONE;
    u64 flags = spin_lock_irqsave(&scheduler_lock);

    for (const kernel_thread_t *thread = threads; thread;
         thread = thread->process_next) {
        u32 running_cpu = thread->running_cpu;
        if (running_cpu == THREAD_CPU_NONE) continue;
        if (cpu_index == THREAD_CPU_NONE) cpu_index = running_cpu;
        else if (cpu_index != running_cpu) {
            cpu_index = ~(u32)1U;
            break;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return cpu_index;
}

thread_state_t scheduler_thread_state_snapshot(const kernel_thread_t *thread)
{
    thread_state_t state = THREAD_STATE_TERMINATED;
    u64 flags;

    if (!thread) return state;
    flags = spin_lock_irqsave(&scheduler_lock);
    state = thread->state;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return state;
}

bool scheduler_block(void)
{
    if (spinlock_depth_current() != 0U)
        return false;
    kernel_thread_t *thread;
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool handoff;
    u64 saved_flags = spin_lock_irqsave(&scheduler_lock);

    /* Pin the CPU before resolving its running thread. */
    thread = current_thread;

    if (scheduler_consume_pending_wake_locked(thread, true)) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return true;
    }

    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }

    thread->state = THREAD_STATE_BLOCKED;
    scheduler_stats.blocks++;
    scheduler_stats.blocked_threads++;
    if (!scheduler_prepare_dispatch_locked(SCHEDULER_DISPATCH_BLOCK,
                                            &outgoing, &target, &handoff)) {
        thread->state = THREAD_STATE_RUNNING;
        thread->running_cpu = cpu_current_index();
        current_thread = thread;
        scheduler_stats.blocks--;
        scheduler_stats.blocked_threads--;
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }
    if (!handoff) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return true;
    }
    return scheduler_handoff(outgoing, target, saved_flags);
}

bool scheduler_terminate(void)
{
    if (spinlock_depth_current() != 0U)
        return false;
    kernel_thread_t *thread;
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool handoff;
    u64 saved_flags;

    saved_flags = spin_lock_irqsave(&scheduler_lock);
    /* Pin the CPU before resolving its running thread. */
    thread = current_thread;
    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }
    thread->state = THREAD_STATE_TERMINATED;
    if (!scheduler_prepare_dispatch_locked(SCHEDULER_DISPATCH_TERMINATE,
                                            &outgoing, &target, &handoff)) {
        thread->state = THREAD_STATE_RUNNING;
        thread->running_cpu = cpu_current_index();
        current_thread = thread;
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }
    if (!handoff) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return true;
    }
    return scheduler_handoff(outgoing, target, saved_flags);
}

bool scheduler_sleep(u64 milliseconds)
{
    kernel_thread_t *thread;
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool handoff;
    u64 now_ms;
    u64 wakeup_deadline_ms;
    u64 saved_flags;

    if (milliseconds == 0) {
        return true;
    }
    if (spinlock_depth_current() != 0U)
        return false;
    now_ms = timer_uptime_ms();
    wakeup_deadline_ms = now_ms > (~(u64)0 - milliseconds) ?
        ~(u64)0 : now_ms + milliseconds;
    saved_flags = spin_lock_irqsave(&scheduler_lock);
    /* Pin the CPU before resolving its running thread. */
    thread = current_thread;
    if (scheduler_consume_pending_wake_locked(thread, false)) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return true;
    }
    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued ||
        thread->sleeping) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }

    thread->wakeup_deadline_ms = wakeup_deadline_ms;
    thread->next = sleeping_threads;
    sleeping_threads = thread;
    thread->sleeping = true;
    scheduler_stats.sleeping_threads++;
    thread->state = THREAD_STATE_BLOCKED;
    scheduler_stats.blocks++;
    scheduler_stats.blocked_threads++;
    if (!scheduler_prepare_dispatch_locked(SCHEDULER_DISPATCH_BLOCK,
                                            &outgoing, &target, &handoff)) {
        sleeping_remove(thread);
        if (scheduler_stats.sleeping_threads) {
            scheduler_stats.sleeping_threads--;
        }
        thread->state = THREAD_STATE_RUNNING;
        current_thread = thread;
        scheduler_stats.blocks--;
        scheduler_stats.blocked_threads--;
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return false;
    }
    if (!handoff) {
        spin_unlock_irqrestore(&scheduler_lock, saved_flags);
        return true;
    }
    return scheduler_handoff(outgoing, target, saved_flags);
}

void scheduler_syscall_enter(void)
{
    if (current_thread) {
        if (current_thread->syscall_nesting != ~(u32)0)
            current_thread->syscall_nesting++;
        current_thread->syscall_active = true;
    }
}

void scheduler_syscall_leave(void)
{
    if (current_thread && current_thread->syscall_nesting == 1U &&
        current_thread->syscall_active &&
        scheduler_interrupts_enabled()) {
        panic("scheduler: syscall resumed with interrupts enabled");
    }
    if (current_thread && current_thread->syscall_nesting) {
        current_thread->syscall_nesting--;
        current_thread->syscall_active = current_thread->syscall_nesting != 0;
    }
}

bool scheduler_global_tick(void)
{
    cpu_local_t *cpu = scheduler_cpu_local();
    kernel_thread_t *sleeping;
    kernel_thread_t *next_sleeping;
    kernel_thread_t *thread;
    bool woke_thread = false;
    bool promoted_thread;
    u64 now_ms = timer_uptime_ms();
    u64 flags;

    if (!cpu || !cpu->bsp)
        return false;
    flags = spin_lock_irqsave(&scheduler_lock);
    if (scheduler_tick_count != ~(u64)0)
        scheduler_tick_count++;

    sleeping = sleeping_threads;
    while (sleeping) {
        next_sleeping = sleeping->next;
        if (sleeping->wakeup_deadline_ms <= now_ms) {
            sleeping_remove(sleeping);
            if (scheduler_stats.sleeping_threads)
                scheduler_stats.sleeping_threads--;
            sleeping->effective_priority = sleeping->base_priority ==
                THREAD_PRIORITY_BACKGROUND ? THREAD_PRIORITY_NORMAL :
                sleeping->base_priority;
            sleeping->wakeup_boosted =
                sleeping->base_priority == THREAD_PRIORITY_BACKGROUND;
            sleeping->state = THREAD_STATE_READY;
            sleeping->running_cpu = THREAD_CPU_NONE;
            if (scheduler_enqueue_locked(sleeping)) {
                woke_thread = true;
                if (scheduler_stats.blocked_threads)
                    scheduler_stats.blocked_threads--;
                scheduler_stats.wakeups++;
            } else {
                sleeping->state = THREAD_STATE_BLOCKED;
                sleeping->next = sleeping_threads;
                sleeping_threads = sleeping;
                sleeping->sleeping = true;
            }
        }
        sleeping = next_sleeping;
    }

    thread = current_thread;
    if (woke_thread && thread &&
        (thread == idle_thread ||
         (idle_thread && thread->effective_priority > THREAD_PRIORITY_HIGH)))
        preemption_pending = true;

    promoted_thread = scheduler_age_ready_threads_locked();
    if (promoted_thread && thread == idle_thread)
        preemption_pending = true;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return woke_thread || promoted_thread;
}

static bool scheduler_has_eligible_ready_locked(void)
{
    for (thread_priority_t priority = THREAD_PRIORITY_HIGH;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        for (kernel_thread_t *thread = ready_queues[priority].head;
             thread; thread = thread->next) {
            if (thread->state == THREAD_STATE_READY &&
                thread->running_cpu == THREAD_CPU_NONE &&
                scheduler_thread_eligible(thread))
                return true;
        }
    }
    return false;
}

bool scheduler_timer_tick(void)
{
    cpu_local_t *cpu = scheduler_cpu_local();
    kernel_thread_t *thread = current_thread;
    bool should_preempt = false;
    u64 flags;

    if (cpu) {
        __atomic_add_fetch(&cpu->scheduler_accounted_ticks, 1,
                           __ATOMIC_RELAXED);
        if (thread && thread != cpu->scheduler_idle_thread &&
            thread->state == THREAD_STATE_RUNNING) {
            __atomic_add_fetch(&cpu->scheduler_busy_ticks, 1,
                               __ATOMIC_RELAXED);
            __atomic_add_fetch(&thread->cpu_time_ticks, 1,
                               __ATOMIC_RELAXED);
            if (thread->process)
                __atomic_add_fetch(&thread->process->cpu_time_ticks, 1,
                                   __ATOMIC_RELAXED);
        }
    }
    if (scheduler_context_switch_in_progress)
        return false;
    flags = spin_lock_irqsave(&scheduler_lock);
    if (thread == idle_thread) {
        scheduler_stats.idle_runtime_ticks++;
        if (scheduler_has_eligible_ready_locked())
            preemption_pending = true;
    }
    if (!thread || thread->state != THREAD_STATE_RUNNING ||
        thread->queued || thread->default_time_slice == 0) {
        should_preempt = preemption_pending;
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return should_preempt;
    }
    if (thread->remaining_time_slice > 0)
        thread->remaining_time_slice--;
    if (thread->remaining_time_slice == 0) {
        thread->remaining_time_slice = thread->default_time_slice;
        preemption_pending = true;
    }
    should_preempt = preemption_pending;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return should_preempt;
}

bool scheduler_prepare_preemption(struct cpu_registers *regs)
{
    kernel_thread_t *thread = current_thread;
    cpu_local_t *cpu = scheduler_cpu_local();

    if (!cpu || __atomic_load_n(&cpu->spinlock_depth, __ATOMIC_ACQUIRE) ||
        scheduler_context_switch_in_progress || !preemption_pending ||
        !regs || !thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued ||
        !thread_saved_stack_valid(thread)) {
        return false;
    }

    if (thread->syscall_active) {
        /* A blocked syscall owns a resumable C frame on the thread's kernel
         * stack.  It may be woken by this IRQ, but must not be turned into a
         * same-ring trampoline context. */
        preemption_pending = false;
        return false;
    }

    /* Process-wide exit is cooperative at syscall boundaries, but a sibling
     * may be executing Ring 3 code without making syscalls. Retire it at the
     * next user-origin interrupt before returning to that address space. The
     * complete privilege frame remains on its kernel stack and the normal
     * scheduler handoff/reaper path still owns all resource release. */
    if (cpu_registers_has_privilege_stack(regs) &&
        process_thread_exit_requested(thread)) {
        process_thread_entry_completed(thread, -1);
        if (!scheduler_terminate())
            panic("scheduler: process-exit worker could not be retired");
        return false;
    }

    /* A privilege-changing interrupt already has a complete user return
     * frame on this thread's kernel stack.  Switch directly from the IRQ
     * handler instead of redirecting RIP to the same-ring trampoline: an
     * iretq cannot return to a kernel address with the saved Ring 3 CS.  The
     * live IRQ frame remains on the outgoing stack and is resumed normally
     * when this thread is selected again. */
    if (cpu_registers_has_privilege_stack(regs)) {
        preemption_pending = false;
        preemption_count_increment(&scheduler_stats.timer_preemptions);
        (void)scheduler_reschedule();
        return false;
    }

    /* The IRQ frame is still live; defer scheduling until iretq has restored it.
     * Keep the original flags so the interrupted context can resume exactly. */
    thread->preempt_return_rip = (uintptr_t)regs->rip;
    thread->preempt_return_rflags = regs->rflags;
    thread->preempt_return_rsp = cpu_registers_interrupted_rsp(regs);
    thread->preempt_return_cs = regs->cs;
    thread->preempt_return_ss = cpu_registers_interrupted_ss(regs);
    thread->preempt_from_user = cpu_registers_has_privilege_stack(regs);
    /* Keep the transition interrupt-free until the trampoline is running. */
    regs->rflags &= ~(1ULL << 9);
    regs->rip = (u64)(uintptr_t)&thread_interrupt_return_trampoline;
    preemption_count_increment(&scheduler_stats.timer_preemptions);
    preemption_pending = false;
    return true;
}

u64 scheduler_preempt_from_trampoline(void)
{
    kernel_thread_t *thread = current_thread;
    u64 return_rip;

    if (!thread || thread->state != THREAD_STATE_RUNNING ||
        !thread->preempt_return_rip) {
        return 0;
    }

    return_rip = (u64)thread->preempt_return_rip;
    (void)scheduler_reschedule();
    return return_rip;
}

u64 scheduler_preempt_return_flags(void)
{
    return current_thread ? current_thread->preempt_return_rflags : 0;
}

void scheduler_preempt_context_restored(uintptr_t restored_rsp)
{
    if (current_thread) {
        /* A restored context must stay within an owned kernel stack.  The
         * bootstrap stack is supplied by the boot environment and is
         * intentionally exempt from the allocator-range check. */
        if (!current_thread->stack_external &&
            (restored_rsp < current_thread->kernel_stack_base ||
             restored_rsp >= current_thread->kernel_stack_base +
                 current_thread->kernel_stack_size)) {
            uintptr_t stack_base = current_thread->kernel_stack_base;
            uintptr_t stack_end = stack_base +
                                  current_thread->kernel_stack_size;
            kprint("scheduler: restored stack mismatch cpu=%u thread=%p "
                   "id=%llu rsp=%p range=%p-%p size=%llu saved=%p "
                   "preempt_rsp=%p preempt_rip=%p running_cpu=%u state=%u\n",
                   cpu_current_index(), (void *)current_thread,
                   current_thread->id, (void *)restored_rsp,
                   (void *)stack_base, (void *)stack_end,
                   current_thread->kernel_stack_size,
                   (void *)current_thread->saved_stack_pointer,
                   (void *)current_thread->preempt_return_rsp,
                   (void *)current_thread->preempt_return_rip,
                   current_thread->running_cpu,
                   (unsigned)current_thread->state);
            panic("scheduler: restored stack outside kernel stack");
        }
        current_thread->preempt_return_rip = 0;
        current_thread->preempt_return_rflags = 0;
    }
}

const char *thread_state_name(thread_state_t state)
{
    switch (state) {
        case THREAD_STATE_RUNNING:    return "running";
        case THREAD_STATE_READY:      return "ready";
        case THREAD_STATE_BLOCKED:    return "blocked";
        case THREAD_STATE_TERMINATED: return "terminated";
        default:                      return "unknown";
    }
}

const char *thread_priority_name(thread_priority_t priority)
{
    switch (priority) {
        case THREAD_PRIORITY_HIGH:       return "high";
        case THREAD_PRIORITY_NORMAL:     return "normal";
        case THREAD_PRIORITY_BACKGROUND: return "background";
        default:                         return "unknown";
    }
}
