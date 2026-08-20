#include <scheduler.h>
#include <idt.h>
#include <heap.h>
#include <string.h>
#include <kprint.h>
#include <panic.h>
#include <process.h>
#include <vmm.h>
#include <gdt.h>
#include <timer.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

extern char __stack_bottom[];
extern char __stack_top[];

static kernel_thread_t bootstrap_thread;
static kernel_thread_t *idle_thread;
static kernel_thread_t *current_thread;
static u64 next_thread_id = 2;
static u64 scheduler_tick_count;
static kernel_thread_t *sleeping_threads;
static scheduler_stats_t scheduler_stats;
/* Set only during the short C-to-assembly handoff.  IRQ accounting may still
 * run, but it must not capture a preemption frame while current_thread names
 * the target and RSP still belongs to the outgoing thread. */
volatile u8 scheduler_context_switch_in_progress;

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
extern void thread_interrupt_return_trampoline(void);

typedef enum {
    SCHEDULER_DISPATCH_REQUEUE = 0,
    SCHEDULER_DISPATCH_BLOCK,
    SCHEDULER_DISPATCH_TERMINATE,
} scheduler_dispatch_action_t;

static bool scheduler_dispatch(scheduler_dispatch_action_t action);
static bool scheduler_dispatch_internal(scheduler_dispatch_action_t action,
                                         bool caller_locked,
                                         u64 caller_flags);
static bool preemption_pending;

/* Scheduler queue/state transitions must be indivisible with respect to the
 * timer and device IRQs. Keep the caller's IF bit separately: the assembly
 * switch is entered with interrupts masked, but it must still save the flags
 * that were live before this critical section. */
static u64 scheduler_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void scheduler_irq_restore(u64 flags)
{
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static bool scheduler_dispatch_return(bool result, u64 flags)
{
    scheduler_irq_restore(flags);
    return result;
}

static bool scheduler_dispatch_finish(bool result, u64 flags,
                                      bool restore_flags)
{
    if (restore_flags) {
        scheduler_irq_restore(flags);
    }
    return result;
}

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
    vmm_switch_address_space(address_space);
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
    kernel_thread_t *thread = current_thread;

    if (thread && thread->entry) {
        thread->entry(thread->entry_argument);
    }

    if (thread) {
        thread->state = THREAD_STATE_TERMINATED;
    }

    /* Termination uses the same dispatch path as yield and future preemption. */
    if (thread && scheduler_dispatch(SCHEDULER_DISPATCH_TERMINATE)) {
        return;
    }

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void idle_thread_entry(void *argument)
{
    (void)argument;
    for (;;) {
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
    return thread && thread->saved_context_valid &&
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

    if (outgoing_rsp_slot) {
        outgoing = (kernel_thread_t *)((uintptr_t)outgoing_rsp_slot -
            __builtin_offsetof(kernel_thread_t, saved_stack_pointer));
    }
    if (outgoing) {
        outgoing->saved_context_valid = true;
    }

    /* current_thread is the target selected by scheduler_dispatch().  Its
     * frame has just been consumed by the pops below, so it is not a valid
     * incoming context again until a later outgoing save recreates it. */
    if (current_thread && current_thread->saved_stack_pointer == incoming_rsp) {
        current_thread->saved_context_valid = false;
    }

    /* context_switch.s has already saved the outgoing flags and disabled
     * interrupts; the target stack is now safe to expose to IRQ code. */
    scheduler_context_switch_in_progress = 0;
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

static bool scheduler_validate(void)
{
    thread_ready_queue_t *queue;
    kernel_thread_t *thread;
    kernel_thread_t *previous;
    kernel_thread_t *other;
    kernel_thread_t *slow;
    kernel_thread_t *fast;
    thread_priority_t priority;
    u32 seen;

    if (current_thread && current_thread->queued) {
        return false;
    }
    if (idle_thread && idle_thread->queued) {
        return false;
    }
    if (current_thread && !thread_priority_state_valid(current_thread)) {
        return false;
    }
    if (idle_thread && (idle_thread->base_priority != THREAD_PRIORITY_BACKGROUND ||
        idle_thread->effective_priority != THREAD_PRIORITY_BACKGROUND)) {
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
                thread->sleeping || thread == current_thread ||
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
            !thread->sleeping || thread == idle_thread ||
            thread == current_thread) {
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

bool scheduler_validate_state(void)
{
    return scheduler_validate();
}

u32 scheduler_ready_count(thread_priority_t priority)
{
    if (!thread_priority_valid(priority)) {
        return 0;
    }
    return ready_queues[priority].count;
}

void scheduler_get_stats(scheduler_stats_t *stats)
{
    if (stats) {
        *stats = scheduler_stats;
    }
}

void scheduler_dump(void)
{
    thread_priority_t priority;
    kernel_thread_t *thread;
    u32 seen;

    kprint("Scheduler: tick=%llu current=%s(%llu) idle=%s(%llu) valid=%s\n",
           scheduler_tick_count,
           current_thread ? current_thread->name : "none",
           current_thread ? current_thread->id : 0,
           idle_thread ? idle_thread->name : "none",
           idle_thread ? idle_thread->id : 0,
           scheduler_validate() ? "yes" : "no");
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
               thread->wakeup_tick);
    }
    kprint("\n  switches=%llu preemptions=%llu yields=%llu blocks=%llu wakes=%llu\n",
           scheduler_stats.context_switches,
           scheduler_stats.timer_preemptions,
           scheduler_stats.voluntary_yields,
           scheduler_stats.blocks,
           scheduler_stats.wakeups);
    kprint("  peak-runnable=%llu idle-ticks=%llu dispatches=%llu/%llu/%llu\n",
           scheduler_stats.peak_runnable_threads,
           scheduler_stats.idle_runtime_ticks,
           scheduler_stats.dispatches[THREAD_PRIORITY_HIGH],
           scheduler_stats.dispatches[THREAD_PRIORITY_NORMAL],
           scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]);
}

static void scheduler_remove_queued(kernel_thread_t *thread);

static void scheduler_remove_queued(kernel_thread_t *thread)
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

bool scheduler_enqueue(kernel_thread_t *thread)
{
    thread_ready_queue_t *queue;

    if (!thread || thread->state != THREAD_STATE_READY || thread->queued ||
        !thread_priority_valid(thread->effective_priority)) {
        return false;
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
    if (!scheduler_validate()) {
        scheduler_remove_queued(thread);
        return false;
    }
    scheduler_update_runnable_peak();
    return true;
}

kernel_thread_t *scheduler_select_next(void)
{
    thread_ready_queue_t *queue;
    kernel_thread_t *thread;
    thread_priority_t priority;

    if (!scheduler_validate()) {
        return NULL;
    }

    for (priority = THREAD_PRIORITY_HIGH;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        queue = &ready_queues[priority];
        while (queue->head) {
            thread = queue->head;
            scheduler_remove_queued(thread);
            if (thread->state == THREAD_STATE_READY &&
                !thread->queued && thread->effective_priority == priority &&
                thread_context_ready(thread)) {
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
        !idle_thread->queued && thread_context_ready(idle_thread)) {
        scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]++;
        return idle_thread;
    }
    return NULL;
}

static bool scheduler_age_ready_threads(void)
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
                    scheduler_remove_queued(thread);
                    thread->effective_priority = THREAD_PRIORITY_HIGH;
                    thread->wakeup_boosted = false;
                    thread->ready_wait_ticks = 0;
                    promoted = true;
                    if (!scheduler_enqueue(thread)) {
                        thread->effective_priority = old_priority;
                        (void)scheduler_enqueue(thread);
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
                scheduler_remove_queued(thread);
                thread->effective_priority = old_priority - 1;
                thread->ready_wait_ticks = 0;
                promoted = true;
                if (!scheduler_enqueue(thread)) {
                    thread->effective_priority = old_priority;
                    (void)scheduler_enqueue(thread);
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
    scheduler_remove_queued(idle_thread);
    idle_thread->state = THREAD_STATE_READY;
    return true;
}

kernel_thread_t *thread_current(void)
{
    return current_thread;
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

    if (!name || !entry || !current_thread || next_thread_id == 0 ||
        !thread_priority_valid(priority)) {
        return NULL;
    }

    thread = (kernel_thread_t *)kmalloc(sizeof(*thread));
    if (!thread) {
        return NULL;
    }

    memset(thread, 0, sizeof(*thread));
    thread->kernel_stack_base = (uintptr_t)kmalloc(THREAD_KERNEL_STACK_SIZE);
    if (!thread->kernel_stack_base) {
        kfree(thread);
        return NULL;
    }

    thread->id = next_thread_id++;
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
        kfree((void *)thread->kernel_stack_base);
        kfree(thread);
        return NULL;
    }

    if (!scheduler_enqueue(thread)) {
        kfree((void *)thread->kernel_stack_base);
        kfree(thread);
        return NULL;
    }

    return thread;
}

kernel_thread_t *thread_create_suspended_with_priority(const char *name,
                                                       thread_entry_t entry,
                                                       void *argument,
                                                       thread_priority_t priority)
{
    kernel_thread_t *thread;

    if (!name || !entry || !current_thread || next_thread_id == 0 ||
        !thread_priority_valid(priority)) {
        return NULL;
    }

    thread = (kernel_thread_t *)kmalloc(sizeof(*thread));
    if (!thread) {
        return NULL;
    }

    memset(thread, 0, sizeof(*thread));
    thread->kernel_stack_base = (uintptr_t)kmalloc(THREAD_KERNEL_STACK_SIZE);
    if (!thread->kernel_stack_base) {
        kfree(thread);
        return NULL;
    }

    thread->id = next_thread_id++;
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
        kfree((void *)thread->kernel_stack_base);
        kfree(thread);
        return NULL;
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
    if (!thread || thread == &bootstrap_thread || thread == idle_thread ||
        thread == current_thread ||
        thread->sleeping ||
        (thread->state != THREAD_STATE_READY &&
         thread->state != THREAD_STATE_TERMINATED)) {
        return false;
    }

    scheduler_remove_queued(thread);
    kfree((void *)thread->kernel_stack_base);
    kfree(thread);
    return true;
}

bool thread_switch_to(kernel_thread_t *target)
{
    kernel_thread_t *outgoing;
    bool target_was_queued;
    u64 saved_flags = scheduler_irq_save();

    if (!target || target == current_thread ||
        target->state != THREAD_STATE_READY ||
        !thread_context_ready(target)) {
        return scheduler_dispatch_return(false, saved_flags);
    }

    target_was_queued = target->queued;
    if (target != &bootstrap_thread && !target_was_queued) {
        return scheduler_dispatch_return(false, saved_flags);
    }

    outgoing = current_thread;
    if (!outgoing || outgoing->state != THREAD_STATE_RUNNING ||
        outgoing->queued || !thread_saved_stack_valid(outgoing)) {
        return scheduler_dispatch_return(false, saved_flags);
    }

    scheduler_remove_queued(target);
    if (target->effective_priority != target->base_priority) {
        target->effective_priority = target->base_priority;
    }
    target->ready_wait_ticks = 0;
    outgoing->state = THREAD_STATE_READY;
    if (!scheduler_enqueue(outgoing)) {
        outgoing->state = THREAD_STATE_RUNNING;
        if (target_was_queued) {
            scheduler_enqueue(target);
        }
        return scheduler_dispatch_return(false, saved_flags);
    }
    target->state = THREAD_STATE_RUNNING;
    scheduler_context_switch_in_progress = 1;
    current_thread = target;
    scheduler_validate_saved_context(target);
    scheduler_activate_thread_context(target);
    scheduler_stats.context_switches++;
    thread_context_switch(&outgoing->saved_stack_pointer,
                          target->saved_stack_pointer, saved_flags);
    return scheduler_dispatch_return(true, saved_flags);
}

static bool scheduler_dispatch(scheduler_dispatch_action_t action)
{
    return scheduler_dispatch_internal(action, false, 0);
}

static bool scheduler_dispatch_internal(scheduler_dispatch_action_t action,
                                         bool caller_locked,
                                         u64 caller_flags)
{
    /* All yield, block, terminate, and deferred-preemption paths converge
     * here so queue/state invariants are changed in one place. */
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool queue_outgoing;
    bool requeue_current = action == SCHEDULER_DISPATCH_REQUEUE;
    bool restore_flags = !caller_locked;
    u64 saved_flags = caller_locked ? caller_flags : scheduler_irq_save();


    outgoing = current_thread;
    if (!outgoing ||
        (requeue_current && outgoing->state != THREAD_STATE_RUNNING) ||
        (action == SCHEDULER_DISPATCH_BLOCK &&
         outgoing->state != THREAD_STATE_BLOCKED) ||
        (action == SCHEDULER_DISPATCH_TERMINATE &&
         outgoing->state != THREAD_STATE_TERMINATED) ||
        outgoing->queued ||
        !thread_saved_stack_valid(outgoing)) {
        return scheduler_dispatch_finish(false, saved_flags, restore_flags);
    }

    /* Bootstrap remains READY but unqueued while it yields to workers. */
    queue_outgoing = requeue_current && outgoing != &bootstrap_thread &&
        outgoing != idle_thread;
    if (requeue_current) {
        outgoing->state = THREAD_STATE_READY;
    }
    if (queue_outgoing) {
        /* The outgoing thread is READY while it is being published to a
         * queue.  Detach current_thread during that brief transition so the
         * invariant "the current thread is never queued" remains true even
         * while scheduler_enqueue() validates the queue. */
        current_thread = NULL;
        if (!scheduler_enqueue(outgoing)) {
            current_thread = outgoing;
            outgoing->state = THREAD_STATE_RUNNING;
            return scheduler_dispatch_finish(false, saved_flags, restore_flags);
        }
    } else if (action == SCHEDULER_DISPATCH_BLOCK) {
        /* A sleeper is already on sleeping_threads at this point.  It is no
         * longer the runnable current thread while the next context is being
         * selected, so detach it before scheduler_validate() inspects the
         * sleep-list invariant. */
        current_thread = NULL;
    }

    target = scheduler_select_next();
    if (target == idle_thread &&
        action == SCHEDULER_DISPATCH_TERMINATE &&
        outgoing != &bootstrap_thread &&
        bootstrap_thread.state == THREAD_STATE_READY &&
        !bootstrap_thread.queued &&
        scheduler_stats.blocked_threads == 0) {
        /* No runnable or blocked worker remains; bootstrap is the
         * cooperative fallback.  If a worker is still blocked (for example,
         * on a sleep deadline), idle must continue until it becomes READY. */
        if (scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]) {
            scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]--;
        }
        target = &bootstrap_thread;
    }

    /* Bootstrap is deliberately not an ordinary ready-queue member while it
     * hands work to scheduler threads.  A timer preemption or yield cannot
     * hand an otherwise-alone bootstrap context to idle: there is no queued
     * work to run, so it must simply continue on its existing stack. */
    if (target == idle_thread && requeue_current &&
        outgoing == &bootstrap_thread) {
        if (scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]) {
            scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]--;
        }
        target = outgoing;
    }
    if (!target) {
        if (queue_outgoing) {
            scheduler_remove_queued(outgoing);
        }
        if (requeue_current) {
            outgoing->state = THREAD_STATE_RUNNING;
            current_thread = outgoing;
        } else if (action == SCHEDULER_DISPATCH_BLOCK) {
            current_thread = outgoing;
        }
        return scheduler_dispatch_finish(false, saved_flags, restore_flags);
    }

    if (target == outgoing) {
        target->state = THREAD_STATE_RUNNING;
        current_thread = target;
        return scheduler_dispatch_finish(false, saved_flags, restore_flags);
    }

    if (!thread_context_ready(target)) {
        panic("scheduler: selected thread has no saved context");
    }

    target->state = THREAD_STATE_RUNNING;
    scheduler_context_switch_in_progress = 1;
    current_thread = target;
    scheduler_validate_saved_context(target);
    scheduler_activate_thread_context(target);
    scheduler_stats.context_switches++;
    /* The context-switch frame restores the target's RFLAGS.  A deferred IRQ
     * trampoline therefore resumes with IF clear, while a blocked syscall
     * resumes with the IF-masked state established by SYSCALL. */
    thread_context_switch(&outgoing->saved_stack_pointer,
                          target->saved_stack_pointer, saved_flags);
    return scheduler_dispatch_finish(true, saved_flags, restore_flags);
}

bool scheduler_reschedule(void)
{
    return scheduler_dispatch(SCHEDULER_DISPATCH_REQUEUE);
}

bool scheduler_yield(void)
{
    scheduler_stats.voluntary_yields++;
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
    thread->wakeup_tick = 0;
}

bool scheduler_unblock(kernel_thread_t *thread)
{
    bool was_sleeping;
    bool old_wakeup_boosted;
    thread_priority_t old_effective_priority;
    u64 old_wakeup_tick;
    u64 saved_flags = scheduler_irq_save();

    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_BLOCKED || thread->queued) {
        scheduler_irq_restore(saved_flags);
        return false;
    }

    was_sleeping = thread->sleeping;
    old_wakeup_boosted = thread->wakeup_boosted;
    old_effective_priority = thread->effective_priority;
    old_wakeup_tick = thread->wakeup_tick;
    sleeping_remove(thread);
    thread->effective_priority = thread->base_priority ==
        THREAD_PRIORITY_BACKGROUND ? THREAD_PRIORITY_NORMAL :
        thread->base_priority;
    thread->wakeup_boosted = thread->base_priority == THREAD_PRIORITY_BACKGROUND;
    thread->state = THREAD_STATE_READY;
    if (!scheduler_enqueue(thread)) {
        thread->state = THREAD_STATE_BLOCKED;
        thread->wakeup_boosted = old_wakeup_boosted;
        thread->effective_priority = old_effective_priority;
        if (was_sleeping) {
            thread->wakeup_tick = old_wakeup_tick;
            thread->next = sleeping_threads;
            sleeping_threads = thread;
            thread->sleeping = true;
        }
        scheduler_irq_restore(saved_flags);
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
    scheduler_irq_restore(saved_flags);
    return true;
}

bool scheduler_block(void)
{
    kernel_thread_t *thread = current_thread;
    u64 saved_flags = scheduler_irq_save();

    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued) {
        scheduler_irq_restore(saved_flags);
        return false;
    }

    thread->state = THREAD_STATE_BLOCKED;
    scheduler_stats.blocks++;
    scheduler_stats.blocked_threads++;
    if (!scheduler_dispatch_internal(SCHEDULER_DISPATCH_BLOCK, true,
                                     saved_flags)) {
        thread->state = THREAD_STATE_RUNNING;
        current_thread = thread;
        scheduler_stats.blocks--;
        scheduler_stats.blocked_threads--;
        scheduler_irq_restore(saved_flags);
        return false;
    }
    scheduler_irq_restore(saved_flags);
    return true;
}

bool scheduler_terminate(void)
{
    kernel_thread_t *thread = current_thread;
    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued) {
        return false;
    }
    thread->state = THREAD_STATE_TERMINATED;
    return scheduler_dispatch(SCHEDULER_DISPATCH_TERMINATE);
}

bool scheduler_sleep(u64 ticks)
{
    kernel_thread_t *thread = current_thread;
    u64 saved_flags;

    if (ticks == 0) {
        return true;
    }
    saved_flags = scheduler_irq_save();
    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued ||
        thread->sleeping) {
        scheduler_irq_restore(saved_flags);
        return false;
    }

    thread->wakeup_tick = scheduler_tick_count > (~(u64)0 - ticks) ?
        ~(u64)0 : scheduler_tick_count + ticks;
    thread->next = sleeping_threads;
    sleeping_threads = thread;
    thread->sleeping = true;
    scheduler_stats.sleeping_threads++;
    thread->state = THREAD_STATE_BLOCKED;
    scheduler_stats.blocks++;
    scheduler_stats.blocked_threads++;
    if (!scheduler_dispatch_internal(SCHEDULER_DISPATCH_BLOCK, true,
                                     saved_flags)) {
        sleeping_remove(thread);
        if (scheduler_stats.sleeping_threads) {
            scheduler_stats.sleeping_threads--;
        }
        thread->state = THREAD_STATE_RUNNING;
        current_thread = thread;
        scheduler_stats.blocks--;
        scheduler_stats.blocked_threads--;
        scheduler_irq_restore(saved_flags);
        return false;
    }
    scheduler_irq_restore(saved_flags);
    return true;
}

void scheduler_syscall_enter(void)
{
    if (current_thread) current_thread->syscall_active = true;
}

void scheduler_syscall_leave(void)
{
    if (current_thread && current_thread->syscall_active &&
        scheduler_interrupts_enabled()) {
        panic("scheduler: syscall resumed with interrupts enabled");
    }
    if (current_thread) current_thread->syscall_active = false;
}

bool scheduler_timer_tick(void)
{
    kernel_thread_t *thread = current_thread;
    kernel_thread_t *sleeping;
    kernel_thread_t *next_sleeping;
    bool woke_thread = false;
    bool promoted_thread;

    if (scheduler_context_switch_in_progress) {
        return false;
    }

    if (thread == idle_thread) {
        scheduler_stats.idle_runtime_ticks++;
    }

    if (scheduler_tick_count != ~(u64)0) {
        scheduler_tick_count++;
    }

    sleeping = sleeping_threads;
    while (sleeping) {
        next_sleeping = sleeping->next;
        if (sleeping->wakeup_tick <= scheduler_tick_count) {
            sleeping_remove(sleeping);
            if (scheduler_stats.sleeping_threads) {
                scheduler_stats.sleeping_threads--;
            }
            sleeping->effective_priority = sleeping->base_priority ==
                THREAD_PRIORITY_BACKGROUND ? THREAD_PRIORITY_NORMAL :
                sleeping->base_priority;
            sleeping->wakeup_boosted =
                sleeping->base_priority == THREAD_PRIORITY_BACKGROUND;
            sleeping->state = THREAD_STATE_READY;
            if (scheduler_enqueue(sleeping)) {
                woke_thread = true;
                if (scheduler_stats.blocked_threads) {
                    scheduler_stats.blocked_threads--;
                }
                scheduler_stats.wakeups++;
            } else {
                sleeping->state = THREAD_STATE_BLOCKED;
            }
        }
        sleeping = next_sleeping;
    }

    if (woke_thread && thread &&
        (thread == idle_thread ||
         (idle_thread && thread->effective_priority > THREAD_PRIORITY_HIGH))) {
        preemption_pending = true;
    }

    promoted_thread = scheduler_age_ready_threads();
    if (promoted_thread && thread == idle_thread) {
        preemption_pending = true;
    }

    if (!thread || thread->state != THREAD_STATE_RUNNING ||
        thread->queued || thread->default_time_slice == 0) {
        return false;
    }

    if (thread->remaining_time_slice > 0) {
        thread->remaining_time_slice--;
    }
    if (thread->remaining_time_slice != 0) {
        return false;
    }

    thread->remaining_time_slice = thread->default_time_slice;
    preemption_pending = true;
    return true;
}

bool scheduler_prepare_preemption(struct cpu_registers *regs)
{
    kernel_thread_t *thread = current_thread;

    if (scheduler_context_switch_in_progress || !preemption_pending ||
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

    /* User execution has no process/context-switch support yet.  Leave its
     * complete privilege-changing interrupt frame untouched so iretq returns
     * to Ring 3 safely; timer accounting and IRQ acknowledgement continue. */
    if (cpu_registers_has_privilege_stack(regs)) {
        preemption_pending = false;
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
    scheduler_stats.timer_preemptions++;
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
