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
    enum { LOOKUP_SAMPLES = 128, LOOKUPS_PER_SAMPLE = 1000000 };
    void *object = NULL;
    slab_classifier_stats_t stats;
    u64 checksum = 0;
    u64 started = now_ns();

    assert(slab_classifier_ensure(0U));
    printf("classifier_growth,%llu\n",
           (unsigned long long)(now_ns() - started));
    started = now_ns();
    object = slab_alloc(72U);
    assert(object);
    printf("classifier_first_slab,%llu\n",
           (unsigned long long)(now_ns() - started));
    usize page_index = ((uintptr_t)object - benchmark_heap_base) / PAGE_SIZE;
    u64 token = slab_page_token_load(page_index);
    assert(slab_token_class(token) != 0U);
    dense_classifier_reference[page_index] = slab_token_class(token);

    for (u32 sample = 0; sample < LOOKUP_SAMPLES; sample++) {
        u64 start = now_ns();
        for (u32 lookup = 0; lookup < LOOKUPS_PER_SAMPLE; lookup++)
            checksum += slab_page_token_load(page_index);
        printf("lookup,sparse,%u,%u,%llu,%llu\n", (unsigned)page_index,
               sample,
               (unsigned long long)LOOKUPS_PER_SAMPLE,
               (unsigned long long)(now_ns() - start));
    }
    for (u32 sample = 0; sample < LOOKUP_SAMPLES; sample++) {
        u64 start = now_ns();
        for (u32 lookup = 0; lookup < LOOKUPS_PER_SAMPLE; lookup++)
            checksum += __atomic_load_n(
                &dense_classifier_reference[page_index], __ATOMIC_ACQUIRE);
        printf("lookup,dense,%u,%u,%llu,%llu\n", (unsigned)page_index,
               sample,
               (unsigned long long)LOOKUPS_PER_SAMPLE,
               (unsigned long long)(now_ns() - start));
    }
    assert(slab_get_classifier_stats(&stats));
    printf("classifier_payload,%llu,%llu,%llu,%llu,%llu\n",
           (unsigned long long)stats.root_payload_bytes,
           (unsigned long long)stats.middle_payload_bytes,
           (unsigned long long)stats.leaf_payload_bytes,
           (unsigned long long)stats.retained_payload_bytes,
           (unsigned long long)stats.peak_payload_bytes);
    assert(checksum != 0U && slab_try_free(object) && slab_verify());
    printf("classifier_heap_used,%llu\n",
           (unsigned long long)heap_get_used_size());
    return 0;
}

static bool use_slab;

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
                         !strcmp(argv[1], "classifier")));
    use_slab = !strcmp(argv[1], "slab") ||
               !strcmp(argv[1], "classifier");
    assert(posix_memalign(&arena, PAGE_SIZE, benchmark_heap_capacity) == 0);
    memset(arena, 0, benchmark_heap_capacity);
    benchmark_heap_base = (uintptr_t)arena;
    assert(heap_init());
    if (use_slab)
        assert(slab_init(arena, benchmark_heap_capacity));
    if (!strcmp(argv[1], "classifier"))
        return run_classifier_comparison();

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
