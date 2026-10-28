/* SPDX-License-Identifier: GPL-3.0-or-later */
.section .text
.global threadtest_syscall_with_rsp
.type threadtest_syscall_with_rsp, @function

/* Probe syscall 22 with an explicitly selected userspace RSP. The helper's
 * real caller stack is retained in RBX and restored without touching the
 * selected RSP after SYSRET. The observed post-syscall RFLAGS are written to
 * the caller-provided output slot after the original stack is restored. */
threadtest_syscall_with_rsp:
    pushq %rbx
    movq %rsp, %rbx
    pushfq
    popq %rdx
    orq $(1 << 18), %rdx
    pushq %rdx
    popfq
    movq %rdi, %rsp
    movq $22, %rax
    std
    syscall
    movq %rbx, %rsp
    pushfq
    popq %rdx
    movq %rdx, (%rsi)
    andq $-262145, %rdx           /* Clear AC before returning to C. */
    cld
    pushq %rdx
    popfq
    popq %rbx
    ret

.global threadtest_syscall_then_touch_rsp
.type threadtest_syscall_then_touch_rsp, @function
/* After a successful syscall return on the selected user RSP, perform the
 * next normal stack write. An unmapped/exhausted stack must fault in Ring 3. */
threadtest_syscall_then_touch_rsp:
    pushq %rbx
    movq %rsp, %rbx
    movq %rdi, %rsp
    movq $22, %rax
    syscall
    pushq %rax
    movq %rbx, %rsp
    popq %rbx
    ret
