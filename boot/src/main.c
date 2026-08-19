#include <console.h>
#include <memory.h>
#include <filesystem.h>
#include <kloader.h>
#include <bootinfo.h>
#include <handoff.h>
#include <bootstrap.h>

static const EFI_GUID ACPI20_TABLE_GUID =
{
    0x8868E871,
    0xE4F1,
    0x11D3,
    {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}
};

static const EFI_GUID ACPI10_TABLE_GUID =
{
    0xEB9D2D30,
    0x2D88,
    0x11D3,
    {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}
};

static bool guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    if (a->Data1 != b->Data1)
        return false;

    if (a->Data2 != b->Data2)
        return false;

    if (a->Data3 != b->Data3)
        return false;

    for (u32 i = 0; i < 8; i++)
    {
        if (a->Data4[i] != b->Data4[i])
            return false;
    }

    return true;
}

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

    void *Rsdp = NULL;
    
    for (usize i = 0; i < SystemTable->NumberOfTableEntries; i++)
    {
        EFI_CONFIGURATION_TABLE *table =
            &SystemTable->ConfigurationTable[i];
    
        if (guid_equal(&table->VendorGuid, &ACPI20_TABLE_GUID))
        {
            Rsdp = table->VendorTable;
            break;
        }
    
        if (guid_equal(&table->VendorGuid, &ACPI10_TABLE_GUID))
        {
            Rsdp = table->VendorTable;
        }
    }

    BOOT_INFO BootInfo;

    BootInfo.Size = sizeof(BOOT_INFO);

    BootInfo.FramebufferBase = (void *)Gop->Mode->FrameBufferBase;
    BootInfo.FramebufferPhysicalBase = (u64)Gop->Mode->FrameBufferBase;
    BootInfo.FramebufferSize = (usize)Gop->Mode->FrameBufferSize;
    BootInfo.FramebufferWidth = (u32)Gop->Mode->Info->HorizontalResolution;
    BootInfo.FramebufferHeight = (u32)Gop->Mode->Info->VerticalResolution;
    BootInfo.PixelsPerScanLine = (u32)Gop->Mode->Info->PixelsPerScanLine;
    
    BootInfo.Rsdp = Rsdp;

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

    BootInfo.HandoffStackBase = (void *)(uintptr_t)StackBase;
    BootInfo.HandoffStackEnd = StackTop;

    /* All persistent loader allocations, including the handoff stack, must
     * be complete before taking the final map snapshot.  BootInfo receives
     * the finalized map below, after any ExitBootServices retry has updated
     * Map. */
    MEMORY_MAP Map;
    Status = memory_map_init(&Map);

    if (Status != EFI_SUCCESS)
    {
        console_write(L"Memory map retrieval failed!\r\n");
        for (;;) {}
    }

    EFI_PHYSICAL_ADDRESS BootstrapPml4 = 0;
    Status = bootstrap_build_page_tables(
        &Map, &Header, ProgramHeaders,
        (u64)StackBase, 16 * EFI_PAGE_SIZE,
        (u64)Gop->Mode->FrameBufferBase, (u64)Gop->Mode->FrameBufferSize,
        (u64)(uintptr_t)Rsdp,
        (u64)(uintptr_t)&BootInfo, sizeof(BootInfo),
        (u64)(uintptr_t)Map.MemoryMap, Map.MemoryMapSize,
        (u64)(uintptr_t)&handoff, &BootstrapPml4);
    if (Status != EFI_SUCCESS) {
        console_write(L"Bootstrap page-table construction failed!\r\n");
        console_write_hex(Status);
        console_write(L"\r\n");
        for (;;) {}
    }

    /* Bootstrap table allocations changed the descriptor key.  Refresh the
     * final map after those persistent allocations and before ExitBootServices. */
    Status = memory_map_update(&Map);
    if (Status != EFI_SUCCESS) {
        console_write(L"Post-bootstrap memory map failed!\r\n");
        for (;;) {}
    }

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

    /* memory_exit_boot_services() may have replaced the map buffer while
     * recovering from a stale map key.  Publish the map that actually
     * accompanied the successful ExitBootServices() call, not the earlier
     * snapshot.  The buffer is EFI_LOADER_DATA and remains owned by the
     * loader/kernel after boot services have ended. */
    BootInfo.MemoryMap = (u8 *)Map.MemoryMap;
    BootInfo.MemoryMapSize = Map.MemoryMapSize;
    BootInfo.MapKey = Map.MapKey;
    BootInfo.DescriptorSize = Map.DescriptorSize;
    BootInfo.DescriptorVersion = Map.DescriptorVersion;
    BootInfo.BootstrapPml4 = (void *)(uintptr_t)BootstrapPml4;

    handoff(KernelEntry, &BootInfo, StackTop,
            (void *)(uintptr_t)BootstrapPml4);

    for (;;) {}
    return EFI_SUCCESS;
}
