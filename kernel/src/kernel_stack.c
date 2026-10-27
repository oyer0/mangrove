/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <kernel_stack.h>

#include <cpu.h>
#include <kprint.h>
#include <panic.h>
#include <spinlock.h>
#include <string.h>

static volatile u64 kernel_stack_usage_samples[KERNEL_STACK_USAGE_KIND_COUNT];
static volatile u64 kernel_stack_usage_peaks[KERNEL_STACK_USAGE_KIND_COUNT];

static bool kernel_stack_kind_valid(kernel_stack_usage_kind_t kind)
{
    return kind >= KERNEL_STACK_USAGE_THREAD &&
           kind < KERNEL_STACK_USAGE_KIND_COUNT;
}

static void kernel_stack_record_usage(kernel_stack_usage_kind_t kind,
                                      usize used_bytes)
{
    u64 previous;

    if (!kernel_stack_kind_valid(kind))
        return;
    __atomic_add_fetch(&kernel_stack_usage_samples[kind], 1U,
                       __ATOMIC_RELAXED);
    previous = __atomic_load_n(&kernel_stack_usage_peaks[kind],
                               __ATOMIC_RELAXED);
    while (previous < used_bytes &&
           !__atomic_compare_exchange_n(&kernel_stack_usage_peaks[kind],
                                        &previous, used_bytes, true,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

static void kernel_stack_release_resources(kernel_stack_t *stack,
                                           u32 acquired_frames,
                                           u32 mapped_frames)
{
    if (!stack)
        panic("kernel stack: invalid rollback state");

    if (mapped_frames &&
        !vmm_kernel_range_unmap(&stack->range, 1U, mapped_frames))
        panic("kernel stack: mapped-page rollback failed");

    for (u32 index = 0; index < acquired_frames; index++) {
        if (!pmm_owned_frame_release(&stack->frames[index]))
            panic("kernel stack: frame rollback failed");
    }

    if (stack->range.base && !vmm_kernel_range_release(&stack->range))
        panic("kernel stack: virtual-range rollback failed");
    memset(stack, 0, sizeof(*stack));
}

bool kernel_stack_allocate(kernel_stack_t *stack, usize usable_bytes,
                           kernel_stack_usage_kind_t usage_kind)
{
    usize rounded_bytes;
    u32 usable_pages;
    u32 acquired_frames = 0;
    u32 mapped_frames = 0;
    u64 total_pages;

    if (!stack || stack->active || stack->range.base || stack->base ||
        !usable_bytes || !kernel_stack_kind_valid(usage_kind) ||
        usable_bytes > KERNEL_STACK_MAX_USABLE_PAGES *
                           (usize)KERNEL_STACK_PAGE_SIZE ||
        !kernel_stack_release_context_valid())
        return false;

    if (usable_bytes > ~(usize)0 - (KERNEL_STACK_PAGE_SIZE - 1U))
        return false;
    rounded_bytes = (usable_bytes + KERNEL_STACK_PAGE_SIZE - 1U) &
                    ~((usize)KERNEL_STACK_PAGE_SIZE - 1U);
    usable_pages = (u32)(rounded_bytes / KERNEL_STACK_PAGE_SIZE);
    if (!usable_pages || usable_pages > KERNEL_STACK_MAX_USABLE_PAGES)
        return false;
    total_pages = (u64)usable_pages + 2U;

    if (!vmm_kernel_range_reserve(total_pages, &stack->range))
        return false;

    stack->base = (uintptr_t)stack->range.base + KERNEL_STACK_PAGE_SIZE;
    stack->size = rounded_bytes;
    stack->usage_kind = usage_kind;

    for (u32 page = 0; page < usable_pages; page++) {
        void *direct_mapping;

        if (!pmm_owned_frame_acquire(&stack->frames[page]))
            goto fail;
        acquired_frames++;
        direct_mapping = phys_to_virt(stack->frames[page].physical);
        if (!direct_mapping)
            goto fail;
        if (!vmm_kernel_range_map_owned(&stack->range, (u64)page + 1U,
                                       &stack->frames[page],
                                       PTE_READWRITE | PTE_NX))
            goto fail;
        mapped_frames++;
    }

    stack->mapped_pages = mapped_frames;
    stack->active = true;
    memset((void *)stack->base, KERNEL_STACK_CANARY_BYTE, stack->size);
    return true;

fail:
    kernel_stack_release_resources(stack, acquired_frames, mapped_frames);
    return false;
}

bool kernel_stack_release_context_valid(void)
{
    u64 rflags;

    if (spinlock_depth_current())
        return false;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");
    return cpu_online_count() <= 1U || (rflags & (1ULL << 9)) != 0;
}

bool kernel_stack_release(kernel_stack_t *stack)
{
    usize measured;
    u32 usable_pages;

    if (!stack || !stack->active || !stack->range.base || !stack->base ||
        !stack->size || !kernel_stack_kind_valid(stack->usage_kind) ||
        stack->mapped_pages != stack->size / KERNEL_STACK_PAGE_SIZE ||
        !kernel_stack_release_context_valid())
        return false;

    measured = kernel_stack_measure_high_water(stack);
    if (measured > stack->high_water_bytes)
        stack->high_water_bytes = measured;
    kernel_stack_record_usage(stack->usage_kind, stack->high_water_bytes);

    usable_pages = (u32)(stack->size / KERNEL_STACK_PAGE_SIZE);
    if (!vmm_kernel_range_unmap(&stack->range, 1U, usable_pages))
        return false;
    for (u32 page = 0; page < usable_pages; page++) {
        if (!pmm_owned_frame_release(&stack->frames[page]))
            panic("kernel stack: owned frame release failed after unmap");
    }
    if (!vmm_kernel_range_release(&stack->range))
        panic("kernel stack: lease release failed after unmap");
    memset(stack, 0, sizeof(*stack));
    return true;
}

bool kernel_stack_guard_contains(const kernel_stack_t *stack,
                                 uintptr_t fault_address)
{
    uintptr_t range_end;
    uintptr_t usable_end;

    if (!stack || !stack->active || !stack->range.base || !stack->base ||
        !stack->size || stack->range.page_count >
            ((~(uintptr_t)0 - (uintptr_t)stack->range.base) /
             KERNEL_STACK_PAGE_SIZE))
        return false;
    range_end = (uintptr_t)stack->range.base +
        (uintptr_t)stack->range.page_count * KERNEL_STACK_PAGE_SIZE;
    usable_end = stack->base + stack->size;
    if (usable_end < stack->base)
        return false;
    return (fault_address >= (uintptr_t)stack->range.base &&
            fault_address < stack->base) ||
           (fault_address >= usable_end && fault_address < range_end);
}

void kernel_stack_sample_rsp(kernel_stack_t *stack, uintptr_t stack_pointer)
{
    uintptr_t stack_end;
    usize used;

    if (!stack || !stack->active || !stack->base || !stack->size)
        return;
    stack_end = stack->base + stack->size;
    if (stack_end < stack->base)
        return;
    if (stack_pointer <= stack->base)
        used = stack->size;
    else if (stack_pointer >= stack_end)
        used = 0;
    else
        used = (usize)(stack_end - stack_pointer);
    if (used > stack->high_water_bytes)
        stack->high_water_bytes = used;
    kernel_stack_record_usage(stack->usage_kind, used);
}

usize kernel_stack_measure_high_water(const kernel_stack_t *stack)
{
    const volatile u8 *bytes;
    usize untouched = 0;
    usize measured;

    if (!stack || !stack->active || !stack->base || !stack->size)
        return 0;
    bytes = (const volatile u8 *)stack->base;
    while (untouched < stack->size &&
           bytes[untouched] == KERNEL_STACK_CANARY_BYTE)
        untouched++;
    measured = stack->size - untouched;
    return measured > stack->high_water_bytes
        ? measured : stack->high_water_bytes;
}

void kernel_stack_usage_sample(kernel_stack_usage_kind_t kind,
                               uintptr_t stack_top,
                               uintptr_t stack_pointer)
{
    usize used;

    if (!kernel_stack_kind_valid(kind) || !stack_top || !stack_pointer)
        return;
    used = stack_pointer < stack_top ? (usize)(stack_top - stack_pointer) : 0;
    kernel_stack_record_usage(kind, used);
}

void kernel_stack_usage_snapshot(
    kernel_stack_usage_stat_t out_stats[KERNEL_STACK_USAGE_KIND_COUNT])
{
    if (!out_stats)
        return;
    for (u32 index = 0; index < KERNEL_STACK_USAGE_KIND_COUNT; index++) {
        out_stats[index].samples = __atomic_load_n(
            &kernel_stack_usage_samples[index], __ATOMIC_RELAXED);
        out_stats[index].peak_bytes = (usize)__atomic_load_n(
            &kernel_stack_usage_peaks[index], __ATOMIC_RELAXED);
    }
}

void kernel_stack_usage_report(void)
{
#if defined(KERNEL_STACK_DIAGNOSTICS)
    static const char *const names[KERNEL_STACK_USAGE_KIND_COUNT] = {
        "thread", "ap-startup", "ap-idle", "ist-double-fault",
        "ist-nmi", "ist-machine-check", "bootstrap"
    };
    kernel_stack_usage_stat_t stats[KERNEL_STACK_USAGE_KIND_COUNT];

    kernel_stack_usage_snapshot(stats);
    for (u32 index = 0; index < KERNEL_STACK_USAGE_KIND_COUNT; index++) {
        kprint("[STACK] kind=%s samples=%llu peak=%u bytes\n",
               names[index], stats[index].samples,
               (u32)stats[index].peak_bytes);
    }
#endif
}
