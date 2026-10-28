/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdt.h>
#include <cpu.h>
#include <idt.h>
#include <string.h>
#include <syscall.h>
#include <fpu.h>
#include <panic.h>
#include <kprint.h>

static gdt_cpu_state_t bsp_descriptor_state;
static u8 bsp_early_ist_stacks[3][8192] __attribute__((aligned(16)));

extern void gdt_flush(u64 gdt_ptr_addr);
extern void tss_load(u16 tss_selector);
extern char __stack_top[];

static void gdt_set_gate(gdt_cpu_state_t *state, i32 num, u32 base,
                         u32 limit, u8 access, u8 gran) {
    state->gdt[num].base_low       = (base & 0xFFFF);
    state->gdt[num].base_middle    = (base >> 16) & 0xFF;
    state->gdt[num].base_high      = (base >> 24) & 0xFF;

    state->gdt[num].limit_low      = (limit & 0xFFFF);
    state->gdt[num].granularity    = (limit >> 16) & 0x0F;

    state->gdt[num].granularity    |= gran & 0xF0;
    state->gdt[num].access         = access;
}

static void gdt_set_tss(gdt_cpu_state_t *state, i32 num, u64 base, u32 limit) {
    struct gdt_system_entry *tss_gate =
        (struct gdt_system_entry *)&state->gdt[num];

    tss_gate->limit_low     = (limit & 0xFFFF);
    tss_gate->base_low      = (base & 0xFFFF);
    tss_gate->base_middle   = (base >> 16) & 0xFF;
    tss_gate->access        = 0x89;
    tss_gate->granularity   = (limit >> 16) & 0xFF;
    tss_gate->base_high     = (base >> 24) & 0xFF;
    tss_gate->base_highest  = (base >> 32) & 0xFFFFFFFF;
    tss_gate->reserved      = 0;
}

static void gdt_build_cpu_state(gdt_cpu_state_t *state,
                                uintptr_t kernel_stack_top,
    const uintptr_t early_ist_tops[3])
{
    memset(state, 0, sizeof(*state));

    state->gdt_pointer.limit = sizeof(state->gdt) - 1;
    state->gdt_pointer.base = (u64)&state->gdt;

    gdt_set_gate(state, 0, 0, 0, 0, 0);

    gdt_set_gate(state, 1, 0, 0, 0x9A, 0x20);

    gdt_set_gate(state, 2, 0, 0, 0x92, 0x00);

    state->tss.rsp0 = (u64)kernel_stack_top;
    if (early_ist_tops) {
        state->tss.ist1 = early_ist_tops[0];
        state->tss.ist2 = early_ist_tops[1];
        state->tss.ist3 = early_ist_tops[2];
    }
    state->tss.iomap_base = sizeof(state->tss);

    /* Ring 3 descriptors.  Long-mode code ignores the base/limit, but the
     * present, DPL=3 access bytes are still required by iretq. */
    gdt_set_gate(state, 6, 0, 0, 0xF2, 0x00);
    gdt_set_gate(state, 7, 0, 0, 0xFA, 0x20);

    gdt_set_tss(state, 3, (u64)&state->tss, sizeof(state->tss) - 1);
}

static bool gdt_load_cpu(cpu_local_t *cpu, gdt_cpu_state_t *state)
{
    if (!cpu || !state)
        return false;

    if (!fpu_init_cpu())
        return false;
    KERNEL_BOOT_DEBUG_LOG("[GDT] CPU floating-point state ready\n");
    gdt_flush((u64)&state->gdt_pointer);
    /* Loading the flat data selector also resets the current GS base. Restore
     * the CPU-local base immediately, before diagnostics or helpers that may
     * consult cpu_current(). */
    if (!cpu_activate_kernel_gs(cpu))
        return false;
    KERNEL_BOOT_DEBUG_LOG("[GDT] CPU descriptor table loaded\n");
    tss_load(0x18);
    KERNEL_BOOT_DEBUG_LOG("[GDT] CPU TSS loaded\n");
    syscall_init_cpu();
    KERNEL_BOOT_DEBUG_LOG("[GDT] CPU syscall MSRs ready\n");
    return cpu_current() == cpu || cpu_activate_kernel_gs(cpu);
}

static bool gdt_prepare_guarded_ists(gdt_cpu_state_t *state)
{
    kernel_stack_t *stacks[3];
    const kernel_stack_usage_kind_t kinds[3] = {
        KERNEL_STACK_USAGE_IST_DOUBLE_FAULT,
        KERNEL_STACK_USAGE_IST_NMI,
        KERNEL_STACK_USAGE_IST_MACHINE_CHECK
    };

    if (!state || state->guarded_ist_stacks_ready)
        return state && state->guarded_ist_stacks_ready;
    stacks[0] = &state->double_fault_stack;
    stacks[1] = &state->nmi_stack;
    stacks[2] = &state->machine_check_stack;

    for (u32 index = 0; index < 3U; index++) {
        if (kernel_stack_allocate(stacks[index], 8192U, kinds[index]))
            continue;
        for (u32 release = 0; release < index; release++) {
            if (!kernel_stack_release(stacks[release]))
                panic("gdt: guarded IST allocation rollback failed");
        }
        return false;
    }

    /* Publish the new stack tops only after all four independent guarded
     * mappings are ready. The old BSP emergency stacks remain valid until
     * each aligned 64-bit TSS field is replaced. */
    state->guarded_ist_stacks_ready = true;
    state->tss.ist1 = state->double_fault_stack.base +
                      state->double_fault_stack.size;
    state->tss.ist2 = state->nmi_stack.base + state->nmi_stack.size;
    state->tss.ist3 = state->machine_check_stack.base +
                      state->machine_check_stack.size;
    return true;
}

bool gdt_init(void) {
    cpu_local_t *cpu = cpu_bootstrap_local();
    gdt_cpu_state_t *state = &bsp_descriptor_state;
    uintptr_t early_ist_tops[3];

    if (!cpu)
        return false;
    for (u32 index = 0; index < 3U; index++)
        early_ist_tops[index] = (uintptr_t)bsp_early_ist_stacks[index] +
                                sizeof(bsp_early_ist_stacks[index]);
    cpu->descriptor = state;
    gdt_build_cpu_state(state, (uintptr_t)__stack_top, early_ist_tops);
    return gdt_load_cpu(cpu, state);
}

bool gdt_install_guarded_ists(struct cpu_local *cpu)
{
    return cpu && cpu->descriptor &&
           gdt_prepare_guarded_ists(cpu->descriptor);
}

bool gdt_prepare_ap_state(gdt_cpu_state_t *state,
                          uintptr_t kernel_stack_top)
{
    const uintptr_t no_early_stacks[3] = {0};

    if (!state || !kernel_stack_top || state->guarded_ist_stacks_ready)
        return false;
    gdt_build_cpu_state(state, kernel_stack_top, no_early_stacks);
    return gdt_prepare_guarded_ists(state);
}

void gdt_release_unstarted_ap_state(gdt_cpu_state_t *state)
{
    kernel_stack_t *stacks[3];

    if (!state || !state->guarded_ist_stacks_ready)
        return;
    stacks[0] = &state->double_fault_stack;
    stacks[1] = &state->nmi_stack;
    stacks[2] = &state->machine_check_stack;
    for (u32 index = 0; index < 3U; index++) {
        if (!kernel_stack_release(stacks[index]))
            panic("gdt: failed to release unstarted AP IST stack");
    }
    state->guarded_ist_stacks_ready = false;
}

bool gdt_init_cpu(struct cpu_local *cpu, uintptr_t kernel_stack_top)
{
    if (!cpu || !cpu->descriptor || !kernel_stack_top ||
        cpu_current() != cpu ||
        !cpu->descriptor->guarded_ist_stacks_ready)
        return false;
    /* The BSP prepared all guarded mappings before SIPI. AP-local allocation
     * here would be unable to service shootdowns if a prior AP is online. */
    cpu->descriptor->tss.rsp0 = (u64)kernel_stack_top;
    if (!gdt_load_cpu(cpu, cpu->descriptor))
        return false;
    idt_load_shared();
    return true;
}

bool gdt_exception_stack_guard_fault(cpu_local_t *cpu,
                                     uintptr_t fault_address,
                                     kernel_stack_usage_kind_t *out_kind)
{
    gdt_cpu_state_t *state;
    kernel_stack_t *stacks[3];
    const kernel_stack_usage_kind_t kinds[3] = {
        KERNEL_STACK_USAGE_IST_DOUBLE_FAULT,
        KERNEL_STACK_USAGE_IST_NMI,
        KERNEL_STACK_USAGE_IST_MACHINE_CHECK
    };

    if (!cpu || !cpu->descriptor)
        return false;
    state = cpu->descriptor;
    if (!state->guarded_ist_stacks_ready)
        return false;
    stacks[0] = &state->double_fault_stack;
    stacks[1] = &state->nmi_stack;
    stacks[2] = &state->machine_check_stack;
    for (u32 index = 0; index < 3U; index++) {
        if (kernel_stack_guard_contains(stacks[index], fault_address)) {
            if (out_kind)
                *out_kind = kinds[index];
            return true;
        }
    }
    return false;
}

uintptr_t gdt_exception_stack_top(cpu_local_t *cpu, u64 vector,
                                  kernel_stack_usage_kind_t *out_kind)
{
    gdt_cpu_state_t *state;
    kernel_stack_usage_kind_t kind;
    uintptr_t top;

    if (!cpu || !cpu->descriptor)
        return 0;
    state = cpu->descriptor;
    switch (vector) {
    case IDT_VECTOR_DOUBLE_FAULT:
        kind = KERNEL_STACK_USAGE_IST_DOUBLE_FAULT;
        top = (uintptr_t)state->tss.ist1;
        break;
    case IDT_VECTOR_NMI:
        kind = KERNEL_STACK_USAGE_IST_NMI;
        top = (uintptr_t)state->tss.ist2;
        break;
    case IDT_VECTOR_MACHINE_CHECK:
        kind = KERNEL_STACK_USAGE_IST_MACHINE_CHECK;
        top = (uintptr_t)state->tss.ist3;
        break;
    default:
        return 0;
    }
    if (out_kind)
        *out_kind = kind;
    return top;
}

void gdt_set_kernel_stack(uintptr_t stack_top)
{
    cpu_local_t *cpu = cpu_current();

    if (!stack_top || !cpu || !cpu->descriptor)
        panic("gdt: cannot publish missing current-thread kernel stack");
    /* SYSCALL uses a GS-relative stack pointer because it does not perform
     * the privilege-stack switch that hardware interrupts do. Publish both
     * entry mechanisms while the scheduler holds the executing CPU's context. */
    cpu->syscall_kernel_stack_top = stack_top;
    cpu->descriptor->tss.rsp0 = (u64)stack_top;
}
