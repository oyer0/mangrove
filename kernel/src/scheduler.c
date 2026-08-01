#include <scheduler.h>
#include <idt.h>
#include <heap.h>
#include <string.h>
#include <kprint.h>

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
static bool scheduler_timer_trace_enabled;
static bool scheduler_timer_rsp_mismatch;
static bool scheduler_timer_stack_oob;
static bool scheduler_timer_stack_drift_reported;
static bool scheduler_timer_measure_done;

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

#define SCHEDULER_RR_TRACE_CAPACITY 48U
typedef struct {
    char event;
    char marker;
    u32 reason;
    u64 thread_id;
    u64 head_id;
    u64 tail_id;
    u64 selected_id;
} scheduler_rr_trace_event_t;

static bool scheduler_rr_trace_enabled;
static u64 scheduler_rr_trace_first;
static u64 scheduler_rr_trace_second;
static u32 scheduler_rr_trace_count;
static scheduler_rr_trace_event_t
    scheduler_rr_trace_events[SCHEDULER_RR_TRACE_CAPACITY];
static u32 scheduler_rr_dispatch_reason;

#define SCHEDULER_SLEEP_TRACE_CAPACITY 48U
typedef struct {
    u32 event;
    u64 thread_id;
    u64 tick;
    u64 wake_tick;
    u64 sleeping_count;
    u64 idle_dispatch_count;
    u32 state;
    u32 requeue_reject;
    bool queued;
} scheduler_sleep_trace_event_t;

static bool scheduler_sleep_trace_enabled;
static u64 scheduler_sleep_trace_first;
static u64 scheduler_sleep_trace_second;
static u32 scheduler_sleep_trace_count;
static bool scheduler_sleep_trace_first_woke;
static bool scheduler_sleep_trace_second_woke;
static u32 scheduler_sleep_trace_first_status;
static u32 scheduler_sleep_trace_second_status;
static u32 scheduler_sleep_trace_first_reject;
static u32 scheduler_sleep_trace_second_reject;
static u64 scheduler_sleep_trace_first_wake;
static u64 scheduler_sleep_trace_second_wake;
static scheduler_sleep_trace_event_t
    scheduler_sleep_trace_events[SCHEDULER_SLEEP_TRACE_CAPACITY];

static bool scheduler_fairness_trace_enabled;
static u64 scheduler_fairness_trace_high;
static u64 scheduler_fairness_trace_normal;
static u64 scheduler_fairness_trace_background;
static u64 scheduler_fairness_first_dispatch;
static u64 scheduler_fairness_first_dispatch_tick;
static u64 scheduler_fairness_first_promotion;
static u64 scheduler_fairness_first_promotion_tick;
static thread_priority_t scheduler_fairness_first_promotion_from;
static thread_priority_t scheduler_fairness_first_promotion_to;
static u32 scheduler_fairness_normal_high_promotions;
static u32 scheduler_fairness_background_normal_promotions;
static u32 scheduler_fairness_background_high_promotions;

static bool scheduler_rr_trace_thread(const kernel_thread_t *thread)
{
    return scheduler_rr_trace_enabled && thread &&
        (thread->id == scheduler_rr_trace_first ||
         thread->id == scheduler_rr_trace_second);
}

static void scheduler_rr_trace_record(char event, char marker, u32 reason,
                                      kernel_thread_t *thread,
                                      kernel_thread_t *head,
                                      kernel_thread_t *tail,
                                      kernel_thread_t *selected)
{
    scheduler_rr_trace_event_t *record;

    if (!scheduler_rr_trace_enabled ||
        scheduler_rr_trace_count >= SCHEDULER_RR_TRACE_CAPACITY) {
        return;
    }
    record = &scheduler_rr_trace_events[scheduler_rr_trace_count++];
    record->event = event;
    record->marker = marker;
    record->reason = reason;
    record->thread_id = thread ? thread->id : 0;
    record->head_id = head ? head->id : 0;
    record->tail_id = tail ? tail->id : 0;
    record->selected_id = selected ? selected->id : 0;
}

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
static bool scheduler_debug_timeout_armed;
static bool scheduler_debug_timeout_fired;
static bool scheduler_debug_timeout_force_bootstrap;
static u64 scheduler_debug_timeout_tick;
static u64 scheduler_debug_timeout_start_tick;
static u64 scheduler_debug_timeout_reached_tick;
static u64 scheduler_debug_timeout_reached_current_id;
static u64 scheduler_debug_timeout_selected_id;
static u32 scheduler_debug_timeout_arm_bootstrap_state;
static bool scheduler_debug_timeout_arm_bootstrap_queued;
static u32 scheduler_debug_timeout_reached_bootstrap_state;
static bool scheduler_debug_timeout_reached_bootstrap_queued;
static bool scheduler_debug_timeout_enqueue_ok;
static u32 scheduler_debug_timeout_enqueue_reject;
static bool scheduler_debug_timeout_left_idle;

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

    if (!thread || !thread->saved_stack_pointer) {
        return false;
    }

    /* Bootstrap runs on the stack established by the boot environment.  It
     * does not own a scheduler-allocated stack whose bounds we can verify.
     * Created kernel threads take the strict range-checked path below. */
    if (thread->stack_external) {
        return true;
    }

    if (!thread->kernel_stack_base || !thread->kernel_stack_size) {
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

void scheduler_debug_timer_trace(bool enabled)
{
    scheduler_timer_trace_enabled = enabled;
    if (enabled) {
        scheduler_timer_rsp_mismatch = false;
        scheduler_timer_stack_oob = false;
        scheduler_timer_stack_drift_reported = false;
        scheduler_timer_measure_done = false;
    }
}

bool scheduler_debug_timer_trace_active(void)
{
    return scheduler_timer_trace_enabled;
}

bool scheduler_debug_timer_stack_oob(void)
{
    return scheduler_timer_stack_oob;
}

bool scheduler_debug_timer_rsp_ok(void)
{
    return scheduler_timer_measure_done && !scheduler_timer_rsp_mismatch;
}

bool scheduler_debug_timer_rsp_measured(void)
{
    return scheduler_timer_measure_done;
}

bool scheduler_debug_timer_rsp_mismatch(void)
{
    return scheduler_timer_rsp_mismatch;
}

void scheduler_debug_rr_trace_start(kernel_thread_t *first,
                                    kernel_thread_t *second)
{
    thread_ready_queue_t *queue;

    scheduler_rr_trace_first = first ? first->id : 0;
    scheduler_rr_trace_second = second ? second->id : 0;
    scheduler_rr_trace_count = 0;
    scheduler_rr_trace_enabled = first && second;
    if (!scheduler_rr_trace_enabled) return;
    queue = &ready_queues[first->effective_priority];
    scheduler_rr_trace_record('Q', 0, 0, NULL, queue->head, queue->tail, NULL);
}

void scheduler_debug_rr_trace_marker(char marker)
{
    if (scheduler_rr_trace_thread(current_thread)) {
        scheduler_rr_trace_record('M', marker, 0, current_thread,
                                  NULL, NULL, NULL);
    }
}

void scheduler_debug_rr_trace_dump(void)
{
    u32 i;
    scheduler_rr_trace_event_t *record;

    for (i = 0; i < scheduler_rr_trace_count; i++) {
        record = &scheduler_rr_trace_events[i];
        kprint("[rr-trace] event=%c marker=%c thread=%llu reason=%u "
               "head=%llu tail=%llu selected=%llu\n",
               record->event, record->marker ? record->marker : '-',
               record->thread_id, record->reason, record->head_id,
               record->tail_id, record->selected_id);
    }
}

void scheduler_debug_rr_trace_stop(void)
{
    scheduler_rr_trace_enabled = false;
}

void scheduler_debug_sleep_trace_start(kernel_thread_t *first,
                                       kernel_thread_t *second)
{
    scheduler_sleep_trace_first = first ? first->id : 0;
    scheduler_sleep_trace_second = second ? second->id : 0;
    scheduler_sleep_trace_count = 0;
    scheduler_sleep_trace_first_woke = false;
    scheduler_sleep_trace_second_woke = false;
    scheduler_sleep_trace_first_status = 0;
    scheduler_sleep_trace_second_status = 0;
    scheduler_sleep_trace_first_reject = 0;
    scheduler_sleep_trace_second_reject = 0;
    scheduler_sleep_trace_first_wake = 0;
    scheduler_sleep_trace_second_wake = 0;
    scheduler_sleep_trace_enabled = first && second;
}

void scheduler_debug_sleep_trace_event(u32 event, kernel_thread_t *thread)
{
    scheduler_sleep_trace_event_t *record;
    u32 bit = 0;

    if (!scheduler_sleep_trace_enabled ||
        scheduler_sleep_trace_count >= SCHEDULER_SLEEP_TRACE_CAPACITY) {
        return;
    }
    if (thread && thread != idle_thread && thread != &bootstrap_thread &&
        thread->id != scheduler_sleep_trace_first &&
        thread->id != scheduler_sleep_trace_second) {
        return;
    }
    if (event == SCHEDULER_SLEEP_TRACE_REQUEUED && thread) {
        if (thread->id == scheduler_sleep_trace_first) {
            scheduler_sleep_trace_first_woke = true;
        } else if (thread->id == scheduler_sleep_trace_second) {
            scheduler_sleep_trace_second_woke = true;
        }
    }
    switch (event) {
        case SCHEDULER_SLEEP_TRACE_SELECTED:
            if (thread &&
                ((thread->id == scheduler_sleep_trace_first &&
                  (scheduler_sleep_trace_first_status &
                   SCHEDULER_SLEEP_STATUS_REQUEUE_OK)) ||
                 (thread->id == scheduler_sleep_trace_second &&
                  (scheduler_sleep_trace_second_status &
                   SCHEDULER_SLEEP_STATUS_REQUEUE_OK)))) {
                bit = SCHEDULER_SLEEP_STATUS_SELECTED;
            }
            break;
        case SCHEDULER_SLEEP_TRACE_ENTERED:
            bit = SCHEDULER_SLEEP_STATUS_ENTERED;
            break;
        case SCHEDULER_SLEEP_TRACE_INSERTED:
            bit = SCHEDULER_SLEEP_STATUS_INSERTED;
            if (thread->id == scheduler_sleep_trace_first) {
                scheduler_sleep_trace_first_wake = thread->wakeup_tick;
            } else if (thread->id == scheduler_sleep_trace_second) {
                scheduler_sleep_trace_second_wake = thread->wakeup_tick;
            }
            break;
        case SCHEDULER_SLEEP_TRACE_WAKE_DUE:
            bit = SCHEDULER_SLEEP_STATUS_DEADLINE;
            break;
        case SCHEDULER_SLEEP_TRACE_REMOVED:
            bit = SCHEDULER_SLEEP_STATUS_REMOVED;
            break;
        case SCHEDULER_SLEEP_TRACE_REQUEUE:
            bit = SCHEDULER_SLEEP_STATUS_REQUEUE_TRIED;
            break;
        case SCHEDULER_SLEEP_TRACE_REQUEUED:
            bit = SCHEDULER_SLEEP_STATUS_REQUEUE_OK;
            break;
        case SCHEDULER_SLEEP_TRACE_COMPLETED:
            bit = SCHEDULER_SLEEP_STATUS_COMPLETED;
            break;
        default:
            break;
    }
    if (thread && bit) {
        if (thread->id == scheduler_sleep_trace_first) {
            scheduler_sleep_trace_first_status |= bit;
        } else if (thread->id == scheduler_sleep_trace_second) {
            scheduler_sleep_trace_second_status |= bit;
        }
    }
    record = &scheduler_sleep_trace_events[scheduler_sleep_trace_count++];
    record->event = event;
    record->thread_id = thread ? thread->id : 0;
    record->tick = scheduler_tick_count;
    record->wake_tick = thread ? thread->wakeup_tick : 0;
    record->sleeping_count = scheduler_stats.sleeping_threads;
    record->idle_dispatch_count =
        scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND];
    record->state = thread ? (u32)thread->state : 0xffffffffU;
    record->requeue_reject = thread ?
        scheduler_debug_sleep_requeue_reject(thread) : 0;
    record->queued = thread ? thread->queued : false;
}

void scheduler_debug_sleep_trace_dump(void)
{
    static const char *names[] = {
        "invalid", "created", "selected", "sleep-enter", "sleep-list",
        "wake-due", "requeued", "idle-selected", "completed", "bootstrap",
        "removed", "requeue-attempt", "requeue-rejected"
    };
    u32 i;
    scheduler_sleep_trace_event_t *record;

    for (i = 0; i < scheduler_sleep_trace_count; i++) {
        record = &scheduler_sleep_trace_events[i];
        kprint("[sleep-trace] event=%s thread=%llu tick=%llu wake=%llu "
               "sleeping=%llu state=%u queued=%u reject=%u "
               "idle_dispatch=%llu\n",
               record->event < 13 ? names[record->event] : "unknown",
               record->thread_id, record->tick, record->wake_tick,
               record->sleeping_count, record->state,
               record->queued ? 1U : 0U, record->requeue_reject,
               record->idle_dispatch_count);
    }
}

void scheduler_debug_sleep_trace_stop(void)
{
    scheduler_sleep_trace_enabled = false;
}

bool scheduler_debug_sleep_wakeup_seen(kernel_thread_t *thread)
{
    if (!thread) return false;
    if (thread->id == scheduler_sleep_trace_first) {
        return scheduler_sleep_trace_first_woke;
    }
    if (thread->id == scheduler_sleep_trace_second) {
        return scheduler_sleep_trace_second_woke;
    }
    return false;
}

u32 scheduler_debug_sleep_status(kernel_thread_t *thread)
{
    if (!thread) return 0;
    if (thread->id == scheduler_sleep_trace_first) {
        return scheduler_sleep_trace_first_status;
    }
    if (thread->id == scheduler_sleep_trace_second) {
        return scheduler_sleep_trace_second_status;
    }
    return 0;
}

u32 scheduler_debug_sleep_requeue_reject(kernel_thread_t *thread)
{
    if (!thread) return 0;
    if (thread->id == scheduler_sleep_trace_first) {
        return scheduler_sleep_trace_first_reject;
    }
    if (thread->id == scheduler_sleep_trace_second) {
        return scheduler_sleep_trace_second_reject;
    }
    return 0;
}

u64 scheduler_debug_sleep_assigned_wake(kernel_thread_t *thread)
{
    if (!thread) return 0;
    if (thread->id == scheduler_sleep_trace_first) {
        return scheduler_sleep_trace_first_wake;
    }
    if (thread->id == scheduler_sleep_trace_second) {
        return scheduler_sleep_trace_second_wake;
    }
    return 0;
}

void scheduler_debug_fairness_trace_start(kernel_thread_t *high,
                                          kernel_thread_t *normal,
                                          kernel_thread_t *background)
{
    scheduler_fairness_trace_high = high ? high->id : 0;
    scheduler_fairness_trace_normal = normal ? normal->id : 0;
    scheduler_fairness_trace_background = background ? background->id : 0;
    scheduler_fairness_first_dispatch = 0;
    scheduler_fairness_first_dispatch_tick = 0;
    scheduler_fairness_first_promotion = 0;
    scheduler_fairness_first_promotion_tick = 0;
    scheduler_fairness_first_promotion_from = THREAD_PRIORITY_BACKGROUND;
    scheduler_fairness_first_promotion_to = THREAD_PRIORITY_BACKGROUND;
    scheduler_fairness_normal_high_promotions = 0;
    scheduler_fairness_background_normal_promotions = 0;
    scheduler_fairness_background_high_promotions = 0;
    scheduler_fairness_trace_enabled = high && normal && background;
}

void scheduler_debug_fairness_trace_dump(void)
{
    kprint("[fair-trace] workers high=%llu normal=%llu background=%llu "
           "first_dispatch=%llu@%llu\n",
           scheduler_fairness_trace_high, scheduler_fairness_trace_normal,
           scheduler_fairness_trace_background,
           scheduler_fairness_first_dispatch,
           scheduler_fairness_first_dispatch_tick);
    kprint("[fair-trace] first_promotion=%llu@%llu %s->%s "
           "promotions normal-high=%u background-normal=%u "
           "background-high=%u\n",
           scheduler_fairness_first_promotion,
           scheduler_fairness_first_promotion_tick,
           thread_priority_name(scheduler_fairness_first_promotion_from),
           thread_priority_name(scheduler_fairness_first_promotion_to),
           scheduler_fairness_normal_high_promotions,
           scheduler_fairness_background_normal_promotions,
           scheduler_fairness_background_high_promotions);
}

void scheduler_debug_fairness_trace_stop(void)
{
    scheduler_fairness_trace_enabled = false;
}

u32 scheduler_debug_fairness_promotions_to(kernel_thread_t *thread,
                                           thread_priority_t priority)
{
    if (!thread) return 0;
    if (thread->id == scheduler_fairness_trace_normal &&
        priority == THREAD_PRIORITY_HIGH) {
        return scheduler_fairness_normal_high_promotions;
    }
    if (thread->id == scheduler_fairness_trace_background) {
        if (priority == THREAD_PRIORITY_NORMAL) {
            return scheduler_fairness_background_normal_promotions;
        }
        if (priority == THREAD_PRIORITY_HIGH) {
            return scheduler_fairness_background_high_promotions;
        }
    }
    return 0;
}

static void scheduler_debug_fairness_dispatch(kernel_thread_t *thread)
{
    if (!scheduler_fairness_trace_enabled ||
        scheduler_fairness_first_dispatch || !thread) {
        return;
    }
    if (thread->id == scheduler_fairness_trace_high ||
        thread->id == scheduler_fairness_trace_normal ||
        thread->id == scheduler_fairness_trace_background) {
        scheduler_fairness_first_dispatch = thread->id;
        scheduler_fairness_first_dispatch_tick = scheduler_tick_count;
    }
}

static void scheduler_debug_fairness_promotion(
    kernel_thread_t *thread, thread_priority_t from,
    thread_priority_t to)
{
    if (!scheduler_fairness_trace_enabled || !thread) return;
    if (thread->id == scheduler_fairness_trace_normal) {
        if (to == THREAD_PRIORITY_HIGH) {
            scheduler_fairness_normal_high_promotions++;
        }
    } else if (thread->id == scheduler_fairness_trace_background) {
        if (to == THREAD_PRIORITY_NORMAL) {
            scheduler_fairness_background_normal_promotions++;
        } else if (to == THREAD_PRIORITY_HIGH) {
            scheduler_fairness_background_high_promotions++;
        }
    } else {
        return;
    }
    if (!scheduler_fairness_first_promotion) {
        scheduler_fairness_first_promotion = thread->id;
        scheduler_fairness_first_promotion_tick = scheduler_tick_count;
        scheduler_fairness_first_promotion_from = from;
        scheduler_fairness_first_promotion_to = to;
    }
}

void scheduler_debug_arm_bootstrap_timeout(u64 ticks)
{
    scheduler_debug_timeout_fired = false;
    scheduler_debug_timeout_force_bootstrap = false;
    scheduler_debug_timeout_start_tick = scheduler_tick_count;
    scheduler_debug_timeout_tick = scheduler_tick_count > (~(u64)0 - ticks) ?
        ~(u64)0 : scheduler_tick_count + ticks;
    scheduler_debug_timeout_reached_tick = 0;
    scheduler_debug_timeout_reached_current_id = 0;
    scheduler_debug_timeout_selected_id = 0;
    scheduler_debug_timeout_arm_bootstrap_state = (u32)bootstrap_thread.state;
    scheduler_debug_timeout_arm_bootstrap_queued = bootstrap_thread.queued;
    scheduler_debug_timeout_reached_bootstrap_state = 0xffffffffU;
    scheduler_debug_timeout_reached_bootstrap_queued = false;
    scheduler_debug_timeout_enqueue_ok = false;
    scheduler_debug_timeout_enqueue_reject = 0;
    scheduler_debug_timeout_left_idle = false;
    scheduler_debug_timeout_armed = ticks != 0;
    kprint("[sleep-watchdog] armed start=%llu deadline=%llu bootstrap=%llu "
           "state=%u queued=%u\n",
           scheduler_debug_timeout_start_tick, scheduler_debug_timeout_tick,
           bootstrap_thread.id, scheduler_debug_timeout_arm_bootstrap_state,
           scheduler_debug_timeout_arm_bootstrap_queued ? 1U : 0U);
}

void scheduler_debug_disarm_bootstrap_timeout(void)
{
    scheduler_debug_timeout_armed = false;
}

bool scheduler_debug_bootstrap_timeout_fired(void)
{
    return scheduler_debug_timeout_fired;
}

void scheduler_debug_bootstrap_timeout_dump(void)
{
    kprint("[sleep-watchdog] armed start=%llu deadline=%llu bootstrap=%llu "
           "state=%u queued=%u\n",
           scheduler_debug_timeout_start_tick, scheduler_debug_timeout_tick,
           bootstrap_thread.id, scheduler_debug_timeout_arm_bootstrap_state,
           scheduler_debug_timeout_arm_bootstrap_queued ? 1U : 0U);
    kprint("[sleep-watchdog] reached=%u tick=%llu current=%llu ge_deadline=%u "
           "bootstrap_state=%u queued=%u enqueue=%u reject=%u "
           "selected=%llu left_idle=%u\n",
           scheduler_debug_timeout_fired ? 1U : 0U,
           scheduler_debug_timeout_reached_tick,
           scheduler_debug_timeout_reached_current_id,
           scheduler_debug_timeout_reached_tick >= scheduler_debug_timeout_tick ?
               1U : 0U,
           scheduler_debug_timeout_reached_bootstrap_state,
           scheduler_debug_timeout_reached_bootstrap_queued ? 1U : 0U,
           scheduler_debug_timeout_enqueue_ok ? 1U : 0U,
           scheduler_debug_timeout_enqueue_reject,
           scheduler_debug_timeout_selected_id,
           scheduler_debug_timeout_left_idle ? 1U : 0U);
}

bool scheduler_debug_timer_measure_pending(kernel_thread_t *thread)
{
    return scheduler_timer_trace_enabled && thread &&
        thread->timer_measure_pending;
}

void scheduler_debug_timer_measure_resume(kernel_thread_t *thread,
                                          uintptr_t actual_rsp)
{
    if (!scheduler_timer_trace_enabled || !thread ||
        !thread->timer_measure_pending) {
        return;
    }

    if (scheduler_timer_measure_done) return;

    thread->timer_measure_pending = false;
    scheduler_timer_measure_done = true;
    scheduler_timer_rsp_mismatch =
        actual_rsp != thread->preempt_return_rsp;
    kprint("[timer-trace] RSP MEASURE thread=%llu name=%s captured=%llx observed=%llx delta=%lld\n",
           thread->id, thread->name, (u64)thread->preempt_return_rsp,
           (u64)actual_rsp,
           (i64)actual_rsp - (i64)thread->preempt_return_rsp);
    if (!scheduler_timer_rsp_mismatch) {
        kprint("[timer-trace] RSP ASSERTION PASSED thread=%llu\n", thread->id);
    }
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
    if (scheduler_rr_trace_thread(thread)) {
        scheduler_rr_trace_record('E', 0, scheduler_rr_dispatch_reason,
                                  thread, queue->head, queue->tail, NULL);
    }
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
            if (scheduler_rr_trace_thread(thread)) {
                scheduler_rr_trace_record('S', 0, scheduler_rr_dispatch_reason,
                                          thread, queue->head, queue->tail,
                                          thread);
            }
            scheduler_remove_queued(thread);
            if (thread->state == THREAD_STATE_READY &&
                !thread->queued && thread->effective_priority == priority &&
                thread_saved_stack_valid(thread)) {
                thread->last_selected_priority = thread->effective_priority;
                thread->last_selection_was_wakeup_boost =
                    thread->wakeup_boosted;
                scheduler_stats.dispatches[priority]++;
                if (thread->effective_priority != thread->base_priority) {
                    thread->effective_priority = thread->base_priority;
                }
                thread->wakeup_boosted = false;
                thread->ready_wait_ticks = 0;
                scheduler_debug_fairness_dispatch(thread);
                scheduler_debug_sleep_trace_event(
                    SCHEDULER_SLEEP_TRACE_SELECTED, thread);
                return thread;
            }
        }
    }
    if (idle_thread && idle_thread->state == THREAD_STATE_READY &&
        !idle_thread->queued && thread_saved_stack_valid(idle_thread)) {
        scheduler_stats.dispatches[THREAD_PRIORITY_BACKGROUND]++;
        scheduler_debug_sleep_trace_event(SCHEDULER_SLEEP_TRACE_IDLE,
                                          idle_thread);
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
                    } else {
                        scheduler_debug_fairness_promotion(
                            thread, old_priority,
                            thread->effective_priority);
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
                } else {
                    scheduler_debug_fairness_promotion(
                        thread, old_priority, thread->effective_priority);
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
        thread->kernel_stack_size < 7 * sizeof(u64)) {
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
    memset(&scheduler_stats, 0, sizeof(scheduler_stats));
    idle_thread = NULL;
    interrupt_switch_requested = false;
    preemption_pending = false;
    scheduler_debug_timeout_armed = false;
    scheduler_debug_timeout_fired = false;
    scheduler_debug_timeout_force_bootstrap = false;
    scheduler_debug_timeout_tick = 0;
    memset(&bootstrap_thread, 0, sizeof(bootstrap_thread));
    bootstrap_thread.id = 1;
    bootstrap_thread.state = THREAD_STATE_RUNNING;
    bootstrap_thread.effective_priority = THREAD_PRIORITY_NORMAL;
    bootstrap_thread.base_priority = THREAD_PRIORITY_NORMAL;
    bootstrap_thread.default_time_slice =
        thread_default_time_slice(bootstrap_thread.effective_priority);
    bootstrap_thread.remaining_time_slice = bootstrap_thread.default_time_slice;
    bootstrap_thread.saved_stack_pointer = stack_pointer;
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
        return false;
    }
    target->state = THREAD_STATE_RUNNING;
    current_thread = target;
    scheduler_stats.context_switches++;
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
    /* All yield, block, terminate, and deferred-preemption paths converge
     * here so queue/state invariants are changed in one place. */
    kernel_thread_t *outgoing;
    kernel_thread_t *target;
    bool queue_outgoing;
    bool enable_interrupts;
    bool requeue_current = action == SCHEDULER_DISPATCH_REQUEUE;

    enable_interrupts = interrupt_switch_requested;
    interrupt_switch_requested = false;

    scheduler_rr_dispatch_reason = action == SCHEDULER_DISPATCH_BLOCK ? 3U :
        (action == SCHEDULER_DISPATCH_TERMINATE ? 4U :
         (enable_interrupts ? 1U : 2U));

    outgoing = current_thread;
    if (scheduler_rr_trace_thread(outgoing)) {
        scheduler_rr_trace_record('D', 0, scheduler_rr_dispatch_reason,
                                  outgoing, NULL, NULL, NULL);
    }
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
    if (queue_outgoing) {
        /* The outgoing thread is READY while it is being published to a
         * queue.  Detach current_thread during that brief transition so the
         * invariant "the current thread is never queued" remains true even
         * while scheduler_enqueue() validates the queue. */
        current_thread = NULL;
        if (!scheduler_enqueue(outgoing)) {
            current_thread = outgoing;
            outgoing->state = THREAD_STATE_RUNNING;
            return false;
        }
    } else if (action == SCHEDULER_DISPATCH_BLOCK) {
        /* A sleeper is already on sleeping_threads at this point.  It is no
         * longer the runnable current thread while the next context is being
         * selected, so detach it before scheduler_validate() inspects the
         * sleep-list invariant. */
        current_thread = NULL;
    }

    if (scheduler_debug_timeout_force_bootstrap &&
        outgoing != &bootstrap_thread &&
        bootstrap_thread.state == THREAD_STATE_READY) {
        scheduler_remove_queued(&bootstrap_thread);
        target = &bootstrap_thread;
        scheduler_debug_timeout_force_bootstrap = false;
    } else {
        target = scheduler_select_next();
    }
    if (scheduler_debug_timeout_fired) {
        scheduler_debug_timeout_selected_id = target ? target->id : 0;
        scheduler_debug_timeout_left_idle =
            outgoing == idle_thread && target && target != idle_thread;
    }
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
        return false;
    }

    if (target == outgoing) {
        target->state = THREAD_STATE_RUNNING;
        current_thread = target;
        return false;
    }

    target->state = THREAD_STATE_RUNNING;
    current_thread = target;
    scheduler_stats.context_switches++;
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

    if (!thread || thread == idle_thread ||
        thread->state != THREAD_STATE_BLOCKED || thread->queued) {
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
        return false;
    }
    scheduler_debug_sleep_trace_event(SCHEDULER_SLEEP_TRACE_REQUEUED, thread);
    if (scheduler_stats.blocked_threads) {
        scheduler_stats.blocked_threads--;
    }
    if (was_sleeping && scheduler_stats.sleeping_threads) {
        scheduler_stats.sleeping_threads--;
    }
    scheduler_stats.wakeups++;
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
    scheduler_stats.blocks++;
    scheduler_stats.blocked_threads++;
    if (!scheduler_dispatch(SCHEDULER_DISPATCH_BLOCK)) {
        thread->state = THREAD_STATE_RUNNING;
        current_thread = thread;
        scheduler_stats.blocks--;
        scheduler_stats.blocked_threads--;
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

    scheduler_debug_sleep_trace_event(SCHEDULER_SLEEP_TRACE_ENTERED, thread);
    thread->wakeup_tick = scheduler_tick_count > (~(u64)0 - ticks) ?
        ~(u64)0 : scheduler_tick_count + ticks;
    thread->next = sleeping_threads;
    sleeping_threads = thread;
    thread->sleeping = true;
    scheduler_stats.sleeping_threads++;
    thread->state = THREAD_STATE_BLOCKED;
    scheduler_stats.blocks++;
    scheduler_stats.blocked_threads++;
    scheduler_debug_sleep_trace_event(SCHEDULER_SLEEP_TRACE_INSERTED, thread);
    if (!scheduler_dispatch(SCHEDULER_DISPATCH_BLOCK)) {
        sleeping_remove(thread);
        if (scheduler_stats.sleeping_threads) {
            scheduler_stats.sleeping_threads--;
        }
        thread->state = THREAD_STATE_RUNNING;
        current_thread = thread;
        scheduler_stats.blocks--;
        scheduler_stats.blocked_threads--;
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
    bool promoted_thread;

    if (thread == idle_thread) {
        scheduler_stats.idle_runtime_ticks++;
    }

    if (scheduler_tick_count != ~(u64)0) {
        scheduler_tick_count++;
    }

    /* Test-only escape hatch: unlike sleepers, bootstrap is published
     * directly from the timer path when the diagnostic deadline expires. */
    if (scheduler_debug_timeout_armed &&
        scheduler_tick_count >= scheduler_debug_timeout_tick) {
        scheduler_debug_timeout_armed = false;
        scheduler_debug_timeout_fired = true;
        scheduler_debug_timeout_reached_tick = scheduler_tick_count;
        scheduler_debug_timeout_reached_current_id =
            current_thread ? current_thread->id : 0;
        scheduler_debug_timeout_reached_bootstrap_state =
            (u32)bootstrap_thread.state;
        scheduler_debug_timeout_reached_bootstrap_queued =
            bootstrap_thread.queued;
        scheduler_debug_timeout_force_bootstrap =
            current_thread != &bootstrap_thread &&
            bootstrap_thread.state == THREAD_STATE_READY;
        if (current_thread == &bootstrap_thread) {
            scheduler_debug_timeout_enqueue_reject = 1;
        } else if (bootstrap_thread.state != THREAD_STATE_READY) {
            scheduler_debug_timeout_enqueue_reject = 2;
        } else if (bootstrap_thread.queued) {
            scheduler_debug_timeout_enqueue_ok = true;
        } else if (scheduler_enqueue(&bootstrap_thread)) {
            scheduler_debug_timeout_enqueue_ok = true;
        } else {
            scheduler_debug_timeout_enqueue_reject = 3;
        }
        if (scheduler_debug_timeout_force_bootstrap) {
            preemption_pending = true;
        }
    }

    sleeping = sleeping_threads;
    while (sleeping) {
        next_sleeping = sleeping->next;
        if (sleeping->wakeup_tick <= scheduler_tick_count) {
            scheduler_debug_sleep_trace_event(
                SCHEDULER_SLEEP_TRACE_WAKE_DUE, sleeping);
            sleeping_remove(sleeping);
            scheduler_debug_sleep_trace_event(
                SCHEDULER_SLEEP_TRACE_REMOVED, sleeping);
            if (scheduler_stats.sleeping_threads) {
                scheduler_stats.sleeping_threads--;
            }
            sleeping->effective_priority = sleeping->base_priority ==
                THREAD_PRIORITY_BACKGROUND ? THREAD_PRIORITY_NORMAL :
                sleeping->base_priority;
            sleeping->wakeup_boosted =
                sleeping->base_priority == THREAD_PRIORITY_BACKGROUND;
            sleeping->state = THREAD_STATE_READY;
            scheduler_debug_sleep_trace_event(
                SCHEDULER_SLEEP_TRACE_REQUEUE, sleeping);
            if (scheduler_enqueue(sleeping)) {
                scheduler_debug_sleep_trace_event(
                    SCHEDULER_SLEEP_TRACE_REQUEUED, sleeping);
                woke_thread = true;
                if (scheduler_stats.blocked_threads) {
                    scheduler_stats.blocked_threads--;
                }
                scheduler_stats.wakeups++;
            } else {
                u32 reject = sleeping->state != THREAD_STATE_READY ? 1U :
                    (sleeping->queued ? 2U :
                     (!thread_priority_valid(sleeping->effective_priority) ?
                      3U : 4U));
                if (sleeping->id == scheduler_sleep_trace_first) {
                    scheduler_sleep_trace_first_reject = reject;
                } else if (sleeping->id == scheduler_sleep_trace_second) {
                    scheduler_sleep_trace_second_reject = reject;
                }
                scheduler_debug_sleep_trace_event(
                    SCHEDULER_SLEEP_TRACE_REJECTED, sleeping);
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

    if (!preemption_pending || !regs || !thread ||
        thread->state != THREAD_STATE_RUNNING || thread->queued ||
        !thread_saved_stack_valid(thread)) {
        if (scheduler_debug_timeout_force_bootstrap) {
            if (!preemption_pending) scheduler_debug_timeout_enqueue_reject = 10;
            else if (!regs) scheduler_debug_timeout_enqueue_reject = 11;
            else if (!thread) scheduler_debug_timeout_enqueue_reject = 12;
            else if (thread->state != THREAD_STATE_RUNNING)
                scheduler_debug_timeout_enqueue_reject = 13;
            else if (thread->queued) scheduler_debug_timeout_enqueue_reject = 14;
            else scheduler_debug_timeout_enqueue_reject = 15;
        }
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
    thread->timer_measure_pending = scheduler_timer_trace_enabled &&
        !scheduler_timer_measure_done && thread != idle_thread &&
        thread != &bootstrap_thread;
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
    bool switched;

    if (!thread || thread->state != THREAD_STATE_RUNNING ||
        !thread->preempt_return_rip) {
        return 0;
    }

    return_rip = (u64)thread->preempt_return_rip;
    interrupt_switch_requested = true;
    switched = scheduler_reschedule();
    if (scheduler_debug_timeout_force_bootstrap && !switched) {
        scheduler_debug_timeout_enqueue_reject = 20;
    }
    return return_rip;
}

u64 scheduler_preempt_return_flags(void)
{
    return current_thread ? current_thread->preempt_return_rflags : 0;
}

void scheduler_preempt_context_restored(uintptr_t restored_rsp)
{
    if (current_thread) {
        /* This callback only validates stack bounds.  Exact RSP equality is
         * measured in the resumed worker before it calls back into C. */
        if (scheduler_timer_trace_enabled &&
            (restored_rsp < current_thread->kernel_stack_base ||
             restored_rsp >= current_thread->kernel_stack_base +
                 current_thread->kernel_stack_size)) {
            scheduler_timer_stack_oob = true;
            if (!scheduler_timer_stack_drift_reported) {
                scheduler_timer_stack_drift_reported = true;
                kprint("[timer-trace] RESTORE RSP OUT OF BOUNDS thread=%llu rsp=%llx\n",
                       current_thread->id, (u64)restored_rsp);
            }
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
