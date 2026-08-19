.intel_syntax noprefix

.global handoff

handoff:
    mov cr3, r9
    mov rsp, r8
    mov rdi, rdx
    jmp rcx
