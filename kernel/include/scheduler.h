#pragma once

#include <types.h>

#define THREAD_KERNEL_STACK_SIZE  (16U * 1024U)
#define THREAD_TIME_SLICE_HIGH    3ULL
#define THREAD_TIME_SLICE_NORMAL  5ULL
#define THREAD_TIME_SLICE_BACKGROUND 8ULL

typedef enum {
    THREAD_STATE_RUNNING = 0,
    THREAD_STATE_READY,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_TERMINATED,
} thread_state_t;

typedef enum {
    THREAD_PRIORITY_HIGH = 0,
    THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_BACKGROUND,
} thread_priority_t;

typedef void (*thread_entry_t)(void *argument);

struct cpu_registers;

typedef struct kernel_thread kernel_thread_t;

struct kernel_thread {
    u64 id;
    thread_state_t state;
    thread_priority_t priority;
    bool queued;
    u64 remaining_time_slice;
    u64 default_time_slice;
    uintptr_t preempt_return_rip;
    u64 preempt_return_rflags;
    u64 wakeup_tick;
    bool sleeping;
    uintptr_t saved_stack_pointer;
    uintptr_t kernel_stack_base;
    usize kernel_stack_size;
    thread_entry_t entry;
    void *entry_argument;
    kernel_thread_t *next;
    kernel_thread_t *previous;
    char name[32];
};

bool scheduler_init(void);
kernel_thread_t *thread_current(void);
kernel_thread_t *thread_create(const char *name, thread_entry_t entry,
                               void *argument);
kernel_thread_t *thread_create_with_priority(const char *name,
                                             thread_entry_t entry,
                                             void *argument,
                                             thread_priority_t priority);
bool thread_destroy(kernel_thread_t *thread);
bool thread_switch_to(kernel_thread_t *target);
bool scheduler_enqueue(kernel_thread_t *thread);
kernel_thread_t *scheduler_select_next(void);
bool scheduler_reschedule(void);
bool scheduler_yield(void);
bool scheduler_timer_tick(void);
bool scheduler_block(void);
bool scheduler_unblock(kernel_thread_t *thread);
bool scheduler_sleep(u64 ticks);
bool scheduler_prepare_preemption(struct cpu_registers *regs);
u64 scheduler_preempt_from_trampoline(void);
u64 scheduler_preempt_return_flags(void);
void scheduler_preempt_context_restored(void);
const char *thread_state_name(thread_state_t state);
const char *thread_priority_name(thread_priority_t priority);
