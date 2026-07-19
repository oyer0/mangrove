ENTRY(kmain)

SECTIONS
{
    . = 1M;

    .text :
    {
        *(.text*)
    }

    .rodata :
    {
        *(.rodata*)
    }

    .data :
    {
        *(.data*)
    }

   . = ALIGN(4096);

   .bss :
    {
        *(.bss*)
        *(COMMON)
    
        . = ALIGN(16);
        __stack_bottom = .;
        . += 64K;
        __stack_top = .;
    } 
}
