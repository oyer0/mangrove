#pragma once

#include <uefi.h>

EFI_STATUS filesystem_init(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
);
