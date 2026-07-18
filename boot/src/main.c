#include <console.h>

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;

    console_init(SystemTable);
    
    console_clear();

    console_set_attribute(CONSOLE_LIGHT_GREEN);

    console_write(L"Mangrove Boot\r\n");

    console_set_cursor(0, 3);

    console_set_attribute(CONSOLE_WHITE);
    console_write(L"Cursor moved!");

    for (;;)
    {
    }

    return EFI_SUCCESS;
}
