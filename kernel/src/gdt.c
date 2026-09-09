/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdt.h>
#include <cpu.h>
#include <idt.h>
#include <string.h>
#include <syscall.h>

static gdt_cpu_state_t bsp_descriptor_state;

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
                                uintptr_t kernel_stack_top)
{
    memset(state, 0, sizeof(*state));

    state->gdt_pointer.limit = sizeof(state->gdt) - 1;
    state->gdt_pointer.base = (u64)&state->gdt;

    gdt_set_gate(state, 0, 0, 0, 0, 0);

    gdt_set_gate(state, 1, 0, 0, 0x9A, 0x20);

    gdt_set_gate(state, 2, 0, 0, 0x92, 0x00);

    u64 stack_top = (u64)state->emergency_stack +
        sizeof(state->emergency_stack);
    state->tss.rsp0 = (u64)kernel_stack_top;
    state->tss.ist1 = stack_top;
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

    gdt_flush((u64)&state->gdt_pointer);
    tss_load(0x18);
    syscall_init_cpu();
    return cpu_activate_kernel_gs(cpu);
}

bool gdt_init(void) {
    cpu_local_t *cpu = cpu_bootstrap_local();
    gdt_cpu_state_t *state = &bsp_descriptor_state;

    if (!cpu)
        return false;
    cpu->descriptor = state;
    gdt_build_cpu_state(state, (uintptr_t)__stack_top);
    return gdt_load_cpu(cpu, state);
}

bool gdt_init_cpu(struct cpu_local *cpu, uintptr_t kernel_stack_top)
{
    if (!cpu || !cpu->descriptor || !kernel_stack_top)
        return false;
    gdt_build_cpu_state(cpu->descriptor, kernel_stack_top);
    if (!gdt_load_cpu(cpu, cpu->descriptor))
        return false;
    idt_load_shared();
    return true;
}

void gdt_set_kernel_stack(uintptr_t stack_top)
{
    cpu_local_t *cpu = cpu_current();

    if (stack_top && cpu && cpu->descriptor) {
        cpu->descriptor->tss.rsp0 = (u64)stack_top;
    }
}
