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
#define BENCH_SAMPLES 7U
#define BENCH_WARMUPS 2U
#define BENCH_MAX_ATTEMPTS 128U
#define BENCH_PARALLEL_PAIRS 512U
#define BENCH_READY_TIMEOUT_MS 10000U
#define BENCH_WORK_TIMEOUT_MS 60000U

typedef struct {
    u64 tsc;
    u64 scheduler_ticks;
    u32 cpu;
    bool valid;
} bench_stamp_t;

typedef struct {
    volatile u32 started;
    volatile u32 done;
    volatile u32 failed;
    volatile u32 cpu_mask;
    volatile u32 cross_cpu_frees;
    volatile u32 active_mask;
    volatile u32 active_cpu_count[32];
    volatile u32 peak_active_cpus;
    volatile u64 start_ms;
    u32 allocator; /* zero is kmalloc/kfree; one is the slab API. */
    u32 workload;  /* zero is one hot class; one varies by worker. */
    u32 iterations;
    u16 request[ BENCH_WORKERS ];
    void *cross_object[BENCH_WORKERS];
    u32 cross_owner_cpu[BENCH_WORKERS];
    u64 checksum[BENCH_WORKERS];
} bench_parallel_state_t;

static void *bench_objects[BENCH_POPULATION];
static u8 bench_expected[BENCH_POPULATION];
static bench_parallel_state_t bench_parallel;

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
    return allocator ? slab_alloc(size) : kmalloc(size);
}

static bool bench_release(u32 allocator, void *object)
{
    if (allocator)
        return slab_try_free(object);
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
                       allocator ? "slab" : "heap", (u32)request, trial,
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
                       allocator ? "slab" : "heap", (u32)request, trial,
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
           allocator ? "slab" : "heap", (u32)request,
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
    __atomic_fetch_or(&bench_parallel.cpu_mask, 1U << cpu,
                      __ATOMIC_RELAXED);
    u32 previous = __atomic_fetch_add(&bench_parallel.active_cpu_count[cpu],
                                      1U, __ATOMIC_ACQ_REL);
    if (!previous)
        __atomic_fetch_or(&bench_parallel.active_mask, 1U << cpu,
                          __ATOMIC_ACQ_REL);
    u32 active = bench_popcount32(__atomic_load_n(
        &bench_parallel.active_mask, __ATOMIC_ACQUIRE));
    u32 peak = __atomic_load_n(&bench_parallel.peak_active_cpus,
                               __ATOMIC_RELAXED);
    while (active > peak && !__atomic_compare_exchange_n(
               &bench_parallel.peak_active_cpus, &peak, active, true,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void bench_overlap_leave(u32 entered_cpu)
{
    u32 cpu = cpu_current_index();
    if (cpu >= 32U || entered_cpu >= 32U) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        return;
    }
    if (cpu != entered_cpu)
        __atomic_fetch_or(&bench_parallel.cpu_mask, 1U << cpu,
                          __ATOMIC_RELAXED);
    u32 previous = __atomic_fetch_sub(
        &bench_parallel.active_cpu_count[entered_cpu],
                                      1U, __ATOMIC_ACQ_REL);
    if (!previous) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        return;
    }
    if (previous == 1U)
        __atomic_fetch_and(&bench_parallel.active_mask,
                           ~(1U << entered_cpu),
                           __ATOMIC_ACQ_REL);
}

static void *bench_parallel_allocate(usize size)
{
    u64 flags = bench_irq_save_disable();
    u32 cpu = cpu_current_index();
    bench_overlap_enter();
    void *object = bench_allocate(bench_parallel.allocator, size);
    bench_overlap_leave(cpu);
    bench_irq_restore(flags);
    return object;
}

static bool bench_parallel_release(void *object, u32 *called_cpu)
{
    u64 flags = bench_irq_save_disable();
    u32 cpu = cpu_current_index();
    bench_overlap_enter();
    bool success = bench_release(bench_parallel.allocator, object);
    bench_overlap_leave(cpu);
    if (called_cpu)
        *called_cpu = cpu;
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
    if (__atomic_fetch_add(&bench_parallel.started, 1U,
                           __ATOMIC_ACQ_REL) == 0U)
        __atomic_store_n(&bench_parallel.start_ms, timer_uptime_ms(),
                         __ATOMIC_RELEASE);

    u64 checksum = (u64)(worker + 1U);
    for (u32 iteration = 0; iteration < bench_parallel.iterations;
         iteration++) {
        void *object = bench_parallel_allocate(request);
        if (!object) {
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
            break;
        }
        u8 marker = (u8)(iteration ^ worker ^ 0x5aU);
        ((u8 *)object)[0] = marker;
        ((u8 *)object)[request - 1U] = (u8)~marker;
        if (((u8 *)object)[0] != marker ||
            ((u8 *)object)[request - 1U] != (u8)~marker)
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
        checksum = (checksum << 7) ^ (checksum >> 3) ^
                   (uintptr_t)object ^ marker;
        if (!bench_parallel_release(object, NULL))
            __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
    }
    bench_parallel.checksum[worker] = checksum;
    u32 owner = (worker + 1U) % BENCH_WORKERS;
    void *cross_object = bench_parallel.cross_object[owner];
    u32 free_cpu = ~(u32)0;
    if (!cross_object || !bench_contents(cross_object,
            bench_parallel.request[owner], (u8)(0x91U + owner)) ||
        !bench_parallel_release(cross_object, &free_cpu)) {
        __atomic_store_n(&bench_parallel.failed, 1U, __ATOMIC_RELEASE);
    } else if (bench_parallel.cross_owner_cpu[owner] != free_cpu) {
        __atomic_add_fetch(&bench_parallel.cross_cpu_frees, 1U,
                           __ATOMIC_RELAXED);
    }
    bench_parallel.cross_object[owner] = NULL;
    __atomic_add_fetch(&bench_parallel.done, 1U, __ATOMIC_RELEASE);
}

static bool bench_parallel_run(u32 allocator, u32 workload, u32 *run_index)
{
    kernel_thread_t *threads[BENCH_WORKERS] = {0};
    u32 created = 0U, queued = 0U;
    u32 initial_run = *run_index;
    u32 online = cpu_online_count();
    u64 start_ms, elapsed_ms;
    u32 cpu_mask, cross_frees, peak_active;

    memset(&bench_parallel, 0, sizeof(bench_parallel));
    bench_parallel.allocator = allocator;
    bench_parallel.workload = workload;
    bench_parallel.iterations = BENCH_PARALLEL_PAIRS;
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
           allocator ? "slab" : "heap", workload ? "mixed" : "same72");

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
    for (; queued < created; queued++) {
        if (!scheduler_enqueue(threads[queued])) {
            kprint("SLAB_NATIVE_PARALLEL_ENQUEUE_FAIL worker=%u\n", queued);
            break;
        }
    }
    if (queued != created)
        goto cleanup;
    kprint("SLAB_NATIVE_PARALLEL_QUEUED workers=%u\n", queued);
    u64 work_start_ms = timer_uptime_ms();
    while (__atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE) < queued &&
           timer_uptime_ms() - work_start_ms < BENCH_WORK_TIMEOUT_MS)
        (void)scheduler_yield();
    start_ms = __atomic_load_n(&bench_parallel.start_ms, __ATOMIC_ACQUIRE);
    elapsed_ms = timer_uptime_ms() - start_ms;
    u32 done = __atomic_load_n(&bench_parallel.done, __ATOMIC_ACQUIRE);
    if (done != queued || !elapsed_ms) {
        kprint("SLAB_NATIVE_PARALLEL_WORK_FAIL done=%u queued=%u elapsed_ms=%llu failed=%u\n",
               done, queued, elapsed_ms,
               __atomic_load_n(&bench_parallel.failed, __ATOMIC_ACQUIRE));
        goto cleanup;
    }

    cpu_mask = __atomic_load_n(&bench_parallel.cpu_mask, __ATOMIC_ACQUIRE);
    cross_frees = __atomic_load_n(&bench_parallel.cross_cpu_frees,
                                  __ATOMIC_ACQUIRE);
    peak_active = __atomic_load_n(&bench_parallel.peak_active_cpus,
                                  __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&bench_parallel.failed, __ATOMIC_ACQUIRE) ||
        !cpu_mask || !peak_active ||
        (online >= 2U && ((cpu_mask & 3U) != 3U || !cross_frees ||
                          peak_active < 2U))) {
        kprint("SLAB_NATIVE_PARALLEL_REJECT allocator=%s workload=%s online=%u cpu_mask=%x peak_active_cpus=%u cross_cpu_frees=%u failed=%u checksums=%llu,%llu,%llu,%llu\n",
               allocator ? "slab" : "heap",
               workload ? "mixed" : "same72", online, cpu_mask,
               peak_active, cross_frees,
               __atomic_load_n(&bench_parallel.failed, __ATOMIC_ACQUIRE),
               bench_parallel.checksum[0], bench_parallel.checksum[1],
               bench_parallel.checksum[2], bench_parallel.checksum[3]);
        goto cleanup;
    }
    for (u32 worker = 0; worker < BENCH_WORKERS; worker++) {
        if (!bench_parallel.checksum[worker]) {
            kprint("SLAB_NATIVE_PARALLEL_REJECT allocator=%s workload=%s zero_checksum_worker=%u\n",
                   allocator ? "slab" : "heap",
                   workload ? "mixed" : "same72", worker);
            goto cleanup;
        }
    }

    kprint("SLAB_NATIVE_PARALLEL allocator=%s workload=%s run=%u pairs=%u workers=%u elapsed_ms=%llu cpu_mask=%x peak_active_cpus=%u cross_cpu_frees=%u checksum=%llu,%llu,%llu,%llu\n",
           allocator ? "slab" : "heap", workload ? "mixed" : "same72",
           *run_index, BENCH_PARALLEL_PAIRS, BENCH_WORKERS, elapsed_ms,
           cpu_mask, peak_active, cross_frees,
           bench_parallel.checksum[0], bench_parallel.checksum[1],
           bench_parallel.checksum[2], bench_parallel.checksum[3]);
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

bool slab_guest_benchmark_run(void)
{
    static const u16 requests[] = {16U, 72U, 192U, 384U};
    const u64 overhead = bench_timer_pair_min();
    slab_classifier_stats_t classifier;
    u32 run_index = 0U;
    kprint("SLAB_NATIVE_BEGIN version=1 timing=lfence-rdtsc units=tsc_ticks requests=16,72,192,384 population=%u transient=%u samples=%u batch=%u warmups=%u parallel_pairs=%u online=%u tsc_pair_min=%llu tick_resolution_ms=1 preemption_filter=same_cpu_and_unchanged_local_scheduler_tick\n",
           BENCH_POPULATION, BENCH_BATCH, BENCH_SAMPLES, BENCH_BATCH,
           BENCH_WARMUPS, BENCH_PARALLEL_PAIRS, cpu_online_count(),
           overhead);
    if (cpu_online_count() < 1U || !bench_first_refill_and_retirement())
        return false;
    for (u32 request_index = 0;
         request_index < sizeof(requests) / sizeof(requests[0]);
         request_index++) {
        for (u32 allocator = 0; allocator < 2U; allocator++) {
            if (!bench_measure_steady_path(allocator,
                                           requests[request_index]))
                return false;
        }
    }
    for (u32 allocator = 0; allocator < 2U; allocator++)
        for (u32 workload = 0; workload < 2U; workload++)
            if (!bench_parallel_run(allocator, workload, &run_index))
                return false;
    if (!slab_verify())
        return false;
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_stats_t stats;
        if (!slab_get_class_stats(class_index, &stats) || stats.live_slots ||
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
        kprint("SLAB_NATIVE_CLASS request=%u class_size=%u lock_acquisitions=%llu allocations=%llu frees=%llu bitmap_words=%llu backing_allocations=%llu backing_releases=%llu peak_live_slots=%llu live_slots=%llu partial_slabs=%llu full_slabs=%llu\n",
               requests[request_index], slab_class_size(class_index),
               stats.lock_acquisitions, stats.allocations, stats.frees,
               stats.bitmap_words_examined, stats.backing_allocations,
               stats.backing_releases, stats.peak_live_slots,
               stats.live_slots, stats.partial_slabs, stats.full_slabs);
    }
    if (!slab_get_classifier_stats(&classifier))
        return false;
    kprint("SLAB_NATIVE_MEMORY heap_used_bytes=%llu classifier_payload_bytes=%llu classifier_peak_payload_bytes=%llu static_root_bytes=%llu middle_payload_bytes=%llu leaf_payload_bytes=%llu\n",
           heap_get_used_size(), classifier.retained_payload_bytes,
           classifier.peak_payload_bytes, classifier.static_root_bytes,
           classifier.middle_payload_bytes, classifier.leaf_payload_bytes);
    kprint("SLAB_NATIVE_END samples=%u parallel_runs=%u status=PASS ownership=pass payload=pass\n",
           4U * 2U * BENCH_SAMPLES * 2U, run_index);
    return run_index == 4U;
}
#endif
