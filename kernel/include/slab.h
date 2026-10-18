/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define SLAB_CLASS_COUNT 20U
#define SLAB_MAX_OBJECT_SIZE 384U
#define SLAB_OBJECT_AREA_OFFSET 96U

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
} slab_class_stats_t;

/* Private core API. The range is the page-aligned heap virtual reservation;
 * production callers pass [HEAP_START, HEAP_LIMIT). */
bool slab_init(void *heap_start, usize heap_capacity);
void *slab_alloc(usize size);
/* Returns true when a slab object was freed; false means indexed-heap owner. */
bool slab_try_free(void *ptr);
u16 slab_class_size(u32 class_index);
u16 slab_class_slots(u32 class_index);
bool slab_get_class_stats(u32 class_index, slab_class_stats_t *stats);
bool slab_verify(void);
