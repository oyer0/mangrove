#include <heap.h>
#include <vmm.h>
#include <pmm.h>
#include <font.h>

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

void *kmalloc(usize size) {
    heap_block_t *current = kernel_heap.first;
    
    while (current != 0) {
        if (current->free && current->size >= size) {
            current->free = false;

            return (void *)((u8 *)current + sizeof(heap_block_t));
        }
        
        current = current->next;
    }

    return 0;
}

void kfree(void *ptr) {
    (void)ptr;
}

void heap_dump(void)
{
    kprint("[HEAP] Start: %p\n", kernel_heap.start);
    kprint("[HEAP] End:   %p\n", kernel_heap.end);

    kprint("[HEAP] First block: %p\n", kernel_heap.first);
    kprint("[HEAP] Block size: %u\n", kernel_heap.first->size);
    kprint("[HEAP] Free: %u\n", kernel_heap.first->free);
}
