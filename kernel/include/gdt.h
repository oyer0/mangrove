/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once


#include <types.h>
#include <kernel_stack.h>

struct cpu_local;

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 granularity;
    u8 base_high;
} __attribute__((packed));

struct gdt_system_entry {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 granularity;
    u8 base_high;
    u32 base_highest;
    u32 reserved;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

struct tss_entry {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u64 reserved3;
    u16 iomap_base;
} __attribute__((packed));

/* Descriptor state is owned by one CPU-local record.  The BSP bootstrap
 * instance is initialized before the heap-backed topology table exists; APs
 * will receive independent instances when they are brought online. */
typedef struct gdt_cpu_state {
    struct gdt_entry gdt[8];
    struct gdt_ptr gdt_pointer;
    /* Long-mode hardware reads the packed TSS register fields at fixed byte
     * offsets. Place its base four bytes off an 8-byte boundary so RSP0 and
     * IST1..7 themselves are naturally aligned. */
    u8 tss_alignment[2];
    struct tss_entry tss;
    kernel_stack_t double_fault_stack;
    kernel_stack_t nmi_stack;
    kernel_stack_t machine_check_stack;
    bool guarded_ist_stacks_ready;
} gdt_cpu_state_t;

bool gdt_init(void);
/* Prepare an AP's descriptor/TSS state and guarded IST leases on the BSP,
 * before that AP is launched. Mapping these stacks from an AP before its
 * local interrupt controller is initialized cannot safely service remote
 * TLB shootdowns when another AP is already online. */
bool gdt_prepare_ap_state(gdt_cpu_state_t *state,
                          uintptr_t kernel_stack_top);
/* Release a prepared AP descriptor whose processor was never launched. */
void gdt_release_unstarted_ap_state(gdt_cpu_state_t *state);
/* Initializes and loads descriptor state for a CPU already running in long
 * mode.  The caller supplies that CPU's Ring 0 stack top. */
bool gdt_init_cpu(struct cpu_local *cpu, uintptr_t kernel_stack_top);
/* Replace the BSP's early, image-backed exception stacks after VMM and heap
 * initialization. APs install guarded stacks before loading their TSS. */
bool gdt_install_guarded_ists(struct cpu_local *cpu);
bool gdt_exception_stack_guard_fault(struct cpu_local *cpu,
                                     uintptr_t fault_address,
                                     kernel_stack_usage_kind_t *out_kind);
uintptr_t gdt_exception_stack_top(struct cpu_local *cpu, u64 vector,
                                  kernel_stack_usage_kind_t *out_kind);
/* Select the Ring 0 stack used by the next userspace-to-kernel interrupt. */
void gdt_set_kernel_stack(uintptr_t stack_top);
