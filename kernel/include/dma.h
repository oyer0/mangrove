#pragma once

#include <types.h>

void dma_init(void);

void *dma_alloc(usize size, usize alignment);
void dma_free(void *ptr);

u64 dma_phys_addr(void *ptr);
