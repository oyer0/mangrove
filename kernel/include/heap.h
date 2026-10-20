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

/* Kernel-private backing for one page-aligned slab.  The returned block is
 * virtually page-aligned and remains ordinary heap-owned storage; it is not
 * physically contiguous or DMA-safe. */
void *heap_alloc_aligned_page(void);
void heap_free_aligned_page(void *ptr);
/* Lock-free range check for heap addresses.  Mapped heap pages are never
 * unmapped, so the published upper bound is monotonic. */
bool heap_address_is_mapped(const void *ptr);
bool heap_address_range_is_mapped(const void *ptr, usize size);

u64 heap_get_total_size(void);
u64 heap_get_used_size(void);
u64 heap_get_free_size(void);

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
