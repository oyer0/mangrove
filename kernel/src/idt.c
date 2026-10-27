/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <idt.h>
#include <timer.h>
#include <irq.h>
#include <panic.h>
#include <lapic.h>
#include <scheduler.h>
#include <process.h>
#include <cpu.h>
#include <gdt.h>
#include <kernel_stack.h>
#include <kprint.h>
#include <smp.h>

static struct idt_entry idt[256];
static struct idt_ptr   idt_pointer;

extern void idt_load(u64 idt_ptr_addr);

extern u64 isr_stub_table[];
extern u64 irq_stub_table[];
extern u64 spurious_irq_stub;
extern u64 tlb_shootdown_irq_stub;

static const char *exception_messages[32] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

void exception_handler(struct cpu_registers *regs)
{
    kernel_stack_usage_kind_t stack_kind;
    uintptr_t exception_stack_top;
    uintptr_t stack_pointer;

    if (!regs) {
        panic_exception("Unknown CPU Exception", regs);
    }

    exception_stack_top = gdt_exception_stack_top(
        cpu_current(), regs->vec_no, &stack_kind);
    if (exception_stack_top) {
        __asm__ volatile("movq %%rsp, %0" : "=r"(stack_pointer));
        kernel_stack_usage_sample(stack_kind, exception_stack_top,
                                  stack_pointer);
    }

    if (regs->vec_no == IDT_VECTOR_PAGE_FAULT ||
        regs->vec_no == IDT_VECTOR_DOUBLE_FAULT) {
        uintptr_t fault_address;
        kernel_stack_usage_kind_t fault_stack_kind;

        __asm__ volatile("movq %%cr2, %0" : "=r"(fault_address));
        if ((regs->cs & 3U) == 0U) {
            kernel_thread_t *thread = scheduler_current_thread();
            if (thread && thread->kernel_stack.active)
                kernel_stack_sample_rsp(&thread->kernel_stack,
                                        (uintptr_t)regs->rsp);
        }

        if (scheduler_current_stack_guard_fault(fault_address)) {
            kernel_thread_t *thread = scheduler_current_thread();
            kprint("[STACK] kernel stack guard fault thread=%llu\n",
                   thread ? thread->id : 0ULL);
#if defined(KERNEL_STACK_DIAGNOSTICS)
            kernel_stack_usage_report();
#endif
            panic("kernel stack guard page fault");
        }

        if (gdt_exception_stack_guard_fault(cpu_current(), fault_address,
                                            &fault_stack_kind)) {
            kprint("[STACK] exception stack guard fault cpu=%u kind=%u\n",
                   cpu_current_index(), (u32)fault_stack_kind);
#if defined(KERNEL_STACK_DIAGNOSTICS)
            kernel_stack_usage_report();
#endif
            panic("kernel exception stack guard page fault");
        }

        if (regs->vec_no == IDT_VECTOR_PAGE_FAULT &&
            smp_ap_startup_stack_guard_fault(cpu_current_index(),
                                             fault_address)) {
            kprint("[STACK] AP startup stack guard fault cpu=%u\n",
                   cpu_current_index());
#if defined(KERNEL_STACK_DIAGNOSTICS)
            kernel_stack_usage_report();
#endif
            panic("AP startup stack guard page fault");
        }
    }

    /* These vectors describe machine/controller conditions, not an
     * instruction fault that can safely be charged to the current process.
     * No recovery handlers exist yet, so fail-stop on the dedicated IST. */
    if (regs->vec_no == IDT_VECTOR_NMI ||
        regs->vec_no == IDT_VECTOR_DOUBLE_FAULT ||
        regs->vec_no == IDT_VECTOR_MACHINE_CHECK)
        panic_exception(exception_messages[regs->vec_no], regs);

    /* An interrupt frame carrying a privilege stack was raised while Ring 3
     * was active.  The fault belongs to that process, not to the kernel.  Its
     * process exit path releases IPC state, handles and mappings, wakes a
     * waiting parent, and scheduler_terminate() hands execution to another
     * runnable thread instead of returning through the poisoned user frame. */
    if (regs->vec_no < 32 && cpu_registers_has_privilege_stack(regs) &&
        process_terminate_current_exception(MG_PROCESS_STATUS_CRASHED)) {
        /* A successful termination switches away and cannot return here. */
        panic("scheduler returned after userspace exception termination");
    }

    if (regs->vec_no < 32) {
        panic_exception(exception_messages[regs->vec_no], regs);
    }
    panic_exception("Unknown CPU Exception", regs);
}

void irq_handler(struct cpu_registers *regs)
{
    if (!regs)
        return;

    /* The local APIC spurious vector is not an in-service interrupt and must
     * not receive an EOI. */
    if (regs->vec_no == LAPIC_SPURIOUS_VECTOR)
        return;

    irq_dispatch(regs);

    if (lapic_enabled()) {
        lapic_eoi();
    }

    /* The IRQ is acknowledged before arranging deferred preemption. */
    (void)scheduler_prepare_preemption(regs);
}

static void idt_set_gate(u8 num, u64 handler, u8 ist, u8 flags) {
    u64 addr = handler;
    idt[num].offset_low      = (u16)(addr & 0xFFFF);
    idt[num].selector        = 0x08;
    idt[num].ist             = ist & 0x07;
    idt[num].type_attributes = flags;
    idt[num].offset_mid      = (u16)((addr >> 16) & 0xFFFF);
    idt[num].offset_high     = (u32)((addr >> 32) & 0xFFFFFFFF);
    idt[num].zero            = 0;
}

void idt_init(void) {
    idt_pointer.limit = sizeof(idt) - 1;
    idt_pointer.base  = (u64)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    for (u8 i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stub_table[i], 0, 0x8E);
    }
    idt_set_gate(IDT_VECTOR_NMI, isr_stub_table[IDT_VECTOR_NMI], 2, 0x8E);
    idt_set_gate(IDT_VECTOR_DOUBLE_FAULT,
                 isr_stub_table[IDT_VECTOR_DOUBLE_FAULT], 1, 0x8E);
    idt_set_gate(IDT_VECTOR_MACHINE_CHECK,
                 isr_stub_table[IDT_VECTOR_MACHINE_CHECK], 3, 0x8E);
    
    for (u8 i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_stub_table[i], 0, 0x8E);
    }
    idt_set_gate(IRQ_VECTOR_TLB_SHOOTDOWN,
                 (u64)&tlb_shootdown_irq_stub, 0, 0x8E);
    idt_set_gate(LAPIC_SPURIOUS_VECTOR, (u64)&spurious_irq_stub, 0, 0x8E);

    idt_load((u64)&idt_pointer);
}

void idt_load_shared(void)
{
    idt_load((u64)&idt_pointer);
}
