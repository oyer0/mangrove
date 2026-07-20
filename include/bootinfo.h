#pragma once

#include "types.h"

// Renamed to avoid name collisions with the UEFI environment headers entirely
typedef struct
{
    u32 Type;
    u32 Pad;
    u64 PhysicalStart;
    u64 VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
} MANGROVE_MEMORY_DESCRIPTOR;

typedef struct
{
    usize Size;

    u8   *MemoryMap;
    usize MemoryMapSize;
    usize MapKey;
    usize DescriptorSize;
    u32   DescriptorVersion;

    void *FramebufferBase;
    usize FramebufferSize;

    u32 FramebufferWidth;
    u32 FramebufferHeight;
    u32 PixelsPerScanLine;

} BOOT_INFO;

typedef void (*KERNEL_ENTRY)(BOOT_INFO *BootInfo);
