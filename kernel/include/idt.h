#ifndef IDT_H
#define IDT_H

#include <types.h>

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attributes;
    u16 offset_mid;
    u32 offset_high;
    u32 zero; 
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

struct interrupt_frame {
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

void idt_init(void);

#endif
