#pragma once

#include <types.h>

#define THREAD_KERNEL_STACK_SIZE  (64U * 1024U)
#define THREAD_TIME_SLICE_HIGH    3ULL
#define THREAD_TIME_SLICE_NORMAL  5ULL
#define THREAD_TIME_SLICE_BACKGROUND 8ULL
/* PIT runs at 1000 Hz: 500 ms and 5 seconds of ready wait. */
#define BACKGROUND_STARVATION_THRESHOLD 500ULL
#define NORMAL_STARVATION_THRESHOLD     5000ULL
/* A BACKGROUND thread rescued to NORMAL must wait another 5 seconds before
 * receiving its one-turn HIGH rescue. */
#define BACKGROUND_HIGH_RESCUE_THRESHOLD NORMAL_STARVATION_THRESHOLD

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

struct process;

typedef struct {
    u64 context_switches;
    u64 timer_preemptions;
    u64 voluntary_yields;
    u64 blocks;
    u64 wakeups;
    u64 sleeping_threads;
    u64 blocked_threads;
    u64 peak_runnable_threads;
    u64 idle_runtime_ticks;
    u64 dispatches[3];
} scheduler_stats_t;

struct cpu_registers;

typedef struct kernel_thread kernel_thread_t;

struct kernel_thread {
    u64 id;
    thread_state_t state;
    thread_priority_t base_priority;
    thread_priority_t effective_priority;
    bool queued;
    bool wakeup_boosted;
    thread_priority_t last_selected_priority;
    bool last_selection_was_wakeup_boost;
    u64 ready_wait_ticks;
    u64 remaining_time_slice;
    u64 default_time_slice;
    uintptr_t preempt_return_rip;
    u64 preempt_return_rflags;
    uintptr_t preempt_return_rsp;
    u64 preempt_return_cs;
    u64 preempt_return_ss;
    bool preempt_from_user;
    u64 wakeup_tick;
    bool sleeping;
    /* A syscall runs on this thread's kernel stack and may deliberately
     * block.  IRQ accounting still runs, but its live syscall frame must not
     * be redirected into the deferred same-ring preemption trampoline. */
    bool syscall_active;
    uintptr_t saved_stack_pointer;
    uintptr_t kernel_stack_base;
    usize kernel_stack_size;
    bool stack_external;
    thread_entry_t entry;
    void *entry_argument;
    kernel_thread_t *next;
    kernel_thread_t *previous;
    char name[32];
    struct process *process;
};

bool scheduler_init(void);
kernel_thread_t *thread_current(void);
/* Top of the current thread's kernel stack for syscall entry. */
uintptr_t scheduler_kernel_stack_top(void);
kernel_thread_t *scheduler_idle_thread(void);
kernel_thread_t *thread_create(const char *name, thread_entry_t entry,
                               void *argument);
kernel_thread_t *thread_create_with_priority(const char *name,
                                             thread_entry_t entry,
                                             void *argument,
                                             thread_priority_t priority);
kernel_thread_t *thread_create_suspended(const char *name, thread_entry_t entry,
                                         void *argument);
kernel_thread_t *thread_create_suspended_with_priority(const char *name,
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
bool scheduler_terminate(void);
bool scheduler_unblock(kernel_thread_t *thread);
bool scheduler_sleep(u64 ticks);
void scheduler_syscall_enter(void);
void scheduler_syscall_leave(void);
bool scheduler_validate_state(void);
u32 scheduler_ready_count(thread_priority_t priority);
void scheduler_get_stats(scheduler_stats_t *stats);
void scheduler_dump(void);
bool scheduler_prepare_preemption(struct cpu_registers *regs);
u64 scheduler_preempt_from_trampoline(void);
u64 scheduler_preempt_return_flags(void);
void scheduler_preempt_context_restored(uintptr_t restored_rsp);
const char *thread_state_name(thread_state_t state);
const char *thread_priority_name(thread_priority_t priority);
