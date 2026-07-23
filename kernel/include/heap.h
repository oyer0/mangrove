#pragma once

#include <types.h>

#define HEAP_START 0x10000000ULL
#define HEAP_INITIAL_PAGES 4

void heap_init(void);
void *kmalloc(usize size);
void kfree(void *ptr);

u64 heap_get_total_size(void);
u64 heap_get_used_size(void);
u64 heap_get_free_size(void);

void heap_dump(void);

typedef struct heap_block {
    usize size;
    bool free;

    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

typedef struct {
    void *start;
    void *end;
    
    heap_block_t *first;
} heap_t;

static void heap_grow(void);
