/* SPDX-License-Identifier: GPL-3.0-or-later */
#if !defined(SLAB_CORE_DIAGNOSTICS) && !defined(KERNEL_HYBRID_ALLOCATOR)
#error "slab core requires hybrid routing or an isolated diagnostic build"
#endif
#ifndef SLAB_DEEP_VALIDATION
#define SLAB_DEEP_VALIDATION 0
#endif
#if SLAB_DEEP_VALIDATION != 0 && SLAB_DEEP_VALIDATION != 1
#error "SLAB_DEEP_VALIDATION must be zero or one"
#endif

#include <slab.h>
#include <heap.h>
#include <pmm.h>
#include <spinlock.h>
#include <panic.h>
#include <stddef.h>
#include <address_layout.h>
#include <cpu.h>
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
#include <heap_profile.h>
#endif
#define SLAB_BITMAP_WORDS 4U
#define SLAB_BITMAP_BYTES (SLAB_BITMAP_WORDS * sizeof(u64))
#define SLAB_DESCRIPTOR_BYTES 72U
#define SLAB_STATE_PARTIAL 1U
#define SLAB_STATE_FULL 2U
#define SLAB_STATE_EMPTY 3U
#define SLAB_BACKING_HEAP_PAGE_COUNT 1U
#define SLAB_SLOT_SHARED_FREE 0U
#define SLAB_SLOT_CLIENT 1U
#define SLAB_SLOT_CACHED 2U
#define SLAB_SLOT_REFILL 3U
#define SLAB_SLOT_STATE_ENCODE(state) \
    ((u8)(0xa0U | (((~(state)) & 3U) << 2) | ((state) & 3U)))
#define SLAB_CPU_CACHE_LOCK_RETRIES 4U
#define SLAB_TOKEN_GENERATION_BITS 47U
#define SLAB_TOKEN_GENERATION_MASK \
    ((1ULL << SLAB_TOKEN_GENERATION_BITS) - 1U)

_Static_assert(PAGE_SIZE == SLAB_PAGE_BYTES,
               "slab profile and implementation page sizes must match");
_Static_assert(SLAB_DESCRIPTOR_BYTES + SLAB_BITMAP_BYTES <=
                   SLAB_OBJECT_AREA_OFFSET,
               "slab object offset must contain descriptor and bitmap");
_Static_assert((SLAB_OBJECT_AREA_OFFSET & 15U) == 0U,
               "slab object area must preserve 16-byte alignment");
#define SLAB_TOKEN_REF_SHIFT 52U
#define SLAB_TOKEN_REF_MASK (0x7ffULL << SLAB_TOKEN_REF_SHIFT)
#define SLAB_TOKEN_REF_ONE (1ULL << SLAB_TOKEN_REF_SHIFT)
#define SLAB_TOKEN_RETIRING (1ULL << 63)
#define SLAB_TOKEN_REFS_MAX 0x7ffU
#define SLAB_RETIRE_WAIT_LIMIT 10000000U
#ifndef SLAB_EMPTY_RESERVE_PER_CLASS
#define SLAB_EMPTY_RESERVE_PER_CLASS 0
#endif
#if SLAB_EMPTY_RESERVE_PER_CLASS != 0 && SLAB_EMPTY_RESERVE_PER_CLASS != 1
#error "SLAB_EMPTY_RESERVE_PER_CLASS must be zero or one"
#endif
#define SLAB_DESCRIPTOR_MAGIC 0x534c414250495448ULL
#define SLAB_INTEGRITY_SEED 0xa31d59c7e20b846fULL
#define SLAB_CLASSIFIER_FANOUT 512U
#define SLAB_CLASSIFIER_ROOT_PAGES \
    ((usize)SLAB_CLASSIFIER_FANOUT * SLAB_CLASSIFIER_FANOUT)
#define SLAB_CLASSIFIER_ROOT_ENTRIES_MAX \
    (((usize)(KERNEL_HEAP_LIMIT - KERNEL_HEAP_BASE) / PAGE_SIZE + \
      SLAB_CLASSIFIER_ROOT_PAGES - 1U) / SLAB_CLASSIFIER_ROOT_PAGES)
#define SLAB_CLASSIFIER_CLASS_BITS 5U
#define SLAB_CLASSIFIER_CLASS_MASK ((1U << SLAB_CLASSIFIER_CLASS_BITS) - 1U)
#define SLAB_CLASSIFIER_MIDDLE_MAGIC 0x534c41424d494444ULL
#define SLAB_CLASSIFIER_LEAF_MAGIC 0x534c41424c454146ULL

_Static_assert(PAGE_SIZE == 4096U, "Pith slab pages are 4 KiB");
_Static_assert(SLAB_BITMAP_BYTES == 32U,
               "slab bitmap covers 256 slots");
_Static_assert((PAGE_SIZE - SLAB_OBJECT_AREA_OFFSET) / 16U <=
                   SLAB_BITMAP_WORDS * 64U,
               "bitmap covers every slot in the smallest class");
_Static_assert((SLAB_OBJECT_AREA_OFFSET & 15U) == 0,
               "slab object area preserves 16-byte alignment");
_Static_assert((PAGE_SIZE - SLAB_OBJECT_AREA_OFFSET) / 18U <= 256U,
               "slot owner records fit in one byte");
_Static_assert(SLAB_CLASS_COUNT <= SLAB_CLASSIFIER_CLASS_MASK,
               "page-classifier tags fit in the class field");

typedef struct slab_descriptor slab_descriptor_t;
struct slab_descriptor {
    u64 magic;
    u32 class_index;
    u16 slot_count;
    u16 allocated_count;
    u32 state;
    u32 backing_page_count;
    slab_descriptor_t *partial_prev;
    slab_descriptor_t *partial_next;
    u64 integrity;
    u32 bitmap_integrity[SLAB_BITMAP_WORDS];
    u64 static_integrity;
};

typedef struct {
    void *object;
    u64 page_token;
} slab_cache_entry_t;

typedef struct {
    spinlock_t lock;
    u32 count;
    u32 refill_active;
    slab_cache_entry_t entries[SLAB_LOCAL_CACHE_CAPACITY];
    volatile u64 allocations;
    volatile u64 frees;
    volatile u64 hits;
    volatile u64 misses;
    volatile u64 refills;
    volatile u64 refill_conflicts;
    volatile u64 refill_objects;
    volatile u64 drain_objects;
    volatile u64 overflows;
    volatile u64 cross_cpu_frees;
} __attribute__((aligned(64))) slab_cpu_cache_t;

typedef struct {
    u64 magic;
    usize root_index;
    uintptr_t leaves[SLAB_CLASSIFIER_FANOUT];
} slab_classifier_middle_t;

/* Token fields are class[0:4], generation[5:51], live pins[52:62], and
 * retiring[63]. A pin prevents backing reuse after ownership lookup. */
typedef struct {
    u64 magic;
    usize leaf_index;
    u64 page_tokens[SLAB_CLASSIFIER_FANOUT];
} slab_classifier_leaf_t;

typedef struct {
    spinlock_t lock;
    slab_descriptor_t *partial_head;
    slab_descriptor_t *empty_slab;
    slab_class_stats_t stats;
} slab_class_t;

_Static_assert(sizeof(slab_descriptor_t) == SLAB_DESCRIPTOR_BYTES,
               "slab descriptor occupies 72 bytes");
_Static_assert(_Alignof(slab_descriptor_t) >= 8U,
               "slab descriptor requires natural x86-64 alignment");
_Static_assert(sizeof(slab_cpu_cache_t) % 64U == 0U,
               "per-CPU cache records must not share cache lines");
_Static_assert(SLAB_DESCRIPTOR_BYTES + SLAB_BITMAP_BYTES <=
                   SLAB_OBJECT_AREA_OFFSET,
               "slab objects begin after descriptor and bitmap metadata");
_Static_assert(sizeof(slab_classifier_middle_t) == 4112U,
               "classifier middle node layout is stable");
_Static_assert(sizeof(slab_classifier_leaf_t) == 4112U,
               "classifier leaf node layout is stable");
_Static_assert(offsetof(slab_classifier_leaf_t, page_tokens) % 8U == 0U,
               "page tokens are aligned for atomic access");

static const u16 slab_sizes[SLAB_CLASS_COUNT] = {
    16U, 32U, 48U, 64U, 80U, 96U, 112U, 128U,
    144U, 160U, 176U, 192U, 208U, 224U, 240U, 256U,
    288U, 320U, 352U, 384U
};
_Static_assert(sizeof(slab_sizes) / sizeof(slab_sizes[0]) == SLAB_CLASS_COUNT,
               "slab class table matches its public count");

static slab_class_t slab_classes[SLAB_CLASS_COUNT];
static slab_cpu_cache_t *slab_cpu_caches;
static u32 slab_cpu_cache_count;
static void *slab_cpu_cache_allocation;
static usize slab_cpu_cache_payload_bytes;
static volatile u32 slab_reclaim_cursor;
static uintptr_t slab_heap_start;
static uintptr_t slab_heap_end;
static usize slab_heap_pages;
static usize slab_classifier_root_entries;
static uintptr_t slab_classifier_root_storage[
    SLAB_CLASSIFIER_ROOT_ENTRIES_MAX];
static u32 slab_initialization_state;
static u64 slab_classifier_static_root_bytes;
static u64 slab_classifier_middle_payload_bytes;
static u64 slab_classifier_leaf_payload_bytes;
static u64 slab_classifier_retained_payload_bytes;
static u64 slab_classifier_peak_payload_bytes;
static u64 slab_classifier_root_tables;
static u64 slab_classifier_middle_tables;
static u64 slab_classifier_leaf_tables;
static u64 slab_classifier_allocation_failures;
static volatile u64 slab_refill_reservations;

static void slab_require_valid(bool valid, const char *reason);

#ifdef SLAB_CORE_GUEST_FREE_PROFILE
typedef struct {
    bool enabled;
    bool sampled;
    bool interval_started;
    bool interval_completed;
    u32 ordinal;
    u32 cpu_index;
    u64 interval_start;
    u64 interval_ticks;
} slab_free_profile_call_t;

static volatile u32 slab_free_profile_active;
static slab_free_profile_stage_t slab_free_profile_selected_stage;
static slab_free_profile_case_t slab_free_profile_selected_case;
static u32 slab_free_profile_selected_class;
static u32 slab_free_profile_selected_cpu;
static u32 slab_free_profile_stride;
static u32 slab_free_profile_seen;
static u32 slab_free_profile_observed;
static u32 slab_free_profile_sample_count;
static u32 slab_free_profile_lost;
static u32 slab_free_profile_unexpected_lost;
static u32 slab_free_profile_last_lost_reason;
static u32 slab_free_profile_last_lost_ordinal;
static slab_free_profile_sample_t slab_free_profile_samples[
    SLAB_FREE_PROFILE_SAMPLE_CAPACITY];

static u64 slab_free_profile_clock(void)
{
    u32 low;
    u32 high;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence"
                     : "=a"(low), "=d"(high) :: "memory");
    return ((u64)high << 32) | low;
}

static slab_free_profile_call_t slab_free_profile_call_begin(void)
{
    slab_free_profile_call_t call = {0};
    if (!__atomic_load_n(&slab_free_profile_active, __ATOMIC_ACQUIRE))
        return call;
    call.cpu_index = cpu_current_index();
    if (call.cpu_index != slab_free_profile_selected_cpu)
        return call;
    call.enabled = true;
    u32 ordinal = slab_free_profile_seen++;
    call.ordinal = ordinal;
    call.sampled = ordinal % slab_free_profile_stride == 0U;
    return call;
}

static void slab_free_profile_interval_begin(
    slab_free_profile_call_t *call, slab_free_profile_stage_t stage)
{
    if (call && call->sampled &&
        slab_free_profile_selected_stage == stage) {
        call->interval_start = slab_free_profile_clock();
        call->interval_started = true;
    }
}

static void slab_free_profile_interval_end(
    slab_free_profile_call_t *call, slab_free_profile_stage_t stage)
{
    if (call && call->interval_started &&
        slab_free_profile_selected_stage == stage) {
        u64 end = slab_free_profile_clock();
        call->interval_ticks = end - call->interval_start;
        call->interval_started = false;
        call->interval_completed = true;
    }
}

static void slab_free_profile_call_finish(
    slab_free_profile_call_t *call, u32 class_index,
    slab_free_profile_case_t free_case)
{
    if (!call || !call->enabled ||
        call->cpu_index != slab_free_profile_selected_cpu ||
        class_index != slab_free_profile_selected_class ||
        free_case != slab_free_profile_selected_case)
        return;
    slab_free_profile_observed++;
    if (!call->sampled)
        return;
    if (!call->interval_ticks) {
        slab_free_profile_lost++;
        slab_free_profile_last_lost_reason = call->interval_completed ? 2U : 1U;
        if (!call->interval_completed)
            slab_free_profile_unexpected_lost++;
        slab_free_profile_last_lost_ordinal = call->ordinal;
        return;
    }
    u32 index = __atomic_load_n(&slab_free_profile_sample_count,
                                __ATOMIC_RELAXED);
    if (index >= SLAB_FREE_PROFILE_SAMPLE_CAPACITY) {
        slab_free_profile_lost++;
        slab_free_profile_last_lost_reason = 3U;
        slab_free_profile_unexpected_lost++;
        slab_free_profile_last_lost_ordinal = call->ordinal;
        return;
    }
    slab_free_profile_samples[index] = (slab_free_profile_sample_t){
        .ticks = call->interval_ticks,
        .cpu_index = call->cpu_index,
        .class_index = class_index,
        .stage = (u32)slab_free_profile_selected_stage,
        .free_case = (u32)free_case,
    };
    __atomic_store_n(&slab_free_profile_sample_count, index + 1U,
                     __ATOMIC_RELEASE);
}

bool slab_free_profile_configure(slab_free_profile_stage_t stage,
                                 slab_free_profile_case_t free_case,
                                 u32 class_index, u32 cpu_index,
                                 u32 sample_stride)
{
    if (stage <= SLAB_FREE_PROFILE_DISABLED ||
        stage > SLAB_FREE_PROFILE_RELEASE ||
        free_case < SLAB_FREE_PROFILE_NONFINAL ||
        free_case > SLAB_FREE_PROFILE_FINAL ||
        class_index >= SLAB_CLASS_COUNT || cpu_index >= CPU_MAX_COUNT ||
        !sample_stride ||
        __atomic_load_n(&slab_free_profile_active, __ATOMIC_ACQUIRE))
        return false;
    slab_free_profile_selected_stage = stage;
    slab_free_profile_selected_case = free_case;
    slab_free_profile_selected_class = class_index;
    slab_free_profile_selected_cpu = cpu_index;
    slab_free_profile_stride = sample_stride;
    slab_free_profile_seen = 0U;
    slab_free_profile_observed = 0U;
    slab_free_profile_sample_count = 0U;
    slab_free_profile_lost = 0U;
    slab_free_profile_unexpected_lost = 0U;
    slab_free_profile_last_lost_reason = 0U;
    slab_free_profile_last_lost_ordinal = 0U;
    __atomic_store_n(&slab_free_profile_active, 1U, __ATOMIC_RELEASE);
    return true;
}

void slab_free_profile_disable(void)
{
    __atomic_store_n(&slab_free_profile_active, 0U, __ATOMIC_RELEASE);
}

bool slab_free_profile_read(slab_free_profile_sample_t *samples,
                            u32 capacity, u32 *sample_count,
                            u32 *observed_count, u32 *lost_count,
                            u32 *unexpected_lost_count,
                            u32 *lost_reason, u32 *lost_ordinal)
{
    if (__atomic_load_n(&slab_free_profile_active, __ATOMIC_ACQUIRE) ||
        !sample_count || !observed_count || !lost_count ||
        !unexpected_lost_count || !lost_reason ||
        !lost_ordinal ||
        (capacity && !samples))
        return false;
    u32 count = __atomic_load_n(&slab_free_profile_sample_count,
                                __ATOMIC_ACQUIRE);
    if (count > capacity)
        return false;
    for (u32 index = 0; index < count; index++)
        samples[index] = slab_free_profile_samples[index];
    *sample_count = count;
    *observed_count = slab_free_profile_observed;
    *lost_count = slab_free_profile_lost;
    *unexpected_lost_count = slab_free_profile_unexpected_lost;
    *lost_reason = slab_free_profile_last_lost_reason;
    *lost_ordinal = slab_free_profile_last_lost_ordinal;
    return true;
}
#endif

static u64 *slab_bitmap(slab_descriptor_t *slab)
{
    return (u64 *)((u8 *)slab + SLAB_DESCRIPTOR_BYTES);
}

static const u64 *slab_bitmap_const(const slab_descriptor_t *slab)
{
    return (const u64 *)((const u8 *)slab + SLAB_DESCRIPTOR_BYTES);
}

static u8 *slab_slot_states(slab_descriptor_t *slab)
{
    return (u8 *)slab + PAGE_SIZE - 2U * slab->slot_count;
}

static const u8 *slab_slot_states_const(const slab_descriptor_t *slab)
{
    return (const u8 *)slab + PAGE_SIZE - 2U * slab->slot_count;
}

static u8 *slab_slot_owners(slab_descriptor_t *slab)
{
    return (u8 *)slab + PAGE_SIZE - slab->slot_count;
}

static const u8 *slab_slot_owners_const(const slab_descriptor_t *slab)
{
    return (const u8 *)slab + PAGE_SIZE - slab->slot_count;
}

static u8 slab_slot_state_encode(u32 state)
{
    return SLAB_SLOT_STATE_ENCODE(state);
}

static bool slab_slot_state_decode(u8 encoded, u32 *state_out)
{
    u32 state = encoded & 3U;
    if ((encoded & 0xf0U) != 0xa0U ||
        ((encoded >> 2) & 3U) != ((~state) & 3U))
        return false;
    if (state_out)
        *state_out = state;
    return true;
}

static u32 slab_slot_state_load(const slab_descriptor_t *slab, u32 slot)
{
    if (slot >= slab->slot_count)
        return ~(u32)0;
    u8 value = __atomic_load_n(&slab_slot_states_const(slab)[slot],
                               __ATOMIC_ACQUIRE);
    u32 state;
    return slab_slot_state_decode(value, &state) ? state : ~(u32)0;
}

static u32 slab_slot_owner_load(const slab_descriptor_t *slab, u32 slot)
{
    if (slot >= slab->slot_count)
        return ~(u32)0;
    return __atomic_load_n(&slab_slot_owners_const(slab)[slot],
                           __ATOMIC_ACQUIRE);
}

static void slab_slot_owner_store(slab_descriptor_t *slab, u32 slot,
                                  u32 owner)
{
    if (slot >= slab->slot_count || owner >= CPU_MAX_COUNT)
        panic("slab: invalid slot owner update");
    __atomic_store_n(&slab_slot_owners(slab)[slot], (u8)owner,
                     __ATOMIC_RELEASE);
}

static bool slab_slot_state_change(slab_descriptor_t *slab, u32 slot,
                                   u32 from, u32 to)
{
    if (slot >= slab->slot_count)
        return false;
    u8 expected = slab_slot_state_encode(from);
    return __atomic_compare_exchange_n(&slab_slot_states(slab)[slot],
        &expected, slab_slot_state_encode(to), false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static u8 slab_token_class(u64 token)
{
    return (u8)(token & SLAB_CLASSIFIER_CLASS_MASK);
}

static u64 slab_token_generation(u64 token)
{
    return (token >> SLAB_CLASSIFIER_CLASS_BITS) &
           SLAB_TOKEN_GENERATION_MASK;
}

static u32 slab_token_refs(u64 token)
{
    return (u32)((token & SLAB_TOKEN_REF_MASK) >> SLAB_TOKEN_REF_SHIFT);
}

static u64 slab_token_identity(u64 token)
{
    return token & (((SLAB_TOKEN_GENERATION_MASK <<
                      SLAB_CLASSIFIER_CLASS_BITS)) |
                    SLAB_CLASSIFIER_CLASS_MASK);
}

static bool slab_token_matches(u64 token, u32 class_index, u64 generation)
{
    return class_index < SLAB_CLASS_COUNT &&
           slab_token_class(token) == class_index + 1U &&
           slab_token_generation(token) == generation &&
           !(token & SLAB_TOKEN_RETIRING);
}

static bool slab_classifier_metadata_valid(const void *pointer, usize bytes)
{
    return pointer && !((uintptr_t)pointer & 15U) &&
           heap_address_range_is_mapped(pointer, bytes);
}

static void slab_classifier_require_metadata(const void *pointer, usize bytes,
                                             const char *reason)
{
    if (!slab_classifier_metadata_valid(pointer, bytes))
        panic(reason);
}

static usize slab_classifier_payload_size(usize bytes)
{
    if (!bytes || bytes > (usize)-1 - 15U)
        return 0;
    return (bytes + 15U) & ~(usize)15U;
}

static void slab_classifier_record_published(u32 kind, usize requested)
{
    u64 payload = slab_classifier_payload_size(requested);
    u64 total;

    if (!payload)
        panic("slab: classifier metadata size overflow");
    if (kind == 1U) {
        __atomic_add_fetch(&slab_classifier_middle_payload_bytes, payload,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_classifier_middle_tables, 1U,
                           __ATOMIC_RELAXED);
    } else if (kind == 2U) {
        __atomic_add_fetch(&slab_classifier_leaf_payload_bytes, payload,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_classifier_leaf_tables, 1U,
                           __ATOMIC_RELAXED);
    } else {
        panic("slab: invalid classifier metadata kind");
    }
    total = __atomic_add_fetch(&slab_classifier_retained_payload_bytes,
                               payload, __ATOMIC_RELAXED);
    u64 peak = __atomic_load_n(&slab_classifier_peak_payload_bytes,
                               __ATOMIC_RELAXED);
    while (total > peak && !__atomic_compare_exchange_n(
               &slab_classifier_peak_payload_bytes, &peak, total, true,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void *slab_classifier_allocate(usize bytes)
{
    if (!slab_classifier_payload_size(bytes)) {
        __atomic_add_fetch(&slab_classifier_allocation_failures, 1U,
                           __ATOMIC_RELAXED);
        return NULL;
    }
    void *allocation = heap_alloc_raw(bytes);
    if (!allocation)
        __atomic_add_fetch(&slab_classifier_allocation_failures, 1U,
                           __ATOMIC_RELAXED);
    return allocation;
}

static slab_classifier_middle_t *slab_classifier_middle(
    usize root_index, bool create)
{
    if (root_index >= SLAB_CLASSIFIER_ROOT_ENTRIES_MAX)
        panic("slab: classifier root index out of range");
    uintptr_t value = __atomic_load_n(&slab_classifier_root_storage[root_index],
                                      __ATOMIC_ACQUIRE);
    slab_classifier_middle_t *middle = (slab_classifier_middle_t *)value;
    if (middle) {
        slab_classifier_require_metadata(middle, sizeof(*middle),
            "slab: classifier middle node outside mapped heap");
        if (middle->magic != SLAB_CLASSIFIER_MIDDLE_MAGIC ||
            middle->root_index != root_index)
            panic("slab: corrupted classifier middle node");
        return middle;
    }
    if (!create)
        return NULL;

    middle = slab_classifier_allocate(sizeof(*middle));
    if (!middle)
        return NULL;
    middle->magic = SLAB_CLASSIFIER_MIDDLE_MAGIC;
    middle->root_index = root_index;
    for (usize index = 0; index < SLAB_CLASSIFIER_FANOUT; index++)
        __atomic_store_n(&middle->leaves[index], (uintptr_t)0,
                         __ATOMIC_RELAXED);
    uintptr_t expected = 0;
    if (!__atomic_compare_exchange_n(
            &slab_classifier_root_storage[root_index], &expected,
            (uintptr_t)middle, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        heap_free_raw(middle);
        middle = (slab_classifier_middle_t *)expected;
    } else {
        slab_classifier_record_published(1U, sizeof(*middle));
    }
    slab_classifier_require_metadata(middle, sizeof(*middle),
        "slab: published classifier middle node outside mapped heap");
    if (middle->magic != SLAB_CLASSIFIER_MIDDLE_MAGIC ||
        middle->root_index != root_index)
        panic("slab: corrupted published classifier middle node");
    return middle;
}

static slab_classifier_leaf_t *slab_classifier_leaf(
    slab_classifier_middle_t *middle, usize leaf_index, bool create)
{
    usize slot = leaf_index % SLAB_CLASSIFIER_FANOUT;
    uintptr_t value = __atomic_load_n(&middle->leaves[slot],
                                      __ATOMIC_ACQUIRE);
    slab_classifier_leaf_t *leaf = (slab_classifier_leaf_t *)value;
    if (leaf) {
        slab_classifier_require_metadata(leaf, sizeof(*leaf),
            "slab: classifier leaf node outside mapped heap");
        if (leaf->magic != SLAB_CLASSIFIER_LEAF_MAGIC ||
            leaf->leaf_index != leaf_index)
            panic("slab: corrupted classifier leaf node");
        return leaf;
    }
    if (!create)
        return NULL;

    leaf = slab_classifier_allocate(sizeof(*leaf));
    if (!leaf)
        return NULL;
    leaf->magic = SLAB_CLASSIFIER_LEAF_MAGIC;
    leaf->leaf_index = leaf_index;
    for (usize index = 0; index < SLAB_CLASSIFIER_FANOUT; index++)
        __atomic_store_n(&leaf->page_tokens[index], 0U, __ATOMIC_RELAXED);
    uintptr_t expected = 0;
    if (!__atomic_compare_exchange_n(&middle->leaves[slot], &expected,
                                     (uintptr_t)leaf, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        heap_free_raw(leaf);
        leaf = (slab_classifier_leaf_t *)expected;
    } else {
        slab_classifier_record_published(2U, sizeof(*leaf));
    }
    slab_classifier_require_metadata(leaf, sizeof(*leaf),
        "slab: published classifier leaf node outside mapped heap");
    if (leaf->magic != SLAB_CLASSIFIER_LEAF_MAGIC ||
        leaf->leaf_index != leaf_index)
        panic("slab: corrupted published classifier leaf node");
    return leaf;
}

static bool slab_classifier_ensure(usize page_index)
{
    usize root_index = page_index / SLAB_CLASSIFIER_ROOT_PAGES;
    usize leaf_index = page_index / SLAB_CLASSIFIER_FANOUT;
    slab_classifier_middle_t *middle;

    if (page_index >= slab_heap_pages ||
        root_index >= slab_classifier_root_entries)
        return false;
    middle = slab_classifier_middle(root_index, true);
    if (!middle)
        return false;
    return slab_classifier_leaf(middle, leaf_index, true) != NULL;
}

static u64 *slab_page_token_pointer(usize page_index)
{
    usize root_index = page_index / SLAB_CLASSIFIER_ROOT_PAGES;
    usize leaf_index = page_index / SLAB_CLASSIFIER_FANOUT;
    /* The root is a fixed array sized from the full heap reservation.  Avoid
     * reloading a root pointer or checking dynamic root metadata on every hot
     * ownership lookup; child pointers remain untrusted and are range/magic
     * checked before use. */
    if (root_index >= SLAB_CLASSIFIER_ROOT_ENTRIES_MAX)
        panic("slab: classifier root index out of range");
    uintptr_t middle_value = __atomic_load_n(
        &slab_classifier_root_storage[root_index], __ATOMIC_ACQUIRE);
    slab_classifier_middle_t *middle =
        (slab_classifier_middle_t *)middle_value;
    if (!middle)
        return 0;
    slab_classifier_require_metadata(middle, sizeof(*middle),
        "slab: classifier middle node outside mapped heap");
    if (middle->magic != SLAB_CLASSIFIER_MIDDLE_MAGIC ||
        middle->root_index != root_index)
        panic("slab: corrupted classifier middle node");
    slab_classifier_leaf_t *leaf = slab_classifier_leaf(
        middle, leaf_index, false);
    if (!leaf)
        return NULL;
    return &leaf->page_tokens[page_index % SLAB_CLASSIFIER_FANOUT];
}

static u64 slab_page_token_load(usize page_index)
{
    u64 *token = slab_page_token_pointer(page_index);
    return token ? __atomic_load_n(token, __ATOMIC_ACQUIRE) : 0;
}

static void slab_page_token_store(usize page_index, u64 token)
{
    u64 *target = slab_page_token_pointer(page_index);
    if (!target)
        panic("slab: missing classifier leaf during tag update");
    __atomic_store_n(target, token, __ATOMIC_RELEASE);
}

static bool slab_page_token_pin(usize page_index, u32 class_index,
                               u64 generation, u64 *identity_out)
{
    u64 observed = slab_page_token_load(page_index);

    for (;;) {
        if (!slab_token_matches(observed, class_index, generation))
            return false;
        if (slab_token_refs(observed) == SLAB_TOKEN_REFS_MAX)
            return false;
        u64 desired = observed + SLAB_TOKEN_REF_ONE;
        if (__atomic_compare_exchange_n(
                slab_page_token_pointer(page_index), &observed, desired,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (identity_out)
                *identity_out = slab_token_identity(desired);
            return true;
        }
    }
}

static void slab_page_token_unpin(usize page_index, u64 identity)
{
    u64 observed = slab_page_token_load(page_index);

    for (;;) {
        bool identity_matches = (observed & SLAB_TOKEN_RETIRING)
            ? slab_token_generation(observed) ==
                  slab_token_generation(identity) &&
              slab_token_class(observed) == 0U
            : slab_token_identity(observed) == identity;
        if (!identity_matches ||
            !slab_token_refs(observed))
            panic("slab: classifier pin identity or count corrupted");
        u64 desired = observed - SLAB_TOKEN_REF_ONE;
        if (__atomic_compare_exchange_n(
                slab_page_token_pointer(page_index), &observed, desired,
                false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
            return;
    }
}

static bool slab_page_token_retire(usize page_index, u32 class_index,
                                   u64 generation)
{
    u64 observed = slab_page_token_load(page_index);

    for (;;) {
        if (!slab_token_matches(observed, class_index, generation))
            return false;
        u64 desired = (observed & ~((u64)SLAB_CLASSIFIER_CLASS_MASK)) |
                      SLAB_TOKEN_RETIRING;
        if (__atomic_compare_exchange_n(
                slab_page_token_pointer(page_index), &observed, desired,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}

static void slab_page_token_wait_unpinned(usize page_index, u64 generation)
{
    for (u32 iteration = 0; iteration < SLAB_RETIRE_WAIT_LIMIT; iteration++) {
        u64 token = slab_page_token_load(page_index);
        if (slab_token_generation(token) != generation ||
            !(token & SLAB_TOKEN_RETIRING))
            panic("slab: retired page token changed before quiescence");
        if (!slab_token_refs(token))
            return;
        __asm__ volatile("pause" ::: "memory");
    }
    panic("slab: timed out waiting for page ownership pins");
}

static bool slab_page_index(const void *page, usize *index_out)
{
    uintptr_t address = (uintptr_t)page;
    uintptr_t relative;

    if (!index_out || !slab_heap_start || address < slab_heap_start ||
        address >= slab_heap_end || (address & (PAGE_SIZE - 1U)) ||
        !heap_address_is_mapped(page))
        return false;
    relative = address - slab_heap_start;
    if (relative & (PAGE_SIZE - 1U))
        return false;
    usize index = (usize)(relative / PAGE_SIZE);
    if (index >= slab_heap_pages)
        return false;
    *index_out = index;
    return true;
}

static bool slab_pointer_page(const void *pointer, uintptr_t *page_out,
                              usize *index_out)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t page;
    usize index;

    if (!pointer || !slab_heap_start || address < slab_heap_start ||
        address >= slab_heap_end)
        return false;
    page = address & ~((uintptr_t)PAGE_SIZE - 1U);
    if (page < slab_heap_start || page >= slab_heap_end ||
        !slab_page_index((const void *)page, &index))
        return false;
    if (page_out) *page_out = page;
    if (index_out) *index_out = index;
    return true;
}

static u16 slab_slots_for_class(u32 class_index)
{
    if (class_index >= SLAB_CLASS_COUNT)
        return 0;
    return (u16)((PAGE_SIZE - SLAB_OBJECT_AREA_OFFSET) /
                 ((usize)slab_sizes[class_index] + 2U));
}

static u64 slab_integrity_finish(u64 value)
{
    value ^= value >> 29;
    value *= 0x9e3779b185ebca87ULL;
    value ^= value >> 31;
    return value;
}

/* Each bitmap word carries its own bounded corruption check. Hot allocation
 * and free paths validate only words they inspect; slab_verify still checks
 * every word and the complete population count. */
static u32 slab_bitmap_word_integrity(u64 word, u32 word_index)
{
    u64 value = word ^ (SLAB_INTEGRITY_SEED +
                        (u64)word_index * 0x9e3779b97f4a7c15ULL);
    value ^= value >> 29;
    value *= 0x9e3779b185ebca87ULL;
    value ^= value >> 31;
    return (u32)(value ^ (value >> 32));
}

static u64 slab_descriptor_integrity(const slab_descriptor_t *slab)
{
    u64 value = SLAB_INTEGRITY_SEED ^ slab->magic;
    value ^= ((u64)slab->class_index << 32) |
             ((u64)slab->slot_count << 16) | slab->allocated_count;
    value ^= (u64)slab->state << 48;
    value ^= (uintptr_t)slab->partial_prev;
    value = (value << 17) | (value >> (64U - 17U));
    value ^= (uintptr_t)slab->partial_next;
    value ^= (u64)slab->backing_page_count << 9;
    value ^= slab->static_integrity;
    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        value ^= ((u64)slab->bitmap_integrity[word] << 32) |
                 (0x9e3779b9U + word);
        value = (value << 13) | (value >> (64U - 13U));
    }
    return slab_integrity_finish(value);
}

static u64 slab_descriptor_static_integrity(
    const slab_descriptor_t *slab)
{
    u64 value = SLAB_INTEGRITY_SEED ^ slab->magic;
    value ^= ((u64)slab->class_index << 32) | slab->slot_count;
    value ^= (u64)slab->backing_page_count << 19;
    value ^= (uintptr_t)slab;
    return slab_integrity_finish(value);
}

static u32 slab_word_population(u64 word)
{
    word -= (word >> 1) & 0x5555555555555555ULL;
    word = (word & 0x3333333333333333ULL) +
           ((word >> 2) & 0x3333333333333333ULL);
    word = (word + (word >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (u32)((word * 0x0101010101010101ULL) >> 56);
}

static u64 slab_bitmap_word_mask(const slab_descriptor_t *slab, u32 word)
{
    u32 first_slot = word * 64U;
    u32 remaining = slab->slot_count > first_slot
        ? (u32)slab->slot_count - first_slot : 0U;

    return remaining >= 64U ? ~(u64)0 :
        (remaining ? (((u64)1U << remaining) - 1U) : 0U);
}

static bool slab_bitmap_word_valid(const slab_descriptor_t *slab,
                                   u32 word)
{
    if (word >= SLAB_BITMAP_WORDS)
        return false;
    u64 bitmap_word = slab_bitmap_const(slab)[word];
    return !(bitmap_word & ~slab_bitmap_word_mask(slab, word)) &&
           slab->bitmap_integrity[word] ==
               slab_bitmap_word_integrity(bitmap_word, word);
}

static void slab_update_bitmap_word_integrity(slab_descriptor_t *slab,
                                              u32 word)
{
    if (word >= SLAB_BITMAP_WORDS)
        panic("slab: bitmap checksum index out of range");
    slab->bitmap_integrity[word] = slab_bitmap_word_integrity(
        slab_bitmap(slab)[word], word);
}

static bool slab_bitmap_valid(const slab_descriptor_t *slab)
{
    const u64 *bitmap = slab_bitmap_const(slab);
    u32 population = 0;
    u32 occupied = 0;

    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        if (!slab_bitmap_word_valid(slab, word))
            return false;
        population += slab_word_population(bitmap[word]);
    }
    if (population != slab->allocated_count)
        return false;
    const u8 *states = slab_slot_states_const(slab);
    for (u32 slot = 0; slot < slab->slot_count; slot++) {
        u32 state;
        if (!slab_slot_state_decode(
                __atomic_load_n(&states[slot], __ATOMIC_ACQUIRE), &state))
            return false;
        bool bit_set = (bitmap[slot / 64U] &
                        ((u64)1U << (slot % 64U))) != 0U;
        if (bit_set != (state != SLAB_SLOT_SHARED_FREE))
            return false;
        if (state != SLAB_SLOT_SHARED_FREE &&
            slab_slot_owner_load(slab, slot) >= slab_cpu_cache_count)
            return false;
        occupied += state != SLAB_SLOT_SHARED_FREE;
    }
    return occupied == slab->allocated_count;
}

static bool slab_descriptor_address_valid(const slab_descriptor_t *slab,
                                          u32 class_index)
{
    usize page_index;
    if (!slab || class_index >= SLAB_CLASS_COUNT ||
        ((uintptr_t)slab & (PAGE_SIZE - 1U)) ||
        !slab_page_index(slab, &page_index))
        return false;
    u64 token = slab_page_token_load(page_index);
    return slab_token_class(token) == (u8)(class_index + 1U) &&
        slab_token_generation(token) != 0U &&
        !(token & SLAB_TOKEN_RETIRING);
}

static bool slab_descriptor_contents_valid(const slab_descriptor_t *slab,
                                           u32 class_index)
{
    if (slab->magic != SLAB_DESCRIPTOR_MAGIC ||
        slab->class_index != class_index ||
        slab->slot_count != slab_slots_for_class(class_index) ||
        slab->allocated_count > slab->slot_count ||
        slab->backing_page_count != SLAB_BACKING_HEAP_PAGE_COUNT ||
        slab->static_integrity != slab_descriptor_static_integrity(slab) ||
        slab->integrity != slab_descriptor_integrity(slab))
        return false;
    if (!slab->allocated_count)
        return slab->state == SLAB_STATE_EMPTY && !slab->partial_prev &&
               !slab->partial_next;
    if (slab->allocated_count == slab->slot_count) {
        if (slab->state != SLAB_STATE_FULL || slab->partial_prev ||
            slab->partial_next)
            return false;
    } else if (slab->state != SLAB_STATE_PARTIAL) {
        return false;
    }
    return true;
}

static bool slab_descriptor_fast_valid(const slab_descriptor_t *slab,
                                       u32 class_index)
{
    return slab && class_index < SLAB_CLASS_COUNT &&
           slab->magic == SLAB_DESCRIPTOR_MAGIC &&
           slab->class_index == class_index &&
           slab->slot_count == slab_slots_for_class(class_index) &&
           slab->backing_page_count == SLAB_BACKING_HEAP_PAGE_COUNT &&
           slab->static_integrity == slab_descriptor_static_integrity(slab);
}

static bool slab_descriptor_fields_valid(const slab_descriptor_t *slab,
                                         u32 class_index)
{
    return slab_descriptor_address_valid(slab, class_index) &&
           slab_descriptor_contents_valid(slab, class_index);
}

/* The caller has validated this descriptor's page token while holding its
 * class lock. The ordinary path checks only state that it must trust for the
 * operation. In particular, a non-transitioning update does not dereference
 * list neighbors. */
static bool slab_descriptor_membership_valid_locked_classified(
    const slab_descriptor_t *slab, u32 class_index)
{
    if (!slab_descriptor_contents_valid(slab, class_index))
        return false;
    if (slab->state == SLAB_STATE_EMPTY)
        return slab_classes[class_index].empty_slab == slab;
    if (slab->state == SLAB_STATE_PARTIAL) {
        if (slab->partial_prev == slab || slab->partial_next == slab ||
            (slab->partial_prev &&
             slab->partial_prev == slab->partial_next))
            return false;
        if (!slab->partial_prev)
            return slab_classes[class_index].partial_head == slab;
        if (slab_classes[class_index].partial_head == slab)
            return false;
    }
    return true;
}

/* Deep mode checks neighboring descriptors on every touched partial slab.
 * Each neighbor's address and live class token are checked before its fields
 * are read. The ordinary mode performs this validation only when a list
 * transition is about to dereference and modify a neighbor. */
#if SLAB_DEEP_VALIDATION
static bool slab_partial_neighbors_valid_locked_classified(
    const slab_descriptor_t *slab, u32 class_index)
{
    if (slab->state != SLAB_STATE_PARTIAL)
        return true;
    if (slab->partial_prev) {
        const slab_descriptor_t *previous = slab->partial_prev;
        if (!slab_descriptor_fields_valid(previous, class_index) ||
            previous->state != SLAB_STATE_PARTIAL ||
            previous->partial_next != slab)
            return false;
    } else if (slab_classes[class_index].partial_head != slab) {
        return false;
    }
    if (slab->partial_next) {
        const slab_descriptor_t *next = slab->partial_next;
        if (!slab_descriptor_fields_valid(next, class_index) ||
            next->state != SLAB_STATE_PARTIAL ||
            next->partial_prev != slab)
            return false;
    }
    return true;
}
#endif

static bool slab_descriptor_valid_locked_classified(
    const slab_descriptor_t *slab, u32 class_index)
{
    if (!slab_descriptor_membership_valid_locked_classified(slab,
                                                             class_index))
        return false;
#if SLAB_DEEP_VALIDATION
    return slab_bitmap_valid(slab) &&
           slab_partial_neighbors_valid_locked_classified(slab, class_index);
#else
    return true;
#endif
}

static bool slab_descriptor_valid_locked(const slab_descriptor_t *slab,
                                         u32 class_index)
{
    return slab_descriptor_address_valid(slab, class_index) &&
           slab_descriptor_valid_locked_classified(slab, class_index);
}

static void slab_require_valid(bool valid, const char *reason)
{
    if (!valid)
        panic(reason);
}

static void slab_update_integrity(slab_descriptor_t *slab)
{
    slab->integrity = slab_descriptor_integrity(slab);
}

static void slab_validate_after_mutation_locked(
    const slab_descriptor_t *slab, u32 class_index, const char *reason)
{
#if SLAB_DEEP_VALIDATION
    slab_require_valid(slab_descriptor_valid_locked(slab, class_index),
                       reason);
#else
    (void)slab;
    (void)class_index;
    (void)reason;
#endif
}

static void slab_list_insert_locked(u32 class_index,
                                    slab_descriptor_t *slab)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *head = class_state->partial_head;

    slab_require_valid(slab->state == SLAB_STATE_PARTIAL &&
                       !slab->partial_prev && !slab->partial_next &&
                       head != slab,
                       "slab: duplicate or invalid partial-list insertion");
    if (head) {
        slab_require_valid(slab_descriptor_valid_locked(head, class_index) &&
                           head->state == SLAB_STATE_PARTIAL &&
                           !head->partial_prev,
                           "slab: invalid partial-list head");
        head->partial_prev = slab;
        slab_update_integrity(head);
    }
    slab->partial_next = head;
    class_state->partial_head = slab;
    slab_update_integrity(slab);
}

static void slab_list_remove_locked(u32 class_index,
                                    slab_descriptor_t *slab)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *previous = slab->partial_prev;
    slab_descriptor_t *next = slab->partial_next;

    /* Validate every neighbor before changing any link. The pointer is first
     * range-checked and classified; only then is its descriptor read. This is
     * mandatory even in normal mode because removal dereferences and mutates
     * adjacent slab metadata. */
    slab_require_valid(slab->state == SLAB_STATE_PARTIAL,
                       "slab: invalid partial-list removal state");
    slab_require_valid(slab->partial_prev != slab &&
                       slab->partial_next != slab &&
                       (!previous || !next || previous != next),
                       "slab: cyclic partial-list links during removal");
    if (previous) {
        slab_require_valid(class_state->partial_head != slab &&
                           slab_descriptor_fields_valid(previous,
                                                         class_index) &&
                           previous->state == SLAB_STATE_PARTIAL &&
                           previous->partial_next == slab,
                           "slab: partial predecessor changed during removal");
    } else {
        slab_require_valid(class_state->partial_head == slab,
                           "slab: partial slab missing from class list");
    }
    if (next) {
        slab_require_valid(slab_descriptor_fields_valid(next, class_index) &&
                           next->state == SLAB_STATE_PARTIAL &&
                           next->partial_prev == slab,
                           "slab: partial successor changed during removal");
    }

    if (previous) {
        previous->partial_next = next;
        slab_update_integrity(previous);
    } else {
        class_state->partial_head = next;
    }
    if (next) {
        next->partial_prev = previous;
        slab_update_integrity(next);
    }
    slab->partial_prev = NULL;
    slab->partial_next = NULL;
    slab_update_integrity(slab);
}

/* Caller holds this class lock.  At most four words are inspected. */
static bool slab_find_free_slot_locked(slab_descriptor_t *slab,
                                       u32 class_index, u16 *slot_out)
{
    u64 *bitmap = slab_bitmap(slab);
    u32 slots = slab->slot_count;

    if (!slots || slots > SLAB_BITMAP_WORDS * 64U)
        panic("slab: invalid slot count during bitmap search");

    if (slots <= 16U) {
        /* These classes fit in the first bitmap word.  A short bounded scan
         * avoids paying for the bit-scan instruction when the free slot is
         * usually among the first few positions. */
        u64 valid_mask = ((u64)1U << slots) - 1U;
        if (!slab_bitmap_word_valid(slab, 0U))
            panic("slab: corrupted bitmap word during allocation");
        u64 available = ~bitmap[0] & valid_mask;
        slab_classes[class_index].stats.bitmap_words_examined++;
        if (!available)
            return false;
        u32 bit = 0U;
        while (!(available & ((u64)1U << bit)))
            bit++;
        *slot_out = (u16)bit;
        return true;
    }

    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        u32 first_slot = word * 64U;
        u64 valid_mask = slab_bitmap_word_mask(slab, word);
        u64 available;
        if (!slab_bitmap_word_valid(slab, word))
            panic("slab: corrupted bitmap word during allocation");
        slab_classes[class_index].stats.bitmap_words_examined++;
        available = ~bitmap[word] & valid_mask;
        if (available) {
            /* available is nonzero, so the builtin is defined and lowers to
             * one x86 bit-scan instruction instead of a per-bit branch loop. */
            u32 bit = (u32)__builtin_ctzll(
                (unsigned long long)available);
            *slot_out = (u16)(first_slot + bit);
            return true;
        }
    }
    return false;
}

static void slab_record_live_locked(slab_class_t *class_state)
{
    u64 live = __atomic_add_fetch(&class_state->stats.live_slots, 1U,
                                  __ATOMIC_RELAXED);
    u64 peak = __atomic_load_n(&class_state->stats.peak_live_slots,
                               __ATOMIC_RELAXED);
    while (live > peak && !__atomic_compare_exchange_n(
               &class_state->stats.peak_live_slots, &peak, live, false,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    __atomic_add_fetch(&class_state->stats.allocations, 1U,
                       __ATOMIC_RELAXED);
}

static void slab_record_live_free(slab_class_t *class_state)
{
    u64 old = __atomic_fetch_sub(&class_state->stats.live_slots, 1U,
                                 __ATOMIC_RELAXED);
    slab_require_valid(old != 0U, "slab: live-slot counter underflow");
    __atomic_add_fetch(&class_state->stats.frees, 1U, __ATOMIC_RELAXED);
}

static slab_cpu_cache_t *slab_cpu_cache(u32 cpu_index, u32 class_index)
{
    if (!slab_cpu_caches || cpu_index >= slab_cpu_cache_count ||
        class_index >= SLAB_CLASS_COUNT)
        return NULL;
    return &slab_cpu_caches[(usize)cpu_index * SLAB_CLASS_COUNT +
                            class_index];
}

static void slab_cache_metadata_valid_locked(
    const slab_cpu_cache_t *cache)
{
    slab_require_valid(cache &&
                       cache->count <= SLAB_LOCAL_CACHE_CAPACITY &&
                       cache->refill_active <= 1U,
                       "slab: corrupted per-CPU cache metadata");
}

/* Selecting the cache before irqsave is safe only if the caller verifies that
 * it did not migrate between reading GS and disabling local interrupts. */
static bool slab_local_cache_lock(u32 class_index,
                                  slab_cpu_cache_t **cache_out,
                                  u32 *cpu_out, u64 *flags_out)
{
    for (u32 attempt = 0; attempt < SLAB_CPU_CACHE_LOCK_RETRIES; attempt++) {
        u32 cpu_index = cpu_current_index();
        slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index, class_index);
        if (!cache)
            return false;
        u64 flags = spin_lock_irqsave(&cache->lock);
        if (cpu_current_index() == cpu_index) {
            slab_cache_metadata_valid_locked(cache);
            *cache_out = cache;
            *cpu_out = cpu_index;
            *flags_out = flags;
            return true;
        }
        spin_unlock_irqrestore(&cache->lock, flags);
    }
    return false;
}

static void *slab_take_existing_locked(u32 class_index, u32 slot_state)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *slab = class_state->partial_head;
    bool was_empty = false;
    u16 slot;
    u64 *bitmap;
    u32 old_count;

    if (!slab) {
        slab = class_state->empty_slab;
        if (!slab)
            return NULL;
        slab_require_valid(slab_descriptor_valid_locked(slab, class_index) &&
                           slab->state == SLAB_STATE_EMPTY &&
                           !slab->allocated_count && !slab->partial_prev &&
                           !slab->partial_next && slab_bitmap_valid(slab),
                           "slab: corrupted empty reserve");
        was_empty = true;
    } else {
        slab_require_valid(slab_descriptor_valid_locked(slab, class_index) &&
                           slab->state == SLAB_STATE_PARTIAL &&
                           !slab->partial_prev,
                           "slab: corrupted partial-list head");
    }
    if (!slab_find_free_slot_locked(slab, class_index, &slot))
        panic("slab: partial slab has no free slot");

    old_count = slab->allocated_count;
    if (!was_empty && old_count + 1U == slab->slot_count) {
        slab_require_valid(class_state->stats.partial_slabs != 0,
                           "slab: partial-slab counter underflow");
        slab_list_remove_locked(class_index, slab);
    }
    if (was_empty) {
        slab_require_valid(class_state->stats.empty_slabs == 1U &&
                           class_state->empty_slab == slab,
                           "slab: empty reserve ownership changed");
        class_state->empty_slab = NULL;
        class_state->stats.empty_slabs--;
        class_state->stats.empty_slab_reuses++;
    }
    bitmap = slab_bitmap(slab);
    slab_require_valid(slab_slot_state_load(slab, slot) ==
                           SLAB_SLOT_SHARED_FREE,
                       "slab: occupied bitmap has a nonfree slot state");
    bitmap[slot / 64U] |= (u64)1U << (slot % 64U);
    slab_update_bitmap_word_integrity(slab, slot / 64U);
    slab_slot_owner_store(slab, slot, cpu_current_index());
    slab_require_valid(slab_slot_state_change(slab, slot,
                                              SLAB_SLOT_SHARED_FREE,
                                              slot_state),
                       "slab: shared allocation slot state changed");
    slab->allocated_count++;
    if (slot_state == SLAB_SLOT_CLIENT)
        slab_record_live_locked(class_state);
    if (slab->allocated_count == slab->slot_count) {
        if (was_empty)
            panic("slab: empty reserve unexpectedly filled in one allocation");
        slab->state = SLAB_STATE_FULL;
        class_state->stats.partial_slabs--;
        class_state->stats.full_slabs++;
        class_state->stats.transitions_to_full++;
    } else if (was_empty) {
        slab->state = SLAB_STATE_PARTIAL;
        slab_update_integrity(slab);
        slab_list_insert_locked(class_index, slab);
        class_state->stats.partial_slabs++;
    } else if (slab->allocated_count <= old_count) {
        panic("slab: allocation count did not advance");
    }
    slab_update_integrity(slab);
    slab_validate_after_mutation_locked(slab, class_index,
                                        "slab: invalid descriptor after allocation");
    return (u8 *)slab + SLAB_OBJECT_AREA_OFFSET +
           (usize)slot * slab_sizes[class_index];
}

bool slab_init(void *heap_start, usize heap_capacity)
{
    uintptr_t start = (uintptr_t)heap_start;
    uintptr_t end;
    u32 expected = 0U;

    if (!heap_start || (start & (PAGE_SIZE - 1U)) || !heap_capacity ||
        (heap_capacity & (PAGE_SIZE - 1U)) ||
        heap_capacity > (usize)(KERNEL_HEAP_LIMIT - KERNEL_HEAP_BASE) ||
        start > (uintptr_t)-1 - heap_capacity)
        return false;
    end = start + heap_capacity;
    if (!__atomic_compare_exchange_n(&slab_initialization_state, &expected,
                                     1U, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return false;

    u32 cache_count = cpu_count();
    usize cache_records;
    if (!cache_count || cache_count > CPU_MAX_COUNT) {
        __atomic_store_n(&slab_initialization_state, 0U, __ATOMIC_RELEASE);
        return false;
    }
    cache_records = (usize)cache_count * SLAB_CLASS_COUNT;
    if (cache_records > ((usize)-1 - 63U) / sizeof(slab_cpu_cache_t)) {
        __atomic_store_n(&slab_initialization_state, 0U, __ATOMIC_RELEASE);
        return false;
    }
    usize cache_bytes = cache_records * sizeof(slab_cpu_cache_t);
    void *cache_allocation = heap_alloc_raw(cache_bytes + 63U);
    if (!cache_allocation) {
        __atomic_store_n(&slab_initialization_state, 0U, __ATOMIC_RELEASE);
        return false;
    }
    uintptr_t cache_aligned = ((uintptr_t)cache_allocation + 63U) &
                              ~((uintptr_t)63U);
    slab_cpu_cache_allocation = cache_allocation;
    slab_cpu_cache_payload_bytes = cache_bytes;
    slab_cpu_caches = (slab_cpu_cache_t *)cache_aligned;
    slab_cpu_cache_count = cache_count;
    __atomic_store_n(&slab_reclaim_cursor, 0U, __ATOMIC_RELAXED);
    for (usize index = 0; index < cache_records; index++) {
        slab_cpu_cache_t *cache = &slab_cpu_caches[index];
        *cache = (slab_cpu_cache_t){0};
        spinlock_init(&cache->lock);
    }

    slab_heap_start = start;
    slab_heap_end = end;
    slab_heap_pages = heap_capacity / PAGE_SIZE;
    slab_classifier_root_entries = slab_heap_pages /
        SLAB_CLASSIFIER_ROOT_PAGES +
        (slab_heap_pages % SLAB_CLASSIFIER_ROOT_PAGES != 0U);
    if (!slab_classifier_root_entries ||
        slab_classifier_root_entries > SLAB_CLASSIFIER_ROOT_ENTRIES_MAX) {
        heap_free_raw(slab_cpu_cache_allocation);
        slab_cpu_cache_allocation = NULL;
        slab_cpu_cache_payload_bytes = 0U;
        slab_cpu_caches = NULL;
        slab_cpu_cache_count = 0U;
        __atomic_store_n(&slab_initialization_state, 0U, __ATOMIC_RELEASE);
        return false;
    }
    for (usize index = 0; index < SLAB_CLASSIFIER_ROOT_ENTRIES_MAX; index++)
        __atomic_store_n(&slab_classifier_root_storage[index],
                         (uintptr_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&slab_classifier_static_root_bytes,
                     sizeof(slab_classifier_root_storage),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&slab_classifier_root_tables, 1U, __ATOMIC_RELAXED);
    for (u32 index = 0; index < SLAB_CLASS_COUNT; index++) {
        spinlock_init(&slab_classes[index].lock);
        slab_classes[index].partial_head = NULL;
        slab_classes[index].empty_slab = NULL;
        slab_classes[index].stats = (slab_class_stats_t){0};
    }
    __atomic_store_n(&slab_initialization_state, 2U, __ATOMIC_RELEASE);
    return true;
}

bool slab_is_initialized(void)
{
    return __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) ==
           2U;
}

static bool slab_select_class(usize size, u32 *class_index)
{
    if (!size || size > SLAB_MAX_OBJECT_SIZE || !class_index)
        return false;
    for (u32 index = 0; index < SLAB_CLASS_COUNT; index++) {
        if (size <= slab_sizes[index]) {
            *class_index = index;
            return true;
        }
    }
    return false;
}

static void slab_backing_release(heap_page_extent_t *extent,
                                 const char *reason)
{
    slab_require_valid(extent && extent->base &&
                       extent->page_count == SLAB_BACKING_HEAP_PAGE_COUNT &&
                       !((uintptr_t)extent->base & (PAGE_SIZE - 1U)),
                       reason);
    heap_free_page_extent(extent);
    slab_require_valid(!extent->base && !extent->page_count,
                       "slab: heap did not consume backing extent");
}

static void slab_backing_release_descriptor(slab_descriptor_t *slab,
                                            const char *reason)
{
    heap_page_extent_t extent = {
        .base = slab,
        .page_count = slab ? slab->backing_page_count : 0U,
    };
    slab_backing_release(&extent, reason);
}

static void slab_initialize_descriptor(slab_descriptor_t *slab,
                                       u32 class_index,
                                       const heap_page_extent_t *backing)
{
    slab_require_valid(backing && backing->base == slab &&
                       backing->page_count == SLAB_BACKING_HEAP_PAGE_COUNT &&
                       !((uintptr_t)backing->base & (PAGE_SIZE - 1U)),
                       "slab: invalid heap backing extent during initialization");
    *slab = (slab_descriptor_t){
        .magic = SLAB_DESCRIPTOR_MAGIC,
        .class_index = class_index,
        .slot_count = slab_slots_for_class(class_index),
        .allocated_count = 0,
        .state = SLAB_STATE_PARTIAL,
        .partial_prev = NULL,
        .partial_next = NULL,
        .integrity = 0,
        .backing_page_count = (u32)backing->page_count,
        .bitmap_integrity = {0},
        .static_integrity = 0,
    };
    u64 *bitmap = slab_bitmap(slab);
    u8 *states = slab_slot_states(slab);
    u8 *owners = slab_slot_owners(slab);
    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        bitmap[word] = 0;
        slab_update_bitmap_word_integrity(slab, word);
    }
    for (u32 slot = 0; slot < slab->slot_count; slot++)
        __atomic_store_n(&states[slot],
                         slab_slot_state_encode(SLAB_SLOT_SHARED_FREE),
                         __ATOMIC_RELAXED);
    for (u32 slot = 0; slot < slab->slot_count; slot++)
        __atomic_store_n(&owners[slot], 0U, __ATOMIC_RELAXED);
    slab->static_integrity = slab_descriptor_static_integrity(slab);
    slab_update_integrity(slab);
}

static void *slab_publish_new_locked(u32 class_index, void *backing,
                                    u32 slot_state)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *slab = (slab_descriptor_t *)backing;
    usize page_index;
    u16 slot = 0;
    u64 old_token;
    u64 generation;

    slab_require_valid(slab_page_index(backing, &page_index),
                       "slab: backing page outside mapped heap");
    old_token = slab_page_token_load(page_index);
    slab_require_valid(slab_token_class(old_token) == 0U,
                       "slab: backing page already classified");
    slab_require_valid(!slab_token_refs(old_token),
                       "slab: backing page has outstanding ownership pins");
    slab_require_valid(slab->magic == SLAB_DESCRIPTOR_MAGIC &&
                       slab->class_index == class_index &&
                       slab->slot_count == slab_slots_for_class(class_index) &&
                       !slab->allocated_count &&
                       slab->state == SLAB_STATE_PARTIAL &&
                       !slab->partial_prev && !slab->partial_next &&
                       slab->backing_page_count ==
                           SLAB_BACKING_HEAP_PAGE_COUNT &&
                       slab->integrity == slab_descriptor_integrity(slab),
                       "slab: invalid unpublished descriptor header");
    slab_require_valid(slab_bitmap_valid(slab),
                       "slab: invalid unpublished descriptor bitmap");
    slab_bitmap(slab)[0] = 1U;
    slab_update_bitmap_word_integrity(slab, 0U);
    slab_slot_owner_store(slab, 0U, cpu_current_index());
    slab_require_valid(slab_slot_state_change(slab, 0U,
                                              SLAB_SLOT_SHARED_FREE,
                                              slot_state),
                       "slab: new backing slot state changed");
    slab->allocated_count = 1U;
    slab_update_integrity(slab);
    generation = slab_token_generation(old_token);
    if (generation == SLAB_TOKEN_GENERATION_MASK)
        panic("slab: page classification generation exhausted");
    generation++;
    slab_page_token_store(page_index,
        (generation << SLAB_CLASSIFIER_CLASS_BITS) | (class_index + 1U));
    slab_list_insert_locked(class_index, slab);
    class_state->stats.partial_slabs++;
    class_state->stats.backing_allocations++;
#if defined(SLAB_CORE_DIAGNOSTICS) || \
    defined(HEAP_PROFILE_HYBRID_DIAGNOSTICS)
    u64 slab_pages = class_state->stats.partial_slabs +
                     class_state->stats.full_slabs +
                     class_state->stats.empty_slabs;
    if (slab_pages > class_state->stats.peak_slab_pages)
        class_state->stats.peak_slab_pages = slab_pages;
#endif
    if (slot_state == SLAB_SLOT_CLIENT)
        slab_record_live_locked(class_state);
    slab_validate_after_mutation_locked(slab, class_index,
                                        "slab: invalid descriptor after publication");
    return (u8 *)slab + SLAB_OBJECT_AREA_OFFSET +
           (usize)slot * slab_sizes[class_index];
}

static void *slab_alloc_state(usize size, u32 slot_state,
                              u64 *token_identity_out)
{
    u32 class_index;
    slab_class_t *class_state;
    heap_page_extent_t backing_extent = {0};
    u64 flags;

    if (__atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return NULL;
    if (!slab_select_class(size, &class_index) ||
        (slot_state != SLAB_SLOT_CLIENT &&
         slot_state != SLAB_SLOT_REFILL))
        return NULL;
    class_state = &slab_classes[class_index];

    flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    void *object = slab_take_existing_locked(class_index, slot_state);
    if (object) {
        if (token_identity_out) {
            uintptr_t page;
            usize page_index;
            slab_require_valid(slab_pointer_page(object, &page, &page_index),
                               "slab: allocation outside slab heap");
            *token_identity_out = slab_token_identity(
                slab_page_token_load(page_index));
        }
        spin_unlock_irqrestore(&class_state->lock, flags);
        return object;
    }
    spin_unlock_irqrestore(&class_state->lock, flags);

    /* Backing allocation may grow the indexed heap and therefore must run
     * without a class lock held. */
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    bool profile_backing_operation = heap_profile_hybrid_enter();
    u32 backing_failures = 0U;
#endif
    bool backing_acquired = heap_alloc_page_extent(
        SLAB_BACKING_HEAP_PAGE_COUNT, &backing_extent);
    if (!backing_acquired) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        backing_failures++;
#endif
        slab_reclaim_result_t reclaim = {0};
        bool reclaimed = slab_reclaim_bounded(
            SLAB_RECLAIM_PRESSURE_CACHE_RECORDS,
            SLAB_RECLAIM_PRESSURE_OBJECTS, &reclaim);
        if (reclaimed && (reclaim.slab_pages_released ||
                          reclaim.empty_slab_pages_released)) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
            heap_profile_hybrid_reclaim_retry(profile_backing_operation);
#endif
            backing_acquired = heap_alloc_page_extent(
                SLAB_BACKING_HEAP_PAGE_COUNT, &backing_extent);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
            if (!backing_acquired)
                backing_failures++;
#endif
        }
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_reclaim_complete(
            profile_backing_operation, reclaimed,
            reclaim.cached_objects_returned, reclaim.slab_pages_released,
            reclaim.empty_slab_pages_released);
#endif
    }
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    heap_profile_hybrid_backing_allocation_complete(
        profile_backing_operation, backing_failures);
#endif
    if (!backing_acquired) {
        flags = spin_lock_irqsave(&class_state->lock);
        class_state->stats.lock_acquisitions++;
        object = slab_take_existing_locked(class_index, slot_state);
        if (!object) class_state->stats.allocation_failures++;
        if (object && token_identity_out) {
            uintptr_t page;
            usize page_index;
            slab_require_valid(slab_pointer_page(object, &page, &page_index),
                               "slab: allocation outside slab heap");
            *token_identity_out = slab_token_identity(
                slab_page_token_load(page_index));
        }
        spin_unlock_irqrestore(&class_state->lock, flags);
        return object;
    }
    usize backing_page_index;
    void *backing = backing_extent.base;
    slab_require_valid(backing_extent.page_count ==
                           SLAB_BACKING_HEAP_PAGE_COUNT &&
                       slab_page_index(backing, &backing_page_index),
                       "slab: heap returned invalid page extent");
    if (!slab_classifier_ensure(backing_page_index)) {
        slab_backing_release(&backing_extent,
                             "slab: invalid backing after classifier failure");
        flags = spin_lock_irqsave(&class_state->lock);
        class_state->stats.lock_acquisitions++;
        object = slab_take_existing_locked(class_index, slot_state);
        if (!object) class_state->stats.allocation_failures++;
        if (object && token_identity_out) {
            uintptr_t page;
            usize page_index;
            slab_require_valid(slab_pointer_page(object, &page, &page_index),
                               "slab: allocation outside slab heap");
            *token_identity_out = slab_token_identity(
                slab_page_token_load(page_index));
        }
        spin_unlock_irqrestore(&class_state->lock, flags);
        return object;
    }
    slab_require_valid(slab_token_class(
                           slab_page_token_load(backing_page_index)) == 0U,
                       "slab: heap returned classified slab backing");
    /* This heap block is exclusively owned.  Prepare metadata before taking
     * the class lock; publish only after rechecking the partial list. */
    slab_initialize_descriptor((slab_descriptor_t *)backing, class_index,
                                &backing_extent);

    flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    object = slab_take_existing_locked(class_index, slot_state);
    bool release_backing = object != NULL;
    if (!release_backing)
        object = slab_publish_new_locked(class_index, backing,
                                         slot_state);
    else
        class_state->stats.backing_allocations++;
    if (release_backing) {
        class_state->stats.backing_releases++;
        if (token_identity_out) {
            uintptr_t page;
            usize page_index;
            slab_require_valid(slab_pointer_page(object, &page, &page_index),
                               "slab: allocation outside slab heap");
            *token_identity_out = slab_token_identity(
                slab_page_token_load(page_index));
        }
        spin_unlock_irqrestore(&class_state->lock, flags);
        slab_backing_release(&backing_extent,
                             "slab: invalid unused backing extent");
        return object;
    }
    slab_require_valid(object != NULL,
                       "slab: published backing returned no object");
    /* Publication transferred exclusive ownership from this temporary
     * acquisition handle to the descriptor and classifier token. */
    backing_extent.base = NULL;
    backing_extent.page_count = 0U;
    if (token_identity_out) {
        uintptr_t page;
        usize page_index;
        slab_require_valid(slab_pointer_page(object, &page, &page_index),
                           "slab: allocation outside slab heap");
        *token_identity_out = slab_token_identity(
            slab_page_token_load(page_index));
    }
    spin_unlock_irqrestore(&class_state->lock, flags);
    return object;
}

void *slab_alloc(usize size)
{
    return slab_alloc_state(size, SLAB_SLOT_CLIENT, NULL);
}

/* The caller holds the class lock and a classifier pin. The descriptor is
 * stable until that pin is dropped; this helper is the only transition from
 * cache/caller ownership back to the shared bitmap. */
static bool slab_pointer_slot(uintptr_t page, uintptr_t address,
                              u32 class_index,
                              const slab_descriptor_t *slab, u16 *slot_out);

static bool slab_release_slot_locked(u32 class_index, uintptr_t page,
                                     usize page_index,
                                     slab_descriptor_t *slab, void *ptr,
                                     u32 expected_state, u64 identity,
                                     u32 owner_cpu, bool *release_backing,
                                     u64 *generation_out,
                                     void *profile_context,
                                     u32 *profile_case_out)
{
    slab_class_t *class_state = &slab_classes[class_index];
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_call_t *profile_call = profile_context;
    slab_free_profile_interval_begin(profile_call,
                                     SLAB_FREE_PROFILE_VALIDATE);
#else
    (void)profile_context;
#endif
    u64 token = slab_page_token_load(page_index);
    if (slab_token_identity(token) != identity ||
        (token & SLAB_TOKEN_RETIRING) ||
        !slab_descriptor_valid_locked_classified(slab, class_index))
        panic("slab: stale ownership or corrupted descriptor during release");
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_interval_end(profile_call,
                                   SLAB_FREE_PROFILE_VALIDATE);
    slab_free_profile_interval_begin(profile_call, SLAB_FREE_PROFILE_SLOT);
#endif
    uintptr_t object_start = page + SLAB_OBJECT_AREA_OFFSET;
    u16 slot;
    slab_require_valid(slab_pointer_slot(page, (uintptr_t)ptr, class_index,
                                         slab, &slot),
                       "slab: invalid slot during release");
    slab_require_valid((uintptr_t)ptr >= object_start,
                       "slab: object precedes slot area");
    u32 owner = slab_slot_owner_load(slab, slot);
    slab_require_valid(owner < slab_cpu_cache_count,
                       "slab: corrupted slot owner during release");
    u64 *bitmap = slab_bitmap(slab);
    u32 word = slot / 64U;
    u64 mask = (u64)1U << (slot % 64U);
    slab_require_valid(slab->allocated_count &&
                       (slab->state == SLAB_STATE_PARTIAL ||
                        slab->state == SLAB_STATE_FULL),
                       "slab: invalid occupancy during release");
    bool was_full = slab->state == SLAB_STATE_FULL;
    bool will_empty = slab->allocated_count == 1U;
    slab_require_valid(slab_bitmap_word_valid(slab, word) &&
                       (bitmap[word] & mask),
                       "slab: free slot has inconsistent bitmap state");
    if (will_empty)
        slab_require_valid(slab_bitmap_valid(slab),
                           "slab: inconsistent bitmap before retirement");
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_interval_end(profile_call, SLAB_FREE_PROFILE_SLOT);
    slab_free_profile_interval_begin(profile_call, SLAB_FREE_PROFILE_MUTATE);
    if (profile_case_out)
        *profile_case_out = will_empty ? SLAB_FREE_PROFILE_FINAL :
            (was_full ? SLAB_FREE_PROFILE_FULL_TO_PARTIAL :
                        SLAB_FREE_PROFILE_NONFINAL);
#else
    (void)profile_case_out;
#endif
    slab_require_valid(slab_slot_state_change(slab, slot, expected_state,
                                              SLAB_SLOT_SHARED_FREE),
                       expected_state == SLAB_SLOT_CLIENT
                           ? "slab: duplicate or unallocated object free"
                           : "slab: cached slot ownership changed");

    if (will_empty)
        slab_list_remove_locked(class_index, slab);

    bitmap[word] &= ~mask;
    slab_update_bitmap_word_integrity(slab, word);
    slab_slot_owner_store(slab, slot, 0U);
    slab->allocated_count--;
    if (expected_state == SLAB_SLOT_CLIENT) {
        slab_record_live_free(class_state);
        if (owner_cpu != owner)
            __atomic_add_fetch(&class_state->stats.cross_cpu_frees, 1U,
                               __ATOMIC_RELAXED);
    } else if (expected_state == SLAB_SLOT_CACHED) {
        u64 old = __atomic_fetch_sub(&class_state->stats.cached_slots, 1U,
                                     __ATOMIC_RELAXED);
        slab_require_valid(old != 0U, "slab: cached-slot counter underflow");
        __atomic_add_fetch(&class_state->stats.cache_drain_objects, 1U,
                           __ATOMIC_RELAXED);
    }

    if (will_empty) {
        slab->state = SLAB_STATE_EMPTY;
        slab_update_integrity(slab);
        slab_require_valid(class_state->stats.partial_slabs != 0U,
                           "slab: partial-slab counter underflow");
        class_state->stats.partial_slabs--;
#if SLAB_EMPTY_RESERVE_PER_CLASS
        if (!class_state->empty_slab) {
            class_state->empty_slab = slab;
            class_state->stats.empty_slabs++;
            *release_backing = false;
            slab_validate_after_mutation_locked(
                slab, class_index, "slab: invalid empty reserve");
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
            slab_free_profile_interval_end(profile_call,
                                           SLAB_FREE_PROFILE_MUTATE);
#endif
            return true;
        }
#endif
        slab_require_valid(slab_page_token_retire(page_index, class_index,
                           slab_token_generation(token)),
                           "slab: failed to withdraw retiring classification");
        class_state->stats.retired_slabs++;
        class_state->stats.backing_releases++;
        *release_backing = true;
        *generation_out = slab_token_generation(token);
    } else if (was_full) {
        slab->state = SLAB_STATE_PARTIAL;
        slab_update_integrity(slab);
        slab_list_insert_locked(class_index, slab);
        slab_require_valid(class_state->stats.full_slabs != 0U,
                           "slab: full-slab counter underflow");
        class_state->stats.full_slabs--;
        class_state->stats.partial_slabs++;
        class_state->stats.transitions_to_partial++;
    } else {
        slab->state = SLAB_STATE_PARTIAL;
        slab_update_integrity(slab);
    }
    if (!*release_backing)
        slab_validate_after_mutation_locked(
            slab, class_index, "slab: invalid descriptor after release");
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_interval_end(profile_call,
                                   SLAB_FREE_PROFILE_MUTATE);
#endif
    return true;
}

static void slab_finish_retirement(uintptr_t page, usize page_index,
                                   u64 identity, u64 generation)
{
    slab_page_token_unpin(page_index, identity);
    slab_page_token_wait_unpinned(page_index, generation);
    slab_backing_release_descriptor((slab_descriptor_t *)page,
                                    "slab: invalid retired backing extent");
}

static bool slab_pointer_slot(uintptr_t page, uintptr_t address,
                              u32 class_index,
                              const slab_descriptor_t *slab, u16 *slot_out)
{
    uintptr_t object_start = page + SLAB_OBJECT_AREA_OFFSET;
    if (address < object_start)
        return false;
    usize offset = address - object_start;
    usize size = slab_sizes[class_index];
    usize index = offset / size;
    if (offset != index * size || index >= slab->slot_count)
        return false;
    *slot_out = (u16)index;
    return true;
}

static bool slab_free_pinned(void *ptr, uintptr_t page, usize page_index,
                             u32 class_index, u64 identity,
                             void *profile_context)
{
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_call_t *profile_call = profile_context;
    u32 profile_case = 0U;
#else
    (void)profile_context;
#endif
    slab_class_t *class_state = &slab_classes[class_index];
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_interval_begin(profile_call, SLAB_FREE_PROFILE_LOCK);
#endif
    u64 flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_interval_end(profile_call, SLAB_FREE_PROFILE_LOCK);
#endif
    slab_descriptor_t *slab = (slab_descriptor_t *)page;
    bool release = false;
    u64 generation = 0U;
    slab_release_slot_locked(class_index, page, page_index, slab, ptr,
        SLAB_SLOT_CLIENT, identity, cpu_current_index(), &release, &generation,
        profile_context,
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
        &profile_case
#else
        NULL
#endif
        );
    spin_unlock_irqrestore(&class_state->lock, flags);
    if (release) {
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
        slab_free_profile_interval_begin(profile_call,
                                         SLAB_FREE_PROFILE_RELEASE);
#endif
        slab_finish_retirement(page, page_index, identity, generation);
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
        slab_free_profile_interval_end(profile_call,
                                       SLAB_FREE_PROFILE_RELEASE);
#endif
    } else {
        slab_page_token_unpin(page_index, identity);
    }
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_interval_end(profile_call,
                                   SLAB_FREE_PROFILE_END_TO_END);
    slab_free_profile_call_finish(profile_call, class_index,
        (slab_free_profile_case_t)profile_case);
#endif
    return true;
}

bool slab_try_free(void *ptr)
{
    uintptr_t page;
    usize page_index;
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_call_t profile_call = slab_free_profile_call_begin();
    slab_free_profile_interval_begin(&profile_call,
                                     SLAB_FREE_PROFILE_END_TO_END);
    slab_free_profile_interval_begin(&profile_call,
                                     SLAB_FREE_PROFILE_CLASSIFY);
#endif
    if (!ptr || __atomic_load_n(&slab_initialization_state,
                                __ATOMIC_ACQUIRE) != 2U ||
        !slab_pointer_page(ptr, &page, &page_index))
        return false;
    u64 observed = slab_page_token_load(page_index);
    u8 tag = slab_token_class(observed);
    if (!tag)
        return false;
    slab_require_valid(tag <= SLAB_CLASS_COUNT,
                       "slab: invalid page-classification value");
    u32 class_index = tag - 1U;
    u64 identity = 0U;
    if (!slab_page_token_pin(page_index, class_index,
                             slab_token_generation(observed), &identity))
        panic("slab: stale ownership token during free");
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
    slab_free_profile_interval_end(&profile_call,
                                   SLAB_FREE_PROFILE_CLASSIFY);
    return slab_free_pinned(ptr, page, page_index, class_index, identity,
                            &profile_call);
#else
    return slab_free_pinned(ptr, page, page_index, class_index, identity,
                            NULL);
#endif
}

static bool slab_cache_take_client_locked(slab_cpu_cache_t *cache,
                                          u32 class_index, u32 cpu_index,
                                          void **object_out)
{
    slab_cache_metadata_valid_locked(cache);
    if (!cache->count)
        return false;
    slab_cache_entry_t entry = cache->entries[cache->count - 1U];
    uintptr_t page;
    usize page_index;
    if (!entry.object || !slab_pointer_page(entry.object, &page, &page_index))
        panic("slab: invalid object in local cache");
    u64 identity;
    u64 generation = slab_token_generation(entry.page_token);
    if (!slab_page_token_pin(page_index, class_index, generation, &identity) ||
        identity != entry.page_token)
        panic("slab: stale local-cache classification");
    slab_descriptor_t *slab = (slab_descriptor_t *)page;
    u16 slot;
    slab_require_valid(slab_descriptor_fast_valid(slab, class_index) &&
                       slab_pointer_slot(page, (uintptr_t)entry.object,
                                         class_index, slab, &slot),
                       "slab: corrupt local-cache object metadata");
#if SLAB_DEEP_VALIDATION
    slab_class_t *class_state = &slab_classes[class_index];
    u64 class_flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    bool descriptor_valid = slab_descriptor_valid_locked(slab, class_index);
    spin_unlock_irqrestore(&class_state->lock, class_flags);
    slab_require_valid(descriptor_valid,
                       "slab: deep descriptor validation failed on cache hit");
#endif
    slab_require_valid(slab_slot_state_load(slab, slot) == SLAB_SLOT_CACHED &&
                       slab_slot_owner_load(slab, slot) == cpu_index,
                       "slab: cache entry belongs to another CPU or slot state");
    slab_slot_owner_store(slab, slot, cpu_index);
    slab_require_valid(slab_slot_state_change(slab, slot, SLAB_SLOT_CACHED,
                                              SLAB_SLOT_CLIENT),
                       "slab: local cache contains a noncached object");
    cache->entries[cache->count - 1U] = (slab_cache_entry_t){0};
    cache->count--;
    u64 cached = __atomic_fetch_sub(
        &slab_classes[class_index].stats.cached_slots, 1U,
        __ATOMIC_RELAXED);
    slab_require_valid(cached != 0U, "slab: cached-slot counter underflow");
    slab_record_live_locked(&slab_classes[class_index]);
    cache->allocations++;
    *object_out = entry.object;
    slab_page_token_unpin(page_index, identity);
    return true;
}

static void slab_return_refill(slab_cache_entry_t entry, u32 class_index)
{
    uintptr_t page;
    usize page_index;
    if (!entry.object || !slab_pointer_page(entry.object, &page, &page_index))
        panic("slab: invalid refill object");
    u64 generation = slab_token_generation(entry.page_token);
    u64 identity;
    if (!slab_page_token_pin(page_index, class_index, generation, &identity) ||
        identity != entry.page_token)
        panic("slab: stale refill classification");
    slab_descriptor_t *slab = (slab_descriptor_t *)page;
    u16 slot;
    slab_require_valid(slab_descriptor_fast_valid(slab, class_index) &&
                       slab_pointer_slot(page, (uintptr_t)entry.object,
                                         class_index, slab, &slot),
                       "slab: corrupt refill object metadata");
    slab_class_t *class_state = &slab_classes[class_index];
    u64 flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    bool release = false;
    u64 retired_generation = 0U;
    slab_release_slot_locked(class_index, page, page_index, slab,
        entry.object, SLAB_SLOT_REFILL, identity, cpu_current_index(),
        &release, &retired_generation, NULL, NULL);
    spin_unlock_irqrestore(&class_state->lock, flags);
    if (release)
        slab_finish_retirement(page, page_index, identity,
                               retired_generation);
    else
        slab_page_token_unpin(page_index, identity);
}

void *slab_cpu_alloc(usize size)
{
    u32 class_index;
    if (__atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U ||
        !slab_select_class(size, &class_index))
        return NULL;

    slab_cpu_cache_t *cache;
    slab_cpu_cache_t *refill_cache;
    u32 cpu_index;
    u64 flags;
    if (!slab_local_cache_lock(class_index, &cache, &cpu_index, &flags))
        return slab_alloc(size);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    bool profile_cache_path = heap_profile_hybrid_cache_path_enter(cpu_index);
#endif
    if (cache->count) {
        cache->hits++;
        void *object = NULL;
        slab_cache_take_client_locked(cache, class_index, cpu_index, &object);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_cache_path_leave(cpu_index, profile_cache_path);
#endif
        spin_unlock_irqrestore(&cache->lock, flags);
        return object;
    }
    cache->misses++;
    if (cache->refill_active) {
        cache->refill_conflicts++;
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_cache_path_leave(cpu_index, profile_cache_path);
#endif
        spin_unlock_irqrestore(&cache->lock, flags);
        return slab_alloc(size);
    }
    cache->refill_active = 1U;
    cache->refills++;
    refill_cache = cache;
    __atomic_add_fetch(&slab_refill_reservations,
                       SLAB_LOCAL_REFILL_BATCH, __ATOMIC_ACQ_REL);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    heap_profile_hybrid_cache_path_leave(cpu_index, profile_cache_path);
#endif
    spin_unlock_irqrestore(&cache->lock, flags);

    slab_cache_entry_t refill[SLAB_LOCAL_REFILL_BATCH] = {{0}};
    u32 refill_count = 0U;
    for (u32 index = 0; index < SLAB_LOCAL_REFILL_BATCH; index++) {
        void *object = slab_alloc_state(slab_sizes[class_index],
                                        SLAB_SLOT_REFILL,
                                        &refill[refill_count].page_token);
        if (!object)
            break;
        refill[refill_count].object = object;
        refill_count++;
    }

    bool cache_locked = slab_local_cache_lock(class_index, &cache,
                                               &cpu_index, &flags);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    bool profile_refill_cache_path = cache_locked &&
        heap_profile_hybrid_cache_path_enter(cpu_index);
#endif
    bool cached_refill[SLAB_LOCAL_REFILL_BATCH] = {false};
    for (u32 index = 0; index < refill_count; index++) {
        slab_cache_entry_t entry = refill[index];
        if (cache_locked && cache->count < SLAB_LOCAL_CACHE_CAPACITY) {
            uintptr_t page;
            usize page_index;
            if (!slab_pointer_page(entry.object, &page, &page_index))
                panic("slab: refill outside heap");
            u64 identity;
            if (!slab_page_token_pin(page_index, class_index,
                    slab_token_generation(entry.page_token), &identity) ||
                identity != entry.page_token)
                panic("slab: refill ownership changed before caching");
            slab_descriptor_t *slab = (slab_descriptor_t *)page;
            u16 slot;
            slab_require_valid(slab_descriptor_fast_valid(slab,
                                                           class_index) &&
                               slab_pointer_slot(page, (uintptr_t)entry.object,
                                                 class_index, slab, &slot),
                               "slab: corrupt refill descriptor");
#if SLAB_DEEP_VALIDATION
            slab_class_t *class_state = &slab_classes[class_index];
            u64 class_flags = spin_lock_irqsave(&class_state->lock);
            class_state->stats.lock_acquisitions++;
            bool descriptor_valid = slab_descriptor_valid_locked(
                slab, class_index);
            spin_unlock_irqrestore(&class_state->lock, class_flags);
            slab_require_valid(descriptor_valid,
                "slab: deep descriptor validation failed during refill");
#endif
            slab_require_valid(slab_slot_state_change(slab, slot,
                                   SLAB_SLOT_REFILL, SLAB_SLOT_CACHED),
                               "slab: refill slot state changed");
            /* While cached, this byte identifies the cache record that owns
             * the reference. It is distinct from the last caller CPU kept
             * while the slot is client-owned. */
            slab_slot_owner_store(slab, slot, cpu_index);
            cache->entries[cache->count++] = entry;
            cache->refill_objects++;
            __atomic_add_fetch(&slab_classes[class_index].stats.cached_slots,
                               1U, __ATOMIC_RELAXED);
            cached_refill[index] = true;
            slab_page_token_unpin(page_index, identity);
        }
    }
    void *object = NULL;
    if (cache_locked && cache->count)
        slab_cache_take_client_locked(cache, class_index, cpu_index, &object);
    if (cache_locked) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_cache_path_leave(cpu_index,
                                             profile_refill_cache_path);
#endif
        spin_unlock_irqrestore(&cache->lock, flags);
    }
    for (u32 index = 0; index < refill_count; index++)
        if (!cached_refill[index])
            slab_return_refill(refill[index], class_index);
    flags = spin_lock_irqsave(&refill_cache->lock);
    slab_require_valid(refill_cache->refill_active == 1U,
                       "slab: refill gate lost before release");
    __atomic_sub_fetch(&slab_refill_reservations,
                       SLAB_LOCAL_REFILL_BATCH, __ATOMIC_RELEASE);
    refill_cache->refill_active = 0U;
    spin_unlock_irqrestore(&refill_cache->lock, flags);
    if (!cache_locked)
        return slab_alloc(size);
    return object;
}

bool slab_cpu_try_free(void *ptr)
{
    uintptr_t page;
    usize page_index;
    if (!ptr || __atomic_load_n(&slab_initialization_state,
                                __ATOMIC_ACQUIRE) != 2U ||
        !slab_pointer_page(ptr, &page, &page_index))
        return false;
    u64 observed = slab_page_token_load(page_index);
    u8 tag = slab_token_class(observed);
    if (!tag)
        return false;
    slab_require_valid(tag <= SLAB_CLASS_COUNT,
                       "slab: invalid page-classification value");
    u32 class_index = tag - 1U;
    u64 identity;
    if (!slab_page_token_pin(page_index, class_index,
                             slab_token_generation(observed), &identity))
        panic("slab: stale ownership token during cached free");

    slab_cpu_cache_t *cache;
    u32 cpu_index;
    u64 flags;
    if (!slab_local_cache_lock(class_index, &cache, &cpu_index, &flags))
        return slab_free_pinned(ptr, page, page_index, class_index, identity,
                                NULL);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    bool profile_cache_path = heap_profile_hybrid_cache_path_enter(cpu_index);
#endif
    if (cache->count == SLAB_LOCAL_CACHE_CAPACITY) {
        cache->overflows++;
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_cache_path_leave(cpu_index, profile_cache_path);
#endif
        spin_unlock_irqrestore(&cache->lock, flags);
        return slab_free_pinned(ptr, page, page_index, class_index, identity,
                                NULL);
    }

    if (slab_token_identity(slab_page_token_load(page_index)) != identity ||
        !slab_descriptor_fast_valid((slab_descriptor_t *)page, class_index))
        panic("slab: stale or corrupt descriptor during cached free");
    slab_descriptor_t *slab = (slab_descriptor_t *)page;
    u16 slot;
    slab_require_valid(slab_pointer_slot(page, (uintptr_t)ptr, class_index,
                                         slab, &slot),
                       "slab: interior, misaligned, or out-of-range free");
#if SLAB_DEEP_VALIDATION
    slab_class_t *class_state = &slab_classes[class_index];
    u64 class_flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    bool descriptor_valid = slab_descriptor_valid_locked(slab, class_index);
    spin_unlock_irqrestore(&class_state->lock, class_flags);
    slab_require_valid(descriptor_valid,
                       "slab: deep descriptor validation failed on cached free");
#endif
    u32 owner = slab_slot_owner_load(slab, slot);
    slab_require_valid(owner < slab_cpu_cache_count,
                       "slab: corrupted slot owner during cached free");
    slab_require_valid(slab_slot_state_change(slab, slot, SLAB_SLOT_CLIENT,
                                              SLAB_SLOT_CACHED),
                       "slab: duplicate or unallocated object free");
    /* Cached ownership follows the freeing CPU, including remote frees. */
    slab_slot_owner_store(slab, slot, cpu_index);
    cache->entries[cache->count++] = (slab_cache_entry_t){
        .object = ptr,
        .page_token = identity,
    };
    cache->frees++;
    if (owner != cpu_index) {
        cache->cross_cpu_frees++;
        __atomic_add_fetch(&slab_classes[class_index].stats.cross_cpu_frees,
                           1U, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&slab_classes[class_index].stats.cached_slots, 1U,
                       __ATOMIC_RELAXED);
    slab_record_live_free(&slab_classes[class_index]);
    slab_page_token_unpin(page_index, identity);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    heap_profile_hybrid_cache_path_leave(cpu_index, profile_cache_path);
#endif
    spin_unlock_irqrestore(&cache->lock, flags);
    return true;
}

static bool slab_drain_cache_entry_locked(slab_cpu_cache_t *cache,
                                          u32 cpu_index, u32 class_index,
                                          uintptr_t *page_out,
                                          usize *page_index_out,
                                          u64 *identity_out,
                                          u64 *generation_out,
                                          bool *retired_out,
                                          bool try_class_lock,
                                          bool *class_lock_busy_out)
{
    slab_cache_metadata_valid_locked(cache);
    if (!cache->count)
        return false;
    slab_cache_entry_t entry = cache->entries[cache->count - 1U];
    uintptr_t page;
    usize page_index;
    if (!entry.object || !slab_pointer_page(entry.object, &page, &page_index))
        panic("slab: invalid object while draining cache");
    u64 identity;
    if (!slab_page_token_pin(page_index, class_index,
            slab_token_generation(entry.page_token), &identity) ||
        identity != entry.page_token)
        panic("slab: stale object while draining cache");
    slab_descriptor_t *slab = (slab_descriptor_t *)page;
    u16 slot;
    slab_require_valid(slab_descriptor_fast_valid(slab, class_index) &&
                       slab_pointer_slot(page, (uintptr_t)entry.object,
                                         class_index, slab, &slot) &&
                       slab_slot_owner_load(slab, slot) == cpu_index,
                       "slab: corrupt object while draining cache");
    slab_class_t *class_state = &slab_classes[class_index];
    u64 flags;
    if (try_class_lock) {
        if (!spin_try_lock_irqsave(&class_state->lock, &flags)) {
            slab_page_token_unpin(page_index, identity);
            if (class_lock_busy_out) *class_lock_busy_out = true;
            return false;
        }
    } else {
        flags = spin_lock_irqsave(&class_state->lock);
    }
    class_state->stats.lock_acquisitions++;
    bool release = false;
    u64 generation = 0U;
    slab_release_slot_locked(class_index, page, page_index, slab,
        entry.object, SLAB_SLOT_CACHED, identity, cpu_current_index(),
        &release, &generation, NULL, NULL);
    cache->entries[cache->count - 1U] = (slab_cache_entry_t){0};
    cache->count--;
    cache->drain_objects++;
    spin_unlock_irqrestore(&class_state->lock, flags);
    *page_out = page;
    *page_index_out = page_index;
    *identity_out = identity;
    *generation_out = generation;
    *retired_out = release;
    return true;
}

usize slab_cpu_drain_all(void)
{
    usize drained = 0U;
    if (__atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U ||
        spinlock_depth_current() != 0U)
        return 0U;
    for (u32 cpu_index = 0; cpu_index < slab_cpu_cache_count; cpu_index++) {
        for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT;
             class_index++) {
            slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index, class_index);
            for (;;) {
                uintptr_t page = 0U;
                usize page_index = 0U;
                u64 identity = 0U;
                u64 generation = 0U;
                bool retired = false;
                u64 flags = spin_lock_irqsave(&cache->lock);
                bool did_drain = slab_drain_cache_entry_locked(cache,
                    cpu_index, class_index, &page, &page_index, &identity,
                    &generation, &retired, false, NULL);
                spin_unlock_irqrestore(&cache->lock, flags);
                if (!did_drain)
                    break;
                if (retired)
                    slab_finish_retirement(page, page_index, identity,
                                           generation);
                else
                    slab_page_token_unpin(page_index, identity);
                drained++;
            }
        }
    }
    return drained;
}

static bool slab_reclaim_empty_class(u32 class_index,
                                     slab_reclaim_result_t *result)
{
#if SLAB_EMPTY_RESERVE_PER_CLASS
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *slab;
    u64 flags;
    usize page_index;
    u64 generation;

    if (!spin_try_lock_irqsave(&class_state->lock, &flags)) {
        result->class_lock_busy++;
        return false;
    }
    class_state->stats.lock_acquisitions++;
    slab = class_state->empty_slab;
    if (!slab) {
        spin_unlock_irqrestore(&class_state->lock, flags);
        return false;
    }
    slab_require_valid(slab_descriptor_valid_locked(slab, class_index) &&
                       slab->state == SLAB_STATE_EMPTY &&
                       !slab->allocated_count && !slab->partial_prev &&
                       !slab->partial_next && slab_bitmap_valid(slab) &&
                       class_state->stats.empty_slabs == 1U,
                       "slab: corrupted empty reserve during pressure reclaim");
    slab_require_valid(slab_page_index(slab, &page_index),
                       "slab: empty reserve outside mapped heap");
    u64 token = slab_page_token_load(page_index);
    slab_require_valid(slab_token_class(token) == class_index + 1U &&
                       slab_token_generation(token) != 0U,
                       "slab: empty reserve classification mismatch");
    class_state->empty_slab = NULL;
    class_state->stats.empty_slabs--;
    class_state->stats.retired_slabs++;
    class_state->stats.backing_releases++;
    generation = slab_token_generation(token);
    slab_require_valid(slab_page_token_retire(page_index, class_index,
                                              generation),
                       "slab: failed to retire empty reserve");
    spin_unlock_irqrestore(&class_state->lock, flags);
    slab_page_token_wait_unpinned(page_index, generation);
    slab_backing_release_descriptor(slab,
                                    "slab: invalid reclaimed backing extent");
    result->empty_slab_pages_released++;
    return true;
#else
    (void)class_index;
    (void)result;
    return false;
#endif
}

bool slab_reclaim_bounded(u32 max_cache_records, usize max_objects,
                          slab_reclaim_result_t *result)
{
    slab_reclaim_result_t local = {0};
    bool visited_classes[SLAB_CLASS_COUNT] = {false};
    usize record_count;
    u32 visit_count;
    u32 start;

    if (!result || !max_cache_records || !max_objects ||
        __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U ||
        spinlock_depth_current() != 0U)
        return false;
    record_count = (usize)slab_cpu_cache_count * SLAB_CLASS_COUNT;
    if (!record_count || record_count > (usize)(u32)-1)
        return false;
    visit_count = max_cache_records < record_count
        ? max_cache_records : (u32)record_count;
    start = __atomic_fetch_add(&slab_reclaim_cursor, visit_count,
                               __ATOMIC_RELAXED) % (u32)record_count;

    for (u32 visit = 0; visit < visit_count &&
         local.cached_objects_returned < max_objects; visit++) {
        usize record_index = ((usize)start + visit) % record_count;
        u32 cpu_index = (u32)(record_index / SLAB_CLASS_COUNT);
        u32 class_index = (u32)(record_index % SLAB_CLASS_COUNT);
        slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index, class_index);
        u64 flags;
        bool class_busy = false;
        bool cache_locked;
        visited_classes[class_index] = true;
        local.records_examined++;
        cache_locked = spin_try_lock_irqsave(&cache->lock, &flags);
        if (!cache_locked) {
            local.cache_lock_busy++;
            continue;
        }
        slab_cache_metadata_valid_locked(cache);
        if (cache->refill_active)
            local.refills_in_flight++;
        while (cache->count &&
               local.cached_objects_returned < max_objects) {
            uintptr_t page = 0U;
            usize page_index = 0U;
            u64 identity = 0U;
            u64 generation = 0U;
            bool retired = false;
            bool did_drain = slab_drain_cache_entry_locked(
                cache, cpu_index, class_index, &page, &page_index,
                &identity, &generation, &retired, true, &class_busy);
            if (!did_drain)
                break;
            spin_unlock_irqrestore(&cache->lock, flags);
            cache_locked = false;
            if (retired) {
                slab_finish_retirement(page, page_index, identity,
                                       generation);
                local.slab_pages_released++;
            } else {
                slab_page_token_unpin(page_index, identity);
            }
            local.cached_objects_returned++;
            if (local.cached_objects_returned == max_objects)
                break;
            cache_locked = spin_try_lock_irqsave(&cache->lock, &flags);
            if (!cache_locked) {
                local.cache_lock_busy++;
                break;
            }
            slab_cache_metadata_valid_locked(cache);
        }
        if (cache_locked)
            spin_unlock_irqrestore(&cache->lock, flags);
        if (class_busy)
            local.class_lock_busy++;
    }

    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++)
        if (visited_classes[class_index])
            (void)slab_reclaim_empty_class(class_index, &local);
    local.cache_record_budget_reached =
        visit_count < record_count && local.records_examined == visit_count;
    local.object_budget_reached =
        local.cached_objects_returned >= max_objects;
    *result = local;
    return true;
}

u64 slab_cpu_cache_metadata_bytes(void)
{
    return __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) == 2U
        ? slab_cpu_cache_payload_bytes : 0U;
}

#if defined(SLAB_CORE_DIAGNOSTICS) || \
    defined(HEAP_PROFILE_HYBRID_DIAGNOSTICS)
u32 slab_cpu_cache_count_current(void)
{
    return __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) == 2U
        ? slab_cpu_cache_count : 0U;
}
#endif

usize slab_drain_empty(void)
{
    usize drained = 0U;
    if (__atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return 0U;

    /* Visit one class at a time. Classification is withdrawn under the same
     * class lock that protects the reserve; the indexed heap release occurs
     * only after unlocking and never sleeps. */
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_t *class_state = &slab_classes[class_index];
        u64 flags = spin_lock_irqsave(&class_state->lock);
        class_state->stats.lock_acquisitions++;
        slab_descriptor_t *slab = class_state->empty_slab;
        if (!slab) {
            spin_unlock_irqrestore(&class_state->lock, flags);
            continue;
        }
        slab_require_valid(slab_descriptor_valid_locked(slab, class_index) &&
                           slab->state == SLAB_STATE_EMPTY &&
                           !slab->allocated_count && !slab->partial_prev &&
                           !slab->partial_next && slab_bitmap_valid(slab) &&
                           class_state->stats.empty_slabs == 1U,
                           "slab: corrupted empty reserve during drain");
        usize page_index;
        slab_require_valid(slab_page_index(slab, &page_index),
                           "slab: empty reserve outside mapped heap");
        u64 token = slab_page_token_load(page_index);
        slab_require_valid(slab_token_class(token) == class_index + 1U &&
                           (token >> SLAB_CLASSIFIER_CLASS_BITS) != 0U,
                           "slab: empty reserve classification mismatch");
        class_state->empty_slab = NULL;
        class_state->stats.empty_slabs--;
        class_state->stats.retired_slabs++;
        class_state->stats.backing_releases++;
        u64 generation = slab_token_generation(token);
        slab_require_valid(slab_page_token_retire(page_index, class_index,
                                                  generation),
                           "slab: failed to retire empty reserve");
        spin_unlock_irqrestore(&class_state->lock, flags);
        slab_page_token_wait_unpinned(page_index, generation);
        slab_backing_release_descriptor(
            slab, "slab: invalid drained backing extent");
        drained++;
    }
    return drained;
}

u16 slab_class_size(u32 class_index)
{
    return class_index < SLAB_CLASS_COUNT ? slab_sizes[class_index] : 0U;
}

u16 slab_class_slots(u32 class_index)
{
    return slab_slots_for_class(class_index);
}

bool slab_get_class_stats(u32 class_index, slab_class_stats_t *stats)
{
    if (class_index >= SLAB_CLASS_COUNT || !stats ||
        __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return false;
    slab_class_t *class_state = &slab_classes[class_index];
    u64 flags = spin_lock_irqsave(&class_state->lock);
    stats->lock_acquisitions = class_state->stats.lock_acquisitions;
    stats->allocation_failures = class_state->stats.allocation_failures;
    stats->bitmap_words_examined = class_state->stats.bitmap_words_examined;
    stats->backing_allocations = class_state->stats.backing_allocations;
    stats->backing_releases = class_state->stats.backing_releases;
    stats->transitions_to_full = class_state->stats.transitions_to_full;
    stats->transitions_to_partial = class_state->stats.transitions_to_partial;
    stats->retired_slabs = class_state->stats.retired_slabs;
    stats->peak_live_slots = __atomic_load_n(
        &class_state->stats.peak_live_slots, __ATOMIC_RELAXED);
#if defined(SLAB_CORE_DIAGNOSTICS) || \
    defined(HEAP_PROFILE_HYBRID_DIAGNOSTICS)
    stats->peak_slab_pages = class_state->stats.peak_slab_pages;
#endif
    stats->partial_slabs = class_state->stats.partial_slabs;
    stats->full_slabs = class_state->stats.full_slabs;
    stats->empty_slabs = class_state->stats.empty_slabs;
    stats->empty_slab_reuses = class_state->stats.empty_slab_reuses;
    spin_unlock_irqrestore(&class_state->lock, flags);
    stats->allocations = __atomic_load_n(&class_state->stats.allocations,
                                         __ATOMIC_RELAXED);
    stats->frees = __atomic_load_n(&class_state->stats.frees,
                                   __ATOMIC_RELAXED);
    stats->live_slots = __atomic_load_n(&class_state->stats.live_slots,
                                        __ATOMIC_RELAXED);
    stats->cached_slots = __atomic_load_n(&class_state->stats.cached_slots,
                                          __ATOMIC_RELAXED);
    stats->cross_cpu_frees = __atomic_load_n(
        &class_state->stats.cross_cpu_frees, __ATOMIC_RELAXED);
    stats->cache_hits = stats->cache_misses = 0U;
    stats->cache_refills = stats->cache_refill_conflicts = 0U;
    stats->cache_refill_objects = stats->cache_drain_objects = 0U;
    stats->cache_overflows = 0U;
    for (u32 cpu_index = 0; cpu_index < slab_cpu_cache_count; cpu_index++) {
        slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index, class_index);
        u64 cache_flags = spin_lock_irqsave(&cache->lock);
        stats->cache_hits += cache->hits;
        stats->cache_misses += cache->misses;
        stats->cache_refills += cache->refills;
        stats->cache_refill_conflicts += cache->refill_conflicts;
        stats->cache_refill_objects += cache->refill_objects;
        stats->cache_drain_objects += cache->drain_objects;
        stats->cache_overflows += cache->overflows;
        spin_unlock_irqrestore(&cache->lock, cache_flags);
    }
    return true;
}

#if defined(SLAB_CORE_DIAGNOSTICS) || \
    defined(HEAP_PROFILE_HYBRID_DIAGNOSTICS)
bool slab_get_cpu_class_stats(u32 cpu_index, u32 class_index,
                              slab_cpu_class_stats_t *stats)
{
    if (!stats || class_index >= SLAB_CLASS_COUNT ||
        __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return false;
    slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index, class_index);
    if (!cache)
        return false;
    u64 flags = spin_lock_irqsave(&cache->lock);
    slab_cache_metadata_valid_locked(cache);
    stats->allocations = cache->allocations;
    stats->frees = cache->frees;
    stats->hits = cache->hits;
    stats->misses = cache->misses;
    stats->refills = cache->refills;
    stats->refill_conflicts = cache->refill_conflicts;
    stats->refill_objects = cache->refill_objects;
    stats->drain_objects = cache->drain_objects;
    stats->overflows = cache->overflows;
    stats->cross_cpu_frees = cache->cross_cpu_frees;
    stats->cached_objects = cache->count;
    stats->refill_active = cache->refill_active;
    spin_unlock_irqrestore(&cache->lock, flags);
    return true;
}
#endif

bool slab_get_classifier_stats(slab_classifier_stats_t *stats)
{
    if (!stats ||
        __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return false;
    *stats = (slab_classifier_stats_t){
        .root_payload_bytes = 0U,
        .static_root_bytes = __atomic_load_n(
            &slab_classifier_static_root_bytes, __ATOMIC_RELAXED),
        .middle_payload_bytes = __atomic_load_n(
            &slab_classifier_middle_payload_bytes, __ATOMIC_RELAXED),
        .leaf_payload_bytes = __atomic_load_n(
            &slab_classifier_leaf_payload_bytes, __ATOMIC_RELAXED),
        .retained_payload_bytes = __atomic_load_n(
            &slab_classifier_retained_payload_bytes, __ATOMIC_RELAXED),
        .peak_payload_bytes = __atomic_load_n(
            &slab_classifier_peak_payload_bytes, __ATOMIC_RELAXED),
        .root_tables = __atomic_load_n(&slab_classifier_root_tables,
                                       __ATOMIC_RELAXED),
        .middle_tables = __atomic_load_n(&slab_classifier_middle_tables,
                                         __ATOMIC_RELAXED),
        .leaf_tables = __atomic_load_n(&slab_classifier_leaf_tables,
                                       __ATOMIC_RELAXED),
        .allocation_failures = __atomic_load_n(
            &slab_classifier_allocation_failures, __ATOMIC_RELAXED),
    };
    return true;
}

bool slab_verify(void)
{
    if (__atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return false;
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_t *class_state = &slab_classes[class_index];
        u64 cache_flags[CPU_MAX_COUNT];
        for (u32 cpu_index = 0; cpu_index < slab_cpu_cache_count;
             cpu_index++) {
            slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index, class_index);
            cache_flags[cpu_index] = spin_lock_irqsave(&cache->lock);
        }
        u64 flags = spin_lock_irqsave(&class_state->lock);
        slab_descriptor_t *slab = class_state->partial_head;
        slab_descriptor_t *previous = NULL;
        usize partial_count = 0;
        usize mapped_partial_count = 0;
        usize full_count = 0;
        usize empty_count = 0;
        u64 client_count = 0;
        u64 cached_count = 0;
        u64 refill_count = 0;
        u64 cache_entry_count = 0;
        u64 active_refill_gates = 0U;
        uintptr_t cache_objects[CPU_MAX_COUNT *
                                SLAB_LOCAL_CACHE_CAPACITY];
        bool valid = true;

        while (slab) {
            if (++partial_count > slab_heap_pages ||
                !slab_descriptor_valid_locked(slab, class_index) ||
                slab->state != SLAB_STATE_PARTIAL ||
                slab->partial_prev != previous) {
                valid = false;
                break;
            }
            previous = slab;
            slab = slab->partial_next;
        }
        for (usize page_index = 0; valid && page_index < slab_heap_pages;
             page_index++) {
            uintptr_t page_address = slab_heap_start +
                                     page_index * PAGE_SIZE;
            /* Heap pages are committed as one monotonic prefix.  Do not scan
             * the large uncommitted virtual reservation during diagnostics. */
            if (!heap_address_is_mapped((const void *)page_address))
                break;
            u64 token = slab_page_token_load(page_index);
            u8 tag = slab_token_class(token);
            if (tag > SLAB_CLASS_COUNT ||
                (tag && !(token >> SLAB_CLASSIFIER_CLASS_BITS))) {
                valid = false;
                break;
            }
            if (tag != class_index + 1U)
                continue;
            slab = (slab_descriptor_t *)page_address;
            if (!slab_descriptor_fields_valid(slab, class_index)) {
                valid = false;
                break;
            }
            if (!slab_bitmap_valid(slab)) {
                valid = false;
                break;
            }
            const u8 *states = slab_slot_states_const(slab);
            for (u32 slot = 0; slot < slab->slot_count; slot++) {
                u32 state;
                if (!slab_slot_state_decode(
                        __atomic_load_n(&states[slot], __ATOMIC_ACQUIRE),
                        &state)) {
                    valid = false;
                    break;
                }
                if (state == SLAB_SLOT_CLIENT)
                    client_count++;
                else if (state == SLAB_SLOT_CACHED)
                    cached_count++;
                else if (state == SLAB_SLOT_REFILL)
                    refill_count++;
            }
            if (!valid)
                break;
            if (slab->state == SLAB_STATE_FULL) {
                full_count++;
            } else if (slab->state == SLAB_STATE_PARTIAL) {
                mapped_partial_count++;
            } else if (slab->state == SLAB_STATE_EMPTY) {
                if (class_state->empty_slab != slab ||
                    slab->allocated_count || slab->partial_prev ||
                    slab->partial_next) {
                    valid = false;
                    break;
                }
                empty_count++;
            } else {
                valid = false;
                break;
            }
        }
        for (u32 cpu_index = 0; valid &&
             cpu_index < slab_cpu_cache_count; cpu_index++) {
            slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index, class_index);
            if (cache->count > SLAB_LOCAL_CACHE_CAPACITY) {
                valid = false;
                break;
            }
            if (cache->refill_active > 1U) {
                valid = false;
                break;
            }
            active_refill_gates += cache->refill_active;
            for (u32 entry_index = 0; entry_index < cache->count;
                 entry_index++) {
                slab_cache_entry_t entry = cache->entries[entry_index];
                uintptr_t page;
                usize page_index;
                if (!entry.object ||
                    !slab_pointer_page(entry.object, &page, &page_index) ||
                    slab_token_identity(slab_page_token_load(page_index)) !=
                        entry.page_token) {
                    valid = false;
                    break;
                }
                slab_descriptor_t *entry_slab = (slab_descriptor_t *)page;
                u16 slot;
                if (!slab_descriptor_fast_valid(entry_slab, class_index) ||
                    !slab_pointer_slot(page, (uintptr_t)entry.object,
                                       class_index, entry_slab, &slot) ||
                    slab_slot_state_load(entry_slab, slot) !=
                        SLAB_SLOT_CACHED ||
                    slab_slot_owner_load(entry_slab, slot) != cpu_index) {
                    valid = false;
                    break;
                }
                for (u64 prior = 0; prior < cache_entry_count; prior++) {
                    if (cache_objects[prior] == (uintptr_t)entry.object) {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                    break;
                if (cache_entry_count >=
                    CPU_MAX_COUNT * SLAB_LOCAL_CACHE_CAPACITY) {
                    valid = false;
                    break;
                }
                cache_objects[cache_entry_count] = (uintptr_t)entry.object;
                cache_entry_count++;
            }
            for (u32 entry_index = cache->count;
                 valid && entry_index < SLAB_LOCAL_CACHE_CAPACITY;
                 entry_index++) {
                if (cache->entries[entry_index].object ||
                    cache->entries[entry_index].page_token)
                    valid = false;
            }
        }
        if (valid && (partial_count != mapped_partial_count ||
            partial_count != class_state->stats.partial_slabs ||
            full_count != class_state->stats.full_slabs ||
            empty_count > 1U ||
            empty_count != class_state->stats.empty_slabs ||
            (!!class_state->empty_slab != (empty_count == 1U)) ||
            client_count != __atomic_load_n(&class_state->stats.live_slots,
                                             __ATOMIC_RELAXED) ||
            cached_count != __atomic_load_n(&class_state->stats.cached_slots,
                                             __ATOMIC_RELAXED) ||
            cached_count != cache_entry_count ||
            refill_count > active_refill_gates *
                               SLAB_LOCAL_REFILL_BATCH ||
            __atomic_load_n(&slab_refill_reservations, __ATOMIC_ACQUIRE) <
                active_refill_gates * SLAB_LOCAL_REFILL_BATCH))
            valid = false;
        spin_unlock_irqrestore(&class_state->lock, flags);
        for (u32 cpu_index = slab_cpu_cache_count; cpu_index > 0U;
             cpu_index--) {
            slab_cpu_cache_t *cache = slab_cpu_cache(cpu_index - 1U,
                                                       class_index);
            spin_unlock_irqrestore(&cache->lock, cache_flags[cpu_index - 1U]);
        }
        if (!valid)
            return false;
    }
    return true;
}
