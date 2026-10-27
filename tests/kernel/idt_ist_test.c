/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include <cpu.h>
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
_Static_assert(offsetof(struct tss_entry, ist1) == 36U,
               "hardware TSS IST1 offset changed");
_Static_assert(offsetof(struct tss_entry, ist4) == 60U,
               "hardware TSS IST4 offset changed");
_Static_assert(offsetof(gdt_cpu_state_t, tss) % 8U == 4U,
               "TSS register fields must be naturally aligned in the record");

u64 isr_stub_table[32];
u64 irq_stub_table[16];
u64 spurious_irq_stub;
u64 tlb_shootdown_irq_stub;
static u64 loaded_idt;
static cpu_local_t *active_cpu;
static unsigned stack_allocations;
static unsigned stack_releases;
void idt_load(u64 pointer) { loaded_idt = pointer; }
cpu_local_t *cpu_current(void) { return active_cpu; }
bool cpu_activate_kernel_gs(cpu_local_t *cpu) { active_cpu = cpu; return cpu != NULL; }
bool fpu_init_cpu(void) { return true; }
void syscall_init_cpu(void) { }
void kprint_debug(const char *format, ...) { (void)format; }
void gdt_flush(u64 pointer) { (void)pointer; }
void tss_load(u16 selector) { (void)selector; }
bool kernel_stack_allocate(kernel_stack_t *stack, usize bytes,
                           kernel_stack_usage_kind_t kind)
{
    assert(stack && !stack->active && bytes == 8192U);
    stack_allocations++;
    stack->range.base = 0x100000U + (uintptr_t)stack_allocations * 0x10000U;
    stack->range.page_count = 4U;
    stack->range.generation = stack_allocations;
    stack->base = stack->range.base + KERNEL_STACK_PAGE_SIZE;
    stack->size = bytes;
    stack->usage_kind = kind;
    stack->mapped_pages = 2U;
    stack->active = true;
    return true;
}
bool kernel_stack_release(kernel_stack_t *stack)
{
    assert(stack && stack->active);
    stack_releases++;
    stack->active = false;
    return true;
}
bool kernel_stack_guard_contains(const kernel_stack_t *stack,
                                uintptr_t fault_address)
{
    return stack && stack->active &&
        (fault_address == (uintptr_t)stack->range.base ||
         fault_address == stack->base + stack->size);
}
void panic(const char *message) { (void)message; assert(!"unexpected panic"); }

#include "../../kernel/src/idt.c"
#include "../../kernel/src/gdt.c"

int main(void)
{
    gdt_cpu_state_t state;
    gdt_cpu_state_t ap_state = {0};
    gdt_cpu_state_t abandoned_state = {0};
    cpu_local_t ap = {.index = 1U, .present = true, .bsp = false};
    uintptr_t early_ist_tops[3] = {
        0x11111000U, 0x22222000U, 0x33333000U
    };
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
    assert(idt[IDT_VECTOR_PAGE_FAULT].ist == 0U);

    gdt_build_cpu_state(&state, 0xfeed0000U, early_ist_tops);
    assert(state.tss.rsp0 == 0xfeed0000U);
    assert(state.tss.ist1 == early_ist_tops[0]);
    assert(state.tss.ist2 == early_ist_tops[1]);
    assert(state.tss.ist3 == early_ist_tops[2]);
    assert(state.tss.ist4 == 0U);

    /* The BSP prepares each AP's guarded IST mappings before SIPI. Once an
     * earlier AP is online, the AP itself cannot safely allocate shared
     * mappings before its local interrupt controller is initialized. */
    assert(gdt_prepare_ap_state(&ap_state, 0xfeed8000U));
    assert(stack_allocations == 3U && ap_state.guarded_ist_stacks_ready);
    assert(ap_state.tss.rsp0 == 0xfeed8000U);
    assert(ap_state.tss.ist1 == ap_state.double_fault_stack.base + 8192U);
    assert(ap_state.tss.ist2 == ap_state.nmi_stack.base + 8192U);
    assert(ap_state.tss.ist3 == ap_state.machine_check_stack.base + 8192U);
    ap.descriptor = &ap_state;
    kernel_stack_usage_kind_t fault_kind = KERNEL_STACK_USAGE_THREAD;
    assert(gdt_exception_stack_guard_fault(
        &ap, (uintptr_t)ap_state.double_fault_stack.range.base,
        &fault_kind));
    assert(fault_kind == KERNEL_STACK_USAGE_IST_DOUBLE_FAULT);
    assert(!gdt_exception_stack_guard_fault(
        &ap, ap_state.double_fault_stack.base + 32U, &fault_kind));
    assert(gdt_exception_stack_top(&ap, IDT_VECTOR_MACHINE_CHECK,
                                   &fault_kind) == ap_state.tss.ist3);
    assert(fault_kind == KERNEL_STACK_USAGE_IST_MACHINE_CHECK);
    active_cpu = &ap;
    assert(gdt_init_cpu(&ap, 0xbeef8000U));
    assert(stack_allocations == 3U && ap_state.tss.rsp0 == 0xbeef8000U);

    assert(gdt_prepare_ap_state(&abandoned_state, 0xcafe8000U));
    assert(stack_allocations == 6U);
    gdt_release_unstarted_ap_state(&abandoned_state);
    assert(stack_releases == 3U && !abandoned_state.guarded_ist_stacks_ready);

    assert(cpu_registers_has_stack_switch(&frame));
    assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
    assert(cpu_registers_interrupted_ss(&frame) == frame.ss);
    frame.vec_no = 2;
    assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
    frame.vec_no = 18;
    assert(cpu_registers_interrupted_rsp(&frame) == frame.rsp);
    frame.vec_no = IDT_VECTOR_PAGE_FAULT;
    assert(!cpu_registers_has_stack_switch(&frame));
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
