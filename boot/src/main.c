#include <console.h>
#include <memory.h>
#include <filesystem.h>
#include <kloader.h>
#include <bootinfo.h>
#include <handoff.h>

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

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
        for (;;) {}
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
        for (;;) {}
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
        for (;;) {}
    }

    console_write(L"Kernel header read!\r\n");

    Status = elf_validate(
        &Header,
        BufferSize
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Invalid ELF header!\r\n");
        for (;;) {}
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
        for (;;) {}
    }
    
    console_write(L"Program headers read!\r\n");

    void *KernelEntry;

    Status = elf_load_segments(
            Kernel,
            &Header,
            ProgramHeaders,
            &KernelEntry
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Kernel loading failed!\r\n");
        console_write_hex(Status);
        console_write(L"\r\n");
        for (;;) {}
    }

    console_write(L"Kernel loaded!\r\n");
    console_write(L"Entry: ");
    console_write_hex((u64)KernelEntry);
    console_write(L"\r\n");

    console_write(L"Kernel bytes: ");

    u8 *p = (u8 *)KernelEntry;

    for (usize i = 0; i < 16; i++)
    {
        console_write_hex(p[i]);
        console_write(L" ");
    }
    console_write(L"\r\n");

    // =========================================================================
    // 1. LOCATE GOP FIRST WHILE BOOT SERVICES ARE ALIVE
    // =========================================================================
    Status = SystemTable->BootServices->LocateProtocol(
        &EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
        NULL,
        (void **)&Gop
    );
    
    if (Status != EFI_SUCCESS)
    {
        console_write(L"Failed to locate GOP!\r\n");
        for (;;) {}
    }

    // =========================================================================
    // 2. FETCH MEMORY MAP AT THE LAST SECOND TO ENSURE FRESH MAPKEY
    // =========================================================================
    MEMORY_MAP Map;
    Status = memory_map_init(&Map);

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Memory map retrieval failed!\r\n");
        for (;;) {}
    }

    // =========================================================================
    // 3. PACK BOOTINFO STRUCT (Now safe because Gop is valid)
    // =========================================================================
    BOOT_INFO BootInfo;

    BootInfo.Size = sizeof(BOOT_INFO);
    BootInfo.MemoryMap = (u8 *)Map.MemoryMap;
    BootInfo.MemoryMapSize = Map.MemoryMapSize;
    BootInfo.MapKey = Map.MapKey;
    BootInfo.DescriptorSize = Map.DescriptorSize;
    BootInfo.DescriptorVersion = Map.DescriptorVersion;

    BootInfo.FramebufferBase = (void *)Gop->Mode->FrameBufferBase;
    BootInfo.FramebufferSize = (usize)Gop->Mode->FrameBufferSize;
    BootInfo.FramebufferWidth = (u32)Gop->Mode->Info->HorizontalResolution;
    BootInfo.FramebufferHeight = (u32)Gop->Mode->Info->VerticalResolution;
    BootInfo.PixelsPerScanLine = (u32)Gop->Mode->Info->PixelsPerScanLine;

    // Do NOT place any console prints here. It will corrupt the MapKey.

    // =========================================================================
    // 4. EXIT BOOT SERVICES SAFELY
    // =========================================================================
    Status = memory_exit_boot_services(
        ImageHandle,
        &Map
    );

    if (Status != EFI_SUCCESS)
    {
        // If exit fails, flash a red color to top-left pixel manually since console is dead
        u32 *fail_fb = (u32 *)Gop->Mode->FrameBufferBase;
        fail_fb[0] = 0x00FF0000; 
        for (;;) {}
    }

    // =========================================================================
    // 5. JUMP TO KERNEL
    // =========================================================================
    handoff(
        KernelEntry,
        &BootInfo
    );

    for (;;) {}
    return EFI_SUCCESS;
}
