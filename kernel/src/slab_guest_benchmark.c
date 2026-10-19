/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef SLAB_CORE_GUEST_BENCHMARK
#include <cpu.h>
#include <heap.h>
#include <kprint.h>
#include <panic.h>
#include <scheduler.h>
#include <slab.h>
#include <timer.h>
#include <string.h>

#define BENCH_WORKERS 4U
#define BENCH_POPULATION 32U
#define BENCH_BATCH 16U
#define BENCH_SAMPLES 31U
#define BENCH_WARMUPS 2U
#define BENCH_MAX_ATTEMPTS 256U
#define BENCH_PARALLEL_PAIRS 512U
#define BENCH_READY_TIMEOUT_MS 10000U
#define BENCH_WORK_TIMEOUT_MS 60000U

typedef struct {
    u64 tsc;
    u64 scheduler_ticks;
    u32 cpu;
    bool valid;
} bench_stamp_t;

typedef struct __attribute__((aligned(64))) {
    volatile u32 active;
} bench_cpu_activity_t;

typedef struct __attribute__((aligned(64))) {
    u32 seen;
    u32 allocations;
    u32 frees;
} bench_cpu_metrics_t;

typedef struct {
    volatile u32 started;
    volatile u32 done;
    volatile u32 failed;
    volatile u32 go;
    volatile u32 remote_wait_mask;
    volatile u32 cpu_mask;
    volatile u32 cross_cpu_frees;
    volatile u32 cross_release_go;
    volatile u32 overlap_cpu_mask;
    volatile u32 peak_active_cpus;
    volatile u32 peak_active_allocator_operations;
    volatile u64 work_failure_detail;
    u32 allocator; /* zero is kmalloc/kfree; one is the slab API. */
    u32 workload;  /* zero is one hot class; one varies by worker. */
    u32 iterations;
    u32 coordinator_cpu;
    u32 online_cpus;
    bool rendezvous;
    u16 request[ BENCH_WORKERS ];
    void *cross_object[BENCH_WORKERS];
    u32 cross_owner_cpu[BENCH_WORKERS];
    u32 cross_free_cpu[BENCH_WORKERS];
    u64 cross_free_tsc[BENCH_WORKERS];
    u64 checksum[BENCH_WORKERS + 1U];
    bench_cpu_activity_t cpu_activity[32];
    bench_cpu_metrics_t cpu_metrics[32];
} bench_parallel_state_t;

static const char *bench_allocator_name(u32 allocator)
{
    return allocator == 0U ? "heap" :
           (allocator == 1U ? "slab" : "cpu-slab");
}

#define BENCH_FREE_PROFILE_OPERATIONS 1024U
#define BENCH_FREE_PROFILE_STRIDE 16U
#define BENCH_FREE_PROFILE_TRIALS 3U
#define BENCH_FREE_PROFILE_SPIN_LIMIT 100000000U
#define BENCH_FREE_PROFILE_READY_TICKS 1000U

static void *bench_objects[BENCH_POPULATION];
static u8 bench_expected[BENCH_POPULATION];
static bench_parallel_state_t bench_parallel;
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
static void *bench_profile_objects[256];
static slab_free_profile_sample_t bench_profile_samples[
    SLAB_FREE_PROFILE_SAMPLE_CAPACITY];
#endif
static char bench_profile_line[4096];

static u64 bench_irq_save_disable(void);
static void bench_irq_restore(u64 flags);

static u64 bench_tsc_read(void)
{
    u32 low;
    u32 high;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence"
                     : "=a"(low), "=d"(high) :: "memory");
    return ((u64)high << 32) | low;
}

/* A sample is usable only when both serialized endpoints came from the same
 * CPU and that CPU saw no scheduler timer tick. This detects normal
 * preemption/migration at the scheduler tick granularity; it is not a claim
 * that TSC ticks equal core cycles or nanoseconds. */
static bench_stamp_t bench_stamp(void)
{
    bench_stamp_t stamp = {0};
    cpu_local_t *cpu = cpu_current();
    if (!cpu)
        return stamp;
    stamp.cpu = cpu->index;
    stamp.scheduler_ticks = __atomic_load_n(&cpu->scheduler_accounted_ticks,
                                             __ATOMIC_RELAXED);
    stamp.tsc = bench_tsc_read();
    cpu_local_t *after = cpu_current();
    if (after != cpu || after->index != stamp.cpu)
        return stamp;
    if (__atomic_load_n(&after->scheduler_accounted_ticks,
                        __ATOMIC_RELAXED) != stamp.scheduler_ticks)
        return stamp;
    stamp.valid = true;
    return stamp;
}

static bool bench_interval(bench_stamp_t start, bench_stamp_t end,
                           u64 *ticks)
{
    cpu_local_t *cpu = cpu_current();
    if (!ticks || !start.valid || !end.valid || !cpu ||
        start.cpu != end.cpu || cpu->index != start.cpu ||
        start.scheduler_ticks != end.scheduler_ticks)
        return false;
    *ticks = end.tsc - start.tsc;
    return *ticks != 0U;
}

static bool bench_lifecycle_interval(bench_stamp_t start, bench_stamp_t end,
                                     u64 *ticks, u32 *scheduler_tick_delta)
{
    u32 current = cpu_current_index();
    if (!ticks || !scheduler_tick_delta || start.cpu == ~(u32)0 ||
        end.cpu == ~(u32)0 || start.cpu != end.cpu || current != start.cpu)
        return false;
    *ticks = end.tsc - start.tsc;
    *scheduler_tick_delta = (u32)(end.scheduler_ticks -
                                  start.scheduler_ticks);
    return *ticks != 0U;
}

static void *bench_allocate(u32 allocator, usize size)
{
    return allocator == 0U ? kmalloc(size) :
           (allocator == 1U ? slab_alloc(size) : slab_cpu_alloc(size));
}

static bool bench_release(u32 allocator, void *object)
{
    if (allocator == 1U)
        return slab_try_free(object);
    if (allocator == 2U)
        return slab_cpu_try_free(object);
    kfree(object);
    return true;
}

static void bench_fill(void *object, usize size, u8 value)
{
    u8 *bytes = object;
    for (usize index = 0; index < size; index++)
        bytes[index] = value;
}

static bool bench_contents(const void *object, usize size, u8 expected)
{
    const u8 *bytes = object;
    for (usize index = 0; index < size; index++)
        if (bytes[index] != expected)
            return false;
    return true;
}

static bool bench_live_ranges_valid(const void **objects,
                                    const u8 *live, usize count,
                                    usize request_size)
{
    for (usize left = 0; left < count; left++) {
        if (!live[left] || !objects[left])
            continue;
        uintptr_t a = (uintptr_t)objects[left];
        if (a > (uintptr_t)-1 - request_size)
            return false;
        uintptr_t a_end = a + request_size;
        if ((a & 15U) || !heap_address_range_is_mapped(objects[left],
                                                        request_size))
            return false;
        for (usize right = left + 1U; right < count; right++) {
            if (!live[right] || !objects[right])
                continue;
            uintptr_t b = (uintptr_t)objects[right];
            if (b > (uintptr_t)-1 - request_size)
                return false;
            uintptr_t b_end = b + request_size;
            if (a < b_end && b < a_end)
                return false;
        }
    }
    return true;
}

static bool bench_prepare_population(u32 allocator, usize size)
{
    u8 live[BENCH_POPULATION];
    memset(bench_objects, 0, sizeof(bench_objects));
    memset(live, 0, sizeof(live));
    for (u32 index = 0; index < BENCH_POPULATION; index++) {
        void *object = bench_allocate(allocator, size);
        if (!object)
            goto fail;
        bench_objects[index] = object;
        bench_expected[index] = (u8)(0x41U + (index % 31U));
        bench_fill(object, size, bench_expected[index]);
        live[index] = 1U;
    }
    if (!bench_live_ranges_valid((const void **)bench_objects, live,
                                 BENCH_POPULATION, size))
        goto fail;
    for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
        if (!bench_contents(bench_objects[index], size,
                            bench_expected[index]) ||
            !bench_release(allocator, bench_objects[index]))
            goto fail;
        bench_objects[index] = NULL;
        live[index] = 0U;
    }
    return true;

fail:
    for (u32 index = 0; index < BENCH_POPULATION; index++) {
        if (live[index])
            (void)bench_release(allocator, bench_objects[index]);
        bench_objects[index] = NULL;
    }
    return false;
}

static void bench_class_delta(u32 class_index,
                              const slab_class_stats_t *before,
                              slab_class_stats_t *delta)
{
    slab_class_stats_t after = {0};
    memset(delta, 0, sizeof(*delta));
    if (!slab_get_class_stats(class_index, &after))
        panic("slab benchmark: class statistics unavailable");
    delta->lock_acquisitions = after.lock_acquisitions -
                               before->lock_acquisitions;
    delta->bitmap_words_examined = after.bitmap_words_examined -
                                   before->bitmap_words_examined;
    delta->backing_allocations = after.backing_allocations -
                                 before->backing_allocations;
    delta->backing_releases = after.backing_releases -
                              before->backing_releases;
}

static u32 bench_class_index(usize request)
{
    for (u32 index = 0; index < SLAB_CLASS_COUNT; index++)
        if (request <= slab_class_size(index))
            return index;
    return SLAB_CLASS_COUNT;
}

static bool bench_line_append_text(char *buffer, usize capacity,
                                   usize *position, const char *text)
{
    usize length = strlen(text);
    if (*position > capacity || length >= capacity - *position)
        return false;
    memcpy(buffer + *position, text, length);
    *position += length;
    buffer[*position] = '\0';
    return true;
}

static bool bench_line_append_u64(char *buffer, usize capacity,
                                  usize *position, u64 value)
{
    char reverse[24];
    usize count = 0U;
    do {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < sizeof(reverse));
    if (*position > capacity || count >= capacity - *position)
        return false;
    while (count)
        buffer[(*position)++] = reverse[--count];
    buffer[*position] = '\0';
    return true;
}

#ifdef SLAB_CORE_GUEST_FREE_PROFILE
static const char *bench_free_profile_stage_name(u32 stage)
{
    switch ((slab_free_profile_stage_t)stage) {
    case SLAB_FREE_PROFILE_END_TO_END: return "end_to_end";
    case SLAB_FREE_PROFILE_CLASSIFY: return "classify";
    case SLAB_FREE_PROFILE_LOCK: return "lock";
    case SLAB_FREE_PROFILE_VALIDATE: return "token_descriptor";
    case SLAB_FREE_PROFILE_SLOT: return "slot_bitmap";
    case SLAB_FREE_PROFILE_MUTATE: return "mutate_transition";
    case SLAB_FREE_PROFILE_RELEASE: return "heap_release";
    default: return "invalid";
    }
}

static const char *bench_free_profile_case_name(u32 free_case)
{
    switch ((slab_free_profile_case_t)free_case) {
    case SLAB_FREE_PROFILE_NONFINAL: return "nonfinal";
    case SLAB_FREE_PROFILE_FULL_TO_PARTIAL: return "full_to_partial";
    case SLAB_FREE_PROFILE_FINAL: return "final_retire";
    default: return "invalid";
    }
}

static bool bench_free_profile_print(
    slab_free_profile_stage_t stage, slab_free_profile_case_t free_case,
    usize request, u32 sample_count, u32 observed_count, u32 lost_count,
    u32 unexpected_lost_count,
    u64 allocation_delta, u64 free_delta, u64 refill_delta,
    u64 retirement_delta, u64 classifier_bytes, u64 heap_delta,
    u32 trial)
{
    usize position = 0U;
    bench_profile_line[0] = '\0';
#define BENCH_APPEND_TEXT(value) \
    do { if (!bench_line_append_text(bench_profile_line, \
             sizeof(bench_profile_line), &position, (value))) return false; \
    } while (0)
#define BENCH_APPEND_NUM(value) \
    do { if (!bench_line_append_u64(bench_profile_line, \
             sizeof(bench_profile_line), &position, (u64)(value))) return false; \
    } while (0)
    BENCH_APPEND_TEXT("SLAB_FREE_RAW case=");
    BENCH_APPEND_TEXT(bench_free_profile_case_name(free_case));
    BENCH_APPEND_TEXT(" trial="); BENCH_APPEND_NUM(trial);
    BENCH_APPEND_TEXT(" stage=");
    BENCH_APPEND_TEXT(bench_free_profile_stage_name(stage));
    BENCH_APPEND_TEXT(" request="); BENCH_APPEND_NUM(request);
    BENCH_APPEND_TEXT(" class="); BENCH_APPEND_NUM(bench_class_index(request));
    BENCH_APPEND_TEXT(" cpu="); BENCH_APPEND_NUM(cpu_current_index());
    BENCH_APPEND_TEXT(" observed="); BENCH_APPEND_NUM(observed_count);
    BENCH_APPEND_TEXT(" samples="); BENCH_APPEND_NUM(sample_count);
    BENCH_APPEND_TEXT(" lost="); BENCH_APPEND_NUM(lost_count);
    BENCH_APPEND_TEXT(" unexpected_lost=");
    BENCH_APPEND_NUM(unexpected_lost_count);
    BENCH_APPEND_TEXT(" allocs="); BENCH_APPEND_NUM(allocation_delta);
    BENCH_APPEND_TEXT(" frees="); BENCH_APPEND_NUM(free_delta);
    BENCH_APPEND_TEXT(" refills="); BENCH_APPEND_NUM(refill_delta);
    BENCH_APPEND_TEXT(" retirements="); BENCH_APPEND_NUM(retirement_delta);
    BENCH_APPEND_TEXT(" classifier_delta="); BENCH_APPEND_NUM(classifier_bytes);
    BENCH_APPEND_TEXT(" heap_delta="); BENCH_APPEND_NUM(heap_delta);
    BENCH_APPEND_TEXT(" ticks=");
    for (u32 index = 0; index < sample_count; index++) {
        if (index)
            BENCH_APPEND_TEXT(",");
        BENCH_APPEND_NUM(bench_profile_samples[index].ticks);
    }
    BENCH_APPEND_TEXT("\n");
    kprint("%s", bench_profile_line);
#undef BENCH_APPEND_TEXT
#undef BENCH_APPEND_NUM
    return true;
}

static bool bench_profile_fixture_prepare(usize request,
                                          slab_free_profile_case_t free_case,
                                          u32 *object_count)
{
    u32 class_index = bench_class_index(request);
    if (class_index >= SLAB_CLASS_COUNT || !object_count)
        return false;
    /* Earlier steady-state measurements exercise the per-CPU path and can
     * leave slots cached. Free-path cases use the shared API and need a
     * controlled slab occupancy, especially when measuring final retirement. */
    (void)slab_cpu_drain_all();
    if (!slab_verify())
        return false;
    *object_count = 0U;
    if (free_case == SLAB_FREE_PROFILE_FINAL) {
        void *warm = slab_alloc(request);
        if (!warm || !slab_try_free(warm))
            return false;
        return true;
    }
    u32 slots = slab_class_slots(class_index);
    u32 count = free_case == SLAB_FREE_PROFILE_NONFINAL
        ? (slots > 2U && slots - 2U < 64U ? slots - 2U : 64U) : slots;
    if (!count || count > sizeof(bench_profile_objects) /
                        sizeof(bench_profile_objects[0]))
        return false;
    for (u32 index = 0; index < count; index++) {
        bench_profile_objects[index] = slab_alloc(request);
        if (!bench_profile_objects[index]) {
            for (u32 cleanup = 0; cleanup < index; cleanup++)
                (void)slab_try_free(bench_profile_objects[cleanup]);
            return false;
        }
        bench_fill(bench_profile_objects[index], request, 0xa5U);
    }
    *object_count = count;
    return true;
}

static bool bench_profile_case_step(usize request,
                                    slab_free_profile_case_t free_case,
                                    u32 iteration, u32 object_count)
{
    if (free_case == SLAB_FREE_PROFILE_NONFINAL) {
        u32 slot = iteration % object_count;
        void *replacement = slab_alloc(request);
        if (!replacement || !bench_contents(bench_profile_objects[slot],
                                             request, 0xa5U))
            return false;
        bench_fill(replacement, request, 0xa5U);
        if (!slab_try_free(bench_profile_objects[slot]))
            return false;
        bench_profile_objects[slot] = replacement;
        return true;
    }
    if (free_case == SLAB_FREE_PROFILE_FULL_TO_PARTIAL) {
        void *old = bench_profile_objects[0];
        if (!old || !bench_contents(old, request, 0xa5U) ||
            !slab_try_free(old))
            return false;
        bench_profile_objects[0] = NULL;
        void *replacement = slab_alloc(request);
        if (!replacement)
            return false;
        bench_fill(replacement, request, 0xa5U);
        bench_profile_objects[0] = replacement;
        return true;
    }
    if (free_case == SLAB_FREE_PROFILE_FINAL) {
        void *object = slab_alloc(request);
        if (!object)
            return false;
        bench_fill(object, request, 0x3cU);
        bool payload_ok = bench_contents(object, request, 0x3cU);
        bool free_ok = slab_try_free(object);
        return payload_ok && free_ok;
    }
    return false;
}

static void bench_profile_fixture_cleanup(
    slab_free_profile_case_t free_case, u32 object_count)
{
    for (u32 index = 0; index < object_count; index++) {
        if (bench_profile_objects[index]) {
            (void)slab_try_free(bench_profile_objects[index]);
            bench_profile_objects[index] = NULL;
        }
    }
    (void)free_case;
}

static bool bench_profile_free_stage(usize request,
                                     slab_free_profile_case_t free_case,
                                     slab_free_profile_stage_t stage,
                                     u32 trial)
{
    const u32 class_index = bench_class_index(request);
    slab_class_stats_t before, after;
    slab_classifier_stats_t classifier_before, classifier_after;
    u64 heap_before, heap_after;
    u32 object_count, sample_count = 0U, observed_count = 0U;
    u32 lost_count = 0U, unexpected_lost_count = 0U;
    u32 lost_reason = 0U, lost_ordinal = 0U;
    u64 flags;

    if (!bench_profile_fixture_prepare(request, free_case, &object_count)) {
        kprint("SLAB_FREE_PROFILE_FAIL phase=prepare case=%s request=%u trial=%u\n",
               bench_free_profile_case_name(free_case), request, trial);
        return false;
    }
    if (!slab_get_class_stats(class_index, &before) ||
        !slab_get_classifier_stats(&classifier_before)) {
        kprint("SLAB_FREE_PROFILE_FAIL phase=before_stats case=%s request=%u trial=%u\n",
               bench_free_profile_case_name(free_case), request, trial);
        bench_profile_fixture_cleanup(free_case, object_count);
        return false;
    }
    heap_before = heap_get_used_size();

    flags = bench_irq_save_disable();
    if (!slab_free_profile_configure(stage, free_case, class_index,
                                     cpu_current_index(),
                                     BENCH_FREE_PROFILE_STRIDE)) {
        bench_irq_restore(flags);
        bench_profile_fixture_cleanup(free_case, object_count);
        kprint("SLAB_FREE_PROFILE_FAIL phase=configure case=%s request=%u trial=%u\n",
               bench_free_profile_case_name(free_case), request, trial);
        return false;
    }
    bool operations_ok = true;
    for (u32 operation = 0; operation < BENCH_FREE_PROFILE_OPERATIONS;
         operation++) {
        if (!bench_profile_case_step(request, free_case, operation,
                                     object_count)) {
            operations_ok = false;
            break;
        }
    }
    slab_free_profile_disable();
    bench_irq_restore(flags);

    if (!operations_ok || !slab_get_class_stats(class_index, &after) ||
        !slab_get_classifier_stats(&classifier_after)) {
        kprint("SLAB_FREE_PROFILE_FAIL phase=operations case=%s request=%u trial=%u operations_ok=%u\n",
               bench_free_profile_case_name(free_case), request, trial,
               operations_ok ? 1U : 0U);
        bench_profile_fixture_cleanup(free_case, object_count);
        return false;
    }
    heap_after = heap_get_used_size();
    if (!slab_free_profile_read(bench_profile_samples,
                                SLAB_FREE_PROFILE_SAMPLE_CAPACITY,
                                &sample_count, &observed_count, &lost_count,
                                &unexpected_lost_count,
                                &lost_reason, &lost_ordinal)) {
        bench_profile_fixture_cleanup(free_case, object_count);
        return false;
    }
    if (observed_count != BENCH_FREE_PROFILE_OPERATIONS ||
        sample_count < SLAB_FREE_PROFILE_SAMPLE_CAPACITY - 4U ||
        sample_count + lost_count != SLAB_FREE_PROFILE_SAMPLE_CAPACITY ||
        unexpected_lost_count != 0U ||
        after.frees - before.frees != BENCH_FREE_PROFILE_OPERATIONS ||
        after.allocations - before.allocations !=
            BENCH_FREE_PROFILE_OPERATIONS) {
        kprint("SLAB_FREE_PROFILE_FAIL phase=counters case=%s stage=%s request=%u trial=%u observed=%u samples=%u lost=%u unexpected_lost=%u lost_reason=%u lost_ordinal=%u allocs=%llu frees=%llu refills=%llu retirements=%llu\n",
               bench_free_profile_case_name(free_case),
               bench_free_profile_stage_name(stage), request, trial,
               observed_count, sample_count, lost_count,
               unexpected_lost_count,
               lost_reason, lost_ordinal,
               after.allocations - before.allocations,
               after.frees - before.frees,
               after.backing_allocations - before.backing_allocations,
               after.retired_slabs - before.retired_slabs);
        bench_profile_fixture_cleanup(free_case, object_count);
        return false;
    }
    for (u32 index = 0; index < sample_count; index++) {
        if (bench_profile_samples[index].stage != (u32)stage ||
            bench_profile_samples[index].free_case != (u32)free_case ||
            bench_profile_samples[index].class_index != class_index ||
            bench_profile_samples[index].cpu_index != cpu_current_index() ||
            !bench_profile_samples[index].ticks) {
            kprint("SLAB_FREE_PROFILE_FAIL phase=sample case=%s request=%u trial=%u index=%u stage=%u case_id=%u class=%u cpu=%u ticks=%llu\n",
                   bench_free_profile_case_name(free_case), request, trial,
                   index, bench_profile_samples[index].stage,
                   bench_profile_samples[index].free_case,
                   bench_profile_samples[index].class_index,
                   bench_profile_samples[index].cpu_index,
                   bench_profile_samples[index].ticks);
            bench_profile_fixture_cleanup(free_case, object_count);
            return false;
        }
    }
    bool printed = bench_free_profile_print(
        stage, free_case, request, sample_count, observed_count, lost_count,
        unexpected_lost_count,
        after.allocations - before.allocations,
        after.frees - before.frees,
        after.backing_allocations - before.backing_allocations,
        after.retired_slabs - before.retired_slabs,
        classifier_after.retained_payload_bytes -
            classifier_before.retained_payload_bytes,
        heap_after - heap_before, trial);
    bench_profile_fixture_cleanup(free_case, object_count);
    return printed && slab_verify();
}

static bool bench_profile_free_paths(void)
{
    static const usize requests[] = {16U, 72U, 192U, 384U};
    static const slab_free_profile_stage_t nonfinal_stages[] = {
        SLAB_FREE_PROFILE_END_TO_END,
        SLAB_FREE_PROFILE_CLASSIFY,
        SLAB_FREE_PROFILE_LOCK,
        SLAB_FREE_PROFILE_VALIDATE,
        SLAB_FREE_PROFILE_SLOT,
        SLAB_FREE_PROFILE_MUTATE,
    };
    static const slab_free_profile_stage_t transition_stages[] = {
        SLAB_FREE_PROFILE_END_TO_END,
        SLAB_FREE_PROFILE_MUTATE,
    };
    static const slab_free_profile_stage_t retirement_stages[] = {
        SLAB_FREE_PROFILE_END_TO_END,
        SLAB_FREE_PROFILE_MUTATE,
        SLAB_FREE_PROFILE_RELEASE,
    };
    for (u32 trial = 0; trial < BENCH_FREE_PROFILE_TRIALS; trial++)
        for (u32 index = 0; index < sizeof(requests) / sizeof(requests[0]);
             index++) {
            for (u32 stage = 0;
                 stage < sizeof(nonfinal_stages) / sizeof(nonfinal_stages[0]);
                 stage++)
                if (!bench_profile_free_stage(
                        requests[index], SLAB_FREE_PROFILE_NONFINAL,
                        nonfinal_stages[stage], trial))
                    return false;
            for (u32 stage = 0;
                 stage < sizeof(transition_stages) /
                             sizeof(transition_stages[0]); stage++)
                if (!bench_profile_free_stage(
                        requests[index], SLAB_FREE_PROFILE_FULL_TO_PARTIAL,
                        transition_stages[stage], trial))
                    return false;
            for (u32 stage = 0;
                 stage < sizeof(retirement_stages) /
                             sizeof(retirement_stages[0]); stage++)
                if (!bench_profile_free_stage(
                        requests[index], SLAB_FREE_PROFILE_FINAL,
                        retirement_stages[stage], trial))
                    return false;
        }
    kprint("SLAB_FREE_PROFILE_END cases=3 requests=4 trials=%u operations_per_case=1024 sampled_attempts_per_trial=64 stride=16 zero_tick_deltas_are_counted=1 status=PASS\n",
           BENCH_FREE_PROFILE_TRIALS);
    return true;
}
#endif

static bool bench_measure_steady(u32 allocator, usize request,
                                 u32 trial, bool allocation_phase,
                                 u32 sample_index, bool emit_sample,
                                 u32 *rejected,
                                 bool *sample_accepted)
{
    const u32 class_index = bench_class_index(request);
    u8 live[BENCH_POPULATION];
    slab_class_stats_t before = {0}, delta;
    bench_stamp_t start, end;
    u64 elapsed;

    *sample_accepted = false;
    if (!bench_prepare_population(allocator, request))
        return false;
    memset(live, 1, sizeof(live));
    for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
        live[index] = 0U;
    }
    if (allocation_phase) {
        if (allocator && !slab_get_class_stats(class_index, &before))
            return false;
        start = bench_stamp();
        for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
            void *object = bench_allocate(allocator, request);
            if (!object) {
                __atomic_add_fetch(&bench_parallel.failed, 1U,
                                   __ATOMIC_RELAXED);
                return false;
            }
            bench_objects[index] = object;
        }
        end = bench_stamp();
        for (u32 index = 0; index < BENCH_POPULATION; index += 2U)
            live[index] = 1U;
        if (!bench_live_ranges_valid((const void **)bench_objects, live,
                                     BENCH_POPULATION, request))
            return false;
        if (allocator)
            bench_class_delta(class_index, &before, &delta);
        if (bench_interval(start, end, &elapsed)) {
            for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
                bench_fill(bench_objects[index], request,
                           bench_expected[index]);
            }
            if (emit_sample)
                kprint("SLAB_NATIVE_SAMPLE allocator=%s size=%u phase=alloc trial=%u sample=%u count=%u tsc_ticks_total=%llu cpu=%u scheduler_tick_delta=0 lock_acquisitions=%llu bitmap_words=%llu ownership=pass payload=pass\n",
                       bench_allocator_name(allocator), (u32)request, trial,
                       sample_index, BENCH_BATCH, elapsed,
                       start.cpu, allocator ? delta.lock_acquisitions : 0U,
                       allocator ? delta.bitmap_words_examined : 0U);
            *sample_accepted = true;
        } else {
            (*rejected)++;
            for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
                bench_fill(bench_objects[index], request,
                           bench_expected[index]);
            }
        }
    } else {
        /* Reconstruct exactly the same live population and reusable holes;
         * allocation setup occurs outside the measured free interval. */
        for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
            void *object = bench_allocate(allocator, request);
            if (!object)
                return false;
            bench_objects[index] = object;
            live[index] = 1U;
            bench_fill(object, request, bench_expected[index]);
        }
        if (!bench_live_ranges_valid((const void **)bench_objects, live,
                                     BENCH_POPULATION, request))
            return false;
        for (u32 index = 0; index < BENCH_POPULATION; index++)
            if (!bench_contents(bench_objects[index], request,
                                bench_expected[index]))
                return false;
        if (allocator && !slab_get_class_stats(class_index, &before))
            return false;
        start = bench_stamp();
        for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
            if (!bench_release(allocator, bench_objects[index]))
                return false;
            bench_objects[index] = NULL;
        }
        end = bench_stamp();
        if (allocator)
            bench_class_delta(class_index, &before, &delta);
        if (bench_interval(start, end, &elapsed)) {
            if (emit_sample)
                kprint("SLAB_NATIVE_SAMPLE allocator=%s size=%u phase=free trial=%u sample=%u count=%u tsc_ticks_total=%llu cpu=%u scheduler_tick_delta=0 lock_acquisitions=%llu bitmap_words=%llu ownership=pass payload=pass\n",
                       bench_allocator_name(allocator), (u32)request, trial,
                       sample_index, BENCH_BATCH, elapsed,
                       start.cpu, allocator ? delta.lock_acquisitions : 0U,
                       allocator ? delta.bitmap_words_examined : 0U);
            *sample_accepted = true;
        } else {
            (*rejected)++;
        }
    }

    for (u32 index = 1U; index < BENCH_POPULATION; index += 2U) {
        if (!bench_contents(bench_objects[index], request,
                            bench_expected[index]) ||
            !bench_release(allocator, bench_objects[index]))
            return false;
        bench_objects[index] = NULL;
    }
    /* In the allocation phase also release the newly allocated slots. */
    if (allocation_phase) {
        for (u32 index = 0; index < BENCH_POPULATION; index += 2U) {
            if (!bench_contents(bench_objects[index], request,
                                bench_expected[index]) ||
                !bench_release(allocator, bench_objects[index]))
                return false;
            bench_objects[index] = NULL;
        }
    }
    return true;
}

static bool bench_measure_steady_path(u32 allocator, usize request)
{
    u32 rejected_alloc = 0U, rejected_free = 0U;
    u32 accepted_alloc = 0U, accepted_free = 0U;
    u32 attempts = 0U;

    /* Two unreported cycles warm the path while retaining no slab backing. */
    for (u32 warm = 0; warm < BENCH_WARMUPS; warm++) {
        bool ignored;
        if (!bench_measure_steady(allocator, request, warm, true, 0U,
                                  false, &rejected_alloc, &ignored) ||
            !bench_measure_steady(allocator, request, warm, false, 0U,
                                  false, &rejected_free, &ignored))
            return false;
    }
    while ((accepted_alloc < BENCH_SAMPLES || accepted_free < BENCH_SAMPLES) &&
           attempts < BENCH_MAX_ATTEMPTS) {
        /* The two phases use independently prepared equivalent populations.
         * Each attempt may be retained for either phase if its timing window
         * was not interrupted or migrated. */
        if (accepted_alloc < BENCH_SAMPLES) {
            bool accepted;
            if (!bench_measure_steady(allocator, request, attempts, true,
                                      accepted_alloc, true, &rejected_alloc,
                                      &accepted))
                return false;
            if (accepted)
                accepted_alloc++;
        }
        if (accepted_free < BENCH_SAMPLES) {
            bool accepted;
            if (!bench_measure_steady(allocator, request, attempts, false,
                                      accepted_free, true, &rejected_free,
                                      &accepted))
                return false;
            if (accepted)
                accepted_free++;
        }
        attempts++;
    }
    if (accepted_alloc != BENCH_SAMPLES || accepted_free != BENCH_SAMPLES)
        return false;
    kprint("SLAB_NATIVE_REJECTED allocator=%s size=%u alloc=%u free=%u attempts=%u\n",
           bench_allocator_name(allocator), (u32)request,
           rejected_alloc, rejected_free, attempts);
    return accepted_alloc == BENCH_SAMPLES &&
           accepted_free == BENCH_SAMPLES;
}

static u32 bench_popcount32(u32 value)
{
    value -= (value >> 1) & 0x55555555U;
    value = (value & 0x33333333U) + ((value >> 2) & 0x33333333U);
    value = (value + (value >> 4)) & 0x0f0f0f0fU;
    return (value * 0x01010101U) >> 24;
}

static u64 bench_irq_save_disable(void)
{
    u64 flags;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli"
                     : "=r"(flags) :: "memory");
    return flags;
}

static void bench_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0\n\tpopfq" :: "r"(flags) : "memory", "cc");
}

static void bench_overlap_enter(void)
{
    u32 cpu = cpu_current_index();
    if (cpu >= 32U) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        return;
    }
    bench_cpu_activity_t *activity = &bench_parallel.cpu_activity[cpu];
    bench_cpu_metrics_t *metrics = &bench_parallel.cpu_metrics[cpu];
    if (!metrics->seen) {
        metrics->seen = 1U;
        __atomic_fetch_or(&bench_parallel.cpu_mask, 1U << cpu,
                          __ATOMIC_RELAXED);
    }
    if (__atomic_exchange_n(&activity->active, 1U, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        return;
    }
    /* The per-CPU flags are separated by cache lines. A sequentially
     * consistent publish/read pair makes a positive observation proof that
     * allocator-call brackets overlapped, even if both CPUs enter together. */
    for (u32 other = 0; other < bench_parallel.online_cpus; other++) {
        if (other != cpu && __atomic_load_n(
                &bench_parallel.cpu_activity[other].active,
                __ATOMIC_SEQ_CST)) {
            u32 overlap = (1U << cpu) | (1U << other);
            __atomic_fetch_or(&bench_parallel.overlap_cpu_mask, overlap,
                              __ATOMIC_RELAXED);
        }
    }
}

static void bench_overlap_leave(u32 entered_cpu)
{
    u32 cpu = cpu_current_index();
    if (cpu >= 32U || entered_cpu >= 32U) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        return;
    }
    if (cpu != entered_cpu || !__atomic_load_n(
            &bench_parallel.cpu_activity[entered_cpu].active,
            __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&bench_parallel.cpu_activity[entered_cpu].active, 0U,
                     __ATOMIC_SEQ_CST);
}

static void *bench_parallel_allocate(usize size)
{
    u64 flags = bench_irq_save_disable();
    u32 cpu = cpu_current_index();
    bench_overlap_enter();
    void *object = bench_allocate(bench_parallel.allocator, size);
    bench_overlap_leave(cpu);
    if (object && cpu < 32U)
        bench_parallel.cpu_metrics[cpu].allocations++;
    bench_irq_restore(flags);
    return object;
}

static bool bench_parallel_release(void *object, u32 *called_cpu,
                                   u64 *elapsed_tsc)
{
    u64 flags = bench_irq_save_disable();
    u32 cpu = cpu_current_index();
    bench_overlap_enter();
    u64 start = elapsed_tsc ? bench_tsc_read() : 0U;
    bool success = bench_release(bench_parallel.allocator, object);
    bench_overlap_leave(cpu);
    u64 end = elapsed_tsc ? bench_tsc_read() : 0U;
    if (success && cpu < 32U)
        bench_parallel.cpu_metrics[cpu].frees++;
    if (called_cpu)
        *called_cpu = cpu;
    if (elapsed_tsc)
        *elapsed_tsc = end >= start ? end - start : 0U;
    bench_irq_restore(flags);
    return success;
}

static void *bench_parallel_prepare_object(u32 allocator, usize size,
                                          u32 *owner_cpu)
{
    u64 flags = bench_irq_save_disable();
    u32 cpu = cpu_current_index();
    void *object = bench_allocate(allocator, size);
    u32 after = cpu_current_index();
    bench_irq_restore(flags);
    if (after != cpu) {
        if (object)
            (void)bench_release(allocator, object);
        return NULL;
    }
    if (owner_cpu)
        *owner_cpu = cpu;
    return object;
}

static bool bench_parallel_work(u32 worker, usize request, u32 iterations,
                                u64 *checksum_out)
{
    if (!checksum_out || !request)
        return false;
    u64 checksum = (u64)(worker + 1U);
    for (u32 iteration = 0; iteration < iterations; iteration++) {
        void *object = bench_parallel_allocate(request);
        u32 failure_reason = 0U;
        if (!object) {
            failure_reason = 1U; /* allocator returned NULL */
        } else {
            u8 marker = (u8)(iteration ^ worker ^ 0x5aU);
            ((u8 *)object)[0] = marker;
            ((u8 *)object)[request - 1U] = (u8)~marker;
            if (((u8 *)object)[0] != marker ||
                ((u8 *)object)[request - 1U] != (u8)~marker) {
                failure_reason = 2U; /* payload did not survive */
            } else {
                checksum = (checksum << 7) ^ (checksum >> 3) ^
                           (uintptr_t)object ^ marker;
                if (!bench_parallel_release(object, NULL, NULL))
                    failure_reason = 3U; /* free rejected allocator object */
            }
        }
        if (failure_reason) {
            u64 detail = ((u64)(worker + 1U) << 56) |
                         ((u64)request << 40) |
                         ((u64)iteration << 8) | failure_reason;
            u64 expected = 0U;
            (void)__atomic_compare_exchange_n(
                &bench_parallel.work_failure_detail, &expected, detail,
                false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            return false;
        }
    }
    *checksum_out = checksum;
    return true;
}

static bool bench_parallel_cross_print(u32 allocator, u32 workload,
                                       u32 run)
{
    usize position = 0U;
    bench_profile_line[0] = '\0';
#define BENCH_CROSS_TEXT(value) \
    do { if (!bench_line_append_text(bench_profile_line, \
             sizeof(bench_profile_line), &position, (value))) return false; \
    } while (0)
#define BENCH_CROSS_NUM(value) \
    do { if (!bench_line_append_u64(bench_profile_line, \
             sizeof(bench_profile_line), &position, (u64)(value))) return false; \
    } while (0)
    BENCH_CROSS_TEXT("SLAB_NATIVE_PARALLEL_CROSS allocator=");
    BENCH_CROSS_TEXT(bench_allocator_name(allocator));
    BENCH_CROSS_TEXT(" workload=");
    BENCH_CROSS_TEXT(workload ? "mixed" : "same72");
    BENCH_CROSS_TEXT(" run="); BENCH_CROSS_NUM(run);
    BENCH_CROSS_TEXT(" owners=");
    for (u32 index = 0; index < BENCH_WORKERS; index++) {
        if (index) BENCH_CROSS_TEXT(",");
        BENCH_CROSS_NUM(bench_parallel.cross_owner_cpu[index]);
    }
    BENCH_CROSS_TEXT(" free_cpus=");
    for (u32 index = 0; index < BENCH_WORKERS; index++) {
        if (index) BENCH_CROSS_TEXT(",");
        BENCH_CROSS_NUM(bench_parallel.cross_free_cpu[index]);
    }
    BENCH_CROSS_TEXT(" ticks=");
    for (u32 index = 0; index < BENCH_WORKERS; index++) {
        if (index) BENCH_CROSS_TEXT(",");
        BENCH_CROSS_NUM(bench_parallel.cross_free_tsc[index]);
    }
    BENCH_CROSS_TEXT("\n");
    kprint("%s", bench_profile_line);
#undef BENCH_CROSS_TEXT
#undef BENCH_CROSS_NUM
    return true;
}

static void bench_parallel_cpu_print(u32 allocator, u32 workload, u32 run)
{
    for (u32 cpu = 0; cpu < bench_parallel.online_cpus; cpu++) {
        const bench_cpu_metrics_t *metrics = &bench_parallel.cpu_metrics[cpu];
        if (!metrics->seen)
            continue;
        kprint("SLAB_NATIVE_PARALLEL_CPU allocator=%s workload=%s run=%u cpu=%u seen=%u allocs=%u frees=%u\n",
               bench_allocator_name(allocator), workload ? "mixed" : "same72",
               run, cpu, metrics->seen, metrics->allocations, metrics->frees);
    }
}

static void bench_parallel_worker(void *argument)
{
    u32 worker = (u32)(uintptr_t)argument;
    if (worker >= BENCH_WORKERS) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&bench_parallel.done, 1U, __ATOMIC_RELEASE);
        return;
    }
    usize request = bench_parallel.request[worker];
    u32 cpu = cpu_current_index();
    if (cpu >= 32U) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&bench_parallel.done, 1U, __ATOMIC_RELEASE);
        return;
    }
    if (bench_parallel.rendezvous) {
        for (u32 spin = 0U; spin < BENCH_FREE_PROFILE_SPIN_LIMIT; spin++) {
            u32 current_cpu = cpu_current_index();
            if (current_cpu < 32U)
                __atomic_fetch_or(&bench_parallel.remote_wait_mask,
                                  1U << current_cpu, __ATOMIC_RELEASE);
            if (__atomic_load_n(&bench_parallel.go, __ATOMIC_ACQUIRE))
                break;
            if (spin + 1U == BENCH_FREE_PROFILE_SPIN_LIMIT) {
                __atomic_store_n(&bench_parallel.failed, 1U,
                                 __ATOMIC_RELEASE);
                __atomic_add_fetch(&bench_parallel.done, 1U,
                                   __ATOMIC_RELEASE);
                return;
            }
            __asm__ volatile("pause");
        }
    }
    __atomic_add_fetch(&bench_parallel.started, 1U, __ATOMIC_ACQ_REL);

    u64 checksum = 0U;
    if (!bench_parallel_work(worker, request, bench_parallel.iterations,
                             &checksum)) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
    }
    bench_parallel.checksum[worker] = checksum;
    /* Keep the preallocated per-class cross-free objects live until the BSP
     * has finished its own measured work. Otherwise the last worker to finish
     * can retire a slab while the coordinator is still allocating from that
     * class with interrupts disabled, where heap growth is correctly barred. */
    if (bench_parallel.rendezvous) {
        u64 release_wait_start = timer_uptime_ms();
        while (!__atomic_load_n(&bench_parallel.cross_release_go,
                                __ATOMIC_ACQUIRE) &&
               timer_uptime_ms() - release_wait_start <
                   BENCH_WORK_TIMEOUT_MS)
            (void)scheduler_yield();
        if (!__atomic_load_n(&bench_parallel.cross_release_go,
                             __ATOMIC_ACQUIRE))
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
    }
    u32 owner = (worker + 1U) % BENCH_WORKERS;
    void *cross_object = bench_parallel.cross_object[owner];
    u32 free_cpu = ~(u32)0;
    u64 free_ticks = 0U;
    if (!cross_object || !bench_contents(cross_object,
            bench_parallel.request[owner], (u8)(0x91U + owner)) ||
        !bench_parallel_release(cross_object, &free_cpu, &free_ticks)) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
    } else if (bench_parallel.cross_owner_cpu[owner] != free_cpu) {
        __atomic_add_fetch(&bench_parallel.cross_cpu_frees, 1U,
                           __ATOMIC_RELAXED);
    }
    bench_parallel.cross_free_cpu[owner] = free_cpu;
    bench_parallel.cross_free_tsc[owner] = free_ticks;
    bench_parallel.cross_object[owner] = NULL;
    __atomic_add_fetch(&bench_parallel.done, 1U, __ATOMIC_RELEASE);
}

static bool bench_parallel_run(u32 allocator, u32 workload, u32 *run_index)
{
    kernel_thread_t *threads[BENCH_WORKERS] = {0};
    u32 created = 0U, queued = 0U;
    u32 initial_run = *run_index;
    u32 online = cpu_online_count();
    u32 coordinator_cpu = cpu_current_index();
    bool rendezvous = online >= 2U;
    u64 start_ms = 0U, elapsed_ms = 0U;
    u64 elapsed_tsc_ticks = 0U;
    u64 start_tsc = 0U;
    u32 cpu_mask, cross_frees, peak_active, peak_operations;
    u32 started, done, measured_allocations = 0U, measured_frees = 0U;
    u64 rendezvous_flags = 0U;
    bool rendezvous_interrupts_disabled = false;
    u32 remote_cpu = ~(u32)0;

    if (!run_index || online > 32U || coordinator_cpu >= 32U)
        return false;
    memset(&bench_parallel, 0, sizeof(bench_parallel));
    bench_parallel.allocator = allocator;
    bench_parallel.workload = workload;
    bench_parallel.iterations = BENCH_PARALLEL_PAIRS;
    bench_parallel.coordinator_cpu = coordinator_cpu;
    bench_parallel.online_cpus = online;
    bench_parallel.rendezvous = rendezvous;
    for (u32 worker = 0; worker < BENCH_WORKERS; worker++)
        bench_parallel.request[worker] = workload
            ? (u16[]){16U, 72U, 192U, 384U}[worker] : 72U;

    /* Cross-free objects are prepared before any worker is published. This
     * keeps worker startup independent of a spinning rendezvous and lets
     * the measured interval begin when the first worker actually runs. Keep
     * local interrupts disabled across allocation and CPU capture so a
     * preemption cannot mislabel the allocation CPU. */
    for (u32 worker = 0; worker < BENCH_WORKERS; worker++) {
        u32 owner_cpu = ~(u32)0;
        void *object = bench_parallel_prepare_object(
            allocator, bench_parallel.request[worker], &owner_cpu);
        if (!object) {
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            goto cleanup;
        }
        bench_parallel.cross_object[worker] = object;
        bench_parallel.cross_owner_cpu[worker] = owner_cpu;
        bench_fill(object, bench_parallel.request[worker],
                   (u8)(0x91U + worker));
    }

    kprint("SLAB_NATIVE_PARALLEL_BEGIN allocator=%s workload=%s\n",
           bench_allocator_name(allocator), workload ? "mixed" : "same72");

    for (u32 worker = 0; worker < BENCH_WORKERS; worker++) {
        threads[worker] = thread_create_suspended(
            "allocator-bench", bench_parallel_worker,
            (void *)(uintptr_t)worker);
        if (!threads[worker]) {
            kprint("SLAB_NATIVE_PARALLEL_CREATE_FAIL worker=%u\n", worker);
            break;
        }
        created++;
    }
    kprint("SLAB_NATIVE_PARALLEL_CREATED workers=%u\n", created);
    if (created != BENCH_WORKERS)
        goto cleanup;

    if (rendezvous) {
        cpu_local_t *coordinator = cpu_current();
        if (!coordinator || !coordinator->bsp || coordinator_cpu >= 32U) {
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            goto cleanup;
        }
        /* Keep the bootstrap BSP as an active allocator participant while an
         * AP worker reaches the rendezvous.  No worker is queued until IF is
         * clear here, so the BSP cannot switch to one of them first. */
        rendezvous_flags = bench_irq_save_disable();
        rendezvous_interrupts_disabled = true;
        for (; queued < created; queued++) {
            if (!scheduler_enqueue(threads[queued])) {
                kprint("SLAB_NATIVE_PARALLEL_ENQUEUE_FAIL worker=%u\n",
                       queued);
                __atomic_store_n(&bench_parallel.failed, 1U,
                                 __ATOMIC_RELEASE);
                break;
            }
        }
        if (queued != created) {
            __atomic_store_n(&bench_parallel.go, 1U, __ATOMIC_RELEASE);
            bench_irq_restore(rendezvous_flags);
            rendezvous_interrupts_disabled = false;
            goto cleanup;
        }

        cpu_local_t *remote = NULL;
        for (u32 index = 0; index < cpu_count(); index++) {
            cpu_local_t *candidate = cpu_by_index(index);
            if (candidate && candidate->online && index != coordinator_cpu) {
                remote = candidate;
                break;
            }
        }
        if (!remote) {
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            __atomic_store_n(&bench_parallel.go, 1U, __ATOMIC_RELEASE);
            bench_irq_restore(rendezvous_flags);
            rendezvous_interrupts_disabled = false;
            goto cleanup;
        }
        remote_cpu = remote->index;
        u64 remote_tick_start = __atomic_load_n(
            &remote->scheduler_accounted_ticks, __ATOMIC_ACQUIRE);
        bool remote_ready = false;
        for (u32 spin = 0U; spin < BENCH_FREE_PROFILE_SPIN_LIMIT; spin++) {
            u32 mask = __atomic_load_n(&bench_parallel.remote_wait_mask,
                                       __ATOMIC_ACQUIRE);
            if (mask & (1U << remote->index)) {
                remote_ready = true;
                break;
            }
            u64 remote_ticks = __atomic_load_n(
                &remote->scheduler_accounted_ticks, __ATOMIC_ACQUIRE);
            if (remote_ticks - remote_tick_start >=
                BENCH_FREE_PROFILE_READY_TICKS)
                break;
            __asm__ volatile("pause");
        }
        if (!remote_ready) {
            kprint("SLAB_NATIVE_PARALLEL_RENDEZVOUS_FAIL bsp=%u remote=%u ready_mask=%x ap_ticks=%llu\n",
                   coordinator_cpu, remote->index,
                   __atomic_load_n(&bench_parallel.remote_wait_mask,
                                   __ATOMIC_ACQUIRE),
                   __atomic_load_n(&remote->scheduler_accounted_ticks,
                                   __ATOMIC_ACQUIRE) - remote_tick_start);
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            __atomic_store_n(&bench_parallel.go, 1U, __ATOMIC_RELEASE);
            bench_irq_restore(rendezvous_flags);
            rendezvous_interrupts_disabled = false;
            goto cleanup;
        }

        start_tsc = bench_tsc_read();
        __atomic_store_n(&bench_parallel.go, 1U, __ATOMIC_RELEASE);
        u64 coordinator_checksum = 0U;
        if (!bench_parallel_work(BENCH_WORKERS, 72U,
                                 BENCH_PARALLEL_PAIRS,
                                 &coordinator_checksum))
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        bench_parallel.checksum[BENCH_WORKERS] = coordinator_checksum;
        __atomic_store_n(&bench_parallel.cross_release_go, 1U,
                         __ATOMIC_RELEASE);

        /* Keep the BSP thread on its original CPU until the AP workers
         * finish. Both timestamp reads therefore use the same local TSC;
         * no cross-CPU TSC synchronization is assumed. AP scheduler ticks
         * bound this polling loop while BSP interrupts remain disabled. */
        u64 work_tick_start = __atomic_load_n(
            &remote->scheduler_accounted_ticks, __ATOMIC_ACQUIRE);
        u32 spin = 0U;
        while (__atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE) <
                   queued &&
               spin++ < BENCH_FREE_PROFILE_SPIN_LIMIT) {
            u64 remote_ticks = __atomic_load_n(
                &remote->scheduler_accounted_ticks, __ATOMIC_ACQUIRE);
            if (remote_ticks - work_tick_start >= BENCH_WORK_TIMEOUT_MS)
                break;
            __asm__ volatile("pause");
        }
        done = __atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE);
        started = __atomic_load_n(&bench_parallel.started, __ATOMIC_ACQUIRE);
        if (done != queued || started != BENCH_WORKERS) {
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            bench_irq_restore(rendezvous_flags);
            rendezvous_interrupts_disabled = false;
            kprint("SLAB_NATIVE_PARALLEL_WORK_FAIL started=%u done=%u queued=%u remote_ticks=%llu\n",
                   started, done, queued,
                   __atomic_load_n(&remote->scheduler_accounted_ticks,
                                   __ATOMIC_ACQUIRE) - work_tick_start);
            goto cleanup;
        }
        u64 finish_tsc = bench_tsc_read();
        if (cpu_current_index() != coordinator_cpu || finish_tsc < start_tsc) {
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            bench_irq_restore(rendezvous_flags);
            rendezvous_interrupts_disabled = false;
            goto cleanup;
        }
        elapsed_tsc_ticks = finish_tsc - start_tsc;
        bench_irq_restore(rendezvous_flags);
        rendezvous_interrupts_disabled = false;
    } else {
        rendezvous_flags = bench_irq_save_disable();
        rendezvous_interrupts_disabled = true;
        start_ms = timer_uptime_ms();
        start_tsc = bench_tsc_read();
        __atomic_store_n(&bench_parallel.go, 1U, __ATOMIC_RELEASE);
        for (; queued < created; queued++) {
            if (!scheduler_enqueue(threads[queued])) {
                kprint("SLAB_NATIVE_PARALLEL_ENQUEUE_FAIL worker=%u\n",
                       queued);
                break;
            }
        }
        bench_irq_restore(rendezvous_flags);
        rendezvous_interrupts_disabled = false;
        if (queued != created)
            goto cleanup;
    }

    if (queued != created)
        goto cleanup;

    if (!rendezvous) {
        u64 work_start_ms = timer_uptime_ms();
        while (__atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE) <
                   queued &&
               timer_uptime_ms() - work_start_ms < BENCH_WORK_TIMEOUT_MS)
            (void)scheduler_yield();
        done = __atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE);
        started = __atomic_load_n(&bench_parallel.started, __ATOMIC_ACQUIRE);
        if (done != queued || started != BENCH_WORKERS) {
            kprint("SLAB_NATIVE_PARALLEL_WORK_FAIL started=%u done=%u queued=%u elapsed_ms=%llu failed=%u\n",
                   started, done, queued, timer_uptime_ms() - work_start_ms,
                   __atomic_load_n(&bench_parallel.failed,
                                   __ATOMIC_ACQUIRE));
            goto cleanup;
        }
        elapsed_ms = timer_uptime_ms() - start_ms;
        u64 finish_tsc = bench_tsc_read();
        if (cpu_current_index() != coordinator_cpu || finish_tsc < start_tsc) {
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            goto cleanup;
        }
        elapsed_tsc_ticks = finish_tsc - start_tsc;
    }

    done = __atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE);
    started = __atomic_load_n(&bench_parallel.started, __ATOMIC_ACQUIRE);
    for (u32 cpu = 0; cpu < 32U; cpu++) {
        measured_allocations += bench_parallel.cpu_metrics[cpu].allocations;
        measured_frees += bench_parallel.cpu_metrics[cpu].frees;
    }

    cpu_mask = __atomic_load_n(&bench_parallel.cpu_mask, __ATOMIC_ACQUIRE);
    cross_frees = __atomic_load_n(&bench_parallel.cross_cpu_frees,
                                  __ATOMIC_ACQUIRE);
    u32 overlap_mask = __atomic_load_n(&bench_parallel.overlap_cpu_mask,
                                       __ATOMIC_ACQUIRE);
    u64 work_failure_detail = __atomic_load_n(
        &bench_parallel.work_failure_detail, __ATOMIC_ACQUIRE);
    peak_active = overlap_mask ? bench_popcount32(overlap_mask) :
                  (cpu_mask ? 1U : 0U);
    peak_operations = overlap_mask ? bench_popcount32(overlap_mask) :
                      (cpu_mask ? 1U : 0U);
    bench_parallel.peak_active_cpus = peak_active;
    bench_parallel.peak_active_allocator_operations = peak_operations;
    if (__atomic_load_n(&bench_parallel.failed, __ATOMIC_ACQUIRE) ||
        measured_allocations !=
            (BENCH_WORKERS + (rendezvous ? 1U : 0U)) *
                BENCH_PARALLEL_PAIRS ||
        measured_frees != measured_allocations + BENCH_WORKERS ||
        !cpu_mask || !peak_active ||
        (online >= 2U && ((cpu_mask & (1U << coordinator_cpu)) == 0U ||
                          (cpu_mask & (1U << remote_cpu)) == 0U ||
                          (__atomic_load_n(&bench_parallel.remote_wait_mask,
                                          __ATOMIC_ACQUIRE) &
                           (1U << remote_cpu)) == 0U ||
                          !cross_frees ||
                          peak_active < 2U || peak_operations < 2U))) {
        kprint("SLAB_NATIVE_PARALLEL_REJECT allocator=%s workload=%s online=%u started=%u done=%u allocs=%u frees=%u coordinator_cpu=%u remote_cpu=%u cpu_mask=%x worker_wait_mask=%x peak_active_cpus=%u peak_active_operations=%u cross_cpu_frees=%u failed=%u work_failure_detail=%llu checksums=%llu,%llu,%llu,%llu,%llu\n",
               bench_allocator_name(allocator),
               workload ? "mixed" : "same72", online, started, done,
               measured_allocations, measured_frees, coordinator_cpu,
               remote_cpu, cpu_mask,
               __atomic_load_n(&bench_parallel.remote_wait_mask,
                               __ATOMIC_ACQUIRE),
               peak_active, peak_operations, cross_frees,
               __atomic_load_n(&bench_parallel.failed, __ATOMIC_ACQUIRE),
               (unsigned long long)work_failure_detail,
               bench_parallel.checksum[0], bench_parallel.checksum[1],
               bench_parallel.checksum[2], bench_parallel.checksum[3],
               bench_parallel.checksum[4]);
        if (work_failure_detail) {
            kprint("SLAB_NATIVE_PARALLEL_WORK_FAILURE allocator=%s workload=%s worker=%u request=%u iteration=%u reason=%u\n",
                   bench_allocator_name(allocator),
                   workload ? "mixed" : "same72",
                   (u32)((work_failure_detail >> 56) & 0xffU),
                   (u32)((work_failure_detail >> 40) & 0xffffU),
                   (u32)((work_failure_detail >> 8) & 0xffffffffU),
                   (u32)(work_failure_detail & 0xffU));
        }
        goto cleanup;
    }
    for (u32 worker = 0; worker < BENCH_WORKERS; worker++) {
        if (!bench_parallel.checksum[worker]) {
            kprint("SLAB_NATIVE_PARALLEL_REJECT allocator=%s workload=%s zero_checksum_worker=%u\n",
                   bench_allocator_name(allocator),
                   workload ? "mixed" : "same72", worker);
            goto cleanup;
        }
    }

    kprint("SLAB_NATIVE_PARALLEL allocator=%s workload=%s run=%u pairs=%u workers=%u started=%u done=%u failed=%u allocs=%u frees=%u coordinator_pairs=%u coordinator_cpu=%u remote_cpu=%u elapsed_ms=%llu elapsed_tsc_ticks=%llu elapsed_unit=%s cpu_mask=%x worker_wait_mask=%x peak_active_cpus=%u peak_active_operations=%u cross_cpu_frees=%u checksum=%llu,%llu,%llu,%llu,%llu\n",
           bench_allocator_name(allocator), workload ? "mixed" : "same72",
           *run_index, BENCH_PARALLEL_PAIRS, BENCH_WORKERS,
           started, done,
           __atomic_load_n(&bench_parallel.failed, __ATOMIC_ACQUIRE),
           measured_allocations, measured_frees,
           rendezvous ? BENCH_PARALLEL_PAIRS : 0U, coordinator_cpu, remote_cpu,
           elapsed_ms, elapsed_tsc_ticks,
           "tsc_ticks", cpu_mask,
           __atomic_load_n(&bench_parallel.remote_wait_mask,
                           __ATOMIC_ACQUIRE),
           peak_active, peak_operations, cross_frees,
           bench_parallel.checksum[0], bench_parallel.checksum[1],
           bench_parallel.checksum[2], bench_parallel.checksum[3],
           bench_parallel.checksum[4]);
    bench_parallel_cpu_print(allocator, workload, *run_index);
    if (rendezvous && !bench_parallel_cross_print(
            allocator, workload, *run_index))
        goto cleanup;
    (*run_index)++;

    for (u32 spins = 0U; spins < 100000000U; spins++) {
        bool terminated = true;
        for (u32 worker = 0; worker < queued; worker++)
            if (threads[worker]->state != THREAD_STATE_TERMINATED ||
                scheduler_thread_is_running(threads[worker]))
                terminated = false;
        if (terminated)
            goto cleanup;
        (void)scheduler_yield();
    }
    panic("slab benchmark: worker failed to terminate");

cleanup:
    if (rendezvous_interrupts_disabled) {
        __atomic_store_n(&bench_parallel.go, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&bench_parallel.cross_release_go, 1U,
                         __ATOMIC_RELEASE);
        bench_irq_restore(rendezvous_flags);
        rendezvous_interrupts_disabled = false;
    }
    __atomic_store_n(&bench_parallel.cross_release_go, 1U,
                     __ATOMIC_RELEASE);
    if (queued) {
        u64 cleanup_start = timer_uptime_ms();
        while (__atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE) <
                   queued &&
               timer_uptime_ms() - cleanup_start < BENCH_WORK_TIMEOUT_MS)
            (void)scheduler_yield();
        if (__atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE) < queued)
            panic("slab benchmark: worker cleanup timed out");
        cleanup_start = timer_uptime_ms();
        for (;;) {
            bool terminated = true;
            for (u32 worker = 0; worker < queued; worker++)
                if (threads[worker]->state != THREAD_STATE_TERMINATED ||
                    scheduler_thread_is_running(threads[worker]))
                    terminated = false;
            if (terminated)
                break;
            if (timer_uptime_ms() - cleanup_start >=
                BENCH_READY_TIMEOUT_MS)
                panic("slab benchmark: worker termination timed out");
            (void)scheduler_yield();
        }
    }
    for (u32 worker = 0; worker < created; worker++) {
        if (threads[worker] && !scheduler_thread_is_running(threads[worker]) &&
            (threads[worker]->state == THREAD_STATE_TERMINATED ||
             worker >= queued))
            (void)thread_destroy(threads[worker]);
    }
    for (u32 worker = 0; worker < BENCH_WORKERS; worker++) {
        if (bench_parallel.cross_object[worker]) {
            (void)bench_release(allocator,
                                bench_parallel.cross_object[worker]);
            bench_parallel.cross_object[worker] = NULL;
        }
    }
    return *run_index != initial_run &&
           __atomic_load_n(&bench_parallel.failed, __ATOMIC_ACQUIRE) == 0U &&
           queued == created;
}

static bool bench_first_refill_and_retirement(void)
{
    slab_classifier_stats_t before, after;
    u64 heap_before, heap_after_alloc, heap_after_free;
    void *object;
    bench_stamp_t start, end, alloc_start, alloc_end;
    u64 alloc_ticks = 0U, free_ticks = 0U;
    u32 alloc_scheduler_ticks = 0U, free_scheduler_ticks = 0U;
    u32 alloc_current_cpu, free_current_cpu;
    bool alloc_timed, free_timed;

    if (!slab_get_classifier_stats(&before))
        return false;
    heap_before = heap_get_used_size();
    start = bench_stamp();
    object = slab_alloc(16U);
    end = bench_stamp();
    alloc_start = start;
    alloc_end = end;
    alloc_current_cpu = cpu_current_index();
    alloc_timed = bench_lifecycle_interval(start, end, &alloc_ticks,
                                           &alloc_scheduler_ticks);
    if (!object || ((uintptr_t)object & 15U))
        return false;
    memset(object, 0xa5, 16U);
    if (!bench_contents(object, 16U, 0xa5U))
        return false;
    heap_after_alloc = heap_get_used_size();
    start = bench_stamp();
    bool freed = slab_try_free(object);
    end = bench_stamp();
    free_current_cpu = cpu_current_index();
    free_timed = bench_lifecycle_interval(start, end, &free_ticks,
                                          &free_scheduler_ticks);
    if (!freed || !slab_get_classifier_stats(&after))
        return false;
    heap_after_free = heap_get_used_size();
    kprint("SLAB_NATIVE_LIFECYCLE first_refill_cpu_same=%u first_refill_start_cpu=%u first_refill_end_cpu=%u first_refill_current_cpu=%u first_refill_start_tsc=%llu first_refill_end_tsc=%llu first_refill_tsc_ticks=%llu first_refill_scheduler_ticks=%u retirement_cpu_same=%u retirement_start_cpu=%u retirement_end_cpu=%u retirement_current_cpu=%u retirement_start_tsc=%llu retirement_end_tsc=%llu retirement_tsc_ticks=%llu retirement_scheduler_ticks=%u heap_used_before=%llu heap_used_with_slab=%llu heap_used_after_retire=%llu classifier_retained_before=%llu classifier_retained_after=%llu\n",
           alloc_timed ? 1U : 0U, alloc_start.cpu, alloc_end.cpu,
           alloc_current_cpu, alloc_start.tsc, alloc_end.tsc, alloc_ticks,
           alloc_scheduler_ticks,
           free_timed ? 1U : 0U, start.cpu, end.cpu, free_current_cpu,
           start.tsc, end.tsc, free_ticks, free_scheduler_ticks,
           heap_before, heap_after_alloc, heap_after_free,
           before.retained_payload_bytes, after.retained_payload_bytes);
    return heap_after_alloc >= heap_before &&
           heap_after_free >= heap_before &&
           after.retained_payload_bytes >= before.retained_payload_bytes;
}

static u64 bench_timer_pair_min(void)
{
    u64 minimum = (u64)-1;
    for (u32 sample = 0; sample < 128U; sample++) {
        u64 before = bench_tsc_read();
        __asm__ volatile("" ::: "memory");
        u64 after = bench_tsc_read();
        if (after - before < minimum)
            minimum = after - before;
    }
    return minimum;
}

static bool bench_tracker_overhead_print(u64 timer_pair_min)
{
    enum { iterations = 512U, trials = 7U, max_attempts = 56U };
    u64 totals[trials];

    u32 completed = 0U;
    u32 attempts = 0U;
    while (completed < trials && attempts < max_attempts) {
        attempts++;
        u64 outer_flags = bench_irq_save_disable();
        u32 cpu = cpu_current_index();
        if (cpu >= 32U) {
            bench_irq_restore(outer_flags);
            return false;
        }
        u64 start = bench_tsc_read();
        for (u32 operation = 0; operation < iterations; operation++) {
            u64 flags = bench_irq_save_disable();
            bench_overlap_enter();
            __asm__ volatile("" ::: "memory");
            bench_overlap_leave(cpu);
            bench_irq_restore(flags);
        }
        u64 end = bench_tsc_read();
        u32 end_cpu = cpu_current_index();
        bool valid = end > start && end_cpu == cpu;
        bench_irq_restore(outer_flags);
        /* Check CPU identity before restoring the caller's interrupt state.
         * Once interrupts are enabled, this thread may be preempted and
         * migrated, which says nothing about the timed interval itself.
         * QEMU TCG can return equal TSC values for a short interval. Discard
         * that sample and retry; never report a zero or synthesized duration. */
        if (valid)
            totals[completed++] = end - start;
    }
    if (completed != trials) {
        kprint("SLAB_NATIVE_TRACKER_REJECT valid=%u trials=%u attempts=%u max_attempts=%u reason=timer-resolution-or-cpu-change\n",
               completed, trials, attempts, max_attempts);
        return false;
    }
    kprint("SLAB_NATIVE_TRACKER_OVERHEAD timing=lfence-rdtsc units=tsc_ticks iterations=%u trials=%u attempts=%u tsc_pair_min=%llu ticks=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
           iterations, trials, attempts, timer_pair_min,
           totals[0], totals[1], totals[2], totals[3],
           totals[4], totals[5], totals[6]);
    return true;
}

bool slab_guest_benchmark_run(void)
{
    static const u16 requests[] = {16U, 72U, 192U, 384U};
    const u64 overhead = bench_timer_pair_min();
    slab_classifier_stats_t classifier;
    u32 run_index = 0U;
    kprint("SLAB_NATIVE_BEGIN version=6 allocators=heap,slab,cpu-slab timing=lfence-rdtsc units=tsc_ticks requests=16,72,192,384 population=%u transient=%u samples=%u batch=%u warmups=%u parallel_pairs=%u online=%u tsc_pair_min=%llu tick_resolution_ms=1 free_profile=%u free_profile_trials=%u free_profile_ops=1024 free_profile_samples=64 free_profile_stride=16 free_profile_stages=end_to_end,classify,lock,token_descriptor,slot_bitmap,mutate_transition,heap_release smp_rendezvous=bsp-participant-ap-wait overlap_tracking=per-cpu-sequentially-consistent-flags log_capacity=%u\n",
           BENCH_POPULATION, BENCH_BATCH, BENCH_SAMPLES, BENCH_BATCH,
           BENCH_WARMUPS, BENCH_PARALLEL_PAIRS, cpu_online_count(),
           overhead,
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
           1U,
#else
           0U,
#endif
           BENCH_FREE_PROFILE_TRIALS,
           (u32)KERNEL_BOOT_LOG_CAPACITY);
    if (cpu_online_count() < 1U || !bench_first_refill_and_retirement())
        return false;
    for (u32 request_index = 0;
         request_index < sizeof(requests) / sizeof(requests[0]);
         request_index++) {
        for (u32 allocator = 0; allocator < 3U; allocator++) {
            if (!bench_measure_steady_path(allocator,
                                           requests[request_index]))
                return false;
        }
    }
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    if (!bench_profile_free_paths())
        return false;
#endif
    for (u32 allocator = 0; allocator < 3U; allocator++)
        for (u32 workload = 0; workload < 2U; workload++)
            if (!bench_parallel_run(allocator, workload, &run_index))
                return false;
    if (!bench_tracker_overhead_print(overhead))
        return false;
    if (!slab_cpu_drain_all())
        return false;
    if (!slab_verify())
        return false;
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_stats_t stats;
        if (!slab_get_class_stats(class_index, &stats) || stats.live_slots ||
            stats.cached_slots ||
            stats.partial_slabs || stats.full_slabs)
            return false;
    }
    for (u32 request_index = 0;
         request_index < sizeof(requests) / sizeof(requests[0]);
         request_index++) {
        u32 class_index = bench_class_index(requests[request_index]);
        slab_class_stats_t stats;
        if (!slab_get_class_stats(class_index, &stats))
            return false;
        kprint("SLAB_NATIVE_CLASS request=%u class_size=%u lock_acquisitions=%llu allocations=%llu frees=%llu bitmap_words=%llu backing_allocations=%llu backing_releases=%llu peak_live_slots=%llu live_slots=%llu cached_slots=%llu cache_hits=%llu cache_misses=%llu cache_refills=%llu cache_refill_conflicts=%llu cache_refill_objects=%llu cache_drain_objects=%llu cache_overflows=%llu cross_cpu_frees=%llu partial_slabs=%llu full_slabs=%llu\n",
               requests[request_index], slab_class_size(class_index),
               stats.lock_acquisitions, stats.allocations, stats.frees,
               stats.bitmap_words_examined, stats.backing_allocations,
               stats.backing_releases, stats.peak_live_slots,
               stats.live_slots, stats.cached_slots, stats.cache_hits,
               stats.cache_misses, stats.cache_refills,
               stats.cache_refill_conflicts, stats.cache_refill_objects,
               stats.cache_drain_objects, stats.cache_overflows,
               stats.cross_cpu_frees, stats.partial_slabs, stats.full_slabs);
    }
    if (!slab_get_classifier_stats(&classifier))
        return false;
    kprint("SLAB_NATIVE_MEMORY heap_used_bytes=%llu cpu_cache_metadata_bytes=%llu classifier_payload_bytes=%llu classifier_peak_payload_bytes=%llu static_root_bytes=%llu middle_payload_bytes=%llu leaf_payload_bytes=%llu\n",
           heap_get_used_size(), slab_cpu_cache_metadata_bytes(),
           classifier.retained_payload_bytes,
           classifier.peak_payload_bytes, classifier.static_root_bytes,
           classifier.middle_payload_bytes, classifier.leaf_payload_bytes);
    kprint("SLAB_NATIVE_END samples=%u parallel_runs=%u status=PASS ownership=pass payload=pass\n",
           3U * 4U * BENCH_SAMPLES * 2U, run_index);
    return run_index == 6U;
}
#endif
