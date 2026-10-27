/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <pmm.h>
#include <vmm.h>

#define KERNEL_STACK_PAGE_SIZE 4096U
#define KERNEL_STACK_MAX_USABLE_PAGES 16U
#define KERNEL_STACK_CANARY_BYTE 0xA5U

typedef enum {
    KERNEL_STACK_USAGE_THREAD = 0,
    KERNEL_STACK_USAGE_AP_STARTUP,
    KERNEL_STACK_USAGE_AP_IDLE,
    KERNEL_STACK_USAGE_IST_DOUBLE_FAULT,
    KERNEL_STACK_USAGE_IST_NMI,
    KERNEL_STACK_USAGE_IST_MACHINE_CHECK,
    KERNEL_STACK_USAGE_BOOTSTRAP,
    KERNEL_STACK_USAGE_KIND_COUNT
} kernel_stack_usage_kind_t;

typedef struct kernel_stack {
    vmm_kernel_range_t range;
    pmm_owned_frame_t frames[KERNEL_STACK_MAX_USABLE_PAGES];
    uintptr_t base;
    usize size;
    usize high_water_bytes;
    u32 mapped_pages;
    kernel_stack_usage_kind_t usage_kind;
    bool active;
} kernel_stack_t;

typedef struct kernel_stack_usage_stat {
    u64 samples;
    usize peak_bytes;
} kernel_stack_usage_stat_t;

/* Allocate page-rounded usable storage between two deliberately unmapped
 * pages. The caller must initialize the output object to zero. The current
 * implementation supports up to 64 KiB usable stacks, matching the ordinary
 * kernel-thread allocation. */
bool kernel_stack_allocate(kernel_stack_t *stack, usize usable_bytes,
                           kernel_stack_usage_kind_t usage_kind);

/* Release is valid only after all CPUs have stopped executing on this stack.
 * On SMP it also requires interrupts enabled and no caller-owned spinlock. */
bool kernel_stack_release(kernel_stack_t *stack);
/* Preflight before unlinking an owner. SMP teardown requires a context that
 * can wait for remote TLB-shootdown acknowledgements. */
bool kernel_stack_release_context_valid(void);

bool kernel_stack_guard_contains(const kernel_stack_t *stack,
                                 uintptr_t fault_address);

/* Sample the live stack pointer and scan the canary after the stack is
 * quiescent. Stack growth is downward; the result is a measured high-water
 * estimate, not a proof that every byte of every frame was written. */
void kernel_stack_sample_rsp(kernel_stack_t *stack, uintptr_t stack_pointer);
usize kernel_stack_measure_high_water(const kernel_stack_t *stack);

/* Also used for fixed bootstrap and exception stacks which cannot share a
 * managed kernel_stack_t. Inputs are trusted kernel addresses. */
void kernel_stack_usage_sample(kernel_stack_usage_kind_t kind,
                               uintptr_t stack_top,
                               uintptr_t stack_pointer);
void kernel_stack_usage_snapshot(
    kernel_stack_usage_stat_t out_stats[KERNEL_STACK_USAGE_KIND_COUNT]);
void kernel_stack_usage_report(void);
