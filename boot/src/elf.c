#include <elf.h>

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

    return EFI_SUCCESS;
}
