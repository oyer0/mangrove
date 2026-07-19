#include <memory.h>

#define MEMORY_MAP_SLACK 2

static EFI_BOOT_SERVICES *BootServices;

void memory_init(EFI_SYSTEM_TABLE *SystemTable)
{
    BootServices = SystemTable->BootServices;
}

EFI_STATUS memory_allocate(
    EFI_MEMORY_TYPE Type,
    usize Size,
    void **Buffer)
{
    return BootServices->AllocatePool(
        Type,
        Size,
        Buffer
    );
}

EFI_STATUS memory_allocate_pages(
    EFI_ALLOCATE_TYPE AllocateType,
    EFI_MEMORY_TYPE MemoryType,
    usize Pages,
    EFI_PHYSICAL_ADDRESS *Address)
{
    return BootServices->AllocatePages(
        AllocateType,
        MemoryType,
        Pages,
        Address
    );
}

EFI_STATUS memory_free(
    void *Buffer)
{
    return BootServices->FreePool(
        Buffer
    );
}

EFI_STATUS memory_map_update(MEMORY_MAP *Map)
{
    EFI_STATUS Status;

    for (;;)
    {
        Status = BootServices->GetMemoryMap(
            &Map->MemoryMapSize,
            Map->MemoryMap,
            &Map->MapKey,
            &Map->DescriptorSize,
            &Map->DescriptorVersion
        );

        if (Status == EFI_SUCCESS)
        {
            return EFI_SUCCESS;
        }

        if (Status != EFI_BUFFER_TOO_SMALL)
        {
            return Status;
        }

        if (Map->MemoryMap != NULL)
        {
            BootServices->FreePool(Map->MemoryMap);
            Map->MemoryMap = NULL;
        }

        Map->MemoryMapSize += Map->DescriptorSize * MEMORY_MAP_SLACK;

        Status = BootServices->AllocatePool(
            EFI_LOADER_DATA,
            Map->MemoryMapSize,
            (void **)&Map->MemoryMap
        );

        if (Status != EFI_SUCCESS)
        {
            return Status;
        }
    }
}

EFI_STATUS memory_map_init(MEMORY_MAP *Map)
{
    Map->MemoryMap = NULL;
    Map->MemoryMapSize = 0;
    Map->MapKey = 0;
    Map->DescriptorSize = 0;
    Map->DescriptorVersion = 0;

    return memory_map_update(Map); 
}

EFI_STATUS memory_exit_boot_services(
    EFI_HANDLE ImageHandle,
    MEMORY_MAP *Map
)
{
    EFI_STATUS Status;

    Status = BootServices->ExitBootServices(
        ImageHandle,
        Map->MapKey
    );

    if (Status == EFI_INVALID_PARAMETER)
    {
        Status = memory_map_update(Map);

        if (EFI_ERROR(Status))
        {
            return Status;
        }

        Status = BootServices->ExitBootServices(
            ImageHandle,
            Map->MapKey
        );
    }

    return Status;
}
