#include <heap.h>
#include <vmm.h>
#include <pmm.h>
#include <terminal.h>

static heap_t kernel_heap;

void heap_init(void) {
    page_table_t *pml4 = vmm_get_kernel_pml4();

    for (usize i = 0; i < HEAP_INITIAL_PAGES; i++) {
        void *frame = pmm_alloc_frame();

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

static void heap_grow(void)
{
    page_table_t *pml4 = vmm_get_kernel_pml4();
    void *old_end = kernel_heap.end;

    for (usize i = 0; i < HEAP_INITIAL_PAGES; i++)
    {
        void *frame = pmm_alloc_frame();

        vmm_map(
            pml4,
            (void *)((u8 *)old_end + (i * PAGE_SIZE)),
            frame,
            PTE_PRESENT | PTE_READWRITE
        );
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
        return;
    }

    heap_block_t *new_block = (heap_block_t *)old_end;

    new_block->size = (HEAP_INITIAL_PAGES * PAGE_SIZE) - sizeof(heap_block_t);
    new_block->free = true;

    new_block->next = 0;
    new_block->prev = last;

    last->next = new_block;
}

void *kmalloc(usize size) {
    size = (size + 15) & ~15;

    heap_block_t *current = kernel_heap.first;
    
    while (current != 0) {
        if (current->free && current->size >= size) {
            usize remaining = current->size - size;
            if (remaining > sizeof(heap_block_t) + 16) {
                heap_block_t *new_block =
                    (heap_block_t *)((u8 *)current + sizeof(heap_block_t) + size);

                new_block->size = remaining - sizeof(heap_block_t);
                new_block->free = true;

                new_block->next = current->next;
                new_block->prev = current;

                current->next = new_block;

                if (new_block->next != 0) {
                    new_block->next->prev = new_block;
                }

                current->size = size;
            }

            current->free = false;

            return (void *)((u8 *)current + sizeof(heap_block_t));
        }
        
        current = current->next;
    }

    heap_grow();

    return kmalloc(size);
}

void kfree(void *ptr) {
    if (ptr == 0) {
        return;
    }

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
}

void heap_dump(void)
{
    kprint("[HEAP] Start: %p\n", kernel_heap.start);
    kprint("[HEAP] End:   %p\n", kernel_heap.end);

    heap_block_t *current = kernel_heap.first;
    int i = 0;

    while (current != 0)
    {
        kprint("[HEAP] Block %d\n", i);
        kprint("        Addr: %p\n", current);
        kprint("        Size: %u\n", current->size);
        kprint("        Free: %u\n", current->free);

        current = current->next;
        i++;
    }
}
