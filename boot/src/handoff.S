.intel_syntax noprefix

.global handoff

handoff:
    mov rsp, 0x900000
    mov rdi, rdx
    jmp rcx
