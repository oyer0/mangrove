.global ring3_enter
.type ring3_enter, @function

/* Enter a fixed Ring 3 test entry using the standard iretq frame. */
ring3_enter:
    cli
    mov $0x33, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* System V arguments: RDI = user RIP, RSI = user RSP, RDX = argc, RCX = argv */
    pushq $0x33
    pushq %rsi
    pushfq
    orq $0x200, (%rsp)
    pushq $0x3b
    pushq %rdi

    /* Forward argc and argv registers to userspace RDI and RSI */
    mov %rdx, %rdi
    mov %rcx, %rsi
    xor %rdx, %rdx
    xor %rcx, %rcx

    iretq
