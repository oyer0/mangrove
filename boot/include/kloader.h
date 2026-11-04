/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <uefi.h>
#include <bootinfo.h>
#include <filesystem.h>

#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define EV_CURRENT   1
#define ET_EXEC      2
#define EM_X86_64    62
#define PT_LOAD      1
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

typedef struct
{
    u32 Type;
    u32 Flags;

    u64 Offset;

    u64 VirtualAddress;
    u64 PhysicalAddress;

    u64 FileSize;
    u64 MemorySize;

    u64 Alignment;
} ELF_PROGRAM_HEADER;

typedef enum
{
    ELF_LOAD_FAILURE_NONE,
    ELF_LOAD_FAILURE_ALIGNMENT,
    ELF_LOAD_FAILURE_SEGMENT,
    ELF_LOAD_FAILURE_ALLOCATION,
    ELF_LOAD_FAILURE_SEEK,
    ELF_LOAD_FAILURE_READ
} ELF_LOAD_FAILURE_STAGE;

typedef struct
{
    ELF_LOAD_FAILURE_STAGE Stage;
    usize ProgramHeaderIndex;
    u64 PhysicalAddress;
    usize Pages;
    u64 FileOffset;
} ELF_LOAD_FAILURE;

EFI_STATUS elf_validate(
    ELF_HEADER *Header,
    usize HeaderSize
);

EFI_STATUS elf_read_program_headers(
    BOOT_FILE *Kernel,
    ELF_HEADER *Header,
    ELF_PROGRAM_HEADER **ProgramHeaders
);

EFI_STATUS elf_load_segments(
    BOOT_FILE *Kernel,
    ELF_HEADER *Header,
    ELF_PROGRAM_HEADER *ProgramHeaders,
    void **KernelEntry,
    u64 *ImagePhysicalBase,
    ELF_LOAD_FAILURE *Failure
);
