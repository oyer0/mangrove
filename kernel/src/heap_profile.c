/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef HEAP_PROFILE_DIAGNOSTICS
#include <heap_profile.h>
#include <cpu.h>
#include <heap.h>
#include <slab.h>
#include <string.h>
#include <heap_profile_diag.h>

#define PROFILE_SLOT_COUNT HEAP_PROFILE_SIZE_SLOTS
#define PROFILE_TRACK_MASK (HEAP_PROFILE_TRACK_CAPACITY - 1U)
#define PROFILE_PROBE_LIMIT 64U
#define PROFILE_STATE_EMPTY 0U
#define PROFILE_STATE_LIVE 1U
#define PROFILE_STATE_TOMBSTONE 2U

_Static_assert(HEAP_PROFILE_EVENT_CAPACITY <= 0x00ffffffU,
               "profile event IDs must fit the live-entry metadata");

enum {
    SIZE_ATTEMPTS,
    SIZE_SUCCESSES,
    SIZE_FAILURES,
    SIZE_FREES,
    SIZE_SUCCESS_BYTES,
    SIZE_FAILED_BYTES,
    SIZE_CURRENT_LIVE,
    SIZE_CURRENT_BYTES,
    SIZE_PEAK_LIVE,
    SIZE_PEAK_BYTES,
    SIZE_LIFETIME_SAMPLES,
    SIZE_LIFETIME_OPERATIONS,
    SIZE_CROSS_CPU_FREES
};

enum {
    RANGE_CURRENT_LIVE,
    RANGE_CURRENT_BYTES,
    RANGE_PEAK_LIVE,
    RANGE_PEAK_BYTES
};

enum {
    PHASE_ALLOCATIONS,
    PHASE_FREES,
    PHASE_PEAK_LIVE,
    PHASE_PEAK_BYTES,
    PHASE_LIVE_AT_END,
    PHASE_BYTES_AT_END,
    PHASE_SURVIVORS,
    PHASE_SURVIVOR_BYTES
};

typedef struct {
    u64 pointer;
    u64 requested;
    u64 operation;
    u64 metadata;
} heap_profile_live_t;

static heap_profile_diag_snapshot_t profile_data;
static heap_profile_event_t *profile_events;
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
static heap_profile_hybrid_snapshot_t *profile_hybrid;
static volatile u32 profile_hybrid_recording;
static volatile u32 profile_hybrid_inflight;
static volatile u32 profile_hybrid_cache_active;
#endif
#ifdef HEAP_PROFILE_HOST_TEST
extern void *heap_profile_test_storage_alloc(usize bytes);
#endif
#define profile_header profile_data.header
#define profile_sizes profile_data.sizes
#define profile_ranges profile_data.ranges
#define profile_phases profile_data.phases
#define profile_lifetime profile_data.lifetime
#define profile_cpu profile_data.cpu
#define profile_candidate profile_data.candidate
#define profile_candidate_phase profile_data.candidate_phase
#define profile_candidate_summary profile_data.candidate_summary
#define profile_candidate_phase_summary profile_data.candidate_phase_summary
#define profile_candidate_cpu_mask profile_data.candidate_cpu_mask
static heap_profile_live_t profile_live[HEAP_PROFILE_TRACK_CAPACITY];
static volatile u32 profile_recording;
static volatile u32 profile_inflight;
static volatile u32 profile_inflight_overflow;
static bool profile_frozen;
static u32 profile_phase;
static void profile_flag(u64 flag);

static u32 profile_event_append(u32 kind, usize requested, u32 cpu_index,
                                u32 context, u32 object_id,
                                u32 caller_text_offset)
{
    u64 count = profile_header[HPPH_TRACE_EVENT_COUNT];
    u64 capacity = profile_header[HPPH_TRACE_EVENT_CAPACITY];
    if (!profile_events || !capacity) {
        profile_flag(HPPF_TRACE_UNAVAILABLE);
        return 0U;
    }
    if (count >= capacity) {
        profile_flag(HPPF_TRACE_OVERFLOW);
        return 0U;
    }

    heap_profile_event_t *event = &profile_events[count];
    u32 assigned_id = kind == HEAP_PROFILE_EVENT_ALLOCATED
        ? (u32)count + 1U : object_id;
    event->object_id = assigned_id;
    event->caller_text_offset = caller_text_offset;
    event->requested_bytes = requested;
    event->cpu = cpu_index <= 0xffffU ? (u16)cpu_index : 0xffffU;
    event->kind = (u8)kind;
    event->context = context < HEAP_PROFILE_CONTEXT_COUNT
        ? (u8)context : 0xffU;
    event->phase = (u8)profile_phase;
    event->reserved1 = 0U;
    event->reserved2 = 0U;
    /* The count is the publication field. All event data is written first;
     * callers and freeze hold heap_lock, so no reservation can be mistaken
     * for a completed event. */
    profile_header[HPPH_TRACE_EVENT_COUNT] = count + 1U;
    return assigned_id;
}

enum {
    CANDIDATE_CLASS_SIZE,
    CANDIDATE_ALLOCATIONS,
    CANDIDATE_FREES,
    CANDIDATE_REQUESTED_BYTES,
    CANDIDATE_ROUNDING_WASTE,
    CANDIDATE_CURRENT_LIVE,
    CANDIDATE_CURRENT_BYTES,
    CANDIDATE_PEAK_LIVE,
    CANDIDATE_PEAK_BYTES,
    CANDIDATE_CROSS_CPU_FREES
};

enum {
    CANDIDATE_CUTOFF,
    CANDIDATE_CLASS_COUNT,
    CANDIDATE_ROUTED,
    CANDIDATE_BYPASSED,
    CANDIDATE_BYPASS_BYTES,
    CANDIDATE_TOTAL_CURRENT_LIVE,
    CANDIDATE_TOTAL_CURRENT_BYTES,
    CANDIDATE_TOTAL_PEAK_LIVE,
    CANDIDATE_TOTAL_PEAK_BYTES,
    CANDIDATE_TOTAL_CURRENT_SLABS,
    CANDIDATE_TOTAL_PEAK_SLABS,
    CANDIDATE_TOTAL_CURRENT_CAPACITY,
    CANDIDATE_TOTAL_PEAK_CAPACITY,
    CANDIDATE_TOTAL_PEAK_UNUSED_SLOTS
};

enum {
    CANDIDATE_PHASE_PEAK_LIVE,
    CANDIDATE_PHASE_PEAK_BYTES,
    CANDIDATE_PHASE_PEAK_SLABS,
    CANDIDATE_PHASE_PEAK_CAPACITY,
    CANDIDATE_PHASE_PEAK_UNUSED_SLOTS
};

static const u32 profile_candidate_cutoffs[HEAP_PROFILE_CANDIDATES] = {
    256U, 384U, 512U, 768U, 1024U
};

static u32 profile_candidate_class_count(u32 cutoff)
{
    if (cutoff <= 256U) return 16U;
    if (cutoff <= 384U) return 20U;
    if (cutoff <= 512U) return 24U;
    if (cutoff <= 768U) return 28U;
    return 32U;
}

static u32 profile_candidate_class_size(u32 index)
{
    if (index < 16U) return (index + 1U) * 16U;
    if (index < 24U) return 288U + (index - 16U) * 32U;
    if (index < 28U) return 576U + (index - 24U) * 64U;
    return 832U + (index - 28U) * 64U;
}

static bool profile_candidate_index(u32 candidate, u64 requested,
                                    u32 *out_index)
{
    u64 normalized;
    u32 cutoff = profile_candidate_cutoffs[candidate];

    if (!requested || requested > (u64)-1 - 15U)
        return false;
    normalized = (requested + 15U) & ~15ULL;
    if (normalized > cutoff)
        return false;
    if (normalized <= 256U) {
        *out_index = (u32)(normalized / 16U) - 1U;
    } else if (normalized <= 512U) {
        u64 units = (normalized - 256U + 31U) / 32U;
        *out_index = 15U + (u32)units;
    } else {
        u64 units = (normalized - 512U + 63U) / 64U;
        *out_index = 23U + (u32)units;
    }
    return *out_index < profile_candidate_class_count(cutoff);
}

static void profile_candidate_peak(u32 candidate, u32 index)
{
    uint64_t *row = profile_candidate[candidate][index];
    uint64_t *phase = profile_candidate_phase[profile_phase][candidate][index];
    if (row[CANDIDATE_CURRENT_LIVE] > row[CANDIDATE_PEAK_LIVE])
        row[CANDIDATE_PEAK_LIVE] = row[CANDIDATE_CURRENT_LIVE];
    if (row[CANDIDATE_CURRENT_BYTES] > row[CANDIDATE_PEAK_BYTES])
        row[CANDIDATE_PEAK_BYTES] = row[CANDIDATE_CURRENT_BYTES];
    if (row[CANDIDATE_CURRENT_LIVE] > phase[PHASE_PEAK_LIVE])
        phase[PHASE_PEAK_LIVE] = row[CANDIDATE_CURRENT_LIVE];
    if (row[CANDIDATE_CURRENT_BYTES] > phase[PHASE_PEAK_BYTES])
        phase[PHASE_PEAK_BYTES] = row[CANDIDATE_CURRENT_BYTES];
}

static u64 profile_candidate_slab_count(u64 live, u64 class_size)
{
    u64 slots = (SLAB_PAGE_BYTES - SLAB_OBJECT_AREA_OFFSET) /
                (class_size + SLAB_SLOT_METADATA_BYTES);
    if (!slots)
        return 0U;
    return live ? 1U + (live - 1U) / slots : 0U;
}

static u64 profile_candidate_slot_capacity(u64 class_size)
{
    return (SLAB_PAGE_BYTES - SLAB_OBJECT_AREA_OFFSET) /
           (class_size + SLAB_SLOT_METADATA_BYTES);
}

static void profile_candidate_total_peak(u32 candidate)
{
    uint64_t *summary = profile_candidate_summary[candidate];
    uint64_t *phase = profile_candidate_phase_summary[profile_phase][candidate];

    if (summary[CANDIDATE_TOTAL_CURRENT_LIVE] > summary[CANDIDATE_TOTAL_PEAK_LIVE])
        summary[CANDIDATE_TOTAL_PEAK_LIVE] = summary[CANDIDATE_TOTAL_CURRENT_LIVE];
    if (summary[CANDIDATE_TOTAL_CURRENT_BYTES] > summary[CANDIDATE_TOTAL_PEAK_BYTES])
        summary[CANDIDATE_TOTAL_PEAK_BYTES] = summary[CANDIDATE_TOTAL_CURRENT_BYTES];
    if (summary[CANDIDATE_TOTAL_CURRENT_SLABS] > summary[CANDIDATE_TOTAL_PEAK_SLABS])
        summary[CANDIDATE_TOTAL_PEAK_SLABS] = summary[CANDIDATE_TOTAL_CURRENT_SLABS];
    if (summary[CANDIDATE_TOTAL_CURRENT_CAPACITY] >
        summary[CANDIDATE_TOTAL_PEAK_CAPACITY])
        summary[CANDIDATE_TOTAL_PEAK_CAPACITY] =
            summary[CANDIDATE_TOTAL_CURRENT_CAPACITY];
    u64 unused = summary[CANDIDATE_TOTAL_CURRENT_CAPACITY] >=
                 summary[CANDIDATE_TOTAL_CURRENT_LIVE]
        ? summary[CANDIDATE_TOTAL_CURRENT_CAPACITY] -
              summary[CANDIDATE_TOTAL_CURRENT_LIVE]
        : 0U;
    if (unused > summary[CANDIDATE_TOTAL_PEAK_UNUSED_SLOTS])
        summary[CANDIDATE_TOTAL_PEAK_UNUSED_SLOTS] = unused;
    if (summary[CANDIDATE_TOTAL_CURRENT_LIVE] > phase[CANDIDATE_PHASE_PEAK_LIVE])
        phase[CANDIDATE_PHASE_PEAK_LIVE] =
            summary[CANDIDATE_TOTAL_CURRENT_LIVE];
    if (summary[CANDIDATE_TOTAL_CURRENT_BYTES] > phase[CANDIDATE_PHASE_PEAK_BYTES])
        phase[CANDIDATE_PHASE_PEAK_BYTES] =
            summary[CANDIDATE_TOTAL_CURRENT_BYTES];
    if (summary[CANDIDATE_TOTAL_CURRENT_SLABS] > phase[CANDIDATE_PHASE_PEAK_SLABS])
        phase[CANDIDATE_PHASE_PEAK_SLABS] =
            summary[CANDIDATE_TOTAL_CURRENT_SLABS];
    if (summary[CANDIDATE_TOTAL_CURRENT_CAPACITY] >
        phase[CANDIDATE_PHASE_PEAK_CAPACITY])
        phase[CANDIDATE_PHASE_PEAK_CAPACITY] =
            summary[CANDIDATE_TOTAL_CURRENT_CAPACITY];
    if (unused > phase[CANDIDATE_PHASE_PEAK_UNUSED_SLOTS])
        phase[CANDIDATE_PHASE_PEAK_UNUSED_SLOTS] = unused;
}

static void profile_range_peak(u32 range)
{
    uint64_t *row = profile_ranges[range];
    if (row[RANGE_CURRENT_LIVE] > row[RANGE_PEAK_LIVE])
        row[RANGE_PEAK_LIVE] = row[RANGE_CURRENT_LIVE];
    if (row[RANGE_CURRENT_BYTES] > row[RANGE_PEAK_BYTES])
        row[RANGE_PEAK_BYTES] = row[RANGE_CURRENT_BYTES];
}

static void profile_flag(u64 flag)
{
    profile_header[HPPH_FLAGS] |= flag;
}

static void profile_add(uint64_t *counter, u64 amount)
{
    if ((u64)-1 - *counter < amount) {
        *counter = (u64)-1;
        profile_flag(HPPF_COUNTER_SATURATED);
    } else {
        *counter += amount;
    }
}

static void profile_context_add(u32 first_field, u32 context)
{
    if (context >= HEAP_PROFILE_CONTEXT_COUNT) {
        profile_flag(HPPF_TRACKING_INCOMPLETE);
        return;
    }
    profile_add(&profile_header[first_field + context], 1U);
}

static u32 profile_log2(u64 value)
{
    u32 result = 0;
    while (value > 1U) {
        value >>= 1;
        result++;
    }
    return result;
}

static u32 profile_size_slot(u64 size)
{
    if (size <= HEAP_PROFILE_EXACT_MAX)
        return (u32)size;
    return HEAP_PROFILE_EXACT_MAX + 1U + profile_log2(size);
}

static u32 profile_range_slot(u64 size)
{
    if (size == 0U) return 6U;
    if (size <= 16U) return 0U;
    if (size <= 64U) return 1U;
    if (size <= 256U) return 2U;
    if (size <= 512U) return 3U;
    if (size <= 1024U) return 4U;
    return 5U;
}

static u32 profile_size_log_bucket(u64 size)
{
    if (!size)
        return 0;
    return profile_log2(size);
}

static u32 profile_lifetime_bucket(u64 operations)
{
    u32 bucket = operations ? profile_log2(operations) : 0U;
    return bucket < HEAP_PROFILE_LIFETIME_BUCKETS
        ? bucket : HEAP_PROFILE_LIFETIME_BUCKETS - 1U;
}

static u32 profile_cpu_slot(u32 cpu)
{
    return cpu < CPU_MAX_COUNT ? cpu : CPU_MAX_COUNT;
}

static u32 profile_live_state(const heap_profile_live_t *entry)
{
    return (u32)(entry->metadata & 0xffU);
}

static u32 profile_live_cpu(const heap_profile_live_t *entry)
{
    return (u32)((entry->metadata >> 16) & 0xffffU);
}

static u32 profile_live_phase(const heap_profile_live_t *entry)
{
    return (u32)((entry->metadata >> 8) & 0xffU);
}

static u32 profile_live_event_id(const heap_profile_live_t *entry)
{
    return (u32)(entry->metadata >> 32);
}

static u64 profile_metadata(u32 state, u32 phase, u32 cpu, u32 event_id)
{
    return ((u64)event_id << 32) | ((u64)(cpu & 0xffffU) << 16) |
           ((u64)(phase & 0xffU) << 8) | state;
}

static u32 profile_hash(u64 pointer)
{
    u64 value = pointer >> 4;
    value ^= value >> 23;
    value *= 0x9e3779b185ebca87ULL;
    value ^= value >> 29;
    return (u32)value & PROFILE_TRACK_MASK;
}

/* Every lookup is bounded. A full or collision-heavy table makes ownership
 * and lifetime evidence incomplete; it never changes allocation behavior. */
static heap_profile_live_t *profile_find(u64 pointer, bool insert,
                                        u32 *probes)
{
    u32 first = profile_hash(pointer);
    heap_profile_live_t *tombstone = NULL;

    for (u32 count = 0; count < PROFILE_PROBE_LIMIT; count++) {
        heap_profile_live_t *entry =
            &profile_live[(first + count) & PROFILE_TRACK_MASK];
        u32 state = profile_live_state(entry);
        if (probes && count + 1U > *probes)
            *probes = count + 1U;
        if (state == PROFILE_STATE_LIVE) {
            if (entry->pointer == pointer)
                return entry;
        } else if (state == PROFILE_STATE_TOMBSTONE) {
            if (!tombstone)
                tombstone = entry;
        } else if (state == PROFILE_STATE_EMPTY) {
            return insert && tombstone ? tombstone : (insert ? entry : NULL);
        } else {
            profile_flag(HPPF_TRACKING_INCOMPLETE);
            return NULL;
        }
    }
    if (insert && tombstone)
        return tombstone;
    return NULL;
}

static void profile_update_peak(u32 slot)
{
    uint64_t *size = profile_sizes[slot];
    uint64_t *phase = profile_phases[profile_phase][slot];
    if (size[SIZE_CURRENT_LIVE] > size[SIZE_PEAK_LIVE])
        size[SIZE_PEAK_LIVE] = size[SIZE_CURRENT_LIVE];
    if (size[SIZE_CURRENT_BYTES] > size[SIZE_PEAK_BYTES])
        size[SIZE_PEAK_BYTES] = size[SIZE_CURRENT_BYTES];
    if (size[SIZE_CURRENT_LIVE] > phase[PHASE_PEAK_LIVE])
        phase[PHASE_PEAK_LIVE] = size[SIZE_CURRENT_LIVE];
    if (size[SIZE_CURRENT_BYTES] > phase[PHASE_PEAK_BYTES])
        phase[PHASE_PEAK_BYTES] = size[SIZE_CURRENT_BYTES];
}

void heap_profile_init(void)
{
    __atomic_store_n(&profile_recording, 0U, __ATOMIC_SEQ_CST);
    profile_frozen = true;
    memset(&profile_data, 0, sizeof(profile_data));
    memset(profile_live, 0, sizeof(profile_live));
    if (!profile_events) {
#ifdef HEAP_PROFILE_HOST_TEST
        profile_events = heap_profile_test_storage_alloc(
            HEAP_PROFILE_EVENT_CAPACITY * sizeof(*profile_events));
#else
        profile_events = heap_alloc_raw(
            HEAP_PROFILE_EVENT_CAPACITY * sizeof(*profile_events));
#endif
    }
    if (profile_events)
        memset(profile_events, 0,
               HEAP_PROFILE_EVENT_CAPACITY * sizeof(*profile_events));
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    if (!profile_hybrid) {
#ifdef HEAP_PROFILE_HOST_TEST
        profile_hybrid = heap_profile_test_storage_alloc(
            sizeof(*profile_hybrid));
#else
        profile_hybrid = heap_alloc_raw(sizeof(*profile_hybrid));
#endif
    }
    if (profile_hybrid)
        memset(profile_hybrid, 0, sizeof(*profile_hybrid));
    __atomic_store_n(&profile_hybrid_inflight, 0U, __ATOMIC_SEQ_CST);
    __atomic_store_n(&profile_hybrid_recording, 0U, __ATOMIC_SEQ_CST);
    __atomic_store_n(&profile_hybrid_cache_active, 0U, __ATOMIC_RELAXED);
#endif
    profile_header[HPPH_MAGIC] = HEAP_PROFILE_DIAG_MAGIC;
    profile_header[HPPH_VERSION] = HEAP_PROFILE_DIAG_VERSION;
    profile_header[HPPH_TRACE_EVENT_CAPACITY] = profile_events
        ? HEAP_PROFILE_EVENT_CAPACITY : 0U;
    profile_header[HPPH_TOTAL_BYTES] = sizeof(heap_profile_diag_snapshot_t) +
        profile_header[HPPH_TRACE_EVENT_CAPACITY] * sizeof(*profile_events);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    profile_header[HPPH_TOTAL_BYTES] += sizeof(*profile_hybrid);
    if (!profile_hybrid)
        profile_flag(HPPF_HYBRID_STATS_UNAVAILABLE);
#endif
    if (!profile_events)
        profile_flag(HPPF_TRACE_UNAVAILABLE);
    profile_phase = 0;
    for (u32 candidate = 0; candidate < HEAP_PROFILE_CANDIDATES;
         candidate++) {
        u32 count = profile_candidate_class_count(
            profile_candidate_cutoffs[candidate]);
        profile_candidate_summary[candidate][CANDIDATE_CUTOFF] =
            profile_candidate_cutoffs[candidate];
        profile_candidate_summary[candidate][CANDIDATE_CLASS_COUNT] = count;
        for (u32 index = 0; index < count; index++)
            profile_candidate[candidate][index][CANDIDATE_CLASS_SIZE] =
                profile_candidate_class_size(index);
    }
    profile_frozen = false;
    __atomic_store_n(&profile_inflight, 0U, __ATOMIC_SEQ_CST);
    __atomic_store_n(&profile_inflight_overflow, 0U, __ATOMIC_SEQ_CST);
    __atomic_store_n(&profile_recording, 1U, __ATOMIC_RELEASE);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    if (profile_hybrid)
        __atomic_store_n(&profile_hybrid_recording, 1U, __ATOMIC_RELEASE);
#endif
}

bool heap_profile_request_enter(void)
{
    if (!__atomic_load_n(&profile_recording, __ATOMIC_SEQ_CST))
        return false;

    u32 old = __atomic_load_n(&profile_inflight, __ATOMIC_SEQ_CST);
    for (;;) {
        if (old == (u32)-1) {
            __atomic_store_n(&profile_inflight_overflow, 1U,
                             __ATOMIC_SEQ_CST);
            return false;
        }
        if (__atomic_compare_exchange_n(&profile_inflight, &old, old + 1U,
                                        false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            break;
    }
    if (!__atomic_load_n(&profile_recording, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&profile_inflight, 1U, __ATOMIC_SEQ_CST);
        return false;
    }
    return true;
}

void heap_profile_request_leave_locked(void)
{
    u32 value = __atomic_load_n(&profile_inflight, __ATOMIC_RELAXED);
    if (!value)
        profile_flag(HPPF_INFLIGHT_AT_FREEZE);
    else
        __atomic_fetch_sub(&profile_inflight, 1U, __ATOMIC_RELEASE);
}

void heap_profile_failed_locked(usize requested, u32 cpu_index, u32 context,
                                u32 caller_text_offset)
{
    if (profile_frozen)
        return;
    profile_context_add(HPPH_FAIL_CONTEXT_NORMAL, context);
    u32 slot = profile_size_slot(requested);
    uint64_t *size = profile_sizes[slot];
    profile_add(&profile_header[HPPH_ATTEMPTS], 1U);
    profile_add(&profile_header[HPPH_FAILURES], 1U);
    profile_add(&profile_header[HPPH_FAILED_BYTES], requested);
    profile_add(&size[SIZE_ATTEMPTS], 1U);
    profile_add(&size[SIZE_FAILURES], 1U);
    profile_add(&size[SIZE_FAILED_BYTES], requested);
    (void)profile_event_append(HEAP_PROFILE_EVENT_FAILED, requested,
                               cpu_index, context, 0U,
                               caller_text_offset);
}

void heap_profile_allocated_locked(void *pointer, usize requested,
                                   u32 cpu_index, u32 context,
                                   u32 caller_text_offset)
{
    if (profile_frozen)
        return;
    profile_context_add(HPPH_ALLOC_CONTEXT_NORMAL, context);
    u32 slot = profile_size_slot(requested);
    u32 cpu = profile_cpu_slot(cpu_index);
    u32 probes = 0;
    uint64_t *size = profile_sizes[slot];
    uint64_t *phase = profile_phases[profile_phase][slot];
    heap_profile_live_t *entry = profile_find((u64)(uintptr_t)pointer,
                                               true, &probes);

    profile_add(&profile_header[HPPH_ATTEMPTS], 1U);
    profile_add(&profile_header[HPPH_SUCCESSES], 1U);
    profile_add(&profile_header[HPPH_SUCCESS_BYTES], requested);
    profile_add(&size[SIZE_ATTEMPTS], 1U);
    profile_add(&size[SIZE_SUCCESSES], 1U);
    profile_add(&size[SIZE_SUCCESS_BYTES], requested);
    profile_add(&profile_header[HPPH_OPERATIONS], 1U);
    profile_add(&profile_cpu[cpu][0], 1U);
    profile_add(&phase[PHASE_ALLOCATIONS], 1U);
    u32 event_id = profile_event_append(HEAP_PROFILE_EVENT_ALLOCATED,
                                        requested, cpu_index, context, 0U,
                                        caller_text_offset);

    profile_add(&size[SIZE_CURRENT_LIVE], 1U);
    profile_add(&size[SIZE_CURRENT_BYTES], requested);
    profile_update_peak(slot);
    u32 range_index = profile_range_slot(requested);
    profile_add(&profile_ranges[range_index][RANGE_CURRENT_LIVE], 1U);
    profile_add(&profile_ranges[range_index][RANGE_CURRENT_BYTES], requested);
    profile_range_peak(range_index);

    for (u32 candidate = 0; candidate < HEAP_PROFILE_CANDIDATES;
         candidate++) {
        u32 index;
        if (!profile_candidate_index(candidate, requested, &index)) {
            uint64_t *summary = profile_candidate_summary[candidate];
            profile_add(&summary[CANDIDATE_BYPASSED], 1U);
            profile_add(&summary[CANDIDATE_BYPASS_BYTES], requested);
            continue;
        }
        uint64_t *row = profile_candidate[candidate][index];
        uint64_t *phase_row = profile_candidate_phase[profile_phase]
                                               [candidate][index];
        uint64_t *summary = profile_candidate_summary[candidate];
        u64 old_slabs = profile_candidate_slab_count(
            row[CANDIDATE_CURRENT_LIVE], row[CANDIDATE_CLASS_SIZE]);
        if (cpu_index < CPU_MAX_COUNT)
            profile_candidate_cpu_mask[candidate][index][cpu_index / 64U] |=
                (u64)1U << (cpu_index % 64U);
        profile_add(&row[CANDIDATE_ALLOCATIONS], 1U);
        profile_add(&row[CANDIDATE_REQUESTED_BYTES], requested);
        profile_add(&row[CANDIDATE_ROUNDING_WASTE],
                    row[CANDIDATE_CLASS_SIZE] - requested);
        profile_add(&row[CANDIDATE_CURRENT_LIVE], 1U);
        profile_add(&row[CANDIDATE_CURRENT_BYTES], requested);
        profile_add(&summary[CANDIDATE_ROUTED], 1U);
        profile_add(&summary[CANDIDATE_TOTAL_CURRENT_LIVE], 1U);
        profile_add(&summary[CANDIDATE_TOTAL_CURRENT_BYTES], requested);
        u64 new_slabs = profile_candidate_slab_count(
            row[CANDIDATE_CURRENT_LIVE], row[CANDIDATE_CLASS_SIZE]);
        if (new_slabs >= old_slabs) {
            profile_add(&summary[CANDIDATE_TOTAL_CURRENT_SLABS],
                        new_slabs - old_slabs);
            profile_add(&summary[CANDIDATE_TOTAL_CURRENT_CAPACITY],
                (new_slabs - old_slabs) *
                profile_candidate_slot_capacity(
                    row[CANDIDATE_CLASS_SIZE]));
        } else
            profile_flag(HPPF_TRACKING_INCOMPLETE);
        profile_add(&phase_row[PHASE_ALLOCATIONS], 1U);
        profile_candidate_peak(candidate, index);
        profile_candidate_total_peak(candidate);
    }

    if (probes > profile_header[HPPH_MAX_PROBE])
        profile_header[HPPH_MAX_PROBE] = probes;
    if (!entry || profile_live_state(entry) == PROFILE_STATE_LIVE) {
        profile_flag(HPPF_TRACKING_INCOMPLETE);
        profile_add(&profile_header[HPPH_TRACKING_MISSES], 1U);
        return;
    }

    u64 operation = profile_header[HPPH_OPERATIONS];
    entry->pointer = (u64)(uintptr_t)pointer;
    entry->requested = requested;
    entry->operation = operation;
    entry->metadata = profile_metadata(PROFILE_STATE_LIVE, profile_phase,
                                       cpu_index, event_id);
    profile_add(&profile_header[HPPH_TRACKED_LIVE], 1U);
    if (profile_header[HPPH_TRACKED_LIVE] >
        profile_header[HPPH_TRACKED_PEAK])
        profile_header[HPPH_TRACKED_PEAK] =
            profile_header[HPPH_TRACKED_LIVE];
}

void heap_profile_freed_locked(void *pointer, u32 cpu_index, u32 context,
                               u32 caller_text_offset)
{
    if (profile_frozen)
        return;
    profile_context_add(HPPH_FREE_CONTEXT_NORMAL, context);
    u32 cpu = profile_cpu_slot(cpu_index);
    u32 probes = 0;
    heap_profile_live_t *entry = profile_find((u64)(uintptr_t)pointer,
                                               false, &probes);

    if (probes > profile_header[HPPH_MAX_PROBE])
        profile_header[HPPH_MAX_PROBE] = probes;

    profile_add(&profile_header[HPPH_FREES], 1U);
    profile_add(&profile_header[HPPH_OPERATIONS], 1U);
    profile_add(&profile_cpu[cpu][1], 1U);
    if (!entry || profile_live_state(entry) != PROFILE_STATE_LIVE) {
        profile_flag(HPPF_TRACKING_INCOMPLETE);
        profile_add(&profile_header[HPPH_TRACKING_MISSES], 1U);
        return;
    }

    u32 event_id = profile_live_event_id(entry);
    (void)profile_event_append(HEAP_PROFILE_EVENT_FREED, entry->requested,
                               cpu_index, context, event_id,
                               caller_text_offset);

    u32 slot = profile_size_slot(entry->requested);
    u32 birth_phase = profile_live_phase(entry);
    u32 alloc_cpu = profile_live_cpu(entry);
    u64 lifetime = profile_header[HPPH_OPERATIONS] - entry->operation;
    uint64_t *size = profile_sizes[slot];
    uint64_t *phase = profile_phases[profile_phase][slot];

    profile_add(&size[SIZE_FREES], 1U);
    profile_add(&size[SIZE_LIFETIME_SAMPLES], 1U);
    profile_add(&size[SIZE_LIFETIME_OPERATIONS], lifetime);
    profile_add(&phase[PHASE_FREES], 1U);
    profile_add(&profile_lifetime[profile_size_log_bucket(entry->requested)]
                                 [profile_lifetime_bucket(lifetime)], 1U);
    profile_add(&profile_header[HPPH_LIFETIME_SAMPLES], 1U);
    profile_add(&profile_header[HPPH_LIFETIME_OPERATIONS], lifetime);

    u32 range_index = profile_range_slot(entry->requested);
    uint64_t *range = profile_ranges[range_index];
    if (!range[RANGE_CURRENT_LIVE] ||
        range[RANGE_CURRENT_BYTES] < entry->requested) {
        profile_flag(HPPF_TRACKING_INCOMPLETE);
    } else {
        range[RANGE_CURRENT_LIVE]--;
        range[RANGE_CURRENT_BYTES] -= entry->requested;
    }

    for (u32 candidate = 0; candidate < HEAP_PROFILE_CANDIDATES;
         candidate++) {
        u32 index;
        if (!profile_candidate_index(candidate, entry->requested, &index))
            continue;
        uint64_t *row = profile_candidate[candidate][index];
        uint64_t *phase_row = profile_candidate_phase[profile_phase]
                                               [candidate][index];
        uint64_t *summary = profile_candidate_summary[candidate];
        u64 old_slabs = profile_candidate_slab_count(
            row[CANDIDATE_CURRENT_LIVE], row[CANDIDATE_CLASS_SIZE]);
        if (cpu_index < CPU_MAX_COUNT)
            profile_candidate_cpu_mask[candidate][index][cpu_index / 64U] |=
                (u64)1U << (cpu_index % 64U);
        profile_add(&row[CANDIDATE_FREES], 1U);
        profile_add(&phase_row[PHASE_FREES], 1U);
        if (!row[CANDIDATE_CURRENT_LIVE] ||
            row[CANDIDATE_CURRENT_BYTES] < entry->requested) {
            profile_flag(HPPF_TRACKING_INCOMPLETE);
        } else {
            row[CANDIDATE_CURRENT_LIVE]--;
            row[CANDIDATE_CURRENT_BYTES] -= entry->requested;
            if (!summary[CANDIDATE_TOTAL_CURRENT_LIVE] ||
                summary[CANDIDATE_TOTAL_CURRENT_BYTES] < entry->requested) {
                profile_flag(HPPF_TRACKING_INCOMPLETE);
            } else {
                summary[CANDIDATE_TOTAL_CURRENT_LIVE]--;
                summary[CANDIDATE_TOTAL_CURRENT_BYTES] -= entry->requested;
                u64 new_slabs = profile_candidate_slab_count(
                    row[CANDIDATE_CURRENT_LIVE],
                    row[CANDIDATE_CLASS_SIZE]);
                if (old_slabs >= new_slabs &&
                    summary[CANDIDATE_TOTAL_CURRENT_SLABS] >=
                        old_slabs - new_slabs &&
                    summary[CANDIDATE_TOTAL_CURRENT_CAPACITY] >=
                        (old_slabs - new_slabs) *
                            profile_candidate_slot_capacity(
                                row[CANDIDATE_CLASS_SIZE])) {
                    summary[CANDIDATE_TOTAL_CURRENT_SLABS] -=
                        old_slabs - new_slabs;
                    summary[CANDIDATE_TOTAL_CURRENT_CAPACITY] -=
                        (old_slabs - new_slabs) *
                        profile_candidate_slot_capacity(
                            row[CANDIDATE_CLASS_SIZE]);
                } else
                    profile_flag(HPPF_TRACKING_INCOMPLETE);
            }
        }
        if (alloc_cpu != cpu_index)
            profile_add(&row[CANDIDATE_CROSS_CPU_FREES], 1U);
        profile_candidate_peak(candidate, index);
        profile_candidate_total_peak(candidate);
    }

    if (alloc_cpu != cpu_index) {
        profile_add(&size[SIZE_CROSS_CPU_FREES], 1U);
        profile_add(&profile_header[HPPH_CROSS_CPU_FREES], 1U);
        profile_add(&profile_cpu[cpu][2], 1U);
    }
    if (size[SIZE_CURRENT_LIVE] == 0U ||
        size[SIZE_CURRENT_BYTES] < entry->requested) {
        profile_flag(HPPF_TRACKING_INCOMPLETE);
    } else {
        size[SIZE_CURRENT_LIVE]--;
        size[SIZE_CURRENT_BYTES] -= entry->requested;
    }
    profile_range_peak(range_index);
    if (profile_header[HPPH_TRACKED_LIVE])
        profile_header[HPPH_TRACKED_LIVE]--;
    else
        profile_flag(HPPF_TRACKING_INCOMPLETE);

    /* The table preserves birth CPU and phase; pointer reuse is a new record
     * because each successful free turns the old entry into a tombstone. */
    entry->metadata = profile_metadata(PROFILE_STATE_TOMBSTONE, birth_phase,
                                       alloc_cpu, event_id);
    profile_update_peak(slot);
}

bool heap_profile_set_phase_locked(u32 phase)
{
    if (profile_frozen)
        return false;
    if (phase != profile_phase + 1U || phase >= HEAP_PROFILE_PHASES) {
        profile_flag(HPPF_PHASE_ERROR);
        return false;
    }

    for (u32 slot = 0; slot < PROFILE_SLOT_COUNT; slot++) {
        uint64_t *size = profile_sizes[slot];
        uint64_t *previous = profile_phases[profile_phase][slot];
        uint64_t *next = profile_phases[phase][slot];
        previous[PHASE_LIVE_AT_END] = size[SIZE_CURRENT_LIVE];
        previous[PHASE_BYTES_AT_END] = size[SIZE_CURRENT_BYTES];
        next[PHASE_PEAK_LIVE] = size[SIZE_CURRENT_LIVE];
        next[PHASE_PEAK_BYTES] = size[SIZE_CURRENT_BYTES];
    }

    for (u32 candidate = 0; candidate < HEAP_PROFILE_CANDIDATES;
         candidate++) {
        uint64_t *summary = profile_candidate_summary[candidate];
        uint64_t *phase_summary = profile_candidate_phase_summary[phase][candidate];
        phase_summary[CANDIDATE_PHASE_PEAK_LIVE] =
            summary[CANDIDATE_TOTAL_CURRENT_LIVE];
        phase_summary[CANDIDATE_PHASE_PEAK_BYTES] =
            summary[CANDIDATE_TOTAL_CURRENT_BYTES];
        phase_summary[CANDIDATE_PHASE_PEAK_SLABS] =
            summary[CANDIDATE_TOTAL_CURRENT_SLABS];
        phase_summary[CANDIDATE_PHASE_PEAK_CAPACITY] =
            summary[CANDIDATE_TOTAL_CURRENT_CAPACITY];
        phase_summary[CANDIDATE_PHASE_PEAK_UNUSED_SLOTS] =
            summary[CANDIDATE_TOTAL_CURRENT_CAPACITY] -
            summary[CANDIDATE_TOTAL_CURRENT_LIVE];
        for (u32 index = 0;
             index < profile_candidate_summary[candidate]
                                                  [CANDIDATE_CLASS_COUNT];
             index++) {
            uint64_t *row = profile_candidate[candidate][index];
            uint64_t *previous = profile_candidate_phase[profile_phase]
                                                   [candidate][index];
            uint64_t *next = profile_candidate_phase[phase][candidate][index];
            previous[PHASE_LIVE_AT_END] = row[CANDIDATE_CURRENT_LIVE];
            previous[PHASE_BYTES_AT_END] = row[CANDIDATE_CURRENT_BYTES];
            next[PHASE_PEAK_LIVE] = row[CANDIDATE_CURRENT_LIVE];
            next[PHASE_PEAK_BYTES] = row[CANDIDATE_CURRENT_BYTES];
        }
    }

    for (u32 i = 0; i < HEAP_PROFILE_TRACK_CAPACITY; i++) {
        heap_profile_live_t *entry = &profile_live[i];
        if (profile_live_state(entry) != PROFILE_STATE_LIVE ||
            profile_live_phase(entry) != profile_phase)
            continue;
        u32 slot = profile_size_slot(entry->requested);
        profile_add(&profile_phases[profile_phase][slot][PHASE_SURVIVORS], 1U);
        profile_add(&profile_phases[profile_phase][slot][PHASE_SURVIVOR_BYTES],
                    entry->requested);
        for (u32 candidate = 0; candidate < HEAP_PROFILE_CANDIDATES;
             candidate++) {
            u32 index;
            if (!profile_candidate_index(candidate, entry->requested, &index))
                continue;
            uint64_t *phase_row = profile_candidate_phase[profile_phase]
                                                   [candidate][index];
            profile_add(&phase_row[PHASE_SURVIVORS], 1U);
            profile_add(&phase_row[PHASE_SURVIVOR_BYTES], entry->requested);
        }
    }
    profile_phase = phase;
    profile_header[HPPH_FINAL_PHASE] = phase;
    return true;
}

void heap_profile_freeze_locked(void)
{
    if (profile_frozen)
        return;
    profile_frozen = true;
    /* Close admission before sampling the count. Sequentially consistent
     * admission and count operations ensure a request that committed to
     * profiling is either visible here or observes the closed gate and is
     * excluded. A request outside the heap lock (for example during growth)
     * remains in-flight and makes this snapshot explicitly incomplete. */
    __atomic_store_n(&profile_recording, 0U, __ATOMIC_SEQ_CST);
    u32 inflight = __atomic_load_n(&profile_inflight, __ATOMIC_SEQ_CST);
    profile_header[HPPH_CPU_COUNT] = cpu_online_count();
    profile_header[HPPH_FINAL_PHASE] = profile_phase;
    profile_header[HPPH_INFLIGHT_AT_FREEZE] = inflight;
    if (inflight)
        profile_flag(HPPF_INFLIGHT_AT_FREEZE);
    if (__atomic_load_n(&profile_inflight_overflow, __ATOMIC_SEQ_CST))
        profile_flag(HPPF_INFLIGHT_AT_FREEZE);

    for (u32 slot = 0; slot < PROFILE_SLOT_COUNT; slot++) {
        profile_phases[profile_phase][slot][PHASE_LIVE_AT_END] =
            profile_sizes[slot][SIZE_CURRENT_LIVE];
        profile_phases[profile_phase][slot][PHASE_BYTES_AT_END] =
            profile_sizes[slot][SIZE_CURRENT_BYTES];
    }
    for (u32 candidate = 0; candidate < HEAP_PROFILE_CANDIDATES;
         candidate++) {
        for (u32 index = 0;
             index < profile_candidate_summary[candidate]
                                                  [CANDIDATE_CLASS_COUNT];
             index++) {
            uint64_t *row = profile_candidate[candidate][index];
            uint64_t *phase = profile_candidate_phase[profile_phase]
                                                [candidate][index];
            phase[PHASE_LIVE_AT_END] = row[CANDIDATE_CURRENT_LIVE];
            phase[PHASE_BYTES_AT_END] = row[CANDIDATE_CURRENT_BYTES];
        }
    }
}

#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
static void profile_hybrid_flag(u64 flag)
{
    if (profile_hybrid)
        __atomic_fetch_or(&profile_hybrid->header[HPPHY_FLAGS], flag,
                          __ATOMIC_RELAXED);
}

static void profile_hybrid_add(u32 field, u64 amount)
{
    if (!profile_hybrid || field >= HEAP_PROFILE_HYBRID_ROUTE_FIELDS)
        return;
    uint64_t *counter = &profile_hybrid->routes[field];
    uint64_t old = __atomic_load_n(counter, __ATOMIC_RELAXED);
    for (;;) {
        if (old > (u64)-1 - amount) {
            profile_hybrid_flag(HPPHYF_COUNTER_SATURATED);
            return;
        }
        if (__atomic_compare_exchange_n(counter, &old, old + amount, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return;
    }
}

bool heap_profile_hybrid_enter(void)
{
    if (!profile_hybrid ||
        !__atomic_load_n(&profile_hybrid_recording, __ATOMIC_SEQ_CST))
        return false;
    u32 old = __atomic_load_n(&profile_hybrid_inflight, __ATOMIC_SEQ_CST);
    for (;;) {
        if (old == (u32)-1) {
            profile_hybrid_flag(HPPHYF_COUNTER_SATURATED);
            return false;
        }
        if (__atomic_compare_exchange_n(&profile_hybrid_inflight, &old,
                                        old + 1U, false,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            break;
    }
    if (!__atomic_load_n(&profile_hybrid_recording, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&profile_hybrid_inflight, 1U, __ATOMIC_SEQ_CST);
        return false;
    }
    return true;
}

static void profile_hybrid_leave(bool admitted)
{
    if (!admitted)
        return;
    u32 old = __atomic_fetch_sub(&profile_hybrid_inflight, 1U,
                                 __ATOMIC_SEQ_CST);
    if (!old)
        profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
}

bool heap_profile_hybrid_cache_path_enter(u32 cpu_index)
{
    if (cpu_index >= HEAP_PROFILE_HYBRID_CPU_COUNT ||
        !heap_profile_hybrid_enter()) {
        if (cpu_index >= HEAP_PROFILE_HYBRID_CPU_COUNT)
            profile_hybrid_flag(HPPHYF_CPU_COUNT_MISMATCH);
        return false;
    }
    u32 active_cpus = __atomic_add_fetch(&profile_hybrid_cache_active, 1U,
                                         __ATOMIC_ACQ_REL);
    if (!active_cpus) {
        profile_hybrid_flag(HPPHYF_COUNTER_SATURATED);
        profile_hybrid_leave(true);
        return false;
    }
    uint64_t *maximum =
        &profile_hybrid->header[HPPHY_MAX_CONCURRENT_CACHE_CPUS];
    uint64_t old = __atomic_load_n(maximum, __ATOMIC_RELAXED);
    while (old < active_cpus &&
           !__atomic_compare_exchange_n(maximum, &old, active_cpus, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    return true;
}

void heap_profile_hybrid_cache_path_leave(u32 cpu_index, bool admitted)
{
    if (!admitted)
        return;
    if (cpu_index >= HEAP_PROFILE_HYBRID_CPU_COUNT) {
        profile_hybrid_flag(HPPHYF_CPU_COUNT_MISMATCH);
        profile_hybrid_leave(true);
        return;
    }
    u32 previous = __atomic_fetch_sub(&profile_hybrid_cache_active, 1U,
                                      __ATOMIC_RELEASE);
    if (!previous)
        profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
    profile_hybrid_leave(true);
}

void heap_profile_hybrid_alloc_complete(bool admitted, u32 path,
                                        u32 context)
{
    if (!admitted)
        return;
    profile_hybrid_add(HPPHY_ROUTE_KMALLOC_ATTEMPTS, 1U);
    switch (path) {
    case HEAP_PROFILE_HYBRID_SLAB_SUCCESS:
        profile_hybrid_add(HPPHY_ROUTE_SLAB_SUCCESS, 1U);
        break;
    case HEAP_PROFILE_HYBRID_HEAP_FALLBACK_SUCCESS:
        profile_hybrid_add(HPPHY_ROUTE_HEAP_FALLBACK_SUCCESS, 1U);
        break;
    case HEAP_PROFILE_HYBRID_HEAP_DIRECT_SUCCESS:
        profile_hybrid_add(HPPHY_ROUTE_HEAP_DIRECT_SUCCESS, 1U);
        break;
    case HEAP_PROFILE_HYBRID_ALLOCATION_FAILURE:
        profile_hybrid_add(HPPHY_ROUTE_ALLOCATION_FAILURE, 1U);
        break;
    default:
        profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
        break;
    }
    u32 field = context < HEAP_PROFILE_CONTEXT_COUNT
        ? HPPHY_ROUTE_ALLOC_CONTEXT_NORMAL + context
        : HEAP_PROFILE_HYBRID_ROUTE_FIELDS;
    if (field < HPPHY_ROUTE_FIELD_COUNT)
        profile_hybrid_add(field, 1U);
    else
        profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
    profile_hybrid_leave(admitted);
}

void heap_profile_hybrid_free_complete(bool admitted, bool slab_owned,
                                       u32 context)
{
    if (!admitted)
        return;
    profile_hybrid_add(slab_owned ? HPPHY_ROUTE_SLAB_FREES
                                  : HPPHY_ROUTE_HEAP_FREES, 1U);
    u32 field = context < HEAP_PROFILE_CONTEXT_COUNT
        ? HPPHY_ROUTE_FREE_CONTEXT_NORMAL + context
        : HEAP_PROFILE_HYBRID_ROUTE_FIELDS;
    if (field < HPPHY_ROUTE_FIELD_COUNT)
        profile_hybrid_add(field, 1U);
    else
        profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
    profile_hybrid_leave(admitted);
}

void heap_profile_hybrid_reclaim_complete(bool admitted, bool succeeded,
                                          u64 cached_objects,
                                          u64 slab_pages,
                                          u64 empty_slab_pages)
{
    if (!admitted)
        return;
    profile_hybrid_add(HPPHY_ROUTE_RECLAIM_ATTEMPTS, 1U);
    if (!succeeded)
        profile_hybrid_add(HPPHY_ROUTE_RECLAIM_CALL_FAILURES, 1U);
    profile_hybrid_add(HPPHY_ROUTE_RECLAIM_CACHED_OBJECTS, cached_objects);
    profile_hybrid_add(HPPHY_ROUTE_RECLAIM_SLAB_BACKING_PAGES, slab_pages);
    profile_hybrid_add(HPPHY_ROUTE_RECLAIM_EMPTY_BACKING_PAGES,
                       empty_slab_pages);
}

void heap_profile_hybrid_reclaim_retry(bool admitted)
{
    if (admitted)
        profile_hybrid_add(HPPHY_ROUTE_RECLAIM_ALLOC_RETRIES, 1U);
}

void heap_profile_hybrid_backing_allocation_complete(bool admitted,
                                                     u32 failures)
{
    if (!admitted)
        return;
    profile_hybrid_add(HPPHY_ROUTE_SLAB_BACKING_ALLOCATION_FAILURES,
                       failures);
    profile_hybrid_leave(admitted);
}

static bool profile_hybrid_class_row(u32 index, slab_class_stats_t *stats)
{
    if (!profile_hybrid || index >= HEAP_PROFILE_HYBRID_CLASS_COUNT ||
        !slab_get_class_stats(index, stats))
        return false;
    uint64_t *row = profile_hybrid->classes[index];
    row[HPPHY_CLASS_SIZE] = slab_class_size(index);
    row[HPPHY_CLASS_LOCK_ACQUISITIONS] = stats->lock_acquisitions;
    row[HPPHY_CLASS_ALLOCATIONS] = stats->allocations;
    row[HPPHY_CLASS_FREES] = stats->frees;
    row[HPPHY_CLASS_ALLOCATION_FAILURES] = stats->allocation_failures;
    row[HPPHY_CLASS_BITMAP_WORDS_EXAMINED] = stats->bitmap_words_examined;
    row[HPPHY_CLASS_BACKING_ALLOCATIONS] = stats->backing_allocations;
    row[HPPHY_CLASS_BACKING_RELEASES] = stats->backing_releases;
    row[HPPHY_CLASS_TRANSITIONS_TO_FULL] = stats->transitions_to_full;
    row[HPPHY_CLASS_TRANSITIONS_TO_PARTIAL] =
        stats->transitions_to_partial;
    row[HPPHY_CLASS_RETIRED_SLABS] = stats->retired_slabs;
    row[HPPHY_CLASS_LIVE_SLOTS] = stats->live_slots;
    row[HPPHY_CLASS_PEAK_LIVE_SLOTS] = stats->peak_live_slots;
    row[HPPHY_CLASS_PEAK_SLAB_PAGES] = stats->peak_slab_pages;
    row[HPPHY_CLASS_PARTIAL_SLABS] = stats->partial_slabs;
    row[HPPHY_CLASS_FULL_SLABS] = stats->full_slabs;
    row[HPPHY_CLASS_EMPTY_SLABS] = stats->empty_slabs;
    row[HPPHY_CLASS_EMPTY_SLAB_REUSES] = stats->empty_slab_reuses;
    row[HPPHY_CLASS_CACHED_SLOTS] = stats->cached_slots;
    row[HPPHY_CLASS_CACHE_HITS] = stats->cache_hits;
    row[HPPHY_CLASS_CACHE_MISSES] = stats->cache_misses;
    row[HPPHY_CLASS_CACHE_REFILLS] = stats->cache_refills;
    row[HPPHY_CLASS_CACHE_REFILL_CONFLICTS] =
        stats->cache_refill_conflicts;
    row[HPPHY_CLASS_CACHE_REFILL_OBJECTS] = stats->cache_refill_objects;
    row[HPPHY_CLASS_CACHE_DRAIN_OBJECTS] = stats->cache_drain_objects;
    row[HPPHY_CLASS_CACHE_OVERFLOWS] = stats->cache_overflows;
    row[HPPHY_CLASS_CROSS_CPU_FREES] = stats->cross_cpu_frees;
    for (u32 field = 0; field < HEAP_PROFILE_HYBRID_CLASS_FIELDS; field++)
        if (row[field] == (u64)-1)
            profile_hybrid_flag(HPPHYF_COUNTER_SATURATED);
    return true;
}

static bool profile_hybrid_cpu_row(u32 cpu, u32 class_index,
                                   slab_cpu_class_stats_t *stats)
{
    if (!profile_hybrid || cpu >= HEAP_PROFILE_HYBRID_CPU_COUNT ||
        class_index >= HEAP_PROFILE_HYBRID_CLASS_COUNT ||
        !slab_get_cpu_class_stats(cpu, class_index, stats))
        return false;
    uint64_t *row = profile_hybrid->cpu_cache[cpu][class_index];
    row[HPPHY_CPU_ALLOCATIONS] = stats->allocations;
    row[HPPHY_CPU_FREES] = stats->frees;
    row[HPPHY_CPU_CACHE_HITS] = stats->hits;
    row[HPPHY_CPU_CACHE_MISSES] = stats->misses;
    row[HPPHY_CPU_CACHE_REFILLS] = stats->refills;
    row[HPPHY_CPU_CACHE_REFILL_CONFLICTS] = stats->refill_conflicts;
    row[HPPHY_CPU_CACHE_REFILL_OBJECTS] = stats->refill_objects;
    row[HPPHY_CPU_CACHE_DRAIN_OBJECTS] = stats->drain_objects;
    row[HPPHY_CPU_CACHE_OVERFLOWS] = stats->overflows;
    row[HPPHY_CPU_CROSS_CPU_FREES] = stats->cross_cpu_frees;
    row[HPPHY_CPU_CACHED_OBJECTS] = stats->cached_objects;
    row[HPPHY_CPU_REFILL_ACTIVE] = stats->refill_active;
    for (u32 field = 0; field < HEAP_PROFILE_HYBRID_CPU_FIELDS; field++)
        if (row[field] == (u64)-1)
            profile_hybrid_flag(HPPHYF_COUNTER_SATURATED);
    return true;
}

void heap_profile_hybrid_capture(void)
{
    if (!profile_hybrid || !profile_frozen)
        return;
    __atomic_store_n(&profile_hybrid_recording, 0U, __ATOMIC_SEQ_CST);
    uint64_t *header = profile_hybrid->header;
    /* The guest issues freeze from a quiescent diagnostic phase. Still use a
     * bounded wait for a public allocator operation admitted just before the
     * gate closed. If it cannot finish promptly, reject export rather than
     * copying route counters while a completion may still update them. */
    u32 inflight = 0U;
    for (u32 attempt = 0; attempt < 1000000U; attempt++) {
        inflight = __atomic_load_n(&profile_hybrid_inflight, __ATOMIC_SEQ_CST);
        if (!inflight)
            break;
        __asm__ volatile("pause" ::: "memory");
    }
    header[HPPHY_MAGIC] = HEAP_PROFILE_HYBRID_STATS_MAGIC;
    header[HPPHY_VERSION] = 2U;
    header[HPPHY_TOTAL_BYTES] = sizeof(*profile_hybrid);
    header[HPPHY_ONLINE_CPUS_BEGIN] = cpu_online_count();
    header[HPPHY_CLASS_COUNT] = SLAB_CLASS_COUNT;
    header[HPPHY_ROUTE_INFLIGHT_AT_FREEZE] = inflight;
    if (inflight) {
        profile_hybrid_flag(HPPHYF_ROUTE_INFLIGHT);
        profile_flag(HPPF_HYBRID_STATS_UNAVAILABLE);
        return;
    }
    if (!slab_is_initialized())
        profile_hybrid_flag(HPPHYF_SLAB_UNINITIALIZED);
    u32 cache_cpu_count = slab_cpu_cache_count_current();
    header[HPPHY_CACHE_CPU_COUNT] = cache_cpu_count;
    if (cache_cpu_count != header[HPPHY_ONLINE_CPUS_BEGIN] ||
        cache_cpu_count > HEAP_PROFILE_HYBRID_CPU_COUNT)
        profile_hybrid_flag(HPPHYF_CPU_COUNT_MISMATCH);
    for (u32 class_index = 0;
         class_index < HEAP_PROFILE_HYBRID_CLASS_COUNT; class_index++) {
        slab_class_stats_t class_stats;
        if (!profile_hybrid_class_row(class_index, &class_stats)) {
            profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
            continue;
        }
    }
    if (cache_cpu_count <= HEAP_PROFILE_HYBRID_CPU_COUNT) {
        for (u32 cpu = 0; cpu < cache_cpu_count; cpu++)
            for (u32 class_index = 0;
                 class_index < HEAP_PROFILE_HYBRID_CLASS_COUNT;
                 class_index++) {
                slab_cpu_class_stats_t cpu_stats;
                if (!profile_hybrid_cpu_row(cpu, class_index, &cpu_stats))
                    profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
            }
    }
    slab_classifier_stats_t classifier;
    if (!slab_get_classifier_stats(&classifier)) {
        profile_hybrid_flag(HPPHYF_STATS_UNAVAILABLE);
    } else {
        uint64_t *row = profile_hybrid->classifier;
        row[HPPHY_CLASSIFIER_ROOT_PAYLOAD_BYTES] =
            classifier.root_payload_bytes;
        row[HPPHY_CLASSIFIER_STATIC_ROOT_BYTES] =
            classifier.static_root_bytes;
        row[HPPHY_CLASSIFIER_MIDDLE_PAYLOAD_BYTES] =
            classifier.middle_payload_bytes;
        row[HPPHY_CLASSIFIER_LEAF_PAYLOAD_BYTES] =
            classifier.leaf_payload_bytes;
        row[HPPHY_CLASSIFIER_RETAINED_PAYLOAD_BYTES] =
            classifier.retained_payload_bytes;
        row[HPPHY_CLASSIFIER_PEAK_PAYLOAD_BYTES] =
            classifier.peak_payload_bytes;
        row[HPPHY_CLASSIFIER_ROOT_TABLES] = classifier.root_tables;
        row[HPPHY_CLASSIFIER_MIDDLE_TABLES] = classifier.middle_tables;
        row[HPPHY_CLASSIFIER_LEAF_TABLES] = classifier.leaf_tables;
        row[HPPHY_CLASSIFIER_ALLOCATION_FAILURES] =
            classifier.allocation_failures;
        header[HPPHY_CLASSIFIER_RETAINED_BYTES] =
            classifier.retained_payload_bytes;
        for (u32 field = 0; field < HEAP_PROFILE_HYBRID_CLASSIFIER_FIELDS;
             field++)
            if (row[field] == (u64)-1)
                profile_hybrid_flag(HPPHYF_COUNTER_SATURATED);
    }
    header[HPPHY_CPU_CACHE_METADATA_BYTES] =
        slab_cpu_cache_metadata_bytes();
    header[HPPHY_HEAP_MAPPED_BYTES] = heap_profile_mapped_bytes();
    header[HPPHY_HEAP_TOTAL_PAYLOAD] = heap_get_total_size();
    header[HPPHY_HEAP_USED_PAYLOAD] = heap_get_used_size();
    header[HPPHY_HEAP_FREE_PAYLOAD] = heap_get_free_size();
    header[HPPHY_EVENT_TRACE_BYTES] =
        profile_header[HPPH_TRACE_EVENT_CAPACITY] *
        sizeof(heap_profile_event_t);
    header[HPPHY_ROUTE_TRACE_BYTES] = sizeof(*profile_hybrid);
    usize event_payload = profile_header[HPPH_TRACE_EVENT_CAPACITY] *
                          sizeof(heap_profile_event_t);
    usize hybrid_payload = sizeof(*profile_hybrid);
    header[HPPHY_PROFILE_STORAGE_PAYLOAD_BYTES] =
        ((event_payload + 15U) & ~(usize)15U) +
        ((hybrid_payload + 15U) & ~(usize)15U);
    header[HPPHY_ONLINE_CPUS_END] = cpu_online_count();
    if (header[HPPHY_ONLINE_CPUS_END] != header[HPPHY_ONLINE_CPUS_BEGIN])
        profile_hybrid_flag(HPPHYF_CPU_COUNT_MISMATCH);
    if (__atomic_load_n(&header[HPPHY_FLAGS], __ATOMIC_RELAXED))
        profile_flag(HPPF_HYBRID_STATS_UNAVAILABLE);
}
#endif

bool heap_profile_copy_frozen_range(void *destination, usize offset,
                                    usize length)
{
    if (!profile_frozen || !destination || !length ||
        offset > profile_header[HPPH_TOTAL_BYTES] ||
        length > profile_header[HPPH_TOTAL_BYTES] - offset)
        return false;
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    if (profile_header[HPPH_FLAGS] & HPPF_HYBRID_STATS_UNAVAILABLE)
        return false;
#endif
    u8 *output = destination;
    usize base_bytes = sizeof(profile_data);
    while (length) {
        if (offset < base_bytes) {
            usize amount = base_bytes - offset;
            if (amount > length)
                amount = length;
            memcpy(output, (const u8 *)&profile_data + offset, amount);
            output += amount;
            offset += amount;
            length -= amount;
            continue;
        }
        usize event_bytes = profile_header[HPPH_TRACE_EVENT_CAPACITY] *
                            sizeof(*profile_events);
        usize event_end = base_bytes + event_bytes;
        if (offset < event_end) {
            if (!profile_events)
                return false;
            usize event_offset = offset - base_bytes;
            usize amount = event_end - offset;
            if (amount > length)
                amount = length;
            memcpy(output, (const u8 *)profile_events + event_offset, amount);
            output += amount;
            offset += amount;
            length -= amount;
            continue;
        }
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        if (!profile_hybrid)
            return false;
        usize hybrid_offset = offset - event_end;
        if (hybrid_offset >= sizeof(*profile_hybrid))
            return false;
        usize amount = sizeof(*profile_hybrid) - hybrid_offset;
        if (amount > length)
            amount = length;
        memcpy(output, (const u8 *)profile_hybrid + hybrid_offset, amount);
        output += amount;
        offset += amount;
        length -= amount;
#else
        return false;
#endif
    }
    return true;
}

u64 heap_profile_total_bytes(void)
{
    return profile_header[HPPH_TOTAL_BYTES];
}
#endif
