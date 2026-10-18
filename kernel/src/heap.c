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
static bool heap_header_valid(const heap_block_t *block);
static bool heap_block_valid(const heap_block_t *block);
static void heap_free_insert_locked(heap_block_t *block);
static void heap_free_remove_locked(heap_block_t *block);

#ifdef HEAP_PROFILE_DIAGNOSTICS
static void *heap_profile_fail_request(usize requested, bool profile_active)
{
    if (profile_active) {
        u64 flags = spin_lock_irqsave(&heap_lock);
        heap_profile_failed_locked(requested);
        heap_profile_request_leave_locked();
        spin_unlock_irqrestore(&heap_lock, flags);
    }
    return NULL;
}
#define HEAP_PROFILE_SUCCESS(ptr, requested, active) do { \
    if (active) { \
        heap_profile_allocated_locked((ptr), (requested), cpu_current_index()); \
        heap_profile_request_leave_locked(); \
    } \
} while (0)
#define HEAP_PROFILE_FREE(ptr) \
    heap_profile_freed_locked((ptr), cpu_current_index())
#define HEAP_PROFILE_FAIL(requested, active) \
    return heap_profile_fail_request((requested), (active))
#else
#define HEAP_PROFILE_SUCCESS(ptr, requested, active) ((void)0)
#define HEAP_PROFILE_FREE(ptr) ((void)0)
#define HEAP_PROFILE_FAIL(requested, active) return NULL
#endif

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
    block->next = 0;
    block->prev = 0;
    block->free_next = NULL;
    block->free_prev = NULL;
    
    kernel_heap.first = block;
    kernel_heap.last = block;
    heap_total_payload = block->size;
    heap_free_payload = block->size;
    heap_free_insert_locked(block);
#ifdef HEAP_PROFILE_DIAGNOSTICS
    heap_profile_init();
#endif
    heap_bootstrap_growth_open = true;
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
        spin_unlock_irqrestore(&heap_lock, flags);
        return complete;
    }

    heap_block_t *new_block = (heap_block_t *)old_end;

    kernel_heap.end =
        (void *)((u8 *)kernel_heap.end + pages * PAGE_SIZE);
    new_block->size = (pages * PAGE_SIZE) - sizeof(heap_block_t);
    new_block->free = true;
    new_block->in_free_bin = false;

    new_block->next = 0;
    new_block->prev = last;
    new_block->free_next = NULL;
    new_block->free_prev = NULL;

    last->next = new_block;
    kernel_heap.last = new_block;
    heap_total_payload += new_block->size;
    heap_free_payload += new_block->size;
    heap_free_insert_locked(new_block);
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

static heap_block_t *heap_free_find_locked(usize size)
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
            if (block->size >= size) return block;
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

void *kmalloc(usize size) {
    usize heap_capacity = (usize)(HEAP_LIMIT - HEAP_START);
    bool partial_growth = false;
#ifdef HEAP_PROFILE_DIAGNOSTICS
    usize requested = size;
    bool profile_active = heap_profile_request_enter();
#endif
    if (!size || size > (usize)-1 - 15U)
        HEAP_PROFILE_FAIL(size, profile_active);
    size = (size + 15) & ~15;
    if (heap_capacity <= sizeof(heap_block_t) ||
        size > heap_capacity - sizeof(heap_block_t))
        HEAP_PROFILE_FAIL(requested, profile_active);
    for (;;) {
        u64 flags = spin_lock_irqsave(&heap_lock);
        heap_block_t *current;

        if (!heap_verify("kmalloc enter"))
            panic("heap: verification failed before allocation");

        current = heap_free_find_locked(size);
        if (current) {
            usize allocated_size = current->size;
            usize remaining = allocated_size - size;
            bool split = remaining > sizeof(heap_block_t) + 16U;

            heap_free_remove_locked(current);
            current->free = false;
            if (split) {
                heap_block_t *new_block = (heap_block_t *)
                    ((u8 *)current + sizeof(heap_block_t) + size);

                new_block->size = remaining - sizeof(heap_block_t);
                new_block->free = true;
                new_block->in_free_bin = false;
                new_block->next = current->next;
                new_block->prev = current;
                new_block->free_next = NULL;
                new_block->free_prev = NULL;
                current->next = new_block;
                if (new_block->next) new_block->next->prev = new_block;
                else kernel_heap.last = new_block;
                current->size = size;
                heap_total_payload -= sizeof(heap_block_t);
                heap_used_payload += size;
                heap_free_payload -= size + sizeof(heap_block_t);
                heap_free_insert_locked(new_block);
            } else {
                heap_used_payload += allocated_size;
                heap_free_payload -= allocated_size;
            }
            if (!heap_block_valid(current))
                panic("heap: invalid block after allocation");
            if (!heap_verify("kmalloc exit"))
                panic("heap: verification failed after allocation");
            void *ptr = (void *)((u8 *)current + sizeof(heap_block_t));
            HEAP_PROFILE_SUCCESS(ptr, requested, profile_active);
            spin_unlock_irqrestore(&heap_lock, flags);
            return ptr;
        }
        spin_unlock_irqrestore(&heap_lock, flags);

        /* A VMM failure may still have published a usable mapped prefix.  It
         * gets one allocation retry, but an insufficient prefix does not
         * trigger an unbounded series of failed growth attempts. */
        if (partial_growth)
            HEAP_PROFILE_FAIL(requested, profile_active);

        bool interrupts_enabled = (flags & (1ULL << 9)) != 0;
        bool bootstrap = !interrupts_enabled;
        /* Growth can acquire a scheduler mutex and publish mappings. A
         * caller-held spinlock makes that operation non-sleepable even when
         * local interrupts happen to be enabled. Existing free blocks were
         * already handled above and remain usable in this context. */
        if (spinlock_depth_current() != 0U)
            HEAP_PROFILE_FAIL(requested, profile_active);
        if (bootstrap) {
            if (!heap_bootstrap_growth_open)
                HEAP_PROFILE_FAIL(requested, profile_active);
        } else if (!mutex_lock(&heap_growth_lock)) {
            HEAP_PROFILE_FAIL(requested, profile_active);
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
            HEAP_PROFILE_FAIL(requested, profile_active);
        }

        flags = spin_lock_irqsave(&heap_lock);
        bool suitable = heap_free_find_locked(size) != NULL;
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
            HEAP_PROFILE_FAIL(requested, profile_active);
        partial_growth = !complete;
    }
}

void kfree(void *ptr) {
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
    HEAP_PROFILE_FREE(ptr);
    heap_used_payload -= block->size;
    heap_free_payload += block->size;

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
