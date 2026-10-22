/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Heap RAM is permanently mapped into the high kernel vmalloc region. */
#define HEAP_START KERNEL_HEAP_BASE
#define HEAP_LIMIT KERNEL_HEAP_LIMIT
#define HEAP_INITIAL_PAGES 4

bool heap_init(void);
/* Close the single-CPU, pre-IRQ growth window immediately before STI. */
void heap_bootstrap_finish(void);
void *kmalloc(usize size);
void kfree(void *ptr);

/* Internal indexed-heap entry points for allocator metadata. These bypass
 * any future small-object dispatch and must never be used for DMA storage. */
void *heap_alloc_raw(usize size);
void heap_free_raw(void *ptr);

/* A page extent is one exclusive, page-aligned virtual allocation owned by
 * the indexed heap. Its physical frames need not be contiguous. Initialize
 * the output handle to zero before allocation; it must be empty on entry and
 * remains empty on failure. The generation binds release to this allocation,
 * so a copied stale handle cannot release a later allocation at the same
 * address. A successful release consumes the handle. This is not a DMA or
 * frame-reclamation API. */
typedef struct {
    void *base;
    usize page_count;
    u32 generation;
} heap_page_extent_t;

bool heap_alloc_page_extent(usize page_count, heap_page_extent_t *extent);
void heap_free_page_extent(heap_page_extent_t *extent);
/* Lock-free range check for the currently committed heap prefix. A bounded
 * tail trim may reduce the published upper bound after SMP unmapping. */
bool heap_address_is_mapped(const void *ptr);
bool heap_address_range_is_mapped(const void *ptr, usize size);

typedef struct {
    u32 groups_released;
    u32 frames_released;
    u64 bytes_released;
    /* First frame from the first released group; a nonzero value lets a
     * bounded diagnostic confirm that the PMM can hand the frame out again. */
    u64 first_frame_released;
} heap_reclaim_result_t;

/* Reclaim at most max_groups complete, owned four-page tail groups. Requires
 * enabled interrupts and no spinlock held. Returns false when the caller's
 * context or growth serialization is unavailable. */
bool heap_reclaim_bounded(u32 max_groups, heap_reclaim_result_t *result);
u64 heap_get_mapped_size(void);

u64 heap_get_total_size(void);
u64 heap_get_used_size(void);
u64 heap_get_free_size(void);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
u64 heap_profile_mapped_bytes(void);
#endif

void heap_dump(void);

#ifdef HEAP_PROFILE_DIAGNOSTICS
/* Private guest-test controls; absent from ordinary Pith builds. */
bool heap_profile_mark_phase(u32 phase);
void heap_profile_freeze(void);
bool heap_profile_read(void *destination, usize offset, usize length);
u64 heap_profile_total_bytes(void);
#endif

typedef struct heap_block {
    usize size;
    bool free;
    bool in_free_bin;
    u32 page_extent_generation;

    struct heap_block *next;
    struct heap_block *prev;
    struct heap_block *free_next;
    struct heap_block *free_prev;
} heap_block_t;

typedef struct {
    void *start;
    void *end;

    heap_block_t *first;
    heap_block_t *last;
} heap_t;
