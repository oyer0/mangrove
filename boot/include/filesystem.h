#pragma once

#include <uefi.h>

EFI_STATUS filesystem_init(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
);

EFI_STATUS filesystem_open(
    CHAR16 *Path,
    EFI_FILE_PROTOCOL **File
);

EFI_STATUS filesystem_read(
    EFI_FILE_PROTOCOL *File,
    void *Buffer,
    usize *BufferSize
);

EFI_STATUS filesystem_seek(
    EFI_FILE_PROTOCOL *File,
    u64 Position
);
