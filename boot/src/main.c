#include <console.h>
#include <memory.h>

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;

    console_init(SystemTable);
    memory_init(SystemTable);
    
    console_clear();

    console_set_attribute(CONSOLE_LIGHT_GREEN);

    console_write(L"Mangrove Boot\r\n");

    console_set_cursor(0, 3);

    console_set_attribute(CONSOLE_WHITE);
    console_write(L"Cursor moved!\r\n");

    EFI_STATUS Status = memory_map_get();



    if (Status == EFI_SUCCESS)
    {
        console_write(L"Memory map retrieved!\r\n");
    }
    else
    {
        console_write(L"Memory map retrieval failed!\r\n");
    }

    for (;;)
    {
    }

    return EFI_SUCCESS;
}
