/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <heap.h>
#include <vmm.h>
#include <pmm.h>
#include <kprint.h>
#include <spinlock.h>

static heap_t kernel_heap;
static spinlock_t heap_lock;
static spinlock_t heap_growth_lock;

void heap_init(void) {
    page_table_t *pml4 = vmm_get_kernel_pml4();

    spinlock_init(&heap_lock);
    spinlock_init(&heap_growth_lock);

    for (usize i = 0; i < HEAP_INITIAL_PAGES; i++) {
        phys_addr_t frame = pmm_alloc_frame();
        if (!frame) return;

        vmm_map(
            pml4,
            (void *)(HEAP_START + (i * PAGE_SIZE)),
            frame,
            PTE_PRESENT | PTE_READWRITE
        );
    }

    kernel_heap.start = (void *)HEAP_START;
    kernel_heap.end = (void *)(HEAP_START + (HEAP_INITIAL_PAGES * PAGE_SIZE));

    heap_block_t *block = (heap_block_t *)HEAP_START;

    block->size = (HEAP_INITIAL_PAGES * PAGE_SIZE) - sizeof(heap_block_t);
    block->free = true;
    block->next = 0;
    block->prev = 0;
    
    kernel_heap.first = block;

}

/* A growth reservation is serialized separately so physical frames can be
 * acquired before heap_lock.  Once reserved, mapping the new range follows
 * heap -> VMM kernel mappings -> PMM; no PMM lock is held while heap_lock is
 * acquired. */
static bool heap_grow_locked(const phys_addr_t *frames, u32 *mapped_count)
{
    page_table_t *pml4 = vmm_get_kernel_pml4();
    void *old_end = kernel_heap.end;

    if (mapped_count) *mapped_count = 0;

    if ((uintptr_t)old_end > HEAP_LIMIT - HEAP_INITIAL_PAGES * PAGE_SIZE) {
        return false;
    }

    for (usize i = 0; i < HEAP_INITIAL_PAGES; i++)
    {
        if (!vmm_map(
            pml4,
            (void *)((u8 *)old_end + (i * PAGE_SIZE)),
            frames[i],
            PTE_PRESENT | PTE_READWRITE
        )) return false;
        if (mapped_count) *mapped_count = (u32)(i + 1U);
    }

    kernel_heap.end =
        (void *)((u8 *)kernel_heap.end + (HEAP_INITIAL_PAGES * PAGE_SIZE));

    heap_block_t *last = kernel_heap.first;

    while (last->next != 0)
    {
        last = last->next;
    }

    if (last->free)
    {
        last->size += HEAP_INITIAL_PAGES * PAGE_SIZE;
        return true;
    }

    heap_block_t *new_block = (heap_block_t *)old_end;

    new_block->size = (HEAP_INITIAL_PAGES * PAGE_SIZE) - sizeof(heap_block_t);
    new_block->free = true;

    new_block->next = 0;
    new_block->prev = last;

    last->next = new_block;
    return true;
}

#include <kprint.h>

static bool heap_verify(const char *where) {
    heap_block_t *current = kernel_heap.first;
    while (current != 0) {
        if ((uintptr_t)current < (uintptr_t)kernel_heap.start ||
            (uintptr_t)current >= (uintptr_t)kernel_heap.end) {
            kprint("[HEAP BUG at %s] invalid block ptr %p (start=%p end=%p)\n",
                   where, current, kernel_heap.start, kernel_heap.end);
            return false;
        }
        if (current->next && current->next->prev != current) {
            kprint("[HEAP BUG at %s] broken link: block=%p size=%u free=%d next=%p next->prev=%p\n",
                   where, current, (u32)current->size, current->free, current->next, current->next->prev);
            return false;
        }
        current = current->next;
    }
    return true;
}

void *kmalloc(usize size) {
    size = (size + 15) & ~15;
    for (;;) {
        u64 flags = spin_lock_irqsave(&heap_lock);
        heap_block_t *current;

        if (!heap_verify("kmalloc enter"))
            kprint("[HEAP] Corrupted before kmalloc(%u)\n", (u32)size);

        for (current = kernel_heap.first; current; current = current->next) {
            if (current->free && current->size >= size) {
                usize remaining = current->size - size;
                if (remaining > sizeof(heap_block_t) + 16) {
                    heap_block_t *new_block = (heap_block_t *)
                        ((u8 *)current + sizeof(heap_block_t) + size);

                    new_block->size = remaining - sizeof(heap_block_t);
                    new_block->free = true;
                    new_block->next = current->next;
                    new_block->prev = current;
                    current->next = new_block;
                    if (new_block->next) new_block->next->prev = new_block;
                    current->size = size;
                }
                current->free = false;
                if (!heap_verify("kmalloc exit"))
                    kprint("[HEAP] Corrupted during kmalloc(%u)\n",
                           (u32)size);
                void *ptr = (void *)((u8 *)current + sizeof(heap_block_t));
                spin_unlock_irqrestore(&heap_lock, flags);
                return ptr;
            }
        }
        spin_unlock_irqrestore(&heap_lock, flags);

        /* Reserve one growth operation without holding heap_lock while PMM
         * allocates.  This avoids the PMM -> VMM -> heap inversion that would
         * otherwise be possible when a new kernel page-table branch is made. */
        spin_lock(&heap_growth_lock);
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
            spin_unlock(&heap_growth_lock);
            return 0;
        }

        flags = spin_lock_irqsave(&heap_lock);
        bool suitable = false;
        for (current = kernel_heap.first; current; current = current->next) {
            if (current->free && current->size >= size) {
                suitable = true;
                break;
            }
        }
        if (suitable) {
            spin_unlock_irqrestore(&heap_lock, flags);
            for (u32 i = 0; i < HEAP_INITIAL_PAGES; i++)
                pmm_free_frame(frames[i]);
            spin_unlock(&heap_growth_lock);
            continue;
        }

        u32 mapped_count = 0;
        if (!heap_grow_locked(frames, &mapped_count)) {
            spin_unlock_irqrestore(&heap_lock, flags);
            /* Mapped pages belong to the heap even if a later mapping failed;
             * only frames which never reached the page tables can be safely
             * returned to PMM. */
            for (u32 i = mapped_count; i < HEAP_INITIAL_PAGES; i++)
                pmm_free_frame(frames[i]);
            spin_unlock(&heap_growth_lock);
            return 0;
        }
        spin_unlock_irqrestore(&heap_lock, flags);
        spin_unlock(&heap_growth_lock);
    }
}

void kfree(void *ptr) {
    if (ptr == 0) {
        return;
    }

    u64 flags = spin_lock_irqsave(&heap_lock);
    heap_verify("kfree enter");

    heap_block_t *block =
        (heap_block_t *)((u8 *)ptr - sizeof(heap_block_t));

    block->free = true;

    if (block->next != 0 && block->next->free) {
        heap_block_t *next = block->next;

        block->size += sizeof(heap_block_t) + next->size;
        block->next = next->next;

        if (block->next != 0) {
            block->next->prev = block;
        }
    }

    if (block->prev != 0 && block->prev->free) {
        heap_block_t *prev = block->prev;

        prev->size += sizeof(heap_block_t) + block->size;
        prev->next = block->next;

        if (prev->next != 0) {
            prev->next->prev = prev;
        }
    }

    heap_verify("kfree exit");
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
    u64 total = 0;
    u64 flags = spin_lock_irqsave(&heap_lock);

    heap_block_t *current = kernel_heap.first;

    while (current != 0)
    {
        total += current->size;
        current = current->next;
    }

    spin_unlock_irqrestore(&heap_lock, flags);
    return total;
}

u64 heap_get_used_size(void)
{
    u64 used = 0;
    u64 flags = spin_lock_irqsave(&heap_lock);

    heap_block_t *current = kernel_heap.first;

    while (current != 0)
    {
        if (!current->free)
        {
            used += current->size;
        }

        current = current->next;
    }

    spin_unlock_irqrestore(&heap_lock, flags);
    return used;
}

u64 heap_get_free_size(void)
{
    u64 free = 0;
    u64 flags = spin_lock_irqsave(&heap_lock);

    heap_block_t *current = kernel_heap.first;

    while (current != 0)
    {
        if (current->free)
        {
            free += current->size;
        }

        current = current->next;
    }

    spin_unlock_irqrestore(&heap_lock, flags);
    return free;
}
