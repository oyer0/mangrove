#pragma once

#include <types.h>

#define THREAD_KERNEL_STACK_SIZE  (16U * 1024U)
#define THREAD_TIME_SLICE_HIGH    3ULL
#define THREAD_TIME_SLICE_NORMAL  5ULL
#define THREAD_TIME_SLICE_BACKGROUND 8ULL
/* PIT runs at 1000 Hz: 500 ms and 5 seconds of ready wait. */
#define BACKGROUND_STARVATION_THRESHOLD 500ULL
#define NORMAL_STARVATION_THRESHOLD     5000ULL
/* A BACKGROUND thread rescued to NORMAL must wait another 5 seconds before
 * receiving its one-turn HIGH rescue. */
#define BACKGROUND_HIGH_RESCUE_THRESHOLD NORMAL_STARVATION_THRESHOLD

#define SCHEDULER_SLEEP_TRACE_CREATED    1U
#define SCHEDULER_SLEEP_TRACE_SELECTED   2U
#define SCHEDULER_SLEEP_TRACE_ENTERED    3U
#define SCHEDULER_SLEEP_TRACE_INSERTED   4U
#define SCHEDULER_SLEEP_TRACE_WAKE_DUE   5U
#define SCHEDULER_SLEEP_TRACE_REQUEUED   6U
#define SCHEDULER_SLEEP_TRACE_IDLE       7U
#define SCHEDULER_SLEEP_TRACE_COMPLETED  8U
#define SCHEDULER_SLEEP_TRACE_BOOTSTRAP  9U
#define SCHEDULER_SLEEP_TRACE_REMOVED    10U
#define SCHEDULER_SLEEP_TRACE_REQUEUE    11U
#define SCHEDULER_SLEEP_TRACE_REJECTED   12U

#define SCHEDULER_SLEEP_STATUS_ENTERED        (1U << 0)
#define SCHEDULER_SLEEP_STATUS_INSERTED       (1U << 1)
#define SCHEDULER_SLEEP_STATUS_DEADLINE       (1U << 2)
#define SCHEDULER_SLEEP_STATUS_REMOVED        (1U << 3)
#define SCHEDULER_SLEEP_STATUS_REQUEUE_TRIED  (1U << 4)
#define SCHEDULER_SLEEP_STATUS_REQUEUE_OK     (1U << 5)
#define SCHEDULER_SLEEP_STATUS_SELECTED       (1U << 6)
#define SCHEDULER_SLEEP_STATUS_COMPLETED      (1U << 7)

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
    bool timer_measure_pending;
    u64 wakeup_tick;
    bool sleeping;
    uintptr_t saved_stack_pointer;
    uintptr_t kernel_stack_base;
    usize kernel_stack_size;
    bool stack_external;
    thread_entry_t entry;
    void *entry_argument;
    kernel_thread_t *next;
    kernel_thread_t *previous;
    char name[32];
};

bool scheduler_init(void);
kernel_thread_t *thread_current(void);
kernel_thread_t *scheduler_idle_thread(void);
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
bool scheduler_validate_state(void);
u32 scheduler_ready_count(thread_priority_t priority);
void scheduler_get_stats(scheduler_stats_t *stats);
void scheduler_dump(void);
void scheduler_debug_timer_trace(bool enabled);
bool scheduler_debug_timer_trace_active(void);
bool scheduler_debug_timer_stack_oob(void);
bool scheduler_debug_timer_rsp_ok(void);
bool scheduler_debug_timer_rsp_measured(void);
bool scheduler_debug_timer_rsp_mismatch(void);
void scheduler_debug_rr_trace_start(kernel_thread_t *first,
                                    kernel_thread_t *second);
void scheduler_debug_rr_trace_marker(char marker);
void scheduler_debug_rr_trace_dump(void);
void scheduler_debug_rr_trace_stop(void);
void scheduler_debug_sleep_trace_start(kernel_thread_t *first,
                                       kernel_thread_t *second);
void scheduler_debug_sleep_trace_event(u32 event, kernel_thread_t *thread);
void scheduler_debug_sleep_trace_dump(void);
void scheduler_debug_sleep_trace_stop(void);
bool scheduler_debug_sleep_wakeup_seen(kernel_thread_t *thread);
u32 scheduler_debug_sleep_status(kernel_thread_t *thread);
u32 scheduler_debug_sleep_requeue_reject(kernel_thread_t *thread);
u64 scheduler_debug_sleep_assigned_wake(kernel_thread_t *thread);
void scheduler_debug_fairness_trace_start(kernel_thread_t *high,
                                          kernel_thread_t *normal,
                                          kernel_thread_t *background);
void scheduler_debug_fairness_trace_dump(void);
void scheduler_debug_fairness_trace_stop(void);
u32 scheduler_debug_fairness_promotions_to(kernel_thread_t *thread,
                                           thread_priority_t priority);
void scheduler_debug_arm_bootstrap_timeout(u64 ticks);
void scheduler_debug_disarm_bootstrap_timeout(void);
bool scheduler_debug_bootstrap_timeout_fired(void);
void scheduler_debug_bootstrap_timeout_dump(void);
bool scheduler_debug_timer_measure_pending(kernel_thread_t *thread);
void scheduler_debug_timer_measure_resume(kernel_thread_t *thread,
                                          uintptr_t actual_rsp);
bool scheduler_prepare_preemption(struct cpu_registers *regs);
u64 scheduler_preempt_from_trampoline(void);
u64 scheduler_preempt_return_flags(void);
void scheduler_preempt_context_restored(uintptr_t restored_rsp);
const char *thread_state_name(thread_state_t state);
const char *thread_priority_name(thread_priority_t priority);
