#pragma once

#include <uefi.h>

typedef struct
{
    u8 Magic[4];
    u8 Class;
    u8 Data;
    u8 Version;
    u8 OSABI;
    u8 ABIVersion;
    u8 Padding[7];
} ELF_IDENT;

typedef struct
{
    ELF_IDENT Ident;

    u16 Type;
    u16 Machine;
    u32 Version;

    u64 Entry;

    u64 ProgramHeaderOffset;
    u64 SectionHeaderOffset;

    u32 Flags;

    u16 HeaderSize;
    u16 ProgramHeaderEntrySize;
    u16 ProgramHeaderCount;
    u16 SectionHeaderEntrySize;
    u16 SectionHeaderCount;
    u16 SectionHeaderStringIndex;
} ELF_HEADER;

EFI_STATUS elf_validate(
    ELF_HEADER *Header,
    usize HeaderSize
);
