#include <elf.h>
#include <filesystem.h>
#include <memory.h>

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
    EFI_FILE_PROTOCOL *Kernel,
    ELF_HEADER *Header,
    ELF_PROGRAM_HEADER **ProgramHeaders)
{
    (void)ProgramHeaders;

    EFI_STATUS Status = filesystem_seek(
        Kernel,
        Header->ProgramHeaderOffset
    );

    if (Status != EFI_SUCCESS)
    {
        return Status;
    }

    usize Size =
        Header->ProgramHeaderCount *
        Header->ProgramHeaderEntrySize;

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
    EFI_FILE_PROTOCOL *Kernel,
    ELF_HEADER *Header,
    ELF_PROGRAM_HEADER *ProgramHeaders)
{
    EFI_STATUS Status;

    for (usize i = 0; i < Header->ProgramHeaderCount; i++)
    {
        ELF_PROGRAM_HEADER *ProgramHeader = &ProgramHeaders[i];

        if (ProgramHeader->Type != PT_LOAD)
        {
            continue;
        }

        if (ProgramHeader->Alignment != EFI_PAGE_SIZE)
        {
            return EFI_UNSUPPORTED;
        }

        usize Pages =
            (ProgramHeader->MemorySize + EFI_PAGE_SIZE - 1) /
            EFI_PAGE_SIZE;

        EFI_PHYSICAL_ADDRESS Address =
            ProgramHeader->PhysicalAddress;

               Status = memory_allocate_pages(
            AllocateAddress,
            EFI_LOADER_DATA,
            Pages,
            &Address
        );

        if (Status != EFI_SUCCESS)
        {
            return Status;
        }

        void *Segment = (void *)Address;

        Status = filesystem_seek(
            Kernel,
            ProgramHeader->Offset
        );

        if (Status != EFI_SUCCESS)
        {
            return Status;
        }

        usize FileSize = ProgramHeader->FileSize;

        Status = filesystem_read(
            Kernel,
            Segment,
            &FileSize
        );

        if (Status != EFI_SUCCESS)
        {
            return Status;
        }

        if (ProgramHeader->MemorySize > ProgramHeader->FileSize)
        {
            u8 *Bytes = Segment;

            usize BssSize =
                ProgramHeader->MemorySize -
                ProgramHeader->FileSize;

            for (usize j = 0; j < BssSize; j++)
            {
                Bytes[ProgramHeader->FileSize + j] = 0;
            }
        }
    }

    return EFI_SUCCESS;
}
