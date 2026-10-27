/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <kernel_stack.h>

static u8 *test_mapping;
static u64 reserved_pages;
static u64 map_indices[32];
static u32 map_count;
static u32 unmap_calls;
static u32 frame_acquire_count;
static u32 frame_release_count;
static u32 fail_acquire_at;
static u32 fail_map_at;
static bool unmapped_before_release;
static u32 online_cpus = 1U;
static u32 caller_spinlocks;

u32 cpu_online_count(void) { return online_cpus; }
u32 spinlock_depth_current(void) { return caller_spinlocks; }

bool vmm_kernel_range_reserve(u64 page_count, vmm_kernel_range_t *range)
{
    assert(range && !range->base && !range->page_count && !range->generation);
    assert(page_count >= 3U && page_count <= 18U);
    assert(!test_mapping);
    test_mapping = aligned_alloc(KERNEL_STACK_PAGE_SIZE,
        (usize)page_count * KERNEL_STACK_PAGE_SIZE);
    if (!test_mapping)
        return false;
    memset(test_mapping, 0xCC, (usize)page_count * KERNEL_STACK_PAGE_SIZE);
    reserved_pages = page_count;
    range->base = (virt_addr_t)(uintptr_t)test_mapping;
    range->page_count = page_count;
    range->generation = 1U;
    map_count = 0;
    unmap_calls = 0;
    unmapped_before_release = false;
    return true;
}

bool vmm_kernel_range_map_owned(const vmm_kernel_range_t *range,
                                u64 page_index,
                                const pmm_owned_frame_t *frame,
                                u64 flags)
{
    assert(range && range->base == (uintptr_t)test_mapping);
    assert(frame && frame->physical);
    assert((flags & (PTE_READWRITE | PTE_NX)) ==
           (PTE_READWRITE | PTE_NX));
    if (fail_map_at && page_index == fail_map_at)
        return false;
    assert(page_index > 0U && page_index + 1U < range->page_count);
    assert(map_count < sizeof(map_indices) / sizeof(map_indices[0]));
    map_indices[map_count++] = page_index;
    return true;
}

bool vmm_kernel_range_unmap(vmm_kernel_range_t *range,
                            u64 first_page, u64 page_count)
{
    assert(range && range->base == (uintptr_t)test_mapping);
    assert(first_page == 1U);
    assert(page_count > 0U && page_count <= KERNEL_STACK_MAX_USABLE_PAGES);
    assert(page_count == map_count);
    unmap_calls++;
    unmapped_before_release = true;
    return true;
}

bool vmm_kernel_range_release(vmm_kernel_range_t *range)
{
    assert(range && range->base == (uintptr_t)test_mapping);
    assert(unmapped_before_release || map_count == 0U);
    free(test_mapping);
    test_mapping = NULL;
    *range = (vmm_kernel_range_t){0};
    reserved_pages = 0;
    return true;
}

bool pmm_owned_frame_acquire(pmm_owned_frame_t *frame)
{
    if (fail_acquire_at && frame_acquire_count == fail_acquire_at)
        return false;
    frame_acquire_count++;
    *frame = (pmm_owned_frame_t){
        .physical = frame_acquire_count * KERNEL_STACK_PAGE_SIZE,
        .generation = frame_acquire_count,
        .slot = frame_acquire_count - 1U
    };
    return true;
}

bool pmm_owned_frame_release(pmm_owned_frame_t *frame)
{
    assert(frame && frame->physical);
    frame_release_count++;
    *frame = (pmm_owned_frame_t){0};
    return true;
}

void *phys_to_virt(phys_addr_t physical)
{
    assert(test_mapping && physical &&
           !(physical & (KERNEL_STACK_PAGE_SIZE - 1U)));
    u64 index = physical / KERNEL_STACK_PAGE_SIZE - 1U;
    assert(index < reserved_pages - 2U);
    return test_mapping + KERNEL_STACK_PAGE_SIZE +
           index * KERNEL_STACK_PAGE_SIZE;
}

void panic(const char *message)
{
    (void)message;
    abort();
}
void kprint(const char *format, ...)
{
    (void)format;
}

#include "../../kernel/src/kernel_stack.c"

static void reset_faults(void)
{
    fail_acquire_at = 0;
    fail_map_at = 0;
    frame_acquire_count = 0;
    frame_release_count = 0;
    map_count = 0;
    unmap_calls = 0;
}

static void test_layout_and_high_water(void)
{
    kernel_stack_t stack = {0};
    kernel_stack_usage_stat_t stats[KERNEL_STACK_USAGE_KIND_COUNT];

    reset_faults();
    assert(kernel_stack_allocate(&stack, 64U * 1024U,
                                 KERNEL_STACK_USAGE_THREAD));
    assert(stack.active && stack.size == 64U * 1024U);
    assert(stack.range.page_count == 18U);
    assert(stack.base == stack.range.base + KERNEL_STACK_PAGE_SIZE);
    assert(map_count == 16U);
    for (u32 i = 0; i < map_count; i++)
        assert(map_indices[i] == (u64)i + 1U);
    assert(!kernel_stack_guard_contains(&stack, stack.base));
    assert(kernel_stack_guard_contains(&stack, stack.range.base));
    assert(kernel_stack_guard_contains(&stack,
                                       stack.base + stack.size));
    assert(!kernel_stack_guard_contains(&stack,
                                        stack.base + stack.size - 1U));
    assert(!kernel_stack_guard_contains(&stack,
        stack.range.base + stack.range.page_count * KERNEL_STACK_PAGE_SIZE));
    for (usize i = 0; i < stack.size; i++)
        assert(((u8 *)stack.base)[i] == KERNEL_STACK_CANARY_BYTE);

    kernel_stack_sample_rsp(&stack, stack.base + stack.size - 768U);
    assert(stack.high_water_bytes == 768U);
    ((u8 *)stack.base)[stack.size - 2049U] = 0;
    assert(kernel_stack_measure_high_water(&stack) >= 2049U);
    assert(kernel_stack_release(&stack));
    assert(!stack.active && !stack.range.base && !stack.base);
    assert(unmap_calls == 1U && unmapped_before_release);
    assert(frame_release_count == 16U);
    kernel_stack_usage_snapshot(stats);
    assert(stats[KERNEL_STACK_USAGE_THREAD].samples >= 2U);
    assert(stats[KERNEL_STACK_USAGE_THREAD].peak_bytes >= 2049U);
}

static void test_allocation_failure_cleanup(void)
{
    kernel_stack_t stack = {0};

    reset_faults();
    fail_acquire_at = 3U;
    assert(!kernel_stack_allocate(&stack, 16U * 1024U,
                                  KERNEL_STACK_USAGE_AP_IDLE));
    assert(!stack.active && !stack.range.base && !test_mapping);
    assert(frame_acquire_count == 3U && frame_release_count == 3U);
    assert(unmap_calls == 1U && map_count == 3U);

    reset_faults();
    fail_map_at = 3U;
    assert(!kernel_stack_allocate(&stack, 16U * 1024U,
                                  KERNEL_STACK_USAGE_AP_IDLE));
    assert(!stack.active && !stack.range.base && !test_mapping);
    assert(frame_acquire_count == 3U && frame_release_count == 3U);
    assert(unmap_calls == 1U && map_count == 2U);
}

static void test_validation_and_external_measurement(void)
{
    kernel_stack_t stack = {0};
    kernel_stack_usage_stat_t stats[KERNEL_STACK_USAGE_KIND_COUNT];

    assert(!kernel_stack_allocate(&stack, 0,
                                  KERNEL_STACK_USAGE_THREAD));
    assert(!kernel_stack_allocate(&stack, 65537U,
                                  KERNEL_STACK_USAGE_THREAD));
    assert(!kernel_stack_allocate(&stack, 4096U,
                                  KERNEL_STACK_USAGE_KIND_COUNT));
    kernel_stack_usage_sample(KERNEL_STACK_USAGE_BOOTSTRAP,
                              0x10000U, 0xF000U);
    kernel_stack_usage_snapshot(stats);
    assert(stats[KERNEL_STACK_USAGE_BOOTSTRAP].samples == 1U);
    assert(stats[KERNEL_STACK_USAGE_BOOTSTRAP].peak_bytes == 4096U);
}

static void test_release_context_preflight(void)
{
    kernel_stack_t stack = {0};

    reset_faults();
    assert(kernel_stack_allocate(&stack, 4096U,
                                 KERNEL_STACK_USAGE_THREAD));
    online_cpus = 2U;
    caller_spinlocks = 1U;
    assert(!kernel_stack_release_context_valid());
    assert(!kernel_stack_release(&stack));
    assert(stack.active && unmap_calls == 0U && frame_release_count == 0U);
    caller_spinlocks = 0U;
    assert(kernel_stack_release_context_valid());
    assert(kernel_stack_release(&stack));
    online_cpus = 1U;
}

int main(void)
{
    test_layout_and_high_water();
    test_allocation_failure_cleanup();
    test_validation_and_external_measurement();
    test_release_context_preflight();
    return 0;
}
