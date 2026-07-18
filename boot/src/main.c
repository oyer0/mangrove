#include <console.h>

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;

    console_init(SystemTable);

    console_write(L"Mangrove Boot\r\n");

    for (;;)
    {
    }

    return EFI_SUCCESS;
}
