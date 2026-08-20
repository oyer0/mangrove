.global thread_context_switch
.extern scheduler_context_switch_saved

/*
 * void thread_context_switch(uintptr_t *outgoing_rsp,
 *                            uintptr_t incoming_rsp,
 *                            uint64_t saved_flags)
 *
 * The stack layout is defined by scheduler.c.  RFLAGS is part of the saved
 * context as well as the cooperative ABI's callee-saved registers.  In
 * particular, a syscall entered with IF masked must resume with IF masked;
 * borrowing the outgoing context's IF bit permits IRQ0 to interrupt scheduler
 * queue mutation on the resumed stack.
 */
thread_context_switch:
    pushfq
    /* Preserve the caller's complete flags in the frame, then close the IRQ
     * window while current_thread and RSP transition to the target. */
    cli
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
    pushq %rbp

    movq %rsp, (%rdi)
    /* The caller closes the IRQ window before mutating scheduler state.  RDX
     * carries the flags observed before that critical section; use them for
     * the outgoing frame instead of the temporary IF=0 flags captured by the
     * pushfq above. */
    movq %rdx, 48(%rsp)
    /* Publish the outgoing frame only after it is complete.  The helper also
     * marks the incoming frame consumed so a resumed thread cannot be chosen
     * again until a later switch saves a fresh frame for it. */
    pushq %rdi
    pushq %rsi
    call scheduler_context_switch_saved
    popq %rsi
    popq %rdi
    movq %rsi, %rsp

    popq %rbp
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    popfq
    ret

.global thread_interrupt_return_trampoline
.extern scheduler_preempt_from_trampoline
.extern scheduler_preempt_return_flags
.extern scheduler_preempt_context_restored
# Entered by iretq after an IRQ has been acknowledged.  The complete IRQ
# frame is therefore already restored; scheduling happens on this thread's
# ordinary stack and the saved RIP is resumed when it is selected again.
thread_interrupt_return_trampoline:
    cli
    subq $16, %rsp
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

    movq %rsp, %r11
    andq $-16, %rsp
    subq $32, %rsp
    movq %r11, 24(%rsp)
    call scheduler_preempt_from_trampoline
    movq %rax, 0(%rsp)
    call scheduler_preempt_return_flags
    movq %rax, 8(%rsp)
    movq 24(%rsp), %r11
    movq %r11, %r12
    movq 8(%rsp), %rax
    movq %rax, 120(%r11)
    movq 0(%rsp), %rax
    movq %rax, 128(%r11)
    leaq 136(%r12), %rdi
    call scheduler_preempt_context_restored
    movq %r12, %r11
    movq %r11, %rsp
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
    /* RSP now points at the saved flags slot followed by saved RIP.  Restore
     * flags without consuming the reserved slot, skip that slot, and let ret
     * consume RIP.  LEA is required here: ADD would overwrite the interrupted
     * arithmetic flags immediately after POPFQ restores them.  This leaves RSP
     * at the original interrupted value. */
    pushq 0(%rsp)
    popfq
    leaq 8(%rsp), %rsp
    ret
