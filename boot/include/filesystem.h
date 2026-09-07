/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <uefi.h>
#include <mgfs.h>

typedef MGFS_BOOT_FILE BOOT_FILE;

EFI_STATUS filesystem_init(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
);

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
