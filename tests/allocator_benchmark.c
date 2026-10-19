/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include <slab.h>
#include <spinlock.h>
#include <mutex.h>
#include <pmm.h>
#include <vmm.h>

static uintptr_t benchmark_heap_base;
static usize benchmark_heap_capacity = 8U * 1024U * 1024U;
/* Benchmark-only compact arena for the historical one-byte lookup. The
 * production dense classifier was 128 MiB for the full kernel reservation. */
static u8 dense_classifier_reference[8U * 1024U * 1024U / PAGE_SIZE];

#undef HEAP_START
#undef HEAP_LIMIT
#define HEAP_START benchmark_heap_base
#define HEAP_LIMIT (benchmark_heap_base + benchmark_heap_capacity)
#define HEAP_VERIFY 0
#define SLAB_CORE_DIAGNOSTICS 1

/* Both production implementations use the same arena and host adapters. */
#include "../kernel/src/heap.c"
#include "../kernel/src/slab.c"

#define BENCH_CLASSIFIER_ROOT_ENTRIES \
    (((usize)(KERNEL_HEAP_LIMIT - KERNEL_HEAP_BASE) / PAGE_SIZE + \
      SLAB_CLASSIFIER_ROOT_PAGES - 1U) / SLAB_CLASSIFIER_ROOT_PAGES)

/* Host-only experiment for replacing the allocated root node with a compact
 * static directory.  This is not linked into Pith. */
static _Atomic uintptr_t benchmark_classifier_root[
    BENCH_CLASSIFIER_ROOT_ENTRIES];

static page_table_t benchmark_pml4;
static _Atomic u64 next_frame = PAGE_SIZE;
static _Thread_local bool benchmark_interrupts_enabled = true;
static _Thread_local u32 benchmark_spin_depth;

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

void mutex_init(mutex_t *mutex) { (void)mutex; }
bool mutex_lock(mutex_t *mutex) { (void)mutex; return true; }
bool mutex_unlock(mutex_t *mutex) { (void)mutex; return true; }
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
phys_addr_t pmm_alloc_frame(void)
{
    return atomic_fetch_add_explicit(&next_frame, PAGE_SIZE,
                                     memory_order_relaxed);
}
void pmm_free_frame(phys_addr_t frame) { assert(frame); }
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
    usize heap_page_count = benchmark_heap_capacity / PAGE_SIZE;
    bool *seen = calloc(heap_page_count, sizeof(*seen));
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
        usize page_index = ((uintptr_t)object - benchmark_heap_base) /
                           PAGE_SIZE;
        assert(page_index < heap_page_count);
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
static void *allocate(usize request);
static void release(void *object);

static int run_separated_operation_benchmark(bool slab_path)
{
    enum { SAMPLES = 64, MAX_BATCH = 16 };
    static const usize requests[] = {16U, 72U, 384U};
    use_slab = slab_path;

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
                   slab_path ? "slab" : "heap",
                   (unsigned long long)request, sample,
                   (unsigned long long)batch,
                   (unsigned long long)allocation_elapsed);

            started = now_ns();
            for (usize index = 0; index < batch; index++)
                release(batch_objects[index]);
            u64 free_elapsed = now_ns() - started;
            printf("phase,%s,free,%llu,%u,%llu,%llu\n",
                   slab_path ? "slab" : "heap",
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
    static const usize requests[] = {16U, 72U, 384U};
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

static void *allocate(usize request)
{
    return use_slab ? slab_alloc(request) : kmalloc(request);
}

static void release(void *object)
{
    if (use_slab) {
        assert(slab_try_free(object));
    } else {
        kfree(object);
    }
}

typedef struct {
    usize request;
    u32 pairs;
    pthread_barrier_t *barrier;
} worker_args_t;

static void *worker(void *argument)
{
    worker_args_t *args = argument;
    int result = pthread_barrier_wait(args->barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (u32 pair = 0; pair < args->pairs; pair++) {
        void *object = allocate(args->request);
        assert(object);
        release(object);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    static const usize requests[] = {16U, 72U, 384U};
    const u32 samples = 128U;
    const u32 pairs_per_sample = 1000U;
    enum { WORKERS = 4, CONCURRENT_PAIRS = 20000 };
    void *arena = NULL;

    assert(argc == 2 && (!strcmp(argv[1], "heap") ||
                         !strcmp(argv[1], "slab") ||
                         !strcmp(argv[1], "classifier") ||
                         !strcmp(argv[1], "bitmap") ||
                         !strcmp(argv[1], "phases-heap") ||
                         !strcmp(argv[1], "phases-slab") ||
                         !strcmp(argv[1], "lifecycle")));
    use_slab = !strcmp(argv[1], "slab") ||
               !strcmp(argv[1], "classifier") ||
               !strcmp(argv[1], "bitmap") ||
               !strcmp(argv[1], "phases-slab") ||
               !strcmp(argv[1], "lifecycle");
    assert(posix_memalign(&arena, PAGE_SIZE, benchmark_heap_capacity) == 0);
    memset(arena, 0, benchmark_heap_capacity);
    benchmark_heap_base = (uintptr_t)arena;
    assert(heap_init());
    if (use_slab)
        assert(slab_init(arena, benchmark_heap_capacity));
    if (!strcmp(argv[1], "classifier"))
        return run_classifier_comparison();
    if (!strcmp(argv[1], "bitmap"))
        return run_bitmap_search_benchmark();
    if (!strcmp(argv[1], "phases-heap"))
        return run_separated_operation_benchmark(false);
    if (!strcmp(argv[1], "phases-slab"))
        return run_separated_operation_benchmark(true);
    if (!strcmp(argv[1], "lifecycle"))
        return run_lifecycle_benchmark();

    for (u32 request_index = 0; request_index <
         sizeof(requests) / sizeof(requests[0]); request_index++) {
        usize request = requests[request_index];
        void *anchor = allocate(request);
        assert(anchor);
        for (u32 sample = 0; sample < samples; sample++) {
            u64 start = now_ns();
            for (u32 pair = 0; pair < pairs_per_sample; pair++) {
                void *object = allocate(request);
                assert(object && object != anchor);
                release(object);
            }
            printf("sample,%s,%llu,%u,%u,%llu\n",
                   use_slab ? "slab" : "heap",
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
        }
        release(anchor);
    }

    for (u32 request_index = 0; request_index < 2U; request_index++) {
        usize request = requests[request_index];
        void *anchor = allocate(request);
        pthread_t threads[WORKERS];
        worker_args_t args[WORKERS];
        pthread_barrier_t barrier;
        u32 class_index = request == 16U ? 0U : 4U;
        slab_class_stats_t before, after;
        assert(anchor);
        if (use_slab)
            assert(slab_get_class_stats(class_index, &before));
        assert(pthread_barrier_init(&barrier, NULL, WORKERS + 1U) == 0);
        for (u32 thread = 0; thread < WORKERS; thread++) {
            args[thread] = (worker_args_t){request, CONCURRENT_PAIRS, &barrier};
            assert(pthread_create(&threads[thread], NULL, worker,
                                  &args[thread]) == 0);
        }
        u64 start = now_ns();
        int result = pthread_barrier_wait(&barrier);
        assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
        for (u32 thread = 0; thread < WORKERS; thread++)
            assert(pthread_join(threads[thread], NULL) == 0);
        printf("concurrent,%s,%llu,%u,%u,%llu\n",
               use_slab ? "slab" : "heap",
               (unsigned long long)request, WORKERS, CONCURRENT_PAIRS,
               (unsigned long long)(now_ns() - start));
        assert(pthread_barrier_destroy(&barrier) == 0);
        if (use_slab) {
            assert(slab_get_class_stats(class_index, &after));
            printf("concurrent_class,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                   (unsigned long long)request,
                   (unsigned long long)(after.lock_acquisitions -
                                        before.lock_acquisitions),
                   (unsigned long long)(after.bitmap_words_examined -
                                        before.bitmap_words_examined),
                   (unsigned long long)(after.backing_allocations -
                                        before.backing_allocations),
                   (unsigned long long)after.live_slots,
                   (unsigned long long)after.peak_live_slots,
                   (unsigned long long)after.partial_slabs);
        }
        release(anchor);
    }
    if (use_slab) {
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
    free(arena);
    return 0;
}
