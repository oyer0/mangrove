/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Private, profile-build-only interface shared by Pith and its guest fixture.
 * It is deliberately kept under tests/ and is not part of Mangrove's ABI. */
#include <stdint.h>

#define HEAP_PROFILE_DIAG_MAGIC UINT64_C(0x4850524f46303131)
#define HEAP_PROFILE_DIAG_VERSION 4U
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
    HPPF_TRACE_UNAVAILABLE = 1U << 6
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
    uint32_t reserved0;
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
