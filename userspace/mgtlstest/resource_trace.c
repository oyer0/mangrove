/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "resource_trace.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MGTLS_TEST_STACK_TOP ((uintptr_t)0x00007ffffff00000ULL)
#define MGTLS_TEST_STACK_BYTES (8U * 4096U)
#define MGTLS_TEST_STACK_PATTERN 0xa5U

typedef union allocation_header {
    struct {
        size_t requested;
        unsigned char tracked;
    } data;
    long double alignment;
    void *pointer_alignment;
} allocation_header_t;

static bool heap_trace_active;
static usize heap_trace_current;
static usize heap_trace_peak;

void * __real_malloc(size_t size);
void * __real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void __wrap_free(void *pointer);

static bool allocation_total(size_t requested, usize *out_total)
{
    if (!out_total || requested > (size_t)~(usize)0 -
                            sizeof(allocation_header_t))
        return false;
    *out_total = (usize)requested + sizeof(allocation_header_t);
    return true;
}

static void heap_trace_add(usize amount)
{
    if (heap_trace_current > (usize)~(usize)0 - amount)
        heap_trace_current = (usize)~(usize)0;
    else
        heap_trace_current += amount;
    if (heap_trace_current > heap_trace_peak)
        heap_trace_peak = heap_trace_current;
}

void * __wrap_malloc(size_t size)
{
    usize total;
    allocation_header_t *header;

    if (!allocation_total(size, &total)) return NULL;
    header = (allocation_header_t *)__real_malloc((size_t)total);
    if (!header) return NULL;
    header->data.requested = size;
    header->data.tracked = heap_trace_active ? 1U : 0U;
    if (heap_trace_active) heap_trace_add(total);
    return (void *)(header + 1);
}

void * __wrap_calloc(size_t count, size_t size)
{
    size_t requested;
    void *result;

    if (count != 0U && size > (size_t)~(size_t)0 / count) return NULL;
    requested = count * size;
    result = __wrap_malloc(requested);
    if (result) memset(result, 0, requested);
    return result;
}

void * __wrap_realloc(void *pointer, size_t size)
{
    allocation_header_t *header;
    allocation_header_t *replacement;
    usize old_total;
    usize new_total;
    bool was_tracked;

    if (!pointer) return __wrap_malloc(size);
    if (size == 0U) {
        __wrap_free(pointer);
        return NULL;
    }
    if (!allocation_total(size, &new_total)) return NULL;
    header = ((allocation_header_t *)pointer) - 1;
    if (!allocation_total(header->data.requested, &old_total)) return NULL;
    was_tracked = header->data.tracked != 0U;
    replacement = (allocation_header_t *)__real_realloc(
        header, (size_t)new_total);
    if (!replacement) return NULL;
    if (was_tracked && heap_trace_current >= old_total)
        heap_trace_current -= old_total;
    replacement->data.requested = size;
    replacement->data.tracked = heap_trace_active ? 1U : 0U;
    if (heap_trace_active) heap_trace_add(new_total);
    return (void *)(replacement + 1);
}

void __wrap_free(void *pointer)
{
    allocation_header_t *header;
    usize total;

    if (!pointer) return;
    header = ((allocation_header_t *)pointer) - 1;
    if (header->data.tracked &&
        allocation_total(header->data.requested, &total) &&
        heap_trace_current >= total)
        heap_trace_current -= total;
    __real_free(header);
}

void mgtlstest_heap_trace_start(void)
{
    heap_trace_current = 0U;
    heap_trace_peak = 0U;
    heap_trace_active = true;
}

void mgtlstest_heap_trace_stop(usize *out_peak_bytes,
                              usize *out_live_bytes)
{
    heap_trace_active = false;
    if (out_peak_bytes) *out_peak_bytes = heap_trace_peak;
    if (out_live_bytes) *out_live_bytes = heap_trace_current;
}

static unsigned char stack_pattern(uintptr_t address)
{
    return (unsigned char)(MGTLS_TEST_STACK_PATTERN ^
        (unsigned char)(address >> 3U) ^ (unsigned char)(address >> 11U));
}

uintptr_t mgtlstest_stack_mark(void)
{
    uintptr_t stack_pointer;
    uintptr_t stack_bottom = MGTLS_TEST_STACK_TOP -
                             MGTLS_TEST_STACK_BYTES;
    volatile unsigned char *bytes;

    __asm__ volatile("mov %%rsp, %0" : "=r"(stack_pointer));
    if (stack_pointer <= stack_bottom ||
        stack_pointer > MGTLS_TEST_STACK_TOP)
        return 0U;
    bytes = (volatile unsigned char *)stack_bottom;
    for (uintptr_t at = stack_bottom; at < stack_pointer; ++at)
        bytes[at - stack_bottom] = stack_pattern(at);
    return stack_pointer;
}

usize mgtlstest_stack_peak_bytes(uintptr_t mark)
{
    uintptr_t stack_bottom = MGTLS_TEST_STACK_TOP -
                             MGTLS_TEST_STACK_BYTES;
    volatile const unsigned char *bytes =
        (volatile const unsigned char *)stack_bottom;

    if (mark <= stack_bottom || mark > MGTLS_TEST_STACK_TOP) return 0U;
    for (uintptr_t at = stack_bottom; at < mark; ++at) {
        if (bytes[at - stack_bottom] != stack_pattern(at))
            return (usize)(MGTLS_TEST_STACK_TOP - at);
    }
    return (usize)(MGTLS_TEST_STACK_TOP - mark);
}
