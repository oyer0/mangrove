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

EFI_STATUS memory_free(
    void *Buffer)
{
    return BootServices->FreePool(
        Buffer
    );
}

EFI_STATUS memory_map_get(MEMORY_MAP *Map)
{
    Map->MemoryMap = NULL;
    Map->MemoryMapSize = 0;
    Map->MapKey = 0;
    Map->DescriptorSize = 0;
    Map->DescriptorVersion = 0;

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
