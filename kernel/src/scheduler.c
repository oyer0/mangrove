#include <scheduler.h>
#include <idt.h>
#include <heap.h>
#include <string.h>

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

typedef struct {
    kernel_thread_t *head;
    kernel_thread_t *tail;
    u32 count;
} thread_ready_queue_t;

static thread_ready_queue_t ready_queues[3];

extern void thread_context_switch(uintptr_t *outgoing_stack_pointer,
                                  uintptr_t incoming_stack_pointer);
extern void thread_context_switch_interrupts_enabled(
    uintptr_t *outgoing_stack_pointer, uintptr_t incoming_stack_pointer);
extern void thread_context_switch_interrupts_disabled(
    uintptr_t *outgoing_stack_pointer, uintptr_t incoming_stack_pointer);
extern void thread_interrupt_return_trampoline(void);

typedef enum {
    SCHEDULER_DISPATCH_REQUEUE = 0,
    SCHEDULER_DISPATCH_BLOCK,
    SCHEDULER_DISPATCH_TERMINATE,
} scheduler_dispatch_action_t;

static bool scheduler_dispatch(scheduler_dispatch_action_t action);
static bool interrupt_switch_requested;
static bool preemption_pending;

/*
 * Phase 13.3 context-switch ABI:
 *
 *     push r15, r14, r13, r12, rbx, rbp
 *     save/restore RSP
 *     pop rbp, rbx, r12, r13, r14, r15
 *     ret
 *
 * A prepared thread stack therefore contains (from low to high addresses)
 * rbp, rbx, r12, r13, r14, r15, and the trampoline return address.
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

    if (!thread || !thread->kernel_stack_base ||
        !thread->kernel_stack_size || !thread->saved_stack_pointer) {
        return false;
    }

    stack_end = thread->kernel_stack_base + thread->kernel_stack_size;
    return stack_end > thread->kernel_stack_base &&
        thread->saved_stack_pointer >= thread->kernel_stack_base &&
        thread->saved_stack_pointer < stack_end;
}

static bool thread_priority_valid(thread_priority_t priority)
{
    return priority >= THREAD_PRIORITY_HIGH &&
        priority <= THREAD_PRIORITY_BACKGROUND;
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
    thread_priority_t priority;
    u32 seen;

    for (priority = THREAD_PRIORITY_HIGH;
         priority <= THREAD_PRIORITY_BACKGROUND; priority++) {
        queue = &ready_queues[priority];
        previous = NULL;
        seen = 0;
        for (thread = queue->head; thread; thread = thread->next) {
            if (++seen > queue->count || !thread->queued ||
                thread->state != THREAD_STATE_READY ||
                thread->priority != priority ||
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
    return true;
}

static void scheduler_remove_queued(kernel_thread_t *thread);

static void scheduler_remove_queued(kernel_thread_t *thread)
{
    thread_ready_queue_t *queue;

    if (!thread || !thread->queued || !thread_priority_valid(thread->priority)) {
        return;
    }

    queue = &ready_queues[thread->priority];
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
        !thread_priority_valid(thread->priority)) {
        return false;
    }

    queue = &ready_queues[thread->priority];
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
    (void)scheduler_validate();
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
                !thread->queued && thread->priority == priority &&
                thread_saved_stack_valid(thread)) {
                return thread;
            }
        }
    }
    if (idle_thread && idle_thread->state == THREAD_STATE_READY &&
        !idle_thread->queued && thread_saved_stack_valid(idle_thread)) {
        return idle_thread;
    }
    return NULL;
}

static bool thread_prepare_context(kernel_thread_t *thread)
{
    uintptr_t stack_top;
    uintptr_t stack_pointer;

    if (!thread || !thread->kernel_stack_base ||
        thread->kernel_stack_size < 7 * sizeof(u64)) {
        return false;
    }

    stack_top = thread->kernel_stack_base + thread->kernel_stack_size;
    stack_top &= ~(uintptr_t)0x0f;
    if (stack_top < thread->kernel_stack_base + 8 * sizeof(u64)) {
        return false;
    }

    /* The trampoline enters with RSP % 16 == 8, as required by the ABI. */
    stack_pointer = stack_top - sizeof(u64);
    *(u64 *)stack_pointer = (u64)(uintptr_t)thread_entry_trampoline;

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
    return true;
}

bool scheduler_init(void)
{
    uintptr_t stack_pointer;

    __asm__ volatile("mov %%rsp, %0" : "=r"(stack_pointer));

    memset(ready_queues, 0, sizeof(ready_queues));
    next_thread_id = 2;
    scheduler_tick_count = 0;
    sleeping_threads = NULL;
    idle_thread = NULL;
    interrupt_switch_requested = false;
    preemption_pending = false;
    memset(&bootstrap_thread, 0, sizeof(bootstrap_thread));
    bootstrap_thread.id = 1;
    bootstrap_thread.state = THREAD_STATE_RUNNING;
    bootstrap_thread.priority = THREAD_PRIORITY_NORMAL;
    bootstrap_thread.default_time_slice =
        thread_default_time_slice(bootstrap_thread.priority);
    bootstrap_thread.remaining_time_slice = bootstrap_thread.default_time_slice;
    bootstrap_thread.saved_stack_pointer = stack_pointer;
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
    thread->priority = priority;
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

    if (!target || target == current_thread ||
        target->state != THREAD_STATE_READY ||
        !thread_saved_stack_valid(target)) {
        return false;
    }

    target_was_queued = target->queued;
    if (target != &bootstrap_thread && !target_was_queued) {
        return false;
    }

    outgoing = current_thread;
    if (!outgoing || outgoing->state != THREAD_STATE_RUNNING ||
        outgoing->queued || !thread_saved_stack_valid(outgoing)) {
        return false;
    }

    scheduler_remove_queued(target);
    outgoing->state = THREAD_STATE_READY;
    if (!scheduler_enqueue(outgoing)) {
        outgoing->state = THREAD_STATE_RUNNING;
        if (target_was_queued) {
            scheduler_enqueue(target);
        }
        return false;
    }
    target->state = THREAD_STATE_RUNNING;
    current_thread = target;
    if (target->preempt_return_rip) {
        thread_context_switch_interrupts_disabled(
            &outgoing->saved_stack_pointer, target->saved_stack_pointer);
    } else {
        thread_context_switch(&outgoing->saved_stack_pointer,
                              target->saved_stack_pointer);
    }
    return true;
}

static bool scheduler_dispatch(scheduler_dispatch_action_t action)
{
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool queue_outgoing;
    bool enable_interrupts;
    bool requeue_current = action == SCHEDULER_DISPATCH_REQUEUE;

    enable_interrupts = interrupt_switch_requested;
    interrupt_switch_requested = false;

    outgoing = current_thread;
    if (!outgoing ||
        (requeue_current && outgoing->state != THREAD_STATE_RUNNING) ||
        (action == SCHEDULER_DISPATCH_BLOCK &&
         outgoing->state != THREAD_STATE_BLOCKED) ||
        (action == SCHEDULER_DISPATCH_TERMINATE &&
         outgoing->state != THREAD_STATE_TERMINATED) ||
        outgoing->queued ||
        !thread_saved_stack_valid(outgoing)) {
        return false;
    }

    /* Bootstrap remains READY but unqueued while it yields to workers. */
    queue_outgoing = requeue_current && outgoing != &bootstrap_thread &&
        outgoing != idle_thread;
    if (requeue_current) {
        outgoing->state = THREAD_STATE_READY;
    }
    if (queue_outgoing && !scheduler_enqueue(outgoing)) {
        outgoing->state = THREAD_STATE_RUNNING;
        return false;
    }

    target = NULL;
    if (action == SCHEDULER_DISPATCH_TERMINATE &&
        outgoing != &bootstrap_thread &&
        bootstrap_thread.state == THREAD_STATE_READY &&
        !bootstrap_thread.queued) {
        /* No queued worker remains; bootstrap is the cooperative fallback. */
        target = &bootstrap_thread;
    }
    if (!target) {
        target = scheduler_select_next();
    }
    if (!target) {
        if (queue_outgoing) {
            scheduler_remove_queued(outgoing);
        }
        if (requeue_current) {
            outgoing->state = THREAD_STATE_RUNNING;
            current_thread = outgoing;
        }
        return false;
    }

    if (target == outgoing) {
        target->state = THREAD_STATE_RUNNING;
        current_thread = target;
        return false;
    }

    target->state = THREAD_STATE_RUNNING;
    current_thread = target;
    if (target->preempt_return_rip) {
        /* Resume a deferred-IRQ trampoline with IF clear until it restores
         * the saved GPRs and RFLAGS. */
        thread_context_switch_interrupts_disabled(
            &outgoing->saved_stack_pointer, target->saved_stack_pointer);
    } else if (enable_interrupts) {
        thread_context_switch_interrupts_enabled(
            &outgoing->saved_stack_pointer, target->saved_stack_pointer);
    } else {
        thread_context_switch(&outgoing->saved_stack_pointer,
                              target->saved_stack_pointer);
    }
    return true;
}

bool scheduler_reschedule(void)
{
    return scheduler_dispatch(SCHEDULER_DISPATCH_REQUEUE);
}

bool scheduler_yield(void)
{
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
    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_BLOCKED || thread->queued) {
        return false;
    }

    sleeping_remove(thread);
    thread->state = THREAD_STATE_READY;
    if (!scheduler_enqueue(thread)) {
        thread->state = THREAD_STATE_BLOCKED;
        return false;
    }
    return true;
}

bool scheduler_block(void)
{
    kernel_thread_t *thread = current_thread;

    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued) {
        return false;
    }

    thread->state = THREAD_STATE_BLOCKED;
    if (!scheduler_dispatch(SCHEDULER_DISPATCH_BLOCK)) {
        thread->state = THREAD_STATE_RUNNING;
        current_thread = thread;
        return false;
    }
    return true;
}

bool scheduler_sleep(u64 ticks)
{
    kernel_thread_t *thread = current_thread;

    if (ticks == 0) {
        return true;
    }
    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued ||
        thread->sleeping) {
        return false;
    }

    thread->wakeup_tick = scheduler_tick_count > (~(u64)0 - ticks) ?
        ~(u64)0 : scheduler_tick_count + ticks;
    thread->next = sleeping_threads;
    sleeping_threads = thread;
    thread->sleeping = true;
    thread->state = THREAD_STATE_BLOCKED;
    if (!scheduler_dispatch(SCHEDULER_DISPATCH_BLOCK)) {
        sleeping_remove(thread);
        thread->state = THREAD_STATE_RUNNING;
        current_thread = thread;
        return false;
    }
    return true;
}

bool scheduler_timer_tick(void)
{
    kernel_thread_t *thread = current_thread;
    kernel_thread_t *sleeping;
    kernel_thread_t *next_sleeping;
    bool woke_thread = false;

    if (scheduler_tick_count != ~(u64)0) {
        scheduler_tick_count++;
    }

    sleeping = sleeping_threads;
    while (sleeping) {
        next_sleeping = sleeping->next;
        if (sleeping->wakeup_tick <= scheduler_tick_count) {
            sleeping_remove(sleeping);
            sleeping->state = THREAD_STATE_READY;
            if (scheduler_enqueue(sleeping)) {
                woke_thread = true;
            } else {
                sleeping->state = THREAD_STATE_BLOCKED;
            }
        }
        sleeping = next_sleeping;
    }

    if (woke_thread && thread &&
        (thread == idle_thread ||
         (idle_thread && thread->priority > THREAD_PRIORITY_HIGH))) {
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

    if (!preemption_pending || !regs || !thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued ||
        !thread_saved_stack_valid(thread)) {
        return false;
    }

    /* The IRQ frame is still live; defer scheduling until iretq has restored it.
     * Keep the original flags so the interrupted context can resume exactly. */
    thread->preempt_return_rip = (uintptr_t)regs->rip;
    thread->preempt_return_rflags = regs->rflags;
    /* Keep the transition interrupt-free until the trampoline is running. */
    regs->rflags &= ~(1ULL << 9);
    regs->rip = (u64)(uintptr_t)&thread_interrupt_return_trampoline;
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
    interrupt_switch_requested = true;
    (void)scheduler_reschedule();
    return return_rip;
}

u64 scheduler_preempt_return_flags(void)
{
    return current_thread ? current_thread->preempt_return_rflags : 0;
}

void scheduler_preempt_context_restored(void)
{
    if (current_thread) {
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
