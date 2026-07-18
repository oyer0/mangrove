#include <memory.h>

#define MEMORY_MAP_SLACK 2

static EFI_BOOT_SERVICES *BootServices;

void memory_init(EFI_SYSTEM_TABLE *SystemTable)
{
    BootServices = SystemTable->BootServices;
}

EFI_STATUS memory_map_get(void)
{
    usize MemoryMapSize = 0;
    usize MapKey;
    usize DescriptorSize;
    u32 DescriptorVersion;

    void *MemoryMap = NULL;

    EFI_STATUS Status;

    for (;;) 
    {
        Status = BootServices->GetMemoryMap(
            &MemoryMapSize,
            MemoryMap,
            &MapKey,
            &DescriptorSize,
            &DescriptorVersion
        );

        if (Status == EFI_SUCCESS)
        {
            return EFI_SUCCESS;
        }

        if (Status != EFI_BUFFER_TOO_SMALL)
        {
            return Status;
        }

        if (MemoryMap != NULL)
        {
            BootServices->FreePool(MemoryMap);
            MemoryMap = NULL;
        }

        MemoryMapSize += DescriptorSize * MEMORY_MAP_SLACK;

        Status = BootServices->AllocatePool(
            EFI_LOADER_DATA,
            MemoryMapSize,
            &MemoryMap
        );

        if (Status != EFI_SUCCESS)
        {
            return Status;
        }

        continue;

    }
}
