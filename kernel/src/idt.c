#include <idt.h>
#include <timer.h>
#include <pic.h>
#include <irq.h>
#include <panic.h>

static struct idt_entry idt[256];
static struct idt_ptr   idt_pointer;

extern void idt_load(u64 idt_ptr_addr);

extern u64 isr_stub_table[];
extern u64 irq_stub_table[];

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
    if (regs->vec_no < 32)
    {
        panic(exception_messages[regs->vec_no], regs);
    }

    panic("Unknown CPU Exception", regs);
}

void irq_handler(struct cpu_registers *regs)
{
    u64 irq = regs->vec_no - 32;

    irq_dispatch(regs);

    pic_send_eoi((unsigned char)irq);
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
    
    for (u8 i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_stub_table[i], 0, 0x8E);
    }

    idt_load((u64)&idt_pointer);
}
