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
