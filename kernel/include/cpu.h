/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define CPU_MAX_COUNT 256U

typedef struct cpu_local
{
    u32 index;
    u8 apic_id;
    bool present;
    bool bsp;
    bool online;
} cpu_local_t;

/* Step 2 keeps current-CPU access BSP-local. Step 3 can replace the
 * implementation with GS-based lookup once interrupt/syscall entry has the
 * matching swapgs contract. */
bool cpu_init_bsp(void);
cpu_local_t *cpu_current(void);
u32 cpu_current_index(void);
cpu_local_t *cpu_by_index(u32 index);
cpu_local_t *cpu_by_apic_id(u8 apic_id);
u32 cpu_count(void);
u32 cpu_online_count(void);
