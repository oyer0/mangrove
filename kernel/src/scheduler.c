#include <scheduler.h>
#include <heap.h>
#include <string.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

extern char __stack_bottom[];
extern char __stack_top[];

static kernel_thread_t bootstrap_thread;
static kernel_thread_t *current_thread;
static u64 next_thread_id = 2;

typedef struct {
    kernel_thread_t *head;
    kernel_thread_t *tail;
    u32 count;
} thread_ready_queue_t;

static thread_ready_queue_t ready_queues[3];

extern void thread_context_switch(uintptr_t *outgoing_stack_pointer,
                                  uintptr_t incoming_stack_pointer);

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
    kernel_thread_t *target;

    if (thread && thread->entry) {
        thread->entry(thread->entry_argument);
    }

    if (thread) {
        thread->state = THREAD_STATE_TERMINATED;
    }

    target = scheduler_select_next();
    if (!target && thread != &bootstrap_thread &&
        bootstrap_thread.state == THREAD_STATE_READY &&
        !bootstrap_thread.queued) {
        /* Phase 13.4 fallback for a worker created by the bootstrap thread. */
        target = &bootstrap_thread;
    }

    if (target) {
        target->state = THREAD_STATE_RUNNING;
        current_thread = target;
        thread_context_switch(&thread->saved_stack_pointer,
                              target->saved_stack_pointer);
    }

    for (;;) {
        __asm__ volatile("cli; hlt");
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
    return true;
}

kernel_thread_t *scheduler_select_next(void)
{
    thread_ready_queue_t *queue;
    kernel_thread_t *thread;
    thread_priority_t priority;

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
    memset(&bootstrap_thread, 0, sizeof(bootstrap_thread));
    bootstrap_thread.id = 1;
    bootstrap_thread.state = THREAD_STATE_RUNNING;
    bootstrap_thread.priority = THREAD_PRIORITY_NORMAL;
    bootstrap_thread.saved_stack_pointer = stack_pointer;
    bootstrap_thread.kernel_stack_base = (uintptr_t)__stack_bottom;
    bootstrap_thread.kernel_stack_size =
        (usize)((uintptr_t)__stack_top - (uintptr_t)__stack_bottom);
    strncpy(bootstrap_thread.name, "bootstrap", sizeof(bootstrap_thread.name) - 1);
    bootstrap_thread.name[sizeof(bootstrap_thread.name) - 1] = '\0';

    current_thread = &bootstrap_thread;
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
    if (!thread || thread == &bootstrap_thread ||
        thread == current_thread ||
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
    thread_context_switch(&outgoing->saved_stack_pointer,
                          target->saved_stack_pointer);
    return true;
}

bool scheduler_reschedule(void)
{
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool queue_outgoing;

    outgoing = current_thread;
    if (!outgoing || outgoing->state != THREAD_STATE_RUNNING ||
        !thread_saved_stack_valid(outgoing)) {
        return false;
    }

    /* Bootstrap remains READY but unqueued while it yields to workers. */
    queue_outgoing = outgoing != &bootstrap_thread;
    outgoing->state = THREAD_STATE_READY;
    if (queue_outgoing && !scheduler_enqueue(outgoing)) {
        outgoing->state = THREAD_STATE_RUNNING;
        return false;
    }

    target = scheduler_select_next();
    if (!target) {
        if (queue_outgoing) {
            scheduler_remove_queued(outgoing);
        }
        outgoing->state = THREAD_STATE_RUNNING;
        current_thread = outgoing;
        return false;
    }

    if (target == outgoing) {
        target->state = THREAD_STATE_RUNNING;
        current_thread = target;
        return false;
    }

    target->state = THREAD_STATE_RUNNING;
    current_thread = target;
    thread_context_switch(&outgoing->saved_stack_pointer,
                          target->saved_stack_pointer);
    return true;
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
