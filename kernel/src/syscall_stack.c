#include <scheduler.h>

uintptr_t scheduler_kernel_stack_top(void)
{
    kernel_thread_t *thread = thread_current();
    uintptr_t top;

    if (!thread || !thread->kernel_stack_base || !thread->kernel_stack_size) {
        return 0;
    }
    top = thread->kernel_stack_base + thread->kernel_stack_size;
    if (top <= thread->kernel_stack_base) {
        return 0;
    }
    return top;
}
