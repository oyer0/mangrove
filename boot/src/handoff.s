.intel_syntax noprefix

.global handoff

handoff:
    mov rsp, r8
    mov rdi, rdx
    jmp rcx
