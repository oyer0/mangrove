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

    Status = elf_validate(
        &Header,
        BufferSize
    );

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Invalid ELF header!\r\n");
        for (;;) {}
    }

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

    MEMORY_MAP Map;
    Status = memory_map_init(&Map);

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Memory map retrieval failed!\r\n");
        for (;;) {}
    }

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


    EFI_PHYSICAL_ADDRESS StackBase = 0;

    Status = memory_allocate_pages(
        AllocateAnyPages,
        EFI_LOADER_DATA,
        16,
        &StackBase
    );

    if (EFI_ERROR(Status))
    {
        return Status;
    }

    void *StackTop = (void *)(StackBase + (16 * EFI_PAGE_SIZE));

    Status = memory_exit_boot_services(
        ImageHandle,
        &Map
    );

    if (Status != EFI_SUCCESS)
    {
        u32 *fail_fb = (u32 *)Gop->Mode->FrameBufferBase;
        fail_fb[0] = 0x00FF0000; 
        for (;;) {}
    }

    handoff(KernelEntry, &BootInfo, StackTop);

    for (;;) {}
    return EFI_SUCCESS;
}
