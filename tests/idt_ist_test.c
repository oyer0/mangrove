/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include <gdt.h>
#include <idt.h>

_Static_assert(offsetof(struct cpu_registers, vec_no) == 15U * sizeof(u64),
               "IRQ stub follows the 15 saved general registers");
_Static_assert(offsetof(struct cpu_registers, rip) == 17U * sizeof(u64),
               "hardware return frame follows vector and error code");
_Static_assert(offsetof(struct cpu_registers, rflags) == 19U * sizeof(u64),
               "same-ring hardware frame ends at RFLAGS");

u64 isr_stub_table[32];
u64 irq_stub_table[16];
u64 spurious_irq_stub;
u64 tlb_shootdown_irq_stub;
static u64 loaded_idt;
void idt_load(u64 pointer) { loaded_idt = pointer; }

#include "../kernel/src/idt.c"
#include "../kernel/src/gdt.c"

int main(void)
{
    gdt_cpu_state_t state;
    struct cpu_registers frame = {
        .vec_no = IDT_VECTOR_DOUBLE_FAULT,
        .cs = 0x08,
        .rsp = 0x123456789abcdef0ULL,
        .ss = 0x10,
    };
    uintptr_t idt_pointer_address;

    idt_init();
    assert(loaded_idt != 0);
    assert(idt_pointer.limit == sizeof(idt) - 1U);
    assert(idt_pointer.base == (u64)(uintptr_t)idt);
    assert(idt[IDT_VECTOR_NMI].ist == 2U);
    assert(idt[IDT_VECTOR_DOUBLE_FAULT].ist == 1U);
    assert(idt[IDT_VECTOR_MACHINE_CHECK].ist == 3U);
    assert(idt[14].ist == 0U);

    gdt_build_cpu_state(&state, 0xfeed0000U);
    assert(state.tss.rsp0 == 0xfeed0000U);
    assert(state.tss.ist1 == (uintptr_t)state.double_fault_stack +
                              sizeof(state.double_fault_stack));
    assert(state.tss.ist2 == (uintptr_t)state.nmi_stack +
                              sizeof(state.nmi_stack));
    assert(state.tss.ist3 == (uintptr_t)state.machine_check_stack +
                              sizeof(state.machine_check_stack));

    assert(cpu_registers_has_stack_switch(&frame));
    assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
    assert(cpu_registers_interrupted_ss(&frame) == frame.ss);
    frame.vec_no = 2;
    assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
    frame.vec_no = 18;
    assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
    frame.vec_no = 14;
    idt_pointer_address = (uintptr_t)&frame;
    assert(!cpu_registers_has_stack_switch(&frame));
    assert(cpu_registers_interrupted_rsp(&frame) ==
           idt_pointer_address + 20U * sizeof(u64));
    frame.cs = 0x23;
    assert(cpu_registers_has_stack_switch(&frame));
    assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
    return 0;
}
