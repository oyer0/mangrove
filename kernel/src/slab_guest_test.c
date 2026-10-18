/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef SLAB_CORE_GUEST_TEST
#include <cpu.h>
#include <heap.h>
#include <kprint.h>
#include <panic.h>
#include <pmm.h>
#include <scheduler.h>
#include <slab.h>
#include <string.h>

#define SLAB_GUEST_WORKERS 4U
#define SLAB_GUEST_OBJECTS 48U
#define SLAB_GUEST_CLASS_OBJECTS 251U
#define SLAB_GUEST_CLASSIFIER_FILLERS 640U

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
        void *object = slab_alloc(72U);
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
        if (!slab_try_free(object))
            __atomic_store_n(&slab_guest_worker_failed, 1U,
                             __ATOMIC_RELEASE);
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
    }
    return true;
}

static bool slab_guest_test_concurrency(u32 online_count)
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
    if (online_count >= 2U && queued == created)
        passed = passed && (cpu_mask & 3U) == 3U && cross_frees != 0U;
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
            stats.partial_slabs || stats.full_slabs ||
            stats.backing_allocations != stats.backing_releases)
            return false;
        retired += stats.retired_slabs;
    }
    if (retired_slabs)
        *retired_slabs = retired;
    return retired != 0U;
}

static u64 slab_guest_mapped_heap_bytes(void)
{
    uintptr_t address = HEAP_START;
    while (address < HEAP_LIMIT &&
           heap_address_is_mapped((const void *)address))
        address += PAGE_SIZE;
    return (u64)(address - HEAP_START);
}

bool slab_guest_test_run(void)
{
    slab_classifier_stats_t before, after;
    u64 used_before;
    u64 mapped_before = slab_guest_mapped_heap_bytes();
    u32 online = cpu_online_count();
    bool classes_ok;
    bool refill_ok = false;
    bool ordinary_heap_ok = false;
    bool concurrency_ok;
    u64 retired_slabs = 0;

    kprint("SLAB_GUEST_BEGIN online=%u\n", online);
    if (!slab_init((void *)HEAP_START,
                   (usize)(HEAP_LIMIT - HEAP_START))) {
        kprint("SLAB_GUEST_FAIL init\n");
        return false;
    }
    used_before = heap_get_used_size();
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

    /* Consume successive heap blocks while forcing each slab to retire. A
     * large allocation alone can grow the heap tail while leaving a small
     * low-address free block available, so inspect the actual slab page and
     * advance until it crosses the 2 MiB classifier-leaf boundary. */
    void *fillers[SLAB_GUEST_CLASSIFIER_FILLERS] = {0};
    usize filler_count = 0;
    for (; filler_count < SLAB_GUEST_CLASSIFIER_FILLERS; filler_count++) {
        void *later = slab_alloc(16U);
        if (!later)
            break;
        usize page_index = ((uintptr_t)later - (uintptr_t)HEAP_START) /
                          PAGE_SIZE;
        if (page_index >= 512U && slab_get_classifier_stats(&after) &&
            after.leaf_tables > before.leaf_tables) {
            refill_ok = slab_try_free(later);
            break;
        }
        if (!slab_try_free(later))
            break;
        fillers[filler_count] = kmalloc(PAGE_SIZE);
        if (!fillers[filler_count])
            break;
        ordinary_heap_ok = !slab_try_free(fillers[filler_count]);
    }
    for (usize index = 0; index < filler_count; index++)
        kfree(fillers[index]);
    concurrency_ok = slab_guest_test_concurrency(online);
    if (!refill_ok || !ordinary_heap_ok || !concurrency_ok || !slab_verify() ||
        !slab_guest_all_retired(&retired_slabs) ||
        !slab_get_classifier_stats(&after)) {
        kprint("SLAB_GUEST_FAIL refill=%u ordinary=%u concurrency=%u\n",
               refill_ok ? 1U : 0U, ordinary_heap_ok ? 1U : 0U,
               concurrency_ok ? 1U : 0U);
        return false;
    }
    u64 used_after = heap_get_used_size();
    u64 mapped_after = slab_guest_mapped_heap_bytes();
    if (used_after < used_before ||
        used_after - used_before < after.retained_payload_bytes ||
        mapped_after < mapped_before ||
        ((mapped_after - mapped_before) & (PAGE_SIZE - 1U))) {
        kprint("SLAB_GUEST_FAIL ownership used-before=%llu used-after=%llu metadata=%llu\n",
               used_before, used_after, after.retained_payload_bytes);
        return false;
    }
    void *ordinary_probe = kmalloc(32768U);
    if (!ordinary_probe || slab_try_free(ordinary_probe)) {
        kprint("SLAB_GUEST_FAIL ordinary-reuse\n");
        return false;
    }
    kfree(ordinary_probe);
    kprint("SLAB_GUEST_PASS classes=%u refill=%u retire=%llu ordinary=1 leaves=%llu online=%u cpumask=%x cross=%u classifier-payload=%llu heap-used-delta=%llu mapped-heap-delta=%llu\n",
           SLAB_CLASS_COUNT, refill_ok ? 1U : 0U,
           retired_slabs,
           after.leaf_tables, online,
           __atomic_load_n(&slab_guest_worker_cpu_mask, __ATOMIC_ACQUIRE),
           __atomic_load_n(&slab_guest_cross_cpu_frees, __ATOMIC_ACQUIRE),
           after.retained_payload_bytes,
           used_after - used_before, mapped_after - mapped_before);
    return true;
}
#endif
