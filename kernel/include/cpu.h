/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define CPU_MAX_COUNT 256U

typedef struct cpu_local
{
    /* GS.base points at this record while the CPU is executing in the
     * kernel.  Keep self at offset zero for the GS load in cpu_current(). */
    struct cpu_local *self;
    u32 index;
    u8 apic_id;
    bool present;
    bool bsp;
    bool online;
    struct gdt_cpu_state *descriptor;
} cpu_local_t;

/* Used only by early descriptor setup, before authoritative MADT-backed CPU
 * storage can be allocated. */
cpu_local_t *cpu_bootstrap_local(void);
bool cpu_activate_kernel_gs(cpu_local_t *cpu);
bool cpu_init_bsp(void);
cpu_local_t *cpu_current(void);
u32 cpu_current_index(void);
cpu_local_t *cpu_by_index(u32 index);
cpu_local_t *cpu_by_apic_id(u8 apic_id);
u32 cpu_count(void);
u32 cpu_online_count(void);
