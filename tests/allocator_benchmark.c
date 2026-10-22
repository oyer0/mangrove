/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <types.h>
#include <heap.h>
#define SLAB_CORE_DIAGNOSTICS 1
#include <slab.h>
#include <spinlock.h>
#include <mutex.h>
#include <pmm.h>
#include <vmm.h>
#include "heap_profile_diag.h"

static uintptr_t benchmark_heap_base;
/* Leave virtual heap growth headroom beyond the bounded PMM frame pool so the
 * pressure control can prove a reclaimed slab frame funds a later heap page. */
static usize benchmark_heap_capacity = 16U * 1024U * 1024U;
static uintptr_t benchmark_slab_base;
static usize benchmark_slab_capacity = 8U * 1024U * 1024U;
#define BENCHMARK_SLAB_PAGE_COUNT (8U * 1024U * 1024U / PAGE_SIZE)
#define BENCHMARK_PHYSICAL_FRAME_COUNT BENCHMARK_SLAB_PAGE_COUNT
/* Benchmark-only compact arena for the historical one-byte lookup. The
 * production dense classifier was 128 MiB for the full kernel reservation. */
static u8 dense_classifier_reference[8U * 1024U * 1024U / PAGE_SIZE];

#undef HEAP_START
#undef HEAP_LIMIT
#define HEAP_START benchmark_heap_base
#define HEAP_LIMIT (benchmark_heap_base + benchmark_heap_capacity)
#define HEAP_VERIFY 0
#ifndef KERNEL_HYBRID_ALLOCATOR_CUTOFF
#define KERNEL_HYBRID_ALLOCATOR_CUTOFF SLAB_MAX_OBJECT_SIZE
#endif

/* Compile the production heap and slab sources against isolated host arenas. */
#include "../kernel/src/heap.c"
#define SLAB_HOST_TEST 1
#include "../kernel/src/slab.c"

#define BENCH_CLASSIFIER_ROOT_ENTRIES \
    (((usize)(KERNEL_HEAP_LIMIT - KERNEL_HEAP_BASE) / PAGE_SIZE + \
      SLAB_CLASSIFIER_ROOT_PAGES - 1U) / SLAB_CLASSIFIER_ROOT_PAGES)

/* Host-only experiment for replacing the allocated root node with a compact
 * static directory.  This is not linked into Pith. */
static _Atomic uintptr_t benchmark_classifier_root[
    BENCH_CLASSIFIER_ROOT_ENTRIES];

static page_table_t benchmark_pml4;
static pthread_mutex_t benchmark_owned_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool benchmark_physical_frame_used[BENCHMARK_PHYSICAL_FRAME_COUNT];
static bool benchmark_owned_frame_used[PMM_OWNED_FRAME_CAPACITY];
static bool benchmark_owned_frame_mapped[PMM_OWNED_FRAME_CAPACITY];
static u64 benchmark_owned_frame_generation[PMM_OWNED_FRAME_CAPACITY];
static phys_addr_t benchmark_owned_frame_physical[
    PMM_OWNED_FRAME_CAPACITY];
static bool benchmark_slab_range_reserved[BENCHMARK_SLAB_PAGE_COUNT];
static bool benchmark_slab_range_mapped[BENCHMARK_SLAB_PAGE_COUNT];
static u32 benchmark_slab_range_frame[BENCHMARK_SLAB_PAGE_COUNT];
static u64 benchmark_slab_range_generation[BENCHMARK_SLAB_PAGE_COUNT];
static u64 benchmark_next_frame_generation = 1U;
static u64 benchmark_next_range_generation = 1U;
static _Atomic bool benchmark_pmm_exhausted;
static _Thread_local bool benchmark_interrupts_enabled = true;
static _Thread_local u32 benchmark_spin_depth;
static _Thread_local u32 benchmark_cpu_index;
static int benchmark_worker_cpus[4] = {-1, -1, -1, -1};
static pthread_mutex_t benchmark_growth_mutex = PTHREAD_MUTEX_INITIALIZER;

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}

u64 spin_lock_irqsave(spinlock_t *lock)
{
    bool enabled = benchmark_interrupts_enabled;
    benchmark_interrupts_enabled = false;
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE))
        sched_yield();
    benchmark_spin_depth++;
    return enabled ? (1ULL << 9) : 0U;
}

bool spin_try_lock_irqsave(spinlock_t *lock, u64 *flags_out)
{
    bool enabled = benchmark_interrupts_enabled;
    u32 expected = 0U;
    benchmark_interrupts_enabled = false;
    if (!__atomic_compare_exchange_n(&lock->value, &expected, 1U, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        benchmark_interrupts_enabled = enabled;
        return false;
    }
    benchmark_spin_depth++;
    *flags_out = enabled ? (1ULL << 9) : 0U;
    return true;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    assert(benchmark_spin_depth);
    benchmark_spin_depth--;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
    benchmark_interrupts_enabled = (flags & (1ULL << 9)) != 0;
}

u32 spinlock_depth_current(void)
{
    return benchmark_spin_depth;
}

u32 cpu_current_index(void)
{
    return benchmark_cpu_index;
}

u32 cpu_count(void)
{
    return 4U;
}

void mutex_init(mutex_t *mutex) { (void)mutex; }
bool mutex_lock(mutex_t *mutex)
{
    assert(mutex == &heap_growth_lock);
    return pthread_mutex_lock(&benchmark_growth_mutex) == 0;
}
bool mutex_unlock(mutex_t *mutex)
{
    assert(mutex == &heap_growth_lock);
    return pthread_mutex_unlock(&benchmark_growth_mutex) == 0;
}
page_table_t *vmm_get_kernel_pml4(void) { return &benchmark_pml4; }
bool vmm_map(page_table_t *pml4, void *address, phys_addr_t frame, u64 flags)
{
    assert(pml4 == &benchmark_pml4 && address && frame &&
           flags == (PTE_PRESENT | PTE_READWRITE | PTE_NX));
    return true;
}
bool vmm_map_kernel_bootstrap(page_table_t *pml4, void *address,
                              phys_addr_t frame, u64 flags)
{
    return vmm_map(pml4, address, frame, flags);
}
/* This public-routing adapter intentionally declines leased heap backing and
 * exercises the documented clean raw-frame fallback. The production-owned
 * heap map/unmap path is covered by tests/heap_test.py and the native guest. */
bool vmm_kernel_heap_range_reserve(virt_addr_t base, u64 page_count,
                                   vmm_kernel_range_t *out_range)
{
    (void)base;
    (void)page_count;
    (void)out_range;
    return false;
}
phys_addr_t pmm_alloc_frame(void)
{
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    if (atomic_load_explicit(&benchmark_pmm_exhausted,
                             memory_order_acquire)) {
        assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
        return 0U;
    }
    for (u32 slot = 0; slot < BENCHMARK_PHYSICAL_FRAME_COUNT; slot++) {
        if (!benchmark_physical_frame_used[slot]) {
            benchmark_physical_frame_used[slot] = true;
            assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
            return ((phys_addr_t)slot + 1U) * PAGE_SIZE;
        }
    }
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
    return 0U;
}
void pmm_free_frame(phys_addr_t frame)
{
    if (!frame || (frame & (PAGE_SIZE - 1U)))
        abort();
    u64 index = frame / PAGE_SIZE - 1U;
    if (index >= BENCHMARK_PHYSICAL_FRAME_COUNT)
        abort();
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    if (!benchmark_physical_frame_used[index])
        abort();
    benchmark_physical_frame_used[index] = false;
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
}

bool pmm_owned_frame_acquire(pmm_owned_frame_t *out_frame)
{
    if (!out_frame || out_frame->physical || out_frame->generation ||
        out_frame->slot)
        return false;
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    if (atomic_load_explicit(&benchmark_pmm_exhausted,
                             memory_order_acquire)) {
        assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
        return false;
    }
    for (u32 owner = 0; owner < PMM_OWNED_FRAME_CAPACITY; owner++) {
        if (benchmark_owned_frame_used[owner])
            continue;
        for (u32 physical = 0;
             physical < BENCHMARK_PHYSICAL_FRAME_COUNT; physical++) {
            if (benchmark_physical_frame_used[physical])
                continue;
            benchmark_physical_frame_used[physical] = true;
            benchmark_owned_frame_used[owner] = true;
            benchmark_owned_frame_generation[owner] =
                benchmark_next_frame_generation++;
            benchmark_owned_frame_physical[owner] =
                ((phys_addr_t)physical + 1U) * PAGE_SIZE;
            assert(benchmark_owned_frame_generation[owner] &&
                   benchmark_next_frame_generation);
            *out_frame = (pmm_owned_frame_t){
                .physical = benchmark_owned_frame_physical[owner],
                .generation = benchmark_owned_frame_generation[owner],
                .slot = owner,
            };
            assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
            return true;
        }
    }
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
    return false;
}

bool pmm_owned_frame_release(pmm_owned_frame_t *frame)
{
    if (!frame || !frame->physical || !frame->generation ||
        frame->slot >= PMM_OWNED_FRAME_CAPACITY)
        return false;
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    u32 slot = frame->slot;
    phys_addr_t physical = benchmark_owned_frame_physical[slot];
    bool valid = benchmark_owned_frame_used[slot] &&
        !benchmark_owned_frame_mapped[slot] &&
        frame->physical == physical &&
        frame->generation == benchmark_owned_frame_generation[slot];
    if (valid) {
        u64 physical_slot = physical / PAGE_SIZE - 1U;
        assert(physical_slot < BENCHMARK_PHYSICAL_FRAME_COUNT &&
               benchmark_physical_frame_used[physical_slot]);
        benchmark_physical_frame_used[physical_slot] = false;
        benchmark_owned_frame_used[slot] = false;
        benchmark_owned_frame_physical[slot] = 0U;
        *frame = (pmm_owned_frame_t){0};
    }
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
    return valid;
}

bool vmm_kernel_range_reserve(u64 page_count,
                              vmm_kernel_range_t *out_range)
{
    if (!out_range || out_range->base || out_range->page_count ||
        out_range->generation || page_count != 1U)
        return false;
    assert(benchmark_slab_base && benchmark_slab_capacity &&
           !(benchmark_slab_base & (PAGE_SIZE - 1U)));
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    for (u32 page = 0; page < BENCHMARK_SLAB_PAGE_COUNT; page++) {
        if (benchmark_slab_range_reserved[page])
            continue;
        benchmark_slab_range_reserved[page] = true;
        benchmark_slab_range_generation[page] =
            benchmark_next_range_generation++;
        assert(benchmark_slab_range_generation[page] &&
               benchmark_next_range_generation);
        *out_range = (vmm_kernel_range_t){
            .base = benchmark_slab_base + (usize)page * PAGE_SIZE,
            .page_count = 1U,
            .generation = benchmark_slab_range_generation[page],
        };
        assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
        return true;
    }
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
    return false;
}

static bool benchmark_slab_range_index(const vmm_kernel_range_t *range,
                                       u32 *page_out)
{
    if (!range || !range->base || range->page_count != 1U ||
        range->base < benchmark_slab_base ||
        range->base >= benchmark_slab_base + benchmark_slab_capacity ||
        ((range->base - benchmark_slab_base) & (PAGE_SIZE - 1U)))
        return false;
    *page_out = (u32)((range->base - benchmark_slab_base) / PAGE_SIZE);
    return *page_out < BENCHMARK_SLAB_PAGE_COUNT;
}

bool vmm_kernel_range_map_owned(const vmm_kernel_range_t *range,
                                u64 page_index,
                                const pmm_owned_frame_t *frame, u64 flags)
{
    u32 page;
    if (!frame || !frame->physical ||
        (frame->physical & (PAGE_SIZE - 1U)))
        return false;
    u64 physical_index = frame->physical / PAGE_SIZE - 1U;
    if (page_index || flags != (PTE_READWRITE | PTE_NX) ||
        frame->slot >= PMM_OWNED_FRAME_CAPACITY ||
        physical_index >= BENCHMARK_PHYSICAL_FRAME_COUNT ||
        !benchmark_slab_range_index(range, &page))
        return false;
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    bool valid = benchmark_slab_range_reserved[page] &&
        benchmark_slab_range_generation[page] == range->generation &&
        !benchmark_slab_range_mapped[page] &&
        benchmark_owned_frame_used[frame->slot] &&
        !benchmark_owned_frame_mapped[frame->slot] &&
        frame->physical == benchmark_owned_frame_physical[frame->slot] &&
        benchmark_physical_frame_used[physical_index] &&
        frame->generation ==
            benchmark_owned_frame_generation[frame->slot];
    if (valid) {
        benchmark_slab_range_mapped[page] = true;
        benchmark_slab_range_frame[page] = frame->slot;
        benchmark_owned_frame_mapped[frame->slot] = true;
        memset((void *)(uintptr_t)range->base, 0, PAGE_SIZE);
    }
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
    return valid;
}

bool vmm_kernel_range_unmap(vmm_kernel_range_t *range,
                            u64 first_page, u64 page_count)
{
    u32 page;
    if (first_page || page_count != 1U || spinlock_depth_current() ||
        !benchmark_slab_range_index(range, &page))
        return false;
    assert(benchmark_interrupts_enabled);
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    bool valid = benchmark_slab_range_reserved[page] &&
        benchmark_slab_range_generation[page] == range->generation &&
        benchmark_slab_range_mapped[page];
    if (valid) {
        u32 slot = benchmark_slab_range_frame[page];
        valid = slot < PMM_OWNED_FRAME_CAPACITY &&
                benchmark_owned_frame_used[slot] &&
                benchmark_owned_frame_mapped[slot];
        if (valid) {
            benchmark_owned_frame_mapped[slot] = false;
            benchmark_slab_range_mapped[page] = false;
            memset((void *)(uintptr_t)range->base, 0xdd, PAGE_SIZE);
        }
    }
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
    return valid;
}

bool vmm_kernel_range_release(vmm_kernel_range_t *range)
{
    u32 page;
    if (!benchmark_slab_range_index(range, &page))
        return false;
    assert(pthread_mutex_lock(&benchmark_owned_mutex) == 0);
    bool valid = benchmark_slab_range_reserved[page] &&
        benchmark_slab_range_generation[page] == range->generation &&
        !benchmark_slab_range_mapped[page];
    if (valid) {
        benchmark_slab_range_reserved[page] = false;
        *range = (vmm_kernel_range_t){0};
    }
    assert(pthread_mutex_unlock(&benchmark_owned_mutex) == 0);
    return valid;
}

void kprint(const char *format, ...) { (void)format; }
void panic(const char *message)
{
    fprintf(stderr, "allocator benchmark panic: %s\n", message);
    abort();
}

static u64 now_ns(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (u64)time.tv_sec * 1000000000ULL + (u64)time.tv_nsec;
}

static int run_classifier_comparison(void)
{
    enum { LOOKUP_SAMPLES = 32, LOOKUPS_PER_SAMPLE = 500000,
           PAGE_COUNT = 600, SEQUENCE_COUNT = 1024, OBJECTS_PER_PAGE = 50 };
    void **objects = calloc((usize)PAGE_COUNT * OBJECTS_PER_PAGE,
                            sizeof(*objects));
    usize page_indices[PAGE_COUNT];
    usize slab_page_count = benchmark_slab_capacity / PAGE_SIZE;
    bool *seen = calloc(slab_page_count, sizeof(*seen));
    slab_classifier_stats_t stats;
    u32 populated_pages = 0;
    u32 object_count = 0;
    u64 sparse_checksum = 0;
    u64 root_checksum = 0;
    u64 dense_checksum = 0;
    u64 sparse_results[LOOKUP_SAMPLES];
    u64 root_results[LOOKUP_SAMPLES];
    u64 dense_class_results[LOOKUP_SAMPLES];
    assert(objects && seen);
    u64 started = now_ns();

    assert(slab_classifier_ensure(0U));
    printf("classifier_growth,%llu\n",
           (unsigned long long)(now_ns() - started));
    started = now_ns();
    for (u32 index = 0; populated_pages < PAGE_COUNT; index++) {
        void *object = slab_alloc(72U);
        assert(object && index < (u32)(benchmark_heap_capacity / PAGE_SIZE) *
                                  OBJECTS_PER_PAGE);
        objects[index] = object;
        object_count = index + 1U;
        usize page_index = ((uintptr_t)object - slab_range_start) /
                           PAGE_SIZE;
        assert(page_index < slab_page_count);
        if (!seen[page_index]) {
            seen[page_index] = true;
            page_indices[populated_pages++] = page_index;
            u64 token = slab_page_token_load(page_index);
            assert(slab_token_class(token) == 5U);
            dense_classifier_reference[page_index] =
                slab_token_class(token);
        }
    }
    printf("classifier_population,%llu\n",
           (unsigned long long)(now_ns() - started));

    assert(slab_classifier_root_entries <= BENCH_CLASSIFIER_ROOT_ENTRIES);
    for (usize index = 0; index < slab_classifier_root_entries; index++)
        __atomic_store_n(&benchmark_classifier_root[index],
                         __atomic_load_n(&slab_classifier_root_storage[index],
                                         __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);

    /* A fixed pseudo-random permutation makes both lookup implementations
     * visit varying page indices while preserving identical address order. */
    u16 sequence[SEQUENCE_COUNT];
    u32 state = 0x9e3779b9U;
    for (u32 index = 0; index < SEQUENCE_COUNT; index++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        sequence[index] = (u16)(state % PAGE_COUNT);
    }
    for (u32 sample = 0; sample < LOOKUP_SAMPLES; sample++) {
        u64 checksum = 0;
        u64 start = now_ns();
        for (u32 lookup = 0; lookup < LOOKUPS_PER_SAMPLE; lookup++) {
            usize page_index = page_indices[sequence[lookup &
                                                       (SEQUENCE_COUNT - 1U)]];
            checksum += slab_page_token_load(page_index);
        }
        sparse_results[sample] = checksum;
        sparse_checksum ^= checksum;
        printf("lookup,sparse,%u,%u,%llu,%llu\n", sample,
               LOOKUPS_PER_SAMPLE,
               (unsigned long long)(now_ns() - start),
               (unsigned long long)checksum);
    }
    for (u32 sample = 0; sample < LOOKUP_SAMPLES; sample++) {
        u64 checksum = 0;
        u64 start = now_ns();
        for (u32 lookup = 0; lookup < LOOKUPS_PER_SAMPLE; lookup++) {
            usize page_index = page_indices[sequence[lookup &
                                                       (SEQUENCE_COUNT - 1U)]];
            usize root_index = page_index / SLAB_CLASSIFIER_ROOT_PAGES;
            usize leaf_index = page_index / SLAB_CLASSIFIER_FANOUT;
            uintptr_t value = __atomic_load_n(
                &benchmark_classifier_root[root_index], __ATOMIC_ACQUIRE);
            slab_classifier_middle_t *middle =
                (slab_classifier_middle_t *)value;
            if (middle) {
                slab_classifier_require_metadata(middle, sizeof(*middle),
                    "slab benchmark: root experiment middle out of range");
                assert(middle->magic == SLAB_CLASSIFIER_MIDDLE_MAGIC &&
                       middle->root_index == root_index);
                slab_classifier_leaf_t *leaf = slab_classifier_leaf(
                    middle, leaf_index, false);
                if (leaf)
                    checksum += __atomic_load_n(
                        &leaf->page_tokens[page_index %
                            SLAB_CLASSIFIER_FANOUT], __ATOMIC_ACQUIRE);
            }
        }
        root_results[sample] = checksum;
        root_checksum ^= checksum;
        printf("lookup,direct_root,%u,%u,%llu,%llu\n", sample,
               LOOKUPS_PER_SAMPLE,
               (unsigned long long)(now_ns() - start),
               (unsigned long long)checksum);
    }
    for (u32 sample = 0; sample < LOOKUP_SAMPLES; sample++) {
        u64 checksum = 0;
        u64 start = now_ns();
        for (u32 lookup = 0; lookup < LOOKUPS_PER_SAMPLE; lookup++) {
            usize page_index = page_indices[sequence[lookup &
                                                       (SEQUENCE_COUNT - 1U)]];
            checksum += __atomic_load_n(
                &dense_classifier_reference[page_index], __ATOMIC_ACQUIRE);
        }
        dense_class_results[sample] = checksum;
        dense_checksum ^= checksum;
        printf("lookup,dense,%u,%u,%llu,%llu\n", sample,
               LOOKUPS_PER_SAMPLE,
               (unsigned long long)(now_ns() - start),
               (unsigned long long)checksum);
    }
    assert(sparse_checksum == root_checksum &&
           (u8)(sparse_checksum & 0xffU) == (u8)(dense_checksum & 0xffU));
    for (u32 sample = 0; sample < LOOKUP_SAMPLES; sample++) {
        assert(sparse_results[sample] == root_results[sample]);
        assert(dense_class_results[sample] ==
               5ULL * LOOKUPS_PER_SAMPLE);
    }
    printf("classifier_pages,%u,%u,%llu\n", populated_pages, PAGE_COUNT,
           (unsigned long long)(sparse_checksum ^ dense_checksum));
    assert(slab_get_classifier_stats(&stats));
    printf("classifier_payload,%llu,%llu,%llu,%llu,%llu,%llu\n",
           (unsigned long long)stats.root_payload_bytes,
           (unsigned long long)stats.static_root_bytes,
           (unsigned long long)stats.middle_payload_bytes,
           (unsigned long long)stats.leaf_payload_bytes,
           (unsigned long long)stats.retained_payload_bytes,
           (unsigned long long)stats.peak_payload_bytes);
    for (u32 index = 0; index < object_count; index++)
        assert(slab_try_free(objects[index]));
    assert(slab_verify());
    printf("classifier_heap_used,%llu\n",
           (unsigned long long)heap_get_used_size());
    free(objects);
    free(seen);
    return 0;
}

/* Exact pre-ctz production selector retained only as a benchmark control. */
__attribute__((noinline))
static bool slab_find_free_slot_historical_impl(slab_descriptor_t *slab,
                                                u32 class_index,
                                                u16 *slot_out)
{
    u64 *bitmap = slab_bitmap(slab);
    u32 slots = slab->slot_count;

    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        u32 first_slot = word * 64U;
        u32 remaining = slots > first_slot ? slots - first_slot : 0U;
        u64 valid_mask = remaining >= 64U ? ~(u64)0 :
            (remaining ? (((u64)1U << remaining) - 1U) : 0U);
        u64 available;
        slab_classes[class_index].stats.bitmap_words_examined++;
        available = ~bitmap[word] & valid_mask;
        if (available) {
            u32 bit = 0;
            while (!(available & ((u64)1U << bit))) bit++;
            *slot_out = (u16)(first_slot + bit);
            return true;
        }
    }
    return false;
}

/* Match the historical selector's call boundary.  The production helper is
 * otherwise eligible for inlining into the timed loop at -O2, which would
 * make a function-call comparison favor the current implementation. */
__attribute__((noinline))
static bool slab_find_free_slot_production(slab_descriptor_t *slab,
                                            u32 class_index, u16 *slot_out)
{
    return slab_find_free_slot_locked(slab, class_index, slot_out);
}

/* Give the historical body the same wrapper and timed call boundary. */
__attribute__((noinline))
static bool slab_find_free_slot_historical(slab_descriptor_t *slab,
                                           u32 class_index, u16 *slot_out)
{
    return slab_find_free_slot_historical_impl(slab, class_index, slot_out);
}

/* Exercise the production bitmap selector at controlled occupancies.  The
 * descriptors live in an array so the compiler cannot fold one fixed bitmap
 * into a constant result.  The timed operation compares the current helper
 * with its exact pre-ctz selector; descriptor setup and checksum output stay
 * outside the timed regions. */
static int run_bitmap_search_benchmark(void)
{
    enum { DESCRIPTOR_COUNT = 1024, SAMPLES = 17, ITERATIONS = 10000 };
    static const u32 class_indices[] = {0U, 4U, 19U};
    static const u16 occupancies[] = {
        0U, 1U, 2U, 5U, 10U, 31U, 63U, 64U, 65U,
        127U, 128U, 200U, 249U, 250U
    };
    typedef union {
        max_align_t alignment;
        u8 bytes[SLAB_OBJECT_AREA_OFFSET];
    } descriptor_storage_t;
    descriptor_storage_t *storage = calloc(DESCRIPTOR_COUNT,
                                            sizeof(*storage));
    assert(storage);
    slab_descriptor_t **descriptors = calloc(DESCRIPTOR_COUNT,
                                              sizeof(*descriptors));
    assert(descriptors);
    for (u32 index = 0; index < DESCRIPTOR_COUNT; index++)
        descriptors[index] = (slab_descriptor_t *)storage[index].bytes;

    for (usize class_position = 0;
         class_position < sizeof(class_indices) / sizeof(class_indices[0]);
         class_position++) {
        u32 class_index = class_indices[class_position];
        u16 slots = slab_slots_for_class(class_index);
        for (usize occupancy_index = 0;
             occupancy_index < sizeof(occupancies) / sizeof(occupancies[0]);
             occupancy_index++) {
            u16 occupied = occupancies[occupancy_index];
            if (occupied > slots)
                continue;
            for (u32 index = 0; index < DESCRIPTOR_COUNT; index++) {
                descriptors[index]->slot_count = slots;
                u64 *bitmap = slab_bitmap(descriptors[index]);
                for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
                    u32 first = word * 64U;
                    u32 count = occupied > first ? occupied - first : 0U;
                    bitmap[word] = count >= 64U ? ~(u64)0 :
                        (count ? (((u64)1U << count) - 1U) : 0U);
                    slab_update_bitmap_word_integrity(descriptors[index],
                                                      word);
                }
            }

            for (u32 sample = 0; sample < SAMPLES; sample++) {
                for (u32 order = 0; order < 2U; order++) {
                    bool production = (sample + order) % 2U == 0U;
                    u64 checksum = 0;
                    u32 found_count = 0;
                    u64 started = now_ns();
                    for (u32 operation = 0; operation < ITERATIONS;
                         operation++) {
                        u32 index = (operation * 613U + sample * 97U) %
                                    DESCRIPTOR_COUNT;
                        u16 slot = 0;
                        bool found = production
                            ? slab_find_free_slot_production(
                                  descriptors[index], class_index, &slot)
                            : slab_find_free_slot_historical(
                                  descriptors[index], class_index, &slot);
                        found_count += found;
                        if (found)
                            checksum += slot;
                    }
                    u32 expected_found = occupied < slots ? ITERATIONS : 0U;
                    u64 expected_checksum = (u64)occupied * expected_found;
                    assert(found_count == expected_found &&
                           checksum == expected_checksum);
                    printf("bitmap,%s,%u,%u,%u,%u,%llu,%u,%llu\n",
                           production ? "production" : "historical",
                           class_index,
                           occupied, sample, ITERATIONS,
                           (unsigned long long)(now_ns() - started),
                           found_count, (unsigned long long)checksum);
                }
            }
        }
    }
    free(descriptors);
    free(storage);
    return 0;
}

static bool use_slab;
static bool use_cpu_slab;
static bool use_public_api;
static void *allocate(usize request);
static void release(void *object);

typedef struct {
    void *pointer;
    usize requested;
    u32 allocation_cpu;
    u8 tag;
    bool live;
    bool slab_owned;
} replay_object_t;

typedef struct {
    u32 sequence;
    u32 kind;
    u32 object_id;
    usize requested;
    u32 cpu;
    u32 context;
    u32 phase;
    u32 caller_text_offset;
} replay_event_t;

static const char *selected_mode_name(void)
{
    return use_public_api ? "public-api" :
           (use_cpu_slab ? "cpu-slab" :
            (use_slab ? "slab" : "heap"));
}

static int run_separated_operation_benchmark(bool slab_path,
                                             bool cpu_slab_path)
{
    enum { SAMPLES = 64, MAX_BATCH = 16 };
    static const usize requests[] = {16U, 72U, 192U, 384U};
    use_slab = slab_path;
    use_cpu_slab = cpu_slab_path;

    for (usize request_index = 0;
         request_index < sizeof(requests) / sizeof(requests[0]);
         request_index++) {
        usize request = requests[request_index];
        u32 class_index = 0;
        while (class_index < SLAB_CLASS_COUNT &&
               slab_class_size(class_index) < request)
            class_index++;
        usize slots = slab_class_slots(class_index);
        usize batch = slots / 5U;
        if (batch < 2U) batch = 2U;
        if (batch > MAX_BATCH) batch = MAX_BATCH;
        if (batch >= slots) batch = slots - 1U;
        usize filler_count = slots - batch - 2U;
        void **fillers = calloc(filler_count ? filler_count : 1U,
                                sizeof(*fillers));
        void **batch_objects = calloc(batch, sizeof(*batch_objects));
        assert(fillers && batch_objects);
        void *anchor = allocate(request);
        assert(anchor);
        for (usize index = 0; index < filler_count; index++) {
            fillers[index] = allocate(request);
            assert(fillers[index]);
        }

        for (u32 sample = 0; sample < SAMPLES; sample++) {
            u64 started = now_ns();
            for (usize index = 0; index < batch; index++) {
                batch_objects[index] = allocate(request);
                assert(batch_objects[index]);
            }
            u64 allocation_elapsed = now_ns() - started;
            printf("phase,%s,alloc,%llu,%u,%llu,%llu\n",
                   cpu_slab_path ? "cpu-slab" :
                       (slab_path ? "slab" : "heap"),
                   (unsigned long long)request, sample,
                   (unsigned long long)batch,
                   (unsigned long long)allocation_elapsed);

            started = now_ns();
            for (usize index = 0; index < batch; index++)
                release(batch_objects[index]);
            u64 free_elapsed = now_ns() - started;
            printf("phase,%s,free,%llu,%u,%llu,%llu\n",
                   cpu_slab_path ? "cpu-slab" :
                       (slab_path ? "slab" : "heap"),
                   (unsigned long long)request, sample,
                   (unsigned long long)batch,
                   (unsigned long long)free_elapsed);
        }
        for (usize index = 0; index < filler_count; index++)
            release(fillers[index]);
        release(anchor);
        if (slab_path)
            assert(slab_verify());
        free(batch_objects);
        free(fillers);
    }
    return 0;
}

static int run_lifecycle_benchmark(void)
{
    enum { SAMPLES = 32 };
    static const usize requests[] = {16U, 72U, 192U, 384U};
    use_slab = true;
    usize initially_drained = slab_drain_empty();
    for (usize request_index = 0;
         request_index < sizeof(requests) / sizeof(requests[0]);
         request_index++) {
        usize request = requests[request_index];
        u32 class_index = 0U;
        while (class_index < SLAB_CLASS_COUNT &&
               slab_class_size(class_index) < request)
            class_index++;
        assert(class_index < SLAB_CLASS_COUNT);
        for (u32 sample = 0; sample < SAMPLES; sample++) {
            slab_class_stats_t before, after;
            assert(slab_get_class_stats(class_index, &before));
            u64 started = now_ns();
            void *object = slab_alloc(request);
            u64 allocation_elapsed = now_ns() - started;
            assert(object);
            started = now_ns();
            assert(slab_try_free(object));
            u64 free_elapsed = now_ns() - started;
            assert(slab_get_class_stats(class_index, &after));
            printf("lifecycle,%llu,%u,%llu,%llu\n",
                   (unsigned long long)request, sample,
                   (unsigned long long)allocation_elapsed,
                   (unsigned long long)free_elapsed);
            printf("lifecycle_stats,%llu,%u,%llu,%llu,%llu,%llu,%llu\n",
                   (unsigned long long)request, sample,
                   (unsigned long long)(after.backing_allocations -
                                        before.backing_allocations),
                   (unsigned long long)(after.backing_releases -
                                        before.backing_releases),
                   (unsigned long long)(after.empty_slab_reuses -
                                        before.empty_slab_reuses),
                   (unsigned long long)after.empty_slabs,
                   (unsigned long long)(after.retired_slabs -
                                        before.retired_slabs));
        }
    }
    printf("lifecycle_setup_drain,%llu\n",
           (unsigned long long)initially_drained);
    printf("lifecycle_final_drain,%llu\n",
           (unsigned long long)slab_drain_empty());
    slab_classifier_stats_t classifier;
    assert(slab_get_classifier_stats(&classifier));
    printf("lifecycle_classifier,%llu,%llu,%llu,%llu\n",
           (unsigned long long)classifier.retained_payload_bytes,
           (unsigned long long)classifier.peak_payload_bytes,
           (unsigned long long)classifier.middle_payload_bytes,
           (unsigned long long)classifier.leaf_payload_bytes);
    assert(slab_verify());
    return 0;
}

static u64 benchmark_touch_payload(void *object, usize request,
                                   u32 worker, u32 operation)
{
    volatile u8 *bytes = object;
    u8 first = (u8)(0x5dU ^ worker ^ operation);
    u8 last = (u8)(first ^ 0xa7U);
    assert(object && request);
    bytes[0] = first;
    if (request > 1U)
        bytes[request - 1U] = last;
    assert(bytes[0] == first &&
           (request == 1U || bytes[request - 1U] == last));
    return ((u64)bytes[0] << 8U) |
           (request == 1U ? bytes[0] : bytes[request - 1U]);
}

static void *allocate(usize request)
{
    if (use_public_api)
        return kmalloc(request);
    return use_cpu_slab ? slab_cpu_alloc(request) :
           (use_slab ? slab_alloc(request) : kmalloc(request));
}

static void release(void *object)
{
    if (use_public_api) {
        kfree(object);
        return;
    }
    if (use_slab) {
        assert(use_cpu_slab ? slab_cpu_try_free(object) :
                              slab_try_free(object));
    } else {
        kfree(object);
    }
}

typedef struct {
    void *object;
    usize request;
    u32 generation;
    u8 tag;
    bool live;
    bool heap_fallback;
} population_object_t;

enum { POPULATION_OBJECTS = 1024 };

static u64 population_write_and_check(population_object_t *record)
{
    volatile u8 *bytes = record->object;
    u8 last = (u8)(record->tag ^ 0xa7U);
    assert(record->object && record->request >= 2U);
    bytes[0] = record->tag;
    bytes[record->request - 1U] = last;
    assert(bytes[0] == record->tag &&
           bytes[record->request - 1U] == last);
    record->live = true;
    return ((u64)bytes[0] << 8U) | bytes[record->request - 1U];
}

static void population_check_ranges(population_object_t *objects)
{
    for (u32 left = 0; left < POPULATION_OBJECTS; left++) {
        if (!objects[left].live)
            continue;
        uintptr_t left_start = (uintptr_t)objects[left].object;
        uintptr_t left_end = left_start + objects[left].request;
        assert(left_end > left_start);
        for (u32 right = left + 1U; right < POPULATION_OBJECTS; right++) {
            if (!objects[right].live)
                continue;
            uintptr_t right_start = (uintptr_t)objects[right].object;
            uintptr_t right_end = right_start + objects[right].request;
            assert(right_end > right_start);
            assert(left_end <= right_start || right_end <= left_start);
        }
    }
}

static u64 population_live_requested(population_object_t *objects)
{
    u64 requested = 0U;
    for (u32 index = 0; index < POPULATION_OBJECTS; index++) {
        if (objects[index].live) {
            volatile u8 *bytes = objects[index].object;
            assert(bytes[0] == objects[index].tag &&
                   bytes[objects[index].request - 1U] ==
                       (u8)(objects[index].tag ^ 0xa7U));
            requested += objects[index].request;
        }
    }
    return requested;
}

static void population_heap_fragmentation(u64 *largest_free_block,
                                          u64 *free_block_count)
{
    heap_block_t *block = kernel_heap.first;
    u64 free_payload_sum = 0U;
    u64 largest = 0U;
    u64 free_blocks = 0U;
    u64 visited = 0U;
    u64 block_limit = benchmark_heap_capacity / sizeof(heap_block_t) + 1U;
    while (block) {
        assert(++visited <= block_limit);
        if (block->free) {
            free_payload_sum += block->size;
            free_blocks++;
            if (block->size > largest)
                largest = block->size;
        }
        block = block->next;
    }
    assert(free_payload_sum == heap_get_free_size());
    *largest_free_block = largest;
    *free_block_count = free_blocks;
}

static void population_slab_capacity(bool slab_path,
                                     u64 *free_slot_bytes,
                                     u64 *cached_slot_bytes,
                                     u64 *backing_bytes)
{
    u64 free_bytes = 0U, cached_bytes = 0U, backing = 0U;
    if (slab_path) {
        for (u32 class_index = 0U; class_index < SLAB_CLASS_COUNT;
             class_index++) {
            slab_class_stats_t stats;
            assert(slab_get_class_stats(class_index, &stats));
            u64 class_size = slab_class_size(class_index);
            u64 slots = (stats.partial_slabs + stats.full_slabs +
                         stats.empty_slabs) *
                        slab_class_slots(class_index);
            assert(stats.live_slots + stats.cached_slots <= slots);
            free_bytes += (slots - stats.live_slots - stats.cached_slots) *
                          class_size;
            cached_bytes += stats.cached_slots * class_size;
            backing += (stats.partial_slabs + stats.full_slabs +
                        stats.empty_slabs) * PAGE_SIZE;
        }
    }
    *free_slot_bytes = free_bytes;
    *cached_slot_bytes = cached_bytes;
    *backing_bytes = backing;
}

static void population_snapshot(const char *mode, const char *phase,
                                population_object_t *objects,
                               u32 operations, u64 elapsed_ns,
                               bool slab_path)
{
    u64 live_requested = population_live_requested(objects);
    u64 largest_free_block = 0U, free_block_count = 0U;
    u64 slab_free_slot_bytes = 0U, slab_cached_slot_bytes = 0U;
    u64 slab_backing_bytes = 0U;
    population_check_ranges(objects);
    population_heap_fragmentation(&largest_free_block, &free_block_count);
    population_slab_capacity(slab_path, &slab_free_slot_bytes,
                             &slab_cached_slot_bytes, &slab_backing_bytes);
    printf("population_phase,%s,%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", mode,
           phase, operations, (unsigned long long)elapsed_ns,
           (unsigned long long)live_requested,
           (unsigned long long)heap_get_used_size(),
           (unsigned long long)heap_get_free_size(),
           (unsigned long long)largest_free_block,
           (unsigned long long)free_block_count,
           (unsigned long long)((uintptr_t)kernel_heap.end -
                                benchmark_heap_base),
           (unsigned long long)heap_get_total_size(),
           (unsigned long long)slab_free_slot_bytes,
           (unsigned long long)slab_cached_slot_bytes,
           (unsigned long long)slab_backing_bytes);
}

static void population_resource_snapshot(const char *mode, const char *phase,
                                         bool slab_path)
{
    u64 backing_allocations = 0U, backing_releases = 0U;
    u64 live_slots = 0U, cached_slots = 0U;
    u64 partial_slabs = 0U, full_slabs = 0U, empty_slabs = 0U;
    slab_classifier_stats_t classifier = {0};
    if (slab_path) {
        for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
             class_index++) {
            slab_class_stats_t stats;
            assert(slab_get_class_stats(class_index, &stats));
            backing_allocations += stats.backing_allocations;
            backing_releases += stats.backing_releases;
            live_slots += stats.live_slots;
            cached_slots += stats.cached_slots;
            partial_slabs += stats.partial_slabs;
            full_slabs += stats.full_slabs;
            empty_slabs += stats.empty_slabs;
            printf("population_class,%s,%s,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                   mode, phase, class_index,
                   (u32)slab_class_size(class_index),
                   (unsigned long long)stats.backing_allocations,
                   (unsigned long long)stats.backing_releases,
                   (unsigned long long)stats.live_slots,
                   (unsigned long long)stats.cached_slots,
                   (unsigned long long)stats.partial_slabs,
                   (unsigned long long)stats.full_slabs,
                   (unsigned long long)stats.empty_slabs);
        }
        assert(slab_get_classifier_stats(&classifier));
    }
    printf("population_resources,%s,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
           mode, phase, (unsigned long long)heap_get_used_size(),
           (unsigned long long)heap_get_total_size(),
           (unsigned long long)((uintptr_t)kernel_heap.end -
                                benchmark_heap_base),
           (unsigned long long)backing_allocations,
           (unsigned long long)backing_releases,
           (unsigned long long)live_slots,
           (unsigned long long)cached_slots,
           (unsigned long long)partial_slabs,
           (unsigned long long)full_slabs,
           (unsigned long long)empty_slabs,
           (unsigned long long)classifier.retained_payload_bytes);
    printf("population_classifier,%s,%s,%llu,%llu\n", mode, phase,
           (unsigned long long)classifier.retained_payload_bytes,
           (unsigned long long)classifier.peak_payload_bytes);
}

static int run_population_benchmark(const char *mode, bool slab_path,
                                    bool cpu_slab_path)
{
    static const usize requests[] = {16U, 32U, 72U, 128U,
                                     192U, 256U, 384U, 385U};
    static population_object_t objects[POPULATION_OBJECTS];
    use_slab = slab_path;
    use_cpu_slab = cpu_slab_path;
    memset(objects, 0, sizeof(objects));
    u64 checksum = 0U;

    u64 started = now_ns();
    for (u32 index = 0; index < POPULATION_OBJECTS; index++) {
        population_object_t *record = &objects[index];
        record->request = requests[index %
            (sizeof(requests) / sizeof(requests[0]))];
        record->generation = 1U;
        record->tag = (u8)(index * 37U + record->request);
        record->heap_fallback = use_public_api
            ? record->request > KERNEL_HYBRID_ALLOCATOR_CUTOFF
            : slab_path && record->request > SLAB_MAX_OBJECT_SIZE;
        record->object = record->heap_fallback
            ? kmalloc(record->request) : allocate(record->request);
        assert(record->object);
        checksum += population_write_and_check(record);
    }
    population_snapshot(mode, "fill-live-set", objects,
                       POPULATION_OBJECTS, now_ns() - started, slab_path);

    started = now_ns();
    for (u32 index = 1U; index < POPULATION_OBJECTS; index += 2U) {
        if (objects[index].heap_fallback)
            kfree(objects[index].object);
        else
            release(objects[index].object);
        objects[index].live = false;
    }
    u64 elapsed = now_ns() - started;
    population_snapshot(mode, "free-short-lived-half", objects,
                       POPULATION_OBJECTS / 2U, elapsed, slab_path);

    started = now_ns();
    for (u32 index = 1U; index < POPULATION_OBJECTS; index += 2U) {
        population_object_t *record = &objects[index];
        record->generation++;
        record->tag = (u8)(record->tag + 53U);
        record->heap_fallback = use_public_api
            ? record->request > KERNEL_HYBRID_ALLOCATOR_CUTOFF
            : slab_path && record->request > SLAB_MAX_OBJECT_SIZE;
        record->object = record->heap_fallback
            ? kmalloc(record->request) : allocate(record->request);
        assert(record->object);
        checksum += population_write_and_check(record);
    }
    population_snapshot(mode, "replace-short-lived-half", objects,
                       POPULATION_OBJECTS / 2U, now_ns() - started,
                       slab_path);

    started = now_ns();
    for (u32 index = 0; index < POPULATION_OBJECTS; index++) {
        assert(objects[index].live);
        if (objects[index].heap_fallback)
            kfree(objects[index].object);
        else
            release(objects[index].object);
        objects[index].live = false;
    }
    population_snapshot(mode, "release-all", objects,
                       POPULATION_OBJECTS, now_ns() - started, slab_path);

    if (slab_path) {
        population_resource_snapshot(mode, "before-drain", true);
        if (cpu_slab_path)
            printf("population_drain,%s,%llu\n", mode,
                   (unsigned long long)slab_cpu_drain_all());
        population_resource_snapshot(mode, "after-drain", true);
        assert(slab_verify());
    } else {
        assert(heap_get_used_size() == 0U);
        population_resource_snapshot(mode, "before-drain", false);
        population_resource_snapshot(mode, "after-drain", false);
    }
    printf("population_result,%s,%u,%llu\n", mode, POPULATION_OBJECTS,
           (unsigned long long)checksum);
    return 0;
}

static u32 cache_backing_pages_for_class(u32 class_index)
{
    bool seen[BENCHMARK_SLAB_PAGE_COUNT] = {false};
    u32 pages = 0U;

    for (u32 cpu = 0; cpu < slab_cpu_cache_count; cpu++) {
        slab_cpu_cache_t *cache = slab_cpu_cache(cpu, class_index);
        for (u32 entry = 0; entry < cache->count; entry++) {
            uintptr_t address = (uintptr_t)cache->entries[entry].object;
            usize page = (address - slab_range_start) / PAGE_SIZE;
            assert(address >= slab_range_start &&
                   address < slab_range_end &&
                   page < benchmark_slab_capacity / PAGE_SIZE);
            if (!seen[page]) {
                seen[page] = true;
                pages++;
            }
        }
    }
    return pages;
}

typedef struct {
    usize request;
    u32 pairs;
    pthread_barrier_t *start_barrier;
    pthread_barrier_t *witness_done_barrier;
    u32 cpu_index;
    int host_cpu;
    int cpu_at_start;
    int cpu_at_end;
    u64 witness_pairs;
    u64 timed_pairs;
    u64 checksum;
} worker_args_t;

enum { BENCHMARK_WORKERS = 4, WITNESS_PAIRS = 2048 };
static _Atomic u32 benchmark_witness_active;
static _Atomic u32 benchmark_witness_peak;
static _Atomic u32 benchmark_timed_workers_done;
static _Atomic bool benchmark_timed_start;

static bool benchmark_select_worker_cpus(void)
{
#ifdef __linux__
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) != 0)
        return false;
    u32 found = 0U;
    for (int cpu = 0; cpu < CPU_SETSIZE &&
                       found < BENCHMARK_WORKERS; cpu++) {
        if (CPU_ISSET(cpu, &available))
            benchmark_worker_cpus[found++] = cpu;
    }
    return found == BENCHMARK_WORKERS;
#else
    return false;
#endif
}

static usize concurrent_request(const worker_args_t *args, u32 pair)
{
    static const usize mixed[] = {16U, 72U, 192U, 384U};
    return args->request ? args->request :
           mixed[(pair + args->cpu_index) %
                 (sizeof(mixed) / sizeof(mixed[0]))];
}

static void benchmark_record_witness_overlap(void)
{
    u32 active = atomic_fetch_add_explicit(&benchmark_witness_active, 1U,
                                            memory_order_relaxed) + 1U;
    u32 peak = atomic_load_explicit(&benchmark_witness_peak,
                                    memory_order_relaxed);
    while (active > peak &&
           !atomic_compare_exchange_weak_explicit(&benchmark_witness_peak,
               &peak, active, memory_order_relaxed, memory_order_relaxed))
        ;
}

static void *worker(void *argument)
{
    worker_args_t *args = argument;
    benchmark_cpu_index = args->cpu_index;
#ifdef __linux__
    cpu_set_t target;
    CPU_ZERO(&target);
    CPU_SET(args->host_cpu, &target);
    assert(pthread_setaffinity_np(pthread_self(), sizeof(target),
                                  &target) == 0);
    args->cpu_at_start = sched_getcpu();
    assert(args->cpu_at_start == args->host_cpu);
#else
    args->cpu_at_start = -1;
#endif
    usize warm_request = args->request ? args->request : 16U;
    void *warm = allocate(warm_request);
    assert(warm);
        (void)benchmark_touch_payload(warm, warm_request,
                                      args->cpu_index, 0U);
    release(warm);
    int result = pthread_barrier_wait(args->start_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);

    /* This instrumented phase is a witness only. The atomics deliberately
     * stay out of the throughput interval below. An active count above one
     * proves that allocator-call brackets overlapped on pinned CPUs. */
    for (u32 pair = 0; pair < WITNESS_PAIRS; pair++) {
        usize request = concurrent_request(args, pair);
        benchmark_record_witness_overlap();
        void *object = allocate(request);
        assert(object);
        u64 payload = benchmark_touch_payload(object, request,
                                               args->cpu_index, pair);
        release(object);
        args->checksum += payload ^ ((u64)request << 16U);
        args->witness_pairs++;
        atomic_fetch_sub_explicit(&benchmark_witness_active, 1U,
                                  memory_order_relaxed);
    }

    result = pthread_barrier_wait(args->witness_done_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    while (!atomic_load_explicit(&benchmark_timed_start,
                                 memory_order_acquire))
        sched_yield();
    for (u32 pair = 0; pair < args->pairs; pair++) {
        usize request = concurrent_request(args, pair);
        void *object = allocate(request);
        assert(object);
        u64 payload = benchmark_touch_payload(object, request,
                                               args->cpu_index, pair);
        release(object);
        args->checksum += payload ^ ((u64)request << 16U);
        args->timed_pairs++;
    }
#ifdef __linux__
    args->cpu_at_end = sched_getcpu();
    assert(args->cpu_at_end == args->host_cpu);
#else
    args->cpu_at_end = -1;
#endif
    atomic_fetch_add_explicit(&benchmark_timed_workers_done, 1U,
                              memory_order_release);
    return NULL;
}

static int run_profile_size_replay(const char *mode, const char *trace_path)
{
    FILE *trace = fopen(trace_path, "r");
    usize *requests = NULL;
    usize count = 0U, capacity = 0U;
    char line[64];
    u64 checksum = 0U;
    enum { REPEATS = 10 };

    assert(trace);
    while (fgets(line, sizeof(line), trace)) {
        char *end = NULL;
        unsigned long value = strtoul(line, &end, 10);
        assert(end != line && (*end == '\n' || *end == '\0'));
        assert(value > 0U && value <= SLAB_MAX_OBJECT_SIZE);
        if (count == capacity) {
            usize next = capacity ? capacity * 2U : 256U;
            usize *grown = realloc(requests, next * sizeof(*requests));
            assert(grown);
            requests = grown;
            capacity = next;
        }
        requests[count++] = (usize)value;
    }
    assert(!ferror(trace) && fclose(trace) == 0 && count);

    /* Warm the identical size multiset before timing. This replays size
     * frequency only; the source profile does not retain an ordered object
     * lifetime trace, so the measured objects have immediate lifetimes. */
    for (usize index = 0; index < count; index++) {
        void *object = allocate(requests[index]);
        assert(object);
        checksum += benchmark_touch_payload(object, requests[index], 0U,
                                             (u32)index);
        release(object);
    }
    u64 started = now_ns();
    for (u32 repeat = 0; repeat < REPEATS; repeat++) {
        for (usize index = 0; index < count; index++) {
            void *object = allocate(requests[index]);
            assert(object);
            checksum += benchmark_touch_payload(object, requests[index],
                                                 repeat + 1U, (u32)index);
            release(object);
        }
    }
    u64 elapsed = now_ns() - started;
    printf("profile_replay,%s,%llu,%u,%llu,%llu\n", mode,
           (unsigned long long)count, REPEATS,
           (unsigned long long)elapsed,
           (unsigned long long)checksum);
    printf("profile_resource,%s,%llu,%llu\n", mode,
           (unsigned long long)heap_get_used_size(),
           (unsigned long long)((uintptr_t)kernel_heap.end -
                                benchmark_heap_base));
    if (slab_is_initialized()) {
        for (u32 class_index = 0U; class_index < SLAB_CLASS_COUNT;
             class_index++) {
            slab_class_stats_t stats;
            assert(slab_get_class_stats(class_index, &stats));
            printf("profile_class,%s,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                   mode, class_index, slab_class_size(class_index),
                   (unsigned long long)stats.allocations,
                   (unsigned long long)stats.frees,
                   (unsigned long long)stats.cache_hits,
                   (unsigned long long)stats.cache_misses,
                   (unsigned long long)stats.cache_refills,
                   (unsigned long long)stats.cache_overflows,
                   (unsigned long long)stats.cached_slots,
                   (unsigned long long)stats.backing_allocations,
                   (unsigned long long)stats.backing_releases,
                   (unsigned long long)stats.live_slots,
                   (unsigned long long)stats.partial_slabs,
                   (unsigned long long)stats.full_slabs);
        }
        slab_classifier_stats_t classifier;
        assert(slab_get_classifier_stats(&classifier));
        printf("profile_classifier,%s,%llu,%llu\n", mode,
               (unsigned long long)classifier.retained_payload_bytes,
               (unsigned long long)classifier.peak_payload_bytes);
        if (use_cpu_slab)
            printf("profile_drain,%s,%llu\n", mode,
                   (unsigned long long)slab_cpu_drain_all());
        assert(slab_verify());
        printf("profile_resource_after_drain,%s,%llu,%llu\n", mode,
               (unsigned long long)heap_get_used_size(),
               (unsigned long long)((uintptr_t)kernel_heap.end -
                                    benchmark_heap_base));
    }
    free(requests);
    return 0;
}

static bool read_profile_events(const char *path, replay_event_t **events_out,
                                u32 *count_out)
{
    FILE *file = fopen(path, "r");
    replay_event_t *events = NULL;
    char line[256];
    u32 count = 0U;
    char checksum[65];
    bool legacy = false;
    if (!file || !fgets(line, sizeof(line), file))
        return false;
    char extra = '\0';
    int header_fields = sscanf(line,
        "PITH-ALLOC-TRACE-1,%u,%64[0-9a-f]%c",
        &count, checksum, &extra);
    if (header_fields == 3 && extra == '\n') {
        legacy = true;
    } else {
        extra = '\0';
        header_fields = sscanf(line,
            "PITH-ALLOC-TRACE-2,%u,%64[0-9a-f]%c",
            &count, checksum, &extra);
    }
    if (header_fields != 3 || extra != '\n' ||
        strlen(checksum) != 64U || !count ||
        count > HEAP_PROFILE_EVENT_CAPACITY) {
        fclose(file);
        return false;
    }
    events = calloc(count, sizeof(*events));
    if (!events) {
        fclose(file);
        return false;
    }
    for (u32 index = 0; index < count; index++) {
        unsigned long long requested;
        unsigned int sequence, kind, object_id, cpu, context, phase;
        unsigned int caller_text_offset = 0xffffffffU;
        bool caller_valid = legacy;
        if (!fgets(line, sizeof(line), file)) {
            free(events);
            fclose(file);
            return false;
        }
        int row_fields;
        if (legacy) {
            row_fields = sscanf(line, "%u,%u,%u,%llu,%u,%u,%u%c",
                                &sequence, &kind, &object_id, &requested,
                                &cpu, &context, &phase, &extra);
        } else {
            unsigned long long parsed_caller_text_offset;
            row_fields = sscanf(line, "%u,%u,%u,%llu,%u,%u,%u,%llu%c",
                                &sequence, &kind, &object_id, &requested,
                                &cpu, &context, &phase,
                                &parsed_caller_text_offset, &extra);
            if (row_fields == 9 &&
                parsed_caller_text_offset <= 0xffffffffULL) {
                caller_text_offset = (unsigned int)
                    parsed_caller_text_offset;
                caller_valid = true;
            }
        }
        if (row_fields != (legacy ? 8 : 9) || !caller_valid ||
            extra != '\n' ||
            sequence != index + 1U ||
            kind < HEAP_PROFILE_EVENT_ALLOCATED ||
            kind > HEAP_PROFILE_EVENT_FAILED || cpu >= cpu_count() ||
            context >= HEAP_PROFILE_CONTEXT_COUNT || phase >= 4U ||
            requested > benchmark_heap_capacity) {
            free(events);
            fclose(file);
            return false;
        }
        events[index] = (replay_event_t){
            .sequence = sequence, .kind = kind, .object_id = object_id,
            .requested = (usize)requested, .cpu = cpu,
            .context = context, .phase = phase,
            .caller_text_offset = caller_text_offset,
        };
    }
    bool complete = !ferror(file) && fgetc(file) == EOF && fclose(file) == 0;
    if (!complete) {
        free(events);
        return false;
    }
    *events_out = events;
    *count_out = count;
    return true;
}

static void replay_cleanup(replay_object_t *objects, u32 capacity)
{
    for (u32 id = 1U; id <= capacity; id++) {
        replay_object_t *record = &objects[id];
        if (!record->live)
            continue;
        benchmark_cpu_index = record->allocation_cpu;
        benchmark_interrupts_enabled = true;
        if (use_public_api || !record->slab_owned) {
            kfree(record->pointer);
        } else {
            assert(use_cpu_slab ? slab_cpu_try_free(record->pointer) :
                                  slab_try_free(record->pointer));
        }
        record->live = false;
    }
    benchmark_interrupts_enabled = true;
    if (use_cpu_slab)
        (void)slab_cpu_drain_all();
    else if (use_slab)
        (void)slab_drain_empty();
    if (use_slab)
        assert(slab_verify());
}

static int replay_profile_events(const char *mode, const char *path)
{
    replay_event_t *events = NULL;
    u32 event_count = 0U;
    if (!read_profile_events(path, &events, &event_count))
        return 2;
    replay_object_t *objects = calloc((usize)event_count + 1U,
                                      sizeof(*objects));
    if (!objects) {
        free(events);
        return 2;
    }
    u32 allocation_count = 0U, free_count = 0U, failure_count = 0U;
    u32 cross_cpu = 0U, live = 0U, peak_live = 0U;
    u64 checksum = 0U;
    slab_class_stats_t class_before[SLAB_CLASS_COUNT] = {{0}};

    /* Validate object identity and lifecycle before timing or allocator use. */
    for (u32 index = 0; index < event_count; index++) {
        replay_event_t *event = &events[index];
        if (event->kind == HEAP_PROFILE_EVENT_ALLOCATED) {
            if (!event->requested || event->object_id != event->sequence ||
                objects[event->object_id].live) {
                free(objects); free(events); return 2;
            }
            objects[event->object_id].requested = event->requested;
            objects[event->object_id].live = true;
            allocation_count++;
        } else if (event->kind == HEAP_PROFILE_EVENT_FREED) {
            if (!event->object_id || event->object_id > event_count ||
                !objects[event->object_id].live ||
                objects[event->object_id].requested != event->requested) {
                free(objects); free(events); return 2;
            }
            objects[event->object_id].live = false;
            free_count++;
        } else {
            failure_count++;
        }
    }
    /* Failed requests are reported by the Pith profile but deliberately do
     * not enter this successful-operation timing replay. */
    if (failure_count) {
        free(objects); free(events); return 2;
    }
    memset(objects, 0, ((usize)event_count + 1U) * sizeof(*objects));

    for (u32 pass = 0; pass < 2U; pass++) {
        allocation_count = free_count = cross_cpu = live = peak_live = 0U;
        checksum = 0U;
        if (pass && slab_is_initialized()) {
            for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
                 class_index++)
                assert(slab_get_class_stats(class_index,
                                            &class_before[class_index]));
        }
        u64 started = pass ? now_ns() : 0U;
        for (u32 index = 0; index < event_count; index++) {
            replay_event_t *event = &events[index];
            benchmark_cpu_index = event->cpu;
            benchmark_interrupts_enabled = true;
            if (event->kind == HEAP_PROFILE_EVENT_ALLOCATED) {
                if (!event->object_id || event->object_id > event_count ||
                    objects[event->object_id].live)
                    abort();
                void *pointer = use_public_api ? kmalloc(event->requested) :
                    (use_slab && event->requested <= SLAB_MAX_OBJECT_SIZE
                        ? (use_cpu_slab ? slab_cpu_alloc(event->requested) :
                                          slab_alloc(event->requested))
                        : kmalloc(event->requested));
                if (!pointer)
                    abort();
                replay_object_t *record = &objects[event->object_id];
                record->pointer = pointer;
                record->requested = event->requested;
                record->allocation_cpu = event->cpu;
                record->tag = (u8)(event->object_id * 37U +
                                   event->requested + event->phase);
                record->live = true;
                record->slab_owned = !use_public_api && use_slab &&
                    event->requested <= SLAB_MAX_OBJECT_SIZE;
                u8 *bytes = pointer;
                bytes[0] = record->tag;
                bytes[event->requested - 1U] = record->tag ^ 0xa7U;
                allocation_count++;
                live++;
                if (live > peak_live) peak_live = live;
                checksum += ((u64)bytes[0] << 8U) |
                            bytes[event->requested - 1U];
            } else if (event->kind == HEAP_PROFILE_EVENT_FREED) {
                if (!event->object_id || event->object_id > event_count)
                    abort();
                replay_object_t *record = &objects[event->object_id];
                if (!record->live || record->requested != event->requested)
                    abort();
                u8 *bytes = record->pointer;
                if (bytes[0] != record->tag ||
                    bytes[event->requested - 1U] != (u8)(record->tag ^ 0xa7U))
                    abort();
                checksum += ((u64)bytes[0] << 8U) |
                            bytes[event->requested - 1U];
                if (record->allocation_cpu != event->cpu)
                    cross_cpu++;
                if (use_public_api || !record->slab_owned) {
                    kfree(record->pointer);
                } else {
                    bool freed = use_cpu_slab
                        ? slab_cpu_try_free(record->pointer)
                        : slab_try_free(record->pointer);
                    if (!freed) abort();
                }
                record->live = false;
                free_count++;
                live--;
            } else {
                abort();
            }
        }
        u64 elapsed = pass ? now_ns() - started : 0U;
        u32 survivors = 0U;
        for (u32 id = 1U; id <= event_count; id++) {
            replay_object_t *record = &objects[id];
            if (!record->live)
                continue;
            u8 *bytes = record->pointer;
            if (bytes[0] != record->tag ||
                bytes[record->requested - 1U] != (u8)(record->tag ^ 0xa7U))
                abort();
            survivors++;
        }
        if (pass) {
            printf("event_replay,%s,%u,%u,%u,%u,%u,%u,%llu,%llu\n",
                   mode, event_count, allocation_count, free_count,
                   cross_cpu, peak_live, survivors,
                   (unsigned long long)elapsed,
                   (unsigned long long)checksum);
            printf("event_heap,%s,%llu,%llu,%llu,%llu\n", mode,
                   (unsigned long long)heap_get_used_size(),
                   (unsigned long long)heap_get_total_size(),
                   (unsigned long long)((uintptr_t)kernel_heap.end -
                                        benchmark_heap_base),
                   (unsigned long long)heap_get_free_size());
            if (slab_is_initialized()) {
                for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
                     class_index++) {
                    slab_class_stats_t stats;
                    assert(slab_get_class_stats(class_index, &stats));
                    printf("event_class,%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                        mode, class_index,
                        (unsigned long long)(stats.allocations -
                            class_before[class_index].allocations),
                        (unsigned long long)(stats.frees -
                            class_before[class_index].frees),
                        (unsigned long long)(stats.backing_allocations -
                            class_before[class_index].backing_allocations),
                        (unsigned long long)(stats.backing_releases -
                            class_before[class_index].backing_releases),
                        (unsigned long long)(stats.cache_hits -
                            class_before[class_index].cache_hits),
                        (unsigned long long)(stats.cache_misses -
                            class_before[class_index].cache_misses),
                        (unsigned long long)(stats.cache_refills -
                            class_before[class_index].cache_refills),
                        (unsigned long long)(stats.cache_refill_objects -
                            class_before[class_index].cache_refill_objects),
                        (unsigned long long)(stats.cache_overflows -
                            class_before[class_index].cache_overflows),
                        (unsigned long long)stats.live_slots,
                        (unsigned long long)stats.cached_slots,
                        (unsigned long long)(stats.partial_slabs +
                            stats.full_slabs + stats.empty_slabs));
                }
                slab_classifier_stats_t classifier;
                assert(slab_get_classifier_stats(&classifier));
                printf("event_classifier,%s,%llu,%llu,%llu\n", mode,
                    (unsigned long long)classifier.retained_payload_bytes,
                    (unsigned long long)classifier.peak_payload_bytes,
                    (unsigned long long)slab_cpu_cache_metadata_bytes());
            }
        }
        replay_cleanup(objects, event_count);
        if (pass && slab_is_initialized()) {
            u64 backing = 0U;
            for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
                 class_index++) {
                slab_class_stats_t stats;
                assert(slab_get_class_stats(class_index, &stats));
                backing += (stats.partial_slabs + stats.full_slabs +
                            stats.empty_slabs) * PAGE_SIZE;
            }
            slab_classifier_stats_t classifier;
            assert(slab_get_classifier_stats(&classifier));
            printf("event_after_drain,%s,%llu,%llu,%llu,%llu,%llu\n", mode,
                (unsigned long long)heap_get_used_size(),
                (unsigned long long)heap_get_total_size(),
                (unsigned long long)((uintptr_t)kernel_heap.end -
                                     benchmark_heap_base),
                (unsigned long long)backing,
                (unsigned long long)classifier.retained_payload_bytes);
        }
        memset(objects, 0, ((usize)event_count + 1U) * sizeof(*objects));
    }
    free(objects);
    free(events);
    return 0;
}

static int run_public_api_check(void)
{
    static const usize requests[SLAB_CLASS_COUNT] = {
        16U, 32U, 48U, 64U, 80U, 96U, 112U, 128U, 144U, 160U,
        176U, 192U, 208U, 224U, 240U, 256U, 288U, 320U, 352U, 384U
    };
    void *before_activation = kmalloc(73U);
    void *objects[SLAB_CLASS_COUNT];
    assert(before_activation && !slab_is_initialized());
#ifdef KERNEL_HYBRID_ALLOCATOR
    assert(slab_init((void *)benchmark_slab_base, benchmark_slab_capacity));
#endif
    for (u32 index = 0U; index < SLAB_CLASS_COUNT; index++) {
        objects[index] = kmalloc(requests[index]);
        assert(objects[index] && ((uintptr_t)objects[index] & 15U) == 0U);
        memset(objects[index], (int)(index + 1U), requests[index]);
    }
#ifdef KERNEL_HYBRID_ALLOCATOR
    for (u32 index = 0U; index < SLAB_CLASS_COUNT; index++) {
        slab_class_stats_t stats;
        u64 expected_live = requests[index] <=
            KERNEL_HYBRID_ALLOCATOR_CUTOFF ? 1U : 0U;
        assert(slab_get_class_stats(index, &stats) &&
               stats.live_slots == expected_live);
    }
#endif
    void *large = kmalloc(385U);
    assert(large && ((uintptr_t)large & 15U) == 0U);
    assert(!kmalloc(0U) && !kmalloc((usize)-1));
    for (u32 index = 0U; index < SLAB_CLASS_COUNT; index++) {
        const u8 *bytes = objects[index];
        for (usize offset = 0U; offset < requests[index]; offset++)
            assert(bytes[offset] == (u8)(index + 1U));
        kfree(objects[index]);
    }
    kfree(large);
    /* This object was allocated before classification was initialized. */
    kfree(before_activation);
#ifdef KERNEL_HYBRID_ALLOCATOR
    (void)slab_cpu_drain_all();
    assert(slab_verify());
    for (u32 index = 0U; index < SLAB_CLASS_COUNT; index++) {
        slab_class_stats_t stats;
        assert(slab_get_class_stats(index, &stats) &&
               !stats.live_slots && !stats.cached_slots);
    }
#else
    assert(heap_get_used_size() == 0U);
#endif
    puts("public_allocator_api_check,PASS");
    return 0;
}

static int run_public_slab_failure_fallback_check(void)
{
#ifdef KERNEL_HYBRID_ALLOCATOR
    slab_class_stats_t before, after_failure, after_slab;
    assert(slab_is_initialized());
    assert(slab_get_class_stats(0U, &before));
    u64 heap_used_before = heap_get_used_size();

    /* With no frame supply, a clean small-object slab miss must use existing
     * indexed-heap capacity. It must not publish slab ownership. */
    atomic_store_explicit(&benchmark_pmm_exhausted, true,
                          memory_order_release);
    void *fallback = kmalloc(16U);
    assert(fallback);
    memset(fallback, 0x53, 16U);
    assert(slab_get_class_stats(0U, &after_failure));
    assert(after_failure.live_slots == 0U &&
           after_failure.allocation_failures > before.allocation_failures &&
           heap_get_used_size() > heap_used_before);
    assert(((const u8 *)fallback)[0] == 0x53 &&
           ((const u8 *)fallback)[15] == 0x53);
    assert(heap_verify("slab failure fallback"));

    /* Restoring PMM supply enables the slab path again. Free dispatch must
     * still return the earlier heap-owned object to its original backend. */
    atomic_store_explicit(&benchmark_pmm_exhausted, false,
                          memory_order_release);
    kfree(fallback);
    assert(heap_get_used_size() == heap_used_before);
    void *slab_object = kmalloc(16U);
    assert(slab_object && slab_get_class_stats(0U, &after_slab) &&
           after_slab.live_slots == 1U);
    /* Sparse classifier metadata is retained after its first page is used;
     * compare against that post-initialization heap watermark. */
    u64 heap_used_after_classifier = heap_get_used_size();
    memset(slab_object, 0xa4, 16U);
    kfree(slab_object);
    (void)slab_cpu_drain_all();
    assert(slab_verify() && heap_verify("slab fallback recovery") &&
           heap_used_after_classifier >= heap_used_before &&
           heap_get_used_size() == heap_used_after_classifier);
    puts("public_slab_failure_fallback,PASS");
    return 0;
#else
    return 77;
#endif
}

static int run_public_pressure_reclaim_check(void)
{
#ifdef KERNEL_HYBRID_ALLOCATOR
    usize maximum_objects = benchmark_heap_capacity /
                            (sizeof(heap_block_t) + 16U) + 1U;
    void **held = calloc(maximum_objects, sizeof(*held));
    slab_class_stats_t before, after_invalid, after_reclaim;
    assert(held && slab_is_initialized());

    void *cached = kmalloc(16U);
    assert(cached);
    kfree(cached);
    assert(slab_get_class_stats(0U, &before) && before.cached_slots);

    /* Invalid requests must not trigger pressure work or mutate cache state. */
    assert(!kmalloc(0U) && !kmalloc((usize)-1));
    assert(slab_get_class_stats(0U, &after_invalid));
    assert(after_invalid.cached_slots == before.cached_slots &&
           after_invalid.backing_releases == before.backing_releases);

    /* Exhaust every indexed-heap block that can satisfy the future request.
     * These raw allocations bypass hybrid routing and are independently held
     * by the host fixture until the pressure/recovery assertion completes. */
    usize held_count = 0U;
    while (held_count < maximum_objects) {
        void *object = heap_alloc_raw(2048U);
        if (!object)
            break;
        held[held_count++] = object;
    }
    assert(held_count && !heap_alloc_raw(2048U));

    void *recovered = kmalloc(2048U);
    assert(recovered);
    assert(slab_get_class_stats(0U, &after_reclaim));
    assert(after_reclaim.cached_slots == 0U &&
           after_reclaim.backing_releases > before.backing_releases);
    printf("public_pressure_recovery,PASS,%llu,%llu,%llu,%llu\n",
           (unsigned long long)held_count,
           (unsigned long long)before.cached_slots,
           (unsigned long long)after_reclaim.backing_releases -
               before.backing_releases,
           (unsigned long long)heap_get_free_size());

    kfree(recovered);
    for (usize index = 0U; index < held_count; index++)
        heap_free_raw(held[index]);
    free(held);
    (void)slab_cpu_drain_all();
    assert(slab_verify());
    return 0;
#else
    return 77;
#endif
}

int main(int argc, char **argv)
{
    static const usize requests[] = {16U, 72U, 192U, 384U};
    const u32 samples = 128U;
    const u32 pairs_per_sample = 1000U;
    enum { CONCURRENT_PAIRS = 20000 };
    void *arena = NULL;
    void *slab_arena = NULL;

    bool profile_replay = argc == 3 &&
                          !strncmp(argv[1], "profile-", 8U);
    bool event_replay = argc == 3 && !strncmp(argv[1], "event-", 6U);
    assert((argc == 2 || profile_replay || event_replay) &&
           (!strcmp(argv[1], "heap") ||
                         !strcmp(argv[1], "public-api") ||
                         !strcmp(argv[1], "api-check") ||
                         !strcmp(argv[1], "api-fallback") ||
                         !strcmp(argv[1], "api-invalid") ||
                         !strcmp(argv[1], "api-double-free") ||
                         !strcmp(argv[1], "api-interior-free") ||
                         !strcmp(argv[1], "pressure-reclaim") ||
                         !strcmp(argv[1], "slab") ||
                         !strcmp(argv[1], "cpu-slab") ||
                         !strcmp(argv[1], "classifier") ||
                         !strcmp(argv[1], "bitmap") ||
                         !strcmp(argv[1], "phases-heap") ||
                         !strcmp(argv[1], "phases-slab") ||
                         !strcmp(argv[1], "phases-cpu-slab") ||
                         !strcmp(argv[1], "lifecycle") ||
                         !strcmp(argv[1], "population-heap") ||
                         !strcmp(argv[1], "population-slab") ||
                         !strcmp(argv[1], "population-cpu-slab") ||
                         !strcmp(argv[1], "population-public-api") ||
                         !strcmp(argv[1], "event-heap") ||
                         !strcmp(argv[1], "event-slab") ||
                         !strcmp(argv[1], "event-cpu-slab") ||
                         !strcmp(argv[1], "event-public-api") ||
                         !strcmp(argv[1], "profile-heap") ||
                         !strcmp(argv[1], "profile-slab") ||
                         !strcmp(argv[1], "profile-cpu-slab") ||
                         !strcmp(argv[1], "profile-public-api")));
    if (profile_replay)
        assert(!strcmp(argv[1], "profile-heap") ||
               !strcmp(argv[1], "profile-slab") ||
               !strcmp(argv[1], "profile-cpu-slab") ||
               !strcmp(argv[1], "profile-public-api"));
    if (event_replay)
        assert(!strcmp(argv[1], "event-heap") ||
               !strcmp(argv[1], "event-slab") ||
               !strcmp(argv[1], "event-cpu-slab") ||
               !strcmp(argv[1], "event-public-api"));
    use_public_api = !strcmp(argv[1], "public-api") ||
                     !strcmp(argv[1], "api-check") ||
                     !strcmp(argv[1], "api-fallback") ||
                     !strcmp(argv[1], "api-invalid") ||
                     !strcmp(argv[1], "api-double-free") ||
                     !strcmp(argv[1], "api-interior-free") ||
                     !strcmp(argv[1], "pressure-reclaim") ||
                     !strcmp(argv[1], "population-public-api") ||
                     !strcmp(argv[1], "profile-public-api") ||
                     !strcmp(argv[1], "event-public-api");
    if (profile_replay) {
        use_public_api = !strcmp(argv[1], "profile-public-api");
        use_slab = !strcmp(argv[1], "profile-slab") ||
                   !strcmp(argv[1], "profile-cpu-slab");
        use_cpu_slab = !strcmp(argv[1], "profile-cpu-slab");
    }
    use_slab = !strcmp(argv[1], "slab") ||
               !strcmp(argv[1], "cpu-slab") ||
               !strcmp(argv[1], "classifier") ||
               !strcmp(argv[1], "bitmap") ||
               !strcmp(argv[1], "phases-slab") ||
               !strcmp(argv[1], "phases-cpu-slab") ||
               !strcmp(argv[1], "lifecycle") ||
               !strcmp(argv[1], "population-slab") ||
               !strcmp(argv[1], "population-cpu-slab") ||
               !strcmp(argv[1], "population-public-api") ||
               !strcmp(argv[1], "event-slab") ||
               !strcmp(argv[1], "event-cpu-slab") ||
               !strcmp(argv[1], "event-public-api") ||
               (profile_replay && use_slab);
    use_cpu_slab = !strcmp(argv[1], "cpu-slab");
    use_cpu_slab = use_cpu_slab ||
                   !strcmp(argv[1], "phases-cpu-slab") ||
                   !strcmp(argv[1], "population-cpu-slab") ||
                   !strcmp(argv[1], "population-public-api") ||
                   !strcmp(argv[1], "event-cpu-slab") ||
                   !strcmp(argv[1], "event-public-api") ||
                   (profile_replay &&
                    !strcmp(argv[1], "profile-cpu-slab"));
#ifdef KERNEL_HYBRID_ALLOCATOR
    if (use_public_api && strcmp(argv[1], "api-check") != 0)
        use_cpu_slab = true;
#endif
    assert(posix_memalign(&arena, PAGE_SIZE, benchmark_heap_capacity) == 0);
    assert(posix_memalign(&slab_arena, PAGE_SIZE, benchmark_slab_capacity) == 0);
    memset(arena, 0, benchmark_heap_capacity);
    memset(slab_arena, 0, benchmark_slab_capacity);
    benchmark_heap_base = (uintptr_t)arena;
    benchmark_slab_base = (uintptr_t)slab_arena;
    assert(heap_init());
    if (use_slab && !slab_is_initialized())
        assert(slab_init(slab_arena, benchmark_slab_capacity));
#ifdef KERNEL_HYBRID_ALLOCATOR
    if (!slab_is_initialized() && strcmp(argv[1], "api-check") != 0)
        assert(slab_init(slab_arena, benchmark_slab_capacity));
#endif
    if (use_cpu_slab)
        printf("cache_configuration,%u,%u,%zu,%u,%llu\n",
               SLAB_LOCAL_CACHE_CAPACITY, SLAB_LOCAL_REFILL_BATCH,
               sizeof(slab_cpu_cache_t), slab_cpu_cache_count,
               (unsigned long long)slab_cpu_cache_metadata_bytes());
    if (!strcmp(argv[1], "api-check"))
        return run_public_api_check();
    if (!strcmp(argv[1], "api-fallback"))
        return run_public_slab_failure_fallback_check();
    if (!strcmp(argv[1], "pressure-reclaim"))
        return run_public_pressure_reclaim_check();
    if (!strcmp(argv[1], "api-invalid"))
        kfree((void *)(benchmark_heap_base - 16U));
    if (!strcmp(argv[1], "api-double-free")) {
        void *object = kmalloc(72U);
        assert(object);
        kfree(object);
        kfree(object);
    }
    if (!strcmp(argv[1], "api-interior-free")) {
        void *object = kmalloc(72U);
        assert(object);
        kfree((u8 *)object + 1U);
    }
    if (!strcmp(argv[1], "api-invalid") ||
        !strcmp(argv[1], "api-double-free") ||
        !strcmp(argv[1], "api-interior-free"))
        abort();
    if (!strcmp(argv[1], "classifier"))
        return run_classifier_comparison();
    if (!strcmp(argv[1], "bitmap"))
        return run_bitmap_search_benchmark();
    if (!strcmp(argv[1], "phases-heap"))
        return run_separated_operation_benchmark(false, false);
    if (!strcmp(argv[1], "phases-slab"))
        return run_separated_operation_benchmark(true, false);
    if (!strcmp(argv[1], "phases-cpu-slab"))
        return run_separated_operation_benchmark(true, true);
    if (!strcmp(argv[1], "lifecycle"))
        return run_lifecycle_benchmark();
    if (!strcmp(argv[1], "population-heap"))
        return run_population_benchmark("heap", false, false);
    if (!strcmp(argv[1], "population-slab"))
        return run_population_benchmark("slab", true, false);
    if (!strcmp(argv[1], "population-cpu-slab"))
        return run_population_benchmark("cpu-slab", true, true);
    if (!strcmp(argv[1], "population-public-api"))
        return run_population_benchmark("public-api", true, true);
    if (event_replay)
        return replay_profile_events(argv[1], argv[2]);
    if (profile_replay)
        return run_profile_size_replay(argv[1], argv[2]);

    for (u32 request_index = 0; request_index <
         sizeof(requests) / sizeof(requests[0]); request_index++) {
        usize request = requests[request_index];
        const u32 warmup_pairs = 64U;
        void *anchor = allocate(request);
        assert(anchor);
        for (u32 pair = 0U; pair < warmup_pairs; pair++) {
            void *object = allocate(request);
            assert(object && object != anchor);
            benchmark_touch_payload(object, request, 0U, pair);
            release(object);
        }
        printf("warmup,%s,%llu,%u\n", selected_mode_name(),
               (unsigned long long)request, warmup_pairs);
        for (u32 sample = 0; sample < samples; sample++) {
            u64 start = now_ns();
            for (u32 pair = 0; pair < pairs_per_sample; pair++) {
                void *object = allocate(request);
                assert(object && object != anchor);
                benchmark_touch_payload(object, request, 0U, pair);
                release(object);
            }
            printf("sample,%s,%llu,%u,%u,%llu\n", selected_mode_name(),
                   (unsigned long long)request, sample, pairs_per_sample,
                   (unsigned long long)(now_ns() - start));
        }
        if (use_slab) {
            u32 class_index = 0;
            while (class_index < SLAB_CLASS_COUNT &&
                   slab_class_size(class_index) < request)
                class_index++;
            slab_class_stats_t stats;
            assert(slab_get_class_stats(class_index, &stats));
            printf("class,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                   (unsigned long long)request,
                   slab_class_size(class_index),
                   slab_class_slots(class_index),
                   (unsigned long long)stats.bitmap_words_examined,
                   (unsigned long long)stats.lock_acquisitions,
                   (unsigned long long)stats.backing_allocations,
                   (unsigned long long)stats.partial_slabs,
                   (unsigned long long)stats.live_slots,
                   (unsigned long long)stats.peak_live_slots,
                   (unsigned long long)stats.full_slabs);
            printf("cache,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu\n",
                   (unsigned long long)request,
                   (unsigned long long)stats.cached_slots,
                   (unsigned long long)stats.cache_hits,
                   (unsigned long long)stats.cache_misses,
                   (unsigned long long)stats.cache_refills,
                   (unsigned long long)stats.cache_refill_conflicts,
                   (unsigned long long)stats.cache_refill_objects,
                   (unsigned long long)stats.cache_overflows,
                   cache_backing_pages_for_class(class_index),
                   (unsigned long long)heap_get_used_size());
        }
        release(anchor);
    }

    if (!benchmark_select_worker_cpus()) {
        puts("concurrent_unavailable,4");
    } else {
        for (u32 worker = 0; worker < BENCHMARK_WORKERS; worker++)
            printf("host_cpu,%u,%d\n", worker, benchmark_worker_cpus[worker]);
        for (u32 request_index = 0; request_index < 3U; request_index++) {
            usize request = request_index < 2U ? requests[request_index] : 0U;
            usize anchor_request = request ? request : 16U;
            void *anchor = allocate(anchor_request);
            pthread_t threads[BENCHMARK_WORKERS];
            worker_args_t args[BENCHMARK_WORKERS];
            pthread_barrier_t start_barrier, witness_done_barrier;
            slab_class_stats_t before[SLAB_CLASS_COUNT] = {{0}};
            slab_class_stats_t after[SLAB_CLASS_COUNT] = {{0}};
            u32 distinct_cpus = 0U;
            u32 witnessed_pairs = 0U;
            assert(anchor);
            if (use_slab) {
                for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
                     class_index++)
                    assert(slab_get_class_stats(class_index,
                                                &before[class_index]));
            }
            atomic_store_explicit(&benchmark_witness_active, 0U,
                                  memory_order_relaxed);
            atomic_store_explicit(&benchmark_witness_peak, 0U,
                                  memory_order_relaxed);
            atomic_store_explicit(&benchmark_timed_workers_done, 0U,
                                  memory_order_relaxed);
            atomic_store_explicit(&benchmark_timed_start, false,
                                  memory_order_relaxed);
            assert(pthread_barrier_init(&start_barrier, NULL,
                       BENCHMARK_WORKERS + 1U) == 0);
            assert(pthread_barrier_init(&witness_done_barrier, NULL,
                       BENCHMARK_WORKERS + 1U) == 0);
            for (u32 thread = 0; thread < BENCHMARK_WORKERS; thread++) {
                args[thread] = (worker_args_t){
                    .request = request,
                    .pairs = CONCURRENT_PAIRS,
                    .start_barrier = &start_barrier,
                    .witness_done_barrier = &witness_done_barrier,
                    .cpu_index = thread,
                    .host_cpu = benchmark_worker_cpus[thread],
                };
                assert(pthread_create(&threads[thread], NULL, worker,
                                      &args[thread]) == 0);
            }
            int result = pthread_barrier_wait(&start_barrier);
            assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
            result = pthread_barrier_wait(&witness_done_barrier);
            assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
            u32 overlap = atomic_load_explicit(&benchmark_witness_peak,
                                                memory_order_relaxed);
            for (u32 thread = 0; thread < BENCHMARK_WORKERS; thread++) {
                assert(args[thread].cpu_at_start == args[thread].host_cpu &&
                       args[thread].witness_pairs == WITNESS_PAIRS &&
                       args[thread].checksum != 0U);
                witnessed_pairs += (u32)args[thread].witness_pairs;
                for (u32 prior = 0; prior < thread; prior++)
                    assert(args[thread].cpu_at_start !=
                           args[prior].cpu_at_start);
                distinct_cpus++;
            }
            printf("concurrent_witness,%llu,%u,%u,%u,%u\n",
                   (unsigned long long)request, BENCHMARK_WORKERS, overlap,
                   distinct_cpus, witnessed_pairs);

            u64 started = now_ns();
            atomic_store_explicit(&benchmark_timed_start, true,
                                  memory_order_release);
            while (atomic_load_explicit(&benchmark_timed_workers_done,
                                        memory_order_acquire) <
                   BENCHMARK_WORKERS)
                sched_yield();
            u64 elapsed = now_ns() - started;
            for (u32 thread = 0; thread < BENCHMARK_WORKERS; thread++) {
                assert(pthread_join(threads[thread], NULL) == 0);
                assert(args[thread].cpu_at_end == args[thread].host_cpu &&
                       args[thread].timed_pairs == CONCURRENT_PAIRS);
                printf("concurrent_worker,%llu,%u,%u,%d,%d,%d,%llu,%llu,%llu\n",
                       (unsigned long long)request, thread,
                       args[thread].cpu_index, args[thread].host_cpu,
                       args[thread].cpu_at_start, args[thread].cpu_at_end,
                       (unsigned long long)args[thread].witness_pairs,
                       (unsigned long long)args[thread].timed_pairs,
                       (unsigned long long)args[thread].checksum);
            }
            printf("concurrent,%s,%llu,%u,%u,%llu\n",
                   selected_mode_name(),
                   (unsigned long long)request, BENCHMARK_WORKERS,
                   CONCURRENT_PAIRS, (unsigned long long)elapsed);
            assert(pthread_barrier_destroy(&start_barrier) == 0);
            assert(pthread_barrier_destroy(&witness_done_barrier) == 0);
            if (use_slab) {
                u64 lock_delta = 0U, words_delta = 0U, backing_delta = 0U;
                u64 live = 0U, peak_live = 0U, partial = 0U;
                u64 cached = 0U, hits = 0U, misses = 0U, refills = 0U;
                u64 refill_conflicts = 0U, refill_objects = 0U;
                u64 overflows = 0U, retained_pages = 0U;
                for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
                     class_index++) {
                    assert(slab_get_class_stats(class_index,
                                                &after[class_index]));
                    lock_delta += after[class_index].lock_acquisitions -
                                  before[class_index].lock_acquisitions;
                    words_delta += after[class_index].bitmap_words_examined -
                                   before[class_index].bitmap_words_examined;
                    backing_delta += after[class_index].backing_allocations -
                                     before[class_index].backing_allocations;
                    live += after[class_index].live_slots;
                    peak_live += after[class_index].peak_live_slots;
                    partial += after[class_index].partial_slabs;
                    cached += after[class_index].cached_slots;
                    hits += after[class_index].cache_hits;
                    misses += after[class_index].cache_misses;
                    refills += after[class_index].cache_refills;
                    refill_conflicts +=
                        after[class_index].cache_refill_conflicts;
                    refill_objects += after[class_index].cache_refill_objects;
                    overflows += after[class_index].cache_overflows;
                    retained_pages +=
                        cache_backing_pages_for_class(class_index);
                }
                printf("concurrent_class,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                       (unsigned long long)request,
                       (unsigned long long)lock_delta,
                       (unsigned long long)words_delta,
                       (unsigned long long)backing_delta,
                       (unsigned long long)live,
                       (unsigned long long)peak_live,
                       (unsigned long long)partial);
                printf("concurrent_cache,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                       (unsigned long long)request,
                       (unsigned long long)cached,
                       (unsigned long long)hits,
                       (unsigned long long)misses,
                       (unsigned long long)refills,
                       (unsigned long long)refill_conflicts,
                       (unsigned long long)refill_objects,
                       (unsigned long long)overflows,
                       (unsigned long long)retained_pages,
                       (unsigned long long)heap_get_used_size());
            }
            release(anchor);
        }
    }
    if (use_public_api && slab_is_initialized()) {
        for (u32 class_index = 0U; class_index < SLAB_CLASS_COUNT;
             class_index++) {
            slab_class_stats_t stats;
            assert(slab_get_class_stats(class_index, &stats));
            printf("public_class,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                   class_index,
                   (unsigned long long)stats.allocations,
                   (unsigned long long)stats.frees,
                   (unsigned long long)stats.cache_hits,
                   (unsigned long long)stats.cache_misses,
                   (unsigned long long)stats.cache_refills,
                   (unsigned long long)stats.cache_overflows,
                   (unsigned long long)stats.cached_slots,
                   (unsigned long long)stats.backing_allocations,
                   (unsigned long long)stats.backing_releases,
                   (unsigned long long)stats.live_slots);
        }
        slab_classifier_stats_t classifier;
        assert(slab_get_classifier_stats(&classifier));
        printf("public_resources,%llu,%llu,%llu\n",
               (unsigned long long)heap_get_used_size(),
               (unsigned long long)classifier.retained_payload_bytes,
               (unsigned long long)((uintptr_t)kernel_heap.end -
                                    benchmark_heap_base));
    }
    if (use_cpu_slab)
        (void)slab_cpu_drain_all();
    if (use_slab || slab_is_initialized()) {
        slab_classifier_stats_t classifier;
        assert(slab_get_classifier_stats(&classifier));
        /* Empty slabs retire their backing pages, while the published sparse
         * classifier nodes intentionally remain stable for module lifetime. */
        assert(heap_get_used_size() >= classifier.retained_payload_bytes);
        for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
             class_index++) {
            slab_class_stats_t class_stats;
            assert(slab_get_class_stats(class_index, &class_stats));
            assert(class_stats.live_slots == 0U &&
                   class_stats.partial_slabs == 0U &&
                   class_stats.full_slabs == 0U &&
                   class_stats.backing_allocations ==
                       class_stats.backing_releases);
        }
        assert(slab_verify());
    } else {
        assert(heap_get_used_size() == 0U);
    }
    printf("heap_memory,%s,%llu,%llu\n", argv[1],
           (unsigned long long)heap_get_used_size(),
           (unsigned long long)heap_get_total_size());
    free(arena);
    free(slab_arena);
    return 0;
}
