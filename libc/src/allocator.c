#include <mangrove.h>
#include <stdlib.h>
#include <string.h>

/*
 * A small first-fit allocator.  Block headers live in the anonymous mappings
 * themselves, so there is no second metadata allocation to keep consistent.
 * Mappings are retained until process exit; memory_unmap() remains available
 * to callers that need explicit mapping lifetime, while the allocator owns
 * its regions for the lifetime of this userspace process.
 */
typedef struct heap_block {
    usize size;
    bool free;
    struct heap_block *previous;
    struct heap_block *next;
} heap_block_t;

#define HEAP_ALIGNMENT ((usize)16)
#define HEAP_PAGE_SIZE ((usize)MG_PAGE_SIZE)
#define HEAP_MIN_SPLIT (HEAP_ALIGNMENT)

static heap_block_t *heap_head;
static heap_block_t *heap_tail;

static bool add_overflow(usize left, usize right, usize *result)
{
    if (left > ~(usize)0 - right) return true;
    *result = left + right;
    return false;
}

static bool align_size(usize size, usize alignment, usize *result)
{
    usize adjusted;
    if (add_overflow(size, alignment - 1, &adjusted)) return false;
    *result = adjusted & ~(alignment - 1);
    return true;
}

static usize header_size(void)
{
    usize aligned;
    (void)align_size(sizeof(heap_block_t), HEAP_ALIGNMENT, &aligned);
    return aligned;
}

static u8 *payload(heap_block_t *block)
{
    return (u8 *)block + header_size();
}

static bool adjacent(heap_block_t *left, heap_block_t *right)
{
    return payload(left) + left->size == (u8 *)right;
}

static void split_block(heap_block_t *block, usize size)
{
    usize overhead = header_size();
    heap_block_t *remainder;

    if (block->size < size || block->size - size < overhead + HEAP_MIN_SPLIT)
        return;

    remainder = (heap_block_t *)(payload(block) + size);
    remainder->size = block->size - size - overhead;
    remainder->free = true;
    remainder->previous = block;
    remainder->next = block->next;
    if (remainder->next) remainder->next->previous = remainder;
    else heap_tail = remainder;
    block->next = remainder;
    block->size = size;
}

static void merge_with_next(heap_block_t *block)
{
    heap_block_t *next = block->next;
    if (!next || !next->free || !adjacent(block, next)) return;

    block->size += header_size() + next->size;
    block->next = next->next;
    if (block->next) block->next->previous = block;
    else heap_tail = block;
}

static heap_block_t *find_block(void *pointer)
{
    heap_block_t *block;
    for (block = heap_head; block; block = block->next) {
        if (payload(block) == (u8 *)pointer) return block;
    }
    return (heap_block_t *)0;
}

static heap_block_t *grow_heap(usize requested)
{
    usize needed;
    usize mapped_size;
    void *address = (void *)0;
    heap_block_t *block;

    if (add_overflow(requested, header_size(), &needed) ||
        !align_size(needed, HEAP_PAGE_SIZE, &mapped_size) ||
        mapped_size == 0 || memory_map(mapped_size, &address) != MG_OK)
        return (heap_block_t *)0;

    block = (heap_block_t *)address;
    block->size = mapped_size - header_size();
    block->free = true;
    block->previous = heap_tail;
    block->next = (heap_block_t *)0;
    if (heap_tail && adjacent(heap_tail, block) && heap_tail->free) {
        heap_tail->size += header_size() + block->size;
        return heap_tail;
    }
    if (heap_tail) heap_tail->next = block;
    else heap_head = block;
    heap_tail = block;
    return block;
}

void *malloc(usize size)
{
    usize aligned_size;
    heap_block_t *block;

    if (size == 0 || !align_size(size, HEAP_ALIGNMENT, &aligned_size))
        return (void *)0;
    for (block = heap_head; block; block = block->next) {
        if (block->free && block->size >= aligned_size) break;
    }
    if (!block) block = grow_heap(aligned_size);
    if (!block) return (void *)0;

    split_block(block, aligned_size);
    block->free = false;
    return payload(block);
}

void free(void *pointer)
{
    heap_block_t *block;
    if (!pointer) return;
    block = find_block(pointer);
    if (!block || block->free) return;
    block->free = true;
    if (block->previous && block->previous->free &&
        adjacent(block->previous, block)) {
        block = block->previous;
        merge_with_next(block);
    }
    merge_with_next(block);
}

void *calloc(usize count, usize size)
{
    usize total;
    void *pointer;
    if (count != 0 && size > ~(usize)0 / count) return (void *)0;
    total = count * size;
    pointer = malloc(total);
    if (pointer) memset(pointer, 0, total);
    return pointer;
}

void *realloc(void *pointer, usize size)
{
    heap_block_t *block;
    usize aligned_size;
    void *replacement;
    usize copy_size;

    if (!pointer) return malloc(size);
    if (size == 0) {
        free(pointer);
        return (void *)0;
    }
    block = find_block(pointer);
    if (!block || block->free || !align_size(size, HEAP_ALIGNMENT, &aligned_size))
        return (void *)0;
    if (block->size >= aligned_size) {
        split_block(block, aligned_size);
        return pointer;
    }
    if (block->next && block->next->free && adjacent(block, block->next) &&
        block->size + header_size() + block->next->size >= aligned_size) {
        merge_with_next(block);
        split_block(block, aligned_size);
        return pointer;
    }
    replacement = malloc(size);
    if (!replacement) return (void *)0;
    copy_size = block->size < size ? block->size : size;
    memcpy(replacement, pointer, copy_size);
    free(pointer);
    return replacement;
}
