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
               "RFLAGS follows RIP and CS");
_Static_assert(offsetof(struct cpu_registers, rsp) == 20U * sizeof(u64),
               "long-mode hardware always saves RSP");
_Static_assert(offsetof(struct cpu_registers, ss) == 21U * sizeof(u64),
               "long-mode hardware always saves SS");
_Static_assert(sizeof(struct cpu_registers) == 22U * sizeof(u64),
               "complete long-mode frame includes all five hardware words");

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
    /* Error-code exceptions, external IRQs, LAPIC timers, and IPIs all use
     * the normalized vector/error pair. Neither CPL nor IST determines
     * whether SS:RSP exists. Exercise both interrupted stack alignments. */
    for (u64 vector = 0; vector < 256; vector++) {
        for (unsigned user = 0; user < 2; user++) {
            for (unsigned misaligned = 0; misaligned < 2; misaligned++) {
                frame.vec_no = vector;
                frame.cs = user ? 0x23 : 0x08;
                frame.ss = user ? 0x1b : 0x10;
                frame.rsp = 0x123456789abcdef0ULL + misaligned * 8;
                assert(cpu_registers_has_privilege_stack(&frame) == !!user);
                assert(cpu_registers_has_stack_switch(&frame) ==
                       (user || vector == IDT_VECTOR_NMI ||
                        vector == IDT_VECTOR_DOUBLE_FAULT ||
                        vector == IDT_VECTOR_MACHINE_CHECK));
                assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
                assert(cpu_registers_interrupted_ss(&frame) == frame.ss);
                assert(cpu_registers_interrupted_rsp(&frame) !=
                       (uintptr_t)&frame + 20U * sizeof(u64));
                assert(cpu_registers_interrupted_rsp(&frame) !=
                       (uintptr_t)&frame + sizeof(frame));
            }
        }
    }
    assert(cpu_registers_interrupted_rsp(NULL) == 0);
    assert(cpu_registers_interrupted_ss(NULL) == 0);
    return 0;
}
