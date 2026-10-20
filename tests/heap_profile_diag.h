/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Private, profile-build-only interface shared by Pith and its guest fixture.
 * It is deliberately kept under tests/ and is not part of Mangrove's ABI. */
#include <stdint.h>

#define HEAP_PROFILE_DIAG_MAGIC UINT64_C(0x4850524f46303131)
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
#define HEAP_PROFILE_DIAG_VERSION 6U
#else
#define HEAP_PROFILE_DIAG_VERSION 5U
#endif
#define HEAP_PROFILE_EXACT_MAX 1024U
#define HEAP_PROFILE_LOG_BUCKETS 64U
#define HEAP_PROFILE_SIZE_SLOTS (HEAP_PROFILE_EXACT_MAX + 1U + HEAP_PROFILE_LOG_BUCKETS)
#define HEAP_PROFILE_PHASES 4U
#define HEAP_PROFILE_LIFETIME_BUCKETS 32U
#define HEAP_PROFILE_CPU_SLOTS 257U
#define HEAP_PROFILE_TRACK_CAPACITY 16384U
#define HEAP_PROFILE_CANDIDATES 5U
#define HEAP_PROFILE_MAX_CLASSES 32U
#define HEAP_PROFILE_CANDIDATE_FIELDS 10U
#define HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS 14U
#define HEAP_PROFILE_CANDIDATE_PHASE_FIELDS 5U
#define HEAP_PROFILE_RANGE_COUNT 7U
#define HEAP_PROFILE_RANGE_FIELDS 4U
#define HEAP_PROFILE_EXPORT_CHUNK_BYTES 4096U
#define HEAP_PROFILE_EVENT_CAPACITY 65536U
#define HEAP_PROFILE_EVENT_BYTES 24U
#define HEAP_PROFILE_HYBRID_CLASS_COUNT 20U
#define HEAP_PROFILE_HYBRID_CPU_COUNT 256U
#define HEAP_PROFILE_HYBRID_ROUTE_FIELDS 20U
#define HEAP_PROFILE_HYBRID_CLASSIFIER_FIELDS 10U
#define HEAP_PROFILE_HYBRID_CLASS_FIELDS 27U
#define HEAP_PROFILE_HYBRID_CPU_FIELDS 12U
#define HEAP_PROFILE_HYBRID_HEADER_FIELDS 19U
#define HEAP_PROFILE_HYBRID_STATS_MAGIC UINT64_C(0x4859425249445354)

#define HEAP_PROFILE_OP_MARK_PHASE 1U
#define HEAP_PROFILE_OP_FREEZE 2U
#define HEAP_PROFILE_OP_READ 3U

enum heap_profile_header_field {
    HPPH_MAGIC,
    HPPH_VERSION,
    HPPH_TOTAL_BYTES,
    HPPH_FLAGS,
    HPPH_CPU_COUNT,
    HPPH_FINAL_PHASE,
    HPPH_OPERATIONS,
    HPPH_ATTEMPTS,
    HPPH_SUCCESSES,
    HPPH_FAILURES,
    HPPH_FREES,
    HPPH_SUCCESS_BYTES,
    HPPH_FAILED_BYTES,
    HPPH_CROSS_CPU_FREES,
    HPPH_TRACKED_LIVE,
    HPPH_TRACKED_PEAK,
    HPPH_TRACKING_MISSES,
    HPPH_MAX_PROBE,
    HPPH_INFLIGHT_AT_FREEZE,
    HPPH_LIFETIME_SAMPLES,
    HPPH_LIFETIME_OPERATIONS,
    HPPH_ALLOC_CONTEXT_NORMAL,
    HPPH_ALLOC_CONTEXT_IRQ_DISABLED,
    HPPH_ALLOC_CONTEXT_SPINLOCK_HELD,
    HPPH_FAIL_CONTEXT_NORMAL,
    HPPH_FAIL_CONTEXT_IRQ_DISABLED,
    HPPH_FAIL_CONTEXT_SPINLOCK_HELD,
    HPPH_FREE_CONTEXT_NORMAL,
    HPPH_FREE_CONTEXT_IRQ_DISABLED,
    HPPH_FREE_CONTEXT_SPINLOCK_HELD,
    HPPH_TRACE_EVENT_COUNT,
    HPPH_TRACE_EVENT_CAPACITY,
    HPPH_FIELD_COUNT = 32
};

enum heap_profile_context {
    HEAP_PROFILE_CONTEXT_NORMAL = 0,
    HEAP_PROFILE_CONTEXT_IRQ_DISABLED = 1,
    HEAP_PROFILE_CONTEXT_SPINLOCK_HELD = 2,
    HEAP_PROFILE_CONTEXT_COUNT = 3
};

enum heap_profile_status_flag {
    HPPF_COUNTER_SATURATED = 1U << 0,
    HPPF_TRACKING_INCOMPLETE = 1U << 1,
    HPPF_OPERATION_SATURATED = 1U << 2,
    HPPF_PHASE_ERROR = 1U << 3,
    HPPF_INFLIGHT_AT_FREEZE = 1U << 4,
    HPPF_TRACE_OVERFLOW = 1U << 5,
    HPPF_TRACE_UNAVAILABLE = 1U << 6,
    HPPF_HYBRID_STATS_UNAVAILABLE = 1U << 7
};

enum heap_profile_hybrid_header_field {
    HPPHY_MAGIC,
    HPPHY_VERSION,
    HPPHY_TOTAL_BYTES,
    HPPHY_FLAGS,
    HPPHY_ONLINE_CPUS_BEGIN,
    HPPHY_ONLINE_CPUS_END,
    HPPHY_CACHE_CPU_COUNT,
    HPPHY_ROUTE_INFLIGHT_AT_FREEZE,
    HPPHY_HEAP_MAPPED_BYTES,
    HPPHY_HEAP_TOTAL_PAYLOAD,
    HPPHY_HEAP_USED_PAYLOAD,
    HPPHY_HEAP_FREE_PAYLOAD,
    HPPHY_CLASSIFIER_RETAINED_BYTES,
    HPPHY_CPU_CACHE_METADATA_BYTES,
    HPPHY_EVENT_TRACE_BYTES,
    HPPHY_ROUTE_TRACE_BYTES,
    HPPHY_CLASS_COUNT,
    HPPHY_PROFILE_STORAGE_PAYLOAD_BYTES,
    HPPHY_MAX_CONCURRENT_CACHE_CPUS,
    HPPHY_FIELD_COUNT
};

enum heap_profile_hybrid_flag {
    HPPHYF_COUNTER_SATURATED = 1U << 0,
    HPPHYF_ROUTE_INFLIGHT = 1U << 1,
    HPPHYF_SLAB_UNINITIALIZED = 1U << 2,
    HPPHYF_CPU_COUNT_MISMATCH = 1U << 3,
    HPPHYF_STATS_UNAVAILABLE = 1U << 4
};

enum heap_profile_hybrid_route_field {
    HPPHY_ROUTE_KMALLOC_ATTEMPTS,
    HPPHY_ROUTE_SLAB_SUCCESS,
    HPPHY_ROUTE_HEAP_FALLBACK_SUCCESS,
    HPPHY_ROUTE_HEAP_DIRECT_SUCCESS,
    HPPHY_ROUTE_ALLOCATION_FAILURE,
    HPPHY_ROUTE_SLAB_FREES,
    HPPHY_ROUTE_HEAP_FREES,
    HPPHY_ROUTE_ALLOC_CONTEXT_NORMAL,
    HPPHY_ROUTE_ALLOC_CONTEXT_IRQ_DISABLED,
    HPPHY_ROUTE_ALLOC_CONTEXT_SPINLOCK_HELD,
    HPPHY_ROUTE_FREE_CONTEXT_NORMAL,
    HPPHY_ROUTE_FREE_CONTEXT_IRQ_DISABLED,
    HPPHY_ROUTE_FREE_CONTEXT_SPINLOCK_HELD,
    HPPHY_ROUTE_RECLAIM_ATTEMPTS,
    HPPHY_ROUTE_RECLAIM_CALL_FAILURES,
    HPPHY_ROUTE_RECLAIM_CACHED_OBJECTS,
    HPPHY_ROUTE_RECLAIM_SLAB_BACKING_PAGES,
    HPPHY_ROUTE_RECLAIM_EMPTY_BACKING_PAGES,
    HPPHY_ROUTE_RECLAIM_ALLOC_RETRIES,
    HPPHY_ROUTE_SLAB_BACKING_ALLOCATION_FAILURES,
    HPPHY_ROUTE_FIELD_COUNT
};

enum heap_profile_hybrid_classifier_field {
    HPPHY_CLASSIFIER_ROOT_PAYLOAD_BYTES,
    HPPHY_CLASSIFIER_STATIC_ROOT_BYTES,
    HPPHY_CLASSIFIER_MIDDLE_PAYLOAD_BYTES,
    HPPHY_CLASSIFIER_LEAF_PAYLOAD_BYTES,
    HPPHY_CLASSIFIER_RETAINED_PAYLOAD_BYTES,
    HPPHY_CLASSIFIER_PEAK_PAYLOAD_BYTES,
    HPPHY_CLASSIFIER_ROOT_TABLES,
    HPPHY_CLASSIFIER_MIDDLE_TABLES,
    HPPHY_CLASSIFIER_LEAF_TABLES,
    HPPHY_CLASSIFIER_ALLOCATION_FAILURES,
    HPPHY_CLASSIFIER_FIELD_COUNT
};

enum heap_profile_hybrid_alloc_path {
    HEAP_PROFILE_HYBRID_SLAB_SUCCESS = 0U,
    HEAP_PROFILE_HYBRID_HEAP_FALLBACK_SUCCESS,
    HEAP_PROFILE_HYBRID_HEAP_DIRECT_SUCCESS,
    HEAP_PROFILE_HYBRID_ALLOCATION_FAILURE
};

enum heap_profile_hybrid_class_field {
    HPPHY_CLASS_SIZE,
    HPPHY_CLASS_LOCK_ACQUISITIONS,
    HPPHY_CLASS_ALLOCATIONS,
    HPPHY_CLASS_FREES,
    HPPHY_CLASS_ALLOCATION_FAILURES,
    HPPHY_CLASS_BITMAP_WORDS_EXAMINED,
    HPPHY_CLASS_BACKING_ALLOCATIONS,
    HPPHY_CLASS_BACKING_RELEASES,
    HPPHY_CLASS_TRANSITIONS_TO_FULL,
    HPPHY_CLASS_TRANSITIONS_TO_PARTIAL,
    HPPHY_CLASS_RETIRED_SLABS,
    HPPHY_CLASS_LIVE_SLOTS,
    HPPHY_CLASS_PEAK_LIVE_SLOTS,
    HPPHY_CLASS_PEAK_SLAB_PAGES,
    HPPHY_CLASS_PARTIAL_SLABS,
    HPPHY_CLASS_FULL_SLABS,
    HPPHY_CLASS_EMPTY_SLABS,
    HPPHY_CLASS_EMPTY_SLAB_REUSES,
    HPPHY_CLASS_CACHED_SLOTS,
    HPPHY_CLASS_CACHE_HITS,
    HPPHY_CLASS_CACHE_MISSES,
    HPPHY_CLASS_CACHE_REFILLS,
    HPPHY_CLASS_CACHE_REFILL_CONFLICTS,
    HPPHY_CLASS_CACHE_REFILL_OBJECTS,
    HPPHY_CLASS_CACHE_DRAIN_OBJECTS,
    HPPHY_CLASS_CACHE_OVERFLOWS,
    HPPHY_CLASS_CROSS_CPU_FREES,
    HPPHY_CLASS_FIELD_COUNT
};

enum heap_profile_hybrid_cpu_field {
    HPPHY_CPU_ALLOCATIONS,
    HPPHY_CPU_FREES,
    HPPHY_CPU_CACHE_HITS,
    HPPHY_CPU_CACHE_MISSES,
    HPPHY_CPU_CACHE_REFILLS,
    HPPHY_CPU_CACHE_REFILL_CONFLICTS,
    HPPHY_CPU_CACHE_REFILL_OBJECTS,
    HPPHY_CPU_CACHE_DRAIN_OBJECTS,
    HPPHY_CPU_CACHE_OVERFLOWS,
    HPPHY_CPU_CROSS_CPU_FREES,
    HPPHY_CPU_CACHED_OBJECTS,
    HPPHY_CPU_REFILL_ACTIVE,
    HPPHY_CPU_FIELD_COUNT
};

enum heap_profile_event_kind {
    HEAP_PROFILE_EVENT_ALLOCATED = 1U,
    HEAP_PROFILE_EVENT_FREED = 2U,
    HEAP_PROFILE_EVENT_FAILED = 3U
};

/* The event array is diagnostic-only and follows allocator commit order.
 * Object IDs are profile-local and never expose kernel pointer values. */
typedef struct heap_profile_event {
    uint32_t object_id;
    /* RVA from __kernel_text_virt_start, or UINT32_MAX when the caller is
     * outside the linked kernel text range. */
    uint32_t caller_text_offset;
    uint64_t requested_bytes;
    uint16_t cpu;
    uint8_t kind;
    uint8_t context;
    uint8_t phase;
    uint8_t reserved1;
    uint16_t reserved2;
} heap_profile_event_t;

_Static_assert(sizeof(heap_profile_event_t) == HEAP_PROFILE_EVENT_BYTES,
               "heap profile trace record layout changed");

/* Each row is a fixed-width little-endian sequence of uint64_t values.
 * Size-row fields are attempts, successes, failures, frees, successful bytes,
 * failed bytes, current live objects, current live requested bytes, peak live
 * objects, peak live requested bytes, lifetime samples, lifetime operation
 * sum, and cross-CPU frees. */
#define HEAP_PROFILE_SIZE_FIELDS 13U

/* Per phase and size row: successful allocations, frees, peak live objects,
 * peak live requested bytes, live objects at phase end, live bytes at phase
 * end, objects from this phase surviving its boundary, and their bytes. */
#define HEAP_PROFILE_PHASE_FIELDS 8U

/* Request sizes 1..1024 have exact slots. Slot 0 contains zero-byte requests.
 * Larger sizes use the additional 64 floor(log2(size)) buckets. */
typedef struct heap_profile_diag_request {
    uint64_t magic;
    uint64_t operation;
    uint64_t phase;
    uint64_t snapshot_address;
    uint64_t snapshot_bytes;
} heap_profile_diag_request_t;

typedef struct heap_profile_diag_snapshot {
    uint64_t header[HPPH_FIELD_COUNT];
    uint64_t sizes[HEAP_PROFILE_SIZE_SLOTS][HEAP_PROFILE_SIZE_FIELDS];
    uint64_t ranges[HEAP_PROFILE_RANGE_COUNT][HEAP_PROFILE_RANGE_FIELDS];
    uint64_t phases[HEAP_PROFILE_PHASES][HEAP_PROFILE_SIZE_SLOTS]
                   [HEAP_PROFILE_PHASE_FIELDS];
    uint64_t lifetime[HEAP_PROFILE_LOG_BUCKETS]
                     [HEAP_PROFILE_LIFETIME_BUCKETS];
    uint64_t cpu[HEAP_PROFILE_CPU_SLOTS][3];
    uint64_t candidate[HEAP_PROFILE_CANDIDATES][HEAP_PROFILE_MAX_CLASSES]
                      [HEAP_PROFILE_CANDIDATE_FIELDS];
    uint64_t candidate_phase[HEAP_PROFILE_PHASES][HEAP_PROFILE_CANDIDATES]
                            [HEAP_PROFILE_MAX_CLASSES]
                            [HEAP_PROFILE_PHASE_FIELDS];
    uint64_t candidate_summary[HEAP_PROFILE_CANDIDATES]
                              [HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS];
    uint64_t candidate_phase_summary[HEAP_PROFILE_PHASES]
                                    [HEAP_PROFILE_CANDIDATES]
                                    [HEAP_PROFILE_CANDIDATE_PHASE_FIELDS];
    uint64_t candidate_cpu_mask[HEAP_PROFILE_CANDIDATES]
                               [HEAP_PROFILE_MAX_CLASSES][4];
    heap_profile_event_t events[];
} heap_profile_diag_snapshot_t;

/* Appended to the heap-backed diagnostic export only in explicitly enabled
 * hybrid runtime-profile builds. Rows are copied from existing allocator
 * counters; this structure adds no static BSS. Per-record snapshots are
 * bounded and internally locked but are not one global stop-the-world view. */
typedef struct heap_profile_hybrid_snapshot {
    uint64_t header[HEAP_PROFILE_HYBRID_HEADER_FIELDS];
    uint64_t routes[HEAP_PROFILE_HYBRID_ROUTE_FIELDS];
    uint64_t classifier[HEAP_PROFILE_HYBRID_CLASSIFIER_FIELDS];
    uint64_t classes[HEAP_PROFILE_HYBRID_CLASS_COUNT]
                    [HEAP_PROFILE_HYBRID_CLASS_FIELDS];
    uint64_t cpu_cache[HEAP_PROFILE_HYBRID_CPU_COUNT]
                      [HEAP_PROFILE_HYBRID_CLASS_COUNT]
                      [HEAP_PROFILE_HYBRID_CPU_FIELDS];
} heap_profile_hybrid_snapshot_t;

_Static_assert(sizeof(heap_profile_hybrid_snapshot_t) ==
                   (HEAP_PROFILE_HYBRID_HEADER_FIELDS +
                    HEAP_PROFILE_HYBRID_ROUTE_FIELDS +
                    HEAP_PROFILE_HYBRID_CLASSIFIER_FIELDS +
                    HEAP_PROFILE_HYBRID_CLASS_COUNT *
                        HEAP_PROFILE_HYBRID_CLASS_FIELDS +
                    HEAP_PROFILE_HYBRID_CPU_COUNT *
                        HEAP_PROFILE_HYBRID_CLASS_COUNT *
                        HEAP_PROFILE_HYBRID_CPU_FIELDS) * 8U,
               "hybrid profile snapshot layout changed");
