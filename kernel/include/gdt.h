/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once


#include <types.h>

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
    struct tss_entry tss;
    u8 emergency_stack[4096] __attribute__((aligned(16)));
} gdt_cpu_state_t;

bool gdt_init(void);
/* Initializes and loads descriptor state for a CPU already running in long
 * mode.  The caller supplies that CPU's Ring 0 stack top. */
bool gdt_init_cpu(struct cpu_local *cpu, uintptr_t kernel_stack_top);
/* Select the Ring 0 stack used by the next userspace-to-kernel interrupt. */
void gdt_set_kernel_stack(uintptr_t stack_top);
