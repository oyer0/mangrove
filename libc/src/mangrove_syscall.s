.section .text
.global mg_syscall
.type mg_syscall, @function

/* Mangrove syscall bridge: (number, arg0, arg1, arg2) -> RAX result. */
mg_syscall:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    syscall
    ret

