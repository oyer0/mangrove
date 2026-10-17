/* SPDX-License-Identifier: GPL-3.0-or-later */
.intel_syntax noprefix

.global handoff

handoff:
    /* Enforce supervisor writes against read-only PTEs from the first kernel
     * instruction instead of inheriting firmware's CR0.WP setting. */
    mov r10, cr0
    or r10, (1 << 16)
    mov cr0, r10
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
