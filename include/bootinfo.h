#pragma once

#include "types.h"

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
