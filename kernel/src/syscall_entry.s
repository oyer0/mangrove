.global syscall_entry
.type syscall_entry, @function
.extern scheduler_kernel_stack_top
.extern syscall_dispatch

/* Native SYSCALL entry.  The frame layout must match syscall_frame_t. */
syscall_entry:
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
    /* Each kernel thread owns its own stack.  This is essential when a
     * syscall blocks (for example wait): a suspended caller's syscall frame
     * must not be overwritten by another thread entering the kernel. */
    call scheduler_kernel_stack_top
    testq %rax, %rax
    jz 1f
    movq %rax, %rsp
    andq $-16, %rsp
    subq $8, %rsp
    pushq %r12
    movq %r12, %rdi
    call syscall_dispatch
    popq %r12
    addq $8, %rsp

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
    sysretq

1:
    /* No current kernel stack is a fatal kernel-entry condition. */
    cli
2:
    hlt
    jmp 2b
