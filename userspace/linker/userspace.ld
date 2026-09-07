/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* readable, executable */
    rodata PT_LOAD FLAGS(4); /* readable */
    data PT_LOAD FLAGS(6);    /* readable, writable */
}

SECTIONS
{
    . = 0x400000;
    .text ALIGN(0x1000) : { *(.text*) } :text
    .rodata ALIGN(0x1000) : { *(.rodata*) } :rodata
    /* Keep the explicit writable PT_LOAD valid for programs with no mutable
     * objects; the ELF loader intentionally rejects zero-sized load segments. */
    .data ALIGN(0x1000) : { *(.data*) BYTE(0) } :data
    .bss ALIGN(0x1000) : { *(.bss*) *(COMMON) } :data
}
