#include <idt.h>
#include <timer.h>
#include <pic.h>
#include <irq.h>

static struct idt_entry idt[256];
static struct idt_ptr   idt_pointer;

extern void idt_load(u64 idt_ptr_addr);
extern u64 irq_stub_table[];

extern void isr_0(void);  extern void isr_1(void);  extern void isr_2(void);  extern void isr_3(void);
extern void isr_4(void);  extern void isr_5(void);  extern void isr_6(void);  extern void isr_7(void);
extern void isr_8(void);  extern void isr_9(void);  extern void isr_10(void); extern void isr_11(void);
extern void isr_12(void); extern void isr_13(void); extern void isr_14(void); extern void isr_15(void);
extern void isr_16(void); extern void isr_17(void); extern void isr_18(void); extern void isr_19(void);
extern void isr_20(void); extern void isr_21(void); extern void isr_22(void); extern void isr_23(void);
extern void isr_24(void); extern void isr_25(void); extern void isr_26(void); extern void isr_27(void);
extern void isr_28(void); extern void isr_29(void); extern void isr_30(void); extern void isr_31(void);

static u64 isr_handlers[32] = {
    (u64)isr_0,  (u64)isr_1,  (u64)isr_2,  (u64)isr_3,
    (u64)isr_4,  (u64)isr_5,  (u64)isr_6,  (u64)isr_7,
    (u64)isr_8,  (u64)isr_9,  (u64)isr_10, (u64)isr_11,
    (u64)isr_12, (u64)isr_13, (u64)isr_14, (u64)isr_15,
    (u64)isr_16, (u64)isr_17, (u64)isr_18, (u64)isr_19,
    (u64)isr_20, (u64)isr_21, (u64)isr_22, (u64)isr_23,
    (u64)isr_24, (u64)isr_25, (u64)isr_26, (u64)isr_27,
    (u64)isr_28, (u64)isr_29, (u64)isr_30, (u64)isr_31
};

void exception_handler(struct cpu_registers *regs) {

    u64 cr2, cr3;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    for (;;) {
        __asm__ volatile ("hlt");
    }
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
        idt_set_gate(i, isr_handlers[i], 0, 0x8E);
    }

    for (u8 i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_stub_table[i], 0, 0x8E);
    }

    idt_load((u64)&idt_pointer);
}
