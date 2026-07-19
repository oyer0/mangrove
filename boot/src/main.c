#include <console.h>
#include <memory.h>
#include <filesystem.h>
#include <elf.h>

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;

    console_init(SystemTable);
    console_clear();

    memory_init(SystemTable);

    console_set_attribute(CONSOLE_LIGHT_GREEN);
    console_write(L"Mangrove Boot\r\n");

    console_set_cursor(0, 2);
    console_set_attribute(CONSOLE_WHITE);

    EFI_STATUS Status = filesystem_init(
        ImageHandle,
        SystemTable
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Filesystem protocol failed!\r\n");

        for (;;)
        {
        }
    }

    console_write(L"Filesystem protocol acquired!\r\n");

    EFI_FILE_PROTOCOL *Kernel;

    Status = filesystem_open(
        L"\\Mangrove\\kernel.elf",
        &Kernel
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Kernel open failed!\r\n");

        for (;;)
        {
        }
    }

    console_write(L"Kernel opened!\r\n");

    ELF_HEADER Header;
    usize BufferSize = sizeof(Header);

    Status = filesystem_read(
        Kernel,
        &Header,
        &BufferSize
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Kernel header read failed!\r\n");

        for (;;)
        {
        }
    }

    console_write(L"Kernel header read!\r\n");

    Status = elf_validate(
        &Header,
        BufferSize
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Invalid ELF header!\r\n");

        for (;;)
        {
        }
    }

    console_write(L"ELF header validated!\r\n");

    ELF_PROGRAM_HEADER *ProgramHeaders;

    Status = elf_read_program_headers(
        Kernel,
        &Header,
        &ProgramHeaders
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Program headers read failed!\r\n");
    
        for (;;)
        {
        }
    }
    
    console_write(L"Program headers read!\r\n");

    MEMORY_MAP Map;

    Status = memory_map_get(&Map);

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Memory map retrieval failed!\r\n");

        for (;;)
        {
        }
    }

    console_write(L"Memory map retrieved!\r\n");

    for (;;)
    {
    }

    return EFI_SUCCESS;
}
