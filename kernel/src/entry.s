/* SPDX-License-Identifier: GPL-3.0-or-later */
.intel_syntax noprefix
.global kmain
.type kmain, @function
.extern kmain_high
.extern __stack_top
.if X250_BOOT_TRACE
.extern x250_boot_trace_event
.endif

/* The bootloader enters high-half kmain with its temporary handoff stack.
 * Switch immediately to the kernel image's high stack before C creates any
 * runtime state; RDI still carries the low physical BootInfo pointer. */
kmain:
.if BOOT_HANDOFF_CHECKPOINT == 5
    /* RDI still contains the identity-mapped physical BOOT_INFO pointer. */
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
5:
    mov rdi, r10
    mov eax, 0x0000ffff
    mov ecx, 16
    rep stosd
    lea r10, [r10 + r11*4]
    dec r9d
    jnz 5b
6:
    cli
    hlt
    jmp 6b
.endif
.if X250_BOOT_TRACE
    mov rbx, rdi
    /* Entry itself proves the loader's final indirect jump succeeded. */
    mov rdi, rbx
    mov esi, 5
    mov edx, 1
    lea rcx, [rip + .Ltrace_entry]
    call x250_boot_trace_event
    mov rdi, rbx
    mov esi, 6
    xor edx, edx
    lea rcx, [rip + .Ltrace_stack_enter]
    call x250_boot_trace_event
.endif
    lea rsp, [rip + __stack_top]
    and rsp, -16
.if X250_BOOT_TRACE
    mov rdi, rbx
    mov esi, 6
    mov edx, 1
    lea rcx, [rip + .Ltrace_stack_pass]
    call x250_boot_trace_event
    mov rdi, rbx
    mov esi, 7
    xor edx, edx
    lea rcx, [rip + .Ltrace_c_enter]
    call x250_boot_trace_event
.endif
    sub rsp, 8
.if BOOT_HANDOFF_CHECKPOINT == 6
    /* The kernel entry page ran and the relocated high-half stack is set. */
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
7:
    mov rdi, r10
    mov eax, 0x00ffff00
    mov ecx, 16
    rep stosd
    lea r10, [r10 + r11*4]
    dec r9d
    jnz 7b
8:
    cli
    hlt
    jmp 8b
.endif
.if X250_BOOT_TRACE
    mov rdi, rbx
.endif
    call kmain_high
1:
    cli
    hlt
    jmp 1b

.if X250_BOOT_TRACE
.section .rodata
.Ltrace_entry:
    .asciz "Pith entry reached"
.Ltrace_stack_enter:
    .asciz "install high-half stack"
.Ltrace_stack_pass:
    .asciz "high-half stack installed"
.Ltrace_c_enter:
    .asciz "call early Pith C bootstrap"
.text
.endif
