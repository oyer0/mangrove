#include <dma.h>
#include <vmm.h>
#include <heap.h>
#include <panic.h>
#include <stddef.h>

typedef struct
{
    void *original;
} dma_header_t;

void dma_init(void)
{
}

dma_buffer_t dma_alloc(usize size, usize alignment)
{
    if (alignment == 0) {
        alignment = sizeof(void *);
    }

    if ((alignment & (alignment - 1)) != 0) {
        panic("dma_alloc: alignment must be a power of two");
    }

    usize total = size + alignment + sizeof(dma_header_t);

    void *original = kmalloc(total);

    if (!original) {
        return (dma_buffer_t){0};
    }

    uintptr_t addr = (uintptr_t)original + sizeof(dma_header_t);

    addr = (addr + alignment - 1) & ~(alignment - 1);

    dma_header_t *header =
        (dma_header_t *)(addr - sizeof(dma_header_t));

    header->original = original;
    
    dma_buffer_t buffer;

    buffer.virt = (void *)addr;
    buffer.phys = vmm_virtual_to_physical((void *)addr);

    if (buffer.phys == 0) {
        panic("dma_alloc: failed to translate virtual address");
    }

    return buffer;
}

void dma_free(dma_buffer_t buffer)
{
    void *ptr = buffer.virt;

    if (!ptr) {
        return;
    }

    dma_header_t *header =
        (dma_header_t *)((uintptr_t)ptr - sizeof(dma_header_t));

    kfree(header->original);
}
