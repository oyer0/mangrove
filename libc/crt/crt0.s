/* SPDX-License-Identifier: GPL-3.0-or-later */
.section .text
.global _start
.type _start, @function
.extern main
.extern exit

_start:
    /* The loader enters with a 16-byte-aligned user stack.  The call itself
     * provides the SysV entry alignment required by main. */
    and $-16, %rsp
    call main
    mov %eax, %edi
    call exit
1:
    pause
    jmp 1b

/* Private worker-return target installed by the kernel in a future worker
 * context's initial stack. Explicit exit() remains process-wide; only a C
 * entry that returns through this target retires that worker. */
.section .mangrove_thread_return,"ax",@progbits
.global __mangrove_thread_return
.type __mangrove_thread_return, @function
__mangrove_thread_return:
    xor %edi, %edi
    mov $105, %eax                 /* SYSCALL_THREAD_RETURN, private ABI */
    syscall
    ud2                            /* This syscall must not return. */
