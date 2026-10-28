/* SPDX-License-Identifier: GPL-3.0-or-later */
.global idt_load
.global isr_stub_table
.global irq_stub_table
.extern exception_handler
.extern irq_handler
idt_load:
    lidt (%rdi)
    ret

.macro ISR_NOERRCODE num
.global isr_\num
isr_\num:
    pushq $0
    pushq $\num
    jmp common_isr_stub
.endm

.macro ISR_ERRCODE num
.global isr_\num
isr_\num:
    pushq $\num
    jmp common_isr_stub
.endm

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_ERRCODE   29
ISR_ERRCODE   30
ISR_NOERRCODE 31

.macro IRQ num
.global irq_\num
irq_\num:
    pushq $0
    pushq $(32 + \num)
    jmp common_irq_stub
.endm

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

.global tlb_shootdown_irq_stub
tlb_shootdown_irq_stub:
    pushq $0
    pushq $0x30
    jmp common_irq_stub

.global spurious_irq_stub
spurious_irq_stub:
    pushq $0
    pushq $0xFF
    jmp common_irq_stub

common_isr_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Keep an explicit transition marker below the saved frame. A Ring 3
     * entry always needs SWAPGS. NMI and machine-check can also arrive in the
     * tiny SYSCALL window after CPL changes but before its first SWAPGS; in
     * that case CS is kernel while GS_BASE still contains the user value
     * (zero under Mangrove's current user-GS contract). */
    subq $8, %rsp
    movq $0, (%rsp)
    testq $3, 152(%rsp)          /* saved CS after marker and register frame */
    jz .Lisr_kernel_gs
    swapgs
    movq $1, (%rsp)
    jmp .Lisr_gs_ready
.Lisr_kernel_gs:
    movq 128(%rsp), %rax         /* saved vector number */
    cmpq $2, %rax                /* NMI */
    je .Lisr_check_gs_base
    cmpq $18, %rax               /* machine check */
    jne .Lisr_gs_ready
.Lisr_check_gs_base:
    movl $0xC0000101, %ecx       /* IA32_GS_BASE; registers are already saved */
    rdmsr
    orl %edx, %eax
    jnz .Lisr_gs_ready
    swapgs
    movq $1, (%rsp)
.Lisr_gs_ready:
    leaq 8(%rsp), %rdi           /* exception_handler sees the native frame */
    movq %rsp, %rbx              /* retain marker address across the C call */
    andq $-16, %rsp
    subq $8, %rsp
    pushq %rbx
    call exception_handler
    popq %rsp

    /* Restore the GS orientation that was active at the interrupted point,
     * including a kernel-CS NMI before syscall-entry SWAPGS. */
    cmpq $0, (%rsp)
    je .Lisr_restore_registers
    swapgs
.Lisr_restore_registers:
    addq $8, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp
    iretq

common_irq_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Keep GS transitions conditional for the same-ring interrupt case. */
    /* The saved CS is 144 bytes above the register frame. */
    testq $3, 144(%rsp)
    jz .Lirq_kernel_gs
    swapgs
.Lirq_kernel_gs:
    movq %rsp, %rdi
    movq %rsp, %rbx
    andq $-16, %rsp
    subq $8, %rsp
    pushq %rbx
    call irq_handler
    popq %rsp

.Lcommon_irq_restore:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* The register frame has been popped; the saved CS is now 24 bytes
     * above the vector/error stub area. */
    testq $3, 24(%rsp)
    jz .Lirq_return_kernel_gs
    swapgs
.Lirq_return_kernel_gs:
    addq $16, %rsp
    iretq

.section .rodata
isr_stub_table:
    .quad isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    .quad isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    .quad isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    .quad isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31

irq_stub_table:
    .quad irq_0,  irq_1,  irq_2,  irq_3
    .quad irq_4,  irq_5,  irq_6,  irq_7
    .quad irq_8,  irq_9,  irq_10, irq_11
    .quad irq_12, irq_13, irq_14, irq_15
