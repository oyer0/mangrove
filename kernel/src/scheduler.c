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

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
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

    memset(&bootstrap_thread, 0, sizeof(bootstrap_thread));
    bootstrap_thread.id = 1;
    bootstrap_thread.state = THREAD_STATE_RUNNING;
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

kernel_thread_t *thread_create(const char *name, thread_entry_t entry,
                               void *argument)
{
    kernel_thread_t *thread;

    if (!name || !entry || next_thread_id == 0) {
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

bool thread_destroy(kernel_thread_t *thread)
{
    if (!thread || thread == &bootstrap_thread ||
        thread == current_thread || thread->state != THREAD_STATE_READY) {
        return false;
    }

    kfree((void *)thread->kernel_stack_base);
    kfree(thread);
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
