/* SPDX-License-Identifier: GPL-3.0-or-later */
.global ring3_enter
.type ring3_enter, @function

/* Enter a fixed Ring 3 test entry using the standard iretq frame. */
ring3_enter:
    cli
    /* Preserve the kernel per-CPU pointer while loading the user selectors.
     * Reloading GS can replace its hidden base with the descriptor base. */
    movq %gs:0, %r8
    movq %rdx, %r10
    movq %rcx, %r9
    mov $0x33, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Restore the conventional pair before returning: GS.base must be the
     * user base (zero), while KERNEL_GS_BASE carries the CPU-local pointer.
     * The final swapgs is the kernel-to-user transition. */
    movq %r8, %rax
    movq %r8, %rdx
    shrq $32, %rdx
    movl $0xC0000101, %ecx
    wrmsr
    movl $0xC0000102, %ecx
    xorl %eax, %eax
    xorl %edx, %edx
    wrmsr
    swapgs
    movq %r9, %rcx

    /* System V arguments: RDI = user RIP, RSI = user RSP, RDX = argc, RCX = argv */
    pushq $0x33
    pushq %rsi
    pushfq
    orq $0x200, (%rsp)
    pushq $0x3b
    pushq %rdi

    /* Forward argc and argv registers to userspace RDI and RSI */
    mov %r10, %rdi
    mov %rcx, %rsi
    xor %rdx, %rdx
    xor %rcx, %rcx

    iretq
