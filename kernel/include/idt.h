#pragma once

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

struct cpu_registers {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vec_no, err_code;
    u64 rip, cs, rflags, rsp, ss;
};

static inline bool cpu_registers_has_privilege_stack(
    const struct cpu_registers *regs)
{
    return regs && ((regs->cs & 3U) != 0);
}

static inline uintptr_t cpu_registers_interrupted_rsp(
    const struct cpu_registers *regs)
{
    uintptr_t frame;

    if (!regs) return 0;
    if (cpu_registers_has_privilege_stack(regs)) {
        return regs->rsp;
    }

    /* The IRQ entry path leaves a fixed 16-byte stub area below the
     * architectural return point; account for it when deriving the original
     * same-ring RSP from the C frame base. */
    frame = (uintptr_t)regs;
    return frame + 22 * sizeof(u64);
}

static inline u64 cpu_registers_interrupted_ss(
    const struct cpu_registers *regs)
{
    return cpu_registers_has_privilege_stack(regs) ? regs->ss : 0;
}

void idt_init(void);
