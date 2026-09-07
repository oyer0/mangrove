/* SPDX-License-Identifier: GPL-3.0-or-later */
.section .text
.global _start
.type _start, @function
.extern main
.extern process_exit

_start:
    /* The loader enters with a 16-byte-aligned user stack.  The call itself
     * provides the SysV entry alignment required by main. */
    and $-16, %rsp
    call main
    mov %eax, %edi
    call process_exit
1:
    pause
    jmp 1b
