.global thread_context_switch

/*
 * void thread_context_switch(uintptr_t *outgoing_rsp,
 *                            uintptr_t incoming_rsp)
 *
 * The stack layout is defined by scheduler.c.  Only the cooperative
 * context-switch ABI's callee-saved registers are preserved here.
 */
thread_context_switch:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
    pushq %rbp

    movq %rsp, (%rdi)
    movq %rsi, %rsp

    popq %rbp
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    ret

.global thread_context_switch_interrupts_enabled
/* Same ABI as thread_context_switch, but enable IRQs only for the incoming
 * context after the old context has been fully saved and the switch is done. */
thread_context_switch_interrupts_enabled:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
    pushq %rbp

    movq %rsp, (%rdi)
    movq %rsi, %rsp

    popq %rbp
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    sti
    ret

.global thread_context_switch_interrupts_disabled
thread_context_switch_interrupts_disabled:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
    pushq %rbp

    movq %rsp, (%rdi)
    movq %rsi, %rsp

    popq %rbp
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    cli
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
    call scheduler_preempt_context_restored
    movq 24(%rsp), %r11
    movq 8(%rsp), %rax
    movq %rax, 120(%r11)
    movq 0(%rsp), %rax
    movq %rax, 128(%r11)
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
    /* RSP now points at the saved flags slot followed by saved RIP. */
    pushq 8(%rsp)
    pushq 8(%rsp)
    popfq
    ret
