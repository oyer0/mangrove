.section .text
.global _start
.type _start, @function
.extern main
.extern process_exit

_start:
    /* The loader enters with a valid empty user stack.  Arrange the SysV
     * call boundary: main receives RSP % 16 == 8. */
    and $-16, %rsp
    sub $8, %rsp
    xor %rdi, %rdi
    xor %rsi, %rsi
    call main
    mov %eax, %edi
    call process_exit
1:
    pause
    jmp 1b
