/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define SLAB_CLASS_COUNT 20U
#define SLAB_MAX_OBJECT_SIZE 384U
#define SLAB_OBJECT_AREA_OFFSET 112U
#define SLAB_LOCAL_CACHE_CAPACITY 2U

typedef struct {
    u64 lock_acquisitions;
    u64 allocations;
    u64 frees;
    u64 allocation_failures;
    u64 bitmap_words_examined;
    u64 backing_allocations;
    u64 backing_releases;
    u64 transitions_to_full;
    u64 transitions_to_partial;
    u64 retired_slabs;
    u64 live_slots;
    u64 peak_live_slots;
    u64 partial_slabs;
    u64 full_slabs;
    u64 empty_slabs;
    u64 empty_slab_reuses;
    u64 cached_slots;
    u64 cache_hits;
    u64 cache_misses;
    u64 cache_refills;
    u64 cache_refill_conflicts;
    u64 cache_refill_objects;
    u64 cache_drain_objects;
    u64 cache_overflows;
    u64 cross_cpu_frees;
} slab_class_stats_t;

typedef struct {
    /* root_payload_bytes is zero: the bounded root directory is static. */
    u64 root_payload_bytes;
    u64 static_root_bytes;
    u64 middle_payload_bytes;
    u64 leaf_payload_bytes;
    u64 retained_payload_bytes;
    u64 peak_payload_bytes;
    u64 root_tables;
    u64 middle_tables;
    u64 leaf_tables;
    u64 allocation_failures;
} slab_classifier_stats_t;

/* Private core API. The range is the page-aligned heap virtual reservation;
 * production callers pass [HEAP_START, HEAP_LIMIT). */
bool slab_init(void *heap_start, usize heap_capacity);
void *slab_alloc(usize size);
/* Returns true when a slab object was freed; false means indexed-heap owner. */
bool slab_try_free(void *ptr);
/* Diagnostic slab fast path. Ordinary kmalloc/kfree do not dispatch here. */
void *slab_cpu_alloc(usize size);
bool slab_cpu_try_free(void *ptr);
/* Drain bounded per-CPU magazines without sleeping. For a complete drain,
 * callers must first quiesce all slab allocation/free users; concurrent
 * operations can refill a cache after this function has visited it. */
usize slab_cpu_drain_all(void);
u64 slab_cpu_cache_metadata_bytes(void);
/* Return all class-local empty reserves to the indexed heap without sleeping. */
usize slab_drain_empty(void);
u16 slab_class_size(u32 class_index);
u16 slab_class_slots(u32 class_index);
bool slab_get_class_stats(u32 class_index, slab_class_stats_t *stats);
bool slab_get_classifier_stats(slab_classifier_stats_t *stats);
bool slab_verify(void);

#ifdef SLAB_CORE_GUEST_TEST
bool slab_guest_test_run(void);
#endif
#ifdef SLAB_CORE_GUEST_BENCHMARK
bool slab_guest_benchmark_run(void);
#endif
#ifdef SLAB_CORE_GUEST_FREE_PROFILE
#define SLAB_FREE_PROFILE_SAMPLE_CAPACITY 64U

typedef enum {
    SLAB_FREE_PROFILE_DISABLED = 0,
    SLAB_FREE_PROFILE_END_TO_END,
    SLAB_FREE_PROFILE_CLASSIFY,
    SLAB_FREE_PROFILE_LOCK,
    SLAB_FREE_PROFILE_VALIDATE,
    SLAB_FREE_PROFILE_SLOT,
    SLAB_FREE_PROFILE_MUTATE,
    SLAB_FREE_PROFILE_RELEASE,
} slab_free_profile_stage_t;

typedef enum {
    SLAB_FREE_PROFILE_NONFINAL = 1,
    SLAB_FREE_PROFILE_FULL_TO_PARTIAL,
    SLAB_FREE_PROFILE_FINAL,
} slab_free_profile_case_t;

typedef struct {
    u64 ticks;
    u32 cpu_index;
    u32 class_index;
    u32 stage;
    u32 free_case;
} slab_free_profile_sample_t;

bool slab_free_profile_configure(slab_free_profile_stage_t stage,
                                 slab_free_profile_case_t free_case,
                                 u32 class_index, u32 cpu_index,
                                 u32 sample_stride);
void slab_free_profile_disable(void);
bool slab_free_profile_read(slab_free_profile_sample_t *samples,
                            u32 capacity, u32 *sample_count,
                            u32 *observed_count, u32 *lost_count,
                            u32 *unexpected_lost_count,
                            u32 *lost_reason, u32 *lost_ordinal);
#endif
