#pragma once

#include <types.h>

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
    /* Physical framebuffer BAR supplied by GOP.  FramebufferBase is rebound
     * to the permanent high ioremap alias once the kernel CR3 is active. */
    u64 FramebufferPhysicalBase;
    usize FramebufferSize;

    u32 FramebufferWidth;
    u32 FramebufferHeight;
    u32 PixelsPerScanLine;

    void *Rsdp;

    /* Physical range of the stack installed by the bootloader immediately
     * before the kernel handoff.  Kept in the handoff record so diagnostic
     * builds can verify that PMM never allocates the live bootstrap stack. */
    void *HandoffStackBase;
    void *HandoffStackEnd;

    /* Physical CR3 prepared by the UEFI loader for the high-half handoff. */
    void *BootstrapPml4;

} BOOT_INFO;

typedef void (*KERNEL_ENTRY)(BOOT_INFO *BootInfo);
