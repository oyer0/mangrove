#pragma once

#include <types.h>

/* Place the heap above the highest physical RAM address so that the
 * identity mappings established at boot are never overwritten.
 * pmm_alloc_frame() zeroes freshly allocated frames by casting the
 * physical address to a C pointer; if the heap virtual range overlaps
 * a physical address, the zeroing corrupts heap metadata instead of
 * the intended frame.  With -m 512M physical RAM reaches 0x20000000;
 * 0xC0000000 (3 GiB) leaves ample headroom for growth.             */
#define HEAP_START 0xC0000000ULL
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
