#include <uefi.h>

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;

    SystemTable->ConOut->OutputString(
        SystemTable->ConOut,
        L"Mangrove Boot\r\n"
    );

    for (;;)
    {
    }

    return EFI_SUCCESS;
}
