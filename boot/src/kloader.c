/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <kloader.h>
#include <filesystem.h>
#include <memory.h>
#include <address_layout.h>

EFI_STATUS elf_validate(
    ELF_HEADER *Header,
    usize HeaderSize)
{
    if (HeaderSize < sizeof(ELF_HEADER))
    {
        return EFI_INVALID_PARAMETER;
    }

    if (Header->Ident.Magic[0] != 0x7F ||
        Header->Ident.Magic[1] != 'E'  ||
        Header->Ident.Magic[2] != 'L'  ||
        Header->Ident.Magic[3] != 'F')
    {
        return EFI_INVALID_PARAMETER;
    }

    if (Header->Ident.Class != ELFCLASS64)
    {
        return EFI_INVALID_PARAMETER;
    }
    
    if (Header->Ident.Data != ELFDATA2LSB)
    {
        return EFI_INVALID_PARAMETER;
    }
    
    if (Header->Ident.Version != EV_CURRENT)
    {
        return EFI_INVALID_PARAMETER;
    }

    if (Header->Machine!= EM_X86_64)
    {
        return EFI_INVALID_PARAMETER;
    }
    
    if (Header->Type!= ET_EXEC)
    {
        return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

EFI_STATUS elf_read_program_headers(
    BOOT_FILE *Kernel,
    ELF_HEADER *Header,
    ELF_PROGRAM_HEADER **ProgramHeaders)
{
    if (!Kernel || !Header || !ProgramHeaders ||
        Header->ProgramHeaderEntrySize != sizeof(ELF_PROGRAM_HEADER) ||
        Header->ProgramHeaderCount == 0U)
    {
        return EFI_COMPROMISED_DATA;
    }

    usize Size = Header->ProgramHeaderCount * sizeof(ELF_PROGRAM_HEADER);
    if (Header->ProgramHeaderOffset > Kernel->size ||
        Size > Kernel->size - Header->ProgramHeaderOffset)
    {
        return EFI_COMPROMISED_DATA;
    }

    EFI_STATUS Status = filesystem_seek(
        Kernel,
        Header->ProgramHeaderOffset
    );

    if (Status != EFI_SUCCESS)
    {
        return Status;
    }

    ELF_PROGRAM_HEADER *Buffer;

    Status = memory_allocate(
        EFI_LOADER_DATA,
        Size,
        (void **)&Buffer
    );

    if (Status != EFI_SUCCESS)
    {
        return Status;
    }

    Status = filesystem_read(
        Kernel,
        Buffer,
        &Size
    );

    if (Status != EFI_SUCCESS)
    {
        return Status;
    }

    *ProgramHeaders = Buffer;

    return EFI_SUCCESS;
}

EFI_STATUS elf_load_segments(
    BOOT_FILE *Kernel,
    ELF_HEADER *Header,
    ELF_PROGRAM_HEADER *ProgramHeaders,
    void **EntryPoint,
    u64 *ImagePhysicalBase,
    ELF_LOAD_FAILURE *Failure)
{
    EFI_STATUS Status;
    u64 image_end = KERNEL_PHYS_BASE;
    usize first_load_index = (usize)-1;
    usize image_pages;
    EFI_PHYSICAL_ADDRESS image_base = 0;
    bool entry_is_executable = false;

    if (!Kernel || !Header || !ProgramHeaders || !EntryPoint ||
        !ImagePhysicalBase)
    {
        return EFI_INVALID_PARAMETER;
    }
    if (Failure) *Failure = (ELF_LOAD_FAILURE){0};
    *ImagePhysicalBase = 0;

    /* Validate the complete preferred physical span before reserving one
     * relocatable contiguous allocation for the linked image. */
    for (usize i = 0; i < Header->ProgramHeaderCount; i++)
    {
        ELF_PROGRAM_HEADER *ProgramHeader = &ProgramHeaders[i];
        u64 segment_end;

        if (ProgramHeader->Type != PT_LOAD || ProgramHeader->MemorySize == 0U)
        {
            if (ProgramHeader->Type == PT_LOAD &&
                ProgramHeader->FileSize != 0U)
            {
                if (Failure)
                {
                    Failure->Stage = ELF_LOAD_FAILURE_SEGMENT;
                    Failure->ProgramHeaderIndex = i;
                }
                return EFI_COMPROMISED_DATA;
            }
            continue;
        }

        if (first_load_index == (usize)-1)
        {
            first_load_index = i;
        }
        if (ProgramHeader->Alignment != EFI_PAGE_SIZE)
        {
            if (Failure)
            {
                Failure->Stage = ELF_LOAD_FAILURE_ALIGNMENT;
                Failure->ProgramHeaderIndex = i;
            }
            return EFI_UNSUPPORTED;
        }
        if (ProgramHeader->PhysicalAddress < KERNEL_PHYS_BASE ||
            ProgramHeader->VirtualAddress < KERNEL_IMAGE_VIRT_BASE ||
            ProgramHeader->PhysicalAddress - KERNEL_PHYS_BASE !=
                ProgramHeader->VirtualAddress - KERNEL_IMAGE_VIRT_BASE ||
            (ProgramHeader->PhysicalAddress & (EFI_PAGE_SIZE - 1U)) != 0U ||
            ProgramHeader->FileSize > ProgramHeader->MemorySize ||
            ProgramHeader->PhysicalAddress >
                (u64)-1 - ProgramHeader->MemorySize ||
            ProgramHeader->Offset > Kernel->size ||
            ProgramHeader->FileSize > Kernel->size - ProgramHeader->Offset)
        {
            if (Failure)
            {
                Failure->Stage = ELF_LOAD_FAILURE_SEGMENT;
                Failure->ProgramHeaderIndex = i;
                Failure->PhysicalAddress = ProgramHeader->PhysicalAddress;
                Failure->FileOffset = ProgramHeader->Offset;
            }
            return EFI_COMPROMISED_DATA;
        }
        segment_end = ProgramHeader->PhysicalAddress +
                      ProgramHeader->MemorySize;
        if (segment_end > (u64)-1 - (EFI_PAGE_SIZE - 1U))
        {
            if (Failure)
            {
                Failure->Stage = ELF_LOAD_FAILURE_SEGMENT;
                Failure->ProgramHeaderIndex = i;
                Failure->PhysicalAddress = ProgramHeader->PhysicalAddress;
            }
            return EFI_COMPROMISED_DATA;
        }
        segment_end = (segment_end + EFI_PAGE_SIZE - 1U) &
                      ~((u64)EFI_PAGE_SIZE - 1U);
        if (segment_end > image_end) image_end = segment_end;
        if ((ProgramHeader->Flags & 1U) != 0U &&
            Header->Entry >= ProgramHeader->VirtualAddress &&
            Header->Entry - ProgramHeader->VirtualAddress <
                ProgramHeader->MemorySize)
            entry_is_executable = true;
    }
    if (first_load_index == (usize)-1 || !entry_is_executable ||
        image_end <= KERNEL_PHYS_BASE)
    {
        if (Failure)
        {
            Failure->Stage = ELF_LOAD_FAILURE_SEGMENT;
            Failure->ProgramHeaderIndex = first_load_index == (usize)-1
                ? 0U : first_load_index;
        }
        return EFI_COMPROMISED_DATA;
    }

    image_pages = (usize)((image_end - KERNEL_PHYS_BASE) / EFI_PAGE_SIZE);
    Status = memory_allocate_pages(AllocateAnyPages, EFI_LOADER_DATA,
                                   image_pages, &image_base);
    if (Status != EFI_SUCCESS)
    {
        if (Failure)
        {
            Failure->Stage = ELF_LOAD_FAILURE_ALLOCATION;
            Failure->ProgramHeaderIndex = first_load_index;
            Failure->PhysicalAddress = KERNEL_PHYS_BASE;
            Failure->Pages = image_pages;
        }
        return Status;
    }

    for (usize i = 0; i < Header->ProgramHeaderCount; i++)
    {
        ELF_PROGRAM_HEADER *ProgramHeader = &ProgramHeaders[i];
        u8 *Segment;
        usize FileSize;

        if (ProgramHeader->Type != PT_LOAD || ProgramHeader->MemorySize == 0U)
            continue;
        Segment = (u8 *)(uintptr_t)kernel_image_relocate_phys(
            ProgramHeader->PhysicalAddress, image_base);
        Status = filesystem_seek(Kernel, ProgramHeader->Offset);
        if (Status != EFI_SUCCESS)
        {
            if (Failure)
            {
                Failure->Stage = ELF_LOAD_FAILURE_SEEK;
                Failure->ProgramHeaderIndex = i;
                Failure->PhysicalAddress = ProgramHeader->PhysicalAddress;
                Failure->Pages = image_pages;
                Failure->FileOffset = ProgramHeader->Offset;
            }
            return Status;
        }

        FileSize = (usize)ProgramHeader->FileSize;
        Status = filesystem_read(Kernel, Segment, &FileSize);
        if (Status != EFI_SUCCESS || FileSize != ProgramHeader->FileSize)
        {
            if (Failure)
            {
                Failure->Stage = ELF_LOAD_FAILURE_READ;
                Failure->ProgramHeaderIndex = i;
                Failure->PhysicalAddress = ProgramHeader->PhysicalAddress;
                Failure->Pages = image_pages;
                Failure->FileOffset = ProgramHeader->Offset;
            }
            return Status == EFI_SUCCESS ? EFI_COMPROMISED_DATA : Status;
        }

        for (u64 j = ProgramHeader->FileSize;
             j < ProgramHeader->MemorySize; j++)
            Segment[j] = 0;
    }

    *EntryPoint = (KERNEL_ENTRY)Header->Entry;
    *ImagePhysicalBase = image_base;

    return EFI_SUCCESS;
}
