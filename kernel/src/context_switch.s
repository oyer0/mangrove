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
