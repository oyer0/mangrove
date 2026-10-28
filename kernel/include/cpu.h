/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <mg/inspection.h>

#define CPU_MAX_COUNT 256U

struct kernel_thread;
struct process;
struct page_table;

typedef struct cpu_local
{
    /* GS.base points at this record while the CPU is executing in the
     * kernel.  Keep self at offset zero for the GS load in cpu_current(). */
    struct cpu_local *self;
    /* SYSCALL does not load RSP from the TSS. The scheduler publishes the
     * selected thread's guarded kernel-stack top here before it can run in
     * Ring 3. The adjacent scratch slot is used only until entry copies the
     * user RSP into its kernel-owned syscall frame. */
    volatile uintptr_t syscall_kernel_stack_top;
    volatile uintptr_t syscall_user_rsp;
    u32 index;
    u8 apic_id;
    bool present;
    bool bsp;
    bool online;
    struct gdt_cpu_state *descriptor;
    struct kernel_thread *scheduler_current_thread;
    /* Process whose newly completed worker can be reclaimed after the
     * incoming context has restored its interrupt state. */
    struct process *scheduler_post_handoff_process;
    /* Number of spinlocks held by code executing on this CPU. The acquisition
     * attempt is counted around its atomic operation; scheduling is deferred
     * until the held count returns to zero. */
    volatile u32 spinlock_depth;
    struct kernel_thread *scheduler_idle_thread;
    volatile bool scheduler_preemption_pending;
    volatile bool scheduler_context_switching;
    struct page_table *current_pml4;
    volatile bool scheduler_timer_active;
    volatile u64 kernel_thread_runs;
    /* Monotonic local scheduler-timer accounting.  These counters are read
     * by the inspection ABI and therefore use atomic updates. */
    volatile u64 scheduler_accounted_ticks;
    volatile u64 scheduler_busy_ticks;
} cpu_local_t;

_Static_assert(__builtin_offsetof(cpu_local_t, self) == 0U,
               "cpu_local self must remain GS offset zero");
_Static_assert(__builtin_offsetof(cpu_local_t, syscall_kernel_stack_top) == 8U,
               "syscall entry assembly stack offset changed");
_Static_assert(__builtin_offsetof(cpu_local_t, syscall_user_rsp) == 16U,
               "syscall entry assembly user-RSP offset changed");

/* Used only by early descriptor setup, before authoritative MADT-backed CPU
 * storage can be allocated. */
cpu_local_t *cpu_bootstrap_local(void);
bool cpu_activate_kernel_gs(cpu_local_t *cpu);
bool cpu_init_bsp(void);
void cpu_mark_online(cpu_local_t *cpu);
void cpu_mark_offline(cpu_local_t *cpu);
cpu_local_t *cpu_current(void);
u32 cpu_current_index(void);
cpu_local_t *cpu_by_index(u32 index);
cpu_local_t *cpu_by_apic_id(u8 apic_id);
u32 cpu_count(void);
u32 cpu_online_count(void);
bool cpu_model_copy(char *output, usize capacity);
u32 cpu_snapshot_read(u32 offset, mg_cpu_info_t *output,
                      u32 capacity, u32 *out_total);
