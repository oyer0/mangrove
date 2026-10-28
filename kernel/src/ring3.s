/* SPDX-License-Identifier: GPL-3.0-or-later */
.global ring3_enter
.type ring3_enter, @function

/* Enter one thread's initial Ring 3 context using the standard iretq frame.
 * Arguments: RIP, RSP, RDI, RSI, FS_BASE. */
ring3_enter:
    cli
    /* Preserve the kernel per-CPU pointer while loading the user selectors.
     * Reloading GS can replace its hidden base with the descriptor base. */
    movq %gs:0, %r9
    movq %rdx, %r10
    movq %rcx, %r11
    movq %r8, %r12
    mov $0x33, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Load the thread's TLS pointer after loading the flat user FS selector,
     * which otherwise resets FS_BASE to the descriptor base. */
    movq %r12, %rax
    movq %r12, %rdx
    shrq $32, %rdx
    movl $0xC0000100, %ecx
    wrmsr

    /* Restore the conventional pair before returning: GS.base must be the
     * user base (zero), while KERNEL_GS_BASE carries the CPU-local pointer.
     * The final swapgs is the kernel-to-user transition. */
    movq %r9, %rax
    movq %r9, %rdx
    shrq $32, %rdx
    movl $0xC0000101, %ecx
    wrmsr
    movl $0xC0000102, %ecx
    xorl %eax, %eax
    xorl %edx, %edx
    wrmsr
    swapgs
    /* Complete the privilege transition frame. */
    pushq $0x33
    pushq %rsi
    pushfq
    orq $0x200, (%rsp)
    pushq $0x3b
    pushq %rdi

    /* Forward the first two user arguments, then clear every other general
     * register so a new context cannot observe stale kernel register data. */
    mov %r10, %rdi
    mov %r11, %rsi
    xor %eax, %eax
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %rdx, %rdx
    xor %ebp, %ebp
    xor %r8d, %r8d
    xor %r9d, %r9d
    xor %r10d, %r10d
    xor %r11d, %r11d
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d

    iretq
