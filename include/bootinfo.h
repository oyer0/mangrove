/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#ifndef X250_BOOT_TRACE
#define X250_BOOT_TRACE 0
#endif

#if X250_BOOT_TRACE
#define BOOT_GOP_MODE_LIMIT 32U

typedef struct {
    u32 Number;
    u32 HorizontalResolution;
    u32 VerticalResolution;
    u32 PixelFormat;
    u32 PixelsPerScanLine;
    u32 Valid;
    u64 QueryStatus;
    u64 ValidationStatus;
    u64 FreeStatus;
} BOOT_GOP_MODE_INFO;
#endif

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

    /* Physical address corresponding to KERNEL_PHYS_BASE in the linked ELF. */
    u64 KernelPhysicalBase;

#if X250_BOOT_TRACE
    /* GOP selection details are bounded and included only in trace builds. */
    u32 GopInitialMode;
    u32 GopCurrentMode;
    u32 GopRequestedMode;
    u32 GopSetModeAttempted;
    u32 GopFallbackUsed;
    u32 GopMaxMode;
    u32 GopModeCount;
    u32 GopModeInventoryComplete;
    u32 GopActivePixelFormat;
    u64 GopSetModeStatus;
    u64 GopRestoreModeStatus;
    u64 GopSelectionStatus;
    BOOT_GOP_MODE_INFO GopModes[BOOT_GOP_MODE_LIMIT];
#endif

} BOOT_INFO;

_Static_assert(__builtin_offsetof(BOOT_INFO, FramebufferBase) == 48U &&
               __builtin_offsetof(BOOT_INFO, FramebufferWidth) == 72U &&
               __builtin_offsetof(BOOT_INFO, FramebufferHeight) == 76U &&
               __builtin_offsetof(BOOT_INFO, PixelsPerScanLine) == 80U,
               "handoff checkpoint assembly depends on BOOT_INFO offsets");

typedef void (*KERNEL_ENTRY)(BOOT_INFO *BootInfo);
