/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * The SIPI vector supplies the page base in CS.  The trampoline therefore
 * derives its physical base from CS and uses only offsets within this page
 * until paging is enabled.  The BSP installs a temporary identity mapping
 * for the page in the shared kernel CR3 and removes it after AP handoff.
 */
.section .smp_trampoline,"ax"
.code16
.global __smp_trampoline_start
.global __smp_trampoline_end

.equ SMP_MAILBOX_OFFSET, 0x800
.equ MAILBOX_CR3,        8
.equ MAILBOX_STACK_TOP,  16
.equ MAILBOX_ENTRY,      24
.equ MAILBOX_CPU_INDEX,  0

__smp_trampoline_start:
    cli
    xorw %ax, %ax
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x0ff0, %sp

    /* EBX remains the physical trampoline base through mode changes. */
    xorl %ebx, %ebx
    movw %cs, %bx
    shll $4, %ebx

    movw $(trampoline_gdt_descriptor - __smp_trampoline_start), %si
    leal (trampoline_gdt - __smp_trampoline_start)(%ebx), %eax
    movl %eax, 2(%si)
    lgdt (%si)

    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    leal (protected_entry - __smp_trampoline_start)(%ebx), %eax
    pushl $0x08
    pushl %eax
    /* Protected mode is not active in the current CS yet, so the operand-size
     * override selects the 32-bit far return used for the new code segment. */
    .byte 0x66, 0xcb

.code32
protected_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* The real-mode stack used the trampoline page as its segment base.  Once
     * SS is flat, move the protected-mode transition stack back into that
     * identity-mapped page before paging is enabled. */
    leal 0x0ff0(%ebx), %esp

    leal SMP_MAILBOX_OFFSET(%ebx), %esi
    movl MAILBOX_CR3(%esi), %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl $0xC0000080, %ecx
    rdmsr
    /* Match the BSP's page-table execution policy: kernel data and BSS use
     * NX mappings, so long mode must enable EFER.NXE before paging. */
    orl $((1 << 11) | (1 << 8)), %eax
    wrmsr

    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0

    leal (long_mode_entry - __smp_trampoline_start)(%ebx), %eax
    movl %eax, (long_mode_far_pointer - __smp_trampoline_start)(%ebx)
    movw $0x18, (long_mode_far_pointer - __smp_trampoline_start + 4)(%ebx)
    ljmpl *(long_mode_far_pointer - __smp_trampoline_start)(%ebx)

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* The temporary identity mapping is still active for this final read. */
    movl %ebx, %esi
    addl $SMP_MAILBOX_OFFSET, %esi
    movq MAILBOX_STACK_TOP(%rsi), %rsp
    andq $-16, %rsp
    subq $8, %rsp
    movl MAILBOX_CPU_INDEX(%rsi), %edi
    movq MAILBOX_ENTRY(%rsi), %rax
    jmp *%rax

.align 8
trampoline_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
    .quad 0x00AF9A000000FFFF

trampoline_gdt_descriptor:
    .word (trampoline_gdt_descriptor - trampoline_gdt - 1)
    .long 0

long_mode_far_pointer:
    .long 0
    .word 0

__smp_trampoline_end:
