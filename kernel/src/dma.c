#include <dma.h>
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

void *dma_alloc(usize size, usize alignment)
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
        return NULL;
    }

    uintptr_t addr = (uintptr_t)original + sizeof(dma_header_t);

    addr = (addr + alignment - 1) & ~(alignment - 1);

    dma_header_t *header =
        (dma_header_t *)(addr - sizeof(dma_header_t));

    header->original = original;

    return (void *)addr;
}

void dma_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    dma_header_t *header =
        (dma_header_t *)((uintptr_t)ptr - sizeof(dma_header_t));

    kfree(header->original);
}

u64 dma_phys_addr(void *ptr)
{
    return (u64)ptr;
}
