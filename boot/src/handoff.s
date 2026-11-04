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
.if BOOT_HANDOFF_CHECKPOINT == 3
    /* BootInfo.FramebufferBase is at byte offset 48 in the shared ABI. */
    mov r10, qword ptr [rdx + 48]
    mov eax, dword ptr [rdx + 76]
    sub eax, 16
    imul eax, dword ptr [rdx + 80]
    mov ecx, dword ptr [rdx + 72]
    sub ecx, 16
    add eax, ecx
    lea r10, [r10 + rax*4]
    mov r11d, dword ptr [rdx + 80]
    mov r9d, 16
1:
    mov rdi, r10
    mov eax, 0x00ffff00
    mov ecx, 16
    rep stosd
    lea r10, [r10 + r11*4]
    dec r9d
    jnz 1b
2:
    cli
    hlt
    jmp 2b
.endif
    mov rsp, r8
    mov rdi, rdx
    /* UEFI does not provide a scheduler-safe RFLAGS contract.  Establish
     * the kernel's initial flags before entering high-half C code: keep
     * interrupts disabled until IDT/PIC setup completes and clear TF, DF,
     * AC, IOPL, and any other firmware-controlled status bits. */
    push 0x2
    popfq
.if BOOT_HANDOFF_CHECKPOINT == 4
    /* This marker is reachable only after CR3 and the handoff stack are set. */
    mov r10, qword ptr [rdi + 48]
    mov eax, dword ptr [rdi + 76]
    sub eax, 16
    imul eax, dword ptr [rdi + 80]
    mov ecx, dword ptr [rdi + 72]
    sub ecx, 16
    add eax, ecx
    lea r10, [r10 + rax*4]
    mov r11d, dword ptr [rdi + 80]
    mov r9d, 16
3:
    mov rdi, r10
    mov eax, 0x000000ff
    mov ecx, 16
    rep stosd
    lea r10, [r10 + r11*4]
    dec r9d
    jnz 3b
4:
    cli
    hlt
    jmp 4b
.endif
    jmp rcx
