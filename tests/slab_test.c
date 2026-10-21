/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <slab.h>
#include <heap.h>
#include <pmm.h>
#include <spinlock.h>

/* Compile the production slab implementation in this translation unit so the
 * independent fixture can inspect its private representation for corruption
 * controls and ownership reconstruction. */
#include "../kernel/src/slab.c"

#define ARENA_PAGES 2048U
#define MAX_METADATA_ALLOCS 4096U
#define WORKERS 8U
#define HELD_PER_WORKER 64U
#define CHURN_OPERATIONS 4000U

static u8 *arena;
static bool backing_used[ARENA_PAGES];
static bool backing_fail_next;
static bool metadata_fail_next;
static i32 backing_force_page = -1;
typedef struct { void *pointer; usize size; } metadata_allocation_t;
static metadata_allocation_t metadata_allocations[MAX_METADATA_ALLOCS];
static usize metadata_allocation_count;
static pthread_mutex_t backing_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t refill_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t refill_condition = PTHREAD_COND_INITIALIZER;
static bool refill_pause;
static bool refill_release;
static u32 refill_target;
static u32 refill_waiters;
static _Thread_local u32 held_spinlocks;
static _Thread_local u32 host_cpu_index;
static bool panic_child_exit;

u32 cpu_current_index(void)
{
    return host_cpu_index;
}

u32 cpu_count(void)
{
    return 8U;
}

static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_condition = PTHREAD_COND_INITIALIZER;
static pthread_t pause_thread;
static _Atomic bool pause_enabled;
static bool pause_reached;
static bool pause_release;
static bool pause_configured;
static spinlock_t *migrate_before_lock;
static u32 migrate_to_cpu;

u32 spinlock_depth_current(void)
{
    return held_spinlocks;
}

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}

u64 spin_lock_irqsave(spinlock_t *lock)
{
    if (lock == migrate_before_lock) {
        migrate_before_lock = NULL;
        host_cpu_index = migrate_to_cpu;
    }
    if (atomic_load_explicit(&pause_enabled, memory_order_acquire) &&
        pthread_equal(pthread_self(), pause_thread)) {
        assert(pthread_mutex_lock(&pause_lock) == 0);
        if (atomic_load_explicit(&pause_enabled, memory_order_relaxed) &&
            pthread_equal(pthread_self(), pause_thread)) {
            atomic_store_explicit(&pause_enabled, false, memory_order_release);
            pause_reached = true;
            assert(pthread_cond_broadcast(&pause_condition) == 0);
            while (!pause_release)
                assert(pthread_cond_wait(&pause_condition, &pause_lock) == 0);
        }
        assert(pthread_mutex_unlock(&pause_lock) == 0);
    }
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE))
        sched_yield();
    held_spinlocks++;
    return 1ULL << 9;
}

bool spin_try_lock_irqsave(spinlock_t *lock, u64 *flags_out)
{
    u32 expected = 0U;
    assert(lock && flags_out);
    if (!__atomic_compare_exchange_n(&lock->value, &expected, 1U, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return false;
    held_spinlocks++;
    *flags_out = 1ULL << 9;
    return true;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    assert(held_spinlocks);
    held_spinlocks--;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
    (void)flags;
}

bool heap_address_is_mapped(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return arena && address >= (uintptr_t)arena &&
           address < (uintptr_t)arena + ARENA_PAGES * PAGE_SIZE;
}

bool heap_address_range_is_mapped(const void *pointer, usize size)
{
    uintptr_t address = (uintptr_t)pointer;
    if (!pointer || !size || address > (uintptr_t)-1 - size)
        return false;
    if (arena && address >= (uintptr_t)arena &&
        address + size <= (uintptr_t)arena + ARENA_PAGES * PAGE_SIZE)
        return true;
    bool found = false;
    assert(pthread_mutex_lock(&backing_lock) == 0);
    for (usize index = 0; index < metadata_allocation_count; index++) {
        uintptr_t begin = (uintptr_t)metadata_allocations[index].pointer;
        usize capacity = metadata_allocations[index].size;
        if (address >= begin && address - begin <= capacity &&
            size <= capacity - (address - begin)) {
            found = true;
            break;
        }
    }
    assert(pthread_mutex_unlock(&backing_lock) == 0);
    return found;
}

void *heap_alloc_raw(usize size)
{
    assert(held_spinlocks == 0U && size);
    assert(pthread_mutex_lock(&backing_lock) == 0);
    if (metadata_fail_next) {
        metadata_fail_next = false;
        assert(pthread_mutex_unlock(&backing_lock) == 0);
        return NULL;
    }
    assert(metadata_allocation_count < MAX_METADATA_ALLOCS);
    void *allocation = malloc(size);
    if (allocation) {
        metadata_allocations[metadata_allocation_count++] =
            (metadata_allocation_t){ allocation, size };
    }
    assert(pthread_mutex_unlock(&backing_lock) == 0);
    return allocation;
}

static bool heap_free_metadata_mock(void *pointer)
{
    for (usize index = 0; index < metadata_allocation_count; index++) {
        if (metadata_allocations[index].pointer != pointer)
            continue;
        free(pointer);
        metadata_allocations[index] =
            metadata_allocations[--metadata_allocation_count];
        return true;
    }
    return false;
}

void heap_free_raw(void *pointer)
{
    assert(held_spinlocks == 0U);
    if (!pointer)
        return;
    assert(pthread_mutex_lock(&backing_lock) == 0);
    if (heap_free_metadata_mock(pointer)) {
        assert(pthread_mutex_unlock(&backing_lock) == 0);
        return;
    }
    uintptr_t address = (uintptr_t)pointer;
    assert(!((address - (uintptr_t)arena) & (PAGE_SIZE - 1U)));
    assert(address >= (uintptr_t)arena &&
           address < (uintptr_t)arena + ARENA_PAGES * PAGE_SIZE);
    u32 page = (u32)((address - (uintptr_t)arena) / PAGE_SIZE);
    assert(backing_used[page]);
    backing_used[page] = false;
    memset(pointer, 0xdd, PAGE_SIZE);
    assert(pthread_mutex_unlock(&backing_lock) == 0);
}

bool heap_alloc_page_extent(usize page_count, heap_page_extent_t *extent)
{
    assert(extent);
    extent->base = NULL;
    extent->page_count = 0U;
    if (page_count != 1U)
        return false;
    assert(held_spinlocks == 0U);
    assert(pthread_mutex_lock(&backing_lock) == 0);
    if (backing_fail_next) {
        backing_fail_next = false;
        assert(pthread_mutex_unlock(&backing_lock) == 0);
        return false;
    }
    if (backing_force_page >= 0) {
        u32 page = (u32)backing_force_page;
        backing_force_page = -1;
        if (page < ARENA_PAGES && !backing_used[page]) {
            backing_used[page] = true;
            memset(arena + (usize)page * PAGE_SIZE, 0xa5, PAGE_SIZE);
            assert(pthread_mutex_unlock(&backing_lock) == 0);
            extent->base = arena + (usize)page * PAGE_SIZE;
            extent->page_count = page_count;
            return true;
        }
    }
    for (u32 page = 0; page < ARENA_PAGES; page++) {
        if (!backing_used[page]) {
            backing_used[page] = true;
            memset(arena + (usize)page * PAGE_SIZE, 0xa5, PAGE_SIZE);
            assert(pthread_mutex_unlock(&backing_lock) == 0);
            assert(pthread_mutex_lock(&refill_mutex) == 0);
            if (refill_pause) {
                refill_waiters++;
                assert(pthread_cond_broadcast(&refill_condition) == 0);
                while (!refill_release)
                    assert(pthread_cond_wait(&refill_condition,
                                             &refill_mutex) == 0);
            }
            assert(pthread_mutex_unlock(&refill_mutex) == 0);
            extent->base = arena + (usize)page * PAGE_SIZE;
            extent->page_count = page_count;
            return true;
        }
    }
    assert(pthread_mutex_unlock(&backing_lock) == 0);
    return false;
}

void heap_free_page_extent(heap_page_extent_t *extent)
{
    assert(extent && extent->base && extent->page_count == 1U);
    heap_free_raw(extent->base);
    extent->base = NULL;
    extent->page_count = 0U;
}

void panic(const char *message)
{
    if (panic_child_exit) {
        (void)message;
        _exit(86);
    }
    fprintf(stderr, "unexpected slab panic: %s\n", message);
    abort();
}

static void fixture_start(void)
{
    void *memory = NULL;
    assert(!slab_initialization_state);
    assert(posix_memalign(&memory, PAGE_SIZE, ARENA_PAGES * PAGE_SIZE) == 0);
    arena = memory;
    memset(backing_used, 0, sizeof(backing_used));
    metadata_allocation_count = 0;
    backing_force_page = -1;
    assert(!slab_init(arena, ARENA_PAGES * PAGE_SIZE - 1U));
    assert(slab_init(arena, ARENA_PAGES * PAGE_SIZE));
    assert(!slab_init(arena, ARENA_PAGES * PAGE_SIZE));
    assert(slab_class_size(SLAB_CLASS_COUNT) == 0U);
    assert(slab_class_slots(SLAB_CLASS_COUNT) == 0U);
    assert(!slab_get_class_stats(SLAB_CLASS_COUNT, &(slab_class_stats_t){0}));
    assert(slab_classifier_root_storage[0] == 0U &&
           slab_classifier_root_storage[
               SLAB_CLASSIFIER_ROOT_ENTRIES_MAX - 1U] == 0U);
}

static usize live_backing_pages(void)
{
    usize count = 0;
    for (u32 page = 0; page < ARENA_PAGES; page++)
        count += backing_used[page] ? 1U : 0U;
    return count;
}

static void independent_map_check(void)
{
    usize mapped = 0;
    for (usize page = 0; page < ARENA_PAGES; page++) {
        u64 token = slab_page_token_load(page);
        u8 tag = slab_token_class(token);
        assert(tag <= SLAB_CLASS_COUNT);
        assert(!tag || (token >> SLAB_CLASSIFIER_CLASS_BITS));
        if (tag) {
            mapped++;
            slab_descriptor_t *slab = (slab_descriptor_t *)(arena +
                                          page * PAGE_SIZE);
            assert(slab->class_index == (u32)tag - 1U);
            assert(slab->slot_count ==
                   (PAGE_SIZE - SLAB_OBJECT_AREA_OFFSET) /
                       (slab_class_size(slab->class_index) + 2U));
            usize set_bits = 0;
            const u64 *bitmap = slab_bitmap_const(slab);
            for (u32 slot = 0; slot < slab->slot_count; slot++)
                set_bits += (bitmap[slot / 64U] >> (slot % 64U)) & 1U;
            assert(set_bits == slab->allocated_count);
            assert(backing_used[page]);
        }
    }
    assert(mapped == live_backing_pages());
    assert(slab_verify());
}

static void ownership_check(void **objects, const usize *sizes, usize count)
{
    for (usize i = 0; i < count; i++) {
        uintptr_t i_start = (uintptr_t)objects[i];
        uintptr_t i_end = i_start + sizes[i];
        assert(objects[i] && !((i_start) & 15U));
        for (usize j = i + 1U; j < count; j++) {
            uintptr_t j_start = (uintptr_t)objects[j];
            uintptr_t j_end = j_start + sizes[j];
            assert(i_end <= j_start || j_end <= i_start);
        }
    }
}

static void every_class_and_transitions(void)
{
    static const u16 expected[SLAB_CLASS_COUNT] = {
        16U, 32U, 48U, 64U, 80U, 96U, 112U, 128U,
        144U, 160U, 176U, 192U, 208U, 224U, 240U, 256U,
        288U, 320U, 352U, 384U
    };
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        usize class_size = expected[class_index];
        usize slots = (PAGE_SIZE - SLAB_OBJECT_AREA_OFFSET) /
                      (class_size + 2U);
        usize count = slots + 1U;
        void **objects = calloc(count, sizeof(*objects));
        usize *sizes = calloc(count, sizeof(*sizes));
        assert(objects && sizes && slab_class_size(class_index) == class_size &&
               slab_class_slots(class_index) == slots);
        for (usize i = 0; i < count; i++) {
            objects[i] = slab_alloc(class_size);
            sizes[i] = class_size;
            assert(objects[i]);
            memset(objects[i], (int)(class_index + i), class_size);
            assert((uintptr_t)objects[i] % 16U == 0U);
            assert((uintptr_t)objects[i] >=
                   (uintptr_t)arena + SLAB_OBJECT_AREA_OFFSET);
        }
        ownership_check(objects, sizes, count);
        independent_map_check();
        slab_class_stats_t stats;
        assert(slab_get_class_stats(class_index, &stats));
        assert(stats.live_slots == count && stats.full_slabs == 1U &&
               stats.partial_slabs == 1U && stats.backing_allocations == 2U &&
               stats.peak_slab_pages == 2U);
        for (usize i = 0; i < count; i++) {
            const u8 *bytes = objects[i];
            for (usize byte = 0; byte < class_size; byte++)
                assert(bytes[byte] == (u8)(class_index + i));
        }
        for (usize i = 0; i < count; i++) {
            assert(slab_try_free(objects[i]));
            independent_map_check();
        }
        assert(!slab_try_free(objects[0])); /* Retired page, classifier clear. */
        assert(slab_get_class_stats(class_index, &stats));
        assert(!stats.live_slots && !stats.partial_slabs && !stats.full_slabs &&
               stats.retired_slabs == 2U && stats.backing_releases == 2U &&
               stats.peak_slab_pages == 2U);
        free(sizes);
        free(objects);
    }
    assert(live_backing_pages() == 0U);
}

static void bitmap_first_free_boundaries(void)
{
    static const u32 classes[] = {0U, 4U, 19U};
    static const u16 occupied_cases[] = {
        0U, 1U, 63U, 64U, 65U, 127U, 128U, 129U, 200U, 249U, 250U
    };
    union {
        max_align_t alignment;
        u8 bytes[SLAB_OBJECT_AREA_OFFSET];
    } storage;
    slab_descriptor_t *slab = (slab_descriptor_t *)storage.bytes;
    u64 *bitmap = slab_bitmap(slab);

    for (usize class_position = 0;
         class_position < sizeof(classes) / sizeof(classes[0]);
         class_position++) {
        u32 class_index = classes[class_position];
        u16 slots = slab_slots_for_class(class_index);
        slab->slot_count = slots;
        for (usize case_index = 0;
             case_index < sizeof(occupied_cases) / sizeof(occupied_cases[0]);
             case_index++) {
            u16 occupied = occupied_cases[case_index];
            if (occupied > slots)
                continue;
            for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
                u32 first = word * 64U;
                u32 count = occupied > first ? occupied - first : 0U;
                bitmap[word] = count >= 64U ? ~(u64)0 :
                    (count ? (((u64)1U << count) - 1U) : 0U);
                slab_update_bitmap_word_integrity(slab, word);
            }
            u16 slot = 0;
            bool found = slab_find_free_slot_locked(slab, class_index, &slot);
            if (occupied == slots) {
                assert(!found);
            } else {
                assert(found && slot == occupied);
            }
        }
    }
}

static void class_selection_boundaries(void)
{
    static const struct {
        usize request;
        u32 expected_class;
    } cases[] = {
        {1U, 0U}, {16U, 0U}, {17U, 1U}, {32U, 1U},
        {33U, 2U}, {240U, 14U}, {241U, 15U}, {256U, 15U},
        {257U, 16U}, {272U, 16U}, {273U, 16U}, {288U, 16U},
        {289U, 17U}, {320U, 17U}, {321U, 18U}, {352U, 18U},
        {353U, 19U}, {384U, 19U},
    };
    for (usize index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        void *object = slab_alloc(cases[index].request);
        assert(object);
        uintptr_t page = (uintptr_t)object &
                         ~((uintptr_t)PAGE_SIZE - 1U);
        slab_descriptor_t *slab = (slab_descriptor_t *)page;
        assert(slab->class_index == cases[index].expected_class);
        assert(slab_class_size(slab->class_index) >=
               cases[index].request);
        assert(slab_try_free(object));
    }
    assert(slab_alloc(0U) == NULL && slab_alloc(385U) == NULL);
    assert(!slab_try_free((void *)((uintptr_t)arena - PAGE_SIZE)));
    assert(!slab_try_free(arena + ARENA_PAGES * PAGE_SIZE));
    independent_map_check();
}

static void classifier_leaf_boundary_and_reuse(void)
{
    slab_classifier_stats_t before, after;
    assert(slab_get_classifier_stats(&before));
    backing_force_page = 511;
    usize left_count = slab_class_slots(0U);
    void *left_objects[256];
    assert(left_count <= sizeof(left_objects) / sizeof(left_objects[0]));
    for (usize index = 0; index < left_count; index++) {
        left_objects[index] = slab_alloc(16U);
        assert(left_objects[index]);
    }
    backing_force_page = 512;
    void *right = slab_alloc(16U);
    assert(right);
    usize left_page = ((uintptr_t)left_objects[0] - slab_heap_start) /
                      PAGE_SIZE;
    usize right_page = ((uintptr_t)right - slab_heap_start) / PAGE_SIZE;
    assert(left_page == 511U && right_page == 512U);
    u64 old_token = slab_page_token_load(left_page);
    assert(slab_token_class(old_token) == 1U &&
           (old_token >> SLAB_CLASSIFIER_CLASS_BITS) == 1U);
    assert(slab_page_token_load(right_page) != 0U);
    assert(slab_try_free(right));
    for (usize index = 0; index < left_count; index++)
        assert(slab_try_free(left_objects[index]));

    backing_force_page = 511;
    void *reused = slab_alloc(16U);
    assert(reused && (((uintptr_t)reused - slab_heap_start) / PAGE_SIZE) ==
                         left_page);
    u64 new_token = slab_page_token_load(left_page);
    assert(slab_token_class(new_token) == 1U && new_token != old_token &&
           (new_token >> SLAB_CLASSIFIER_CLASS_BITS) == 2U);
    assert(slab_try_free(reused));

    assert(slab_get_classifier_stats(&after));
    assert(after.root_tables == 1U && after.middle_tables == 1U &&
           after.leaf_tables == 2U &&
           after.retained_payload_bytes ==
               after.middle_payload_bytes + after.leaf_payload_bytes &&
           after.peak_payload_bytes == after.retained_payload_bytes &&
           after.retained_payload_bytes >= before.retained_payload_bytes);
    assert(after.root_payload_bytes == 0U &&
           after.static_root_bytes == sizeof(slab_classifier_root_storage));
    assert(after.middle_payload_bytes == sizeof(slab_classifier_middle_t));
    assert(after.leaf_payload_bytes == 2U *
           sizeof(slab_classifier_leaf_t));
    independent_map_check();
}

static void classifier_metadata_failure(void)
{
    slab_classifier_stats_t before, after;
    assert(slab_get_classifier_stats(&before));
    metadata_fail_next = true;
    assert(!slab_classifier_ensure(1024U));
    assert(slab_get_classifier_stats(&after));
    assert(after.allocation_failures == before.allocation_failures + 1U &&
           after.retained_payload_bytes == before.retained_payload_bytes);
    assert(slab_classifier_ensure(1024U));
    assert(slab_page_token_load(1024U) == 0U);
    assert(!slab_classifier_ensure(slab_heap_pages));
    assert(slab_get_classifier_stats(&after));
    assert(after.leaf_tables == before.leaf_tables + 1U);
}

static void load_classifier_root_out_of_bounds(void)
{
    (void)slab_page_token_load(SLAB_CLASSIFIER_ROOT_ENTRIES_MAX *
                              SLAB_CLASSIFIER_ROOT_PAGES);
}

static void reuse_failure_and_accounting(void)
{
    slab_class_stats_t stats;
    void *first = slab_alloc(16U);
    assert(first);
    uintptr_t first_page = (uintptr_t)first & ~((uintptr_t)PAGE_SIZE - 1U);
    assert((uintptr_t)first == first_page + SLAB_OBJECT_AREA_OFFSET);
    assert(slab_try_free(first));
    void *other_class = slab_alloc(32U);
    assert(other_class &&
           (((uintptr_t)other_class & ~((uintptr_t)PAGE_SIZE - 1U)) ==
            first_page));
    assert(slab_try_free(other_class));

    backing_fail_next = true;
    assert(!slab_alloc(384U));
    assert(slab_get_class_stats(19U, &stats));
    assert(stats.allocation_failures == 1U && !stats.live_slots &&
           !stats.partial_slabs && !stats.full_slabs);
    void *recovered = slab_alloc(384U);
    assert(recovered && slab_try_free(recovered));
    assert(!slab_alloc(0U) && !slab_alloc(385U));
    independent_map_check();
}

static void backing_exhaustion_and_recovery(void)
{
    const usize slots_per_page = slab_class_slots(19U);
    const usize object_count = ARENA_PAGES * slots_per_page;
    void **objects = calloc(object_count, sizeof(*objects));
    slab_class_stats_t before, exhausted, recovered;
    assert(objects && live_backing_pages() == 0U);
    assert(slab_get_class_stats(19U, &before));

    for (usize index = 0; index < object_count; index++) {
        objects[index] = slab_alloc(384U);
        assert(objects[index]);
    }
    assert(live_backing_pages() == ARENA_PAGES);
    assert(slab_get_class_stats(19U, &exhausted));
    assert(exhausted.live_slots == before.live_slots + object_count &&
           exhausted.full_slabs == before.full_slabs + ARENA_PAGES);
    assert(slab_alloc(384U) == NULL);
    assert(slab_get_class_stats(19U, &exhausted));
    assert(exhausted.allocation_failures == before.allocation_failures + 1U &&
           exhausted.live_slots == before.live_slots + object_count &&
           live_backing_pages() == ARENA_PAGES);

    assert(slab_try_free(objects[0]));
    objects[0] = slab_alloc(384U);
    assert(objects[0]);
    assert(slab_get_class_stats(19U, &recovered));
    assert(recovered.live_slots == exhausted.live_slots &&
           recovered.backing_allocations == exhausted.backing_allocations);
    for (usize index = 0; index < object_count; index++)
        assert(slab_try_free(objects[index]));
    assert(live_backing_pages() == 0U);
    assert(slab_get_class_stats(19U, &recovered));
    assert(recovered.retired_slabs == before.retired_slabs + ARENA_PAGES &&
           recovered.live_slots == before.live_slots);
    independent_map_check();
    free(objects);
}

static void expect_panic(void (*action)(void))
{
    static u32 control_index;
    u32 current_control = ++control_index;
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        panic_child_exit = true;
        alarm(3U);
        action();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 86) {
        fprintf(stderr, "negative control %u did not panic (status=%d)\n",
                current_control, status);
        abort();
    }
}

static void expect_child_success(void (*action)(void))
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        alarm(3U);
        action();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "verification negative control failed (status=%d)\n",
                status);
        abort();
    }
}

static void *cpu_cache_fault_pointer;

static void duplicate_cached_free(void)
{
    (void)slab_cpu_try_free(cpu_cache_fault_pointer);
}

static void cpu_cache_metadata_corruption(void)
{
    slab_cpu_cache_t *cache = slab_cpu_cache(0U, 4U);
    assert(cache && cache->count);
    cache->entries[0].object = (u8 *)cache->entries[0].object + 1U;
    assert(!slab_verify());
}

static void cpu_cache_duplicate_entry_corruption(void)
{
    slab_cpu_cache_t *cache = slab_cpu_cache(0U, 4U);
    assert(cache && cache->count == SLAB_LOCAL_CACHE_CAPACITY);
    if (SLAB_LOCAL_CACHE_CAPACITY == 1U) {
        slab_cpu_cache_t *remote = slab_cpu_cache(1U, 4U);
        assert(remote);
        if (!remote->count) {
            host_cpu_index = 1U;
            void *object = slab_cpu_alloc(72U);
            assert(object && slab_cpu_try_free(object));
            host_cpu_index = 0U;
        }
        assert(remote->count == 1U);
        remote->entries[0] = cache->entries[0];
    } else {
        cache->entries[SLAB_LOCAL_CACHE_CAPACITY - 1U] = cache->entries[0];
    }
    assert(!slab_verify());
}

static void *cpu_cache_foreign_entry_pointer;

static void cpu_cache_foreign_entry_use(void)
{
    host_cpu_index = 0U;
    (void)slab_cpu_alloc(72U);
}

static void cpu_cache_foreign_entry_drain(void)
{
    (void)slab_cpu_drain_all();
}

static void cpu_cache_count_corruption(void)
{
    slab_cpu_cache_t *cache = slab_cpu_cache(0U, 4U);
    assert(cache);
    cache->count = SLAB_LOCAL_CACHE_CAPACITY + 1U;
    (void)slab_cpu_alloc(72U);
}

typedef struct {
    u32 cpu;
    pthread_barrier_t *barrier;
    void *object;
    _Atomic u32 *errors;
} cpu_cache_worker_t;

static void *cpu_cache_remote_free_worker(void *argument)
{
    cpu_cache_worker_t *worker = argument;
    host_cpu_index = worker->cpu;
    int wait = pthread_barrier_wait(worker->barrier);
    assert(wait == 0 || wait == PTHREAD_BARRIER_SERIAL_THREAD);
    if (!slab_cpu_try_free(worker->object))
        atomic_fetch_add(worker->errors, 1U);
    return NULL;
}

static void *cpu_cache_concurrent_churn_worker(void *argument)
{
    cpu_cache_worker_t *worker = argument;
    host_cpu_index = worker->cpu;
    for (u32 operation = 0; operation < 3000U; operation++) {
        usize size = operation & 1U ? 72U : 192U;
        void *object = slab_cpu_alloc(size);
        if (!object) {
            atomic_fetch_add(worker->errors, 1U);
            continue;
        }
        memset(object, (int)worker->cpu, size);
        if (!slab_cpu_try_free(object))
            atomic_fetch_add(worker->errors, 1U);
    }
    return NULL;
}

typedef struct {
    void *object;
} cpu_cache_refill_state_t;

static void *cpu_cache_refill_worker(void *argument)
{
    cpu_cache_refill_state_t *state = argument;
    host_cpu_index = 0U;
    state->object = slab_cpu_alloc(72U);
    return NULL;
}

/* Model a thread suspended in growable backing allocation while another
 * thread runs on the same CPU. Only one refill batch may be reserved for the
 * CPU/class; the competing call must take the shared allocator path. */
static void bounded_cpu_cache_refill(void)
{
    pthread_t threads[2];
    cpu_cache_refill_state_t states[2] = {{0}};
    slab_class_stats_t before, after;
    host_cpu_index = 0U;
    assert(slab_get_class_stats(4U, &before));
    assert(pthread_mutex_lock(&refill_mutex) == 0);
    refill_pause = true;
    refill_release = false;
    refill_target = 2U;
    refill_waiters = 0U;
    assert(pthread_mutex_unlock(&refill_mutex) == 0);
    for (u32 index = 0; index < 2U; index++)
        assert(pthread_create(&threads[index], NULL,
                              cpu_cache_refill_worker,
                              &states[index]) == 0);
    assert(pthread_mutex_lock(&refill_mutex) == 0);
    while (refill_waiters < refill_target)
        assert(pthread_cond_wait(&refill_condition, &refill_mutex) == 0);
    refill_release = true;
    refill_pause = false;
    assert(pthread_cond_broadcast(&refill_condition) == 0);
    assert(pthread_mutex_unlock(&refill_mutex) == 0);
    for (u32 index = 0; index < 2U; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
        assert(states[index].object);
    }
    assert(states[0].object != states[1].object);
    ownership_check((void *[]){states[0].object, states[1].object},
                    (usize[]){72U, 72U}, 2U);
    assert(slab_get_class_stats(4U, &after));
    assert(after.cache_refills == before.cache_refills + 1U &&
           after.cache_refill_conflicts ==
               before.cache_refill_conflicts + 1U);
    assert(slab_cpu_try_free(states[0].object));
    assert(slab_cpu_try_free(states[1].object));
    assert(slab_verify());
    assert(slab_cpu_drain_all() >= 1U);
    assert(slab_verify() && live_backing_pages() == 0U);
}

typedef struct {
    u32 cpu;
    pthread_barrier_t *allocated;
    pthread_barrier_t *release;
    void *object;
} cpu_cache_live_worker_t;

static void *cpu_cache_live_worker(void *argument)
{
    cpu_cache_live_worker_t *worker = argument;
    host_cpu_index = worker->cpu;
    worker->object = slab_cpu_alloc(192U);
    assert(worker->object);
    memset(worker->object, (int)(0x80U + worker->cpu), 192U);
    int result = pthread_barrier_wait(worker->allocated);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    result = pthread_barrier_wait(worker->release);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    assert(slab_cpu_try_free(worker->object));
    return NULL;
}

static void cpu_cache_simultaneous_live_allocations(void)
{
    pthread_t threads[2];
    pthread_barrier_t allocated, release;
    cpu_cache_live_worker_t workers[2] = {
        {.cpu = 0U}, {.cpu = 1U},
    };
    assert(pthread_barrier_init(&allocated, NULL, 3U) == 0);
    assert(pthread_barrier_init(&release, NULL, 3U) == 0);
    for (u32 index = 0; index < 2U; index++) {
        workers[index].allocated = &allocated;
        workers[index].release = &release;
        assert(pthread_create(&threads[index], NULL, cpu_cache_live_worker,
                              &workers[index]) == 0);
    }
    int result = pthread_barrier_wait(&allocated);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    ownership_check((void *[]){workers[0].object, workers[1].object},
                    (usize[]){192U, 192U}, 2U);
    assert(workers[0].object != workers[1].object);
    result = pthread_barrier_wait(&release);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (u32 index = 0; index < 2U; index++)
        assert(pthread_join(threads[index], NULL) == 0);
    assert(pthread_barrier_destroy(&release) == 0);
    assert(pthread_barrier_destroy(&allocated) == 0);
    assert(slab_verify());
}

static void cpu_cache_selection_migration(void)
{
    slab_cpu_cache_t *cache0 = slab_cpu_cache(0U, 4U);
    void *cpu1_object;
    assert(cache0);
    host_cpu_index = 0U;
    void *cpu0_object = slab_cpu_alloc(72U);
    assert(cpu0_object && slab_cpu_try_free(cpu0_object));
    host_cpu_index = 1U;
    cpu1_object = slab_cpu_alloc(72U);
    assert(cpu1_object && slab_cpu_try_free(cpu1_object));

    /* Switch CPUs after the cache index is chosen but before irqsave has
     * disabled migration. The implementation must release the old cache and
     * retry against the new CPU's local state. */
    host_cpu_index = 0U;
    migrate_to_cpu = 1U;
    migrate_before_lock = &cache0->lock;
    void *migrated = slab_cpu_alloc(72U);
    assert(!migrate_before_lock && host_cpu_index == 1U &&
           migrated == cpu1_object);
    assert(slab_cpu_try_free(migrated));
    assert(slab_verify());
    (void)slab_cpu_drain_all();
    assert(slab_verify() && live_backing_pages() == 0U);
}

static void cpu_cache_lifecycle_and_concurrency(void)
{
    void *objects[SLAB_LOCAL_CACHE_CAPACITY + 3U];
    slab_class_stats_t stats;
    const u32 object_count = SLAB_LOCAL_CACHE_CAPACITY + 3U;
    host_cpu_index = 0U;
    bounded_cpu_cache_refill();
    cpu_cache_selection_migration();
    host_cpu_index = 0U;
    for (u32 index = 0; index < object_count; index++) {
        objects[index] = slab_cpu_alloc(72U);
        assert(objects[index] && !((uintptr_t)objects[index] & 15U));
        memset(objects[index], 0x60 + (int)index, 72U);
    }
    for (u32 index = 0; index < SLAB_LOCAL_CACHE_CAPACITY; index++)
        assert(slab_cpu_try_free(objects[index]));
    cpu_cache_fault_pointer = objects[0];
    expect_panic(duplicate_cached_free);
    assert(slab_get_class_stats(4U, &stats));
    assert(stats.live_slots == 3U &&
           stats.cached_slots == SLAB_LOCAL_CACHE_CAPACITY &&
           stats.cache_hits >= 1U && stats.cache_misses >= 1U);
    assert(slab_verify());
    assert(slab_cpu_try_free(objects[SLAB_LOCAL_CACHE_CAPACITY]));
    assert(slab_get_class_stats(4U, &stats));
    assert(stats.live_slots == 2U &&
           stats.cached_slots == SLAB_LOCAL_CACHE_CAPACITY &&
           stats.cache_overflows >= 1U);
    for (u32 index = SLAB_LOCAL_CACHE_CAPACITY + 1U;
         index < object_count; index++)
        assert(slab_cpu_try_free(objects[index]));
    assert(slab_verify());
    expect_child_success(cpu_cache_metadata_corruption);
    expect_child_success(cpu_cache_duplicate_entry_corruption);
    expect_panic(cpu_cache_count_corruption);
    assert(slab_verify());

    /* A corrupted local entry must not steal a slot owned by another CPU's
     * cache. The slot owner byte records the cache CPU while cached. */
    host_cpu_index = 0U;
    void *cpu0_cache_object = slab_cpu_alloc(72U);
    assert(cpu0_cache_object && slab_cpu_try_free(cpu0_cache_object));
    host_cpu_index = 1U;
    void *cpu1_cache_object = slab_cpu_alloc(72U);
    assert(cpu1_cache_object && slab_cpu_try_free(cpu1_cache_object));
    slab_cpu_cache_t *cpu0_cache = slab_cpu_cache(0U, 4U);
    slab_cpu_cache_t *cpu1_cache = slab_cpu_cache(1U, 4U);
    assert(cpu0_cache && cpu1_cache && cpu0_cache->count &&
           cpu1_cache->count);
    u32 top = cpu0_cache->count - 1U;
    void *cpu0_saved_object = cpu0_cache->entries[top].object;
    cpu_cache_foreign_entry_pointer = cpu1_cache->entries[
        cpu1_cache->count - 1U].object;
    cpu0_cache->entries[top].object = cpu_cache_foreign_entry_pointer;
    expect_panic(cpu_cache_foreign_entry_use);
    expect_panic(cpu_cache_foreign_entry_drain);
    cpu0_cache->entries[top].object = cpu0_saved_object;
    assert(slab_verify());
    cpu_cache_simultaneous_live_allocations();

    host_cpu_index = 0U;
    void *remote = slab_cpu_alloc(72U);
    assert(remote);
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 2U) == 0);
    _Atomic u32 errors = 0U;
    cpu_cache_worker_t worker = {
        .cpu = 1U, .barrier = &barrier, .object = remote,
        .errors = &errors,
    };
    pthread_t remote_thread;
    assert(pthread_create(&remote_thread, NULL,
                          cpu_cache_remote_free_worker, &worker) == 0);
    int wait = pthread_barrier_wait(&barrier);
    assert(wait == 0 || wait == PTHREAD_BARRIER_SERIAL_THREAD);
    assert(pthread_join(remote_thread, NULL) == 0);
    assert(pthread_barrier_destroy(&barrier) == 0);
    assert(!atomic_load(&errors));
    assert(slab_get_class_stats(4U, &stats));
    assert(stats.cross_cpu_frees != 0U);

    pthread_t workers[2];
    cpu_cache_worker_t churn[2] = {
        {.cpu = 0U, .errors = &errors},
        {.cpu = 1U, .errors = &errors},
    };
    for (u32 index = 0; index < 2U; index++)
        assert(pthread_create(&workers[index], NULL,
            cpu_cache_concurrent_churn_worker, &churn[index]) == 0);
    for (u32 index = 0; index < 2U; index++)
        assert(pthread_join(workers[index], NULL) == 0);
    assert(!atomic_load(&errors));
    assert(slab_verify());
    usize drained = slab_cpu_drain_all();
    assert(drained >= 1U && slab_verify());
    assert(live_backing_pages() == 0U);

    /* Exercise cache refill, local reuse, payload preservation, and a
     * quiescent drain for every size class, including the 16- and 384-byte
     * boundaries. */
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        usize size = slab_class_size(class_index);
        host_cpu_index = class_index & 1U;
        void *object = slab_cpu_alloc(size);
        assert(object && !((uintptr_t)object & 15U));
        memset(object, (int)(0x40U + class_index), size);
        assert(slab_cpu_try_free(object));
        void *reused = slab_cpu_alloc(size);
        assert(reused == object);
        for (usize byte = 0; byte < size; byte++)
            assert(((u8 *)reused)[byte] == (u8)(0x40U + class_index));
        assert(slab_cpu_try_free(reused));
        assert(slab_verify());
        (void)slab_cpu_drain_all();
        assert(slab_verify() && live_backing_pages() == 0U);
    }
    host_cpu_index = 0U;
    puts("slab: per-CPU cache hits, refill, overflow, cross-CPU free, drain, and churn passed");
}

static void bounded_runtime_cache_reclaim(void)
{
    void *objects[SLAB_LOCAL_CACHE_CAPACITY];
    slab_cpu_cache_t *cache = slab_cpu_cache(0U, 4U);
    slab_reclaim_result_t result;
    slab_class_stats_t before, after;
    pthread_t workers[2];
    cpu_cache_worker_t churn[2];
    _Atomic u32 errors = 0U;

    assert(cache);
    host_cpu_index = 0U;
    for (u32 index = 0; index < SLAB_LOCAL_CACHE_CAPACITY; index++) {
        objects[index] = slab_cpu_alloc(72U);
        assert(objects[index]);
    }
    for (u32 index = 0; index < SLAB_LOCAL_CACHE_CAPACITY; index++)
        assert(slab_cpu_try_free(objects[index]));
    assert(cache->count == SLAB_LOCAL_CACHE_CAPACITY);

    /* A busy remote record is skipped after one try; reclamation reports an
     * incomplete bounded pass and leaves ownership untouched. */
    __atomic_store_n(&slab_reclaim_cursor, 4U, __ATOMIC_RELAXED);
    __atomic_store_n(&cache->lock.value, 1U, __ATOMIC_RELEASE);
    assert(slab_reclaim_bounded(1U, 1U, &result));
    assert(result.records_examined == 1U && result.cache_lock_busy == 1U &&
           !result.cached_objects_returned &&
           cache->count == SLAB_LOCAL_CACHE_CAPACITY);
    __atomic_store_n(&cache->lock.value, 0U, __ATOMIC_RELEASE);

    __atomic_store_n(&slab_reclaim_cursor, 4U, __ATOMIC_RELAXED);
    __atomic_store_n(&slab_classes[4U].lock.value, 1U, __ATOMIC_RELEASE);
    assert(slab_reclaim_bounded(1U, 1U, &result));
    assert(result.records_examined == 1U && result.class_lock_busy == 1U &&
           !result.cached_objects_returned &&
           cache->count == SLAB_LOCAL_CACHE_CAPACITY);
    __atomic_store_n(&slab_classes[4U].lock.value, 0U, __ATOMIC_RELEASE);
    assert(slab_verify());

    u64 held_flags = spin_lock_irqsave(&cache->lock);
    assert(!slab_reclaim_bounded(1U, 1U, &result));
    spin_unlock_irqrestore(&cache->lock, held_flags);

    __atomic_store_n(&slab_reclaim_cursor, 4U, __ATOMIC_RELAXED);
    /* The object budget should stop a full scan before its record budget. */
    assert(slab_reclaim_bounded(CPU_MAX_COUNT * SLAB_CLASS_COUNT, 1U,
                                &result));
    assert(result.cached_objects_returned == 1U &&
           result.object_budget_reached &&
           !result.cache_record_budget_reached);
    void *record_limited = slab_alloc(72U);
    assert(record_limited && slab_cpu_try_free(record_limited));
    __atomic_store_n(&slab_reclaim_cursor, 4U, __ATOMIC_RELAXED);
    assert(slab_reclaim_bounded(1U,
        SLAB_LOCAL_CACHE_CAPACITY + 1U, &result));
    assert(result.cached_objects_returned == SLAB_LOCAL_CACHE_CAPACITY &&
           !result.object_budget_reached &&
           result.cache_record_budget_reached &&
           result.slab_pages_released == 1U && !cache->count);
    assert(slab_verify());

    /* Reclaim while two independent CPU cache records continue allocation
     * and free traffic. Try-lock misses are acceptable; ownership corruption
     * or lost slots are not. */
    for (u32 index = 0; index < 2U; index++) {
        churn[index] = (cpu_cache_worker_t){
            .cpu = index, .errors = &errors,
        };
        assert(pthread_create(&workers[index], NULL,
            cpu_cache_concurrent_churn_worker, &churn[index]) == 0);
    }
    for (u32 pass = 0; pass < 256U; pass++)
        assert(slab_reclaim_bounded(16U, 2U, &result));
    for (u32 index = 0; index < 2U; index++)
        assert(pthread_join(workers[index], NULL) == 0);
    assert(!atomic_load(&errors) && slab_verify());
    (void)slab_cpu_drain_all();
    assert(slab_verify() && live_backing_pages() == 0U);

    /* A failed raw backing request runs one bounded cache-reclaim pass and
     * retries only after it returned owned objects/pages. */
    host_cpu_index = 0U;
    for (u32 index = 0; index < SLAB_LOCAL_CACHE_CAPACITY; index++) {
        objects[index] = slab_cpu_alloc(72U);
        assert(objects[index]);
    }
    for (u32 index = 0; index < SLAB_LOCAL_CACHE_CAPACITY; index++)
        assert(slab_cpu_try_free(objects[index]));
    assert(slab_get_class_stats(4U, &before));
    backing_fail_next = true;
    void *pressure_result = slab_alloc(384U);
    assert(pressure_result);
    assert(slab_get_class_stats(4U, &after));
    assert(after.cache_drain_objects >= before.cache_drain_objects +
           SLAB_LOCAL_CACHE_CAPACITY);
    assert(slab_try_free(pressure_result));
    assert(slab_verify() && live_backing_pages() == 0U);
}

static void bitmap_search_zero_slots(void)
{
    union {
        max_align_t alignment;
        u8 bytes[SLAB_OBJECT_AREA_OFFSET];
    } storage = {0};
    slab_descriptor_t *slab = (slab_descriptor_t *)storage.bytes;
    u16 slot;
    slab->slot_count = 0U;
    (void)slab_find_free_slot_locked(slab, 0U, &slot);
}

static void bitmap_search_excessive_slots(void)
{
    union {
        max_align_t alignment;
        u8 bytes[SLAB_OBJECT_AREA_OFFSET];
    } storage = {0};
    slab_descriptor_t *slab = (slab_descriptor_t *)storage.bytes;
    u16 slot;
    slab->slot_count = SLAB_BITMAP_WORDS * 64U + 1U;
    (void)slab_find_free_slot_locked(slab, 0U, &slot);
}

static void classifier_tree_boundaries(void)
{
    void *memory = NULL;
    assert(posix_memalign(&memory, PAGE_SIZE, PAGE_SIZE) == 0);
    arena = memory;
    assert(slab_init(arena, 2U * SLAB_CLASSIFIER_ROOT_PAGES * PAGE_SIZE));
    assert(slab_heap_pages == 2U * SLAB_CLASSIFIER_ROOT_PAGES &&
           slab_classifier_root_entries == 2U);

    slab_classifier_stats_t stats;
    metadata_fail_next = true;
    assert(!slab_classifier_ensure(0U));
    assert(slab_get_classifier_stats(&stats) &&
           stats.allocation_failures == 1U && stats.root_tables == 1U &&
           stats.static_root_bytes == sizeof(slab_classifier_root_storage));
    assert(slab_classifier_ensure(0U));
    assert(slab_classifier_ensure(511U));
    metadata_fail_next = true;
    assert(!slab_classifier_ensure(512U));
    assert(slab_classifier_ensure(512U));
    assert(slab_classifier_ensure(SLAB_CLASSIFIER_ROOT_PAGES - 1U));

    metadata_fail_next = true;
    assert(!slab_classifier_ensure(SLAB_CLASSIFIER_ROOT_PAGES));
    assert(slab_classifier_ensure(SLAB_CLASSIFIER_ROOT_PAGES));
    assert(slab_classifier_ensure(2U * SLAB_CLASSIFIER_ROOT_PAGES - 1U));
    assert(!slab_classifier_ensure(2U * SLAB_CLASSIFIER_ROOT_PAGES));
    assert(slab_page_token_load(0U) == 0U &&
           slab_page_token_load(511U) == 0U &&
           slab_page_token_load(512U) == 0U &&
           slab_page_token_load(SLAB_CLASSIFIER_ROOT_PAGES - 1U) == 0U &&
           slab_page_token_load(SLAB_CLASSIFIER_ROOT_PAGES) == 0U &&
           slab_page_token_load(2U * SLAB_CLASSIFIER_ROOT_PAGES) == 0U);
    expect_panic(load_classifier_root_out_of_bounds);
    assert(slab_get_classifier_stats(&stats));
    assert(stats.root_tables == 1U && stats.middle_tables == 2U &&
           stats.leaf_tables == 5U && stats.allocation_failures == 3U &&
           stats.root_payload_bytes == 0U &&
           stats.retained_payload_bytes ==
               stats.middle_payload_bytes + stats.leaf_payload_bytes);
    free(arena);
}

static void *fault_pointer;
static void *fault_anchor;
static void free_through_slab_dispatch(void *pointer)
{
    if (!slab_try_free(pointer))
        panic("test indexed-heap fallback rejects invalid slab free");
}
static void free_interior_pointer(void)
{
    (void)slab_try_free((u8 *)fault_pointer + 1U);
}
static void free_after_last_slot(void)
{
    void *object = slab_alloc(384U);
    assert(object);
    uintptr_t page = (uintptr_t)object &
                     ~((uintptr_t)PAGE_SIZE - 1U);
    slab_descriptor_t *slab = (slab_descriptor_t *)page;
    uintptr_t invalid = page + SLAB_OBJECT_AREA_OFFSET +
                        (usize)slab->slot_count *
                            slab_sizes[slab->class_index];
    (void)slab_try_free((void *)invalid);
}
static void free_duplicate_pointer(void)
{
    assert(slab_try_free(fault_pointer));
    (void)slab_try_free(fault_pointer);
}
static void free_retired_pointer_through_dispatch(void)
{
    assert(slab_try_free(fault_pointer));
    free_through_slab_dispatch(fault_pointer);
}
static void corrupt_bitmap_then_free(void)
{
    usize page = ((uintptr_t)fault_pointer - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab_bitmap(slab)[0] ^= 2U;
    (void)slab_try_free(fault_pointer);
}
static void corrupt_other_bitmap_word_before_retirement(void)
{
    assert(slab_try_free(fault_anchor));
    usize page = ((uintptr_t)fault_pointer - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab_bitmap(slab)[1] ^= 1U;
    (void)slab_try_free(fault_pointer);
}
static void corrupt_later_bitmap_word_before_slot_use(void)
{
    void *objects[70];
    for (u32 index = 0; index < 70U; index++) {
        objects[index] = slab_alloc(16U);
        assert(objects[index]);
    }
    usize page = ((uintptr_t)objects[0] - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab_bitmap(slab)[1] ^= (u64)1U << 63;
    /* The unrelated word is not needed to free slot zero. It must be
     * rejected before allocation can select any slot from that word. */
    assert(slab_try_free(objects[0]));
    for (u32 index = 0; index < 66U; index++)
        assert(slab_alloc(16U));
}
static void corrupt_unrelated_bitmap_word_then_nonfinal_free(void)
{
    void *objects[70];
    for (u32 index = 0; index < 70U; index++) {
        objects[index] = slab_alloc(16U);
        assert(objects[index]);
    }
    usize page = ((uintptr_t)objects[0] - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab_bitmap(slab)[1] ^= (u64)1U << 63;
    assert(slab_try_free(objects[0]));
    assert(!slab_verify());
}
static void corrupt_bitmap_population_then_nonfinal_free(void)
{
    void *objects[70];
    for (u32 index = 0; index < 70U; index++) {
        objects[index] = slab_alloc(16U);
        assert(objects[index]);
    }
    usize page = ((uintptr_t)objects[0] - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab_bitmap(slab)[1] |= (u64)1U << 63;
    slab_update_bitmap_word_integrity(slab, 1U);
    slab_update_integrity(slab);
    assert(slab_try_free(objects[0]));
    assert(!slab_verify());
}
static void corrupt_unrelated_partial_neighbor_then_free(void)
{
    const usize slots = slab_class_slots(19U);
    void *objects[30];
    assert(slots == 10U);
    for (u32 index = 0; index < 30U; index++) {
        objects[index] = slab_alloc(384U);
        assert(objects[index]);
    }
    assert(slab_try_free(objects[0]));
    assert(slab_try_free(objects[10]));
    slab_descriptor_t *head = slab_classes[19].partial_head;
    assert(head && head->partial_next);
    slab_descriptor_t *neighbor = head->partial_next;
    neighbor->partial_prev = NULL;
    slab_update_integrity(neighbor);
    assert(slab_try_free(objects[11]));
    assert(!slab_verify());
}
static void corrupt_class_accounting_then_verify(void)
{
    assert(slab_alloc(16U));
    slab_classes[0].stats.partial_slabs++;
    assert(!slab_verify());
}
static void corrupt_bitmap_deep_verifier(void)
{
    usize page = ((uintptr_t)fault_pointer - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab_bitmap(slab)[1] ^= 1U;
    assert(!slab_verify());
}
static void corrupt_descriptor_then_free(void)
{
    usize page = ((uintptr_t)fault_pointer - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab->class_index = 19U;
    (void)slab_try_free(fault_pointer);
}
static void corrupt_backing_extent_then_free(void)
{
    usize page = ((uintptr_t)fault_pointer - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab->backing_page_count++;
    (void)slab_try_free(fault_pointer);
}
static void corrupt_bitmap_checksum_then_free(void)
{
    usize page = ((uintptr_t)fault_pointer - slab_heap_start) / PAGE_SIZE;
    slab_descriptor_t *slab = (slab_descriptor_t *)(slab_heap_start +
                                                    page * PAGE_SIZE);
    slab->bitmap_integrity[1] ^= 1U;
    (void)slab_try_free(fault_pointer);
}
static void corrupt_class_tag_then_free(void)
{
    usize page = ((uintptr_t)fault_pointer - slab_heap_start) / PAGE_SIZE;
    slab_page_token_store(page,
        (1ULL << SLAB_CLASSIFIER_CLASS_BITS) | 0x1fU);
    (void)slab_try_free(fault_pointer);
}
static void corrupt_partial_link_then_allocate(void)
{
    slab_descriptor_t *head = slab_classes[0].partial_head;
    head->partial_next = (slab_descriptor_t *)(uintptr_t)0xdead0000U;
    (void)slab_alloc(16U);
}

static void corrupt_classifier_root_entry(void)
{
    __atomic_store_n(&slab_classifier_root_storage[0],
                     (uintptr_t)0xdead0000U, __ATOMIC_RELEASE);
    (void)slab_page_token_load(0U);
}

static void corrupt_classifier_middle_pointer(void)
{
    slab_classifier_middle_t *middle = (slab_classifier_middle_t *)
        __atomic_load_n(&slab_classifier_root_storage[0], __ATOMIC_ACQUIRE);
    middle->magic ^= 1U;
    (void)slab_page_token_load(0U);
}

static void corrupt_classifier_leaf_pointer(void)
{
    slab_classifier_middle_t *middle = (slab_classifier_middle_t *)
        __atomic_load_n(&slab_classifier_root_storage[0], __ATOMIC_ACQUIRE);
    __atomic_store_n(&middle->leaves[0], (uintptr_t)0xdead0000U,
                     __ATOMIC_RELEASE);
    (void)slab_page_token_load(0U);
}

static void corrupt_classifier_leaf_identity(void)
{
    slab_classifier_middle_t *middle = (slab_classifier_middle_t *)
        __atomic_load_n(&slab_classifier_root_storage[0], __ATOMIC_ACQUIRE);
    slab_classifier_leaf_t *leaf = (slab_classifier_leaf_t *)
        __atomic_load_n(&middle->leaves[0], __ATOMIC_ACQUIRE);
    leaf->leaf_index++;
    (void)slab_page_token_load(0U);
}

static void corruption_controls(void)
{
    fault_pointer = slab_alloc(16U);
    fault_anchor = slab_alloc(16U);
    assert(fault_pointer && fault_anchor);
    expect_child_success(corrupt_bitmap_deep_verifier);
    expect_panic(free_interior_pointer);
    expect_panic(free_after_last_slot);
    expect_panic(free_duplicate_pointer);
    expect_panic(corrupt_bitmap_then_free);
    expect_panic(corrupt_other_bitmap_word_before_retirement);
    expect_panic(corrupt_later_bitmap_word_before_slot_use);
    expect_panic(corrupt_descriptor_then_free);
    expect_panic(corrupt_backing_extent_then_free);
    expect_panic(corrupt_bitmap_checksum_then_free);
    expect_panic(corrupt_class_tag_then_free);
    expect_panic(corrupt_partial_link_then_allocate);
#if SLAB_DEEP_VALIDATION
    expect_panic(corrupt_unrelated_bitmap_word_then_nonfinal_free);
    expect_panic(corrupt_bitmap_population_then_nonfinal_free);
    expect_panic(corrupt_unrelated_partial_neighbor_then_free);
#else
    expect_child_success(corrupt_unrelated_bitmap_word_then_nonfinal_free);
    expect_child_success(corrupt_bitmap_population_then_nonfinal_free);
    expect_child_success(corrupt_unrelated_partial_neighbor_then_free);
#endif
    expect_child_success(corrupt_class_accounting_then_verify);
    expect_panic(corrupt_classifier_root_entry);
    expect_panic(corrupt_classifier_middle_pointer);
    expect_panic(corrupt_classifier_leaf_pointer);
    expect_panic(corrupt_classifier_leaf_identity);
    void *original_fault_pointer = fault_pointer;
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
         class_index++) {
        fault_pointer = slab_alloc(slab_class_size(class_index));
        assert(fault_pointer);
        expect_panic(free_interior_pointer);
        assert(slab_try_free(fault_pointer));
    }
    fault_pointer = original_fault_pointer;
    assert(slab_try_free(fault_pointer));
    assert(slab_try_free(fault_anchor));
    fault_pointer = slab_alloc(16U);
    assert(fault_pointer);
    expect_panic(free_retired_pointer_through_dispatch);
    assert(slab_try_free(fault_pointer));
    independent_map_check();
}

typedef struct {
    void *objects[HELD_PER_WORKER];
    u8 marker;
} worker_state_t;
static worker_state_t worker_states[WORKERS];
static pthread_barrier_t allocation_barrier;
static pthread_barrier_t release_barrier;

static void *simultaneous_worker(void *argument)
{
    worker_state_t *state = argument;
    for (u32 i = 0; i < HELD_PER_WORKER; i++) {
        state->objects[i] = slab_alloc(72U);
        assert(state->objects[i]);
        memset(state->objects[i], state->marker, 72U);
    }
    int result = pthread_barrier_wait(&allocation_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    result = pthread_barrier_wait(&release_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (u32 i = 0; i < HELD_PER_WORKER; i++) {
        const u8 *bytes = state->objects[i];
        for (u32 byte = 0; byte < 72U; byte++)
            assert(bytes[byte] == state->marker);
        assert(slab_try_free(state->objects[i]));
    }
    return NULL;
}

static void concurrency_and_cross_cpu_free(void)
{
    pthread_t threads[WORKERS];
    assert(pthread_barrier_init(&allocation_barrier, NULL, WORKERS + 1U) == 0);
    assert(pthread_barrier_init(&release_barrier, NULL, WORKERS + 1U) == 0);
    for (u32 worker = 0; worker < WORKERS; worker++) {
        worker_states[worker].marker = (u8)(worker + 1U);
        assert(pthread_create(&threads[worker], NULL, simultaneous_worker,
                              &worker_states[worker]) == 0);
    }
    int result = pthread_barrier_wait(&allocation_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    void *all[WORKERS * HELD_PER_WORKER];
    usize sizes[WORKERS * HELD_PER_WORKER];
    usize at = 0;
    for (u32 worker = 0; worker < WORKERS; worker++)
        for (u32 i = 0; i < HELD_PER_WORKER; i++) {
            all[at] = worker_states[worker].objects[i];
            sizes[at++] = 72U;
        }
    ownership_check(all, sizes, at);
    independent_map_check();
    result = pthread_barrier_wait(&release_barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (u32 worker = 0; worker < WORKERS; worker++)
        assert(pthread_join(threads[worker], NULL) == 0);
    assert(pthread_barrier_destroy(&release_barrier) == 0);
    assert(pthread_barrier_destroy(&allocation_barrier) == 0);
    independent_map_check();
    assert(live_backing_pages() == 0U);

    void *objects[WORKERS * 8U];
    for (usize i = 0; i < WORKERS * 8U; i++) {
        objects[i] = slab_alloc(80U);
        assert(objects[i]);
    }
    for (u32 worker = 0; worker < WORKERS; worker++) {
        for (usize i = worker; i < WORKERS * 8U; i += WORKERS)
            assert(slab_try_free(objects[i]));
    }
    independent_map_check();
}

typedef struct {
    void *object;
} refill_state_t;

typedef struct {
    pthread_barrier_t *barrier;
    usize page_index;
    bool result;
} classifier_create_state_t;

static void *classifier_create_worker(void *argument)
{
    classifier_create_state_t *state = argument;
    int result = pthread_barrier_wait(state->barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    state->result = slab_classifier_ensure(state->page_index);
    return NULL;
}

static void classifier_concurrent_creation(void)
{
    void *memory = NULL;
    enum { CREATE_WORKERS = 8 };
    const usize capacity = 2U * SLAB_CLASSIFIER_ROOT_PAGES * PAGE_SIZE;
    const usize pages[CREATE_WORKERS] = {
        0U, 1U, 512U, 513U,
        SLAB_CLASSIFIER_ROOT_PAGES,
        SLAB_CLASSIFIER_ROOT_PAGES + 1U,
        SLAB_CLASSIFIER_ROOT_PAGES + 512U,
        SLAB_CLASSIFIER_ROOT_PAGES + 513U,
    };
    pthread_t threads[CREATE_WORKERS];
    classifier_create_state_t states[CREATE_WORKERS];
    pthread_barrier_t barrier;
    slab_classifier_stats_t stats;

    assert(posix_memalign(&memory, PAGE_SIZE, PAGE_SIZE) == 0);
    arena = memory;
    assert(slab_init(arena, capacity));
    assert(slab_classifier_root_entries == 2U);
    assert(pthread_barrier_init(&barrier, NULL, CREATE_WORKERS) == 0);
    for (u32 index = 0; index < CREATE_WORKERS; index++) {
        states[index] = (classifier_create_state_t){
            .barrier = &barrier,
            .page_index = pages[index],
            .result = false,
        };
        assert(pthread_create(&threads[index], NULL,
                              classifier_create_worker,
                              &states[index]) == 0);
    }
    for (u32 index = 0; index < CREATE_WORKERS; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
        assert(states[index].result);
    }
    assert(pthread_barrier_destroy(&barrier) == 0);
    assert(slab_get_classifier_stats(&stats));
    assert(stats.root_tables == 1U && stats.middle_tables == 2U &&
           stats.leaf_tables == 4U && metadata_allocation_count == 7U &&
           slab_cpu_cache_allocation != NULL);
    for (u32 index = 0; index < CREATE_WORKERS; index++)
        assert(slab_page_token_load(pages[index]) == 0U);
    puts("slab: concurrent classifier root, middle, and leaf publication passed");
}

static void *simultaneous_refill_worker(void *argument)
{
    refill_state_t *state = argument;
    state->object = slab_alloc(96U);
    return NULL;
}

static void competing_refills(void)
{
    pthread_t threads[4];
    refill_state_t states[4] = {0};
    slab_class_stats_t before;
    assert(slab_get_class_stats(5U, &before));
    /* The deterministic page provider pauses after each allocation has
     * escaped the class lock, forcing all workers through the same miss. */
    assert(pthread_mutex_lock(&refill_mutex) == 0);
    refill_pause = true;
    refill_release = false;
    refill_target = 4U;
    refill_waiters = 0U;
    assert(pthread_mutex_unlock(&refill_mutex) == 0);
    for (u32 i = 0; i < 4U; i++)
        assert(pthread_create(&threads[i], NULL, simultaneous_refill_worker,
                              &states[i]) == 0);
    assert(pthread_mutex_lock(&refill_mutex) == 0);
    while (refill_waiters < refill_target)
        assert(pthread_cond_wait(&refill_condition, &refill_mutex) == 0);
    refill_release = true;
    refill_pause = false;
    assert(pthread_cond_broadcast(&refill_condition) == 0);
    assert(pthread_mutex_unlock(&refill_mutex) == 0);
    for (u32 i = 0; i < 4U; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(states[i].object);
    }
    ownership_check((void *[]){states[0].object, states[1].object,
                               states[2].object, states[3].object},
                    (usize[]){96U, 96U, 96U, 96U}, 4U);
    slab_class_stats_t stats;
    assert(slab_get_class_stats(5U, &stats));
    assert(stats.live_slots == before.live_slots + 4U &&
           stats.partial_slabs == before.partial_slabs + 1U &&
           stats.backing_allocations - before.backing_allocations == 4U &&
           stats.backing_releases - before.backing_releases == 3U);
    for (u32 i = 0; i < 4U; i++) assert(slab_try_free(states[i].object));
    independent_map_check();
}

static void *pause_free_worker(void *argument)
{
    (void)argument;
    assert(pthread_mutex_lock(&pause_lock) == 0);
    while (!pause_configured)
        assert(pthread_cond_wait(&pause_condition, &pause_lock) == 0);
    assert(pthread_mutex_unlock(&pause_lock) == 0);
    (void)slab_try_free(fault_pointer);
    return NULL;
}

static void retire_and_reclassify_before_waiter(void)
{
    pthread_t worker;
    fault_pointer = slab_alloc(384U);
    assert(fault_pointer);
    assert(pthread_mutex_lock(&pause_lock) == 0);
    pause_configured = false;
    pause_reached = false;
    pause_release = false;
    assert(pthread_mutex_unlock(&pause_lock) == 0);
    assert(pthread_create(&worker, NULL, pause_free_worker, NULL) == 0);
    assert(pthread_mutex_lock(&pause_lock) == 0);
    pause_thread = worker;
    atomic_store_explicit(&pause_enabled, true, memory_order_release);
    pause_configured = true;
    assert(pthread_cond_broadcast(&pause_condition) == 0);
    while (!pause_reached)
        assert(pthread_cond_wait(&pause_condition, &pause_lock) == 0);
    assert(pthread_mutex_unlock(&pause_lock) == 0);

    assert(slab_try_free(fault_pointer));
    void *new_class = slab_alloc(384U);
    assert(new_class &&
           ((uintptr_t)new_class & ~((uintptr_t)PAGE_SIZE - 1U)) ==
           ((uintptr_t)fault_pointer & ~((uintptr_t)PAGE_SIZE - 1U)));
    assert(pthread_mutex_lock(&pause_lock) == 0);
    pause_release = true;
    assert(pthread_cond_broadcast(&pause_condition) == 0);
    assert(pthread_mutex_unlock(&pause_lock) == 0);
    (void)pthread_join(worker, NULL);
    assert(false && "stale waiter should panic before reading retired slab");
}

static void retired_waiter_negative_control(void)
{
    expect_panic(retire_and_reclassify_before_waiter);
}

#if SLAB_EMPTY_RESERVE_PER_CLASS
static void free_reserved_fault_pointer(void)
{
    assert(slab_try_free(fault_pointer));
}

static void empty_reserve_lifecycle_and_pressure(void)
{
    void *first_objects[SLAB_CLASS_COUNT];

    assert(live_backing_pages() == 0U);
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_stats_t before, retained, reused;
        usize request = slab_class_size(class_index);
        first_objects[class_index] = slab_alloc(request);
        assert(first_objects[class_index]);
        assert(slab_get_class_stats(class_index, &before));
        assert(slab_try_free(first_objects[class_index]));
        assert(slab_get_class_stats(class_index, &retained));
        assert(retained.empty_slabs == 1U && !retained.partial_slabs &&
               !retained.full_slabs && retained.retired_slabs == 0U);
        usize page_index = ((uintptr_t)first_objects[class_index] -
                            (uintptr_t)arena) / PAGE_SIZE;
        u64 token = slab_page_token_load(page_index);
        assert(slab_token_class(token) == class_index + 1U &&
               backing_used[page_index]);

        void *reused_object = slab_alloc(request);
        assert(reused_object == first_objects[class_index]);
        assert(slab_get_class_stats(class_index, &reused));
        assert(reused.empty_slab_reuses ==
                   retained.empty_slab_reuses + 1U &&
               reused.backing_allocations == retained.backing_allocations &&
               reused.live_slots == 1U);
        assert(slab_try_free(reused_object));
        assert(slab_get_class_stats(class_index, &retained));
        assert(retained.empty_slabs == 1U && retained.live_slots == 0U);
    }
    assert(live_backing_pages() == SLAB_CLASS_COUNT);
    assert(slab_verify());

    fault_pointer = first_objects[0];
    expect_panic(free_reserved_fault_pointer);
    assert(slab_drain_empty() == SLAB_CLASS_COUNT);
    assert(live_backing_pages() == 0U && slab_verify());
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_stats_t stats;
        usize page_index = ((uintptr_t)first_objects[class_index] -
                            (uintptr_t)arena) / PAGE_SIZE;
        u64 token = slab_page_token_load(page_index);
        assert(!slab_token_class(token) &&
               slab_token_generation(token) == 1U &&
               (token & SLAB_TOKEN_RETIRING));
        assert(slab_get_class_stats(class_index, &stats));
        assert(!stats.empty_slabs && stats.retired_slabs == 1U &&
               stats.backing_releases == 1U);
    }

    /* A failed backing request drains reserves outside class locks, then
     * retries once. The newly obtained page belongs to the requested class. */
    void *reserved = slab_alloc(16U);
    assert(reserved && slab_try_free(reserved));
    assert(live_backing_pages() == 1U);
    slab_class_stats_t large_before;
    assert(slab_get_class_stats(19U, &large_before));
    backing_fail_next = true;
    void *recovered = slab_alloc(384U);
    assert(recovered);
    assert(live_backing_pages() == 1U);
    slab_class_stats_t small_stats, large_stats;
    assert(slab_get_class_stats(0U, &small_stats));
    assert(small_stats.empty_slabs == 0U &&
           small_stats.retired_slabs == 2U);
    assert(slab_get_class_stats(19U, &large_stats));
    assert(large_stats.live_slots == large_before.live_slots + 1U &&
           large_stats.backing_allocations ==
               large_before.backing_allocations + 1U);
    assert(slab_try_free(recovered));
    assert(slab_drain_empty() == 1U && live_backing_pages() == 0U);
    assert(slab_verify());
}

static void *reserve_churn_worker(void *argument)
{
    u32 seed = *(u32 *)argument;
    for (u32 operation = 0; operation < 3000U; operation++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        usize request = slab_class_size(seed % SLAB_CLASS_COUNT);
        void *object = slab_alloc(request);
        assert(object);
        memset(object, (int)seed, request);
        assert(slab_try_free(object));
    }
    *(u32 *)argument = seed;
    return NULL;
}

static void empty_reserve_concurrent_drain(void)
{
    enum { RESERVE_WORKERS = 4 };
    pthread_t workers[RESERVE_WORKERS];
    u32 seeds[RESERVE_WORKERS];
    for (u32 index = 0; index < RESERVE_WORKERS; index++) {
        seeds[index] = 0x61c88647U ^ (index * 0x9e3779b9U);
        assert(pthread_create(&workers[index], NULL, reserve_churn_worker,
                              &seeds[index]) == 0);
    }
    for (u32 iteration = 0; iteration < 300U; iteration++)
        (void)slab_drain_empty();
    for (u32 index = 0; index < RESERVE_WORKERS; index++)
        assert(pthread_join(workers[index], NULL) == 0);
    (void)slab_drain_empty();
    assert(live_backing_pages() == 0U);
    independent_map_check();
}
#endif

static u64 benchmark_clock_ns(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (u64)time.tv_sec * 1000000000ULL + (u64)time.tv_nsec;
}

typedef struct {
    usize request;
    u32 operations;
    pthread_barrier_t *barrier;
} benchmark_worker_t;

static void *benchmark_worker(void *argument)
{
    benchmark_worker_t *worker = argument;
    int result = pthread_barrier_wait(worker->barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (u32 operation = 0; operation < worker->operations; operation++) {
        void *object = slab_alloc(worker->request);
        assert(object);
        assert(slab_try_free(object));
    }
    return NULL;
}

static int run_benchmark(void)
{
    static const usize requests[] = {16U, 72U, 384U};
    const u32 samples = 128U;
    const u32 pairs_per_sample = 1000U;
    enum { BENCH_WORKERS = 4, CONCURRENT_OPERATIONS = 20000 };

    for (u32 request_index = 0;
         request_index < sizeof(requests) / sizeof(requests[0]);
         request_index++) {
        usize request = requests[request_index];
        void *anchor = slab_alloc(request);
        u32 class_index = 0;
        assert(anchor);
        while (class_index < SLAB_CLASS_COUNT &&
               slab_class_size(class_index) < request)
            class_index++;
        assert(class_index < SLAB_CLASS_COUNT);
        slab_class_stats_t before, after;
        assert(slab_get_class_stats(class_index, &before));
        for (u32 sample = 0; sample < samples; sample++) {
            u64 start = benchmark_clock_ns();
            for (u32 pair = 0; pair < pairs_per_sample; pair++) {
                void *object = slab_alloc(request);
                assert(object && object != anchor);
                assert(slab_try_free(object));
            }
            u64 elapsed = benchmark_clock_ns() - start;
            printf("sample,%llu,%u,%u,%llu\n",
                   (unsigned long long)request, sample,
                   pairs_per_sample, (unsigned long long)elapsed);
        }
        assert(slab_get_class_stats(class_index, &after));
        printf("class,%llu,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
               (unsigned long long)request,
               slab_class_size(class_index), slab_class_slots(class_index),
               SLAB_OBJECT_AREA_OFFSET,
               (unsigned long long)(after.lock_acquisitions -
                                    before.lock_acquisitions),
               (unsigned long long)(after.bitmap_words_examined -
                                    before.bitmap_words_examined),
               (unsigned long long)(after.backing_allocations -
                                    before.backing_allocations),
               (unsigned long long)after.partial_slabs,
               (unsigned long long)after.live_slots,
               (unsigned long long)after.peak_live_slots,
               (unsigned long long)after.full_slabs,
               (unsigned long long)after.retired_slabs);
        assert(slab_try_free(anchor));
    }

    for (u32 request_index = 0; request_index < 2U; request_index++) {
        usize request = requests[request_index];
        void *anchor = slab_alloc(request);
        pthread_t threads[BENCH_WORKERS];
        benchmark_worker_t workers[BENCH_WORKERS];
        pthread_barrier_t barrier;
        u32 class_index = request == 16U ? 0U : 4U;
        slab_class_stats_t before, stats;
        assert(anchor);
        assert(slab_get_class_stats(class_index, &before));
        assert(pthread_barrier_init(&barrier, NULL, BENCH_WORKERS + 1U) == 0);
        for (u32 worker = 0; worker < BENCH_WORKERS; worker++) {
            workers[worker] = (benchmark_worker_t){
                .request = request,
                .operations = CONCURRENT_OPERATIONS,
                .barrier = &barrier,
            };
            assert(pthread_create(&threads[worker], NULL, benchmark_worker,
                                  &workers[worker]) == 0);
        }
        u64 start = benchmark_clock_ns();
        int result = pthread_barrier_wait(&barrier);
        assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
        for (u32 worker = 0; worker < BENCH_WORKERS; worker++)
            assert(pthread_join(threads[worker], NULL) == 0);
        u64 elapsed = benchmark_clock_ns() - start;
        printf("concurrent,%llu,%u,%u,%llu\n",
               (unsigned long long)request, BENCH_WORKERS,
               CONCURRENT_OPERATIONS,
               (unsigned long long)elapsed);
        assert(pthread_barrier_destroy(&barrier) == 0);
        assert(slab_get_class_stats(class_index, &stats));
        assert(stats.live_slots == 1U && stats.peak_live_slots >= 2U &&
               stats.peak_live_slots <= BENCH_WORKERS + 1U &&
               stats.partial_slabs == 1U && !stats.full_slabs);
        printf("concurrent_class,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
               (unsigned long long)request,
               (unsigned long long)(stats.lock_acquisitions -
                                    before.lock_acquisitions),
               (unsigned long long)(stats.bitmap_words_examined -
                                    before.bitmap_words_examined),
               (unsigned long long)(stats.backing_allocations -
                                    before.backing_allocations),
               (unsigned long long)stats.live_slots,
               (unsigned long long)stats.peak_live_slots,
               (unsigned long long)stats.partial_slabs,
               (unsigned long long)stats.full_slabs);
        assert(slab_try_free(anchor));
    }
    assert(live_backing_pages() == 0U);
    return 0;
}

static void *churn_worker(void *argument)
{
    u32 seed = *(u32 *)argument;
    for (u32 operation = 0; operation < CHURN_OPERATIONS; operation++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        usize size = 1U + (seed % SLAB_MAX_OBJECT_SIZE);
        void *object = slab_alloc(size);
        assert(object);
        memset(object, (int)(seed & 0xffU), size);
        assert(slab_try_free(object));
    }
    *(u32 *)argument = seed;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--classifier-boundaries")) {
        classifier_tree_boundaries();
        puts("slab: classifier root, leaf, capacity, and metadata failure boundaries passed");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--classifier-concurrency")) {
        classifier_concurrent_creation();
        return 0;
    }
    assert(slab_alloc(16U) == NULL);
    fixture_start();
    if (argc == 2 && !strcmp(argv[1], "--cpu-cache")) {
        cpu_cache_lifecycle_and_concurrency();
        bounded_runtime_cache_reclaim();
        return 0;
    }
#if SLAB_EMPTY_RESERVE_PER_CLASS
    if (argc == 2 && !strcmp(argv[1], "--empty-reserve")) {
        empty_reserve_lifecycle_and_pressure();
        empty_reserve_concurrent_drain();
        puts("slab: bounded empty reserve, pressure drain, and concurrent reuse passed");
        return 0;
    }
#endif
    if (argc == 2 && !strcmp(argv[1], "--benchmark"))
        return run_benchmark();
    assert(argc == 1);
    assert(slab_verify());
    every_class_and_transitions();
    bitmap_first_free_boundaries();
    expect_panic(bitmap_search_zero_slots);
    expect_panic(bitmap_search_excessive_slots);
    class_selection_boundaries();
    classifier_leaf_boundary_and_reuse();
    classifier_metadata_failure();
    reuse_failure_and_accounting();
    backing_exhaustion_and_recovery();
    corruption_controls();
    concurrency_and_cross_cpu_free();
    competing_refills();
    retired_waiter_negative_control();

    pthread_t churn[WORKERS];
    u32 seeds[WORKERS];
    for (u32 i = 0; i < WORKERS; i++) {
        seeds[i] = 0x97e3a51dU ^ (i * 0x9e3779b9U);
        assert(pthread_create(&churn[i], NULL, churn_worker,
                              &seeds[i]) == 0);
    }
    for (u32 i = 0; i < WORKERS; i++)
        assert(pthread_join(churn[i], NULL) == 0);
    independent_map_check();
    assert(live_backing_pages() == 0U);
    printf("slab: classes, sparse ownership, generations, transitions, corruption, retirement races, refill concurrency, and churn passed\n");
    return 0;
}
