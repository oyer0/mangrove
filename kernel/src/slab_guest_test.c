/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef SLAB_CORE_GUEST_TEST
#include <cpu.h>
#include <address_layout.h>
#include <heap.h>
#include <kprint.h>
#include <panic.h>
#include <pmm.h>
#include <scheduler.h>
#include <slab.h>
#include <string.h>
#include <vmm.h>

#ifndef SLAB_DEEP_VALIDATION
#define SLAB_DEEP_VALIDATION 0
#endif

#define SLAB_GUEST_WORKERS 4U
#define SLAB_GUEST_OBJECTS 48U
#define SLAB_GUEST_CLASS_OBJECTS 251U
#define SLAB_GUEST_RECLAIM_EVERY 4U

static void *slab_guest_class_objects[SLAB_GUEST_CLASS_OBJECTS];
static void *slab_guest_worker_objects[SLAB_GUEST_WORKERS]
                                     [SLAB_GUEST_OBJECTS];
static u32 slab_guest_allocation_cpu[SLAB_GUEST_WORKERS]
                                    [SLAB_GUEST_OBJECTS];
static volatile u32 slab_guest_worker_ready;
static volatile u32 slab_guest_worker_done;
static volatile u32 slab_guest_worker_cpu_mask;
static volatile u32 slab_guest_cross_cpu_frees;
static volatile u32 slab_guest_worker_expected;
static volatile u32 slab_guest_barrier_armed;
static volatile u32 slab_guest_worker_failed;
static volatile u32 slab_guest_cleanup_own;
static volatile u32 slab_guest_cpu_cache_mode;
static volatile u32 slab_guest_allocator_active[CPU_MAX_COUNT];
static volatile u32 slab_guest_reclaimer_active[CPU_MAX_COUNT];
static volatile u32 slab_guest_reclaim_cpu_mask;
static volatile u32 slab_guest_reclaim_calls;
static volatile u32 slab_guest_reclaim_overlap;
static volatile u32 slab_guest_reclaim_busy;
static volatile u32 slab_guest_reclaim_record_limit;
static volatile u32 slab_guest_reclaim_object_limit;
static volatile u64 slab_guest_reclaim_objects;
static volatile u64 slab_guest_reclaim_pages;
static volatile u64 slab_guest_reclaim_records;
static void *slab_guest_remote_free_object;
static u32 slab_guest_remote_free_owner_cpu;
static volatile u32 slab_guest_remote_free_done;
static volatile u32 slab_guest_remote_free_passed;

static void slab_guest_remote_free_worker(void *argument)
{
    (void)argument;
    for (u64 attempt = 0; attempt < 1000000ULL; attempt++) {
        u64 entry_flags;
        u32 cpu;

        /* Keep the observed CPU stable through this one free. The allocator
         * itself supports IRQ-disabled frees and restores the incoming IF. */
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(entry_flags) :: "memory");
        cpu = cpu_current_index();
        if (cpu != slab_guest_remote_free_owner_cpu) {
#ifdef KERNEL_HYBRID_ALLOCATOR
            kfree(slab_guest_remote_free_object);
            __atomic_store_n(&slab_guest_remote_free_passed, 1U,
                             __ATOMIC_RELEASE);
#else
            bool freed = slab_cpu_try_free(slab_guest_remote_free_object);
            __atomic_store_n(&slab_guest_remote_free_passed,
                             freed ? 1U : 0U, __ATOMIC_RELEASE);
#endif
            __asm__ volatile("pushq %0; popfq" :: "r"(entry_flags) : "memory");
            __atomic_store_n(&slab_guest_remote_free_done, 1U,
                             __ATOMIC_RELEASE);
            return;
        }
        __asm__ volatile("pushq %0; popfq" :: "r"(entry_flags) : "memory");
        (void)scheduler_yield();
    }
    __atomic_store_n(&slab_guest_remote_free_done, 1U, __ATOMIC_RELEASE);
}

static void slab_guest_remote_free_release(void *object)
{
#ifdef KERNEL_HYBRID_ALLOCATOR
    kfree(object);
#else
    if (!slab_cpu_try_free(object))
        kfree(object);
#endif
}

static bool slab_guest_force_cross_cpu_free(u32 online_count)
{
    if (online_count < 2U)
        return true;

    u32 owner_before = cpu_current_index();
    void *object;
    u32 owner_after;
    kernel_thread_t *worker;
    u64 spins;
    bool terminated = false;

    if (owner_before >= CPU_MAX_COUNT)
        return false;
    object =
#ifdef KERNEL_HYBRID_ALLOCATOR
        kmalloc(72U);
#else
        slab_cpu_alloc(72U);
#endif
    owner_after = cpu_current_index();
    if (!object || owner_before != owner_after) {
        slab_guest_remote_free_release(object);
        return false;
    }
    slab_guest_remote_free_object = object;
    slab_guest_remote_free_owner_cpu = owner_before;
    __atomic_store_n(&slab_guest_remote_free_done, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&slab_guest_remote_free_passed, 0U, __ATOMIC_RELAXED);
    worker = thread_create_suspended("slab-remote-free",
                                     slab_guest_remote_free_worker, NULL);
    if (!worker || !scheduler_enqueue(worker)) {
        if (worker)
            (void)thread_destroy(worker);
        slab_guest_remote_free_release(object);
        return false;
    }

    for (spins = 0; spins < 100000000ULL; spins++) {
        if (__atomic_load_n(&slab_guest_remote_free_done,
                            __ATOMIC_ACQUIRE))
            break;
        (void)scheduler_yield();
    }
    if (!__atomic_load_n(&slab_guest_remote_free_done, __ATOMIC_ACQUIRE))
        panic("slab guest: remote-free worker timed out");
    for (spins = 0; spins < 100000000ULL; spins++) {
        if (worker->state == THREAD_STATE_TERMINATED &&
            !scheduler_thread_is_running(worker)) {
            terminated = true;
            break;
        }
        (void)scheduler_yield();
    }
    if (!terminated)
        panic("slab guest: remote-free worker did not terminate");
    bool passed = __atomic_load_n(&slab_guest_remote_free_passed,
                                  __ATOMIC_ACQUIRE) != 0U;
    if (passed)
        __atomic_add_fetch(&slab_guest_cross_cpu_frees, 1U,
                           __ATOMIC_RELAXED);
    else
        slab_guest_remote_free_release(object);
    slab_guest_remote_free_object = NULL;
    return thread_destroy(worker) && passed;
}

static bool slab_guest_other_cpu_active(volatile u32 *records, u32 cpu)
{
    for (u32 index = 0; index < CPU_MAX_COUNT; index++)
        if (index != cpu &&
            __atomic_load_n(&records[index], __ATOMIC_ACQUIRE))
            return true;
    return false;
}

static void slab_guest_reclaim_pass(void)
{
    u32 cpu = cpu_current_index();
    slab_reclaim_result_t result;

    if (cpu >= CPU_MAX_COUNT) {
        __atomic_store_n(&slab_guest_worker_failed, 1U, __ATOMIC_RELEASE);
        return;
    }
    __atomic_add_fetch(&slab_guest_reclaimer_active[cpu], 1U,
                       __ATOMIC_ACQ_REL);
    __atomic_fetch_or(&slab_guest_reclaim_cpu_mask, 1U << cpu,
                      __ATOMIC_RELAXED);
    if (slab_guest_other_cpu_active(slab_guest_allocator_active, cpu))
        __atomic_add_fetch(&slab_guest_reclaim_overlap, 1U,
                           __ATOMIC_RELAXED);
    if (!slab_reclaim_bounded(64U, 16U, &result)) {
        __atomic_store_n(&slab_guest_worker_failed, 1U, __ATOMIC_RELEASE);
    } else {
        __atomic_add_fetch(&slab_guest_reclaim_calls, 1U, __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_guest_reclaim_records,
                           result.records_examined, __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_guest_reclaim_objects,
                           result.cached_objects_returned, __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_guest_reclaim_pages,
                           result.slab_pages_released +
                               result.empty_slab_pages_released,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_guest_reclaim_busy,
                           result.cache_lock_busy + result.class_lock_busy,
                           __ATOMIC_RELAXED);
        if (result.cache_record_budget_reached)
            __atomic_add_fetch(&slab_guest_reclaim_record_limit, 1U,
                               __ATOMIC_RELAXED);
        if (result.object_budget_reached)
            __atomic_add_fetch(&slab_guest_reclaim_object_limit, 1U,
                               __ATOMIC_RELAXED);
    }
    __atomic_sub_fetch(&slab_guest_reclaimer_active[cpu], 1U,
                       __ATOMIC_RELEASE);
}

static void slab_guest_worker(void *argument)
{
    u32 worker = (u32)(uintptr_t)argument;
    u32 cpu = cpu_current_index();
    if (worker >= SLAB_GUEST_WORKERS || cpu >= 32U) {
        __atomic_store_n(&slab_guest_worker_failed, 1U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&slab_guest_worker_ready, 1U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&slab_guest_worker_done, 1U, __ATOMIC_RELEASE);
        return;
    }
    __atomic_fetch_or(&slab_guest_worker_cpu_mask, 1U << cpu,
                      __ATOMIC_RELAXED);

    for (u32 index = 0; index < SLAB_GUEST_OBJECTS; index++) {
        bool cached_mode = __atomic_load_n(&slab_guest_cpu_cache_mode,
                                            __ATOMIC_ACQUIRE) != 0U;
        void *object;
#ifdef KERNEL_HYBRID_ALLOCATOR
        object = cached_mode ? kmalloc(72U) : slab_alloc(72U);
#else
        object = cached_mode ? slab_cpu_alloc(72U) : slab_alloc(72U);
#endif
        if (!object) {
            __atomic_store_n(&slab_guest_worker_failed, 1U,
                             __ATOMIC_RELEASE);
            continue;
        }
        slab_guest_worker_objects[worker][index] = object;
        slab_guest_allocation_cpu[worker][index] = cpu_current_index();
        memset(object, (int)(worker + 1U), 72U);
    }

    __atomic_add_fetch(&slab_guest_worker_ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&slab_guest_barrier_armed, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&slab_guest_worker_ready, __ATOMIC_ACQUIRE) <
               __atomic_load_n(&slab_guest_worker_expected,
                               __ATOMIC_ACQUIRE))
        (void)scheduler_yield();

    u32 owner = __atomic_load_n(&slab_guest_cleanup_own,
                                __ATOMIC_ACQUIRE)
        ? worker : (worker + SLAB_GUEST_WORKERS - 1U) %
                       SLAB_GUEST_WORKERS;
    for (u32 index = 0; index < SLAB_GUEST_OBJECTS; index++) {
        void *object = slab_guest_worker_objects[owner][index];
        if (!object)
            continue;
        const u8 *bytes = object;
        for (u32 byte = 0; byte < 72U; byte++) {
            if (bytes[byte] != (u8)(owner + 1U)) {
                __atomic_store_n(&slab_guest_worker_failed, 1U,
                                 __ATOMIC_RELEASE);
                break;
            }
        }
        if (slab_guest_allocation_cpu[owner][index] != cpu_current_index())
            __atomic_add_fetch(&slab_guest_cross_cpu_frees, 1U,
                               __ATOMIC_RELAXED);
        bool cached_mode = __atomic_load_n(&slab_guest_cpu_cache_mode,
                                            __ATOMIC_ACQUIRE) != 0U;
        u32 free_cpu = cpu_current_index();
        if (cached_mode && free_cpu < CPU_MAX_COUNT)
            __atomic_add_fetch(&slab_guest_allocator_active[free_cpu], 1U,
                               __ATOMIC_ACQ_REL);
        bool freed;
#ifdef KERNEL_HYBRID_ALLOCATOR
        if (cached_mode) {
            kfree(object);
            freed = true;
        } else
#endif
            freed = cached_mode ? slab_cpu_try_free(object) :
                                  slab_try_free(object);
        if (cached_mode && free_cpu < CPU_MAX_COUNT) {
            if (slab_guest_other_cpu_active(slab_guest_reclaimer_active,
                                            free_cpu))
                __atomic_add_fetch(&slab_guest_reclaim_overlap, 1U,
                                   __ATOMIC_RELAXED);
            __atomic_sub_fetch(&slab_guest_allocator_active[free_cpu], 1U,
                               __ATOMIC_RELEASE);
        }
        if (!freed)
            __atomic_store_n(&slab_guest_worker_failed, 1U,
                             __ATOMIC_RELEASE);
        if (cached_mode && (index % SLAB_GUEST_RECLAIM_EVERY) == 0U)
            slab_guest_reclaim_pass();
        slab_guest_worker_objects[owner][index] = NULL;
    }
    __atomic_add_fetch(&slab_guest_worker_done, 1U, __ATOMIC_RELEASE);
}

static bool slab_guest_test_classes(void)
{
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        u32 class_size = slab_class_size(class_index);
        u32 slots = slab_class_slots(class_index);
        if (!class_size || !slots || slots + 1U > SLAB_GUEST_CLASS_OBJECTS)
            return false;
        for (u32 index = 0; index <= slots; index++) {
            void *object = slab_alloc(class_size);
            if (!object || ((uintptr_t)object & 15U))
                return false;
            slab_guest_class_objects[index] = object;
            memset(object, (int)(class_index + index), class_size);
        }
        for (u32 index = 0; index <= slots; index++) {
            const u8 *bytes = slab_guest_class_objects[index];
            for (u32 byte = 0; byte < class_size; byte++) {
                if (bytes[byte] != (u8)(class_index + index))
                    return false;
            }
        }
        for (u32 index = 0; index <= slots; index++) {
            if (!slab_try_free(slab_guest_class_objects[index]))
                return false;
            slab_guest_class_objects[index] = NULL;
        }

        /* Exercise the CPU-local API for every class while preserving a
         * caller payload across a cached free and same-cache reuse. */
        void *cached = slab_cpu_alloc(class_size);
        if (!cached || ((uintptr_t)cached & 15U))
            return false;
        memset(cached, (int)(0x80U + class_index), class_size);
        if (!slab_cpu_try_free(cached))
            return false;
        void *reused = slab_cpu_alloc(class_size);
        if (reused != cached)
            return false;
        const u8 *payload = reused;
        for (u32 byte = 0; byte < class_size; byte++) {
            if (payload[byte] != (u8)(0x80U + class_index))
                return false;
        }
        if (!slab_cpu_try_free(reused))
            return false;
        (void)slab_cpu_drain_all();
    }
    return true;
}

#ifdef KERNEL_HYBRID_ALLOCATOR
#ifndef KERNEL_HYBRID_ALLOCATOR_CUTOFF
#define KERNEL_HYBRID_ALLOCATOR_CUTOFF SLAB_MAX_OBJECT_SIZE
#endif
static bool slab_guest_test_public_routing(void)
{
    void *objects[SLAB_CLASS_COUNT] = {0};
    slab_class_stats_t before[SLAB_CLASS_COUNT];

    for (u32 class_index = 0U; class_index < SLAB_CLASS_COUNT;
         class_index++) {
        usize size = slab_class_size(class_index);
        if (!size || !slab_get_class_stats(class_index,
                                           &before[class_index]))
            return false;
        objects[class_index] = kmalloc(size);
        if (!objects[class_index] ||
            ((uintptr_t)objects[class_index] & 15U))
            return false;
        memset(objects[class_index], (int)(0x30U + class_index), size);
    }

    for (u32 class_index = 0U; class_index < SLAB_CLASS_COUNT;
         class_index++) {
        slab_class_stats_t allocated;
        bool slab_owned = slab_class_size(class_index) <=
                          KERNEL_HYBRID_ALLOCATOR_CUTOFF;
        if (!slab_get_class_stats(class_index, &allocated) ||
            allocated.allocations != before[class_index].allocations +
                                     (slab_owned ? 1U : 0U) ||
            allocated.live_slots != before[class_index].live_slots +
                                    (slab_owned ? 1U : 0U))
            return false;
    }

    for (u32 class_index = 0U; class_index < SLAB_CLASS_COUNT;
         class_index++) {
        const u8 *bytes = objects[class_index];
        usize size = slab_class_size(class_index);
        for (usize offset = 0U; offset < size; offset++)
            if (bytes[offset] != (u8)(0x30U + class_index))
                return false;
        kfree(objects[class_index]);
        objects[class_index] = NULL;
    }

    for (u32 class_index = 0U; class_index < SLAB_CLASS_COUNT;
         class_index++) {
        slab_class_stats_t freed;
        bool slab_owned = slab_class_size(class_index) <=
                          KERNEL_HYBRID_ALLOCATOR_CUTOFF;
        if (!slab_get_class_stats(class_index, &freed) ||
            freed.frees != before[class_index].frees +
                           (slab_owned ? 1U : 0U) ||
            freed.live_slots != before[class_index].live_slots)
            return false;
    }

    void *large = kmalloc(SLAB_MAX_OBJECT_SIZE + 1U);
    if (!large || slab_cpu_try_free(large))
        return false;
    kfree(large);
    if (kmalloc(0U) || kmalloc((usize)-1))
        return false;
    kfree(NULL);
    (void)slab_cpu_drain_all();
    if (!slab_verify())
        return false;
    kprint("SLAB_GUEST_PUBLIC_API pass=1 classes=%u cutoff=%u large_heap=1 zero=1 overflow=1\n",
           SLAB_CLASS_COUNT, KERNEL_HYBRID_ALLOCATOR_CUTOFF);
    return true;
}
#endif

static bool slab_guest_heap_reclaim_test(u32 online)
{
    const u64 group_bytes = (u64)HEAP_INITIAL_PAGES * PAGE_SIZE;
    u64 mapped_before = heap_get_mapped_size();
    usize request = (usize)(heap_get_free_size() + PAGE_SIZE);
    u64 free_before;
    u64 mapped_grown;
    u64 free_after;
    u64 acknowledgements[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    u64 other_cr3[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    heap_reclaim_result_t reclaimed = {0};
    pmm_owned_frame_t reused = {0};
    void *allocation = kmalloc(request);

    if (!allocation)
        return false;
    mapped_grown = heap_get_mapped_size();
    if (mapped_grown < mapped_before + group_bytes) {
        kfree(allocation);
        return false;
    }
    kfree(allocation);
    free_before = pmm_get_free_memory();
    if (!heap_reclaim_bounded(1U, &reclaimed) ||
        reclaimed.groups_released != 1U ||
        reclaimed.frames_released != HEAP_INITIAL_PAGES ||
        reclaimed.bytes_released != group_bytes ||
        !reclaimed.first_frame_released ||
        heap_get_mapped_size() != mapped_grown - group_bytes)
        return false;
    free_after = pmm_get_free_memory();
    if (free_after < free_before + group_bytes)
        return false;

    /* PMM acquisition must return the same generation-owned frame that the
     * completed tail teardown just released. */
    if (!pmm_owned_frame_acquire(&reused))
        return false;
    bool same_frame = reused.physical == reclaimed.first_frame_released;
    if (!pmm_owned_frame_release(&reused) || !same_frame)
        return false;

    vmm_kernel_range_guest_test_copy_shootdown_masks(
        acknowledgements, other_cr3, (CPU_MAX_COUNT + 63U) / 64U);
    (void)other_cr3;
    u32 current = cpu_current_index();
    if (current >= CPU_MAX_COUNT ||
        !(acknowledgements[current / 64U] & (1ULL << (current % 64U))))
        return false;
    if (online >= 2U &&
        (acknowledgements[0] & 0x3U) != 0x3U)
        return false;

    /* Recommit through the production heap growth path and restore the
     * original tail size before subsequent guest workloads begin. */
    allocation = kmalloc(request);
    if (!allocation || heap_get_mapped_size() != mapped_grown)
        return false;
    kfree(allocation);
    heap_reclaim_result_t cleanup = {0};
    if (!heap_reclaim_bounded(1U, &cleanup) ||
        cleanup.groups_released != 1U ||
        heap_get_mapped_size() != mapped_grown - group_bytes)
        return false;

    kprint("HEAP_RECLAIM_GUEST pass=1 groups=%u frames=%u bytes=%llu frame=%llx free_delta=%llu recommit=1 online=%u ack=%llx\n",
           reclaimed.groups_released, reclaimed.frames_released,
           (unsigned long long)reclaimed.bytes_released,
           (unsigned long long)reclaimed.first_frame_released,
           (unsigned long long)(free_after - free_before), online,
           (unsigned long long)acknowledgements[0]);
    return true;
}

static bool slab_guest_test_concurrency(u32 online_count, bool cpu_cache)
{
    kernel_thread_t *threads[SLAB_GUEST_WORKERS] = {0};
    u32 created = 0;
    u32 queued = 0;

    slab_guest_worker_ready = 0;
    slab_guest_worker_done = 0;
    slab_guest_worker_cpu_mask = 0;
    slab_guest_cross_cpu_frees = 0;
    slab_guest_worker_expected = 0;
    slab_guest_barrier_armed = 0;
    slab_guest_worker_failed = 0;
    slab_guest_cleanup_own = 0;
    slab_guest_reclaim_cpu_mask = 0U;
    slab_guest_reclaim_calls = 0U;
    slab_guest_reclaim_overlap = 0U;
    slab_guest_reclaim_busy = 0U;
    slab_guest_reclaim_record_limit = 0U;
    slab_guest_reclaim_object_limit = 0U;
    slab_guest_reclaim_objects = 0U;
    slab_guest_reclaim_pages = 0U;
    slab_guest_reclaim_records = 0U;
    for (u32 cpu = 0; cpu < CPU_MAX_COUNT; cpu++) {
        __atomic_store_n(&slab_guest_allocator_active[cpu], 0U,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&slab_guest_reclaimer_active[cpu], 0U,
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&slab_guest_cpu_cache_mode, cpu_cache ? 1U : 0U,
                     __ATOMIC_RELEASE);
    memset(slab_guest_worker_objects, 0, sizeof(slab_guest_worker_objects));
    memset(slab_guest_allocation_cpu, 0, sizeof(slab_guest_allocation_cpu));

    for (u32 index = 0; index < SLAB_GUEST_WORKERS; index++) {
        threads[index] = thread_create_suspended(
            "slab-guest", slab_guest_worker, (void *)(uintptr_t)index);
        if (!threads[index])
            break;
        created++;
    }
    if (created != SLAB_GUEST_WORKERS) {
        for (u32 index = 0; index < created; index++)
            (void)thread_destroy(threads[index]);
        return false;
    }
    for (; queued < created; queued++) {
        if (!scheduler_enqueue(threads[queued]))
            break;
    }
    if (queued != created) {
        __atomic_store_n(&slab_guest_cleanup_own, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&slab_guest_worker_expected, queued,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&slab_guest_barrier_armed, 1U, __ATOMIC_RELEASE);
        if (!queued) {
            for (u32 index = 0; index < created; index++)
                (void)thread_destroy(threads[index]);
            return false;
        }
    }
    if (queued == created) {
        __atomic_store_n(&slab_guest_worker_expected, created,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&slab_guest_barrier_armed, 1U, __ATOMIC_RELEASE);
    }

    u64 spins = 0;
    while (__atomic_load_n(&slab_guest_worker_done, __ATOMIC_ACQUIRE) < queued &&
           spins < 100000000ULL) {
        spins++;
        (void)scheduler_yield();
    }
    if (__atomic_load_n(&slab_guest_worker_done, __ATOMIC_ACQUIRE) != queued)
        panic("slab guest: worker completion timed out");
    bool terminated = false;
    for (spins = 0; spins < 100000000ULL; spins++) {
        bool all_terminated = true;
        for (u32 index = 0; index < created; index++) {
            if (index >= queued)
                continue;
            if (threads[index]->state != THREAD_STATE_TERMINATED ||
                scheduler_thread_is_running(threads[index])) {
                all_terminated = false;
                break;
            }
        }
        if (all_terminated) {
            terminated = true;
            break;
        }
        (void)scheduler_yield();
    }
    bool passed = __atomic_load_n(&slab_guest_worker_done,
                                  __ATOMIC_ACQUIRE) == queued &&
        queued == created &&
        !__atomic_load_n(&slab_guest_worker_failed, __ATOMIC_ACQUIRE);
    u32 cpu_mask = __atomic_load_n(&slab_guest_worker_cpu_mask,
                                   __ATOMIC_ACQUIRE);
    u32 cross_frees = __atomic_load_n(&slab_guest_cross_cpu_frees,
                                      __ATOMIC_ACQUIRE);
    if (online_count >= 2U && queued == created) {
        passed = passed && (cpu_mask & 3U) == 3U;
        /* Worker ownership and migration do not guarantee remote frees in
         * this phase. A separate probe below establishes cross-CPU freeing. */
    }
    kprint("SLAB_GUEST_CONCURRENCY pass=%u cpu_cache=%u queued=%u created=%u cpumask=%x cross=%u worker_failed=%u terminated=%u\n",
           passed ? 1U : 0U, cpu_cache ? 1U : 0U, queued, created,
           cpu_mask, cross_frees,
           __atomic_load_n(&slab_guest_worker_failed, __ATOMIC_ACQUIRE),
           terminated ? 1U : 0U);
    if (!terminated)
        panic("slab guest: completed worker did not terminate");
    for (u32 index = 0; index < queued; index++) {
        if (threads[index]->state != THREAD_STATE_TERMINATED ||
            scheduler_thread_is_running(threads[index]))
            panic("slab guest: completed worker did not terminate");
        passed = thread_destroy(threads[index]) && passed;
    }
    for (u32 index = queued; index < created; index++)
        passed = thread_destroy(threads[index]) && passed;
    return passed;
}

static bool slab_guest_all_retired(u64 *retired_slabs)
{
    u64 retired = 0;
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_stats_t stats;
        if (!slab_get_class_stats(class_index, &stats) || stats.live_slots ||
            stats.cached_slots ||
            stats.partial_slabs || stats.full_slabs ||
            stats.backing_allocations != stats.backing_releases)
            return false;
        retired += stats.retired_slabs;
    }
    if (retired_slabs)
        *retired_slabs = retired;
    return retired != 0U;
}

static bool slab_guest_irq_disabled_reclaim(u64 *objects_returned)
{
    void *object = slab_cpu_alloc(72U);
    slab_reclaim_result_t result = {0};
    u64 entry_flags;
    u64 result_flags;

    if (!object) {
        slab_class_stats_t stats = {0};
        bool stats_valid = slab_get_class_stats(4U, &stats);
        kprint("SLAB_GUEST_IRQ_RECLAIM_DETAIL allocated=0 freed=0 ran=0 objects=0 cpu=%u count=%u online=%u initialized=%u stats=%u alloc_fail=%llu backing=%llu live=%llu cached=%llu heap_mapped=%llu heap_free=%llu pmm_free=%llu\n",
               cpu_current_index(), cpu_count(), cpu_online_count(),
               slab_is_initialized() ? 1U : 0U, stats_valid ? 1U : 0U,
               (unsigned long long)stats.allocation_failures,
               (unsigned long long)stats.backing_allocations,
               (unsigned long long)stats.live_slots,
               (unsigned long long)stats.cached_slots,
               (unsigned long long)heap_get_mapped_size(),
               (unsigned long long)heap_get_free_size(),
               (unsigned long long)pmm_get_free_memory());
        return false;
    }
    bool freed = slab_cpu_try_free(object);
    if (!freed) {
        kprint("SLAB_GUEST_IRQ_RECLAIM_DETAIL allocated=1 freed=0 ran=0 objects=0\n");
        return false;
    }
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(entry_flags) :: "memory");
    bool ran = slab_reclaim_bounded(64U, 16U, &result);
    __asm__ volatile("pushfq; popq %0" : "=r"(result_flags) :: "memory");
    __asm__ volatile("pushq %0; popfq" :: "r"(entry_flags) : "memory");
    if (objects_returned)
        *objects_returned = result.cached_objects_returned;
    kprint("SLAB_GUEST_IRQ_RECLAIM_DETAIL allocated=1 freed=%u ran=%u entry_if=%u result_if=%u objects=%llu records=%u busy=%u\n",
           freed ? 1U : 0U, ran ? 1U : 0U,
           (u32)((entry_flags >> 9) & 1U),
           (u32)((result_flags >> 9) & 1U),
           (unsigned long long)result.cached_objects_returned,
           result.records_examined,
           result.cache_lock_busy + result.class_lock_busy);
    return ran && (entry_flags & (1ULL << 9)) &&
           !(result_flags & (1ULL << 9)) &&
           result.cached_objects_returned != 0U;
}

bool slab_guest_test_run(void)
{
    slab_classifier_stats_t before, after;
    u64 used_before;
    u32 online = cpu_online_count();
    bool classes_ok;
    bool refill_ok = false;
    bool ordinary_heap_ok = false;
    bool concurrency_ok;
    bool cpu_cache_ok;
    u64 irq_reclaim_objects = 0U;
    bool irq_reclaim_ok;
    u64 retired_slabs = 0;

    kprint("SLAB_GUEST_BEGIN online=%u deep_validation=%u\n", online,
           (u32)SLAB_DEEP_VALIDATION);
    if (!slab_is_initialized() &&
        !slab_init((void *)KERNEL_VMM_RANGE_BASE,
                   (usize)(KERNEL_VMM_RANGE_LIMIT -
                           KERNEL_VMM_RANGE_BASE))) {
        kprint("SLAB_GUEST_FAIL init\n");
        return false;
    }
    used_before = heap_get_used_size();
    irq_reclaim_ok = slab_guest_irq_disabled_reclaim(&irq_reclaim_objects);
    kprint("SLAB_GUEST_IRQ_RECLAIM pass=%u entry_if=1 result_if=0 objects=%llu\n",
           irq_reclaim_ok ? 1U : 0U, irq_reclaim_objects);
    if (!irq_reclaim_ok) {
        kprint("SLAB_GUEST_FAIL irq-disabled-reclaim\n");
        return false;
    }
#ifdef SLAB_CORE_GUEST_BENCHMARK
    if (!slab_guest_benchmark_run()) {
        kprint("SLAB_GUEST_FAIL native-benchmark\n");
        return false;
    }
#endif
    classes_ok = slab_guest_test_classes();
    if (!classes_ok || !slab_verify() ||
        !slab_get_classifier_stats(&before)) {
        kprint("SLAB_GUEST_FAIL classes\n");
        return false;
    }
    /* The later VMM guest phase exercises PMM-backed slab retirement while a
     * remote CPU holds another CR3. Here all classes have completed direct
     * slab allocation, CPU-cache reuse, and a full quiescent drain. */
    refill_ok = true;
#ifdef KERNEL_HYBRID_ALLOCATOR
    if (!slab_guest_test_public_routing()) {
        kprint("SLAB_GUEST_FAIL public-api-routing\n");
        return false;
    }
#endif
    if (!slab_guest_heap_reclaim_test(online)) {
        kprint("SLAB_GUEST_FAIL heap-physical-reclaim\n");
        return false;
    }

    concurrency_ok = slab_guest_test_concurrency(online, false);
#ifdef KERNEL_HYBRID_ALLOCATOR
    slab_class_stats_t public_smp_before, public_smp_after;
    if (!slab_get_class_stats(4U, &public_smp_before)) {
        kprint("SLAB_GUEST_FAIL public-smp-before\n");
        return false;
    }
#endif
    cpu_cache_ok = slab_guest_test_concurrency(online, true);
    if (cpu_cache_ok)
        cpu_cache_ok = slab_guest_force_cross_cpu_free(online);
#ifdef KERNEL_HYBRID_ALLOCATOR
    u32 public_smp_operations = SLAB_GUEST_WORKERS * SLAB_GUEST_OBJECTS +
                                (online >= 2U ? 1U : 0U);
    if (!slab_get_class_stats(4U, &public_smp_after) ||
        public_smp_after.allocations - public_smp_before.allocations !=
            public_smp_operations ||
        public_smp_after.frees - public_smp_before.frees !=
            public_smp_operations) {
        kprint("SLAB_GUEST_FAIL public-smp-accounting\n");
        return false;
    }
    kprint("SLAB_GUEST_PUBLIC_SMP pass=1 allocations=%u frees=%u cpumask=%x cross=%u overlap=%u\n",
           public_smp_operations, public_smp_operations,
           __atomic_load_n(&slab_guest_worker_cpu_mask, __ATOMIC_ACQUIRE),
           __atomic_load_n(&slab_guest_cross_cpu_frees, __ATOMIC_ACQUIRE),
           __atomic_load_n(&slab_guest_reclaim_overlap, __ATOMIC_ACQUIRE));
#endif
    u32 reclaim_cpu_mask = __atomic_load_n(&slab_guest_reclaim_cpu_mask,
                                            __ATOMIC_ACQUIRE);
    u32 reclaim_overlap = __atomic_load_n(&slab_guest_reclaim_overlap,
                                         __ATOMIC_ACQUIRE);
    u32 reclaim_calls = __atomic_load_n(&slab_guest_reclaim_calls,
                                        __ATOMIC_ACQUIRE);
    u64 reclaim_objects = __atomic_load_n(&slab_guest_reclaim_objects,
                                         __ATOMIC_ACQUIRE);
    kprint("SLAB_GUEST_RECLAIM calls=%u records=%llu objects=%llu pages=%llu busy=%u overlap=%u cpumask=%x record_limit=%u object_limit=%u\n",
           reclaim_calls,
           __atomic_load_n(&slab_guest_reclaim_records, __ATOMIC_ACQUIRE),
           reclaim_objects,
           __atomic_load_n(&slab_guest_reclaim_pages, __ATOMIC_ACQUIRE),
           __atomic_load_n(&slab_guest_reclaim_busy, __ATOMIC_ACQUIRE),
           reclaim_overlap, reclaim_cpu_mask,
           __atomic_load_n(&slab_guest_reclaim_record_limit,
                           __ATOMIC_ACQUIRE),
           __atomic_load_n(&slab_guest_reclaim_object_limit,
                           __ATOMIC_ACQUIRE));
    (void)slab_cpu_drain_all();
    void *ordinary_probe = kmalloc(32768U);
    ordinary_heap_ok = ordinary_probe && !slab_try_free(ordinary_probe);
    if (ordinary_probe)
        kfree(ordinary_probe);
    if (!irq_reclaim_ok || !refill_ok || !ordinary_heap_ok ||
        !concurrency_ok || !cpu_cache_ok ||
        !reclaim_calls || !reclaim_objects ||
        (online >= 2U && (!(reclaim_cpu_mask & 1U) ||
                          !(reclaim_cpu_mask & 2U) || !reclaim_overlap)) ||
        !slab_verify() ||
        !slab_guest_all_retired(&retired_slabs) ||
        !slab_get_classifier_stats(&after)) {
        kprint("SLAB_GUEST_FAIL refill=%u ordinary=%u concurrency=%u cpu-cache=%u\n",
               refill_ok ? 1U : 0U, ordinary_heap_ok ? 1U : 0U,
               concurrency_ok ? 1U : 0U, cpu_cache_ok ? 1U : 0U);
        return false;
    }
    u64 used_after = heap_get_used_size();
    if (used_after < used_before ||
        used_after - used_before < after.retained_payload_bytes) {
        kprint("SLAB_GUEST_FAIL ownership used-before=%llu used-after=%llu metadata=%llu\n",
               used_before, used_after, after.retained_payload_bytes);
        return false;
    }
    kprint("SLAB_GUEST_PASS classes=%u refill=%u retire=%llu ordinary=1 cpu_cache=1 leaves=%llu online=%u cpumask=%x cross=%u classifier-payload=%llu heap-used-delta=%llu\n",
           SLAB_CLASS_COUNT, refill_ok ? 1U : 0U,
           retired_slabs,
           after.leaf_tables, online,
           __atomic_load_n(&slab_guest_worker_cpu_mask, __ATOMIC_ACQUIRE),
           __atomic_load_n(&slab_guest_cross_cpu_frees, __ATOMIC_ACQUIRE),
           after.retained_payload_bytes,
           used_after - used_before);
    return true;
}
#endif
