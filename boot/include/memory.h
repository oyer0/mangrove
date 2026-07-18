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

EFI_STATUS memory_map_get(MEMORY_MAP *Map);
