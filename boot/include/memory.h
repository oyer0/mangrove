#pragma once

#include <uefi.h>

typedef struct
{
    EFI_MEMORY_DESCRIPTOR *MemoryMap;

    usize MemoryMapSize;
    usize MapKey;
    usize DescriptorSize;

    u32 DescriptorVersion;
} MEMORY_MAP;

void memory_init(EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS memory_map_update(MEMORY_MAP *Map);

EFI_STATUS memory_map_init(MEMORY_MAP *Map);

EFI_STATUS memory_allocate(
    EFI_MEMORY_TYPE Type,
    usize Size,
    void **Buffer
);

EFI_STATUS memory_allocate_pages(
    EFI_ALLOCATE_TYPE AllocateType,
    EFI_MEMORY_TYPE MemoryType,
    usize Pages,
    EFI_PHYSICAL_ADDRESS *Adress
);

EFI_STATUS memory_free(
    void *Buffer
);

EFI_STATUS memory_exit_boot_services(
    EFI_HANDLE ImageHandle,
    MEMORY_MAP *Map
);
