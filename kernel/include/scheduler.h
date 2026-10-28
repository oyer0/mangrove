/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <fpu.h>
#include <kernel_stack.h>

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

/* Process ownership is distinct from scheduler state. A completed thread
 * remains process-owned until a join/reap path detaches its record. */
typedef enum {
    THREAD_PROCESS_UNATTACHED = 0,
    THREAD_PROCESS_ATTACHED,
    THREAD_PROCESS_EXIT_REQUESTED,
    THREAD_PROCESS_HANDOFF_COMPLETE,
} thread_process_state_t;

typedef enum {
    THREAD_PRIORITY_HIGH = 0,
    THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_BACKGROUND,
} thread_priority_t;

typedef void (*thread_entry_t)(void *argument);

struct process;
struct cpu_local;
struct kernel_mutex;
struct process_user_region;
struct process_thread_record;

/* Initial Ring 3 state belongs to a schedulable thread. Registers saved by
 * an interrupt or syscall remain on that thread's kernel stack; this record
 * is only the unpublished entry context used on its first user transition. */
typedef struct {
    uintptr_t instruction_pointer;
    uintptr_t stack_pointer;
    uintptr_t rdi;
    uintptr_t rsi;
    uintptr_t rdx;
    uintptr_t rcx;
    uintptr_t fs_base;
    struct process_user_region *stack_region;
    struct process_user_region *tls_region;
    bool valid;
} user_execution_context_t;

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
    u64 wakeup_deadline_ms;
    bool sleeping;
    /* A syscall runs on this thread's kernel stack and may deliberately
     * block.  IRQ accounting still runs, but its live syscall frame must not
     * be redirected into the deferred same-ring preemption trampoline. */
    bool syscall_active;
    u32 syscall_nesting;
    /* Points to the active syscall's stack-owned list of VMM user-range
     * leases. It is per thread because a blocked syscall retains its stack. */
    void *syscall_user_access_context;
    /* An entered kernel thread may have a suspended C frame even without a
     * userspace syscall.  External destruction may only discard its initial
     * prepared frame or a self-terminated thread. */
    bool entry_started;
    uintptr_t saved_stack_pointer;
    /* True only after saved_stack_pointer has been produced by a real
     * thread_context_switch save or thread_prepare_context construction. */
    bool saved_context_valid;
    /* ~(u32)0 means that the thread is not owned by a running CPU. */
    u32 running_cpu;
    /* Scheduler timer ticks spent executing this thread. */
    volatile u64 cpu_time_ticks;
    /* Baseline x87/MMX/SSE state.  This area is private to the thread and
     * remains valid when the thread migrates between CPUs. */
    fpu_state_t fpu_state;
    /* FS base is CPU state and must be saved/restored when a Ring 3 thread
     * migrates or blocks. The entry record separately initializes its first
     * value. Kernel code does not use FS-relative storage. */
    uintptr_t user_fs_base;
    user_execution_context_t user_context;
    /* Set while the assembly handoff still executes on this thread's stack.
     * Selection and reclamation must wait for the incoming-stack completion
     * helper. All ownership transitions are protected by scheduler_lock. */
    bool context_switch_pending;
    uintptr_t kernel_stack_base;
    usize kernel_stack_size;
    bool stack_external;
    kernel_stack_t kernel_stack;
    /* Internal scheduler-backed mutex wait linkage.  A thread belongs to at
     * most one mutex wait chain and this is never used as a scheduler list
     * link. */
    struct kernel_mutex *waiting_mutex;
    kernel_thread_t *mutex_wait_next;
    /* Set when mutex unlock wins the race before scheduler_block() changes
     * this thread to BLOCKED.  scheduler_block() consumes it instead of
     * sleeping, closing the register/block lost-wakeup window. */
    bool mutex_wake_pending;
    /* Set when an event arrives after waiter registration but before this
     * thread enters scheduler_block()/scheduler_sleep(). */
    bool event_wake_pending;
    thread_entry_t entry;
    void *entry_argument;
    kernel_thread_t *next;
    kernel_thread_t *previous;
    char name[32];
    struct process *process;
    struct kernel_thread *process_next;
    struct process_thread_record *worker_record;
    thread_process_state_t process_state;
    bool process_initial;
    bool process_reference_held;
    volatile bool process_exit_requested;
    i32 process_exit_status;
    bool process_exit_status_valid;
};

bool scheduler_init(void);
/* Migration-safe snapshot of the executing thread, without taking a lock. */
kernel_thread_t *scheduler_current_thread(void);
kernel_thread_t *thread_current(void);
/* Used by the fatal page-fault path to classify a faulting kernel stack
 * guard without taking the scheduler lock from exception context. */
bool scheduler_current_stack_guard_fault(uintptr_t fault_address);
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
/* Release the execution stack only after the terminated context has been
 * switched away. The scheduler record remains available for join/reap. */
bool thread_release_terminated_stack(kernel_thread_t *thread);
bool thread_switch_to(kernel_thread_t *target);
bool scheduler_enqueue(kernel_thread_t *thread);
kernel_thread_t *scheduler_select_next(void);
bool scheduler_reschedule(void);
bool scheduler_yield(void);
bool scheduler_timer_tick(void);
/* Advances global scheduler time and wakes sleepers.  The BSP's global
 * time source calls this; local LAPIC timers call scheduler_timer_tick(). */
bool scheduler_global_tick(void);
bool scheduler_block(void);
bool scheduler_terminate(void);
bool scheduler_unblock(kernel_thread_t *thread);
/* Internal mutex handoff; caller holds the mutex state lock. */
bool scheduler_mutex_wake(kernel_thread_t *thread);
/* Wake an event waiter without losing a notification before it can park. */
bool scheduler_wake_waiter(kernel_thread_t *thread);
/* Clear the event notification after its durable queue/state was consumed. */
void scheduler_clear_event_wake(kernel_thread_t *thread);
/* Terminates a non-current thread from a trusted kernel lifecycle path. */
bool scheduler_terminate_thread(kernel_thread_t *thread);
/* Atomically terminate an entire stable process-owned list. The caller must
 * retain a process thread-list reader pin through return. No member is changed
 * unless every member is already stopped or safely terminable. */
bool scheduler_terminate_thread_group(kernel_thread_t *head,
                                      usize expected_count);
/* Snapshot whether a thread is currently executing on any CPU. */
bool scheduler_thread_is_running(const kernel_thread_t *thread);
/* True only after self/external termination and the final stack handoff.
 * A merely blocked or preempted exit syscall is not reclaimable. */
bool scheduler_thread_exit_complete(const kernel_thread_t *thread);
/* True only for a privately owned, prepared thread context that has not been
 * queued, run, or published to a process yet. The creator must keep it private
 * until process_attach_thread() succeeds and must enqueue it afterward. */
bool scheduler_thread_process_attachable(const kernel_thread_t *thread);
/* Boot-only case: the already-running bootstrap context becomes PID 1's
 * initial process thread before entering the first userspace image. */
bool scheduler_thread_initial_process_attachable(
    const kernel_thread_t *thread);
/* Return the stable CPU index currently owning a thread, or UINT32_MAX. */
u32 scheduler_thread_running_cpu(const kernel_thread_t *thread);
/* Atomically sample CPU ownership for a stable process thread list.  Returns
 * THREAD_CPU_NONE, one CPU index, or THREAD_CPU_MULTIPLE.  The caller keeps
 * the process list stable while this scheduler-locked snapshot is taken. */
u32 scheduler_process_running_cpu_snapshot(const kernel_thread_t *threads);
thread_state_t scheduler_thread_state_snapshot(const kernel_thread_t *thread);
bool scheduler_sleep(u64 milliseconds);
void scheduler_syscall_enter(void);
void scheduler_syscall_leave(void);
bool scheduler_validate_state(void);
u32 scheduler_ready_count(thread_priority_t priority);
void scheduler_get_stats(scheduler_stats_t *stats);
void scheduler_dump(void);
/* Prepare an AP-owned idle context before that AP is started. */
bool scheduler_prepare_idle_cpu(struct cpu_local *cpu,
                                kernel_stack_t *stack);
/* Enter the current CPU's idle context.  A successful call does not return. */
bool scheduler_start_cpu(void);
/* Assembly context-switch ownership hooks. The post hook runs after incoming
 * RFLAGS are restored and may reclaim resources using SMP TLB shootdowns. */
void scheduler_context_switch_complete(uintptr_t *outgoing_rsp_slot);
void scheduler_context_switch_post_complete(void);
bool scheduler_prepare_preemption(struct cpu_registers *regs);
u64 scheduler_preempt_from_trampoline(void);
u64 scheduler_preempt_return_flags(void);
void scheduler_preempt_context_restored(uintptr_t restored_rsp);
const char *thread_state_name(thread_state_t state);
const char *thread_priority_name(thread_priority_t priority);
