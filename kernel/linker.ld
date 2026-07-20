ENTRY(kmain)

SECTIONS
{
    . = 2M;

    .text : ALIGN(4096)
    {
        *(.text*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata*)
    }

    .data : ALIGN(4096)
    {
        *(.data*)
    }

   . = ALIGN(4096);

   .bss : ALIGN(4096)
    {
        *(.bss*)
        *(COMMON)
    
        . = ALIGN(16);
        __stack_bottom = .;
        . += 64K;
        __stack_top = .;
    } 
}
