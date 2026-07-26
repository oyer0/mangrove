#pragma once

#include <types.h>

typedef struct {
    void *virt;
    u64 phys;
} dma_buffer_t;

void dma_init(void);

dma_buffer_t dma_alloc(usize size, usize alignment);
void dma_free(dma_buffer_t buffer);

