.intel_syntax noprefix

.global handoff

handoff:
    mov cr3, r9
    mov rsp, r8
    mov rdi, rdx
    /* UEFI does not provide a scheduler-safe RFLAGS contract.  Establish
     * the kernel's initial flags before entering high-half C code: keep
     * interrupts disabled until IDT/PIC setup completes and clear TF, DF,
     * AC, IOPL, and any other firmware-controlled status bits. */
    push 0x2
    popfq
    jmp rcx
