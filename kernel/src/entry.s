.global kmain
.type kmain, @function
.extern kmain_high
.extern __stack_top

/* The bootloader enters high-half kmain with its temporary handoff stack.
 * Switch immediately to the kernel image's high stack before C creates any
 * runtime state; RDI still carries the low physical BootInfo pointer. */
kmain:
    leaq __stack_top(%rip), %rsp
    andq $-16, %rsp
    subq $8, %rsp
    call kmain_high
1:
    cli
    hlt
    jmp 1b
