/* SPDX-License-Identifier: GPL-3.0-or-later */
.global syscall_entry
.type syscall_entry, @function
.extern scheduler_syscall_enter
.extern scheduler_syscall_leave
.extern syscall_dispatch

/* Native SYSCALL entry.  The frame layout must match syscall_frame_t. */
syscall_entry:
    /* SYSCALL leaves RSP unchanged. Do not push, dereference, or call through
     * it: it is entirely controlled by Ring 3. The scheduler publishes the
     * active thread's stack top in cpu_local_t before returning to userspace. */
    swapgs
    movq %rsp, %gs:16
    movq %gs:8, %rsp
    testq %rsp, %rsp
    jz 1f

    /* Save user RSP as the last word in the kernel-owned frame so POP RSP can
     * restore it after all general registers have been restored. */
    pushq %gs:16
    pushq %r11
    pushq %rcx
    pushq %rax
    pushq %rbx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r12
    andq $-16, %rsp
    subq $8, %rsp
    pushq %r12
    call scheduler_syscall_enter
    sti
    movq %r12, %rdi
    call syscall_dispatch
    cli
    call scheduler_syscall_leave
    popq %r12
    addq $8, %rsp

    /* RCX is the SYSRET target and R11 is the captured user flags. The C
     * validator checks the target and saved RSP; sanitize flags here before
     * they reach SYSRET. */
    andq $0x200ad5, 112(%r12)
    orq $0x202, 112(%r12)
    movq %r12, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rbx
    popq %rax
    popq %rcx
    popq %r11
    popq %rsp
    swapgs
    sysretq

1:
    /* No current kernel stack is a fatal kernel-entry condition. */
    cli
2:
    hlt
    jmp 2b
