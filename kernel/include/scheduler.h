#pragma once

#include <types.h>

#define THREAD_KERNEL_STACK_SIZE  (16U * 1024U)

typedef enum {
    THREAD_STATE_RUNNING = 0,
    THREAD_STATE_READY,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_TERMINATED,
} thread_state_t;

typedef void (*thread_entry_t)(void *argument);

typedef struct kernel_thread kernel_thread_t;

struct kernel_thread {
    u64 id;
    thread_state_t state;
    uintptr_t saved_stack_pointer;
    uintptr_t kernel_stack_base;
    usize kernel_stack_size;
    thread_entry_t entry;
    void *entry_argument;
    kernel_thread_t *next;
    char name[32];
};

bool scheduler_init(void);
kernel_thread_t *thread_current(void);
kernel_thread_t *thread_create(const char *name, thread_entry_t entry,
                               void *argument);
bool thread_destroy(kernel_thread_t *thread);
const char *thread_state_name(thread_state_t state);
