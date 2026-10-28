/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* readable, executable */
    rodata PT_LOAD FLAGS(4); /* readable */
    data PT_LOAD FLAGS(6);    /* readable, writable */
    tls PT_TLS FLAGS(4);      /* static executable TLS template */
}

SECTIONS
{
    . = 0x400000;
    .text ALIGN(0x1000) : {
        /* The internal worker-return target has a stable address within the
         * native image so the kernel can seed a worker's return slot without
         * parsing or trusting user-controlled symbol tables. */
        KEEP(*(.mangrove_thread_return))
        *(.text*)
    } :text
    .rodata ALIGN(0x1000) : { *(.rodata*) } :rodata
    /* Keep the explicit writable PT_LOAD valid for programs with no mutable
     * objects; the ELF loader intentionally rejects zero-sized load segments. */
    .data ALIGN(0x1000) : { *(.data*) BYTE(0) } :data
    .tdata ALIGN(8) : { *(.tdata*) } :data :tls
    .tbss ALIGN(8) (NOLOAD) : { *(.tbss*) } :data :tls
    .bss ALIGN(0x1000) : { *(.bss*) *(COMMON) } :data
}
