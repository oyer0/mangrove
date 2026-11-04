/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <uefi.h>
#include <mgfs.h>

#ifndef X250_BOOT_TRACE
#define X250_BOOT_TRACE 0
#endif

typedef MGFS_BOOT_FILE BOOT_FILE;

#if X250_BOOT_TRACE
typedef enum
{
    FILESYSTEM_FAILURE_NONE,
    FILESYSTEM_FAILURE_DEVICE_DISCOVERY,
    FILESYSTEM_FAILURE_ROOT_NOT_FOUND,
    FILESYSTEM_FAILURE_AMBIGUOUS_ROOT,
    FILESYSTEM_FAILURE_MGFS
} FILESYSTEM_FAILURE;
#endif

#if X250_BOOT_TRACE
EFI_STATUS filesystem_init(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable,
    FILESYSTEM_FAILURE *Failure
);

void filesystem_report_diagnostics(void);
#else
EFI_STATUS filesystem_init(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
);
#endif

EFI_STATUS filesystem_open(
    const char *Path,
    BOOT_FILE **File
);

EFI_STATUS filesystem_read(
    BOOT_FILE *File,
    void *Buffer,
    usize *BufferSize
);

EFI_STATUS filesystem_seek(
    BOOT_FILE *File,
    u64 Position
);
