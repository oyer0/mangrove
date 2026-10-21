/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <heap.h>
#include <vmm.h>
#include <pmm.h>
#include <kprint.h>
#include <spinlock.h>
#include <mutex.h>
#include <panic.h>
#include <stddef.h>
#ifdef HEAP_PROFILE_DIAGNOSTICS
#include <heap_profile.h>
#include <heap_profile_diag.h>
#include <cpu.h>
extern char __kernel_text_virt_start[];
extern char __kernel_text_virt_end[];

static u32 heap_profile_callsite_offset(uintptr_t address)
{
    uintptr_t start = (uintptr_t)__kernel_text_virt_start;
    uintptr_t end = (uintptr_t)__kernel_text_virt_end;

    if (address < start || address >= end || address - start > 0xffffffffU)
        return 0xffffffffU;
    return (u32)(address - start);
}

#define HEAP_PROFILE_SITE_ARGUMENT \
    , heap_profile_callsite_offset( \
        (uintptr_t)__builtin_return_address(0))
#endif
#ifdef KERNEL_HYBRID_ALLOCATOR
#include <slab.h>
#endif

_Static_assert((sizeof(heap_block_t) & 15U) == 0,
               "heap block header must preserve 16-byte payload alignment");
_Static_assert(sizeof(heap_block_t) == 48,
               "x86-64 heap metadata must remain 48 bytes per block");

static heap_t kernel_heap;
static spinlock_t heap_lock;
static mutex_t heap_growth_lock;
#define HEAP_FREE_BIN_COUNT 32U
static heap_block_t *heap_free_bins[HEAP_FREE_BIN_COUNT];
static u32 heap_free_bin_bitmap;
static u64 heap_total_payload;
static u64 heap_used_payload;
static u64 heap_free_payload;
static bool heap_bootstrap_growth_open;
static uintptr_t heap_published_end;
/* Extent generations are process-lifetime identifiers. Do not reset this
 * counter when a test fixture reinitializes the heap; stale copied handles
 * must never become valid again after an address is reused. */
static u32 heap_page_extent_next_generation;
static bool heap_header_valid(const heap_block_t *block);
static bool heap_block_valid(const heap_block_t *block);
static void heap_free_insert_locked(heap_block_t *block);
static void heap_free_remove_locked(heap_block_t *block);

#ifdef HEAP_PROFILE_DIAGNOSTICS
static u32 heap_profile_context_current(void)
{
    u64 rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");
    if (spinlock_depth_current())
        return HEAP_PROFILE_CONTEXT_SPINLOCK_HELD;
    return (rflags & (1ULL << 9)) ? HEAP_PROFILE_CONTEXT_NORMAL :
                                   HEAP_PROFILE_CONTEXT_IRQ_DISABLED;
}

static void *heap_profile_fail_request(usize requested, bool profile_active,
                                       u32 context, u32 caller_text_offset)
{
    if (profile_active) {
        u64 flags = spin_lock_irqsave(&heap_lock);
        heap_profile_failed_locked(requested, cpu_current_index(), context,
                                   caller_text_offset);
        heap_profile_request_leave_locked();
        spin_unlock_irqrestore(&heap_lock, flags);
    }
    return NULL;
}
#define HEAP_PROFILE_SUCCESS(ptr, requested, active, context, site) do { \
    if (active) { \
        heap_profile_allocated_locked((ptr), (requested), \
                                      cpu_current_index(), (context), \
                                      (site)); \
        heap_profile_request_leave_locked(); \
    } \
} while (0)
#define HEAP_PROFILE_FREE(ptr, context, caller_text_offset) \
    heap_profile_freed_locked((ptr), cpu_current_index(), (context), \
                              (caller_text_offset))
#define HEAP_PROFILE_FAIL(requested, active, context, caller_text_offset) \
    return heap_profile_fail_request((requested), (active), (context), \
                                     (caller_text_offset))
#define HEAP_PROFILE_SITE_PARAMETER \
    , u32 caller_text_offset
#define HEAP_PROFILE_SITE_VALUE caller_text_offset
#else
#define HEAP_PROFILE_SUCCESS(ptr, requested, active, context, site) ((void)0)
#define HEAP_PROFILE_FREE(ptr, context, caller_text_offset) ((void)0)
#define HEAP_PROFILE_FAIL(requested, active, context, caller_text_offset) \
    return NULL
#define HEAP_PROFILE_SITE_ARGUMENT
#define HEAP_PROFILE_SITE_PARAMETER
#define HEAP_PROFILE_SITE_VALUE 0U
#endif

static void heap_deallocate(void *ptr, usize minimum_payload_size,
                            u32 expected_extent_generation
                            HEAP_PROFILE_SITE_PARAMETER);

/* Return a conservative upper bound for any block or free-list traversal.
 * Every block has a header and at least 16 bytes of payload. */
static usize heap_maximum_blocks_locked(void)
{
    uintptr_t start = (uintptr_t)kernel_heap.start;
    uintptr_t end = (uintptr_t)kernel_heap.end;
    uintptr_t limit = (uintptr_t)HEAP_LIMIT;

    if (start != (uintptr_t)HEAP_START || end < start || end > limit ||
        ((end - start) & (PAGE_SIZE - 1U)))
        panic("heap: invalid mapped range");

    return (usize)((end - start) /
                   (sizeof(heap_block_t) + 16U)) + 1U;
}

/* Bins cover power-of-two payload ranges starting at 16 bytes. */
static u32 heap_bin_index(usize size)
{
    usize units = size >> 4;
    u32 index = 0;

    if (!units) return 0;
    while (units > 1U && index + 1U < HEAP_FREE_BIN_COUNT) {
        units >>= 1;
        index++;
    }
    return index;
}

bool heap_init(void) {
    page_table_t *pml4 = vmm_get_kernel_pml4();
    phys_addr_t frames[HEAP_INITIAL_PAGES];
    usize allocated = 0;

    spinlock_init(&heap_lock);
    mutex_init(&heap_growth_lock);
    for (u32 i = 0; i < HEAP_FREE_BIN_COUNT; i++)
        heap_free_bins[i] = NULL;
    heap_free_bin_bitmap = 0;
    heap_total_payload = 0;
    heap_used_payload = 0;
    heap_free_payload = 0;
    heap_bootstrap_growth_open = false;
    __atomic_store_n(&heap_published_end, 0U, __ATOMIC_RELAXED);

    if (!pml4)
        return false;

    for (usize i = 0; i < HEAP_INITIAL_PAGES; i++) {
        frames[i] = pmm_alloc_frame();
        if (!frames[i]) {
            while (allocated)
                pmm_free_frame(frames[--allocated]);
            return false;
        }
        allocated++;
    }

    for (usize i = 0; i < HEAP_INITIAL_PAGES; i++) {
        if (!vmm_map(
            pml4,
            (void *)(HEAP_START + (i * PAGE_SIZE)),
            frames[i],
            PTE_PRESENT | PTE_READWRITE | PTE_NX
        )) {
            /* The failing leaf was not installed.  Mappings before it remain
             * live in the kernel root, but this frame and the suffix are still
             * PMM-owned and must be returned. */
            for (usize remaining = i; remaining < HEAP_INITIAL_PAGES;
                 remaining++)
                pmm_free_frame(frames[remaining]);
            return false;
        }
    }

    kernel_heap.start = (void *)HEAP_START;
    kernel_heap.end = (void *)(HEAP_START + (HEAP_INITIAL_PAGES * PAGE_SIZE));

    heap_block_t *block = (heap_block_t *)HEAP_START;

    block->size = (HEAP_INITIAL_PAGES * PAGE_SIZE) - sizeof(heap_block_t);
    block->free = true;
    block->in_free_bin = false;
    block->page_extent_generation = 0U;
    block->next = 0;
    block->prev = 0;
    block->free_next = NULL;
    block->free_prev = NULL;
    
    kernel_heap.first = block;
    kernel_heap.last = block;
    heap_total_payload = block->size;
    heap_free_payload = block->size;
    heap_free_insert_locked(block);
    __atomic_store_n(&heap_published_end, (uintptr_t)kernel_heap.end,
                     __ATOMIC_RELEASE);
    /* The heap is fully published at this point.  Diagnostic profile storage
     * may now use the raw heap path without participating in its own trace.
     * This remains inside the single-BSP bootstrap growth window. */
    heap_bootstrap_growth_open = true;
#ifdef HEAP_PROFILE_DIAGNOSTICS
    heap_profile_init();
#endif
    return true;
}

#ifdef HEAP_PROFILE_DIAGNOSTICS
bool heap_profile_mark_phase(u32 phase)
{
    u64 flags = spin_lock_irqsave(&heap_lock);
    bool result = heap_profile_set_phase_locked(phase);
    spin_unlock_irqrestore(&heap_lock, flags);
    return result;
}

void heap_profile_freeze(void)
{
    u64 flags = spin_lock_irqsave(&heap_lock);
    heap_profile_freeze_locked();
    spin_unlock_irqrestore(&heap_lock, flags);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    heap_profile_hybrid_capture();
#endif
}

bool heap_profile_read(void *destination, usize offset, usize length)
{
    u64 flags = spin_lock_irqsave(&heap_lock);
    bool result = heap_profile_copy_frozen_range(destination, offset, length);
    spin_unlock_irqrestore(&heap_lock, flags);
    return result;
}
#endif

void heap_bootstrap_finish(void)
{
    /* Called by the init graph immediately before its first STI.  Once closed,
     * an IRQ-disabled caller can use existing blocks but cannot grow the heap. */
    heap_bootstrap_growth_open = false;
}

/* A growth reservation is serialized separately so physical frames can be
 * acquired before heap_lock.  Kernel mappings may synchronously invalidate
 * remote CPUs, so the heap lock must not be held while those mappings are
 * published. */
static bool heap_grow_reserved(const phys_addr_t *frames, u32 *mapped_count,
                               bool bootstrap)
{
    page_table_t *pml4 = vmm_get_kernel_pml4();
    void *old_end;
    u64 flags;
    bool complete = true;

    if (mapped_count) *mapped_count = 0;

    flags = spin_lock_irqsave(&heap_lock);
    old_end = kernel_heap.end;
    if ((uintptr_t)old_end > HEAP_LIMIT - HEAP_INITIAL_PAGES * PAGE_SIZE) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return false;
    }
    spin_unlock_irqrestore(&heap_lock, flags);

    for (usize i = 0; i < HEAP_INITIAL_PAGES; i++)
    {
        bool mapped = bootstrap
            ? vmm_map_kernel_bootstrap(
                  pml4, (void *)((u8 *)old_end + (i * PAGE_SIZE)),
                  frames[i], PTE_PRESENT | PTE_READWRITE | PTE_NX)
            : vmm_map(pml4,
                      (void *)((u8 *)old_end + (i * PAGE_SIZE)),
                      frames[i], PTE_PRESENT | PTE_READWRITE | PTE_NX);
        if (!mapped) {
            complete = false;
            break;
        }
        if (mapped_count) *mapped_count = (u32)(i + 1U);
    }

    flags = spin_lock_irqsave(&heap_lock);
    if (kernel_heap.end != old_end) {
        spin_unlock_irqrestore(&heap_lock, flags);
        panic("heap: concurrent growth changed heap end");
    }

    u32 pages = mapped_count ? *mapped_count : 0;
    heap_block_t *last = kernel_heap.last;
    if (pages && (!last || !heap_block_valid(last)))
        panic("heap: invalid tail during growth");
    if (pages && (last->free != last->in_free_bin ||
                  (!last->free &&
                   (last->free_next || last->free_prev))))
        panic("heap: inconsistent tail allocation state during growth");

    if (!pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return false;
    }

    if (last->free)
    {
        heap_free_remove_locked(last);
        kernel_heap.end =
            (void *)((u8 *)kernel_heap.end + pages * PAGE_SIZE);
        last->size += pages * PAGE_SIZE;
        heap_total_payload += (u64)pages * PAGE_SIZE;
        heap_free_payload += (u64)pages * PAGE_SIZE;
        heap_free_insert_locked(last);
        __atomic_store_n(&heap_published_end, (uintptr_t)kernel_heap.end,
                         __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&heap_lock, flags);
        return complete;
    }

    heap_block_t *new_block = (heap_block_t *)old_end;

    kernel_heap.end =
        (void *)((u8 *)kernel_heap.end + pages * PAGE_SIZE);
    new_block->size = (pages * PAGE_SIZE) - sizeof(heap_block_t);
    new_block->free = true;
    new_block->in_free_bin = false;
    new_block->page_extent_generation = 0U;

    new_block->next = 0;
    new_block->prev = last;
    new_block->free_next = NULL;
    new_block->free_prev = NULL;

    last->next = new_block;
    kernel_heap.last = new_block;
    heap_total_payload += new_block->size;
    heap_free_payload += new_block->size;
    heap_free_insert_locked(new_block);
    __atomic_store_n(&heap_published_end, (uintptr_t)kernel_heap.end,
                     __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&heap_lock, flags);
    return complete;
}

/* Check only the current block and its neighbours in normal builds. Never
 * dereference a header until its address and payload bounds are validated. */
static bool heap_header_valid(const heap_block_t *block)
{
    uintptr_t address = (uintptr_t)block;
    uintptr_t start = (uintptr_t)kernel_heap.start;
    uintptr_t end = (uintptr_t)kernel_heap.end;

    return block && address >= start && address < end && !(address & 15U) &&
        end - address >= sizeof(*block) && !(block->size & 15U) &&
        block->size <= end - address - sizeof(*block);
}

static bool heap_block_valid(const heap_block_t *block)
{
    if (!heap_header_valid(block)) return false;
    if (block->free && block->page_extent_generation) return false;
    uintptr_t end = (uintptr_t)block + sizeof(*block) + block->size;
    if (block->next) {
        if (!heap_header_valid(block->next) ||
            (uintptr_t)block->next != end || block->next->prev != block)
            return false;
    } else if (end != (uintptr_t)kernel_heap.end ||
               block != kernel_heap.last) return false;
    if (block->prev) {
        if (!heap_header_valid(block->prev) || block->prev->next != block ||
            (uintptr_t)block->prev + sizeof(*block) + block->prev->size !=
                (uintptr_t)block) return false;
    } else if (block != kernel_heap.first ||
               (uintptr_t)block != (uintptr_t)kernel_heap.start) return false;
    return true;
}

static void heap_free_insert_locked(heap_block_t *block)
{
    u32 index;
    heap_block_t *head;

    if (!block || !block->free || block->in_free_bin ||
        !heap_block_valid(block) ||
        block->free_next || block->free_prev)
        panic("heap: invalid free-list insertion");

    index = heap_bin_index(block->size);
    head = heap_free_bins[index];
    if (((heap_free_bin_bitmap >> index) & 1U) != (head != NULL))
        panic("heap: free-list bitmap mismatch on insertion");
    if (head && (!heap_header_valid(head) || !heap_block_valid(head) ||
                 !head->free || !head->in_free_bin ||
                 heap_bin_index(head->size) != index || head->free_prev))
        panic("heap: invalid free-list head");

    block->free_prev = NULL;
    block->free_next = head;
    if (head) {
        if (head->free_next &&
            (!heap_header_valid(head->free_next) ||
             !heap_block_valid(head->free_next) ||
             !head->free_next->free || !head->free_next->in_free_bin ||
             heap_bin_index(head->free_next->size) != index ||
             head->free_next->free_prev != head))
            panic("heap: broken free-list head successor");
        head->free_prev = block;
    }
    else heap_free_bin_bitmap |= (u32)1U << index;
    block->in_free_bin = true;
    heap_free_bins[index] = block;
}

static void heap_free_remove_locked(heap_block_t *block)
{
    u32 index;
    heap_block_t *previous;
    heap_block_t *next;

    if (!block || !block->free || !block->in_free_bin ||
        !heap_block_valid(block))
        panic("heap: invalid free-list removal");

    index = heap_bin_index(block->size);
    previous = block->free_prev;
    next = block->free_next;
    if (((heap_free_bin_bitmap >> index) & 1U) !=
        (heap_free_bins[index] != NULL))
        panic("heap: free-list bitmap mismatch on removal");

    if (previous) {
        if (!heap_header_valid(previous) || !heap_block_valid(previous) ||
            !previous->free || !previous->in_free_bin ||
            heap_bin_index(previous->size) != index ||
            previous->free_next != block)
            panic("heap: broken previous free link");
    } else if (heap_free_bins[index] != block) {
        panic("heap: free block missing from bin");
    }

    if (next && (!heap_header_valid(next) || !heap_block_valid(next) ||
                 !next->free || !next->in_free_bin ||
                 heap_bin_index(next->size) != index ||
                 next->free_prev != block))
        panic("heap: broken next free link");

    if (previous) previous->free_next = next;
    else heap_free_bins[index] = next;
    if (next) next->free_prev = previous;
    else if (!heap_free_bins[index])
        heap_free_bin_bitmap &= ~((u32)1U << index);

    block->free_next = NULL;
    block->free_prev = NULL;
    block->in_free_bin = false;
    if ((((heap_free_bin_bitmap >> index) & 1U) != 0) !=
        (heap_free_bins[index] != NULL))
        panic("heap: free-list bitmap mismatch after removal");
}

typedef struct {
    uintptr_t payload;
    uintptr_t prefix_span;
    usize allocation_size;
    usize prefix_payload;
    usize suffix_payload;
    bool has_prefix;
    bool has_suffix;
} heap_layout_t;

/* Describe a placement inside one free block without modifying allocator
 * state.  Alignment padding is represented by a valid leading block; tiny
 * leading/trailing fragments are never emitted. */
static bool heap_layout_for_block_locked(heap_block_t *block, usize size,
                                         usize alignment,
                                         heap_layout_t *layout)
{
    uintptr_t block_address = (uintptr_t)block;
    uintptr_t payload_start = block_address + sizeof(*block);
    uintptr_t block_end = payload_start + block->size;
    uintptr_t aligned;
    uintptr_t allocation_end;
    uintptr_t prefix_header;
    uintptr_t prefix_span;
    uintptr_t suffix_span;

    if (!layout || alignment < 16U || (alignment & (alignment - 1U)) ||
        payload_start > (uintptr_t)-1 - (alignment - 1U))
        return false;

    aligned = (payload_start + alignment - 1U) &
              ~((uintptr_t)alignment - 1U);
    if (aligned < sizeof(*block))
        return false;
    prefix_header = aligned - sizeof(*block);
    if (prefix_header < block_address)
        return false;
    prefix_span = prefix_header - block_address;

    /* Keep the heap's established 32-byte minimum split payload. Advancing
     * one alignment unit preserves alignment and makes the prefix usable. */
    if (prefix_span && prefix_span < sizeof(*block) + 32U) {
        if (aligned > (uintptr_t)-1 - alignment)
            return false;
        aligned += alignment;
        prefix_header = aligned - sizeof(*block);
        prefix_span = prefix_header - block_address;
    }

    if (aligned > block_end || size > block_end - aligned)
        return false;
    allocation_end = aligned + size;
    suffix_span = block_end - allocation_end;

    layout->payload = aligned;
    layout->prefix_span = prefix_span;
    layout->has_prefix = prefix_span != 0;
    layout->prefix_payload = prefix_span
        ? (usize)(prefix_span - sizeof(*block)) : 0;
    layout->has_suffix = suffix_span > sizeof(*block) + 16U;
    layout->suffix_payload = layout->has_suffix
        ? (usize)(suffix_span - sizeof(*block)) : 0;
    layout->allocation_size = layout->has_suffix
        ? size : (usize)(block_end - aligned);
    return true;
}

static heap_block_t *heap_free_find_layout_locked(usize size,
                                                   usize alignment,
                                                   heap_layout_t *selected)
{
    u32 first_index = heap_bin_index(size);
    u32 candidates = heap_free_bin_bitmap & (~(u32)0U << first_index);
    usize maximum_blocks = heap_maximum_blocks_locked();

    for (u32 index = 0; index < HEAP_FREE_BIN_COUNT; index++) {
        bool bit_set = (heap_free_bin_bitmap & ((u32)1U << index)) != 0;
        if (bit_set != (heap_free_bins[index] != NULL))
            panic("heap: free-list bitmap mismatch during search");
        if (heap_free_bins[index] &&
            (!heap_header_valid(heap_free_bins[index]) ||
             !heap_block_valid(heap_free_bins[index]) ||
             !heap_free_bins[index]->free ||
             !heap_free_bins[index]->in_free_bin ||
             heap_free_bins[index]->free_prev ||
             heap_bin_index(heap_free_bins[index]->size) != index))
            panic("heap: invalid free-list head during search");
    }

    while (candidates) {
        u32 index = first_index;
        heap_block_t *block;
        heap_block_t *previous = NULL;
        usize visited = 0;

        while (!(candidates & ((u32)1U << index))) index++;
        for (block = heap_free_bins[index]; block;
             block = block->free_next) {
            if (++visited > maximum_blocks)
                panic("heap: free-list traversal exceeds heap bound");
            if (!heap_header_valid(block) || !heap_block_valid(block) ||
                !block->free || !block->in_free_bin ||
                block->free_prev != previous ||
                heap_bin_index(block->size) != index)
                panic("heap: invalid free-list candidate");
            if (block->free_next &&
                (!heap_header_valid(block->free_next) ||
                 !heap_block_valid(block->free_next) ||
                 !block->free_next->free ||
                 !block->free_next->in_free_bin ||
                 block->free_next->free_prev != block))
                panic("heap: broken candidate free link");
            heap_layout_t candidate;
            if (block->size >= size &&
                heap_layout_for_block_locked(block, size, alignment,
                                             &candidate)) {
                if (selected) *selected = candidate;
                return block;
            }
            previous = block;
        }
        candidates &= ~((u32)1U << index);
    }
    return NULL;
}

#if HEAP_VERIFY
static bool heap_verify(const char *where) {
    heap_block_t *current = kernel_heap.first;
    usize maximum_blocks = heap_maximum_blocks_locked();
    usize block_count = 0;
    usize physical_free_count = 0;
    usize indexed_free_count = 0;
    u64 total_payload = 0;
    u64 used_payload = 0;
    u64 free_payload = 0;

    if (!current || (uintptr_t)current != (uintptr_t)kernel_heap.start) {
        kprint("[HEAP BUG at %s] invalid first block %p (start=%p)\n",
               where, current, kernel_heap.start);
        return false;
    }

    while (current != 0) {
        if (++block_count > maximum_blocks || !heap_block_valid(current)) {
            kprint("[HEAP BUG at %s] invalid block ptr %p (start=%p end=%p)\n",
                   where, current, kernel_heap.start, kernel_heap.end);
            return false;
        }
        total_payload += current->size;
        if (current->free) {
            if (!current->in_free_bin) {
                kprint("[HEAP BUG at %s] free block absent from index: %p\n",
                       where, current);
                return false;
            }
            physical_free_count++;
            free_payload += current->size;
        } else {
            if (current->in_free_bin || current->free_next ||
                current->free_prev) {
                kprint("[HEAP BUG at %s] allocated block is indexed: %p\n",
                       where, current);
                return false;
            }
            used_payload += current->size;
        }
        current = current->next;
    }

    for (u32 index = 0; index < HEAP_FREE_BIN_COUNT; index++) {
        heap_block_t *previous = NULL;
        heap_block_t *block = heap_free_bins[index];
        bool bit_set = (heap_free_bin_bitmap & ((u32)1U << index)) != 0;

        if (bit_set != (block != NULL)) return false;
        while (block) {
            if (++indexed_free_count > maximum_blocks ||
                !heap_header_valid(block) || !heap_block_valid(block) ||
                !block->free || !block->in_free_bin ||
                heap_bin_index(block->size) != index ||
                block->free_prev != previous)
                return false;
            previous = block;
            block = block->free_next;
        }
    }

    if (physical_free_count != indexed_free_count ||
        heap_total_payload != total_payload ||
        heap_used_payload != used_payload || heap_free_payload != free_payload ||
        total_payload != used_payload + free_payload) {
        kprint("[HEAP BUG at %s] block/index/accounting mismatch\n", where);
        return false;
    }

    /* Verify exact membership as well as cardinality. This quadratic check is
     * compiled only into diagnostic builds, never the normal allocator. */
    for (current = kernel_heap.first; current; current = current->next) {
        if (!current->free) continue;
        usize occurrences = 0;
        usize visited = 0;
        u32 index = heap_bin_index(current->size);
        for (heap_block_t *block = heap_free_bins[index]; block;
             block = block->free_next) {
            if (++visited > maximum_blocks) return false;
            if (block == current) occurrences++;
        }
        if (occurrences != 1U) {
            kprint("[HEAP BUG at %s] free block membership count %u: %p\n",
                   where, (u32)occurrences, current);
            return false;
        }
    }

    return physical_free_count == indexed_free_count &&
        heap_total_payload == total_payload &&
        heap_used_payload == used_payload && heap_free_payload == free_payload;
}
#else
static bool heap_verify(const char *where)
{
    (void)where;
    return true;
}
#endif

static void *heap_allocate(usize size, usize alignment
                            HEAP_PROFILE_SITE_PARAMETER) {
    usize heap_capacity = (usize)(HEAP_LIMIT - HEAP_START);
    bool partial_growth = false;
#ifdef HEAP_PROFILE_DIAGNOSTICS
    usize requested = size;
    u32 profile_context = heap_profile_context_current();
    bool profile_active = heap_profile_request_enter();
#endif
    if (!size || alignment < 16U || (alignment & (alignment - 1U)) ||
        size > (usize)-1 - 15U)
        HEAP_PROFILE_FAIL(size, profile_active, profile_context,
                          HEAP_PROFILE_SITE_VALUE);
    size = (size + 15) & ~15;
    if (heap_capacity <= sizeof(heap_block_t) ||
        size > heap_capacity - sizeof(heap_block_t))
        HEAP_PROFILE_FAIL(requested, profile_active, profile_context,
                          HEAP_PROFILE_SITE_VALUE);
    for (;;) {
        u64 flags = spin_lock_irqsave(&heap_lock);
        heap_block_t *current;

        if (!heap_verify("kmalloc enter"))
            panic("heap: verification failed before allocation");

        heap_layout_t layout;
        current = heap_free_find_layout_locked(size, alignment, &layout);
        if (current) {
            usize original_size = current->size;
            heap_block_t *original_next = current->next;
            heap_block_t *allocated;
            heap_block_t *trailing = NULL;
            usize leading_payload = layout.prefix_payload;
            usize trailing_payload = layout.suffix_payload;
            u32 added_headers = (layout.has_prefix ? 1U : 0U) +
                                (layout.has_suffix ? 1U : 0U);

            heap_free_remove_locked(current);
            if (layout.has_prefix) {
                allocated = (heap_block_t *)(layout.payload -
                                             sizeof(heap_block_t));
                current->size = layout.prefix_payload;
                current->free = true;
                current->in_free_bin = false;
                current->page_extent_generation = 0U;
                current->next = allocated;
                current->free_next = NULL;
                current->free_prev = NULL;
            } else {
                allocated = current;
            }

            if (layout.has_suffix) {
                trailing = (heap_block_t *)(layout.payload + size);
                trailing->size = trailing_payload;
                trailing->free = true;
                trailing->in_free_bin = false;
                trailing->page_extent_generation = 0U;
                trailing->next = original_next;
                trailing->prev = allocated;
                trailing->free_next = NULL;
                trailing->free_prev = NULL;
                allocated->next = trailing;
            } else {
                allocated->next = original_next;
            }
            allocated->size = layout.allocation_size;
            allocated->free = false;
            allocated->in_free_bin = false;
            allocated->page_extent_generation = 0U;
            allocated->free_next = NULL;
            allocated->free_prev = NULL;
            if (!layout.has_prefix)
                allocated->prev = current->prev;
            if (layout.has_prefix)
                allocated->prev = current;

            if (original_next)
                original_next->prev = trailing ? trailing : allocated;
            else
                kernel_heap.last = trailing ? trailing : allocated;

            if (leading_payload)
                heap_free_insert_locked(current);
            if (trailing)
                heap_free_insert_locked(trailing);

            if ((u64)added_headers * sizeof(heap_block_t) >
                    heap_total_payload ||
                original_size < leading_payload + trailing_payload ||
                heap_free_payload < original_size - leading_payload -
                                    trailing_payload)
                panic("heap: aligned split accounting underflow");
            heap_total_payload -=
                (u64)added_headers * sizeof(heap_block_t);
            heap_used_payload += allocated->size;
            heap_free_payload -= original_size - leading_payload -
                                 trailing_payload;

            if (!heap_block_valid(allocated) ||
                (layout.payload & (alignment - 1U)) ||
                (layout.has_prefix && !heap_block_valid(current)) ||
                (trailing && !heap_block_valid(trailing)) ||
                (uintptr_t)allocated + sizeof(*allocated) != layout.payload)
                panic("heap: invalid block after allocation");
            if (!heap_verify("kmalloc exit"))
                panic("heap: verification failed after allocation");
            void *ptr = (void *)layout.payload;
            HEAP_PROFILE_SUCCESS(ptr, requested, profile_active,
                                 profile_context, HEAP_PROFILE_SITE_VALUE);
            spin_unlock_irqrestore(&heap_lock, flags);
            return ptr;
        }
        spin_unlock_irqrestore(&heap_lock, flags);

        /* A VMM failure may still have published a usable mapped prefix.  It
         * gets one allocation retry, but an insufficient prefix does not
         * trigger an unbounded series of failed growth attempts. */
        if (partial_growth)
            HEAP_PROFILE_FAIL(requested, profile_active, profile_context,
                              HEAP_PROFILE_SITE_VALUE);

        bool interrupts_enabled = (flags & (1ULL << 9)) != 0;
        bool bootstrap = !interrupts_enabled;
        /* Growth can acquire a scheduler mutex and publish mappings. A
         * caller-held spinlock makes that operation non-sleepable even when
         * local interrupts happen to be enabled. Existing free blocks were
         * already handled above and remain usable in this context. */
        if (spinlock_depth_current() != 0U)
            HEAP_PROFILE_FAIL(requested, profile_active, profile_context,
                              HEAP_PROFILE_SITE_VALUE);
        if (bootstrap) {
            if (!heap_bootstrap_growth_open)
                HEAP_PROFILE_FAIL(requested, profile_active, profile_context,
                                  HEAP_PROFILE_SITE_VALUE);
        } else if (!mutex_lock(&heap_growth_lock)) {
            HEAP_PROFILE_FAIL(requested, profile_active, profile_context,
                              HEAP_PROFILE_SITE_VALUE);
        }

        /* Ordinary growth is serialized by the mutex.  During the explicitly
         * bounded single-BSP bootstrap window, interrupts are disabled and no
         * AP is online, so growth cannot be preempted or race another grower;
         * it must not depend on a scheduler-owned mutex. */
        phys_addr_t frames[HEAP_INITIAL_PAGES];
        u32 frame_count = 0;
        bool allocation_failed = false;
        for (; frame_count < HEAP_INITIAL_PAGES; frame_count++) {
            frames[frame_count] = pmm_alloc_frame();
            if (!frames[frame_count]) {
                allocation_failed = true;
                break;
            }
        }
        if (allocation_failed) {
            while (frame_count) pmm_free_frame(frames[--frame_count]);
            if (!bootstrap)
                mutex_unlock(&heap_growth_lock);
            HEAP_PROFILE_FAIL(requested, profile_active, profile_context,
                              HEAP_PROFILE_SITE_VALUE);
        }

        flags = spin_lock_irqsave(&heap_lock);
        bool suitable = heap_free_find_layout_locked(size, alignment,
                                                      NULL) != NULL;
        if (suitable) {
            spin_unlock_irqrestore(&heap_lock, flags);
            for (u32 i = 0; i < HEAP_INITIAL_PAGES; i++)
                pmm_free_frame(frames[i]);
            if (!bootstrap)
                mutex_unlock(&heap_growth_lock);
            continue;
        }
        spin_unlock_irqrestore(&heap_lock, flags);

        u32 mapped_count = 0;
        bool complete = heap_grow_reserved(frames, &mapped_count, bootstrap);
        /* Successfully mapped frames are now heap-owned.  Only frames that
         * never became reachable through a leaf mapping return to PMM. */
        for (u32 i = mapped_count; i < HEAP_INITIAL_PAGES; i++)
            pmm_free_frame(frames[i]);
        if (!bootstrap)
            mutex_unlock(&heap_growth_lock);
        if (!mapped_count)
            HEAP_PROFILE_FAIL(requested, profile_active, profile_context,
                              HEAP_PROFILE_SITE_VALUE);
        partial_growth = !complete;
    }
}

void *kmalloc(usize size)
{
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    bool profile_route = heap_profile_hybrid_enter();
    u32 route_context = heap_profile_context_current();
#endif
#ifdef KERNEL_HYBRID_ALLOCATOR
    if (!size) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_alloc_complete(
            profile_route, HEAP_PROFILE_HYBRID_ALLOCATION_FAILURE,
            route_context);
#endif
        return NULL;
    }
#if KERNEL_HYBRID_ALLOCATOR_CUTOFF < 256 || \
    KERNEL_HYBRID_ALLOCATOR_CUTOFF > SLAB_MAX_OBJECT_SIZE
#error "KERNEL_HYBRID_ALLOCATOR_CUTOFF is outside the supported range"
#endif
    if (size <= KERNEL_HYBRID_ALLOCATOR_CUTOFF) {
        void *object = slab_cpu_alloc(size);
        if (object) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
            heap_profile_hybrid_alloc_complete(
                profile_route, HEAP_PROFILE_HYBRID_SLAB_SUCCESS,
                route_context);
#endif
            return object;
        }
    }
#endif
    void *pointer = heap_allocate(size, 16U HEAP_PROFILE_SITE_ARGUMENT);
#ifdef KERNEL_HYBRID_ALLOCATOR
    if (!pointer && size < (usize)(HEAP_LIMIT - HEAP_START)) {
        slab_reclaim_result_t reclaimed = {0};
        bool reclaim_succeeded = slab_reclaim_bounded(
            SLAB_RECLAIM_PRESSURE_CACHE_RECORDS,
            SLAB_RECLAIM_PRESSURE_OBJECTS, &reclaimed);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_reclaim_complete(
            profile_route, reclaim_succeeded,
            reclaimed.cached_objects_returned,
            reclaimed.slab_pages_released,
            reclaimed.empty_slab_pages_released);
#endif
        if (reclaim_succeeded) {
            if (size <= KERNEL_HYBRID_ALLOCATOR_CUTOFF &&
                reclaimed.cached_objects_returned) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
                heap_profile_hybrid_reclaim_retry(profile_route);
#endif
                pointer = slab_cpu_alloc(size);
                if (pointer) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
                    heap_profile_hybrid_alloc_complete(
                        profile_route, HEAP_PROFILE_HYBRID_SLAB_SUCCESS,
                        route_context);
#endif
                    return pointer;
                }
            }
            /* A large indexed-heap request can benefit only when reclaim
             * actually returned backing pages to that heap. A partial cache
             * drain alone is not reported as heap capacity. Retry once. */
            if (reclaimed.slab_pages_released ||
                reclaimed.empty_slab_pages_released) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
                heap_profile_hybrid_reclaim_retry(profile_route);
#endif
                pointer = heap_allocate(size, 16U HEAP_PROFILE_SITE_ARGUMENT);
            }
        }
    }
#endif
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    u32 path = !pointer ? HEAP_PROFILE_HYBRID_ALLOCATION_FAILURE
        : size <= KERNEL_HYBRID_ALLOCATOR_CUTOFF
            ? HEAP_PROFILE_HYBRID_HEAP_FALLBACK_SUCCESS
            : HEAP_PROFILE_HYBRID_HEAP_DIRECT_SUCCESS;
    heap_profile_hybrid_alloc_complete(profile_route, path, route_context);
#endif
    return pointer;
}

void *heap_alloc_raw(usize size)
{
    return heap_allocate(size, 16U HEAP_PROFILE_SITE_ARGUMENT);
}

/* The caller keeps the newly allocated pointer private until this lease is
 * installed, so tagging it after heap_allocate() cannot race with a release. */
static u32 heap_page_extent_assign_generation(void *base, usize bytes)
{
    uintptr_t payload_address = (uintptr_t)base;
    uintptr_t heap_start;
    uintptr_t heap_end;
    heap_block_t *block;
    u32 generation;

    if (!base || (payload_address & (PAGE_SIZE - 1U)))
        panic("heap: invalid page extent allocation result");

    u64 flags = spin_lock_irqsave(&heap_lock);
    heap_start = (uintptr_t)kernel_heap.start;
    heap_end = (uintptr_t)kernel_heap.end;
    if (payload_address < heap_start || payload_address >= heap_end ||
        payload_address - heap_start < sizeof(heap_block_t))
        panic("heap: page extent allocation outside mapped heap");
    block = (heap_block_t *)(payload_address - sizeof(heap_block_t));
    if (!heap_block_valid(block) || block->free || block->in_free_bin ||
        block->page_extent_generation || block->size < bytes)
        panic("heap: page extent allocation lost heap ownership");
    if (heap_page_extent_next_generation == (u32)-1) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return 0U;
    }
    generation = ++heap_page_extent_next_generation;
    block->page_extent_generation = generation;
    spin_unlock_irqrestore(&heap_lock, flags);
    return generation;
}

bool heap_alloc_page_extent(usize page_count, heap_page_extent_t *extent)
{
    if (!extent || extent->base || extent->page_count || extent->generation)
        return false;
    if (!page_count || page_count > (usize)-1 / PAGE_SIZE)
        return false;

    usize bytes = page_count * PAGE_SIZE;
    void *base = heap_allocate(bytes, PAGE_SIZE HEAP_PROFILE_SITE_ARGUMENT);
    if (!base)
        return false;
    u32 generation = heap_page_extent_assign_generation(base, bytes);
    if (!generation) {
        heap_deallocate(base, 0U, 0U HEAP_PROFILE_SITE_ARGUMENT);
        return false;
    }
    extent->base = base;
    extent->page_count = page_count;
    extent->generation = generation;
    return true;
}

void heap_free_page_extent(heap_page_extent_t *extent)
{
    if (!extent || !extent->base || !extent->page_count ||
        !extent->generation ||
        extent->page_count > (usize)-1 / PAGE_SIZE ||
        ((uintptr_t)extent->base & (PAGE_SIZE - 1U)))
        panic("heap: invalid page extent release");

    usize bytes = extent->page_count * PAGE_SIZE;
    heap_deallocate(extent->base, bytes, extent->generation
                    HEAP_PROFILE_SITE_ARGUMENT);
    extent->base = NULL;
    extent->page_count = 0U;
    extent->generation = 0U;
}

bool heap_address_is_mapped(const void *ptr)
{
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)kernel_heap.start;
    uintptr_t end = __atomic_load_n(&heap_published_end, __ATOMIC_ACQUIRE);

    return end > start && address >= start && address < end;
}

bool heap_address_range_is_mapped(const void *ptr, usize size)
{
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)kernel_heap.start;
    uintptr_t end = __atomic_load_n(&heap_published_end, __ATOMIC_ACQUIRE);

    return ptr && size && end > start && address >= start && address < end &&
        size <= end - address;
}

static void heap_deallocate(void *ptr, usize minimum_payload_size,
                            u32 expected_extent_generation
                            HEAP_PROFILE_SITE_PARAMETER)
{
#ifdef HEAP_PROFILE_DIAGNOSTICS
    u32 profile_context = heap_profile_context_current();
#endif
    if (ptr == 0) {
        return;
    }

    u64 flags = spin_lock_irqsave(&heap_lock);
    if (!heap_verify("kfree enter"))
        panic("heap: verification failed before free");

    uintptr_t payload_address = (uintptr_t)ptr;
    uintptr_t heap_start = (uintptr_t)kernel_heap.start;
    uintptr_t heap_end = (uintptr_t)kernel_heap.end;
    if (payload_address < heap_start || payload_address >= heap_end ||
        payload_address - heap_start < sizeof(heap_block_t) ||
        (payload_address & 15U))
        panic("heap: pointer outside allocation payload range");
    heap_block_t *block = (heap_block_t *)(payload_address -
                                           sizeof(heap_block_t));

    if (!heap_block_valid(block) || block->free || block->in_free_bin ||
        block->free_next || block->free_prev)
        panic("heap: invalid or already freed block");
    if (block->page_extent_generation != expected_extent_generation)
        panic("heap: allocation released through wrong ownership handle");
    if (block->size < minimum_payload_size)
        panic("heap: page extent exceeds allocation block");
    HEAP_PROFILE_FREE(ptr, profile_context, HEAP_PROFILE_SITE_VALUE);
    heap_used_payload -= block->size;
    heap_free_payload += block->size;
    block->page_extent_generation = 0U;

    if (block->next != 0 && block->next->free) {
        heap_block_t *next = block->next;

        heap_free_remove_locked(next);
        block->size += sizeof(heap_block_t) + next->size;
        block->next = next->next;
        heap_total_payload += sizeof(heap_block_t);
        heap_free_payload += sizeof(heap_block_t);

        if (block->next != 0) {
            block->next->prev = block;
        } else kernel_heap.last = block;
    }

    if (block->prev != 0 && block->prev->free) {
        heap_block_t *prev = block->prev;

        heap_free_remove_locked(prev);
        prev->size += sizeof(heap_block_t) + block->size;
        prev->next = block->next;
        heap_total_payload += sizeof(heap_block_t);
        heap_free_payload += sizeof(heap_block_t);

        if (prev->next != 0) {
            prev->next->prev = prev;
        } else kernel_heap.last = prev;
        block = prev;
    }

    if (!heap_block_valid(block)) panic("heap: invalid block after free");
    block->free = true;
    heap_free_insert_locked(block);
    if (!heap_verify("kfree exit"))
        panic("heap: verification failed after free");
    spin_unlock_irqrestore(&heap_lock, flags);
}

void heap_free_raw(void *ptr)
{
    heap_deallocate(ptr, 0U, 0U HEAP_PROFILE_SITE_ARGUMENT);
}

void kfree(void *ptr)
{
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    bool profile_route = ptr && heap_profile_hybrid_enter();
    u32 route_context = heap_profile_context_current();
#endif
#ifdef KERNEL_HYBRID_ALLOCATOR
    if (ptr && slab_cpu_try_free(ptr)) {
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        heap_profile_hybrid_free_complete(profile_route, true,
                                          route_context);
#endif
        return;
    }
#endif
    heap_deallocate(ptr, 0U, 0U HEAP_PROFILE_SITE_ARGUMENT);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    if (ptr)
        heap_profile_hybrid_free_complete(profile_route, false,
                                          route_context);
#endif
}

void heap_dump(void)
{
    u64 flags = spin_lock_irqsave(&heap_lock);
    heap_block_t *current = kernel_heap.first;
    int i = 0;

    while (current != 0)
    {
        current = current->next;
        i++;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
}

u64 heap_get_total_size(void)
{
    u64 flags = spin_lock_irqsave(&heap_lock);
    u64 total = heap_total_payload;
    spin_unlock_irqrestore(&heap_lock, flags);
    return total;
}

u64 heap_get_used_size(void)
{
    u64 flags = spin_lock_irqsave(&heap_lock);
    u64 used = heap_used_payload;
    spin_unlock_irqrestore(&heap_lock, flags);
    return used;
}

u64 heap_get_free_size(void)
{
    u64 flags = spin_lock_irqsave(&heap_lock);
    u64 free = heap_free_payload;
    spin_unlock_irqrestore(&heap_lock, flags);
    return free;
}

#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
u64 heap_profile_mapped_bytes(void)
{
    uintptr_t end = __atomic_load_n(&heap_published_end, __ATOMIC_ACQUIRE);
    return end >= HEAP_START ? (u64)(end - HEAP_START) : 0U;
}
#endif
