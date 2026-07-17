#include <uefi.h>

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;
    (void)SystemTable;

    return EFI_SUCCESS;
}
