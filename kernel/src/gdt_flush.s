.global gdt_flush
.global tss_load
.type gdt_flush, @function
.type tss_load, @function

gdt_flush:
    lgdt (%rdi)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    pushq $0x08
    leaq .reload_cs(%rip), %rax
    pushq %rax
    lretq

.reload_cs:
    ret

tss_load:
    mov %di, %ax
    ltr %ax
    ret
