ENTRY(kmain)

KERNEL_PHYS_BASE = 0x00200000;
KERNEL_VIRT_BASE = 0xffffffff80000000;

SECTIONS
{
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_BASE;
    __kernel_virt_start = .;

    .text : AT(KERNEL_PHYS_BASE)
    {
        __kernel_text_virt_start = .;
        *(.text*)
        __kernel_text_virt_end = .;
    }
    __kernel_text_phys_start = LOADADDR(.text);
    __kernel_text_phys_end = LOADADDR(.text) + SIZEOF(.text);

    . = ALIGN(4096);
    .rodata : AT(KERNEL_PHYS_BASE + (ADDR(.rodata) - ADDR(.text)))
    {
        __kernel_rodata_virt_start = .;
        *(.rodata*)
        __kernel_rodata_virt_end = .;
    }
    __kernel_rodata_phys_start = LOADADDR(.rodata);
    __kernel_rodata_phys_end = LOADADDR(.rodata) + SIZEOF(.rodata);

    . = ALIGN(4096);
    .data : AT(KERNEL_PHYS_BASE + (ADDR(.data) - ADDR(.text)))
    {
        __kernel_data_virt_start = .;
        *(.data*)
        __kernel_data_virt_end = .;
    }
    __kernel_data_phys_start = LOADADDR(.data);
    __kernel_data_phys_end = LOADADDR(.data) + SIZEOF(.data);

    . = ALIGN(4096);

    .bss : AT(KERNEL_PHYS_BASE + (ADDR(.bss) - ADDR(.text)))
    {
        __kernel_bss_virt_start = .;
        *(.bss*)
        *(COMMON)

        . = ALIGN(16);
        __stack_bottom = .;
        . += 64K;
        __stack_top = .;
        __kernel_bss_virt_end = .;
    }
    __kernel_bss_phys_start = LOADADDR(.bss);
    __kernel_bss_phys_end = LOADADDR(.bss) + SIZEOF(.bss);

    __kernel_virt_end = .;
    __kernel_phys_start = LOADADDR(.text);
    __kernel_phys_end = LOADADDR(.bss) + SIZEOF(.bss);

    /* Compatibility names remain virtual symbols; physical consumers use
     * the explicit *_phys bounds above. */
    __kernel_start = __kernel_virt_start;
    __kernel_end = __kernel_virt_end;
}
